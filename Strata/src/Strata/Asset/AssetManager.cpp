#include "stpch.h"
#include "Strata/Asset/AssetManager.h"

#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Renderer/Renderer.h"

#include <condition_variable>

namespace Strata
{

	// Defined in Asset/AssetRegistration.cpp: creates loaders for every built-in asset type.
	void CreateBuiltinAssetLoaders(std::unordered_map<AssetType, AssetLoadFunction>& loaders);

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
			std::unordered_map<AssetType, AssetLoadFunction> Loaders;
		};

		LoaderStorage& GetLoaderStorage()
		{
			static LoaderStorage s_Storage;
			return s_Storage;
		}

		// Built-ins are registered before any other registration, so later registrations replace them.
		void EnsureBuiltinLoaders()
		{
			static std::once_flag s_Once;
			std::call_once(s_Once, []()
			{
				std::unordered_map<AssetType, AssetLoadFunction> builtins;
				CreateBuiltinAssetLoaders(builtins);
				LoaderStorage& storage = GetLoaderStorage();
				std::scoped_lock<std::mutex> lock(storage.Mutex);
				for (auto& [type, loader] : builtins)
					storage.Loaders.emplace(type, std::move(loader));
			});
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

	}

	void AssetLoaderRegistry::Register(AssetType type, AssetLoadFunction loader)
	{
		EnsureBuiltinLoaders();
		LoaderStorage& storage = GetLoaderStorage();
		std::scoped_lock<std::mutex> lock(storage.Mutex);
		storage.Loaders[type] = std::move(loader);
	}

	const AssetLoadFunction* AssetLoaderRegistry::Find(AssetType type)
	{
		EnsureBuiltinLoaders();
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
			// A load that is still queued is raised to this priority.
			requestLoad = it->second.State == AssetState::Unloaded || it->second.State == AssetState::Loading;
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
		uint64_t generation = 0;
		uint64_t storedSize = 0;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_Entries.find(handle);
			if (it == m_Entries.end() || it->second.IsMemoryAsset)
				return;
			AssetEntry& entry = it->second;
			TouchLocked(entry);
			if (entry.State == AssetState::Unloaded)
			{
				entry.State = AssetState::Loading;
				entry.Error.clear();
				newRequest = true;
			}
			else if (entry.State != AssetState::Loading)
			{
				return; // Ready or failed
			}
			generation = entry.Generation;
			storedSize = entry.Metadata.StoredSize;
		}

		// The queue's lock is taken without the manager's: pumping dispatches, which takes the manager's lock.
		if (newRequest)
		{
			m_Queue.Enqueue(handle, priority, score, storedSize, generation);
			m_Queue.Pump();
		}
		else if (m_Queue.Raise(handle, priority, score))
		{
			m_Queue.Pump();
		}
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
			finishRead();
			if (!read)
			{
				completion.Error = error.empty() ? std::string("Failed to read asset data") : error;
				PushCompletion(loadState, std::move(completion));
				return;
			}
			if (!IsLoadCurrent(metadata.Handle, request.Generation))
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
						completion.LoadedAsset = (*loader)(metadata, *data, &completion.Error);
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
		if (!asset->FinalizeOnMainThread(AssetFinalizeContext { commandList, this }))
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
		while (!completions.empty())
		{
			// The first completion always proceeds, so an asset larger than the budget still loads.
			if (applyBudget && processed > 0 && (uploadedBytes >= budgets.UploadBytesPerFrame || elapsedMs() >= budgets.FinalizeMsPerFrame))
				break;

			Completion completion = std::move(completions.front());
			completions.pop_front();
			processed++;
			// The load leaves the flight once it is handled, whatever becomes of it; the queue may dispatch the next one.
			m_Queue.OnLoadFinished(completion.InFlightBytes);

			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				auto it = m_Entries.find(completion.Handle);
				if (it == m_Entries.end() || it->second.Generation != completion.Generation)
					continue; // Unregistered, unloaded, cancelled or reloaded since the request: discard
			}

			// Finalization runs without holding the lock: it may request dependent assets (e.g. a material's textures).
			if (completion.LoadedAsset && !completion.LoadedAsset->FinalizeOnMainThread(AssetFinalizeContext { commandList, this }))
			{
				completion.Error = "Failed to create GPU resources";
				completion.LoadedAsset = nullptr;
			}
			// Finalizing creates and fills the asset's GPU resources: what it holds on the GPU is what it uploaded.
			if (completion.LoadedAsset)
				uploadedBytes += completion.LoadedAsset->GetMemoryUsage().GetGpu();

			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_Entries.find(completion.Handle);
			if (it == m_Entries.end() || it->second.Generation != completion.Generation)
				continue;

			AssetEntry& entry = it->second;
			if (completion.LoadedAsset)
			{
				released.push_back(SetLoadedLocked(entry, completion.LoadedAsset));
				entry.State = AssetState::Ready;
				entry.Error.clear();
				PublishContentChange(completion.Handle);
			}
			else
			{
				entry.State = AssetState::Failed;
				entry.Error = completion.Error;
				ST_CORE_ERROR("Failed to load asset '{}' ({}): {}", entry.Metadata.Path.empty() ? entry.Metadata.Name : entry.Metadata.Path,
					entry.Metadata.Handle.ToString(), completion.Error);
			}
			m_TotalLoadsCompleted++;
		}

		if (commandList)
		{
			m_UploadCommandList->close();
			Renderer::GetDevice()->executeCommandList(m_UploadCommandList);
		}

		// Anything left over (budget exhausted) is finalized next frame, ahead of newer completions.
		if (!completions.empty())
		{
			std::scoped_lock<std::mutex> lock(m_LoadState->Mutex);
			while (!completions.empty())
			{
				m_LoadState->Completions.push_front(std::move(completions.back()));
				completions.pop_back();
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
			stats.Cancellations = m_Cancellations;
			stats.StagingReleases = m_StagingReleases;
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
