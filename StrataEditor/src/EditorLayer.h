#pragma once

#include "Editor/EditorAutomation.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "EditorHost.h"
#include "UI/AboutDialog.h"
#include "UI/EditorPanelRegistry.h"
#include "UI/EditorShell.h"
#include "UI/ExportDialog.h"
#include "UI/PreferencesDialog.h"
#include "UI/ProjectDialogs.h"
#include "UI/RecoveryDialog.h"
#include "UI/ScriptDialogs.h"
#include "UI/SettingsPages.h"

#include <Strata/Core/Base.h>
#include <Strata/Core/Layer.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Core/UUID.h>
#include <Strata/Events/Event.h>

#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Strata
{

	struct EditorOptions
	{
		std::filesystem::path ProjectPath;
		std::filesystem::path ScreenshotPath; // Saves the editor window to this PNG on the last frame (with --frames)
		// JSON array of {"command": name, "parameters": {...}} run after startup (after opening ProjectPath), see
		// EditorCommandScript. With MaxFrames, a script that has not finished by the last frame fails the run.
		std::filesystem::path CommandScript;
		std::optional<uint64_t> MaxFrames;
		bool QuitAfterCommands = false; // Close the editor once the command script finished (e.g. after a script build)
		bool ShowImGuiDemo = false;     // Offer ImGui's demo window (Help menu), for UI work only (--imgui-demo)
		bool Headless = false; // No UI: the editor runs for automation only
		// A fixed UI scale (--ui-scale) instead of the window's content scale (DPI); unset follows the window.
		std::optional<float> UIScale;
		// Serve the commands to tools and AI agents (EditorAutomation) on loopback, published through a session file.
		bool EnableAutomation = true;
		uint16_t AutomationPort = 0; // 0 picks a free port
		// Close the editor after this long without a connected automation client (0: never), e.g. a headless editor
		// started for an MCP server that went away.
		std::chrono::seconds IdleTimeout = std::chrono::seconds(0);
		// The user's list of recent projects (EditorContextSpecification::RecentProjectsFile): empty keeps it in memory;
		// read-only for scripted runs (--frames, --commands), which are not the user's sessions.
		std::filesystem::path RecentProjectsFile;
		bool RecentProjectsReadOnly = false;
		// The user's preferences, such as the external code editor (EditorContextSpecification::PreferencesFile): empty keeps
		// them in memory; and 0.5.0's editor settings, imported once while the preferences file does not exist
		// (EditorContextSpecification::EditorSettingsFile).
		std::filesystem::path PreferencesFile;
		std::filesystem::path EditorSettingsFile;
		// The settings sections the editor validates and draws (EditorContextSpecification::Settings); null: the process's.
		// Tests pass a local registry.
		const SettingsRegistry* Settings = nullptr;
		// Watch the project's assets and script module for changes made outside the editor (hot reload). UI tests turn it off:
		// they need no watcher threads.
		bool WatchFiles = true;
		// Open projects with their imports in the background (EditorContextSpecification::ImportInBackground), except in
		// scripted runs (--frames, --commands). UI tests turn it off: they expect every asset imported once a project is open.
		bool ImportInBackground = true;
		// Open ProjectPath's start scene in the background (EditorContext::BeginOpenProject: the window shows its load in the
		// viewport; with ImportInBackground the assets are scanned in the background first) instead of before the first frame,
		// so that automation starts at once. The editor application turns it on for sessions that are not scripted (--frames,
		// --commands): those and tests expect the project's scene once the editor started.
		bool OpenStartSceneInBackground = false;
		// The toolchain script builds use instead of the engine's (EditorContextSpecification::ScriptBuild); UI tests build
		// with a stand-in for CMake.
		std::optional<ScriptBuildSettings> ScriptBuild;
		// Where those builds look for a toolchain when they find one (EditorContextSpecification::ToolchainEnvironment).
		std::function<Scope<ScriptToolchainEnvironment>()> ToolchainEnvironment;
	};

	// Built-in panel ids (EditorPanelRegistry).
	namespace EditorPanels
	{
		constexpr const char* c_Viewport = "Viewport"; // The Scene view (ViewportPanel; "Scene" is an alias in editor.showPanel)
		constexpr const char* c_Game = "Game";         // The Game view (GameViewPanel), a tab beside the Scene view
		constexpr const char* c_Hierarchy = "Hierarchy";
		constexpr const char* c_Inspector = "Inspector";
		constexpr const char* c_AssetInspector = "AssetInspector";
		constexpr const char* c_ContentBrowser = "ContentBrowser";
		constexpr const char* c_Console = "Console";
		constexpr const char* c_Streaming = "Streaming";     // StreamingPanel, opened from the status bar's assets pill
		constexpr const char* c_Performance = "Performance"; // PerformancePanel, opened from the status bar's frame pill
		constexpr const char* c_Problems = "Problems";       // ProblemsPanel: script build diagnostics and crashes
		constexpr const char* c_Activity = "Activity";       // ActivityPanel, opened from the viewport's agent chip or the View menu
		constexpr const char* c_ProjectSettings = "ProjectSettings"; // ProjectSettingsPanel, opened from File > Project Settings...
		constexpr const char* c_Welcome = "Welcome"; // The launcher (WelcomePanel)
	}

	// The editor application layer: owns the editor state (EditorContext), the command registry shared with automation,
	// and the ImGui interface: the menu bar, the main toolbar, the panels (EditorPanelRegistry), the status bar and the
	// default layout. It reaches the application only through its EditorHost.
	//
	// While no project is open it shows the launcher (WelcomePanel) under a short menu bar instead of the dock space,
	// until a project opens or the user continues without one (DismissLauncher); closing the project shows it again. The
	// layer is the panels' EditorShell: it owns the dialogs (New Project, Open Sample, About Strata) and asks about unsaved
	// changes before anything replaces the scene.
	//
	// Idle throttling: a windowed editor redraws at the full rate (vsync) while anything happens - input within the last
	// c_InputActivitySeconds, a camera or gizmo drag, a running and unpaused scene (or pending steps), loading assets,
	// pending commands, an automation request within the last c_AutomationActivitySeconds, or a script build - and
	// otherwise at c_IdleFrameRate (c_UnfocusedIdleFrameRate without the focus). Headless editors, --frames runs and
	// command scripts are never throttled.
	class EditorLayer : public Layer, public EditorShell
	{
	public:
		static constexpr uint32_t c_IdleFrameRate = 30;
		static constexpr uint32_t c_UnfocusedIdleFrameRate = 10;
		static constexpr double c_InputActivitySeconds = 0.5;
		static constexpr double c_AutomationActivitySeconds = 1.0;
		// Version of the default layout: a saved layout of another version is replaced by the default one. Bump it when the
		// default layout gains a docked panel that is open by default (4: the Problems panel, and Activity beside it; 5: the
		// Game view beside the Scene view, a wider Hierarchy), which an older layout would show floating over the viewport.
		static constexpr int c_LayoutVersion = 5;

		EditorLayer(const EditorOptions& options, Scope<EditorHost> host);
		~EditorLayer() override = default;

		void OnAttach() override;
		void OnDetach() override;
		void OnUpdate(Timestep timestep) override;
		void OnImGuiRender() override;
		void OnEvent(Event& event) override;

		EditorContext& GetContext() { return m_Context; }
		const EditorCommandRegistry& GetCommands() const { return m_Commands; }
		EditorCommandRunner& GetCommandRunner() { return m_CommandRunner; }
		EditorPanelRegistry& GetPanels() { return m_Panels; }
		EditorHost& GetHost() { return *m_Host; }
		// Whether idle throttling applies to this editor at all (windowed, no --frames, no command script).
		bool IsThrottlingEnabled() const;
		// Whether the last frame found nothing to do (the frame rate is then lowered).
		bool IsIdle() const { return m_Idle; }
		// Frames per second over the last c_FrameRateWindowSeconds (or since startup).
		double GetAverageFrameRate() const;
		static constexpr double c_FrameRateWindowSeconds = 5.0;
		// The frame timing the status bar and the panels show this frame; the work time is averaged over the last
		// c_FrameWorkSamples frames.
		const EditorFrameStats& GetFrameStats() const { return m_FrameStats; }
		static constexpr size_t c_FrameWorkSamples = 60;
		// How often the status bar looks for a script build to offer while no script module is loaded.
		static constexpr double c_ScriptBuildCheckSeconds = 1.0;
		// Whether this frame shows the launcher instead of the dock space.
		bool IsLauncherShown() const;
		ProjectDialogs& GetProjectDialogs() { return m_ProjectDialogs; }
		const AboutDialog& GetAboutDialog() const { return m_AboutDialog; }
		const ExportDialog& GetExportDialog() const { return m_ExportDialog; }
		NewScriptDialog& GetNewScriptDialog() { return m_NewScriptDialog; }
		PreferencesDialog& GetPreferencesDialog() { return m_PreferencesDialog; }
		// The custom pages of the settings windows (EditorSettingsPageRegistry).
		const EditorSettingsPageRegistry& GetSettingsPages() const { return m_SettingsPages; }
		const RecoveryDialog& GetRecoveryDialog() const { return m_RecoveryDialog; }
		// File > Open Scene... (Ctrl+O) once its file dialog chose `file`: a scene file of the project's asset directory opens
		// (scene.open through the runner) after the question about the edited scene's unsaved changes. False, with the reason
		// logged, without a project, while playing, or for a file outside the asset directory.
		bool OpenSceneFile(const std::filesystem::path& file);

		// EditorShell
		void RequestDiscardChanges(std::function<void()> action) override;
		void RequestReleaseProject(std::function<void()> action) override;
		void RequestQuit(std::function<void()> action) override;
		void DrawDiscardQuestion() override;
		void ShowNewProjectDialog(const std::string& templateId) override;
		void ShowOpenSampleDialog(const std::string& sampleId) override;
		void ShowOpenProjectDialog() override;
		void ShowAboutDialog() override;
		void ShowNewScriptDialog() override;
		void ShowPreferencesDialog(const std::string& section) override;
		void ShowProjectSettings(const std::string& page) override;
		void DismissLauncher() override;
		void RenameEntity(UUID entity) override;
		void ShowPanel(std::string_view panel) override;
		void ShowActivity(uint64_t entry) override;
		void ShowRecoveryDialog() override;
		void ShowAsset(AssetHandle asset) override;
		EditorEnvironment GetEnvironment() const override;
		EditorAutomationState GetAutomationState() const override;
	private:
		void StartAutomation();
		void RegisterBuiltinPanels();
		// editor.showPanel: commands of the editor's window, which only the UI knows.
		void RegisterPanelCommands();
		EditorPanelContext MakePanelContext();
		// The launcher under a menu bar, filling the window (instead of DrawDockspace).
		void DrawLauncher(EditorPanelContext& panelContext);
		void DrawDockspace();
		// Docks the panels into the default arrangement (first run, a saved layout of another version, View > Reset Layout).
		void BuildDefaultLayout(unsigned int dockspaceId);
		void DrawMenuBar();
		// File > Open Recent: the recent projects (RecentProjects) the launcher lists.
		void DrawRecentMenu();
		void DrawSampleMenu();
		void DrawToolbar();
		void DrawGizmoControls();
		void DrawPlayControls();
		void DrawBuildControls();
		void DrawStatusBar();
		// The status bar's pills of the frame time and of assets; clicking them opens the Performance and Streaming panels.
		void DrawFramePill();
		void DrawAssetsPill();
		// "N bodies without collision" while the running game's physics world is at its growth limit with bodies waiting.
		void DrawPhysicsPill();
		// The status bar's pills of automation and scripts; true when the pill was clicked to show the Console.
		bool DrawAutomationPill();
		bool DrawScriptsPill();
		// The text a status bar pill shows: `text`, or `compact` (when given) while the pills' full texts do not fit side by
		// side; counts the full text's width for that decision. Returns one of the two views.
		std::string_view StatusPillText(const char* icon, std::string_view text, std::string_view compact = {});
		// Tool panels (Streaming, Performance) opened in a layout saved before they existed join the Console's dock node, the
		// Project Settings the Scene view's.
		void DockNewToolPanels();
		// The question about unsaved changes (RequestDiscardChanges), at the popup level it is called at: over the whole window,
		// or over the dialog that calls DrawDiscardQuestion. Drawn once per frame, by whichever call comes first.
		void DrawUnsavedChangesModal();
		// The question of RequestReleaseProject about the project's unwritten settings, inside its modal.
		void DrawUnwrittenSettingsQuestion();
		// The question of RequestQuit about the unwritten preferences, inside its modal.
		void DrawUnwrittenPreferencesQuestion();
		// Runs the action that waited for the answer about unwritten settings, or first asks about the scene's changes made
		// while it waited.
		void RunPendingReleaseAction();
		void HandleShortcuts();
		void UpdateWindowTitle();
		// Advances the startup command script; once it finished, reports the result (a failed script fails the process).
		void UpdateCommandScript();
		// --screenshot: the frame being drawn, saved once it is on screen; with closeAfter the editor closes then (the last frame
		// before --quit-after-commands quits). Once per run.
		void TakeScreenshot(bool closeAfter);
		// Notes input that arrived this frame (ImGui's input events, held keys and buttons).
		void TrackInput();
		// Chooses the frame rate for the next frames (idle throttling) and records frame times.
		void UpdateFrameRate();
		// While play.step's steps are pending, frames run unthrottled (each runs one step), headless editors included; the
		// cap in effect before comes back once they ran.
		void UpdateSteppingFrameRate();
		bool IsBusy(double now) const;
		// Fills m_FrameStats for this frame's status bar and panels.
		void UpdateFrameStats();
		// Whether the open project has a script build (EditorContext::HasScriptBuild), looked up at most once per
		// c_ScriptBuildCheckSeconds.
		bool HasScriptBuild();

		// File > Save, Ctrl+S and the unsaved-changes question's Save: scene.save (or, for a scene never saved, scene.saveAs with
		// the path the file dialog gives) through the command runner, since the save writes in the background. `then` runs
		// once the save ended, with whether it succeeded. Returns whether a save started.
		bool SaveScene(std::function<void(bool saved)> then = {});
		bool SaveSceneAs(std::function<void(bool saved)> then = {});
		// The status bar's pill of saves in flight ("Saving 'World1M' 45%"); clicking it opens the Activity panel.
		void DrawSavingPill();
		// File > Open Scene...: the file dialog in the project's asset directory, then OpenSceneFile.
		bool ShowOpenSceneDialog();
		// Opens a project file (project.open) once unsaved changes are dealt with; failures go to ReportError.
		void OpenProjectFile(const std::filesystem::path& projectFile);
		// Runs project.open through the command runner at once (the start scene loads over the next frames).
		void OpenProjectFileNow(const std::filesystem::path& projectFile);
		// Logs an error of a UI action, and shows it on the launcher while that is what the user sees.
		void ReportError(const std::string& message);
		// Builds the project's scripts through the command runner (script.build); the build reports to the log.
		void BuildScripts();
		// Play (the toolbar, Ctrl+P): play.start through the command runner, which builds the scripts first when they need it.
		void StartPlay();
		// play.stop from the UI (the toolbar, Ctrl+P): brings the Scene view back when Play brought the Game view forward.
		void StopPlay();
		// File > Export Game... and the toolbar's Export button: the Export dialog, showing a running export.
		void ShowExportDialog();
		// Scripts > External Editor... and editor.showDialog: the program that opens script files, and the editors found.
	private:
		EditorOptions m_Options;
		Scope<EditorHost> m_Host;
		EditorContext m_Context;
		EditorCommandRegistry m_Commands;
		// Declared before the runner: cancelling pending commands when the runner is destroyed calls the script's
		// completions, so the script must outlive it.
		Scope<EditorCommandScript> m_CommandScript;
		EditorCommandRunner m_CommandRunner;
		// Declared after what it serves, so it stops before they go away.
		EditorAutomation m_Automation;
		// Declared before the panels that draw its pages.
		EditorSettingsPageRegistry m_SettingsPages;
		EditorPanelRegistry m_Panels;
		ProjectDialogs m_ProjectDialogs;
		AboutDialog m_AboutDialog;
		// Completions of its exports refer to it: OnDetach cancels the runner's commands while it still exists.
		ExportDialog m_ExportDialog;
		NewScriptDialog m_NewScriptDialog;
		PreferencesDialog m_PreferencesDialog;
		// The context's requests for the Problems panel this layer has shown (EditorContext::GetProblemsPanelRequests).
		uint64_t m_ShownProblemsRequests = 0;
		// Like the Export dialog, its completions refer to it.
		RecoveryDialog m_RecoveryDialog;
		bool m_ScreenshotRequested = false;
		bool m_RecoveryDialogWasOpen = false; // Closed with an offer left: a toast says where to find it
		float m_StatusBarHeight = 0.0f;       // Of the last frame, which the toasts stay above
		bool m_StatusBarCompact = false;      // The pills show their compact texts (StatusPillText)
		float m_StatusBarFullWidth = 0.0f;    // What the pills' full texts take side by side, summed as they are drawn

		std::function<void()> m_PendingDiscardAction;
		// An action that replaces the edited scene (RequestDiscardChanges) waiting for the saves in flight, while the saving
		// dialog shows their progress; it asks about what is still unsaved once they are written.
		std::function<void()> m_ActionAfterSaves;
		bool m_OpenSavingModal = false;
		// The undo stack's version (UndoStack::GetVersion) the question was asked for: Don't Save discards those changes
		// only, so the question stays when the scene changed meanwhile (an agent or a script edited it).
		uint64_t m_DiscardQuestionVersion = 0;
		bool m_SceneChangedSinceQuestion = false;
		bool m_OpenUnsavedChangesModal = false;
		// An action that releases the project (RequestReleaseProject) waiting for the answer about the project's unwritten
		// settings, and whether the question's Retry (project.save) is writing.
		std::function<void()> m_PendingReleaseAction;
		uint64_t m_ReleaseQuestionVersion = 0; // The undo stack's version when that question was asked
		bool m_OpenUnwrittenSettingsModal = false;
		bool m_RetryingProjectWrite = false;
		// A quit (RequestQuit) waiting for the answer about the unwritten preferences, and whether the question's Retry
		// (preferences.save) is writing.
		std::function<void()> m_PendingQuitAction;
		uint64_t m_QuitQuestionVersion = 0; // The undo stack's version when that question was asked
		bool m_OpenUnwrittenPreferencesModal = false;
		bool m_RetryingPreferencesWrite = false;
		int m_UnsavedChangesModalFrame = -1; // ImGui's frame the question was drawn in last
		bool m_ShowImGuiDemo = false;
		bool m_ResetLayout = false;
		// Play (the toolbar, Ctrl+P) brought the Game view forward: Stop brings the Scene view back.
		bool m_PlayShowedGame = false;
		bool m_LayoutChecked = false;
		bool m_UIDrawn = false; // OnImGuiRender ran at least once
		// The inspection target the inspector tabs show (EditorContext::GetInspectionVersion): a new one brings its tab forward.
		uint64_t m_ShownInspectionVersion = 0;
		std::string m_WindowTitle;
		std::string m_AutomationError; // Why automation did not start (when it was asked for)
		// The automation pill's text, made again only when the connected clients change (EditorAutomation::GetClientsVersion).
		std::string m_AutomationPillText;
		std::string m_AutomationPillCompactText;
		std::vector<RpcClientInfo> m_AutomationClients;
		uint64_t m_AutomationPillVersion = UINT64_MAX;
		uint16_t m_AutomationPillPort = 0;
		bool m_HasScriptBuild = false;
		std::optional<double> m_ScriptBuildCheckTime;
		bool m_LauncherDismissed = false; // Continue without a project (until a project opens)
		std::filesystem::path m_CLIExecutable;
		// The samples File > Open Sample offers (project.samples), read when the menu first opens.
		std::vector<std::pair<std::string, std::string>> m_SampleMenu;
		bool m_SampleMenuRead = false;

		// Startup: the process's age once its first frame was on screen (EditorHost::GetProcessUptime).
		bool m_StartupMeasured = false;
		std::optional<double> m_StartupSeconds;

		// Idle throttling.
		double m_LastInputTime = 0.0;
		double m_LastAutomationTime = 0.0;
		uint64_t m_LastAutomationRequests = 0;
		std::optional<uint32_t> m_CapBeforeSteps; // The frame rate cap that pending steps lifted (UpdateSteppingFrameRate)
		bool m_Idle = false;
		std::deque<double> m_FrameTimes; // Within the last c_FrameRateWindowSeconds

		// Frame timing.
		std::array<double, c_FrameWorkSamples> m_FrameWorkTimes = {}; // Seconds, a ring of the last frames' work times
		size_t m_FrameWorkCount = 0;
		size_t m_NextFrameWork = 0;
		EditorFrameStats m_FrameStats;
	};

}
