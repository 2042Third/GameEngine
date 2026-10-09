#pragma once

#include "Editor/SceneEdit.h"
#include "Editor/ScriptBuild.h"
#include "Editor/UndoStack.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Project/Project.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scripting/ScriptEngine.h>
#include <Strata/Scripting/ScriptTypes.h>

#include <filesystem>
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
		bool HotReloadScripts = true; // Reload the script module when its file changes (e.g. rebuilt from an IDE)
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

	// The editor's state independent of any UI: the open project and its assets, the edited scene, play mode, the
	// selection and the undo history. Every editor operation (UI, automation, tests) goes through it. Main thread only.
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

		// Runs a copy of the edited scene. Changes made while playing are discarded by Stop.
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

		// Once per frame: script hot reload and builds, asset hot reload and loading, then the scene update (simulation
		// while playing). A script crash while playing stops play mode.
		void Update(Timestep timestep);
	private:
		bool OpenProjectInternal(const std::filesystem::path& path, bool created, std::string* outError);
		bool StartRuntime(SceneRuntimeMode mode, std::string* outError);
		void ResetScene(Ref<Scene> scene, AssetHandle handle);
		void OpenScriptEngine(bool created);
		void CloseScriptEngine();
		void OnScriptBuildFinished();
		// Stops play mode (and reports the fault) when the script module crashed. Returns true if it did.
		bool StopOnScriptFault();
	private:
		EditorContextSpecification m_Specification;
		Ref<Project> m_Project;
		Ref<EditorAssetManager> m_AssetManager;

		Ref<Scene> m_EditScene;
		Ref<Scene> m_RuntimeScene;
		AssetHandle m_SceneHandle = UUID::Null();
		SceneState m_SceneState = SceneState::Edit;

		std::vector<UUID> m_Selection;
		UndoStack m_UndoStack;

		Ref<ScriptEngine> m_ScriptEngine;
		ScriptBuilder m_ScriptBuilder;
		ScriptBuildLoad m_LastScriptBuildLoad;
		std::optional<ScriptFault> m_LastScriptFault;
	};

}
