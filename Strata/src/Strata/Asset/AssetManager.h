#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Asset/AssetResidency.h"
#include "Strata/Asset/AssetStreamingQueue.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <list>
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
		uint64_t Cancellations = 0;   // Loads abandoned before they finished (CancelLoad, or unloaded while loading)
		uint64_t StagingReleases = 0; // Times the idle upload command list was recreated to return its staging memory
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

	// Converts an asset's stored bytes (cooked or source, depending on the type) into an Asset object. Runs on
	// worker threads and must not touch engine state.
	using AssetLoadFunction = std::function<Ref<Asset>(const AssetMetadata& metadata, std::span<const uint8_t> data, std::string* outError)>;

	// Loaders for every asset type. The built-in types are registered automatically; registering a type again replaces
	// its loader (at startup only, before any asset loads).
	class AssetLoaderRegistry
	{
	public:
		static void Register(AssetType type, AssetLoadFunction loader);
		static const AssetLoadFunction* Find(AssetType type);
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
		// budgets, runs a scheduled trim, evicts assets of the pools over budget and returns idle staging memory.
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
		// frames, whatever the budgets. Returns how many assets were evicted.
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
			// Position in m_RecencyOrder, which holds the evictable resident assets (loaded, not memory assets)
			std::list<AssetHandle>::iterator RecencyPosition;
			bool InRecencyOrder = false;
		};

		struct Completion
		{
			AssetHandle Handle = UUID::Null();
			uint64_t Generation = 0;
			Ref<Asset> LoadedAsset;
			std::string Error;
			uint64_t InFlightBytes = 0; // Counted in flight by the streaming queue until this completion is handled
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
		// Replaces the loaded object (null drops it), keeping the pool totals and the recency order. Returns the previous
		// object, to be released after the lock (destroying assets may be slow).
		Ref<Asset> SetLoadedLocked(AssetEntry& entry, Ref<Asset> asset);
		// Whether the entry may be evicted, apart from when it was requested (see AssetResidencyBudgets).
		bool CanEvictLocked(const AssetEntry& entry) const;
		// Whether the entry was requested in the last `frames` frames (or in the current one).
		bool WasRequestedWithinLocked(const AssetEntry& entry, uint32_t frames) const;
		void EvictLocked(AssetEntry& entry, std::vector<Ref<Asset>>& released);
		// Removes the queued load of an entry that stops loading (unload, cancellation, reload); a running one is discarded
		// by the caller's generation change. Returns whether the entry was loading.
		bool AbandonLoadLocked(AssetEntry& entry);
		uint32_t EvictUnusedLocked(uint32_t unusedFrames, std::vector<Ref<Asset>>& released);
		void EvictToBudgets();
		void Unpin(AssetHandle handle, uint64_t registration);
		// Records the frame's finalization statistics and recreates an idle upload command list (main thread).
		void EndFrameUploads();
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
		uint64_t m_FrameIndex = 0;
		AssetMemoryUsage m_Resident;
		std::list<AssetHandle> m_RecencyOrder; // Evictable resident assets, least recently requested first
		std::optional<ScheduledTrim> m_ScheduledTrim;
		uint64_t m_Evictions = 0;
		uint64_t m_Cancellations = 0;
		uint64_t m_StagingReleases = 0;
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
