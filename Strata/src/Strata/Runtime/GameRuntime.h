#pragma once

#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Core/Base.h"
#include "Strata/Core/Timestep.h"
#include "Strata/Project/GameManifest.h"
#include "Strata/Scene/Scene.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Strata
{

	// Runs an exported game: opens its asset pack (the active asset manager while the runtime lives), loads scenes
	// from it and simulates the current one. Independent of windowing and rendering, so it also runs headless.
	// Main thread only.
	class GameRuntime
	{
	public:
		// Loads the manifest, opens the asset pack next to it and starts the start scene. Null (with an error) when
		// any of that fails.
		static Scope<GameRuntime> Create(const std::filesystem::path& manifestPath, std::string* outError = nullptr);
		~GameRuntime();

		GameRuntime(const GameRuntime&) = delete;
		GameRuntime& operator=(const GameRuntime&) = delete;

		const GameManifest& GetManifest() const { return m_Manifest; }
		const Ref<RuntimeAssetManager>& GetAssetManager() const { return m_AssetManager; }
		const Ref<Scene>& GetScene() const { return m_Scene; }
		AssetHandle GetSceneHandle() const { return m_SceneHandle; }

		// Stops the current scene and starts another scene of the pack. The current scene keeps running on failure.
		bool LoadScene(AssetHandle scene, std::string* outError = nullptr);

		// Once per frame: finishes asset loads, advances the scene, then honors its requests (Scene::RequestQuit,
		// RequestSceneLoad): a quit ends the game (GetQuitRequest; further updates do nothing), a scene load switches to that
		// scene of the pack, or restarts the current one for the null handle (synchronously; a failure is logged and the
		// scene keeps running).
		void Update(Timestep timestep);
		// The exit code the game ended with, once its scene asked to quit.
		std::optional<int32_t> GetQuitRequest() const { return m_QuitRequest; }
	private:
		GameRuntime() = default;

		void HandleSceneRequests();
	private:
		GameManifest m_Manifest;
		Ref<RuntimeAssetManager> m_AssetManager;
		Ref<Scene> m_Scene;
		AssetHandle m_SceneHandle = UUID::Null();
		std::optional<int32_t> m_QuitRequest;
	};

}
