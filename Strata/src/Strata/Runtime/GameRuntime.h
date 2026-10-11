#pragma once

#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Core/Base.h"
#include "Strata/Core/Timestep.h"
#include "Strata/Project/GameManifest.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneLoader.h"
#include "Strata/Scripting/ScriptEngine.h"
#include "Strata/Scripting/ScriptTypes.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Strata
{

	struct GameRuntimeOptions
	{
		// Budgets for the game's asset manager instead of the defaults (AssetManagerBase::GetDefaultResidencyBudgets), in
		// effect before the start scene loads.
		std::optional<AssetResidencyBudgets> AssetBudgets;
		// Scenes slow down instead of catching up when their fixed steps cannot keep up with real time
		// (Scene::SetTimeDilation): for games driven by the wall clock. Off by default, so that tools and tests that drive
		// the runtime with chosen timesteps step deterministically.
		bool TimeDilation = false;
	};

	// Runs an exported game: opens its asset pack (the active asset manager while the runtime lives), loads its script
	// module (the active script engine while the runtime lives; no hot reload), loads scenes from the pack and simulates the
	// current one. Independent of windowing and rendering, so it also runs headless. Main thread only.
	//
	// Scenes load without stopping the game (SceneLoader): the scene and everything it uses load in the background while
	// frames go on, and the new scene replaces the running one between updates once all of it is in. While the start scene
	// loads, the manifest's loading scene plays (GameManifest::LoadingScene), or nothing (renderers clear the frame); a scene
	// switch keeps the running scene playing (scripts read the progress, Game::GetSceneLoadProgress).
	//
	// When the scripts crash, the module is disabled for the rest of the session (the scene keeps running without them):
	// the crash is logged once and reported by GetScriptFault, so the application can decide what to do.
	class GameRuntime
	{
	public:
		// Loads the manifest, opens the asset pack and the script module next to it and starts loading the start scene (and
		// the loading scene, which plays meanwhile). Null (with an error) when any of that fails or the manifest names scenes
		// the pack does not have. The start scene's load can still fail later (GetStartError).
		static Scope<GameRuntime> Create(const std::filesystem::path& manifestPath, std::string* outError = nullptr, const GameRuntimeOptions& options = {});
		~GameRuntime();

		GameRuntime(const GameRuntime&) = delete;
		GameRuntime& operator=(const GameRuntime&) = delete;

		const GameManifest& GetManifest() const { return m_Manifest; }
		const Ref<RuntimeAssetManager>& GetAssetManager() const { return m_AssetManager; }
		// The scene that plays: the game's, or the loading scene while the start scene loads; null while neither is in.
		const Ref<Scene>& GetScene() const { return m_Scene; }
		// The scene asset GetScene was loaded from (null while nothing plays).
		AssetHandle GetSceneHandle() const { return m_SceneHandle; }
		// Whether the loading scene plays (the start scene is still loading).
		bool IsShowingLoadingScene() const { return m_ShowingLoadingScene; }
		// The game's first scene started (it may have switched since).
		bool HasStarted() const { return m_Started; }
		// Why the start scene could not be loaded: the game cannot start (StrataRuntime exits).
		const std::optional<std::string>& GetStartError() const { return m_StartError; }
		// The game's script engine (null for games without scripts).
		const Ref<ScriptEngine>& GetScriptEngine() const { return m_ScriptEngine; }
		// The crash that disabled the scripts, if they crashed.
		const std::optional<ScriptFault>& GetScriptFault() const { return m_ScriptFault; }

		// Starts loading a scene of the pack: the running scene goes on until it and everything it uses are loaded, then
		// Update swaps it in (stopping the running scene, starting the new one's systems; the assets the new scene does not
		// use are released a few frames later, AssetManagerBase::ScheduleTrim). A load under way is replaced. False, with
		// nothing changed, for a handle that is not a scene of the pack.
		bool LoadScene(AssetHandle scene, std::string* outError = nullptr);
		// A scene is loading to replace the running one (or to start the game).
		bool IsLoadingScene() const { return m_SceneLoad != nullptr; }
		// The scene asset loading, and the share of it and what it uses that is in (SceneLoadRequest::GetProgress).
		AssetHandle GetLoadingSceneAsset() const { return m_SceneLoad ? m_SceneLoad->Scene : UUID::Null(); }
		float GetSceneLoadProgress() const { return m_SceneLoad ? m_SceneLoad->Request.GetProgress() : 1.0f; }

		// Once per frame: finishes asset loads, advances the scene loads (swapping in a scene that is ready), advances the scene,
		// then honors its requests (Scene::RequestQuit, RequestSceneLoad): a quit ends the game (GetQuitRequest; further updates
		// do nothing), a scene load starts loading that scene of the pack, or the current one again for the null handle (a
		// failure is logged and the scene keeps running).
		void Update(Timestep timestep);
		// For tools and tests: waits for the start scene and starts it (or fails, GetStartError) without running any frame.
		// False when it failed or took longer than `timeout`.
		bool WaitForStart(std::chrono::milliseconds timeout = std::chrono::minutes(2));
		// The exit code the game ended with, once its scene asked to quit.
		std::optional<int32_t> GetQuitRequest() const { return m_QuitRequest; }
	private:
		GameRuntime() = default;

		// A scene loading for the game (the next scene, or the loading scene).
		struct PendingScene
		{
			AssetHandle Scene = UUID::Null();
			SceneLoadRequest Request;
		};

		// Advances the loading scene's and the next scene's loads; starts what is ready.
		void UpdateSceneLoads();
		// Stops the running scene and starts `scene` (of asset `handle`) in its place.
		void StartScene(Ref<Scene> scene, AssetHandle handle);
		// Records (and logs once) a crash of the script module.
		void CheckScriptFault();
		void HandleSceneRequests();
	private:
		GameManifest m_Manifest;
		Ref<RuntimeAssetManager> m_AssetManager;
		Ref<Scene> m_Scene;
		AssetHandle m_SceneHandle = UUID::Null();
		bool m_ShowingLoadingScene = false;
		bool m_Started = false;
		std::optional<std::string> m_StartError;
		Scope<PendingScene> m_SceneLoad;
		Scope<PendingScene> m_LoadingSceneLoad;
		bool m_TimeDilation = false; // GameRuntimeOptions::TimeDilation, for every scene the game plays
		std::optional<int32_t> m_QuitRequest;
		Ref<ScriptEngine> m_ScriptEngine;
		Ref<ScriptEngine> m_PreviousScriptEngine; // Active before this runtime; restored when it ends
		std::optional<ScriptFault> m_ScriptFault;
	};

}
