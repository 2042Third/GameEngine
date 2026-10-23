#include "stpch.h"
#include "Strata/Asset/AssetManager.h"

#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/EngineServices.h"
#include "Strata/Core/JobSystem.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <unordered_set>
#include <utility>

namespace Strata
{

	const char* AssetStateToString(AssetState state)
	{
		switch (state)
		{
			case AssetState::Unloaded: return "Unloaded";
			case AssetState::Loading:  return "Loading";
			case AssetState::Ready:    return "Ready";
			case AssetState::Failed:   return "Failed";
		}
		return "Unknown";
	}

	////////////////////////////////////////////////////////////////////////////////
	// AssetLoaderRegistry
	////////////////////////////////////////////////////////////////////////////////

	namespace
	{

		struct LoaderStorage
		{
			std::mutex Mutex;
			std::atomic<bool> Open = false;
			std::unordered_map<AssetType, AssetLoadFunction> Loaders;
			std::unordered_map<AssetType, AssetPrefixFunction> Prefixes;
		};

		struct ExtensionStorage
		{
			std::mutex Mutex;
			std::atomic<bool> Open = false;
			std::vector<AssetManagerExtensionFactory> Factories;
		};

		ExtensionStorage& GetExtensionStorage()
		{
			static ExtensionStorage s_Storage;
			return s_Storage;
		}

		LoaderStorage& GetLoaderStorageUnchecked()
		{
			static LoaderStorage s_Storage;
			return s_Storage;
		}

		LoaderStorage& GetLoaderStorage()
		{
			LoaderStorage& storage = GetLoaderStorageUnchecked();
			ST_CORE_VERIFY(storage.Open.load(std::memory_order_acquire),
				"The asset loader registry is used before Engine::RegisterBuiltinModules() registered the engine's modules");
			return storage;
		}

		JobPriority ToJobPriority(AssetPriority priority)
		{
			switch (priority)
			{
				case AssetPriority::High:   return JobPriority::High;
				case AssetPriority::Normal: return JobPriority::Normal;
				case AssetPriority::Low:    return JobPriority::Low;
			}
			return JobPriority::Normal;
		}

		// The frames in flight of a graphics device created with the default specification.
		constexpr uint32_t c_DefaultFramesInFlight = 2;

		// Whether the read of an asset of this type prefetches what it references. Documents that place entities (scenes,
		// prefabs, models) do not: what their entities use is requested by whatever draws or plays it, once it is needed.
		// Prefetching a scene's meshes and materials (all of them, with the scene) raised the peak private memory of the stress
		// sweep by 60 to 100 MB.
		bool PrefetchesDependencies(AssetType type)
		{
			return type != AssetType::Scene && type != AssetType::Prefab && type != AssetType::Model;
		}

		// Source revisions (AssetSourceRevisions) come from one counter for the process, so that a cache which moves to
		// another manager never mistakes that manager's revision of an asset for the one its data came from.
		std::atomic<uint64_t> s_NextSourceRevision = 1;

		uint64_t NextSourceRevision()
		{
			return s_NextSourceRevision.fetch_add(1, std::memory_order_relaxed);
		}

	}

	void AssetLoaderRegistry::BeginRegistration()
	{
		GetLoaderStorageUnchecked().Open.store(true, std::memory_order_release);
	}

	void AssetLoaderRegistry::Register(AssetType type, AssetLoadFunction loader)
	{
		LoaderStorage& storage = GetLoaderStorage();
		std::scoped_lock<std::mutex> lock(storage.Mutex);
		storage.Loaders[type] = std::move(loader);
	}

	const AssetLoadFunction* AssetLoaderRegistry::Find(AssetType type)
	{
		LoaderStorage& storage = GetLoaderStorage();
		std::scoped_lock<std::mutex> lock(storage.Mutex);
		auto it = storage.Loaders.find(type);
		// Entries are never removed, so the pointer stays valid after the lock is released (std::unordered_map keeps
		// element addresses stable across rehashing). Replacing a loader while loads run is not supported.
		return it != storage.Loaders.end() ? &it->second : nullptr;
	}

	void AssetLoaderRegistry::RegisterPrefix(AssetType type, AssetPrefixFunction prefix)
	{
		LoaderStorage& storage = GetLoaderStorage();
		std::scoped_lock<std::mutex> lock(storage.Mutex);
		storage.Prefixes[type] = std::move(prefix);
	}

	const AssetPrefixFunction* AssetLoaderRegistry::FindPrefix(AssetType type)
	{
		LoaderStorage& storage = GetLoaderStorage();
		std::scoped_lock<std::mutex> lock(storage.Mutex);
		auto it = storage.Prefixes.find(type);
		// Stable like the loaders (never removed).
		return it != storage.Prefixes.end() ? &it->second : nullptr;
	}

	////////////////////////////////////////////////////////////////////////////////
	// AssetManagerExtensionRegistry
	////////////////////////////////////////////////////////////////////////////////

	void AssetManagerExtensionRegistry::BeginRegistration()
	{
		GetExtensionStorage().Open.store(true, std::memory_order_release);
	}

	void AssetManagerExtensionRegistry::Register(AssetManagerExtensionFactory factory)
	{
		ExtensionStorage& storage = GetExtensionStorage();
		ST_CORE_VERIFY(storage.Open.load(std::memory_order_acquire),
			"The asset manager extension registry is used before Engine::RegisterBuiltinModules() registered the engine's modules");
		std::scoped_lock<std::mutex> lock(storage.Mutex);
		storage.Factories.push_back(std::move(factory));
	}

	std::vector<Scope<AssetManagerExtension>> AssetManagerExtensionRegistry::CreateExtensions(AssetManagerBase& manager)
	{
		ExtensionStorage& storage = GetExtensionStorage();
		ST_CORE_VERIFY(storage.Open.load(std::memory_order_acquire),
			"An asset manager is created before Engine::RegisterBuiltinModules() registered the engine's modules");
		std::vector<AssetManagerExtensionFactory> factories;
		{
			std::scoped_lock<std::mutex> lock(storage.Mutex);
			factories = storage.Factories;
		}
		std::vector<Scope<AssetManagerExtension>> extensions;
		for (const AssetManagerExtensionFactory& factory : factories)
		{
			if (Scope<AssetManagerExtension> extension = factory(manager))
				extensions.push_back(std::move(extension));
		}
		return extensions;
	}

	////////////////////////////////////////////////////////////////////////////////
	// AssetManagerBase
	////////////////////////////////////////////////////////////////////////////////

	// Shared between the manager and its in-flight jobs, so a job finishing late never touches a destroyed manager.
	struct AssetManagerBase::LoadState
	{
		std::mutex Mutex;
		std::condition_variable Condition;
		std::deque<Completion> Completions;
		uint32_t InFlight = 0;  // Load jobs running (each ends in exactly one PushCompletion)
		uint32_t RangeJobs = 0; // Range read jobs running (RunRange; each ends in EndRangeJob)
		// The manager waits for the loads in flight before it goes (WaitForInFlightLoads): they give up instead of finishing
		// (AssetLoadData::IsCancelled).
		std::atomic<bool> Closing { false };
	};

	AssetManagerBase::AssetManagerBase()
		: m_LoadState(CreateRef<LoadState>()), m_Queue([this](const AssetStreamingRequest& request) { return DispatchLoad(request); }),
		m_Budgets(GetDefaultResidencyBudgets())
	{
		// Frames the GPU may still be working on, plus two: an asset drawn that recently is still in use.
		const IAssetGraphics* graphics = EngineServices::Get<IAssetGraphics>();
		const uint32_t deviceFrames = graphics ? graphics->GetFramesInFlight() : 0;
		const uint32_t framesInFlight = deviceFrames > 0 ? deviceFrames : c_DefaultFramesInFlight;
		m_EvictionGraceFrames = framesInFlight + 2;
		m_GpuReleaseFrames = framesInFlight + 1;
		UpdateStreamingLimits();
		BuiltinAssets::Register(*this);
		m_Extensions = AssetManagerExtensionRegistry::CreateExtensions(*this);
		UpdatePartialLoads();
	}

	AssetManagerBase::~AssetManagerBase()
	{
		WaitForInFlightLoads();
	}

	AssetResidencyBudgets AssetManagerBase::GetDefaultResidencyBudgets()
	{
		const IAssetGraphics* graphics = EngineServices::Get<IAssetGraphics>();
		return AssetResidencyBudgets::FromDeviceBudget(graphics ? graphics->GetMemoryBudget() : 0);
	}

	AssetUploadContext* AssetManagerBase::GetUploadContext() const
	{
		if (m_UploadContext)
			return m_UploadContext.get();
		IAssetGraphics* graphics = EngineServices::Get<IAssetGraphics>();
		return graphics ? graphics->GetUploadContext() : nullptr;
	}

	void AssetManagerBase::SetUploadContext(Ref<AssetUploadContext> context)
	{
		ST_CORE_ASSERT(!m_ProcessingCompletions, "The upload context cannot change while assets are finalized");
		m_UploadContext = std::move(context);
		UpdatePartialLoads();
	}

	void AssetManagerBase::SetPartialLoads(bool enabled)
	{
		m_PartialLoadsAllowed = enabled;
		UpdatePartialLoads();
	}

	void AssetManagerBase::UpdatePartialLoads()
	{
		const AssetUploadContext* upload = GetUploadContext();
		const bool refined = std::any_of(m_Extensions.begin(), m_Extensions.end(),
			[upload](const Scope<AssetManagerExtension>& extension) { return extension->RefinesPartialAssets(upload); });
		m_PartialLoadsActive.store(m_PartialLoadsAllowed && refined, std::memory_order_release);
	}

	bool AssetManagerBase::IsPartialLoadLocked(const AssetEntry& entry) const
	{
		return ReadsPrefix(entry.Metadata);
	}

