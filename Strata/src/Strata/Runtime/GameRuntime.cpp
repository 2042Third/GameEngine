#include "stpch.h"
#include "Strata/Runtime/GameRuntime.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Profiling.h"
#include "Strata/Renderer/RenderScene.h"

#include <algorithm>
#include <utility>

namespace Strata
{

	namespace
	{

		std::string DescribeBudget(uint64_t bytes)
		{
			return bytes == AssetResidencyBudgets::c_Unlimited ? std::string("unlimited") : fmt::format("{} MB", bytes >> 20);
		}

	}

	Scope<GameRuntime> GameRuntime::Create(const std::filesystem::path& manifestPath, std::string* outError, const GameRuntimeOptions& options)
	{
		std::optional<GameManifest> manifest = GameManifest::Load(manifestPath, outError);
		if (!manifest)
			return nullptr;

		Scope<GameRuntime> runtime(new GameRuntime());
		runtime->m_Opened = std::chrono::steady_clock::now();
		runtime->m_Manifest = std::move(*manifest);
		const std::filesystem::path packPath = manifestPath.parent_path() / FileSystem::FromUTF8(runtime->m_Manifest.AssetPack);
		runtime->m_AssetManager = RuntimeAssetManager::Create(packPath, outError);
		if (!runtime->m_AssetManager)
			return nullptr;
		// Patches replace the game's assets before anything loads; one that is missing or broken stops the game, which
		// would otherwise run with assets the patch was meant to replace.
		for (const std::string& patch : runtime->m_Manifest.PatchPacks)
		{
			std::string error;
			if (!runtime->m_AssetManager->Mount(manifestPath.parent_path() / FileSystem::FromUTF8(patch), &error))
			{
				if (outError)
					*outError = fmt::format("Mounting the patch pack '{}' failed: {}", patch, error);
				return nullptr;
			}
		}
		if (options.AssetBudgets)
			runtime->m_AssetManager->SetResidencyBudgets(*options.AssetBudgets);
		runtime->m_TimeDilation = options.TimeDilation;
		if (options.PrepareRenderScenes)
			runtime->m_LoadOptions.Prepare = &RenderScene::PrepareLoadedScene;
		const AssetResidencyBudgets budgets = runtime->m_AssetManager->GetResidencyBudgets();
		ST_CORE_INFO("Asset budgets: GPU textures {}, GPU buffers {}, CPU {}", DescribeBudget(budgets.GpuTextures), DescribeBudget(budgets.GpuBuffers),
			DescribeBudget(budgets.Cpu));
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
		// The game's save files, from its first script call on (OnCreate may load a save).
		runtime->m_Saves = options.SaveFilesDirectory.empty() ? SaveDirectory::ForGame(runtime->m_Manifest.Name) : CreateScope<SaveDirectory>(options.SaveFilesDirectory);
		runtime->m_SavesProvider = CreateScope<EngineServices::ScopedProvider<IGameSaves>>(*runtime->m_Saves);

		// The loading scene first, so that it plays while the start scene loads (both load in the background).
		const AssetHandle loadingScene = runtime->m_Manifest.LoadingScene;
		if (loadingScene.IsValid())
		{
			if (runtime->m_AssetManager->GetAssetType(loadingScene) != AssetType::Scene)
			{
				if (outError)
					*outError = fmt::format("The loading scene {} is not a scene of the game", loadingScene.ToString());
				return nullptr; // The destructor deactivates the asset manager and the script engine
			}
			runtime->m_LoadingSceneLoad = CreateScope<PendingScene>();
			runtime->m_LoadingSceneLoad->Scene = loadingScene;
			runtime->m_LoadingSceneLoad->Request = SceneLoader::LoadAsync(loadingScene, runtime->m_LoadOptions);
		}
		if (!runtime->LoadScene(runtime->m_Manifest.StartScene, outError))
			return nullptr;
		std::string scripts;
		if (runtime->m_ScriptEngine)
		{
			const size_t classes = runtime->m_ScriptEngine->GetClasses().size();
			scripts = fmt::format(", {} script {}", classes, classes == 1 ? "class" : "classes");
		}
		const size_t assets = runtime->m_AssetManager->GetAllMetadata().size();
		ST_CORE_INFO("Opened '{}' ({} {}{}); its start scene is loading", runtime->m_Manifest.Name, assets, assets == 1 ? "asset" : "assets", scripts);
		return runtime;
	}

	GameRuntime::~GameRuntime()
	{
		// Loads under way stop before the asset manager goes.
		m_SceneLoad.reset();
		m_LoadingSceneLoad.reset();
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
		if (m_AssetManager->GetAssetType(scene) != AssetType::Scene)
		{
			if (outError)
				*outError = fmt::format("{} is not a scene of the game", scene.ToString());
			return false;
		}
		// Asking again for the scene that is loading keeps its load: scripts ask in every update while their condition holds
		// (the running scene goes on meanwhile), and starting over would read it anew each time and never switch.
		if (m_SceneLoad && m_SceneLoad->Scene == scene)
			return true;
		// A request for another scene replaces the one under way: its pins and the loads only it needed go. The new load
		// starts first, so that what both use keeps loading (Cancel leaves assets others pin alone).
		Scope<PendingScene> next = CreateScope<PendingScene>();
		next->Scene = scene;
		next->Request = SceneLoader::LoadAsync(scene, m_LoadOptions);
		m_SceneLoad = std::move(next);
		if (m_Scene)
			m_Scene->SetSceneLoadProgress(0.0f);
		return true;
	}

