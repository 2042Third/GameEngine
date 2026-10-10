#include "stpch.h"
#include "Strata/Runtime/GameRuntime.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Scene/Prefab.h"

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
		runtime->m_Manifest = std::move(*manifest);
		const std::filesystem::path packPath = manifestPath.parent_path() / FileSystem::FromUTF8(runtime->m_Manifest.AssetPack);
		runtime->m_AssetManager = RuntimeAssetManager::Create(packPath, outError);
		if (!runtime->m_AssetManager)
			return nullptr;
		if (options.AssetBudgets)
			runtime->m_AssetManager->SetResidencyBudgets(*options.AssetBudgets);
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

		if (!runtime->LoadScene(runtime->m_Manifest.StartScene, outError))
			return nullptr; // The destructor deactivates the asset manager and the script engine
		runtime->CheckScriptFault();
		std::string scripts;
		if (runtime->m_ScriptEngine)
		{
			const size_t classes = runtime->m_ScriptEngine->GetClasses().size();
			scripts = fmt::format(", {} script {}", classes, classes == 1 ? "class" : "classes");
		}
		const size_t assets = runtime->m_AssetManager->GetAllMetadata().size();
		ST_CORE_INFO("Started '{}' ({} {}{})", runtime->m_Manifest.Name, assets, assets == 1 ? "asset" : "assets", scripts);
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
		// What the previous scene used and this one does not request in its first frames is released then.
		m_AssetManager->ScheduleTrim(AssetResidency::c_SceneSwitchTrimFrames, AssetResidency::c_SceneSwitchTrimFrames);
		return true;
	}

	void GameRuntime::Update(Timestep timestep)
	{
		if (m_QuitRequest)
			return;
		m_AssetManager->Update();
		m_Scene->OnUpdateRuntime(timestep);
		CheckScriptFault();
		HandleSceneRequests();
	}

	void GameRuntime::CheckScriptFault()
	{
		if (m_ScriptFault || !m_ScriptEngine)
			return;
		m_ScriptFault = m_ScriptEngine->GetFault();
		if (m_ScriptFault)
			ST_CORE_ERROR("The scripts of '{}' crashed and stay disabled for the rest of the session: {}", m_Manifest.Name, m_ScriptFault->Description);
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
