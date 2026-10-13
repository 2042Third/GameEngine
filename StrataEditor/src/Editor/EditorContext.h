#pragma once

#include "Editor/CommandJournal.h"
#include "Editor/EditorNotifications.h"
#include "Editor/EditorGameWindow.h"
#include "Editor/EditorViewport.h"
#include "Editor/ExternalEditor.h"
#include "Editor/GameExport.h"
#include "Editor/PrefabSync.h"
#include "Editor/ProjectTemplates.h"
#include "Editor/ProjectThumbnail.h"
#include "Editor/RecentProjects.h"
#include "Editor/SceneEdit.h"
#include "Editor/SceneRecovery.h"
#include "Editor/ScriptBuild.h"
#include "Editor/ScriptSources.h"
#include "Editor/SimulatedInput.h"
#include "Editor/ThumbnailCache.h"
#include "Editor/UndoStack.h"

#include <Strata/Animation/AnimationPreview.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/Crypto.h>
#include <Strata/Core/EngineServices.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Project/Project.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneLoader.h>
#include <Strata/Scripting/ScriptEngine.h>
#include <Strata/Scripting/ScriptTypes.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
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

	// What the inspector panels show: the selected entities, or one asset (EditorContext::SetInspectedAsset).
	enum class InspectionTarget : uint8_t
	{
		Entities = 0,
		Asset
	};

	struct EditorContextSpecification
	{
		bool WatchAssetFiles = true;  // Hot reload of files changed outside the editor
		// Reload the script module when its file changes (e.g. rebuilt from an IDE). With it, modules run from a private copy;
		// without it they load in place, which on Windows keeps script.build from replacing a loaded module.
		bool HotReloadScripts = true;
		// The toolchain script.build uses: the engine's own, or in an installed Strata the installation's SDK with a toolchain
		// found on this machine (ScriptBuildSettings::Detect).
		ScriptBuildSettings ScriptBuild = ScriptBuildSettings::Detect();
		// Where builds that find their toolchain look (ScriptBuilder::SetToolchainEnvironment); empty: this machine. Tests
		// describe machines.
		std::function<Scope<ScriptToolchainEnvironment>()> ToolchainEnvironment;
		// Where the list of recently opened projects is kept (RecentProjects; the editor application passes the user's,
		// RecentProjects::GetDefaultFile). Empty: in memory only, as tests need. Read-only: read but never written.
		std::filesystem::path RecentProjectsFile;
		bool RecentProjectsReadOnly = false;
		// Where the user's editor settings are kept, such as the external code editor (ExternalEditorLauncher; the editor
		// application passes ExternalEditor::GetDefaultSettingsFile). Empty: in memory only, as tests need.
		std::filesystem::path EditorSettingsFile;
		// Where project.samples and project.openSample find the samples (ProjectSamples); empty: next to the executable
		// (ProjectSamples::GetDefaultDirectory).
		std::filesystem::path SamplesDirectory;
		// Running scenes slow down instead of catching up when their fixed steps cannot keep up with real time
		// (Scene::SetTimeDilation). The editor turns it on for interactive and automation sessions; off by default, so that
		// tests and scripted runs step deterministically.
		bool TimeDilation = false;
		// Opening a project imports what needs it in the background (ScanImports::InBackground): the project opens at once and
		// frames go on while textures compress, the assets arriving as their imports finish. The editor turns it on for
		// interactive and automation sessions; off by default, so that tests and scripted runs find every asset imported.
		bool ImportInBackground = false;
	};

	// What became of the module of the last finished script build.
	struct ScriptBuildLoad
	{
		uint64_t BuildID = 0;
		bool Loaded = false; // The built module is the loaded one (loaded, reloaded, or already loaded and unchanged)
		bool Reloaded = false; // It replaced a loaded module (running scenes went through a hot reload)
		std::string Error;   // Why it could not be loaded
	};

	// A scene being opened for editing (EditorContext::BeginOpenScene), or the last one that was: identified by ID, so that
	// whoever started a load (scene.open) recognizes its own.
	struct EditorSceneLoadStatus
	{
		uint64_t ID = 0;                      // 0 before the first load
		AssetHandle Scene = UUID::Null();
		bool StartScene = false;              // The project's start scene, opened with the project (BeginOpenProject)
		SceneLoadState State = SceneLoadState::Cancelled;
		float Progress = 0.0f;                // Of the scene and everything it uses (SceneLoadRequest::GetProgress)
		std::string Error;                    // Why it failed or was cancelled
		std::chrono::steady_clock::time_point Started;
		double Seconds = 0.0;                 // From the start until it finished

		bool IsFinished() const { return State == SceneLoadState::Ready || State == SceneLoadState::Failed || State == SceneLoadState::Cancelled; }
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

		// Creates a project in `directory` from a template (ProjectTemplates) and opens it. Templates with content save their
		// scene as the start scene (ProjectTemplates::c_StartScenePath) and open it in the template's editor view. An
		// unknown template fails before anything is created.
		bool CreateProject(const std::filesystem::path& directory, const std::string& name, std::string_view templateId = ProjectTemplates::c_Empty,
			std::string* outError = nullptr);
		// Opens a project file, or the project in a directory, and starts opening its start scene in the background
		// (BeginOpenScene; GetSceneLoadStatus().StartScene): the project is open when this returns, with an empty edited scene
		// that the start scene replaces between frames once it and everything it uses are in. Closes the current project
		// first. Opened projects go to the front of the recent projects. A start scene that cannot be opened is logged as a
		// warning; the project stays open with the empty scene.
		bool BeginOpenProject(const std::filesystem::path& path, std::string* outError = nullptr);
		// BeginOpenProject, then waits for the start scene (WaitForSceneLoad): tools, tests, scripted and headless runs.
		bool OpenProject(const std::filesystem::path& path, std::string* outError = nullptr);
		// Saves the project's viewport state (editor camera and settings) to its intermediate directory, then closes it.
		void CloseProject();
		bool HasProject() const { return m_Project != nullptr; }
		const Ref<Project>& GetProject() const { return m_Project; }
		// Null without a project.
		EditorAssetManager* GetAssetManager() const { return m_AssetManager.get(); }
		// Changes whenever a project opens or closes. UI state kept per project compares it rather than the manager's address,
		// which the next project's manager may take over.
		uint64_t GetProjectGeneration() const { return m_ProjectGeneration; }

		//////////////////////////////////////////////////////////////////////////
		// Scene
		//////////////////////////////////////////////////////////////////////////

		// Replaces the edited scene with a new one (stops play mode, clears selection and history): empty, or with a
		// template's content (ProjectTemplates; the editor camera then takes the template's view). A template that uses
		// assets of the project finds them, or makes them when the project lacks them (basic3d's ground material,
		// ProjectTemplates::c_GroundMaterialPath); without a project it uses built-in ones. False (nothing changed) for an
		// unknown template.
		bool NewScene(const std::string& name = "Untitled", std::string_view templateId = ProjectTemplates::c_Empty);
		// Starts opening a scene asset for editing (scene.open) without waiting for it: the file is read again (not a copy
		// loaded before) and prepared on the loading worker while everything it uses loads (SceneLoader); Update replaces the
		// edited scene between frames once all of it is in (GetSceneLoadStatus). Meanwhile the edited scene stays visible and
		// read-only: edits are refused (CommitEdit) and so is play mode. Starting another load cancels this one, and so do
		// NewScene and closing the project. The editor camera returns to where it was when the scene was last shown; a scene
		// shown for the first time is framed (EditorViewport::FrameScene), and framed again once meshes that were still
		// loading have loaded, unless the camera was moved meanwhile. False (nothing started) without a project or for a
		// handle that is not a scene asset of the project.
		bool BeginOpenScene(AssetHandle handle, std::string* outError = nullptr);
		// BeginOpenScene, then waits for the load (WaitForSceneLoad: tools and tests).
		bool OpenScene(AssetHandle handle, std::string* outError = nullptr);
		// Waits for the scene being opened (if any) and makes it the edited scene, finalizing what it uses on this thread;
		// for tools and tests, never in a frame. False when the load failed, was cancelled or took longer than `timeout` (it is
		// then cancelled); GetSceneLoadStatus says why.
		bool WaitForSceneLoad(std::chrono::milliseconds timeout = std::chrono::minutes(2));
		// A scene is being opened (BeginOpenScene) and has not replaced the edited scene yet.
		bool IsSceneLoading() const { return m_SceneLoad != nullptr; }
		// The edited scene cannot change while a scene is being opened (edits of a running copy are allowed): false, with the
		// reason, then. Code that edits the scene outside SceneEditTransaction/CommitEdit (which rolls such edits back) checks it.
		bool CheckSceneEditable(std::string* outError = nullptr) const;
		// The scene load in progress (its progress as of the last Update), or the last one that finished.
		const EditorSceneLoadStatus& GetSceneLoadStatus() const { return m_SceneLoadStatus; }
		// Abandons the scene load in progress (its pins and unfinished loads go); the edited scene stays.
		void CancelSceneLoad();
		// Saves the edited scene to its asset; fails for scenes that were never saved (use SaveSceneAs).
		bool SaveScene(std::string* outError = nullptr);
		// Saves the edited scene as a new ".stscene" asset (path relative to the asset directory) and continues
		// editing that asset.
		bool SaveSceneAs(const std::string& relativePath, std::string* outError = nullptr);
		AssetHandle GetSceneHandle() const { return m_SceneHandle; }
		bool IsSceneModified() const { return m_UndoStack.IsModified(); }

		const Ref<Scene>& GetEditScene() const { return m_EditScene; }
		// The animation preview of the edited scene (animation.preview): poses its animators show while it does not play,
		// without changing the scene. Made on first use; another edited scene starts without one. Update advances it.
		AnimationPreview& GetAnimationPreview();
		AnimationPreview* FindAnimationPreview() const { return m_AnimationPreview.get(); }
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
		// The scene a running game asked for (Scene::RequestSceneLoad) while it loads (SwitchRuntimeScene), and its progress.
		bool IsRuntimeSceneLoading() const { return m_RuntimeSceneLoad != nullptr; }
		AssetHandle GetRuntimeSceneLoadTarget() const { return m_RuntimeSceneLoad ? m_RuntimeSceneLoad->Scene : UUID::Null(); }
		float GetRuntimeSceneLoadProgress() const { return m_RuntimeSceneLoad ? m_RuntimeSceneLoad->Request.GetProgress() : 1.0f; }
		// While paused, advances the simulation by `frames` fixed steps over the next updates.
		void Step(uint32_t frames = 1);
		// Whether the viewport's game view feeds keyboard and mouse input to the running game (play mode through the
		// scene's camera with the viewport focused). Editor shortcuts that edit the scene stay off meanwhile, so keys meant
		// for the game never change it. Only play mode can have game input; stopping ends it.
		void SetGameInputActive(bool active);
		bool IsGameInputActive() const { return m_GameInputActive; }
		// The keys and mouse buttons the input.* commands hold in the running game. Input frames (Input::SetSuspended) stop
		// while the game is paused without a pending step, so simulated input given meanwhile reaches its next update.
		SimulatedInput& GetSimulatedInput() { return m_SimulatedInput; }
		const SimulatedInput& GetSimulatedInput() const { return m_SimulatedInput; }
		// Whether keyboard shortcuts that edit the scene or the project (undo, delete, duplicate, save) may act now.
		bool AcceptsEditShortcuts() const { return !m_GameInputActive; }
		// The game's shell while it plays here: its window requests are recorded, not applied (EditorGameWindow, the engine
		// service IGameWindow while the context lives), its save files go to <project>/.strata/Saves (IGameSaves; none without
		// a project). Starting and stopping play forget the window requests and give the cursor back (the game's cursor mode
		// returns to Normal).
		EditorGameWindow& GetGameWindow() { return m_GameWindow; }
		const EditorGameWindow& GetGameWindow() const { return m_GameWindow; }
		std::optional<std::filesystem::path> GetGameSaveDirectory() const;

		//////////////////////////////////////////////////////////////////////////
		// Selection (entities of the active scene)
		//////////////////////////////////////////////////////////////////////////

		// In selection order (the last one is the primary selection).
		const std::vector<UUID>& GetSelection() const { return m_Selection; }
		void SetSelection(std::vector<UUID> selection);
		void Select(UUID entity, bool additive = false);
		void Deselect(UUID entity);
		void ClearSelection();
		// Constant time, also for large selections.
		bool IsSelected(UUID entity) const;
		// The most recently selected entity that still exists.
		Entity GetPrimarySelection() const;
		// Drops selected entities that no longer exist in the active scene.
		void PruneSelection();

		//////////////////////////////////////////////////////////////////////////
		// Inspection
		//////////////////////////////////////////////////////////////////////////

		// Shows an asset in the asset inspector instead of the selected entities, which stay selected (the viewport keeps
		// their outline and gizmo). Selecting an entity (Select, or SetSelection with entities) makes the entities the target
		// again, as does the null handle; an asset that is deleted, or the project closing, ends the inspection. Returns false
		// (changing nothing) for handles the active asset manager does not know.
		bool SetInspectedAsset(AssetHandle handle);
		// The null handle while the entities are inspected.
		AssetHandle GetInspectedAsset() const { return m_InspectedAsset; }
		InspectionTarget GetInspectionTarget() const { return m_InspectedAsset.IsValid() ? InspectionTarget::Asset : InspectionTarget::Entities; }
		// Changes whenever the target changes and whenever an asset is inspected (again), so panels can bring the inspector
		// that shows it to the front.
		uint64_t GetInspectionVersion() const { return m_InspectionVersion; }

		//////////////////////////////////////////////////////////////////////////
		// Undo
		//////////////////////////////////////////////////////////////////////////

		UndoStack& GetUndoStack() { return m_UndoStack; }
		// The prefab instances of the edited scene and their prefabs (updates after prefab changes, modified marks).
		PrefabSync& GetPrefabSync() { return m_PrefabSync; }
		// Thumbnails of the open project's assets (Content Browser, Asset tab); null without a project or a GPU.
		ThumbnailCache* GetThumbnails() const { return m_Thumbnails.get(); }
		// The project's picture for the launcher, made after each scene save.
		const ProjectThumbnail& GetProjectThumbnail() const { return m_ProjectThumbnail; }
		// Records an edit of the active scene. Edits of the running copy during play are not recorded: they are
		// discarded with it. Returns true when an undo step was recorded. While a scene is being opened (IsSceneLoading) the
		// edited scene is read-only: the edit is rolled back and nothing is recorded.
		bool CommitEdit(SceneEditTransaction& transaction);
		// Undo and redo of the history (UndoStack::Undo, Redo): false while playing, with nothing to do, or when the step
		// fails (the reason in outError).
		bool Undo(std::string* outError = nullptr);
		bool Redo(std::string* outError = nullptr);

		//////////////////////////////////////////////////////////////////////////
		// Activity
		//////////////////////////////////////////////////////////////////////////

		// The activity journal: every command run in this editor (EditorCommandRegistry::Execute records them), by the
		// person, agents, scripts or the editor itself (editor.activity, the Activity panel).
		CommandJournal& GetJournal() { return *m_Journal; }
		const CommandJournal& GetJournal() const { return *m_Journal; }
		// Shared with pending commands, which complete their entries even when they are cancelled after the context went.
		const Ref<CommandJournal>& GetJournalRef() const { return m_Journal; }

		//////////////////////////////////////////////////////////////////////////
		// Recovery
		//////////////////////////////////////////////////////////////////////////

		// Autosave and crash recovery (SceneRecovery): the journal of the edited scene's unsaved changes, written in the
		// background into the project's ".strata/Recovery", and the recovery files found there when the project opened (a
		// crash, or changes discarded on purpose). Every replacement of the edited scene with unsaved changes writes them
		// there first, so nothing is lost even then.
		SceneRecovery& GetRecovery() { return m_Recovery; }
		const SceneRecovery& GetRecovery() const { return m_Recovery; }
		// What the person should hear about (the UI's toasts): failed or warning UI commands, script builds and crashes,
		// finished exports, recovery data on offer.
		EditorNotifications& GetNotifications() { return m_Notifications; }
		const EditorNotifications& GetNotifications() const { return m_Notifications; }
		// Posts that the command running now moved `path` (an asset, or a folder with `assets` assets) to the project's trash,
		// when an agent or a command script runs it: the person did not see it go. It names who did it and that undo
		// restores it, as a warning when references to it were left missing (`force`). The person's own deletes post nothing.
		void NotifyAssetsTrashed(const std::string& path, bool folder, size_t assets, bool referencesLeft);
		// The file of a scene asset of the project (absolute) and its path relative to the asset directory; empty for the
		// null handle or without a project.
		std::pair<std::filesystem::path, std::string> GetSceneFile(AssetHandle scene) const;
		// Starts applying recovery data (SceneRecovery::Read, scene.recover): unless the edited scene is the data's scene without
		// unsaved changes, opens that scene in the background (BeginOpenScene; *outSceneLoad is the load's ID, to wait for in
		// GetSceneLoadStatus), or makes a new scene of its name for a scene never saved (*outSceneLoad 0: ApplyRecovery may
		// follow at once). Fails while playing, while a scene is being opened (a pending open would replace what is recovered)
		// and while the edited scene has unsaved changes that this would discard, unless `force`.
		bool BeginRecovery(const RecoveryData& data, bool force, uint64_t* outSceneLoad, std::string* outError = nullptr);
		// Turns the edited scene, which BeginRecovery made the data's scene as saved (or a new one), into what the recovered
		// edits left, as one undo step back to the state before it. The scene stays modified (unsaved). Fails when the edited
		// scene is not that scene any more (another replaced it, or is being opened). outApplied: the entities it touched.
		bool ApplyRecovery(const RecoveryData& data, size_t* outApplied = nullptr, std::string* outError = nullptr);

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
		// Whether the open project has a script build, a CMakeLists.txt in its script directory, that BuildScripts can run
		// (false without a project). Looks at the file system.
		bool HasScriptBuild() const;
		// The toolchain script builds use: EditorContextSpecification::ScriptBuild or, where those are only preferences
		// (ScriptBuildSettings::FindToolchain), the toolchain a build found for them on this machine once one did.
		const ScriptBuildSettings& GetScriptBuildSettings() const;
		// EditorContextSpecification::ScriptBuild, what builds start from.
		const ScriptBuildSettings& GetScriptBuildPreferences() const { return m_Specification.ScriptBuild; }
		const ScriptBuilder& GetScriptBuilder() const { return m_ScriptBuilder; }
		// Ends a running script build (it finishes as failed, "cancelled"); a play waiting for it then fails.
		void CancelScriptBuild();
		const ScriptBuildLoad& GetLastScriptBuildLoad() const { return m_LastScriptBuildLoad; }
		// Whether a scene or prefab of the project attaches a script: the edited scene, or one of the project's saved scenes
		// and prefabs (their files are scanned on an I/O thread when the project opens; until the scan finished, only the
		// edited scene counts). Projects whose scenes use no script are not asked to build theirs.
		bool DoScenesUseScripts() const;
		// Why play mode has to build the scripts first, or nothing when it does not: the project's module is not loaded
		// although scenes use scripts (never built, or built by another engine), or its sources (code and CMake files in the
		// script directory, StampScriptSources) changed since it was built. While the module is the one the editor's last
		// successful build left, that compares the sources with what that build compiled, file by file (a build that changed
		// nothing in the module, a file the module does not compile, a deleted or renamed file); for a module built
		// elsewhere (an IDE, an earlier session) it asks whether a source is newer than the module file. Never for a module
		// loaded from elsewhere (script.load) or a project without a script build. Reads the script directory.
		std::optional<std::string> GetPlayBuildReason() const;
		// Starts the build play mode waits for (BuildScripts); GetPlayBuildID names it until ClearPlayBuild.
		bool StartPlayBuild(std::string* outError = nullptr);
		uint64_t GetPlayBuildID() const { return m_PlayBuildID; }
		void ClearPlayBuild() { m_PlayBuildID = 0; }
		// The person's code editor (script.open, script.setExternalEditor).
		ExternalEditorLauncher& GetExternalEditor() { return m_ExternalEditor; }
		const ExternalEditorLauncher& GetExternalEditor() const { return m_ExternalEditor; }
		// Opens a source file at a line and column (0: unknown) in the external editor: a path relative to the project
		// directory, or absolute inside the project or the engine (whose SDK headers diagnostics name). Only code, CMake and
		// text files; an empty path opens the project's script directory. Fails (with the reason) for anything else or a
		// file that does not exist. On success outHow says what opened it.
		bool OpenScriptSource(const std::filesystem::path& file, uint32_t line, uint32_t column, std::string* outHow = nullptr, std::string* outError = nullptr);
		// Asks the UI to show the Problems panel (a play's build failed): the version changes with every request.
		void RequestProblemsPanel() { m_ProblemsPanelRequests++; }
		uint64_t GetProblemsPanelRequests() const { return m_ProblemsPanelRequests; }
		// The script crash that stopped play mode last; cleared when a module loads.
		const std::optional<ScriptFault>& GetLastScriptFault() const { return m_LastScriptFault; }
		// Reads the loaded module's file and checks that it is still the file that was loaded, so that exports ship the
		// scripts the editor runs. Fails while a script build runs (it may be writing the file) and when the file changed
		// since it was loaded (e.g. a build whose module could not be loaded). Without a loaded module it succeeds with an
		// empty path.
		bool ReadRunningScriptModule(ScriptModuleFile& outFile, std::string* outError = nullptr) const;

		//////////////////////////////////////////////////////////////////////////
		// Export
		//////////////////////////////////////////////////////////////////////////

		// Starts exporting the open project as a playable game in the background (GameExport::Start, which says what is
		// checked at once); Update advances it. One export at a time: fails while one runs. Closing the project or the editor
		// cancels it.
		bool StartGameExport(const GameExportOptions& options, std::string* outError = nullptr);
		// The running export, or the last one that finished (null before the first).
		const Ref<GameExport>& GetGameExport() const { return m_GameExport; }

		//////////////////////////////////////////////////////////////////////////
		// Viewport
		//////////////////////////////////////////////////////////////////////////

		// The editor camera, viewport settings and viewport rendering. Its state is saved per project in
		// "<project>/.strata/EditorViewport.json" when the project closes and restored when it opens.
		EditorViewport& GetViewport() { return m_Viewport; }
		const EditorViewport& GetViewport() const { return m_Viewport; }

		// Once per frame: script hot reload and builds, asset hot reload and loading, a running export, then the scene update
		// (simulation while playing) and the running game's requests, then finished viewport picks. A script crash while
		// playing stops play mode.
		void Update(Timestep timestep);

		//////////////////////////////////////////////////////////////////////////
		// Editor services
		//////////////////////////////////////////////////////////////////////////

		// The projects opened most recently (editor.recentProjects, the launcher).
		RecentProjects& GetRecentProjects() { return m_RecentProjects; }
		// The directory of the samples the editor offers (EditorContextSpecification::SamplesDirectory).
		std::filesystem::path GetSamplesDirectory() const;

		// Asks the editor to close after the current frame (editor.quit); the application layer polls the request.
		void RequestQuit() { m_QuitRequested = true; }
		bool IsQuitRequested() const { return m_QuitRequested; }

		// The CPU time of the editor's frames (EditorHost::GetLastFrameWorkTime: the application's, or a UI test's fake
		// host), recorded once per frame for the frame before by the EditorLayer; editor.wait reports statistics of the
		// frames it waited for. Contexts no EditorLayer drives (e.g. command tests) record nothing.
		void RecordFrameTime(double milliseconds)
		{
			m_LastFrameTime = milliseconds;
			m_RecordedFrameCount++;
		}
		uint64_t GetRecordedFrameCount() const { return m_RecordedFrameCount; }
		// The latest recorded frame's time in milliseconds (0 before the first).
		double GetLastFrameTime() const { return m_LastFrameTime; }

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
		// Suspends input frames while the running game is paused without a step to run (see GetSimulatedInput).
		void UpdateInputSuspension();
		// Drops the simulated device's state and the commands' holds (play mode starts or stops).
		void ResetSimulatedInput();
		// Forgets the game's window requests and frees the cursor it may have locked (play mode starts or stops).
		void ResetGameShell();
		// Starts replacing the running scene with a scene asset (loaded while the running scene goes on, swapped in by
		// UpdateRuntimeSceneLoad), or restarts the running scene at once for the null handle when a copy of the edited scene
		// runs.
		bool SwitchRuntimeScene(AssetHandle scene, std::string* outError);
		// Advances the play-mode scene switch and swaps the loaded scene in once it is ready (Update).
		void UpdateRuntimeSceneLoad();
		// Replaces the running scene with `next`, which continues the way play mode was going (paused, with its steps); what
		// the previous scene used goes a few frames later, unless `next` requested it by then or its load brought it in
		// (`loaded`).
		void ReplaceRuntimeScene(Ref<Scene> next, AssetHandle asset, std::span<const AssetHandle> loaded);
		// Advances the scene being opened and makes it the edited scene once it is ready (Update).
		void UpdateSceneLoad();
		// SceneLoadOptions::Prepare of the editor's scene loads (opened and switched to in play mode): the loaded scene's
		// render proxies while the editor renders (RenderScene::PrepareLoadedScene), nothing without a GPU.
		static std::function<bool(Scene&)> GetScenePreparation();
		// Makes a scene that was opened the edited one: the editor camera, the trim of what the previous scene used (sparing
		// what the load brought in, `loaded`).
		void FinishOpenScene(Ref<Scene> scene, AssetHandle handle, std::span<const AssetHandle> loaded);
		// Where the unsaved changes a scene replacement discards end up, for the notification that tells the person.
		enum class DiscardNotice : uint8_t
		{
			Recoverable = 0, // The project stays open: the changes are offered at once (Recover...)
			ProjectCloses,   // The project closes: they are offered when it opens again
			None             // The editor itself goes away: nobody to tell
		};
		// Replaces the edited scene. Unsaved changes of the scene that goes are written as recovery data first, and the
		// person hears that they were discarded and by whom (NotifyDiscardedChanges).
		void ResetScene(Ref<Scene> scene, AssetHandle handle, DiscardNotice notice = DiscardNotice::Recoverable);
		// Posts that the unsaved changes of `sceneName` were discarded, naming who did it (the command running now: the
		// person, an agent, a command script) and where they are kept.
		void NotifyDiscardedChanges(const std::string& sceneName, DiscardOutcome outcome, const std::string& reason, DiscardNotice notice);
		// Gives a template's new scene its start scene file and the template's editor view (CreateProject).
		bool SaveTemplateStartScene(std::string_view templateId, std::string* outError);
		// The assets a template's scene uses in this project (NewScene).
		TemplateAssets GetTemplateAssets(std::string_view templateId);
		// Frames the scene again once the meshes that were loading when it was first framed have loaded (OpenScene).
		void UpdatePendingFrame();
		// Scenes moved (asset.move, renames): the recovery journal follows the edited scene to its new place, and recovery
		// entries of a scene that moved are read again. Looks only when the asset manager's metadata changed.
		void FollowSceneMoves();
		void OpenScriptEngine(bool created);
		// The open project's thumbnail cache (with a GPU), kept in its intermediate directory.
		void OpenThumbnails();
		// After the edited scene was saved: the project's picture for the launcher is made again.
		void OnSceneSaved();
		void CloseScriptEngine();
		// Fingerprints the loaded module's file after a load. `expected` is its digest from before the load (if known):
		// a file that changed while it was being loaded leaves the running version unknown.
		void RecordScriptModuleFile(const std::optional<Sha256Digest>& expected);
		void OnScriptBuildFinished();
		// Takes the location of the last script crash once its symbolization finished, and reports it.
		void UpdateScriptFaultLocation();
		// Scans the project's saved scenes and prefabs for scripts on an I/O thread (see DoScenesUseScripts).
		void StartScriptUsageScan();
		// Takes the finished scan's answer: a project whose scenes use scripts that are not built is told so once.
		void UpdateScriptUsageScan();
		// Posts how the game export ended (EditorNotifications).
		void NotifyExportFinished(const GameExport& exporter);
		// Posts the imports that failed while nobody waited for them (EditorAssetManager::TakeImportFailures): one toast for
		// all of them, showing the latest.
		void NotifyImportFailures();
		// Stops play mode (and reports the fault) when the script module crashed. Returns true if it did.
		bool StopOnScriptFault();
		// Saves the viewport state and closes the project; with activateBuiltinAssets the built-in asset manager becomes active.
		void ReleaseProject(bool activateBuiltinAssets);
		// The entities become the inspection target (an entity was selected, the inspected asset went away).
		void InspectEntities();
		void ActivateBuiltinAssets();
		std::filesystem::path GetViewportStateFile() const;
	private:
		EditorContextSpecification m_Specification;
		Ref<Project> m_Project;
		Ref<EditorAssetManager> m_AssetManager;
		uint64_t m_ProjectGeneration = 0;

		Ref<Scene> m_EditScene;
		Scope<AnimationPreview> m_AnimationPreview; // Of m_EditScene: declared after it, so it goes first
		Ref<Scene> m_RuntimeScene;
		AssetHandle m_RuntimeSceneAsset = UUID::Null(); // The scene asset playing after a switch; null while a copy of the edited scene runs
		// The scene a play-mode switch loads while the running scene goes on.
		struct RuntimeSceneLoad
		{
			AssetHandle Scene = UUID::Null();
			SceneLoadRequest Request;
		};
		Scope<RuntimeSceneLoad> m_RuntimeSceneLoad;
		// The scene being opened for editing (BeginOpenScene).
		struct SceneLoad
		{
			SceneLoadRequest Request;
			// The journal entry of the command that started it (scene.open, project.open, scene.recover; 0 for none): the
			// replacement happens between frames, when no command runs, and is that command's work (who discarded unsaved
			// changes, the undo step of the prefab catch-up).
			uint64_t RequestedBy = 0;
		};
		Scope<SceneLoad> m_SceneLoad;
		EditorSceneLoadStatus m_SceneLoadStatus;
		uint64_t m_NextSceneLoadID = 1;
		AssetHandle m_SceneHandle = UUID::Null();
		uint64_t m_SceneMetadataVersion = UINT64_MAX; // The asset manager's metadata version FollowSceneMoves looked at last
		SceneState m_SceneState = SceneState::Edit;
		float m_MasterVolumeBeforePlay = 1.0f;
		bool m_GameInputActive = false;
		SimulatedInput m_SimulatedInput;
		// The game's shell (see GetGameWindow): the services and, declared after them so that they end first, their providers.
		class ProjectGameSaves final : public IGameSaves
		{
		public:
			explicit ProjectGameSaves(const EditorContext& context)
				: m_Context(context)
			{
			}

			std::optional<std::filesystem::path> GetSaveDirectory() override { return m_Context.GetGameSaveDirectory(); }
		private:
			const EditorContext& m_Context;
		};
		EditorGameWindow m_GameWindow;
		ProjectGameSaves m_GameSaves { *this };
		Scope<EngineServices::ScopedProvider<IGameWindow>> m_GameWindowProvider;
		Scope<EngineServices::ScopedProvider<IGameSaves>> m_GameSavesProvider;

		std::vector<UUID> m_Selection;
		std::unordered_set<UUID> m_SelectionSet; // The same entities, for IsSelected
		AssetHandle m_InspectedAsset = UUID::Null();
		uint64_t m_InspectionVersion = 0;
		Ref<CommandJournal> m_Journal = CreateRef<CommandJournal>();
		// Declared before the undo stack it observes, so the stack never outlives it.
		SceneRecovery m_Recovery;
		UndoStack m_UndoStack;
		PrefabSync m_PrefabSync;
		Scope<ThumbnailCache> m_Thumbnails;
		ProjectThumbnail m_ProjectThumbnail;

		Ref<ScriptEngine> m_ScriptEngine;
		ScriptBuilder m_ScriptBuilder;
		Ref<GameExport> m_GameExport;
		bool m_GameExportNotified = false; // Its outcome was posted (EditorNotifications, NotifyExportFinished)
		EditorNotifications m_Notifications;
		ScriptBuildLoad m_LastScriptBuildLoad;
		std::optional<ScriptFault> m_LastScriptFault;
		uint64_t m_PlayBuildID = 0;
		// The sources the running build compiles (taken when it started), and those the last successful build compiled with
		// the module file it left (another write time or size: someone else built it since). See GetPlayBuildReason.
		struct BuiltScriptSources
		{
			ScriptSourceListing Sources;
			int64_t ModuleTime = 0;
			uint64_t ModuleSize = 0;
		};
		std::optional<ScriptSourceListing> m_BuildingScriptSources;
		uint64_t m_BuildingScriptSourcesID = 0;
		std::optional<BuiltScriptSources> m_BuiltScriptSources;
		uint64_t m_ProblemsPanelRequests = 0;
		bool m_ScriptFaultLocationPending = false; // m_LastScriptFault waits for its symbolization
		ExternalEditorLauncher m_ExternalEditor;
		// The scan of the project's scene and prefab files for scripts (see DoScenesUseScripts), shared with its I/O job.
		struct ScriptUsageScan
		{
			std::atomic<bool> Cancelled = false;
			std::atomic<bool> Done = false;
			bool UsesScripts = false; // Written by the job before Done
			std::string File;         // The first file found to attach scripts
		};
		Ref<ScriptUsageScan> m_ScriptUsageScan;
		bool m_SavedScenesUseScripts = false; // The finished scan's answer
		bool m_ScriptUsageReported = false;
		// The loaded module's file as it was loaded (see ReadRunningScriptModule), and the load it belongs to.
		std::optional<Sha256Digest> m_ScriptModuleDigest;
		uint64_t m_ScriptModuleLoadCount = 0;
		Ref<AssetManagerBase> m_BuiltinAssets; // Active while no project is open
		EditorViewport m_Viewport;
		// A first view of a scene that was framed while some of its meshes were still loading.
		struct PendingFrame
		{
			AssetHandle Scene = UUID::Null();
			std::vector<AssetHandle> Meshes;
			EditorCamera FramedCamera;
			std::chrono::steady_clock::time_point Deadline;
		};
		std::optional<PendingFrame> m_PendingFrame;
		RecentProjects m_RecentProjects;

		bool m_QuitRequested = false;
		uint64_t m_RecordedFrameCount = 0;
		double m_LastFrameTime = 0.0;
		std::map<std::string, StatusProvider> m_StatusProviders;
	};

}