	void GameRuntime::RestartLoadingScene()
	{
		if (m_LoadingSceneLoad && m_LoadingSceneLoad->Scene == m_SceneHandle)
			return;
		Scope<PendingScene> next = CreateScope<PendingScene>();
		next->Scene = m_SceneHandle;
		next->Request = SceneLoader::LoadAsync(m_SceneHandle, m_LoadOptions);
		m_LoadingSceneLoad = std::move(next);
	}

	void GameRuntime::StartScene(Ref<Scene> scene, AssetHandle handle, std::span<const AssetHandle> loaded)
	{
		// The scene that goes stops here and is destroyed off the frame (a million entities take tens of milliseconds).
		Scene::ReleaseInBackground(std::move(m_Scene));
		m_Scene = std::move(scene);
		m_SceneHandle = handle;
		m_Scene->SetTimeDilation(m_TimeDilation);
		m_Scene->OnRuntimeStart(SceneRuntimeMode::Play);
		// What the previous scene used, this one's load did not bring in and this one does not request in its first frames is
		// released then.
		m_AssetManager->ScheduleTrim(AssetResidency::c_SceneSwitchTrimFrames, AssetResidency::c_SceneSwitchTrimFrames, loaded);
		CheckScriptFault();
	}

	void GameRuntime::UpdateSceneLoads()
	{
		if (!m_LoadingSceneLoad && !m_SceneLoad)
			return;
		ST_PROFILE_STAGE("Scene.Load");

		// The loading scene plays until the game's first scene is in (a failed one is only reported: the frame stays clear).
		if (m_LoadingSceneLoad)
		{
			SceneLoadRequest& request = m_LoadingSceneLoad->Request;
			request.Update();
			if (request.GetState() == SceneLoadState::Ready)
			{
				StartScene(request.TakeScene(), m_LoadingSceneLoad->Scene, request.GetAssets());
				m_ShowingLoadingScene = true;
				m_LoadingSceneLoad.reset();
			}
			else if (request.IsFinished())
			{
				ST_CORE_ERROR("'{}' shows no loading scene: {}", m_Manifest.Name, request.GetError());
				m_LoadingSceneLoad.reset();
			}
		}

		if (!m_SceneLoad)
			return;
		SceneLoadRequest& request = m_SceneLoad->Request;
		request.Update();
		if (m_Scene)
			m_Scene->SetSceneLoadProgress(request.GetProgress());
		if (!request.IsFinished())
			return;
		if (request.GetState() == SceneLoadState::Ready)
		{
			const bool first = !m_Started;
			StartScene(request.TakeScene(), m_SceneLoad->Scene, request.GetAssets());
			m_Started = true;
			m_ShowingLoadingScene = false;
			// Its pins go once the scene that uses the assets requests them itself (from its first frame on).
			m_SceneLoad.reset();
			m_LoadingSceneLoad.reset();
			if (first)
				ST_CORE_INFO("Started '{}' in {:.2f} s", m_Manifest.Name, std::chrono::duration<double>(std::chrono::steady_clock::now() - m_Opened).count());
			return;
		}
		const std::string error = request.GetError();
		m_SceneLoad.reset();
		if (m_Scene)
			m_Scene->SetSceneLoadProgress(std::nullopt);
		if (!m_Started)
		{
			m_StartError = fmt::format("Loading the start scene failed: {}", error);
			ST_CORE_ERROR("'{}' cannot start: {}", m_Manifest.Name, *m_StartError);
			return;
		}
		ST_CORE_ERROR("'{}' cannot switch scenes: {}", m_Manifest.Name, error);
	}

	bool GameRuntime::WaitForStart(std::chrono::milliseconds timeout)
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (!m_Started && !m_StartError && std::chrono::steady_clock::now() < deadline)
		{
			const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
			m_AssetManager->WaitForPendingLoads(std::max(left, std::chrono::milliseconds(1)));
			UpdateSceneLoads();
		}
		return m_Started;
	}

	void GameRuntime::Update(Timestep timestep)
	{
		if (m_QuitRequest)
			return;
		m_AssetManager->Update();
		UpdateSceneLoads();
		if (!m_Scene)
			return;
		m_Scene->OnUpdateRuntime(timestep);
		CheckScriptFault();
		HandleSceneRequests();
	}

	void GameRuntime::CheckScriptFault()
	{
		if (!m_ScriptEngine)
			return;
		if (!m_ScriptFault)
		{
			m_ScriptFault = m_ScriptEngine->GetFault();
			if (m_ScriptFault)
				ST_CORE_ERROR("The scripts of '{}' crashed and stay disabled for the rest of the session: {}", m_Manifest.Name, m_ScriptFault->Description);
		}
		// The crash's location comes from symbolization on a job, a moment later (the module ships with its PDB outside Dist).
		if (m_ScriptFault && !m_ScriptFault->Symbolized)
		{
			std::optional<ScriptFault> current = m_ScriptEngine->GetFault();
			if (current && current->Symbolized)
			{
				m_ScriptFault = std::move(current);
				if (m_ScriptFault->Location)
					ST_CORE_ERROR("The script crash happened at {}", DescribeScriptCodeLocation(*m_ScriptFault->Location));
			}
		}
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
		// The loading scene starting itself over loads again on its own: the start scene's load goes on.
		if (m_ShowingLoadingScene && scene == m_SceneHandle)
		{
			RestartLoadingScene();
			return;
		}
		std::string error;
		if (!LoadScene(scene, &error))
			ST_CORE_ERROR("'{}' cannot switch scenes: {}", m_Manifest.Name, error);
		// While the start scene loads, the loading scene may ask for another scene: that one is then the game's start.
	}

}
