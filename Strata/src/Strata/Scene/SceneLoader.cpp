#include "stpch.h"
#include "Strata/Scene/SceneLoader.h"

#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"

#include <algorithm>

namespace Strata
{

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
		// The scene first (the root of the closure), then what it uses, breadth first; all of it pinned while the request lives.
		request.m_Group = manager->RequestLoadWithDependencies(scene, options.Priority);
		for (const AssetHandle member : request.m_Group.GetAssets())
		{
			if (const std::optional<AssetMetadata> metadata = manager->GetMetadata(member))
				request.m_TotalBytes += metadata->StoredSize;
		}
		request.m_State = SceneLoadState::LoadingScene;
		return request;
	}

	SceneLoadRequest::~SceneLoadRequest()
	{
		// A load nobody waits for anymore stops: its pins go, and what it started and nothing else needs is abandoned.
		if (!IsFinished())
			Cancel();
	}

	void SceneLoadRequest::Fail(std::string error)
	{
		m_State = SceneLoadState::Failed;
		m_Error = std::move(error);
		m_Prepared.reset();
	}

	float SceneLoadRequest::GetProgress() const
	{
		if (m_State == SceneLoadState::Ready)
			return 1.0f;
		if (!m_Group.IsValid())
			return 0.0f;
		return m_Group.Progress();
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
		m_Prepared.reset();
		if (!IsFinished())
			m_State = SceneLoadState::Cancelled;
	}

}
