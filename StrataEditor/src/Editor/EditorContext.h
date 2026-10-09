#pragma once

#include "Editor/SceneEdit.h"
#include "Editor/UndoStack.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Project/Project.h>
#include <Strata/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <map>
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
		bool WatchAssetFiles = true; // Hot reload of files changed outside the editor
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

		// Once per frame: asset hot reload and loading, then the scene update (simulation while playing).
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
		bool StartRuntime(SceneRuntimeMode mode, std::string* outError);
		void ResetScene(Ref<Scene> scene, AssetHandle handle);
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

		bool m_QuitRequested = false;
		std::map<std::string, StatusProvider> m_StatusProviders;
	};

}