	uint64_t AssetManagerBase::GetLoadBytesLocked(const AssetEntry& entry) const
	{
		return EstimateFirstLoadBytes(entry.Metadata);
	}

	bool AssetManagerBase::ReadsPrefix(const AssetMetadata& metadata) const
	{
		return ArePartialLoadsActive() && metadata.StoredSize > 0 && AssetLoaderRegistry::FindPrefix(metadata.Type);
	}

	uint64_t AssetManagerBase::EstimateFirstLoadBytes(const AssetMetadata& metadata) const
	{
		// A partial load reads the head (and what its prefix needs beyond it: a cooked texture's tail is about as large).
		return ReadsPrefix(metadata) ? std::min(metadata.StoredSize, c_AssetPrefixHeadBytes) : metadata.StoredSize;
	}

	void AssetManagerBase::WaitForInFlightLoads()
	{
		// Nothing new starts: a job finishing its read would otherwise dispatch the next queued load to this manager. What is
		// being decoded gives up (a scene of a million entities would otherwise hold the manager's end for seconds).
		m_Queue.Close();
		m_LoadState->Closing.store(true, std::memory_order_relaxed);
		std::unique_lock<std::mutex> lock(m_LoadState->Mutex);
		m_LoadState->Condition.wait(lock, [this]() { return m_LoadState->InFlight == 0 && m_LoadState->RangeJobs == 0; });
	}

