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

		// Scenes use the script engine that is active when they start; a game without scripts runs none, whatever the
		// host had active.
		runtime->m_PreviousScriptEngine = ScriptEngine::GetActive();
		if (!runtime->m_Manifest.ScriptModule.empty())
		{
			runtime->m_ScriptEngine = CreateRef<ScriptEngine>();
			const std::filesystem::path modulePath = manifestPath.parent_path() / FileSystem::FromUTF8(runtime->m_Manifest.ScriptModule);
			std::string error;
			if (!runtime->m_ScriptEngine->LoadModule(modulePath, &error))
			{
				if (outError)
					*outError = fmt::format("Loading the game's script module failed: {}", error);
				return nullptr; // The destructor deactivates the asset manager
			}
		}
		ScriptEngine::SetActive(runtime->m_ScriptEngine);

		if (!runtime->LoadScene(runtime->m_Manifest.StartScene, outError))
			return nullptr; // The destructor deactivates the asset manager and the script engine
		runtime->CheckScriptFault();
		ST_CORE_INFO("Started '{}' ({} assets{})", runtime->m_Manifest.Name, runtime->m_AssetManager->GetAllMetadata().size(),
			runtime->m_ScriptEngine ? fmt::format(", {} script classes", runtime->m_ScriptEngine->GetClasses().size()) : std::string());
		return runtime;
	}

	GameRuntime::~GameRuntime()
	{
		if (m_Scene && m_Scene->IsRunning())
			m_Scene->OnRuntimeStop();
		m_Scene.reset();
		if (ScriptEngine::GetActive() == m_ScriptEngine)
			ScriptEngine::SetActive(m_PreviousScriptEngine);
		m_ScriptEngine.reset();
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
		m_AssetManager->Update();
		m_Scene->OnUpdateRuntime(timestep);
		CheckScriptFault();
	}

	void GameRuntime::CheckScriptFault()
	{
		if (m_ScriptFault || !m_ScriptEngine)
			return;
		m_ScriptFault = m_ScriptEngine->GetFault();
		if (m_ScriptFault)
			ST_CORE_ERROR("The scripts of '{}' crashed and stay disabled for the rest of the session: {}", m_Manifest.Name, m_ScriptFault->Description);
	}

}
