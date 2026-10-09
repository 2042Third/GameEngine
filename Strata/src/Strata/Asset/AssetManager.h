#pragma once

#include "Strata/Asset/Asset.h"

#include <nvrhi/nvrhi.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
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
		uint32_t LoadingAssets = 0;
		uint32_t FailedAssets = 0;
		uint64_t LoadedMemory = 0;     // Sum of GetMemoryUsage() of loaded assets
		uint64_t TotalLoadsCompleted = 0;
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

	// Asset registry plus asynchronous loading. Subclasses provide where the bytes come from (the editor reads and
	// imports project files, the runtime reads asset packs).
	//
	// Loading pipeline: RequestLoad queues a read on the I/O pool, decoding runs on the worker pool, and Update()
	// (main thread, once per frame) finalizes loaded assets - creating GPU resources within a per-frame upload budget -
	// and publishes them. GetAsset never blocks: it returns null until the asset is Ready and requests the load.
	// All functions are thread-safe unless noted; finalization only happens on the main thread.
	class AssetManagerBase
	{
	public:
		// Registers the built-in assets (see BuiltinAssets). Must be constructed on the main thread.
		AssetManagerBase();
		virtual ~AssetManagerBase();

		AssetManagerBase(const AssetManagerBase&) = delete;
		AssetManagerBase& operator=(const AssetManagerBase&) = delete;

		bool IsHandleValid(AssetHandle handle) const;
		std::optional<AssetMetadata> GetMetadata(AssetHandle handle) const;
		AssetType GetAssetType(AssetHandle handle) const;
		std::vector<AssetMetadata> GetAllMetadata(AssetType filter = AssetType::None) const;
		AssetHandle FindAssetByPath(std::string_view path) const; // Relative path; null when not found

		// Returns the loaded asset, or null while it has never finished loading (requesting the load if needed).
		// During a hot reload the previous version is returned until the new one is ready.
		Ref<Asset> GetAsset(AssetHandle handle, AssetPriority priority = AssetPriority::Normal);
		// Loads synchronously. Main thread only (finalization runs here); meant for tools, tests and loading screens,
		// not for per-frame game code. Returns null if the asset fails to load. Must not be called from
		// Asset::FinalizeOnMainThread.
		Ref<Asset> LoadAssetSync(AssetHandle handle);
		AssetState GetAssetState(AssetHandle handle) const;
		std::string GetAssetError(AssetHandle handle) const;
		void RequestLoad(AssetHandle handle, AssetPriority priority = AssetPriority::Normal);
		// Drops the loaded object; it is loaded again on the next request. Memory assets cannot be unloaded.
		void UnloadAsset(AssetHandle handle);
		// Re-reads the asset if it was loaded (hot reload). In-flight loads of the old version are discarded.
		void ReloadAsset(AssetHandle handle);

		// Registers an asset that exists only in memory (built-in or created at runtime). Returns its handle.
		AssetHandle AddMemoryAsset(const Ref<Asset>& asset, AssetMetadata metadata);

		// Main thread, once per frame: finalizes completed loads within the GPU upload budget.
		virtual void Update();
		// Main thread: finalizes loads until nothing is pending or the timeout expires. Returns true if idle.
		bool WaitForPendingLoads(std::chrono::milliseconds timeout = std::chrono::milliseconds(30000));
		bool HasPendingLoads() const;

		void SetUploadBudget(uint64_t bytesPerFrame) { m_UploadBudget.store(bytesPerFrame); }
		AssetManagerStats GetStats() const;
		// Changes whenever an asset object is published, replaced or dropped (a load or reload finished, a memory asset was
		// added, an asset was unloaded or unregistered), so that caches of data derived from loaded assets can check
		// cheaply whether to revalidate.
		uint64_t GetContentVersion() const { return m_ContentVersion.load(std::memory_order_acquire); }
	protected:
		// Reads the stored bytes of an asset. Called on I/O threads; must be thread-safe.
		virtual bool ReadAssetData(const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string* outError) = 0;

		// Blocks until no load job is running. Subclasses must call this first in their destructor: in-flight jobs call
		// ReadAssetData on the subclass.
		void WaitForInFlightLoads();

		// Registers an asset (or updates the metadata of a registered one, keeping its loaded state).
		void RegisterAsset(const AssetMetadata& metadata);
		void UnregisterAsset(AssetHandle handle);
		void UpdateAssetMetadata(const AssetMetadata& metadata);
	private:
		struct AssetEntry
		{
			AssetMetadata Metadata;
			Ref<Asset> Loaded;
			AssetState State = AssetState::Unloaded;
			// Changes on (re)registration, reload and unload so stale completions are discarded. Generations come from
			// one manager-wide counter that never repeats, so even a re-registered entry never matches an old load.
			uint64_t Generation = 0;
			bool IsMemoryAsset = false;
			std::string Error;
		};

		struct Completion
		{
			AssetHandle Handle = UUID::Null();
			uint64_t Generation = 0;
			Ref<Asset> LoadedAsset;
			std::string Error;
		};

		struct LoadState;

		static void PushCompletion(const Ref<LoadState>& loadState, Completion completion) noexcept;
		// Finalizes queued completions; returns the number processed. With a budget, stops once it is exceeded.
		size_t ProcessCompletions(bool applyBudget);
	private:
		mutable std::mutex m_Mutex;
		std::unordered_map<AssetHandle, AssetEntry> m_Entries;
		std::unordered_map<std::string, AssetHandle> m_PathIndex; // Path -> top-level asset handle
		uint64_t m_TotalLoadsCompleted = 0;
		uint64_t m_NextGeneration = 1; // Guarded by m_Mutex
		Ref<LoadState> m_LoadState;

		nvrhi::CommandListHandle m_UploadCommandList;
		std::atomic<uint64_t> m_UploadBudget = 256ull * 1024 * 1024;
		std::atomic<uint64_t> m_ContentVersion = 0; // See GetContentVersion; changed with m_Mutex held
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