	bool AssetManagerBase::IsHandleValid(AssetHandle handle) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		return m_Entries.find(handle) != m_Entries.end();
	}

	std::optional<AssetMetadata> AssetManagerBase::GetMetadata(AssetHandle handle) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		if (it == m_Entries.end())
			return std::nullopt;
		return it->second.Metadata;
	}

	AssetType AssetManagerBase::GetAssetType(AssetHandle handle) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		return it != m_Entries.end() ? it->second.Metadata.Type : AssetType::None;
	}

	std::vector<AssetMetadata> AssetManagerBase::GetAllMetadata(AssetType filter) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		std::vector<AssetMetadata> result;
		result.reserve(m_Entries.size());
		for (const auto& [handle, entry] : m_Entries)
		{
			if (filter == AssetType::None || entry.Metadata.Type == filter)
				result.push_back(entry.Metadata);
		}
		std::sort(result.begin(), result.end(), [](const AssetMetadata& a, const AssetMetadata& b)
		{
			return a.Path != b.Path ? a.Path < b.Path : a.SubAssetKey < b.SubAssetKey;
		});
		return result;
	}

	AssetHandle AssetManagerBase::FindAssetByPath(std::string_view path) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_PathIndex.find(std::string(path));
		return it != m_PathIndex.end() ? it->second : UUID::Null();
	}

	Ref<Asset> AssetManagerBase::GetAsset(AssetHandle handle, AssetPriority priority)
	{
		Ref<Asset> current;
		bool requestLoad = false;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			m_GetAssetCalls++;
			auto it = m_Entries.find(handle);
			if (it == m_Entries.end())
				return nullptr;
			TouchLocked(it->second);
			current = it->second.Loaded; // Also the previous version while a reload is in progress
			// A load that is still queued is raised to this priority, and the dependencies of an asset that was read while it
			// was only prefetched are prefetched now (RequestLoad).
			requestLoad = it->second.State == AssetState::Unloaded || it->second.State == AssetState::Loading || it->second.DependenciesDeferred;
		}
		if (requestLoad)
			RequestLoad(handle, priority);
		return current;
	}

	bool AssetManagerBase::TouchLoadedAssets(std::span<const AssetHandle> handles, std::span<const std::weak_ptr<Asset>> expected)
	{
		ST_CORE_ASSERT(handles.size() == expected.size(), "TouchLoadedAssets needs an expected object per handle");
		if (handles.size() != expected.size())
			return false;

		std::scoped_lock<std::mutex> lock(m_Mutex);
		// Everything is checked before anything is touched, so a refusal changes nothing.
		m_TouchedEntries.clear();
		for (size_t index = 0; index < handles.size(); index++)
		{
			auto it = m_Entries.find(handles[index]);
			if (it == m_Entries.end())
				return false;
			AssetEntry& entry = it->second;
			const std::weak_ptr<Asset>& object = expected[index];
			// GetAsset requests the load of an asset that is not ready, and of one whose dependencies wait for a request.
			if (entry.State != AssetState::Ready || entry.DependenciesDeferred || !entry.Loaded || object.owner_before(entry.Loaded)
				|| entry.Loaded.owner_before(object))
				return false;
			m_TouchedEntries.push_back(&entry);
		}
		for (AssetEntry* entry : m_TouchedEntries)
		{
			m_GetAssetCalls++;
			TouchLocked(*entry);
		}
		return true;
	}

	Ref<Asset> AssetManagerBase::PeekLoadedAsset(AssetHandle handle) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		return it != m_Entries.end() ? it->second.Loaded : nullptr;
	}

	Ref<Asset> AssetManagerBase::LoadAssetSync(AssetHandle handle)
	{
		if (m_ProcessingCompletions)
		{
			// The completions this would wait for are being finalized by the caller: waiting would never end.
			ST_CORE_ERROR("LoadAssetSync called while finalizing assets (from Asset::FinalizeOnMainThread); request the load instead");
			return nullptr;
		}

		RequestLoad(handle, AssetPriority::High);
		while (true)
		{
			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				auto it = m_Entries.find(handle);
				if (it == m_Entries.end() || it->second.State == AssetState::Failed)
					return nullptr;
				if (it->second.State == AssetState::Ready)
					return it->second.Loaded;
				if (it->second.State == AssetState::Unloaded)
					return nullptr;
				if (it->second.AwaitingImport)
				{
					// Its import is applied between frames (the subclass's Update), which waiting here would never reach.
					ST_CORE_WARN("'{}' is still importing: it cannot be loaded synchronously yet", it->second.Metadata.Path.empty() ? handle.ToString()
						: it->second.Metadata.Path);
					return nullptr;
				}
			}

			if (ProcessCompletions(false) == 0)
			{
				std::unique_lock<std::mutex> lock(m_LoadState->Mutex);
				m_LoadState->Condition.wait_for(lock, std::chrono::milliseconds(1), [this]() { return !m_LoadState->Completions.empty(); });
			}
		}
	}

	AssetState AssetManagerBase::GetAssetState(AssetHandle handle) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		return it != m_Entries.end() ? it->second.State : AssetState::Unloaded;
	}

	std::string AssetManagerBase::GetAssetError(AssetHandle handle) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		return it != m_Entries.end() ? it->second.Error : std::string("Unknown asset");
	}

	void AssetManagerBase::RequestLoad(AssetHandle handle, AssetPriority priority, float score)
	{
		bool newRequest = false;
		bool loading = false;
		bool prefetchDependencies = false;
		uint64_t generation = 0;
		uint64_t loadBytes = 0;
		bool partial = false;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_Entries.find(handle);
			if (it == m_Entries.end() || it->second.IsMemoryAsset)
				return;
			AssetEntry& entry = it->second;
			TouchLocked(entry);
			// Requested now: its own dependencies are prefetched when it is read, or now if it was read while it was only
			// prefetched (see PrefetchDependencies).
			entry.PrefetchOnly = false;
			const bool deferred = std::exchange(entry.DependenciesDeferred, false);
			if (entry.State == AssetState::Unloaded)
			{
				SetStateLocked(entry, AssetState::Loading);
				entry.Error.clear();
				newRequest = true;
			}
			loading = entry.State == AssetState::Loading;
			prefetchDependencies = deferred && !newRequest && entry.State != AssetState::Failed;
			generation = entry.Generation;
			loadBytes = GetLoadBytesLocked(entry);
			partial = IsPartialLoadLocked(entry);
		}

		// The queue's lock is taken without the manager's: pumping dispatches, which takes the manager's lock.
		if (newRequest)
		{
			m_Queue.Enqueue(handle, priority, score, loadBytes, generation, partial);
			m_Queue.Pump();
		}
		else if (loading && m_Queue.Raise(handle, priority, score))
		{
			m_Queue.Pump();
		}
		if (prefetchDependencies)
			PrefetchDependencies(handle, priority);
	}

	bool AssetManagerBase::DispatchLoad(const AssetStreamingRequest& request)
	{
		if (request.RangeId != 0)
			return DispatchRange(request);

		AssetMetadata metadata;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_Entries.find(request.Handle);
			if (it == m_Entries.end() || it->second.Generation != request.Generation || it->second.State != AssetState::Loading)
				return false; // Unloaded, cancelled or reloaded since it was queued
			metadata = it->second.Metadata;
		}

		// Nothing to read before the asset's first import is applied: the load waits for ResumeLoadsAwaitingImport. The
		// subclass is asked without the manager's lock, so the import may be applied meanwhile, before the entry is marked;
		// asking again after marking it catches that (ResumeLoadsAwaitingImport then found no mark: the load goes on here).
		if (IsAwaitingImport(metadata))
		{
			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				auto it = m_Entries.find(request.Handle);
				if (it == m_Entries.end() || it->second.Generation != request.Generation || it->second.State != AssetState::Loading)
					return false;
				it->second.AwaitingImport = true;
				it->second.AwaitedPriority = request.Priority;
			}
			if (IsAwaitingImport(metadata))
				return false;
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_Entries.find(request.Handle);
			if (it == m_Entries.end() || it->second.Generation != request.Generation || !it->second.AwaitingImport)
				return false; // Resumed (or cancelled, reloaded) meanwhile: that request carries on
			it->second.AwaitingImport = false;
		}

		{
			std::scoped_lock<std::mutex> lock(m_LoadState->Mutex);
			m_LoadState->InFlight++;
		}

		// Jobs are submitted outside the locks: without an initialized job system they run inline right here.
		std::string error;
		try
		{
			JobSystem::SubmitIO([this, metadata, request]() { RunLoad(metadata, request); }, ToJobPriority(request.Priority));
			return true;
		}
		catch (const std::exception& exception)
		{
			error = fmt::format("Could not queue the load: {}", exception.what());
		}
		catch (...)
		{
			error = "Could not queue the load";
		}
		// The job never runs: the load ends here, as a failure the next update publishes.
		m_Queue.OnReadFinished();
		PushCompletion(m_LoadState, Completion { request.Handle, request.Generation, nullptr, std::move(error), request.Bytes });
		return true;
	}

	bool AssetManagerBase::IsAwaitingImport(const AssetMetadata&) const
	{
		return false;
	}

	void AssetManagerBase::ResumeLoadsAwaitingImport(std::span<const AssetHandle> handles)
	{
		std::vector<std::pair<AssetHandle, AssetPriority>> resumed;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			for (const AssetHandle handle : handles)
			{
				auto it = m_Entries.find(handle);
				if (it == m_Entries.end() || !it->second.AwaitingImport)
					continue;
				AssetEntry& entry = it->second;
				// A new request: the one that waited left the queue when it was not dispatched.
				SetStateLocked(entry, AssetState::Unloaded);
				entry.Generation = m_NextGeneration++;
				// A prefetch nobody has requested since stays one: whatever uses the asset requests it.
				if (!entry.PrefetchOnly)
					resumed.emplace_back(handle, entry.AwaitedPriority);
			}
		}
		for (const auto& [handle, priority] : resumed)
			RequestLoad(handle, priority);
	}

	bool AssetManagerBase::IsLoadCurrent(AssetHandle handle, uint64_t generation) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		return it != m_Entries.end() && it->second.Generation == generation && it->second.State == AssetState::Loading;
	}

	void AssetManagerBase::RunLoad(const AssetMetadata& metadata, const AssetStreamingRequest& request)
	{
		// Every path ends in exactly one PushCompletion, also when code inside throws (bad_alloc on huge assets, third-party
		// code), so in-flight counts always drain; the queue hears of the read's end exactly once before that.
		Ref<LoadState> loadState = m_LoadState;
		Completion completion { metadata.Handle, request.Generation, nullptr, {}, request.Bytes };
		bool readReported = false;
		auto finishRead = [this, &readReported]()
		{
			if (!readReported)
				m_Queue.OnReadFinished();
			readReported = true;
		};

		try
		{
			// A load cancelled after it was dispatched never reads, and one cancelled while it read is never decoded: its
			// completion is discarded when it is handled (its generation is stale).
			if (!IsLoadCurrent(metadata.Handle, request.Generation))
			{
				finishRead();
				PushCompletion(loadState, std::move(completion));
				return;
			}

			auto data = CreateRef<std::vector<uint8_t>>();
			std::string error;
			uint64_t storedSize = 0; // Of the whole stored form, when only a prefix was read
			const bool read = request.Partial ? ReadPrefix(metadata, *data, storedSize, &error) : ReadAssetData(metadata, *data, &error);
			// The asset will arrive: what it references starts loading now, alongside its decoding and finalization. Queued
			// before the read leaves its slot, so the pump that follows weighs the dependencies against what waits already.
			const bool current = read && IsLoadCurrent(metadata.Handle, request.Generation);
			if (current)
				PrefetchDependencies(metadata.Handle, request.Priority);
			finishRead();
			if (!read)
			{
				completion.Error = error.empty() ? std::string("Failed to read asset data") : error;
				PushCompletion(loadState, std::move(completion));
				return;
			}
			if (!current)
			{
				PushCompletion(loadState, std::move(completion));
				return;
			}

			if (storedSize > 0)
				m_PartialLoads.fetch_add(1, std::memory_order_relaxed);
			// The job asks the manager whether the load is still wanted: the manager outlives it, since it waits for every load in
			// flight before it goes (WaitForInFlightLoads, which sets Closing first so that the job gives up instead of finishing).
			JobSystem::Submit([manager = this, loadState, metadata, data, storedSize, completion]() mutable
			{
				try
				{
					const AssetLoadFunction* loader = AssetLoaderRegistry::Find(metadata.Type);
					if (!loader)
					{
						completion.Error = fmt::format("No loader for asset type {}", AssetTypeToString(metadata.Type));
					}
					else
					{
						// The bytes are not needed after decoding: the loader may take them over.
						AssetLoadData loadData(*data, storedSize);
						loadData.SetCancelCheck([manager, &loadState, handle = metadata.Handle, generation = completion.Generation]()
						{
							return loadState->Closing.load(std::memory_order_relaxed) || !manager->IsLoadCurrent(handle, generation);
						});
						completion.LoadedAsset = (*loader)(metadata, loadData, &completion.Error);
						if (completion.LoadedAsset)
							completion.LoadedAsset->Handle = metadata.Handle;
						else if (completion.Error.empty())
							completion.Error = "Loader returned no asset";
					}
				}
				catch (const std::exception& exception)
				{
					completion.LoadedAsset = nullptr;
					completion.Error = fmt::format("Loading failed: {}", exception.what());
				}
				catch (...)
				{
					completion.LoadedAsset = nullptr;
					completion.Error = "Loading failed with an unknown exception";
				}
				data.reset(); // The stored bytes are not needed any more; finalization may wait for frames
				PushCompletion(loadState, std::move(completion));
			}, ToJobPriority(request.Priority));
			return;
		}
		catch (const std::exception& exception)
		{
			completion.Error = fmt::format("Reading failed: {}", exception.what());
		}
		catch (...)
		{
			completion.Error = "Reading failed with an unknown exception";
		}
		finishRead();
		completion.LoadedAsset = nullptr;
		PushCompletion(loadState, std::move(completion));
	}

	void AssetManagerBase::PushCompletion(const Ref<LoadState>& loadState, Completion completion) noexcept
	{
		std::scoped_lock<std::mutex> lock(loadState->Mutex);
		// The in-flight count drops first: even if storing the completion fails (out of memory), waiters never hang.
		loadState->InFlight--;
		try
		{
			loadState->Completions.push_back(std::move(completion));
		}
		catch (...)
		{
			ST_CORE_ERROR("Out of memory while completing the load of asset {}", completion.Handle.ToString());
		}
		loadState->Condition.notify_all();
	}

	bool AssetManagerBase::CancelLoad(AssetHandle handle)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		if (it == m_Entries.end() || it->second.State != AssetState::Loading || it->second.PinCount > 0)
			return false;

		AssetEntry& entry = it->second;
		AbandonLoadLocked(entry);
		// A reload that is abandoned leaves the previous version in place.
		SetStateLocked(entry, entry.Loaded ? AssetState::Ready : AssetState::Unloaded);
		entry.Generation = m_NextGeneration++;
		m_Cancellations++;
		return true;
	}

	void AssetManagerBase::UnloadAsset(AssetHandle handle)
	{
		Ref<Asset> released;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_Entries.find(handle);
			if (it == m_Entries.end() || it->second.IsMemoryAsset)
				return;

			AssetEntry& entry = it->second;
			if (entry.Loaded)
				PublishContentChange(handle);
			released = SetLoadedLocked(entry, nullptr);
			if (AbandonLoadLocked(entry))
				m_Cancellations++;
			SetStateLocked(entry, AssetState::Unloaded);
			entry.Error.clear();
			entry.Generation = m_NextGeneration++;
		}
	}

	void AssetManagerBase::ReloadAsset(AssetHandle handle)
	{
		bool wasRequested = false;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_Entries.find(handle);
			if (it == m_Entries.end() || it->second.IsMemoryAsset)
				return;

			AssetEntry& entry = it->second;
			wasRequested = entry.State != AssetState::Unloaded;
			AbandonLoadLocked(entry); // A queued load of the old version is replaced by the new request
			// Keep serving the old object until the new one is ready, so users never see the asset disappear.
			SetStateLocked(entry, AssetState::Unloaded);
			entry.Generation = m_NextGeneration++;
			entry.SourceRevision = NextSourceRevision();
			// Without a loaded object the change is published now: unless the asset is requested, no arrival will tell caches
			// that keep data derived from it (evicted meshes' render layouts) that its source changed.
			if (!entry.Loaded)
				PublishContentChange(handle);
		}

		if (wasRequested)
			RequestLoad(handle, AssetPriority::High);
	}

	AssetSourceRevisions AssetManagerBase::GetSourceRevisions(AssetHandle handle) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		if (it == m_Entries.end())
			return {};
		return AssetSourceRevisions { it->second.SourceRevision, it->second.LoadedRevision };
	}

	bool AssetManagerBase::ReplaceLoadedAsset(AssetHandle handle, const Ref<Asset>& asset, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		if (!asset)
			return fail("No asset object to publish");
		if (m_ProcessingCompletions)
			return fail("Assets cannot be replaced while assets are being finalized (from Asset::FinalizeOnMainThread)");
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_Entries.find(handle);
			if (it == m_Entries.end())
				return fail(fmt::format("Unknown asset {}", handle.ToString()));
			if (it->second.IsMemoryAsset)
				return fail(fmt::format("'{}' exists only in memory and cannot be replaced", it->second.Metadata.Name));
			if (it->second.Metadata.Type != asset->GetType())
			{
				return fail(fmt::format("'{}' is a {}, not a {}", it->second.Metadata.Name, AssetTypeToString(it->second.Metadata.Type),
					AssetTypeToString(asset->GetType())));
			}
		}

		// Finalized like an arrival without a budget, in a batch of its own (main thread only, and no other batch is open: the
		// finalization of completions is not running).
		asset->Handle = handle;
		AssetUploadContext* upload = GetUploadContext();
		if (upload)
			upload->Begin();
		uint64_t uploadedBytes = 0;
		AssetFinalizeContext context { upload, this };
		context.UploadedBytes = &uploadedBytes;
		const AssetFinalizeResult result = asset->FinalizeOnMainThread(context);
		if (upload)
			upload->Submit();
		if (result != AssetFinalizeResult::Done)
			return fail("The asset could not be finalized");

		Ref<Asset> released; // Released after the lock: destroying assets may be slow
		std::scoped_lock<std::mutex> lock(m_Mutex);
		m_FrameUploadedBytes += uploadedBytes;
		auto it = m_Entries.find(handle);
		if (it == m_Entries.end())
			return fail(fmt::format("Asset {} was unregistered while it was finalized", handle.ToString()));
		AssetEntry& entry = it->second;
		if (AbandonLoadLocked(entry))
			m_Cancellations++;
		released = SetLoadedLocked(entry, asset);
		SetStateLocked(entry, AssetState::Ready);
		entry.Error.clear();
		entry.Generation = m_NextGeneration++; // A load still in flight is discarded when it completes
		TouchLocked(entry);
		PublishContentChange(handle);
		return true;
	}

	AssetHandle AssetManagerBase::AddMemoryAsset(const Ref<Asset>& asset, AssetMetadata metadata)
	{
		if (!metadata.Handle.IsValid())
			metadata.Handle = asset->Handle.IsValid() ? asset->Handle : UUID();
		metadata.Type = asset->GetType();
		asset->Handle = metadata.Handle;

		// Memory assets are finalized immediately (main thread), creating GPU resources when a renderer runs. Without an upload
		// budget the asset finishes in this call.
		AssetUploadContext* upload = GetUploadContext();
		if (upload)
			upload->Begin();
		if (asset->FinalizeOnMainThread(AssetFinalizeContext { upload, this }) != AssetFinalizeResult::Done)
			ST_CORE_WARN("Memory asset '{}' could not be finalized", metadata.Name);
		if (upload)
			upload->Submit();

		Ref<Asset> released;
		std::scoped_lock<std::mutex> lock(m_Mutex);
		bool inserted = false;
		AssetEntry& entry = AddEntryLocked(metadata.Handle, inserted);
		if (!inserted)
			AbandonLoadLocked(entry);
		entry.Metadata = metadata;
		entry.IsMemoryAsset = true; // Before the object is set: memory assets are resident but never evicted
		entry.SourceRevision = NextSourceRevision(); // A new object is a new source, also for a memory asset added again
		released = SetLoadedLocked(entry, asset);
		SetStateLocked(entry, AssetState::Ready);
		entry.Error.clear();
		entry.Generation = m_NextGeneration++;
		if (inserted)
			entry.Registration = entry.Generation;
		if (!metadata.Path.empty() && !metadata.IsSubAsset())
			m_PathIndex[metadata.Path] = metadata.Handle;
		PublishContentChange(metadata.Handle);
		m_MetadataVersion.fetch_add(1, std::memory_order_release);
		return metadata.Handle;
	}

	void AssetManagerBase::RegisterAsset(const AssetMetadata& metadata)
	{
		RegisterAssets(std::span<const AssetMetadata>(&metadata, 1));
	}

	void AssetManagerBase::RegisterAssets(std::span<const AssetMetadata> assets)
	{
		if (assets.empty())
			return;
		std::scoped_lock<std::mutex> lock(m_Mutex);
		for (const AssetMetadata& metadata : assets)
			RegisterAssetLocked(metadata);
		m_MetadataVersion.fetch_add(1, std::memory_order_release);
	}

	void AssetManagerBase::RegisterAssetLocked(const AssetMetadata& metadata)
	{
		bool inserted = false;
		AssetEntry& entry = AddEntryLocked(metadata.Handle, inserted);
		if (inserted)
		{
			entry.Generation = m_NextGeneration++;
			entry.Registration = entry.Generation;
			entry.SourceRevision = NextSourceRevision();
		}
		else if (entry.Metadata.Path != metadata.Path && !entry.Metadata.IsSubAsset())
		{
			m_PathIndex.erase(entry.Metadata.Path);
		}
		entry.Metadata = metadata;
		if (!metadata.Path.empty() && !metadata.IsSubAsset())
			m_PathIndex[metadata.Path] = metadata.Handle;
	}

	void AssetManagerBase::UnregisterAsset(AssetHandle handle)
	{
		Ref<Asset> released;
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		if (it == m_Entries.end())
			return;

		AssetEntry& entry = it->second;
		auto pathIt = m_PathIndex.find(entry.Metadata.Path);
		if (pathIt != m_PathIndex.end() && pathIt->second == handle)
			m_PathIndex.erase(pathIt);
		if (entry.Loaded)
			PublishContentChange(handle);
		released = SetLoadedLocked(entry, nullptr);
		AbandonLoadLocked(entry);
		RemoveEntryLocked(it);
		m_MetadataVersion.fetch_add(1, std::memory_order_release);
	}

	void AssetManagerBase::UpdateAssetMetadata(const AssetMetadata& metadata)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(metadata.Handle);
		if (it == m_Entries.end())
			return;

		auto pathIt = m_PathIndex.find(it->second.Metadata.Path);
		if (pathIt != m_PathIndex.end() && pathIt->second == metadata.Handle)
			m_PathIndex.erase(pathIt);
		it->second.Metadata = metadata;
		if (!metadata.Path.empty() && !metadata.IsSubAsset())
			m_PathIndex[metadata.Path] = metadata.Handle;
		m_MetadataVersion.fetch_add(1, std::memory_order_release);
	}

	void AssetManagerBase::SetStoredSize(AssetHandle handle, uint64_t storedSize)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		if (it != m_Entries.end())
			it->second.Metadata.StoredSize = storedSize;
	}

	void AssetManagerBase::UpdateStreamingLimits()
	{
		AssetStreamingLimits limits;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			limits.MaxInFlightBytes = m_Budgets.InFlightBytes;
		}
		// Two reads per I/O thread: one reading, one waiting to start as soon as it is done.
		limits.MaxOutstandingReads = std::max(1u, 2 * JobSystem::GetIOThreadCount());
		m_Queue.SetLimits(limits);
	}

	void AssetManagerBase::Update()
	{
		ST_PROFILE_FUNCTION();
		ST_PROFILE_STAGE("Assets.Update");

		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			m_FrameIndex++;
		}
		// The job system may have been started after this manager was created, and the renderer (whose texture streamer
		// refines partial loads).
		UpdateStreamingLimits();
		UpdatePartialLoads();
		m_Queue.Pump();

		// One upload batch for the frame: the arrivals' finalizations (which join it), the extensions' work and the levels
		// they release before eviction, so that the frame's uploads reach the GPU together, before it renders.
		AssetUploadContext* upload = GetUploadContext();
		if (upload)
			upload->Begin();
		const auto start = std::chrono::steady_clock::now();
		ProcessCompletions(true);

		// The extensions (the renderer's texture streamer) get what is left of the frame's budgets.
		AssetFinalizeContext context { upload, this };
		uint64_t uploadedBytes = 0;
		float uploadMs = 0.0f;
		AssetResidencyBudgets budgets;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			uploadedBytes = m_FrameUploadedBytes;
			uploadMs = m_FrameUploadMs;
			budgets = m_Budgets;
		}
		const uint64_t uploadedBefore = uploadedBytes;
		const float uploadMsBefore = uploadMs;
		context.UploadBudget = budgets.UploadBytesPerFrame - std::min(uploadedBytes, budgets.UploadBytesPerFrame);
		context.UploadMsBudget = std::max(0.0f, budgets.UploadMsPerFrame - uploadMs);
		context.UploadMsFrameBudget = budgets.UploadMsPerFrame;
		context.Deadline = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
			std::chrono::duration<float, std::milli>(std::min(budgets.FinalizeMsPerFrame, 3600.0f * 1000.0f)));
		context.StagingBytes = budgets.StagingBytes;
		context.UploadedBytes = &uploadedBytes;
		context.UploadMs = &uploadMs;
		context.MayWait = true;
		context.FirstOfFrame = uploadedBytes == 0;
		if (!m_Extensions.empty())
		{
			ST_PROFILE_STAGE("Assets.Finalize");
			const auto extensionsStart = std::chrono::steady_clock::now();
			for (const Scope<AssetManagerExtension>& extension : m_Extensions)
				extension->OnUpdate(context);
			ReleaseExtensionMemory(context);
			const float extensionsMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - extensionsStart).count();
			std::scoped_lock<std::mutex> lock(m_Mutex);
			m_FrameUploadedBytes += uploadedBytes - uploadedBefore;
			m_FrameUploadMs += uploadMs - uploadMsBefore;
			m_FrameFinalizeMs += extensionsMs;
		}
		if (upload)
			upload->Submit();

		std::vector<Ref<Asset>> released;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			if (m_ScheduledTrim && m_FrameIndex >= m_ScheduledTrim->Frame)
			{
				const ScheduledTrim trim = std::move(*m_ScheduledTrim);
				m_ScheduledTrim.reset();
				EvictUnusedLocked(trim.UnusedFrames, released, &trim.Keep);
			}
		}
		released.clear(); // Destroyed outside the lock
		EvictToBudgets();
		EndFrameUploads();
	}

	size_t AssetManagerBase::ProcessCompletions(bool applyBudget)
	{
		ST_PROFILE_STAGE("Assets.Finalize");
		if (m_ProcessingCompletions)
			return 0; // Re-entered from a finalizer; the outer call processes everything

		std::deque<Completion> completions;
		{
			std::scoped_lock<std::mutex> lock(m_LoadState->Mutex);
			completions.swap(m_LoadState->Completions);
		}
		if (completions.empty())
			return 0;
		m_ProcessingCompletions = true;
		struct ProcessingScope
		{
			bool& Flag;
			~ProcessingScope() { Flag = false; }
		} processingScope { m_ProcessingCompletions };

		const auto start = std::chrono::steady_clock::now();
		const auto elapsedMs = [start]() { return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count(); };
		AssetResidencyBudgets budgets;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			budgets = m_Budgets;
		}
		// More than an hour is no limit (and would overflow the clock's arithmetic).
		const float finalizeMs = std::min(budgets.FinalizeMsPerFrame, 3600.0f * 1000.0f);
		const auto deadline = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float, std::milli>(finalizeMs));

		AssetUploadContext* upload = GetUploadContext();
		if (upload)
			upload->Begin();

		size_t processed = 0;
		size_t finalizations = 0; // FinalizeOnMainThread calls of this frame
		uint64_t uploadedBytes = 0;
		float uploadMs = 0.0f;    // Time of the frame's upload steps
		std::vector<Ref<Asset>> released;
		// Arrivals waiting until the GPU memory evicted for them is released, and those behind them (see below).
		std::deque<Completion> waiting;
		// Arrivals whose uploads are recorded and run on a queue beside the frame's work (AssetFinalizeResult::AwaitingGPU):
		// finalized again in a later frame, while the arrivals behind them go on.
		std::deque<Completion> awaitingGpu;
		bool arrivalWaiting = false;
		uint64_t waitingGpuBytes = 0; // GPU memory the waiting arrivals will allocate
		bool makeRoomEarly = true;
		while (!completions.empty())
		{
			// The frame's first finalization always proceeds, so an asset larger than the budget still loads.
			if (applyBudget && processed > 0 && (uploadedBytes >= budgets.UploadBytesPerFrame || std::chrono::steady_clock::now() >= deadline))
				break;

			Completion& completion = completions.front();
			bool current = false;
			bool prefetched = false; // Requested by nobody yet: its finalization requests nothing (prefetches do not chain)
			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				auto it = m_Entries.find(completion.Handle);
				current = it != m_Entries.end() && it->second.Generation == completion.Generation;
				prefetched = current && it->second.PrefetchOnly;
				if (!current)
					m_Reserved -= completion.Reserved;
			}
			if (!current)
			{
				// Unregistered, unloaded, cancelled or reloaded since the request (also while its upload was under way):
				// discard. The load leaves the flight; the queue may dispatch the next one.
				processed++;
				m_Queue.OnLoadFinished(completion.InFlightBytes);
				completions.pop_front();
				continue;
			}

			if (completion.LoadedAsset && !completion.FinalizeStarted)
			{
				// Room first. GPU memory of what was evicted for it is released only once no frame in flight can use it; with a
				// renderer and a budget, an arrival that allocates GPU memory waits for that, so the device never holds both.
				const AssetMemoryUsage incoming = completion.LoadedAsset->GetFinalizedMemoryUsage();
				// A prefetch nobody asked for since makes no room, and takes no room the loads still on their way will need: when
				// it does not fit beside them, it goes unfinalized (it would be uploaded only to be evicted). The load leaves the
				// flight like a discarded one.
				if (!completion.RoomMade)
				{
					const AssetStreamingQueueStats queue = m_Queue.GetStats();
					const uint64_t pendingBytes = queue.WaitingBytes + (queue.InFlightBytes - std::min(completion.InFlightBytes, queue.InFlightBytes));
					bool dropped = false;
					{
						std::scoped_lock<std::mutex> lock(m_Mutex);
						auto it = m_Entries.find(completion.Handle);
						dropped = it != m_Entries.end() && it->second.Generation == completion.Generation
							&& DropUnfittingPrefetchLocked(it->second, incoming, pendingBytes);
					}
					if (dropped)
					{
						processed++;
						m_Queue.OnLoadFinished(completion.InFlightBytes);
						completions.pop_front();
						continue;
					}
				}
				// Arrivals are finalized in order: the ones behind a waiting arrival wait for it. They make room at once, so
				// that arrivals that need room in the same frame wait their releases out together instead of one after
				// another, but only for as much GPU memory as a frame uploads (UploadBytesPerFrame): evicting far ahead of what
				// can be uploaded raised the memory peak of the stress sweep, as did letting them overtake (they allocate, and
				// materials request their textures, while what the waiting arrival replaces and its loaded data are held).
				if (applyBudget && arrivalWaiting && !completion.RoomMade)
					makeRoomEarly = makeRoomEarly && waitingGpuBytes + incoming.GetGpu() <= budgets.UploadBytesPerFrame;
				if (!completion.RoomMade && (!applyBudget || !arrivalWaiting || makeRoomEarly))
				{
					completion.RoomMade = true;
					const RoomResult room = MakeRoomFor(completion.Handle, incoming);
					completion.Reserved = room.Reserved;
					// An arrival that allocates no GPU memory (a material that made room in the CPU pool) has nothing to wait
					// for.
					if (room.EvictedGpuBytes > 0 && incoming.GetGpu() > 0 && upload)
						completion.FinalizeFrame = GetFrameIndex() + m_GpuReleaseFrames;
				}
				const bool waits = arrivalWaiting || GetFrameIndex() < completion.FinalizeFrame;
				if (applyBudget && waits)
				{
					arrivalWaiting = true;
					waitingGpuBytes += incoming.GetGpu();
					waiting.push_back(std::move(completion));
					completions.pop_front();
					continue;
				}
				completion.FinalizeStarted = true;
			}

			// Finalization runs without holding the lock: it may request dependent assets (e.g. a material's textures). With
			// the budget, it gets what is left of the frame's: a large upload continues in the next frame (Pending).
			processed++;
			if (completion.LoadedAsset)
			{
				AssetFinalizeContext context { upload, this };
				context.Prefetched = prefetched;
				if (applyBudget)
				{
					context.UploadBudget = budgets.UploadBytesPerFrame - std::min(uploadedBytes, budgets.UploadBytesPerFrame);
					context.UploadMsBudget = std::max(0.0f, budgets.UploadMsPerFrame - uploadMs);
					context.UploadMsFrameBudget = budgets.UploadMsPerFrame;
					context.Deadline = deadline;
					context.StagingBytes = budgets.StagingBytes;
				}
				context.UploadedBytes = &uploadedBytes;
				context.UploadMs = &uploadMs;
				context.MayWait = applyBudget;
				context.FirstOfFrame = finalizations == 0;
				finalizations++;
				AssetFinalizeResult result = completion.LoadedAsset->FinalizeOnMainThread(context);
				// Without a budget an asset has to finish in its call, but what it needs may not happen in the open batch: a mesh
				// whose geometry needs the geometry pool to grow after the batch wrote into the pool waits for the next batch
				// (GeometryPool::Allocate). It gets that batch at once: what the open one recorded is submitted, and the asset
				// is finalized again in a new one.
				if (result == AssetFinalizeResult::Pending && !applyBudget && upload)
				{
					upload->Submit();
					upload->Begin();
					result = completion.LoadedAsset->FinalizeOnMainThread(context);
				}
				if (result == AssetFinalizeResult::Pending && applyBudget)
					break; // Stays in line
				if (result == AssetFinalizeResult::AwaitingGPU && applyBudget)
				{
					awaitingGpu.push_back(std::move(completion));
					completions.pop_front();
					continue;
				}
				if (result != AssetFinalizeResult::Done)
				{
					// Without a budget an asset has to finish: one that still asks for more in a batch of its own is broken.
					completion.Error = result == AssetFinalizeResult::Failed ? "Failed to create GPU resources"
						: "Finalization did not finish without an upload budget";
					completion.LoadedAsset = nullptr;
				}
			}

			// The load leaves the flight once it is handled, whatever becomes of it; the queue may dispatch the next one.
			m_Queue.OnLoadFinished(completion.InFlightBytes);
			Completion handled = std::move(completion);
			completions.pop_front();

			std::scoped_lock<std::mutex> lock(m_Mutex);
			m_Reserved -= handled.Reserved; // Counted as resident from now on, or not at all
			auto it = m_Entries.find(handled.Handle);
			if (it == m_Entries.end() || it->second.Generation != handled.Generation)
				continue;

			AssetEntry& entry = it->second;
			if (handled.LoadedAsset)
			{
				released.push_back(SetLoadedLocked(entry, handled.LoadedAsset));
				SetStateLocked(entry, AssetState::Ready);
				entry.Error.clear();
				PublishContentChange(handled.Handle);
			}
			else
			{
				SetStateLocked(entry, AssetState::Failed);
				entry.Error = handled.Error;
				ST_CORE_ERROR("Failed to load asset '{}' ({}): {}", entry.Metadata.Path.empty() ? entry.Metadata.Name : entry.Metadata.Path,
					entry.Metadata.Handle.ToString(), handled.Error);
			}
			m_TotalLoadsCompleted++;
		}

		// What the batch recorded reaches the GPU now, ahead of the frame's rendering.
		if (upload)
			upload->Submit();

		// What is left over (waiting for the GPU or for released memory, or the budget ran out) is finalized in a later frame, in
		// its place in line: ahead of newer completions, the arrivals waiting for the GPU first.
		if (!completions.empty() || !waiting.empty() || !awaitingGpu.empty())
		{
			std::scoped_lock<std::mutex> lock(m_LoadState->Mutex);
			for (std::deque<Completion>* leftOver : { &completions, &waiting, &awaitingGpu })
			{
				while (!leftOver->empty())
				{
					m_LoadState->Completions.push_front(std::move(leftOver->back()));
					leftOver->pop_back();
				}
			}
		}

		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			m_FrameUploadedBytes += uploadedBytes;
			m_FrameUploadMs += uploadMs;
			m_FrameFinalizeMs += elapsedMs();
		}
		return processed;
	}

	void AssetManagerBase::EndFrameUploads()
	{
		uint64_t uploadedBytes = 0;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			uploadedBytes = m_FrameUploadedBytes;
			m_LastFrameUploadedBytes = m_FrameUploadedBytes;
			m_LastFrameFinalizeMs = m_FrameFinalizeMs;
			m_LastFrameUploadMs = m_FrameUploadMs;
			m_UploadedBytesHistory[m_HistoryCursor] = m_FrameUploadedBytes;
			m_FinalizeMsHistory[m_HistoryCursor] = m_FrameFinalizeMs;
			m_UploadMsHistory[m_HistoryCursor] = m_FrameUploadMs;
			m_HistoryCursor = (m_HistoryCursor + 1) % c_StatsWindowFrames;
			m_FrameUploadedBytes = 0;
			m_FrameFinalizeMs = 0.0f;
			m_FrameUploadMs = 0.0f;
		}

		// Staging that went idle once uploads stopped is returned by the context (also without device frames, which tools and
		// tests may not run).
		AssetUploadContext* upload = GetUploadContext();
		if (!upload || !upload->EndFrame(uploadedBytes))
			return;
		std::scoped_lock<std::mutex> lock(m_Mutex);
		m_StagingReleases++;
	}

	bool AssetManagerBase::HasPendingLoads() const
	{
		{
			std::scoped_lock<std::mutex> lock(m_LoadState->Mutex);
			if (m_LoadState->InFlight > 0 || !m_LoadState->Completions.empty())
				return true;
		}
		// Range reads are their owner's to poll: they end without the manager's updates.
		return m_Queue.HasLoadWork();
	}

	bool AssetManagerBase::WaitForPendingLoads(std::chrono::milliseconds timeout)
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (HasPendingLoads())
		{
			if (std::chrono::steady_clock::now() >= deadline)
				return false;
			if (ProcessCompletions(false) == 0)
			{
				std::unique_lock<std::mutex> lock(m_LoadState->Mutex);
				m_LoadState->Condition.wait_for(lock, std::chrono::milliseconds(1));
			}
		}
		return true;
	}

	void AssetManagerBase::PublishContentChange(AssetHandle handle)
	{
		const uint64_t version = m_ContentVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
		m_ContentChanges.push_back(ContentChange { version, handle });
		if (m_ContentChanges.size() > c_MaxContentChanges)
			m_ContentChanges.pop_front();
	}

	bool AssetManagerBase::GetContentChanges(uint64_t sinceVersion, std::vector<AssetHandle>& outHandles) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		if (sinceVersion >= m_ContentVersion.load(std::memory_order_acquire))
			return true;
		// The changes after sinceVersion start with version sinceVersion + 1, which must still be remembered.
		if (m_ContentChanges.empty() || m_ContentChanges.front().Version > sinceVersion + 1)
			return false;
		for (auto it = m_ContentChanges.rbegin(); it != m_ContentChanges.rend() && it->Version > sinceVersion; ++it)
			outHandles.push_back(it->Handle);
		return true;
	}

	AssetMemoryUsage AssetManagerBase::GetResidentMemory() const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		return m_Resident;
	}

	AssetManagerStats AssetManagerBase::GetStats() const
	{
		AssetManagerStats stats;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			// Counted as the entries change: the editor reads the statistics every frame (the status bar's assets pill).
			stats.RegisteredAssets = static_cast<uint32_t>(m_Entries.size());
			stats.LoadedAssets = m_StateCounts[static_cast<size_t>(AssetState::Ready)];
			stats.LoadingAssets = m_StateCounts[static_cast<size_t>(AssetState::Loading)];
			stats.FailedAssets = m_StateCounts[static_cast<size_t>(AssetState::Failed)];
			stats.PinnedAssets = m_PinnedAssets;
			stats.TotalLoadsCompleted = m_TotalLoadsCompleted;
			stats.GetAssetCalls = m_GetAssetCalls;
			stats.Frame = m_FrameIndex;
			stats.Resident = m_Resident;
			stats.LoadedMemory = m_Resident.GetTotal();
			stats.Budgets = m_Budgets;
			stats.UploadedBytesLastFrame = m_LastFrameUploadedBytes;
			stats.FinalizeMsLastFrame = m_LastFrameFinalizeMs;
			stats.UploadMsLastFrame = m_LastFrameUploadMs;
			for (size_t index = 0; index < c_StatsWindowFrames; index++)
			{
				stats.UploadedBytesWindowMax = std::max(stats.UploadedBytesWindowMax, m_UploadedBytesHistory[index]);
				stats.FinalizeMsWindowMax = std::max(stats.FinalizeMsWindowMax, m_FinalizeMsHistory[index]);
				stats.UploadMsWindowMax = std::max(stats.UploadMsWindowMax, m_UploadMsHistory[index]);
			}
			stats.Evictions = m_Evictions;
			stats.EvictionChecks = m_EvictionChecks;
			stats.Cancellations = m_Cancellations;
			stats.StagingReleases = m_StagingReleases;
			stats.DependencyPrefetches = m_DependencyPrefetches;
			stats.DependencyPrefetchesDropped = m_DependencyPrefetchesDropped;
			stats.RangeReads = static_cast<uint32_t>(m_RangeReads.size());
			stats.RangeReadsCompleted = m_RangeReadsCompleted;
		}
		stats.PartialLoads = m_PartialLoads.load(std::memory_order_relaxed);
		for (const Scope<AssetManagerExtension>& extension : m_Extensions)
			extension->AddStats(stats);

		const AssetStreamingQueueStats queue = m_Queue.GetStats();
		stats.QueuedLoads = queue.Queued;
		stats.InFlightLoads = queue.InFlightLoads;
		stats.OutstandingReads = queue.OutstandingReads;
		stats.InFlightBytes = queue.InFlightBytes;
		stats.InFlightBytesHighWater = queue.InFlightBytesHighWater;
		return stats;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Dependencies, bounds and group loads
	////////////////////////////////////////////////////////////////////////////////

	std::vector<AssetHandle> AssetManagerBase::NormalizeReferences(AssetHandle handle, std::span<const AssetHandle> references)
	{
		std::vector<AssetHandle> unique;
		unique.reserve(references.size());
		std::unordered_set<AssetHandle> seen;
		for (AssetHandle reference : references)
		{
			if (reference.IsValid() && reference != handle && seen.insert(reference).second)
				unique.push_back(reference);
		}
		return unique;
	}

	void AssetManagerBase::SetAssetReferences(AssetHandle handle, std::span<const AssetHandle> dependencies, std::optional<AABB> bounds)
	{
		std::vector<AssetHandle> unique = NormalizeReferences(handle, dependencies);
		const auto isFinite = [](const glm::vec3& point) { return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z); };
		if (bounds && (!bounds->IsValid() || !isFinite(bounds->Min) || !isFinite(bounds->Max)))
			bounds.reset();

		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		if (it == m_Entries.end())
			return;
		it->second.Dependencies = std::move(unique);
		const std::optional<AABB>& previous = it->second.Bounds;
		const bool boundsChanged = previous.has_value() != bounds.has_value() || (bounds && (previous->Min != bounds->Min || previous->Max != bounds->Max));
		it->second.Bounds = bounds;
		// Caches that place meshes by their recorded bounds before they load (render scenes) learn of new bounds like of new
		// content: an import recorded after a scene was first drawn, or a pack mounted over another.
		if (boundsChanged)
			PublishContentChange(handle);
	}

	std::vector<AssetHandle> AssetManagerBase::GetDependencies(AssetHandle handle) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		return it != m_Entries.end() ? it->second.Dependencies : std::vector<AssetHandle>();
	}

	std::vector<AssetHandle> AssetManagerBase::GetDependencyClosureLocked(AssetHandle handle, AssetClosureScope scope) const
	{
		std::vector<AssetHandle> closure;
		if (m_Entries.find(handle) == m_Entries.end())
			return closure;
		std::unordered_set<AssetHandle> visited { handle };
		closure.push_back(handle);
		// Breadth first: the list itself is the queue.
		for (size_t index = 0; index < closure.size(); index++)
		{
			const AssetEntry& entry = m_Entries.at(closure[index]);
			for (AssetHandle dependency : entry.Dependencies)
			{
				auto dependencyIt = m_Entries.find(dependency);
				if (dependencyIt == m_Entries.end())
					continue;
				if (scope == AssetClosureScope::Load && dependencyIt->second.Metadata.Type == AssetType::Scene)
					continue;
				if (visited.insert(dependency).second)
					closure.push_back(dependency);
			}
		}
		return closure;
	}

	std::vector<AssetHandle> AssetManagerBase::GetDependencyClosure(AssetHandle handle, AssetClosureScope scope) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		return GetDependencyClosureLocked(handle, scope);
	}

	std::optional<AABB> AssetManagerBase::GetBounds(AssetHandle handle) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		return it != m_Entries.end() ? it->second.Bounds : std::nullopt;
	}

	bool AssetManagerBase::IsLoadedWithDependencies(AssetHandle handle) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		const std::vector<AssetHandle> closure = GetDependencyClosureLocked(handle, AssetClosureScope::Load);
		if (closure.empty())
			return false;
		return std::all_of(closure.begin(), closure.end(), [this](AssetHandle member) { return m_Entries.at(member).State == AssetState::Ready; });
	}

	void AssetManagerBase::PrefetchDependencies(AssetHandle handle, AssetPriority priority)
	{
		struct Request
		{
			AssetHandle Handle;
			uint64_t Generation = 0;
			uint64_t Bytes = 0;
			bool Partial = false;
		};
		std::vector<Request> started;
		std::vector<AssetHandle> loading;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_Entries.find(handle);
			if (it == m_Entries.end() || !PrefetchesDependencies(it->second.Metadata.Type))
				return;
			// Prefetches do not chain: what an asset that was only prefetched references is prefetched once something requests
			// it (RequestLoad), and the asset requests nothing when it is finalized (AssetFinalizeContext::Prefetched).
			if (it->second.PrefetchOnly)
			{
				it->second.DependenciesDeferred = true;
				return;
			}
			// What is on its way already (requested loads, earlier prefetches) arrives first: prefetches take only the headroom
			// the budgets have beside it, so they never evict anything in use. The queue's lock may be taken inside the
			// manager's (never the other way round).
			const AssetStreamingQueueStats queue = m_Queue.GetStats();
			uint64_t pendingBytes = queue.WaitingBytes + queue.InFlightBytes;
			for (AssetHandle dependency : it->second.Dependencies)
			{
				auto dependencyIt = m_Entries.find(dependency);
				if (dependencyIt == m_Entries.end() || dependencyIt->second.IsMemoryAsset)
					continue;
				// Scenes a document names (a world's cells, the next level) load when something asks for them: prefetches stop at
				// them as load closures do (AssetClosureScope::Load). A World asset would otherwise bring in every cell it lists.
				if (dependencyIt->second.Metadata.Type == AssetType::Scene)
					continue;
				AssetEntry& entry = dependencyIt->second;
				// Not touched: a prefetch is no use, so it keeps nothing resident and makes no room (see GetDependencies).
				if (entry.State == AssetState::Unloaded)
				{
					const uint64_t loadBytes = GetLoadBytesLocked(entry);
					const uint64_t bytes = loadBytes > 0 ? loadBytes : AssetStreamingQueue::c_UnknownSizeBytes;
					if (!FitsPrefetchLocked(bytes, pendingBytes))
						continue;
					pendingBytes += bytes;
					SetStateLocked(entry, AssetState::Loading);
					entry.Error.clear();
					entry.PrefetchOnly = true;
					entry.DependenciesDeferred = false; // Its read decides again
					started.push_back(Request { dependency, entry.Generation, loadBytes, IsPartialLoadLocked(entry) });
				}
				else if (entry.State == AssetState::Loading)
				{
					loading.push_back(dependency);
				}
			}
			m_DependencyPrefetches += started.size();
		}
		if (started.empty() && loading.empty())
			return;

		// The queue's lock is taken without the manager's (see RequestLoad).
		for (const Request& request : started)
			m_Queue.Enqueue(request.Handle, priority, c_DependencyPrefetchScore, request.Bytes, request.Generation, request.Partial);
		for (AssetHandle dependency : loading)
			m_Queue.Raise(dependency, priority, c_DependencyPrefetchScore);
		m_Queue.Pump();
	}

	bool AssetManagerBase::FitsPrefetchLocked(uint64_t bytes, uint64_t pendingBytes) const
	{
		// Room that requested loads cannot need: beside what is resident, reserved for arrivals and on its way, a whole window
		// of loads in flight (InFlightBytes) stays free for the requests still to come. Tight budgets prefetch nothing: in the
		// stress sweep (a 128 MB texture budget), prefetching textures into the room left beside what was on its way still
		// raised the peak private memory by about 25 MB on average.
		// The in-flight budget may be set to anything: the sum saturates.
		const auto add = [](uint64_t a, uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; };
		for (AssetMemoryPool pool : c_AssetMemoryPools)
		{
			const uint64_t budget = m_Budgets.GetPoolBudget(pool);
			if (budget == AssetResidencyBudgets::c_Unlimited)
				continue;
			const uint64_t held = add(add(add(GetPoolBytes(m_Resident, pool), GetPoolBytes(m_Reserved, pool)), pendingBytes), m_Budgets.InFlightBytes);
			if (held > budget || bytes > budget - held)
				return false;
		}
		return true;
	}

	bool AssetManagerBase::DropUnfittingPrefetchLocked(AssetEntry& entry, const AssetMemoryUsage& incoming, uint64_t pendingBytes)
	{
		if (!entry.PrefetchOnly || entry.State != AssetState::Loading)
			return false;
		bool fits = true;
		for (AssetMemoryPool pool : c_AssetMemoryPools)
		{
			const uint64_t budget = m_Budgets.GetPoolBudget(pool);
			const uint64_t bytes = GetPoolBytes(incoming, pool);
			if (budget == AssetResidencyBudgets::c_Unlimited || bytes == 0)
				continue;
			const uint64_t held = GetPoolBytes(m_Resident, pool) + GetPoolBytes(m_Reserved, pool) + pendingBytes;
			fits = fits && held <= budget && bytes <= budget - held;
		}
		if (fits)
			return false;

		SetStateLocked(entry, AssetState::Unloaded); // Prefetches start from Unloaded: there is no previous version to keep
		entry.Generation = m_NextGeneration++;
		m_DependencyPrefetchesDropped++;
		return true;
	}

	AssetGroupRequest AssetManagerBase::RequestLoadWithDependencies(AssetHandle handle, AssetPriority priority)
	{
		return RequestLoadGroup(GetDependencyClosure(handle, AssetClosureScope::Load), priority);
	}

	AssetGroupRequest AssetManagerBase::RequestLoadGroup(std::span<const AssetHandle> assets, AssetPriority priority)
	{
		AssetGroupRequest group;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			std::unordered_set<AssetHandle> members;
			for (AssetHandle member : assets)
			{
				auto it = m_Entries.find(member);
				if (it == m_Entries.end() || !members.insert(member).second)
					continue;
				const uint64_t weight = std::max<uint64_t>(it->second.Metadata.StoredSize, 1);
				group.m_Assets.push_back(member);
				group.m_Weights.push_back(weight);
				group.m_TotalWeight += weight;
			}
		}
		if (group.m_Assets.empty())
			return group;

		group.m_Manager = weak_from_this();
		group.m_Finished.assign(group.m_Assets.size(), false);
		// Pinning requests the loads, in breadth-first order: the root first.
		group.m_Pins.reserve(group.m_Assets.size());
		for (AssetHandle member : group.m_Assets)
			group.m_Pins.push_back(Pin(member, priority));
		return group;
	}

	bool AssetManagerBase::ReadStoredData(AssetHandle handle, std::vector<uint8_t>& outData, std::string* outError)
	{
		std::optional<AssetMetadata> metadata = GetMetadata(handle);
		if (!metadata || IsBuiltinAssetHandle(handle))
		{
			if (outError)
				*outError = fmt::format("Asset {} has no stored form here", handle.ToString());
			return false;
		}
		try
		{
			return ReadAssetData(*metadata, outData, outError);
		}
		catch (const std::exception& exception)
		{
			if (outError)
				*outError = fmt::format("Reading asset {} failed: {}", handle.ToString(), exception.what());
			return false;
		}
	}

	bool AssetManagerBase::ReadPrefix(const AssetMetadata& metadata, std::vector<uint8_t>& outData, uint64_t& outStoredSize, std::string* outError)
	{
		outStoredSize = 0;
		const AssetPrefixFunction* prefix = AssetLoaderRegistry::FindPrefix(metadata.Type);
		const uint64_t storedSize = metadata.StoredSize;
		// The head, then what the prefix needs beyond it. A head that cannot be read (a stored size that no longer matches the
		// stored form) is no reason to fail: the whole stored form is read instead, as is one the function does not split.
		if (prefix && storedSize > 0 && ReadAssetRange(metadata, 0, std::min(storedSize, c_AssetPrefixHeadBytes), outData, nullptr))
		{
			const std::optional<uint64_t> prefixSize = (*prefix)(outData, storedSize);
			if (prefixSize && *prefixSize > 0 && *prefixSize < storedSize)
			{
				if (*prefixSize <= outData.size())
				{
					outData.resize(static_cast<size_t>(*prefixSize));
				}
				else
				{
					std::vector<uint8_t> rest;
					if (!ReadAssetRange(metadata, outData.size(), *prefixSize - outData.size(), rest, outError))
						return false;
					outData.insert(outData.end(), rest.begin(), rest.end());
				}
				outStoredSize = storedSize;
				return true;
			}
			// Nothing to split: a head that is the whole stored form is all the load needs.
			if (outData.size() == storedSize)
				return true;
		}
		outData.clear();
		return ReadAssetData(metadata, outData, outError);
	}

	bool AssetManagerBase::HasStoredForm(AssetHandle handle) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		return it != m_Entries.end() && !it->second.IsMemoryAsset && !it->second.Metadata.IsBuiltin();
	}

	uint64_t AssetManagerBase::RequestRange(AssetHandle handle, uint64_t offset, uint64_t size, AssetPriority priority, float score)
	{
		uint64_t id = 0;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_Entries.find(handle);
			if (it == m_Entries.end() || it->second.IsMemoryAsset || it->second.Metadata.IsBuiltin() || it->second.State != AssetState::Ready || size == 0)
				return 0;
			const uint64_t storedSize = it->second.Metadata.StoredSize;
			if (storedSize > 0 && (offset > storedSize || size > storedSize - offset))
				return 0;
			id = m_NextRangeId++;
			RangeRead& read = m_RangeReads[id];
			read.Handle = handle;
			read.Generation = it->second.Generation;
			read.Offset = offset;
			read.Size = size;
		}
		// The queue's lock is taken without the manager's (see RequestLoad).
		m_Queue.EnqueueRange(id, handle, priority, score, size);
		m_Queue.Pump();
		return id;
	}

	void AssetManagerBase::RaiseRange(uint64_t id, AssetPriority priority, float score)
	{
		if (m_Queue.RaiseRange(id, priority, score))
			m_Queue.Pump();
	}

	AssetRangeStatus AssetManagerBase::PollRange(uint64_t id, std::vector<uint8_t>& outData, std::string* outError)
	{
		uint64_t releasedBytes = 0;
		AssetRangeStatus status = AssetRangeStatus::Pending;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_RangeReads.find(id);
			if (it == m_RangeReads.end() || it->second.Cancelled)
				return AssetRangeStatus::Unknown;
			RangeRead& read = it->second;
			if (!read.Done)
				return AssetRangeStatus::Pending;
			// Bytes of another version of the asset are of no use: it was unloaded, evicted or reloaded meanwhile.
			auto entry = m_Entries.find(read.Handle);
			const bool current = entry != m_Entries.end() && entry->second.Generation == read.Generation;
			if (read.Error.empty() && current)
			{
				outData = std::move(read.Data);
				status = AssetRangeStatus::Complete;
				m_RangeReadsCompleted++;
			}
			else
			{
				if (outError)
					*outError = !read.Error.empty() ? read.Error : std::string("The asset changed while its range was read");
				status = AssetRangeStatus::Failed;
			}
			releasedBytes = read.Dispatched ? read.Size : 0;
			const bool dispatched = read.Dispatched;
			m_RangeReads.erase(it);
			if (!dispatched)
				return status;
		}
		m_Queue.OnLoadFinished(releasedBytes, true);
		return status;
	}

	void AssetManagerBase::CancelRange(uint64_t id)
	{
		uint64_t releasedBytes = 0;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_RangeReads.find(id);
			if (it == m_RangeReads.end() || it->second.Cancelled)
				return;
			RangeRead& read = it->second;
			if (read.Running)
			{
				read.Cancelled = true; // Its job forgets it when it ends
				return;
			}
			if (read.Done && !read.Dispatched)
			{
				m_RangeReads.erase(it); // Refused when it was dispatched: nothing is in flight
				return;
			}
			if (!read.Dispatched)
			{
				// Still waiting: the queue drops it. Taken by a pump meanwhile, the dispatch finds it cancelled and refuses it.
				// The queue's lock may be taken inside the manager's.
				if (!m_Queue.CancelRange(id))
				{
					read.Cancelled = true;
					return;
				}
				m_RangeReads.erase(it);
				return;
			}
			releasedBytes = read.Size; // Done: its bytes leave the flight
			m_RangeReads.erase(it);
		}
		m_Queue.OnLoadFinished(releasedBytes, true);
	}

	bool AssetManagerBase::DispatchRange(const AssetStreamingRequest& request)
	{
		AssetMetadata metadata;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_RangeReads.find(request.RangeId);
			if (it == m_RangeReads.end())
				return false;
			RangeRead& read = it->second;
			if (read.Cancelled)
			{
				m_RangeReads.erase(it);
				return false;
			}
			auto entry = m_Entries.find(read.Handle);
			if (entry == m_Entries.end() || entry->second.Generation != read.Generation)
			{
				// Nothing to read any more: the owner's next poll reports it.
				read.Done = true;
				read.Error = "The asset changed before its range was read";
				return false;
			}
			metadata = entry->second.Metadata;
			read.Dispatched = true;
			read.Running = true;
		}
		{
			std::scoped_lock<std::mutex> lock(m_LoadState->Mutex);
			m_LoadState->RangeJobs++;
		}
		try
		{
			JobSystem::SubmitIO([this, metadata, request]() { RunRange(metadata, request); }, ToJobPriority(request.Priority));
			return true;
		}
		catch (...)
		{
		}
		// The job never runs: the read ends here, failed.
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_RangeReads.find(request.RangeId);
			if (it != m_RangeReads.end())
			{
				it->second.Running = false;
				it->second.Done = true;
				it->second.Error = "Could not queue the read";
			}
		}
		m_Queue.OnReadFinished();
		EndRangeJob();
		return true;
	}

	void AssetManagerBase::RunRange(const AssetMetadata& metadata, const AssetStreamingRequest& request)
	{
		uint64_t offset = 0;
		uint64_t size = 0;
		bool cancelled = false;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_RangeReads.find(request.RangeId);
			cancelled = it == m_RangeReads.end() || it->second.Cancelled;
			if (!cancelled)
			{
				offset = it->second.Offset;
				size = it->second.Size;
			}
		}

		std::vector<uint8_t> data;
		std::string error;
		bool read = false;
		if (!cancelled)
		{
			try
			{
				read = ReadAssetRange(metadata, offset, size, data, &error);
			}
			catch (const std::exception& exception)
			{
				error = fmt::format("Reading failed: {}", exception.what());
			}
			catch (...)
			{
				error = "Reading failed with an unknown exception";
			}
		}
		m_Queue.OnReadFinished();

		bool release = false;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_RangeReads.find(request.RangeId);
			if (it == m_RangeReads.end() || it->second.Cancelled)
			{
				// Abandoned while it read: its bytes leave the flight now.
				if (it != m_RangeReads.end())
					m_RangeReads.erase(it);
				release = true;
			}
			else
			{
				RangeRead& range = it->second;
				range.Running = false;
				range.Done = true;
				if (read)
					range.Data = std::move(data);
				else
					range.Error = error.empty() ? std::string("Failed to read the range") : error;
			}
		}
		if (release)
			m_Queue.OnLoadFinished(request.Bytes, true);
		EndRangeJob();
	}

	void AssetManagerBase::EndRangeJob()
	{
		std::scoped_lock<std::mutex> lock(m_LoadState->Mutex);
		m_LoadState->RangeJobs--;
		m_LoadState->Condition.notify_all();
	}

	bool AssetManagerBase::ReadStoredRange(AssetHandle handle, uint64_t offset, uint64_t size, std::vector<uint8_t>& outData, std::string* outError)
	{
		std::optional<AssetMetadata> metadata = GetMetadata(handle);
		if (!metadata || IsBuiltinAssetHandle(handle))
		{
			if (outError)
				*outError = fmt::format("Asset {} has no stored form here", handle.ToString());
			return false;
		}
		try
		{
			return ReadAssetRange(*metadata, offset, size, outData, outError);
		}
		catch (const std::exception& exception)
		{
			if (outError)
				*outError = fmt::format("Reading failed: {}", exception.what());
			return false;
		}
	}

	bool AssetManagerBase::ReadAssetRange(const AssetMetadata& metadata, uint64_t offset, uint64_t size, std::vector<uint8_t>& outData, std::string* outError)
	{
		std::vector<uint8_t> data;
		if (!ReadAssetData(metadata, data, outError))
			return false;
		if (offset > data.size() || size > data.size() - offset)
		{
			if (outError)
				*outError = fmt::format("The range {}+{} lies outside the {} bytes of '{}'", offset, size, data.size(), metadata.Path.empty() ? metadata.Name : metadata.Path);
			return false;
		}
		outData.assign(data.begin() + static_cast<std::ptrdiff_t>(offset), data.begin() + static_cast<std::ptrdiff_t>(offset + size));
		return true;
	}

	////////////////////////////////////////////////////////////////////////////////
	// AssetGroupRequest
	////////////////////////////////////////////////////////////////////////////////

	void AssetGroupRequest::Refresh() const
	{
		if (m_FinishedCount == m_Assets.size())
			return;
		Ref<AssetManagerBase> manager = m_Manager.lock();
		if (!manager)
			return;
		for (size_t index = 0; index < m_Assets.size(); index++)
		{
			if (m_Finished[index])
				continue;
			const AssetState state = manager->GetAssetState(m_Assets[index]);
			// A member that was unregistered meanwhile (deleted, or its source removed) never loads: it failed. Only an
			// Unloaded state can mean that (unknown handles read as Unloaded).
			const bool removed = state == AssetState::Unloaded && !manager->IsHandleValid(m_Assets[index]);
			if (state != AssetState::Ready && state != AssetState::Failed && !removed)
				continue;
			m_Finished[index] = true;
			m_FinishedWeight += m_Weights[index];
			m_FinishedCount++;
			m_Failed = m_Failed || state == AssetState::Failed || removed;
		}
	}

	float AssetGroupRequest::Progress() const
	{
		if (m_Assets.empty())
			return 1.0f;
		Refresh();
		return m_FinishedCount == m_Assets.size() ? 1.0f : static_cast<float>(static_cast<double>(m_FinishedWeight) / static_cast<double>(m_TotalWeight));
	}

	bool AssetGroupRequest::IsComplete() const
	{
		Refresh();
		return m_FinishedCount == m_Assets.size();
	}

	bool AssetGroupRequest::HasFailed() const
	{
		Refresh();
		return m_Failed;
	}

	void AssetGroupRequest::Cancel()
	{
		m_Pins.clear(); // CancelLoad refuses pinned assets
		Ref<AssetManagerBase> manager = m_Manager.lock();
		if (!manager)
			return;
		Refresh();
		for (size_t index = 0; index < m_Assets.size(); index++)
		{
			if (!m_Finished[index])
				manager->CancelLoad(m_Assets[index]);
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// AssetManager
	////////////////////////////////////////////////////////////////////////////////

	static Ref<AssetManagerBase>& GetActiveManagerStorage()
	{
		static Ref<AssetManagerBase> s_Active;
		return s_Active;
	}

	void AssetManager::SetActive(const Ref<AssetManagerBase>& manager)
	{
		GetActiveManagerStorage() = manager;
	}

	const Ref<AssetManagerBase>& AssetManager::GetActive()
	{
		return GetActiveManagerStorage();
	}

	AssetState AssetManager::GetAssetState(AssetHandle handle)
	{
		const Ref<AssetManagerBase>& manager = GetActive();
		return manager ? manager->GetAssetState(handle) : AssetState::Unloaded;
	}

	AssetType AssetManager::GetAssetType(AssetHandle handle)
	{
		const Ref<AssetManagerBase>& manager = GetActive();
		return manager ? manager->GetAssetType(handle) : AssetType::None;
	}

	bool AssetManager::IsHandleValid(AssetHandle handle)
	{
		const Ref<AssetManagerBase>& manager = GetActive();
		return manager && manager->IsHandleValid(handle);
	}

}
