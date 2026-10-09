#include "stpch.h"
#include "Strata/Runtime/GameRuntime.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Scene/Prefab.h"

namespace Strata
{

	Scope<GameRuntime> GameRuntime::Create(const std::filesystem::path& manifestPath, std::string* outError)
	{
		std::optional<GameManifest> manifest = GameManifest::Load(manifestPath, outError);
		if (!manifest)
			return nullptr;

		Scope<GameRuntime> runtime(new GameRuntime());
		runtime->m_Manifest = std::move(*manifest);
		const std::filesystem::path packPath = manifestPath.parent_path() / FileSystem::FromUTF8(runtime->m_Manifest.AssetPack);
		runtime->m_AssetManager = RuntimeAssetManager::Create(packPath, outError);
		if (!runtime->m_AssetManager)
			return nullptr;
		AssetManager::SetActive(runtime->m_AssetManager);

		if (!runtime->LoadScene(runtime->m_Manifest.StartScene, outError))
			return nullptr; // The destructor deactivates the asset manager
		ST_CORE_INFO("Started '{}' ({} assets)", runtime->m_Manifest.Name, runtime->m_AssetManager->GetAllMetadata().size());
		return runtime;
	}

	GameRuntime::~GameRuntime()
	{
		if (m_Scene && m_Scene->IsRunning())
			m_Scene->OnRuntimeStop();
		m_Scene.reset();
		if (m_AssetManager && AssetManager::GetActive() == m_AssetManager)
			AssetManager::SetActive(nullptr);
	}

	bool GameRuntime::LoadScene(AssetHandle scene, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		if (m_AssetManager->GetAssetType(scene) != AssetType::Scene)
			return fail(fmt::format("{} is not a scene of the game", scene.ToString()));
		Ref<SceneAsset> asset = AssetManager::LoadAssetSync<SceneAsset>(scene);
		if (!asset)
			return fail(fmt::format("Loading scene {} failed: {}", scene.ToString(), m_AssetManager->GetAssetError(scene)));
		std::string error;
		Ref<Scene> loaded = asset->CreateScene(&error);
		if (!loaded)
			return fail(fmt::format("Scene {} is invalid: {}", scene.ToString(), error));

		if (m_Scene && m_Scene->IsRunning())
			m_Scene->OnRuntimeStop();
		m_Scene = std::move(loaded);
		m_SceneHandle = scene;
		m_Scene->OnRuntimeStart(SceneRuntimeMode::Play);
		return true;
	}

	void GameRuntime::Update(Timestep timestep)
	{
		if (m_QuitRequest)
			return;
		m_AssetManager->Update();
		m_Scene->OnUpdateRuntime(timestep);
		HandleSceneRequests();
	}

	void GameRuntime::HandleSceneRequests()
	{
		if (const std::optional<int32_t> exitCode = m_Scene->GetQuitRequest())
		{
			m_QuitRequest = exitCode;
			ST_CORE_INFO("'{}' quit with exit code {}", m_Manifest.Name, *exitCode);
			return;
		}

		const std::optional<UUID> request = m_Scene->TakeSceneLoadRequest();
		if (!request)
			return;
		const AssetHandle scene = request->IsValid() ? *request : m_SceneHandle;
		std::string error;
		if (!LoadScene(scene, &error))
			ST_CORE_ERROR("'{}' cannot switch scenes: {}", m_Manifest.Name, error);
	}

}
