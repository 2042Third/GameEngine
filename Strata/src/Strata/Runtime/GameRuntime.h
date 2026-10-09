#pragma once

#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Core/Base.h"
#include "Strata/Core/Timestep.h"
#include "Strata/Project/GameManifest.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scripting/ScriptEngine.h"
#include "Strata/Scripting/ScriptTypes.h"

#include <filesystem>
#include <optional>
#include <string>

namespace Strata
{

	// Runs an exported game: opens its asset pack (the active asset manager while the runtime lives), loads its script
	// module (the active script engine while the runtime lives; no hot reload), loads scenes from the pack and
	// simulates the current one. Independent of windowing and rendering, so it also runs headless. Main thread only.
	//
	// When the scripts crash, the module is disabled for the rest of the session (the scene keeps running without them):
	// the crash is logged once and reported by GetScriptFault, so the application can decide what to do.
	class GameRuntime
	{
	public:
		// Loads the manifest, opens the asset pack and the script module next to it and starts the start scene. Null
		// (with an error) when any of that fails.
		static Scope<GameRuntime> Create(const std::filesystem::path& manifestPath, std::string* outError = nullptr);
		~GameRuntime();

		GameRuntime(const GameRuntime&) = delete;
		GameRuntime& operator=(const GameRuntime&) = delete;

		const GameManifest& GetManifest() const { return m_Manifest; }
		const Ref<RuntimeAssetManager>& GetAssetManager() const { return m_AssetManager; }
		const Ref<Scene>& GetScene() const { return m_Scene; }
		AssetHandle GetSceneHandle() const { return m_SceneHandle; }
		// The game's script engine (null for games without scripts).
		const Ref<ScriptEngine>& GetScriptEngine() const { return m_ScriptEngine; }
		// The crash that disabled the scripts, if they crashed.
		const std::optional<ScriptFault>& GetScriptFault() const { return m_ScriptFault; }

		// Stops the current scene and starts another scene of the pack. The current scene keeps running on failure.
		bool LoadScene(AssetHandle scene, std::string* outError = nullptr);

		// Once per frame: finishes asset loads, then advances the scene.
		void Update(Timestep timestep);
	private:
		// Records (and logs once) a crash of the script module.
		void CheckScriptFault();
	private:
		GameRuntime() = default;
	private:
		GameManifest m_Manifest;
		Ref<RuntimeAssetManager> m_AssetManager;
		Ref<Scene> m_Scene;
		AssetHandle m_SceneHandle = UUID::Null();
		Ref<ScriptEngine> m_ScriptEngine;
		Ref<ScriptEngine> m_PreviousScriptEngine; // Active before this runtime; restored when it ends
		std::optional<ScriptFault> m_ScriptFault;
	};

}
