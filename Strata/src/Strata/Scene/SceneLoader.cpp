#include "stpch.h"
#include "Strata/Scene/SceneLoader.h"

#include "Strata/Asset/AssetResidency.h"
#include "Strata/Asset/AssetStreamingQueue.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneReadProgress.h"

#include <algorithm>
#include <array>
#include <optional>
#include <unordered_set>
#include <vector>

namespace Strata
{

	namespace
	{

		// What an asset of `type` whose stored form takes `storedSize` bytes will hold once loaded, before it ever was: about
		// its stored size, in the pool its type lives in (cooked textures and meshes are uploaded as they are stored; meshes
		// keep their geometry for physics too). Everything else is system memory.
		AssetMemoryUsage EstimateMemoryUsage(AssetType type, uint64_t storedSize)
		{
			const uint64_t bytes = storedSize > 0 ? storedSize : AssetStreamingQueue::c_UnknownSizeBytes;
			AssetMemoryUsage usage;
			switch (type)
			{
				case AssetType::Texture:
					usage.GpuTextures = bytes;
					break;
				case AssetType::Mesh:
					usage.GpuBuffers = bytes;
					usage.Cpu = bytes;
					break;
				default:
					usage.Cpu = bytes;
					break;
			}
			return usage;
		}

		// The members of a load of `scene` (see SceneLoadRequest), in the order of its closure, the scene asset first. A pool
		// whose part of the closure does not all fit in the room its budget has beside what is resident is left to the
		// scene, which requests what it draws once it runs: the load cannot know which part that is, and any other part would
		// take the room from it. So is a member that depends directly on something left out, as requesting it would prefetch
		// that into the same room (prefetches do not chain, so one level is enough). Members loaded already hold their memory
		// and always count. The others are counted in outSkipped.
		std::vector<AssetHandle> SelectLoadMembers(const AssetManagerBase& manager, AssetHandle scene, size_t& outSkipped)
		{
			struct Candidate
			{
				AssetHandle Handle;
				AssetMemoryUsage Usage; // Estimated, for members not loaded yet
				bool Loaded = false;
			};

			const std::vector<AssetHandle> closure = manager.GetDependencyClosure(scene, AssetClosureScope::Load);
			std::vector<Candidate> candidates;
			candidates.reserve(closure.size());
			std::array<uint64_t, c_AssetMemoryPools.size()> needed = {};
			for (const AssetHandle member : closure)
			{
				Candidate candidate { member, {}, member == scene || manager.GetAssetState(member) == AssetState::Ready };
				if (!candidate.Loaded)
				{
					const std::optional<AssetMetadata> metadata = manager.GetMetadata(member);
					if (!metadata)
						continue; // Unregistered since the closure was read
					candidate.Usage = EstimateMemoryUsage(metadata->Type, metadata->StoredSize);
					for (size_t pool = 0; pool < c_AssetMemoryPools.size(); pool++)
						needed[pool] += GetPoolBytes(candidate.Usage, c_AssetMemoryPools[pool]);
				}
				candidates.push_back(candidate);
			}

			const AssetManagerStats stats = manager.GetStats();
			std::array<bool, c_AssetMemoryPools.size()> fits = {};
			for (size_t pool = 0; pool < c_AssetMemoryPools.size(); pool++)
			{
				const uint64_t budget = stats.Budgets.GetPoolBudget(c_AssetMemoryPools[pool]);
				const uint64_t resident = GetPoolBytes(stats.Resident, c_AssetMemoryPools[pool]);
				fits[pool] = budget == AssetResidencyBudgets::c_Unlimited || needed[pool] <= budget - std::min(budget, resident);
			}
			std::unordered_set<AssetHandle> leftOut;
			for (const Candidate& candidate : candidates)
			{
				for (size_t pool = 0; pool < c_AssetMemoryPools.size() && !candidate.Loaded; pool++)
				{
					if (!fits[pool] && GetPoolBytes(candidate.Usage, c_AssetMemoryPools[pool]) > 0)
					{
						leftOut.insert(candidate.Handle);
						break;
					}
				}
			}

			std::vector<AssetHandle> members;
			members.reserve(candidates.size());
			outSkipped = 0;
			for (const Candidate& candidate : candidates)
			{
				bool skip = leftOut.contains(candidate.Handle);
				if (!skip && !candidate.Loaded && !leftOut.empty())
				{
					const std::vector<AssetHandle> dependencies = manager.GetDependencies(candidate.Handle);
					skip = std::any_of(dependencies.begin(), dependencies.end(), [&leftOut](AssetHandle dependency) { return leftOut.contains(dependency); });
				}
				if (skip)
					outSkipped++;
				else
					members.push_back(candidate.Handle);
			}
			return members;
		}

	}

	const char* SceneLoadStateToString(SceneLoadState state)
	{
		switch (state)
		{
			case SceneLoadState::LoadingScene:        return "LoadingScene";
			case SceneLoadState::LoadingDependencies: return "LoadingDependencies";
			case SceneLoadState::Ready:               return "Ready";
			case SceneLoadState::Failed:              return "Failed";
			case SceneLoadState::Cancelled:           return "Cancelled";
		}
		return "Unknown";
	}

