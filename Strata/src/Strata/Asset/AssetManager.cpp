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
		uint32_t InFlight = 0;
	};

	AssetManagerBase::AssetManagerBase()
		: m_LoadState(CreateRef<LoadState>())
	{
		BuiltinAssets::Register(*this);
	}

	AssetManagerBase::~AssetManagerBase()
	{
		WaitForInFlightLoads();
	}

	void AssetManagerBase::WaitForInFlightLoads()
	{
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
			current = it->second.Loaded; // Also the previous version while a reload is in progress
			requestLoad = it->second.State == AssetState::Unloaded;
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

	void AssetManagerBase::RequestLoad(AssetHandle handle, AssetPriority priority)
	{
		AssetMetadata metadata;
		uint64_t generation = 0;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_Entries.find(handle);
			if (it == m_Entries.end() || it->second.State != AssetState::Unloaded || it->second.IsMemoryAsset)
				return;

			it->second.State = AssetState::Loading;
			it->second.Error.clear();
			metadata = it->second.Metadata;
			generation = it->second.Generation;
		}

		{
			std::scoped_lock<std::mutex> lock(m_LoadState->Mutex);
			m_LoadState->InFlight++;
		}

		// Every path through the jobs ends in exactly one PushCompletion, also when code inside throws (bad_alloc on
		// huge assets, third-party code), so in-flight counts always drain. Jobs are submitted outside the locks:
		// without an initialized job system they run inline right here.
		Ref<LoadState> loadState = m_LoadState;
		const JobPriority jobPriority = ToJobPriority(priority);
		auto decode = [loadState, metadata, generation](const Ref<std::vector<uint8_t>>& data)
		{
			Completion completion { metadata.Handle, generation, nullptr, {} };
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
			PushCompletion(loadState, std::move(completion));
		};

		auto read = [this, loadState, metadata, generation, jobPriority, decode]()
		{
			std::string error;
			try
			{
				auto data = CreateRef<std::vector<uint8_t>>();
				if (!ReadAssetData(metadata, *data, &error))
				{
					PushCompletion(loadState, Completion { metadata.Handle, generation, nullptr, error.empty() ? std::string("Failed to read asset data") : error });
					return;
				}
				JobSystem::Submit([decode, data]() { decode(data); }, jobPriority);
				return;
			}
			catch (const std::exception& exception)
			{
				error = fmt::format("Reading failed: {}", exception.what());
			}
			catch (...)
			{
				error = "Reading failed with an unknown exception";
			}
			PushCompletion(loadState, Completion { metadata.Handle, generation, nullptr, error });
		};

		try
		{
			JobSystem::SubmitIO(read, jobPriority);
		}
		catch (const std::exception& exception)
		{
			PushCompletion(loadState, Completion { metadata.Handle, generation, nullptr, fmt::format("Could not queue the load: {}", exception.what()) });
		}
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

	void AssetManagerBase::UnloadAsset(AssetHandle handle)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		if (it == m_Entries.end() || it->second.IsMemoryAsset)
			return;

		if (it->second.Loaded)
			PublishContentChange(handle);
		it->second.Loaded = nullptr;
		it->second.State = AssetState::Unloaded;
		it->second.Error.clear();
		it->second.Generation = m_NextGeneration++;
	}

	void AssetManagerBase::ReloadAsset(AssetHandle handle)
	{
		bool wasRequested = false;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_Entries.find(handle);
			if (it == m_Entries.end() || it->second.IsMemoryAsset)
				return;

			wasRequested = it->second.State != AssetState::Unloaded;
			// Keep serving the old object until the new one is ready, so users never see the asset disappear.
			it->second.State = AssetState::Unloaded;
			it->second.Generation = m_NextGeneration++;
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

		std::scoped_lock<std::mutex> lock(m_Mutex);
		AssetEntry& entry = m_Entries[metadata.Handle];
		entry.Metadata = metadata;
		entry.Loaded = asset;
		entry.State = AssetState::Ready;
		entry.IsMemoryAsset = true;
		entry.Generation = m_NextGeneration++;
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
			entry.Generation = m_NextGeneration++;
		else if (entry.Metadata.Path != metadata.Path && !entry.Metadata.IsSubAsset())
			m_PathIndex.erase(entry.Metadata.Path);
		entry.Metadata = metadata;
		if (!metadata.Path.empty() && !metadata.IsSubAsset())
			m_PathIndex[metadata.Path] = metadata.Handle;
	}

	void AssetManagerBase::UnregisterAsset(AssetHandle handle)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		if (it == m_Entries.end())
			return;

		auto pathIt = m_PathIndex.find(it->second.Metadata.Path);
		if (pathIt != m_PathIndex.end() && pathIt->second == handle)
			m_PathIndex.erase(pathIt);
		if (it->second.Loaded)
			PublishContentChange(handle);
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

	void AssetManagerBase::Update()
	{
		ST_PROFILE_FUNCTION();
		ProcessCompletions(true);
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
		const uint64_t uploadBudget = m_UploadBudget.load();

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
		while (!completions.empty())
		{
			if (applyBudget && uploadedBytes >= uploadBudget)
				break;

			Completion completion = std::move(completions.front());
			completions.pop_front();
			processed++;

			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				auto it = m_Entries.find(completion.Handle);
				if (it == m_Entries.end() || it->second.Generation != completion.Generation)
					continue; // Unregistered, unloaded or reloaded since the request: discard
			}

			// Finalization runs without holding the lock: it may request dependent assets (e.g. a material's textures).
			if (completion.LoadedAsset && !completion.LoadedAsset->FinalizeOnMainThread(AssetFinalizeContext { commandList, this }))
			{
				completion.Error = "Failed to create GPU resources";
				completion.LoadedAsset = nullptr;
			}
			if (completion.LoadedAsset)
				uploadedBytes += completion.LoadedAsset->GetMemoryUsage();

			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_Entries.find(completion.Handle);
			if (it == m_Entries.end() || it->second.Generation != completion.Generation)
				continue;

			AssetEntry& entry = it->second;
			if (completion.LoadedAsset)
			{
				entry.Loaded = completion.LoadedAsset;
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
		return processed;
	}

	bool AssetManagerBase::HasPendingLoads() const
	{
		std::scoped_lock<std::mutex> lock(m_LoadState->Mutex);
		return m_LoadState->InFlight > 0 || !m_LoadState->Completions.empty();
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
		std::scoped_lock<std::mutex> lock(m_Mutex);
		stats.RegisteredAssets = static_cast<uint32_t>(m_Entries.size());
		for (const auto& [handle, entry] : m_Entries)
		{
			switch (entry.State)
			{
				case AssetState::Ready:
					stats.LoadedAssets++;
					stats.LoadedMemory += entry.Loaded ? entry.Loaded->GetMemoryUsage() : 0;
					break;
				case AssetState::Loading: stats.LoadingAssets++; break;
				case AssetState::Failed:  stats.FailedAssets++; break;
				default: break;
			}
		}
		stats.TotalLoadsCompleted = m_TotalLoadsCompleted;
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
