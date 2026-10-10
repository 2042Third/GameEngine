#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Asset/AssetResidency.h"
#include "Strata/Asset/AssetStreamingQueue.h"
#include "Strata/Math/AABB.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strata
{

	struct AssetManagerStats
	{
		uint32_t RegisteredAssets = 0;
		uint32_t LoadedAssets = 0;
		uint32_t LoadingAssets = 0;    // Queued or in flight
		uint32_t FailedAssets = 0;
		uint64_t LoadedMemory = 0;     // Resident bytes of every pool (Resident.GetTotal())
		uint64_t TotalLoadsCompleted = 0;

		uint64_t Frame = 0;            // The manager's frame counter (advanced by Update)
		AssetMemoryUsage Resident;     // Per pool: memory of the asset objects the manager holds (memory assets included)
		AssetResidencyBudgets Budgets;
		uint32_t PinnedAssets = 0;     // Assets with at least one pin

		// The streaming queue (see AssetStreamingQueue).
		std::array<uint32_t, 3> QueuedLoads = {}; // Waiting to be dispatched, per priority (High, Normal, Low)
		uint32_t InFlightLoads = 0;
		uint32_t OutstandingReads = 0;
		uint64_t InFlightBytes = 0;
		uint64_t InFlightBytesHighWater = 0; // Since the manager was created

		// Finalization: GPU bytes uploaded and main-thread time, in the last frame (since the previous Update) and at most
		// in one frame of the last c_StatsWindowFrames frames.
		uint64_t UploadedBytesLastFrame = 0;
		uint64_t UploadedBytesWindowMax = 0;
		float FinalizeMsLastFrame = 0.0f;
		float FinalizeMsWindowMax = 0.0f;

		uint64_t Evictions = 0;       // Assets evicted for budgets or trims since the manager was created
		// Resident assets that eviction (for budgets or trims) looked at since the manager was created: the cost of keeping
		// the budgets. Eviction for a pool looks only at assets holding memory of that pool.
		uint64_t EvictionChecks = 0;
		uint64_t Cancellations = 0;   // Loads abandoned before they finished (CancelLoad, or unloaded while loading)
		uint64_t StagingReleases = 0; // Times the idle upload command list was recreated to return its staging memory

		// Loads of dependencies started when the read of an asset that depends on them completed (see GetDependencies), and
		// those dropped on arrival because what they hold no longer fitted in the budgets.
		uint64_t DependencyPrefetches = 0;
		uint64_t DependencyPrefetchesDropped = 0;
	};

	// The residency of one asset (AssetManagerBase::GetResidencyInfo).
	struct AssetResidencyInfo
	{
		AssetHandle Handle = UUID::Null();
		AssetType Type = AssetType::None;
		std::string Path;
		std::string Name;
		AssetState State = AssetState::Unloaded;
		AssetMemoryUsage Usage;          // Of the object the manager holds
		uint64_t LastRequestedFrame = 0; // The manager's frame of the latest request (GetAsset, RequestLoad, Pin)
		uint32_t PinCount = 0;
		bool IsMemoryAsset = false;
	};

	// The stored bytes a loader decodes. Loaders read them as a span (it converts to one, so loaders may take a span);
	// a loader whose asset keeps the bytes, or most of them, takes them over with TakeBytes instead of copying them,
	// which spares a second copy of large assets while they load.
	class AssetLoadData
	{
	public:
		// Bytes the loader may take over (TakeBytes moves them out of `bytes`).
		explicit AssetLoadData(std::vector<uint8_t>& bytes)
			: m_Bytes(bytes), m_Owner(&bytes)
		{
		}

		// Bytes the loader may only read (TakeBytes copies them), e.g. to validate a document.
		explicit AssetLoadData(std::span<const uint8_t> bytes)
			: m_Bytes(bytes)
		{
		}

		std::span<const uint8_t> GetBytes() const { return m_Bytes; }
		operator std::span<const uint8_t>() const { return m_Bytes; }

		// The bytes as a vector of the caller's own: moved out when they were handed over (GetBytes is empty afterwards),
		// copied otherwise.
		std::vector<uint8_t> TakeBytes()
		{
			if (!m_Owner)
				return std::vector<uint8_t>(m_Bytes.begin(), m_Bytes.end());
			std::vector<uint8_t> bytes = std::move(*m_Owner);
			m_Owner->clear();
			m_Owner = nullptr;
			m_Bytes = {};
			return bytes;
		}
	private:
		std::span<const uint8_t> m_Bytes;
		std::vector<uint8_t>* m_Owner = nullptr;
	};

	// Converts an asset's stored bytes (cooked or source, depending on the type) into an Asset object. Runs on
	// worker threads and must not touch engine state.
	using AssetLoadFunction = std::function<Ref<Asset>(const AssetMetadata& metadata, AssetLoadData& data, std::string* outError)>;

	// Loaders for every asset type, registered by the module that owns the type (Engine::RegisterBuiltinModules opens the
	// registry and registers the engine's modules). Registering a type again replaces its loader (at startup only, before
	// any asset loads). Using the registry before BeginRegistration fails ST_CORE_VERIFY.
	class AssetLoaderRegistry
	{
	public:
		static void BeginRegistration();
		static void Register(AssetType type, AssetLoadFunction loader);
		static const AssetLoadFunction* Find(AssetType type);
	};

	// The load of an asset together with everything it depends on (AssetManagerBase::RequestLoadWithDependencies): a
	// loading screen's or a prefetch's handle on a group of loads. It pins every member while it lives, so nothing of the
	// group is evicted, until it is destroyed or cancelled. Movable, not copyable; a request that outlives its manager
	// reports what it saw last. Thread-safe like the manager (one request is used by one thread at a time).
	class AssetGroupRequest
	{
	public:
		AssetGroupRequest() = default;
		AssetGroupRequest(AssetGroupRequest&&) noexcept = default;
		AssetGroupRequest& operator=(AssetGroupRequest&&) noexcept = default;
		AssetGroupRequest(const AssetGroupRequest&) = delete;
		AssetGroupRequest& operator=(const AssetGroupRequest&) = delete;

		// Whether the request names any asset (false for unknown assets and default-constructed requests).
		bool IsValid() const { return !m_Assets.empty(); }
		AssetHandle GetRoot() const { return m_Assets.empty() ? UUID::Null() : m_Assets.front(); }
		// The members: the root, then its dependencies breadth first.
		const std::vector<AssetHandle>& GetAssets() const { return m_Assets; }

		// The share of the members' stored bytes that finished loading (Ready or Failed), in [0, 1]. Never decreases: a
		// member that finished stays counted (also while a hot reload replaces it). 1 for an invalid request.
		float Progress() const;
		// Every member finished loading (Ready or Failed).
		bool IsComplete() const;
		// A member failed to load.
		bool HasFailed() const;
		// Releases the pins and abandons the loads of the members that have not finished (AssetManagerBase::CancelLoad):
		// queued reads never start. Members someone else pins keep loading, and so do members requested again later.
		void Cancel();
	private:
		friend class AssetManagerBase;

		// Records the members that finished since the last call.
		void Refresh() const;
	private:
		std::weak_ptr<AssetManagerBase> m_Manager;
		std::vector<AssetHandle> m_Assets;
		std::vector<uint64_t> m_Weights; // Per member: its stored size (at least 1)
		std::vector<AssetPin> m_Pins;
		uint64_t m_TotalWeight = 0;
		mutable std::vector<bool> m_Finished;
		mutable uint64_t m_FinishedWeight = 0;
		mutable size_t m_FinishedCount = 0;
		mutable bool m_Failed = false;
	};

	// Asset registry plus asynchronous loading and residency. Subclasses provide where the bytes come from (the editor
	// reads and imports project files, the runtime reads asset packs).
	//
	// Loading pipeline: RequestLoad queues the load in the streaming queue (AssetStreamingQueue), which dispatches reads
	// to the I/O pool in priority order while the bytes in flight and the outstanding reads stay within limits; decoding
	// runs on the worker pool, and Update() (main thread, once per frame) finalizes loaded assets - creating GPU resources
	// within a per-frame upload and time budget - and publishes them. GetAsset never blocks: it returns null until the
	// asset is Ready and requests the load.
	//
	// Residency: every resident asset is accounted per memory pool (Asset::GetMemoryUsage). After finalizing, Update
	// evicts the least recently requested assets of the pools over budget (see AssetResidencyBudgets); an evicted asset is
	// Unloaded again and loads on its next request. Requests (GetAsset, RequestLoad, Pin) mark an asset as used in the
	// current frame, so code that draws or uses assets every frame keeps them. Hold handles across frames, not Refs: an
	// object referenced outside the manager is never evicted (dropping it would free nothing), so held Refs defeat the
	// budgets. Pin what gameplay must keep (AssetPin).
	//
	// Managers must be owned by a Ref (pins refer to them weakly). All functions are thread-safe unless noted;
	// finalization only happens on the main thread.
	class AssetManagerBase : public std::enable_shared_from_this<AssetManagerBase>
	{
	public:
		// Registers the built-in assets (see BuiltinAssets) and takes the default budgets (GetDefaultResidencyBudgets).
		// Must be constructed on the main thread.
		AssetManagerBase();
		virtual ~AssetManagerBase();

		AssetManagerBase(const AssetManagerBase&) = delete;
		AssetManagerBase& operator=(const AssetManagerBase&) = delete;

		bool IsHandleValid(AssetHandle handle) const;
		std::optional<AssetMetadata> GetMetadata(AssetHandle handle) const;
		AssetType GetAssetType(AssetHandle handle) const;
		std::vector<AssetMetadata> GetAllMetadata(AssetType filter = AssetType::None) const;
		AssetHandle FindAssetByPath(std::string_view path) const; // Relative path; null when not found

		// Returns the loaded asset, or null while it has never finished loading (requesting the load if needed, or raising
		// the queued request to this priority). During a hot reload the previous version is returned until the new one is
		// ready.
		Ref<Asset> GetAsset(AssetHandle handle, AssetPriority priority = AssetPriority::Normal);
		// Loads synchronously. Main thread only (finalization runs here); meant for tools, tests and loading screens,
		// not for per-frame game code. Returns null if the asset fails to load. Must not be called from
		// Asset::FinalizeOnMainThread.
		Ref<Asset> LoadAssetSync(AssetHandle handle);
		AssetState GetAssetState(AssetHandle handle) const;
		std::string GetAssetError(AssetHandle handle) const;
		// Starts loading an unloaded asset, or raises the queued load of a loading one (a higher priority, or the same
		// priority with a higher score: within a priority, higher scores load first). Never lowers a request.
		void RequestLoad(AssetHandle handle, AssetPriority priority = AssetPriority::Normal, float score = 0.0f);
		// Abandons a load that has not finished: a queued request is removed, a running one is discarded before it is
		// decoded (or before its result is published). The asset is Unloaded again; an asset being reloaded keeps serving
		// its previous version (Ready). Returns false when the asset was not loading, and for pinned assets.
		bool CancelLoad(AssetHandle handle);
		// Drops the loaded object (and a queued or running load); it is loaded again on the next request. Memory assets
		// cannot be unloaded. Unloads pinned assets too: pins only protect from eviction.
		void UnloadAsset(AssetHandle handle);
		// Re-reads the asset if it was loaded (hot reload). In-flight loads of the old version are discarded.
		void ReloadAsset(AssetHandle handle);

		// Registers an asset that exists only in memory (built-in or created at runtime). Returns its handle.
		AssetHandle AddMemoryAsset(const Ref<Asset>& asset, AssetMetadata metadata);

		// Main thread, once per frame: advances the frame counter, finalizes completed loads within the upload and time
		// budgets, runs a scheduled trim, evicts assets of the pools over budget and returns idle staging memory. An arrival
		// in use first makes room for itself in the pools it needs (see GetEvictionGraceFrames); with a renderer, one that
		// allocates GPU memory then waits until the GPU memory evicted for it is released (the frames in flight plus one).
		// Arrivals are finalized in order: those behind a waiting one wait for it, making room at once (so their waits
		// overlap) for up to UploadBytesPerFrame of GPU memory in all.
		virtual void Update();
		// Main thread: finalizes loads until nothing is pending or the timeout expires. Returns true if idle.
		bool WaitForPendingLoads(std::chrono::milliseconds timeout = std::chrono::milliseconds(30000));
		// Loads are queued, in flight or waiting to be finalized.
		bool HasPendingLoads() const;

		//////////////////////////////////////////////////////////////////////////
		// Residency
		//////////////////////////////////////////////////////////////////////////

		// Keeps the asset resident while the pin lives, and requests its load if it is not loaded (or raises it). Returns an
		// empty pin for unknown assets.
		AssetPin Pin(AssetHandle handle, AssetPriority priority = AssetPriority::High);
		uint32_t GetPinCount(AssetHandle handle) const;

		// Budgets take effect at the next Update (pool budgets) or dispatch (in-flight bytes).
		void SetResidencyBudgets(const AssetResidencyBudgets& budgets);
		AssetResidencyBudgets GetResidencyBudgets() const;
		// The budgets new managers start with: derived from the graphics device's memory budget while the renderer runs
		// (AssetResidencyBudgets::FromDeviceBudget), else without pool limits.
		static AssetResidencyBudgets GetDefaultResidencyBudgets();

		// Evicts every evictable asset (see AssetResidencyBudgets) that was not requested in the last `unusedFrames`
		// frames, whatever the budgets; assets that hold no memory stay (evicting them would free nothing). Returns how
		// many assets were evicted.
		uint32_t TrimUnused(uint32_t unusedFrames);
		// Runs TrimUnused(unusedFrames) in the Update `delayFrames` updates from now: after a scene switch, so that the new
		// scene has requested what it uses first (AssetResidency::c_SceneSwitchTrimFrames). Replaces an earlier schedule.
		void ScheduleTrim(uint32_t delayFrames, uint32_t unusedFrames);

		// Assets requested this recently are never evicted for budgets: the frames the GPU may still be working on plus two.
		uint32_t GetEvictionGraceFrames() const { return m_EvictionGraceFrames; }
		uint64_t GetFrameIndex() const;

		AssetManagerStats GetStats() const;
		// The assets that are resident, loading, failed or pinned (unloaded, unpinned ones are left out), in no order.
		std::vector<AssetResidencyInfo> GetResidencyInfo() const;

		// Changes whenever an asset object is published, replaced or dropped (a load or reload finished, a memory asset was
		// added, an asset was unloaded, evicted or unregistered), so that caches of data derived from loaded assets can
		// check cheaply whether to revalidate.
		uint64_t GetContentVersion() const { return m_ContentVersion.load(std::memory_order_acquire); }
		// Appends the handles of the assets whose objects changed after content version `sinceVersion` (a handle may appear
		// more than once), so that such caches revalidate only what changed. Returns false, appending nothing, if the
		// manager no longer remembers that far back (it keeps the latest c_MaxContentChanges changes): then any asset may
		// have changed.
		bool GetContentChanges(uint64_t sinceVersion, std::vector<AssetHandle>& outHandles) const;
		static constexpr size_t c_MaxContentChanges = 4096;

		// Frames without uploads after which the upload command list is recreated (see Update).
		static constexpr uint32_t c_StagingReleaseFrames = 120;
		// Frames the window maximums of AssetManagerStats cover.
		static constexpr uint32_t c_StatsWindowFrames = 120;

		//////////////////////////////////////////////////////////////////////////
		// Dependencies, bounds and group loads
		//////////////////////////////////////////////////////////////////////////

		// The assets an asset references directly, as the asset pipeline recorded them (AssetDependencies): a material's
		// textures; the meshes, materials, textures, prefabs, models, audio clips and fonts the entities of a scene, prefab or
		// model use (their components' asset properties and script fields); a mesh's default materials. The editor answers from
		// its import records, games from their pack's dependency table. Empty for unknown assets and assets without any.
		//
		// When the read of a requested asset completes, the manager requests its direct dependencies at the same priority,
		// behind direct requests (c_DependencyPrefetchScore), so that e.g. a material's textures load alongside the material
		// instead of after it. Documents that place entities (scenes, prefabs, models) prefetch nothing: what their entities
		// use is requested by whatever draws or plays it once it is needed (group loads take a whole closure, see
		// RequestLoadWithDependencies). Prefetches stay within the budgets without evicting anything:
		// - a dependency is prefetched only while its stored size fits in every pool with a budget beside what is resident,
		//   reserved for arrivals, on its way (the stored bytes of every load queued or in flight, prefetches included, until
		//   it is finalized) and a window of InFlightBytes kept free for the requests to come (stored sizes estimate what an
		//   asset holds): tight budgets prefetch nothing;
		// - an arrival nobody requested meanwhile is dropped, unfinalized, when what it holds does not fit beside what is
		//   resident, reserved and still on its way (it would be uploaded only to be evicted); otherwise it is published
		//   without making room, and is the first to be evicted until something requests it (a prefetch marks nothing as
		//   used);
		// - prefetches do not chain (the dependencies of an asset that was only prefetched load once something requests it);
		// - the streaming queue bounds them like any load (in-flight bytes and reads).
		std::vector<AssetHandle> GetDependencies(AssetHandle handle) const;
		// The asset and everything it depends on, transitively: the asset, then breadth first, each once. Unknown handles are
		// left out (an empty list for an unknown asset).
		std::vector<AssetHandle> GetDependencyClosure(AssetHandle handle) const;
		// The local bounds of a mesh as recorded when it was imported, known before the mesh loads (nullopt for other assets).
		std::optional<AABB> GetBounds(AssetHandle handle) const;

		// Loads an asset with everything it depends on (GetDependencyClosure): pins every member at `priority`, which
		// requests the loads in breadth-first order. An invalid request for unknown assets.
		AssetGroupRequest RequestLoadWithDependencies(AssetHandle handle, AssetPriority priority = AssetPriority::Normal);
		// Whether the asset and everything it depends on are Ready.
		bool IsLoadedWithDependencies(AssetHandle handle) const;

		// Reads `size` bytes of an asset's stored form from `offset` without loading the asset (e.g. one level of a texture's
		// mip chain): packs decode only the blocks the range covers. Thread-safe; blocks the calling thread (use an I/O job).
		bool ReadStoredRange(AssetHandle handle, uint64_t offset, uint64_t size, std::vector<uint8_t>& outData, std::string* outError = nullptr);

		// The queue score of dependency prefetches: below the default score of direct requests of the same priority.
		static constexpr float c_DependencyPrefetchScore = -1.0f;
	protected:
		// Reads part of an asset's stored form; any thread. The default reads it all (ReadAssetData) and keeps the range.
		virtual bool ReadAssetRange(const AssetMetadata& metadata, uint64_t offset, uint64_t size, std::vector<uint8_t>& outData, std::string* outError);
		// Records what an asset depends on and its bounds (subclasses: from import records, from the pack's tables).
		// Duplicates, the null handle and the asset itself are left out. Does nothing for unknown assets.
		void SetAssetReferences(AssetHandle handle, std::vector<AssetHandle> dependencies, std::optional<AABB> bounds);
	private:
		// Requests the direct dependencies of an asset whose read completed (see GetDependencies). Any thread.
		void PrefetchDependencies(AssetHandle handle, AssetPriority priority);
		// The closure of GetDependencyClosure. Requires m_Mutex.
		std::vector<AssetHandle> GetDependencyClosureLocked(AssetHandle handle) const;
	protected:
		// Reads the stored bytes of an asset. Called on I/O threads; must be thread-safe.
		virtual bool ReadAssetData(const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string* outError) = 0;

		// Stops dispatching loads and blocks until no load job is running. Subclasses must call this first in their
		// destructor: in-flight jobs call ReadAssetData on the subclass.
		void WaitForInFlightLoads();

		// Registers an asset (or updates the metadata of a registered one, keeping its loaded state).
		void RegisterAsset(const AssetMetadata& metadata);
		void UnregisterAsset(AssetHandle handle);
		void UpdateAssetMetadata(const AssetMetadata& metadata);
		// Records how many bytes loading the asset reads (AssetMetadata::StoredSize).
		void SetStoredSize(AssetHandle handle, uint64_t storedSize);
	private:
		// An asset's place in the recency orders (m_RecencyOrders): the frame of its latest request, then the order of
		// requests within a frame. Ascending order is least recently requested first.
		struct RecencyKey
		{
			uint64_t Frame = 0;
			uint64_t Sequence = 0;

			auto operator<=>(const RecencyKey&) const = default;
		};

		struct AssetEntry
		{
			AssetMetadata Metadata;
			Ref<Asset> Loaded;
			AssetState State = AssetState::Unloaded;
			// Changes on (re)registration, reload and unload so stale completions are discarded. Generations come from
			// one manager-wide counter that never repeats, so even a re-registered entry never matches an old load.
			uint64_t Generation = 0;
			uint64_t Registration = 0; // The generation the entry was registered with; pins count for one registration
			bool IsMemoryAsset = false;
			std::string Error;

			// Residency
			AssetMemoryUsage Usage;          // Of Loaded, read when it was published
			uint64_t LastRequestedFrame = 0;
			uint32_t PinCount = 0;
			// While the entry is evictable and resident (loaded, not a memory asset), it is in the recency order of every pool
			// its Usage holds memory of, with this key (whose Frame is LastRequestedFrame).
			RecencyKey Recency;
			bool InRecencyOrders = false;

			// Recorded by the subclass (SetAssetReferences)
			std::vector<AssetHandle> Dependencies;
			std::optional<AABB> Bounds;
			// Loading because an asset that depends on it was read, and requested by nobody since (see GetDependencies).
			bool PrefetchOnly = false;
		};

		struct Completion
		{
			AssetHandle Handle = UUID::Null();
			uint64_t Generation = 0;
			Ref<Asset> LoadedAsset;
			std::string Error;
			uint64_t InFlightBytes = 0; // Counted in flight by the streaming queue until this completion is handled
			bool RoomMade = false;        // MakeRoomFor ran for it
			AssetMemoryUsage Reserved;    // Its memory, reserved in the budgets by MakeRoomFor until it is published
			uint64_t FinalizeFrame = 0;   // Not finalized (with a budget) before this frame: memory evicted for it is released then
			bool FinalizeStarted = false; // Its finalization is under way (it continues over several frames)
		};

		// What MakeRoomFor did for an arrival.
		struct RoomResult
		{
			uint64_t EvictedGpuBytes = 0; // GPU memory evicted for it, released once no frame in flight can use it
			AssetMemoryUsage Reserved;    // Its memory, now reserved (see m_Reserved)
		};

		struct LoadState;

		struct ContentChange
		{
			uint64_t Version = 0; // Content version the change produced
			AssetHandle Handle = UUID::Null();
		};

		struct ScheduledTrim
		{
			uint64_t Frame = 0;
			uint32_t UnusedFrames = 0;
		};

		friend class AssetPin;

		static void PushCompletion(const Ref<LoadState>& loadState, Completion completion) noexcept;
		// Advances the content version for a changed asset object and remembers the change. Requires m_Mutex.
		void PublishContentChange(AssetHandle handle);
		// Finalizes queued completions; returns the number processed. With the budget, stops once the frame's upload bytes
		// or finalization time are used up (after at least one completion).
		size_t ProcessCompletions(bool applyBudget);

		// Streaming (AssetManager.cpp)
		bool DispatchLoad(const AssetStreamingRequest& request);
		void RunLoad(const AssetMetadata& metadata, const AssetStreamingRequest& request);
		bool IsLoadCurrent(AssetHandle handle, uint64_t generation) const;
		void UpdateStreamingLimits();

		// Residency bookkeeping (AssetResidency.cpp); every function requires m_Mutex.
		void TouchLocked(AssetEntry& entry);
		// Replaces the loaded object (null drops it), keeping the pool totals and the recency orders. Returns the previous
		// object, to be released after the lock (destroying assets may be slow).
		Ref<Asset> SetLoadedLocked(AssetEntry& entry, Ref<Asset> asset);
		// Adds the entry to (removes it from) the recency orders of the pools its Usage holds memory of.
		void LinkRecencyLocked(AssetEntry& entry);
		void UnlinkRecencyLocked(AssetEntry& entry);
		// Whether the entry may be evicted, apart from when it was requested (see AssetResidencyBudgets).
		bool CanEvictLocked(const AssetEntry& entry) const;
		// Whether the entry was requested in the last `frames` frames (or in the current one).
		bool WasRequestedWithinLocked(const AssetEntry& entry, uint32_t frames) const;
		void EvictLocked(AssetEntry& entry, std::vector<Ref<Asset>>& released);
		// Removes the queued load of an entry that stops loading (unload, cancellation, reload); a running one is discarded
		// by the caller's generation change. Returns whether the entry was loading.
		bool AbandonLoadLocked(AssetEntry& entry);
		uint32_t EvictUnusedLocked(uint32_t unusedFrames, std::vector<Ref<Asset>>& released);
		// Evicts the least recently requested assets holding memory of the pool until `extraBytes` more fit in its budget
		// (or the rest is protected).
		void EvictToFitLocked(AssetMemoryPool pool, uint64_t extraBytes, std::vector<Ref<Asset>>& released);
		void EvictToBudgets();
		// Before an asset is finalized: when it is in use (requested within the grace window, so it will stay), evicts what
		// its memory, beside the arrivals reserved before it, would put over budget in the pools it needs, and reserves its
		// memory until it is published (see ProcessCompletions). Arrivals that need room then wait until the GPU memory
		// evicted for them is released, so that the device holds the budget at most, not the budget plus the arrivals.
		RoomResult MakeRoomFor(AssetHandle handle, const AssetMemoryUsage& incoming);
		void Unpin(AssetHandle handle, uint64_t registration);
		// Records the frame's finalization statistics and recreates an idle upload command list (main thread).
		void EndFrameUploads();

		// Dependency prefetch (see GetDependencies); every function requires m_Mutex.
		// Whether `bytes` more fit in every pool with a budget beside what is resident, reserved for arrivals, on its way
		// (`pendingBytes`: the stored bytes of the loads queued or in flight) and a window of InFlightBytes.
		bool FitsPrefetchLocked(uint64_t bytes, uint64_t pendingBytes) const;
		// Before an arrival is finalized: drops it when nobody requested it since it was prefetched and what it holds does not
		// fit in a pool's budget beside what is resident, reserved and still on its way (`pendingBytes`, the other loads queued
		// or in flight): it is Unloaded again, unfinalized. Returns whether it was dropped.
		bool DropUnfittingPrefetchLocked(AssetEntry& entry, const AssetMemoryUsage& incoming, uint64_t pendingBytes);
	private:
		mutable std::mutex m_Mutex;
		std::unordered_map<AssetHandle, AssetEntry> m_Entries;
		std::unordered_map<std::string, AssetHandle> m_PathIndex; // Path -> top-level asset handle
		uint64_t m_TotalLoadsCompleted = 0;
		uint64_t m_NextGeneration = 1; // Guarded by m_Mutex
		Ref<LoadState> m_LoadState;
		AssetStreamingQueue m_Queue;

		// Residency (guarded by m_Mutex)
		AssetResidencyBudgets m_Budgets;
		uint32_t m_EvictionGraceFrames = 4;
		// Frames until the GPU memory of a dropped asset is released: the frames in flight plus one (the bindless table keeps
		// textures that long).
		uint32_t m_GpuReleaseFrames = 3;
		uint64_t m_FrameIndex = 0;
		AssetMemoryUsage m_Resident;
		// Memory of arrivals in use that made room for themselves and are not published yet: room made for later arrivals
		// leaves room for these too.
		AssetMemoryUsage m_Reserved;
		// Per pool (indexed by AssetMemoryPool): the evictable resident assets holding memory of the pool, least recently
		// requested first, so eviction for a pool over budget looks only at assets that free its memory. Requests move an
		// asset to the end in constant time; an arrival requested long ago is placed in logarithmic time.
		std::array<std::map<RecencyKey, AssetHandle>, 3> m_RecencyOrders;
		uint64_t m_NextRecencySequence = 0;
		std::optional<ScheduledTrim> m_ScheduledTrim;
		uint64_t m_Evictions = 0;
		uint64_t m_EvictionChecks = 0;
		uint64_t m_Cancellations = 0;
		uint64_t m_StagingReleases = 0;
		uint64_t m_DependencyPrefetches = 0;
		uint64_t m_DependencyPrefetchesDropped = 0;
		uint64_t m_FrameUploadedBytes = 0; // Since the last Update
		float m_FrameFinalizeMs = 0.0f;
		std::array<uint64_t, c_StatsWindowFrames> m_UploadedBytesHistory = {};
		std::array<float, c_StatsWindowFrames> m_FinalizeMsHistory = {};
		size_t m_HistoryCursor = 0;
		uint64_t m_LastFrameUploadedBytes = 0;
		float m_LastFrameFinalizeMs = 0.0f;

		nvrhi::CommandListHandle m_UploadCommandList;
		uint32_t m_FramesWithoutUploads = 0; // Main thread only
		std::atomic<uint64_t> m_ContentVersion = 0; // See GetContentVersion; changed with m_Mutex held
		std::deque<ContentChange> m_ContentChanges; // The latest changes, oldest first (guarded by m_Mutex)
		bool m_ProcessingCompletions = false; // Main thread only
	};

	// Process-wide access to the active asset manager (set by the project).
	class AssetManager
	{
	public:
		static void SetActive(const Ref<AssetManagerBase>& manager);
		static const Ref<AssetManagerBase>& GetActive();
		static bool HasActive() { return GetActive() != nullptr; }

		template<typename T>
		static Ref<T> GetAsset(AssetHandle handle, AssetPriority priority = AssetPriority::Normal)
		{
			const Ref<AssetManagerBase>& manager = GetActive();
			if (!manager || !handle.IsValid())
				return nullptr;
			Ref<Asset> asset = manager->GetAsset(handle, priority);
			if (!asset || asset->GetType() != T::GetStaticType())
				return nullptr;
			return std::static_pointer_cast<T>(asset);
		}

		template<typename T>
		static Ref<T> LoadAssetSync(AssetHandle handle)
		{
			const Ref<AssetManagerBase>& manager = GetActive();
			if (!manager || !handle.IsValid())
				return nullptr;
			Ref<Asset> asset = manager->LoadAssetSync(handle);
			if (!asset || asset->GetType() != T::GetStaticType())
				return nullptr;
			return std::static_pointer_cast<T>(asset);
		}

		static AssetState GetAssetState(AssetHandle handle);
		static AssetType GetAssetType(AssetHandle handle);
		static bool IsHandleValid(AssetHandle handle);
	};

}