	SceneLoadRequest SceneLoader::LoadAsync(AssetHandle scene, const SceneLoadOptions& options)
	{
		SceneLoadRequest request;
		request.m_Scene = scene;
		request.m_Options = options;
		request.m_Options.RequiredProgress = std::clamp(options.RequiredProgress, 0.0f, 1.0f);
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		if (!manager)
		{
			request.Fail("No asset manager is active");
			return request;
		}
		if (manager->GetAssetType(scene) != AssetType::Scene)
		{
			request.Fail(fmt::format("{} is not a scene asset", scene.ToString()));
			return request;
		}

		request.m_Manager = manager;
		if (options.Reload)
			manager->UnloadAsset(scene);
		// The scene first (the root of the closure), then what it uses, breadth first, as far as the budgets have room; all of
		// it pinned while the request lives.
		request.m_Group = manager->RequestLoadGroup(SelectLoadMembers(*manager, scene, request.m_SkippedAssets), options.Priority);
		if (request.m_SkippedAssets > 0)
		{
			ST_CORE_TRACE("Loading scene {}: {} of the assets it uses are left to the scene (the residency budgets have no room for them now)",
				scene.ToString(), request.m_SkippedAssets);
		}
		uint64_t sceneWeight = 0;
		uint64_t totalWeight = 0;
		for (const AssetHandle member : request.m_Group.GetAssets())
		{
			const std::optional<AssetMetadata> metadata = manager->GetMetadata(member);
			const uint64_t storedSize = metadata ? metadata->StoredSize : 0;
			request.m_TotalBytes += storedSize;
			const uint64_t weight = std::max<uint64_t>(storedSize, 1);
			totalWeight += weight;
			if (member == scene)
				sceneWeight = weight;
		}
		request.m_SceneShare = totalWeight > 0 ? static_cast<float>(static_cast<double>(sceneWeight) / static_cast<double>(totalWeight)) : 0.0f;
		request.m_State = SceneLoadState::LoadingScene;
		return request;
	}

	SceneLoadRequest::~SceneLoadRequest()
	{
		// A load nobody waits for anymore stops: its pins go, and what it started and nothing else needs is abandoned.
		if (!IsFinished())
			Cancel();
		// A scene read but never taken goes like a replaced one, off the frame.
		Scene::ReleaseInBackground(std::move(m_Prepared));
	}

	void SceneLoadRequest::Fail(std::string error)
	{
		m_State = SceneLoadState::Failed;
		m_Error = std::move(error);
		Scene::ReleaseInBackground(std::move(m_Prepared));
	}

	float SceneLoadRequest::GetProgress() const
	{
		if (m_State == SceneLoadState::Ready)
			return 1.0f;
		// Nothing is loading for a request that failed at once or was cancelled.
		if (!m_Group.IsValid())
			return 0.0f;
		float progress = m_Group.Progress();
		// The group counts the scene asset once it is loaded; until then the read of its document counts for its share.
		if (m_State == SceneLoadState::LoadingScene)
		{
			if (const std::optional<float> read = SceneReadProgress::Get(m_Scene))
				progress += m_SceneShare * *read;
		}
		m_ReportedProgress = std::max(m_ReportedProgress, std::min(progress, 1.0f));
		return m_ReportedProgress;
	}

	void SceneLoadRequest::Update()
	{
		if (IsFinished())
			return;
		const Ref<AssetManagerBase> manager = m_Manager.lock();
		if (!manager)
		{
			Fail("The asset manager was closed");
			m_Group = AssetGroupRequest();
			return;
		}

		if (m_State == SceneLoadState::LoadingScene)
		{
			switch (manager->GetAssetState(m_Scene))
			{
				case AssetState::Failed:
					Fail(fmt::format("Loading scene {} failed: {}", m_Scene.ToString(), manager->GetAssetError(m_Scene)));
					return;
				case AssetState::Unloaded:
					// Unregistered meanwhile (deleted, or its file removed): nothing will ever be read.
					if (manager->GetAssetType(m_Scene) != AssetType::Scene)
					{
						Fail(fmt::format("Scene {} is no longer an asset: it was deleted or removed while it loaded", m_Scene.ToString()));
						return;
					}
					// Unloaded meanwhile (another load took and released it, or an editor reloads it): read it again.
					manager->RequestLoad(m_Scene, m_Options.Priority);
					return;
				case AssetState::Loading:
					return;
				case AssetState::Ready:
					break;
			}
			// The group counts the scene asset as loaded before it is unloaded below (its progress never decreases).
			m_Group.Progress();
			const Ref<Asset> loaded = manager->PeekLoadedAsset(m_Scene);
			if (!loaded || loaded->GetType() != AssetType::Scene)
			{
				Fail(fmt::format("{} is not a scene asset", m_Scene.ToString()));
				return;
			}
			const Ref<SceneAsset> asset = std::static_pointer_cast<SceneAsset>(loaded);
			if (asset->IsTaken())
			{
				// Another load took this copy's scene: this one reads the file again.
				manager->UnloadAsset(m_Scene);
				manager->RequestLoad(m_Scene, m_Options.Priority);
				return;
			}
			std::string error;
			m_Prepared = asset->TakePreparedScene(&error);
			// The asset is spent; its entry holds nothing until the scene is loaded again.
			manager->UnloadAsset(m_Scene);
			if (!m_Prepared)
			{
				Fail(fmt::format("Scene {} is invalid: {}", m_Scene.ToString(), error));
				return;
			}
			m_State = SceneLoadState::LoadingDependencies;
		}

		if (m_State == SceneLoadState::LoadingDependencies && m_Group.Progress() >= m_Options.RequiredProgress)
			m_State = SceneLoadState::Ready;
	}

	Ref<Scene> SceneLoadRequest::TakeScene()
	{
		if (m_State != SceneLoadState::Ready || m_Taken)
			return nullptr;
		m_Taken = true;
		return std::move(m_Prepared);
	}

	void SceneLoadRequest::Cancel()
	{
		m_Group.Cancel();
		m_Group = AssetGroupRequest();
		Scene::ReleaseInBackground(std::move(m_Prepared));
		if (!IsFinished())
			m_State = SceneLoadState::Cancelled;
	}

}
