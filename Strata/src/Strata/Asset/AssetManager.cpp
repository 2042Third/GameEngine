#include "stpch.h"
#include "Strata/Asset/AssetManager.h"

#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Renderer/Renderer.h"

#include <algorithm>
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
		};

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

	////////////////////////////////////////////////////////////////////////////////
	// AssetManagerBase
	////////////////////////////////////////////////////////////////////////////////

	// Shared between the manager and its in-flight jobs, so a job finishing late never touches a destroyed manager.
	struct AssetManagerBase::LoadState
	{
		std::mutex Mutex;
		std::condition_variable Condition;
		std::deque<Completion> Completions;
		uint32_t InFlight = 0; // Load jobs running (each ends in exactly one PushCompletion)
	};

	AssetManagerBase::AssetManagerBase()
		: m_LoadState(CreateRef<LoadState>()), m_Queue([this](const AssetStreamingRequest& request) { return DispatchLoad(request); }),
		m_Budgets(GetDefaultResidencyBudgets())
	{
		// Frames the GPU may still be working on, plus two: an asset drawn that recently is still in use.
		const uint32_t framesInFlight = Renderer::IsInitialized() ? Renderer::GetGraphicsDevice().GetMaxFramesInFlight() : c_DefaultFramesInFlight;
		m_EvictionGraceFrames = framesInFlight + 2;
		m_GpuReleaseFrames = framesInFlight + 1;
		UpdateStreamingLimits();
		BuiltinAssets::Register(*this);
	}

	AssetManagerBase::~AssetManagerBase()
	{
		WaitForInFlightLoads();
	}

	AssetResidencyBudgets AssetManagerBase::GetDefaultResidencyBudgets()
	{
		if (!Renderer::IsInitialized())
			return AssetResidencyBudgets::FromDeviceBudget(0);
		return AssetResidencyBudgets::FromDeviceBudget(Renderer::GetGraphicsDevice().GetMemoryBudget().Budget);
	}

	void AssetManagerBase::WaitForInFlightLoads()
	{
		// Nothing new starts: a job finishing its read would otherwise dispatch the next queued load to this manager.
		m_Queue.Close();
		std::unique_lock<std::mutex> lock(m_LoadState->Mutex);
		m_LoadState->Condition.wait(lock, [this]() { return m_LoadState->InFlight == 0; });
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
		uint64_t storedSize = 0;
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
				entry.State = AssetState::Loading;
				entry.Error.clear();
				newRequest = true;
			}
			loading = entry.State == AssetState::Loading;
			prefetchDependencies = deferred && !newRequest && entry.State != AssetState::Failed;
			generation = entry.Generation;
			storedSize = entry.Metadata.StoredSize;
		}

		// The queue's lock is taken without the manager's: pumping dispatches, which takes the manager's lock.
		if (newRequest)
		{
			m_Queue.Enqueue(handle, priority, score, storedSize, generation);
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
		AssetMetadata metadata;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_Entries.find(request.Handle);
			if (it == m_Entries.end() || it->second.Generation != request.Generation || it->second.State != AssetState::Loading)
				return false; // Unloaded, cancelled or reloaded since it was queued
			metadata = it->second.Metadata;
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
			const bool read = ReadAssetData(metadata, *data, &error);
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

			JobSystem::Submit([loadState, metadata, data, completion]() mutable
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
						AssetLoadData loadData(*data);
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
		entry.State = entry.Loaded ? AssetState::Ready : AssetState::Unloaded;
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
			entry.State = AssetState::Unloaded;
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
			entry.State = AssetState::Unloaded;
			entry.Generation = m_NextGeneration++;
		}

		if (wasRequested)
			RequestLoad(handle, AssetPriority::High);
	}

	AssetHandle AssetManagerBase::AddMemoryAsset(const Ref<Asset>& asset, AssetMetadata metadata)
	{
		if (!metadata.Handle.IsValid())
			metadata.Handle = asset->Handle.IsValid() ? asset->Handle : UUID();
		metadata.Type = asset->GetType();
		asset->Handle = metadata.Handle;

		// Memory assets are finalized immediately (main thread), creating GPU resources when a renderer runs.
		nvrhi::CommandListHandle commandList;
		if (Renderer::IsInitialized())
		{
			commandList = Renderer::GetDevice()->createCommandList();
			commandList->open();
		}
		// Without an upload budget the asset finishes in this call.
		if (asset->FinalizeOnMainThread(AssetFinalizeContext { commandList, this }) != AssetFinalizeResult::Done)
			ST_CORE_WARN("Memory asset '{}' could not be finalized", metadata.Name);
		if (commandList)
		{
			commandList->close();
			Renderer::GetDevice()->executeCommandList(commandList);
		}

		Ref<Asset> released;
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto [it, inserted] = m_Entries.try_emplace(metadata.Handle);
		AssetEntry& entry = it->second;
		if (!inserted)
			AbandonLoadLocked(entry);
		entry.Metadata = metadata;
		entry.IsMemoryAsset = true; // Before the object is set: memory assets are resident but never evicted
		released = SetLoadedLocked(entry, asset);
		entry.State = AssetState::Ready;
		entry.Error.clear();
		entry.Generation = m_NextGeneration++;
		if (inserted)
			entry.Registration = entry.Generation;
		if (!metadata.Path.empty() && !metadata.IsSubAsset())
			m_PathIndex[metadata.Path] = metadata.Handle;
		PublishContentChange(metadata.Handle);
		return metadata.Handle;
	}

	void AssetManagerBase::RegisterAsset(const AssetMetadata& metadata)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto [it, inserted] = m_Entries.try_emplace(metadata.Handle);
		AssetEntry& entry = it->second;
		if (inserted)
		{
			entry.Generation = m_NextGeneration++;
			entry.Registration = entry.Generation;
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
		m_Entries.erase(it);
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

		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			m_FrameIndex++;
		}
		// The job system may have been started after this manager was created.
		UpdateStreamingLimits();
		m_Queue.Pump();
		ProcessCompletions(true);

		std::vector<Ref<Asset>> released;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			if (m_ScheduledTrim && m_FrameIndex >= m_ScheduledTrim->Frame)
			{
				const uint32_t unusedFrames = m_ScheduledTrim->UnusedFrames;
				m_ScheduledTrim.reset();
				EvictUnusedLocked(unusedFrames, released);
			}
		}
		released.clear(); // Destroyed outside the lock
		EvictToBudgets();
		EndFrameUploads();
	}

	size_t AssetManagerBase::ProcessCompletions(bool applyBudget)
	{
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

		nvrhi::ICommandList* commandList = nullptr;
		if (Renderer::IsInitialized())
		{
			if (!m_UploadCommandList)
				m_UploadCommandList = Renderer::GetDevice()->createCommandList();
			m_UploadCommandList->open();
			commandList = m_UploadCommandList;
		}

		size_t processed = 0;
		uint64_t uploadedBytes = 0;
		std::vector<Ref<Asset>> released;
		// Arrivals waiting until the GPU memory evicted for them is released, and those behind them (see below).
		std::deque<Completion> waiting;
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
					if (room.EvictedGpuBytes > 0 && incoming.GetGpu() > 0 && commandList)
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
				AssetFinalizeContext context { commandList, this };
				context.Prefetched = prefetched;
				if (applyBudget)
				{
					context.UploadBudget = budgets.UploadBytesPerFrame - std::min(uploadedBytes, budgets.UploadBytesPerFrame);
					context.Deadline = deadline;
					context.StagingBytes = budgets.StagingBytes;
				}
				context.UploadedBytes = &uploadedBytes;
				const AssetFinalizeResult result = completion.LoadedAsset->FinalizeOnMainThread(context);
				if (result == AssetFinalizeResult::Pending && applyBudget)
					break; // Stays in line
				if (result != AssetFinalizeResult::Done)
				{
					// Without a budget an asset has to finish: one that still asks for more is broken.
					completion.Error = result == AssetFinalizeResult::Pending ? "Finalization did not finish without an upload budget"
						: "Failed to create GPU resources";
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
				entry.State = AssetState::Ready;
				entry.Error.clear();
				PublishContentChange(handled.Handle);
			}
			else
			{
				entry.State = AssetState::Failed;
				entry.Error = handled.Error;
				ST_CORE_ERROR("Failed to load asset '{}' ({}): {}", entry.Metadata.Path.empty() ? entry.Metadata.Name : entry.Metadata.Path,
					entry.Metadata.Handle.ToString(), handled.Error);
			}
			m_TotalLoadsCompleted++;
		}

		if (commandList)
		{
			m_UploadCommandList->close();
			Renderer::GetDevice()->executeCommandList(m_UploadCommandList);
		}

		// What is left over (waiting for released memory, or the budget ran out) is finalized in a later frame, in its place
		// in line: ahead of newer completions.
		if (!completions.empty() || !waiting.empty())
		{
			std::scoped_lock<std::mutex> lock(m_LoadState->Mutex);
			for (std::deque<Completion>* leftOver : { &completions, &waiting })
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
			m_UploadedBytesHistory[m_HistoryCursor] = m_FrameUploadedBytes;
			m_FinalizeMsHistory[m_HistoryCursor] = m_FrameFinalizeMs;
			m_HistoryCursor = (m_HistoryCursor + 1) % c_StatsWindowFrames;
			m_FrameUploadedBytes = 0;
			m_FrameFinalizeMs = 0.0f;
		}

		if (uploadedBytes > 0 || !m_UploadCommandList)
		{
			m_FramesWithoutUploads = 0;
			return;
		}
		if (++m_FramesWithoutUploads < c_StagingReleaseFrames)
			return;

		// NVRHI's UploadManager pools the staging chunks of each command list with no memory limit (the Vulkan command list
		// creates it with a limit of 0, Strata/vendor/NVRHI src/vulkan/vulkan-commandlist.cpp), so after a burst of uploads
		// the staging memory stays at its high-water mark for as long as the command list lives. Recreating the command
		// list is the supported way to return it: submissions still in flight keep the old one (and its chunks) alive
		// until the GPU has finished them.
		m_UploadCommandList = nullptr;
		m_FramesWithoutUploads = 0;
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
		return m_Queue.HasWork();
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

	AssetManagerStats AssetManagerBase::GetStats() const
	{
		AssetManagerStats stats;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			stats.RegisteredAssets = static_cast<uint32_t>(m_Entries.size());
			for (const auto& [handle, entry] : m_Entries)
			{
				switch (entry.State)
				{
					case AssetState::Ready:   stats.LoadedAssets++; break;
					case AssetState::Loading: stats.LoadingAssets++; break;
					case AssetState::Failed:  stats.FailedAssets++; break;
					default: break;
				}
				if (entry.PinCount > 0)
					stats.PinnedAssets++;
			}
			stats.TotalLoadsCompleted = m_TotalLoadsCompleted;
			stats.Frame = m_FrameIndex;
			stats.Resident = m_Resident;
			stats.LoadedMemory = m_Resident.GetTotal();
			stats.Budgets = m_Budgets;
			stats.UploadedBytesLastFrame = m_LastFrameUploadedBytes;
			stats.FinalizeMsLastFrame = m_LastFrameFinalizeMs;
			for (size_t index = 0; index < c_StatsWindowFrames; index++)
			{
				stats.UploadedBytesWindowMax = std::max(stats.UploadedBytesWindowMax, m_UploadedBytesHistory[index]);
				stats.FinalizeMsWindowMax = std::max(stats.FinalizeMsWindowMax, m_FinalizeMsHistory[index]);
			}
			stats.Evictions = m_Evictions;
			stats.EvictionChecks = m_EvictionChecks;
			stats.Cancellations = m_Cancellations;
			stats.StagingReleases = m_StagingReleases;
			stats.DependencyPrefetches = m_DependencyPrefetches;
			stats.DependencyPrefetchesDropped = m_DependencyPrefetchesDropped;
		}

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
		if (bounds && !bounds->IsValid())
			bounds.reset();

		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		if (it == m_Entries.end())
			return;
		it->second.Dependencies = std::move(unique);
		it->second.Bounds = bounds;
	}

	std::vector<AssetHandle> AssetManagerBase::GetDependencies(AssetHandle handle) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		return it != m_Entries.end() ? it->second.Dependencies : std::vector<AssetHandle>();
	}

	std::vector<AssetHandle> AssetManagerBase::GetDependencyClosureLocked(AssetHandle handle) const
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
				if (m_Entries.find(dependency) != m_Entries.end() && visited.insert(dependency).second)
					closure.push_back(dependency);
			}
		}
		return closure;
	}

	std::vector<AssetHandle> AssetManagerBase::GetDependencyClosure(AssetHandle handle) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		return GetDependencyClosureLocked(handle);
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
		const std::vector<AssetHandle> closure = GetDependencyClosureLocked(handle);
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
			uint64_t StoredSize = 0;
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
				AssetEntry& entry = dependencyIt->second;
				// Not touched: a prefetch is no use, so it keeps nothing resident and makes no room (see GetDependencies).
				if (entry.State == AssetState::Unloaded)
				{
					const uint64_t bytes = entry.Metadata.StoredSize > 0 ? entry.Metadata.StoredSize : AssetStreamingQueue::c_UnknownSizeBytes;
					if (!FitsPrefetchLocked(bytes, pendingBytes))
						continue;
					pendingBytes += bytes;
					entry.State = AssetState::Loading;
					entry.Error.clear();
					entry.PrefetchOnly = true;
					entry.DependenciesDeferred = false; // Its read decides again
					started.push_back(Request { dependency, entry.Generation, entry.Metadata.StoredSize });
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
			m_Queue.Enqueue(request.Handle, priority, c_DependencyPrefetchScore, request.StoredSize, request.Generation);
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

		entry.State = AssetState::Unloaded; // Prefetches start from Unloaded: there is no previous version to keep
		entry.Generation = m_NextGeneration++;
		m_DependencyPrefetchesDropped++;
		return true;
	}

	AssetGroupRequest AssetManagerBase::RequestLoadWithDependencies(AssetHandle handle, AssetPriority priority)
	{
		AssetGroupRequest group;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			group.m_Assets = GetDependencyClosureLocked(handle);
			for (AssetHandle member : group.m_Assets)
			{
				const uint64_t weight = std::max<uint64_t>(m_Entries.at(member).Metadata.StoredSize, 1);
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
			if (state != AssetState::Ready && state != AssetState::Failed)
				continue;
			m_Finished[index] = true;
			m_FinishedWeight += m_Weights[index];
			m_FinishedCount++;
			m_Failed = m_Failed || state == AssetState::Failed;
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
