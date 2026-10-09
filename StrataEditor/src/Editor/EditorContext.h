#pragma once

#include "Editor/EditorViewport.h"
#include "Editor/SceneEdit.h"
#include "Editor/ScriptBuild.h"
#include "Editor/UndoStack.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/Crypto.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Project/Project.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scripting/ScriptEngine.h>
#include <Strata/Scripting/ScriptTypes.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	enum class SceneState : uint8_t
	{
		Edit = 0,
		Play,
		Simulate
	};

	const char* SceneStateToString(SceneState state);

	struct EditorContextSpecification
	{
		bool WatchAssetFiles = true;  // Hot reload of files changed outside the editor
		// Reload the script module when its file changes (e.g. rebuilt from an IDE). With it, modules run from a private copy;
		// without it they load in place, which on Windows keeps script.build from replacing a loaded module.
		bool HotReloadScripts = true;
		// The toolchain script.build uses (the engine's own by default).
		ScriptBuildSettings ScriptBuild = ScriptBuildSettings::GetEngineDefaults();
	};

	// What became of the module of the last finished script build.
	struct ScriptBuildLoad
	{
		uint64_t BuildID = 0;
		bool Loaded = false; // The built module is the loaded one (loaded, reloaded, or already loaded and unchanged)
		bool Reloaded = false; // It replaced a loaded module (running scenes went through a hot reload)
		std::string Error;   // Why it could not be loaded
	};

	// The file of the script module the editor runs, as read back for shipping it (EditorContext::ReadRunningScriptModule).
	struct ScriptModuleFile
	{
		std::filesystem::path Path; // Empty when no module is loaded
		std::vector<uint8_t> Bytes;
	};

	// The editor's state independent of any UI: the open project and its assets, the edited scene, play mode, the
	// selection, the undo history and the viewport (editor camera, overlays, rendering). Every editor operation (UI,
	// automation, tests) goes through it. Without a project, an asset manager with only the built-in assets is active, so
	// built-in meshes and materials render. Main thread only.
	class EditorContext
	{
	public:
		explicit EditorContext(const EditorContextSpecification& specification = {});
		~EditorContext();

		EditorContext(const EditorContext&) = delete;
		EditorContext& operator=(const EditorContext&) = delete;

		//////////////////////////////////////////////////////////////////////////
		// Project
		//////////////////////////////////////////////////////////////////////////

		// Creates a project in `directory` and opens it.
		bool CreateProject(const std::filesystem::path& directory, const std::string& name, std::string* outError = nullptr);
		// Opens a project file, or the project in a directory. Closes the current project first.
		bool OpenProject(const std::filesystem::path& path, std::string* outError = nullptr);
		// Saves the project's viewport state (editor camera and settings) to its intermediate directory, then closes it.
		void CloseProject();
		bool HasProject() const { return m_Project != nullptr; }
		const Ref<Project>& GetProject() const { return m_Project; }
		// Null without a project.
		EditorAssetManager* GetAssetManager() const { return m_AssetManager.get(); }

		//////////////////////////////////////////////////////////////////////////
		// Scene
		//////////////////////////////////////////////////////////////////////////

		// Replaces the edited scene with an empty one (stops play mode, clears selection and history).
		void NewScene(const std::string& name = "Untitled");
		bool OpenScene(AssetHandle handle, std::string* outError = nullptr);
		// Saves the edited scene to its asset; fails for scenes that were never saved (use SaveSceneAs).
		bool SaveScene(std::string* outError = nullptr);
		// Saves the edited scene as a new ".stscene" asset (path relative to the asset directory) and continues
		// editing that asset.
		bool SaveSceneAs(const std::string& relativePath, std::string* outError = nullptr);
		AssetHandle GetSceneHandle() const { return m_SceneHandle; }
		bool IsSceneModified() const { return m_UndoStack.IsModified(); }

		const Ref<Scene>& GetEditScene() const { return m_EditScene; }
		// The scene edits apply to: the running copy while playing, otherwise the edited scene.
		const Ref<Scene>& GetActiveScene() const { return m_RuntimeScene ? m_RuntimeScene : m_EditScene; }

		//////////////////////////////////////////////////////////////////////////
		// Play mode
		//////////////////////////////////////////////////////////////////////////

		// Runs a copy of the edited scene, with scripts, physics and audio. Changes made while playing are discarded by Stop,
		// which also restores the engine-wide master volume a game may have changed (AudioSystem::SetMasterVolume).
		// The running game's requests are honored after each update (Update): a quit (Scene::RequestQuit) stops play mode and
		// logs the exit code; a scene load (Scene::RequestSceneLoad) replaces the running scene with that scene asset, or restarts
		// the running one for the null handle (a fresh copy of the edited scene when that is what runs). Stop returns to the edited
		// scene either way. Starting and stopping drop simulated input (Input::ClearSimulated, the input.* commands), so keys a
		// tool held never carry over into another session.
		bool Play(std::string* outError = nullptr);
		// Like Play, but only physics runs (no scripts or audio).
		bool Simulate(std::string* outError = nullptr);
		void Stop();
		SceneState GetSceneState() const { return m_SceneState; }
		bool IsPlaying() const { return m_SceneState != SceneState::Edit; }
		void SetPaused(bool paused);
		bool IsPaused() const;
		// While paused, advances the simulation by `frames` fixed steps over the next updates.
		void Step(uint32_t frames = 1);
		// Whether the viewport's game view feeds keyboard and mouse input to the running game (play mode through the
		// scene's camera with the viewport focused). Editor shortcuts that edit the scene stay off meanwhile, so keys meant
		// for the game never change it. Only play mode can have game input; stopping ends it.
		void SetGameInputActive(bool active);
		bool IsGameInputActive() const { return m_GameInputActive; }
		// Whether keyboard shortcuts that edit the scene or the project (undo, delete, duplicate, save) may act now.
		bool AcceptsEditShortcuts() const { return !m_GameInputActive; }

		//////////////////////////////////////////////////////////////////////////
		// Selection (entities of the active scene)
		//////////////////////////////////////////////////////////////////////////

		const std::vector<UUID>& GetSelection() const { return m_Selection; }
		void SetSelection(std::vector<UUID> selection);
		void Select(UUID entity, bool additive = false);
		void Deselect(UUID entity);
		void ClearSelection() { m_Selection.clear(); }
		bool IsSelected(UUID entity) const;
		// The most recently selected entity that still exists.
		Entity GetPrimarySelection() const;
		// Drops selected entities that no longer exist in the active scene.
		void PruneSelection();

		//////////////////////////////////////////////////////////////////////////
		// Undo
		//////////////////////////////////////////////////////////////////////////

		UndoStack& GetUndoStack() { return m_UndoStack; }
		// Records an edit of the active scene. Edits of the running copy during play are not recorded: they are
		// discarded with it. Returns true when an undo step was recorded.
		bool CommitEdit(SceneEditTransaction& transaction);
		bool Undo();
		bool Redo();

		//////////////////////////////////////////////////////////////////////////
		// Scripts
		//////////////////////////////////////////////////////////////////////////

		// The project's script engine (null without a project). Scenes the editor plays run their scripts through it. When
		// a project opens, its built module (Project::GetScriptModulePath) is loaded if it exists.
		const Ref<ScriptEngine>& GetScriptEngine() const { return m_ScriptEngine; }
		// Loads a script module file instead of the loaded one; running scenes keep their script state (hot reload).
		bool LoadScriptModule(const std::filesystem::path& path, std::string* outError = nullptr);
		// Loads the loaded module's file again, or the project's built module when none is loaded.
		bool ReloadScripts(std::string* outError = nullptr);
		// Starts building the project's scripts in the background (one build at a time). When the build succeeds, its
		// module is loaded, or reloaded if it changed; the outcome is GetLastScriptBuildLoad.
		bool BuildScripts(std::string* outError = nullptr);
		const ScriptBuilder& GetScriptBuilder() const { return m_ScriptBuilder; }
		const ScriptBuildLoad& GetLastScriptBuildLoad() const { return m_LastScriptBuildLoad; }
		// The script crash that stopped play mode last; cleared when a module loads.
		const std::optional<ScriptFault>& GetLastScriptFault() const { return m_LastScriptFault; }
		// Reads the loaded module's file and checks that it is still the file that was loaded, so that exports ship the
		// scripts the editor runs. Fails while a script build runs (it may be writing the file) and when the file changed
		// since it was loaded (e.g. a build whose module could not be loaded). Without a loaded module it succeeds with an
		// empty path.
		bool ReadRunningScriptModule(ScriptModuleFile& outFile, std::string* outError = nullptr) const;

		//////////////////////////////////////////////////////////////////////////
		// Viewport
		//////////////////////////////////////////////////////////////////////////

		// The editor camera, viewport settings and viewport rendering. Its state is saved per project in
		// "<project>/.strata/EditorViewport.json" when the project closes and restored when it opens.
		EditorViewport& GetViewport() { return m_Viewport; }
		const EditorViewport& GetViewport() const { return m_Viewport; }

		// Once per frame: script hot reload and builds, asset hot reload and loading, then the scene update (simulation
		// while playing) and the running game's requests, then finished viewport picks. A script crash while playing stops
		// play mode.
		void Update(Timestep timestep);

		//////////////////////////////////////////////////////////////////////////
		// Editor services
		//////////////////////////////////////////////////////////////////////////

		// Asks the editor to close after the current frame (editor.quit); the application layer polls the request.
		void RequestQuit() { m_QuitRequested = true; }
		bool IsQuitRequested() const { return m_QuitRequested; }

		// Extra sections of editor.status, reported by the parts of the editor that own the information (e.g.
		// "automation" by EditorAutomation). Providers run on the main thread whenever editor.status runs. A null provider
		// removes the section; a section named like one of the built-in ones is not reported.
		using StatusProvider = std::function<nlohmann::json()>;
		void SetStatusProvider(const std::string& section, StatusProvider provider);
		const std::map<std::string, StatusProvider>& GetStatusProviders() const { return m_StatusProviders; }
	private:
		bool OpenProjectInternal(const std::filesystem::path& path, bool created, std::string* outError);
		bool StartRuntime(SceneRuntimeMode mode, std::string* outError);
		void HandleRuntimeRequests();
		// Replaces the running scene with a scene asset, or with a restart of the running scene for the null handle.
		bool SwitchRuntimeScene(AssetHandle scene, std::string* outError);
		void ResetScene(Ref<Scene> scene, AssetHandle handle);
		void OpenScriptEngine(bool created);
		void CloseScriptEngine();
		// Fingerprints the loaded module's file after a load. `expected` is its digest from before the load (if known):
		// a file that changed while it was being loaded leaves the running version unknown.
		void RecordScriptModuleFile(const std::optional<Sha256Digest>& expected);
		void OnScriptBuildFinished();
		// Stops play mode (and reports the fault) when the script module crashed. Returns true if it did.
		bool StopOnScriptFault();
		// Saves the viewport state and closes the project; with activateBuiltinAssets the built-in asset manager becomes active.
		void ReleaseProject(bool activateBuiltinAssets);
		void ActivateBuiltinAssets();
		std::filesystem::path GetViewportStateFile() const;
	private:
		EditorContextSpecification m_Specification;
		Ref<Project> m_Project;
		Ref<EditorAssetManager> m_AssetManager;

		Ref<Scene> m_EditScene;
		Ref<Scene> m_RuntimeScene;
		AssetHandle m_RuntimeSceneAsset = UUID::Null(); // The scene asset playing after a switch; null while a copy of the edited scene runs
		AssetHandle m_SceneHandle = UUID::Null();
		SceneState m_SceneState = SceneState::Edit;
		float m_MasterVolumeBeforePlay = 1.0f;
		bool m_GameInputActive = false;

		std::vector<UUID> m_Selection;
		UndoStack m_UndoStack;

		Ref<ScriptEngine> m_ScriptEngine;
		ScriptBuilder m_ScriptBuilder;
		ScriptBuildLoad m_LastScriptBuildLoad;
		std::optional<ScriptFault> m_LastScriptFault;
		// The loaded module's file as it was loaded (see ReadRunningScriptModule), and the load it belongs to.
		std::optional<Sha256Digest> m_ScriptModuleDigest;
		uint64_t m_ScriptModuleLoadCount = 0;
		Ref<AssetManagerBase> m_BuiltinAssets; // Active while no project is open
		EditorViewport m_Viewport;

		bool m_QuitRequested = false;
		std::map<std::string, StatusProvider> m_StatusProviders;
	};

}
