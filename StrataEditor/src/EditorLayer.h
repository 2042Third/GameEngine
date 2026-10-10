#pragma once

#include "Editor/EditorAutomation.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "EditorHost.h"
#include "UI/AboutDialog.h"
#include "UI/EditorPanelRegistry.h"
#include "UI/EditorShell.h"
#include "UI/ProjectDialogs.h"

#include <Strata.h>

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
		// Watch the project's assets and script module for changes made outside the editor (hot reload). UI tests turn it off:
		// they need no watcher threads.
		bool WatchFiles = true;
		// The toolchain script builds use instead of the engine's (EditorContextSpecification::ScriptBuild); UI tests build
		// with a stand-in for CMake.
		std::optional<ScriptBuildSettings> ScriptBuild;
	};

	// Built-in panel ids (EditorPanelRegistry).
	namespace EditorPanels
	{
		constexpr const char* c_Viewport = "Viewport";
		constexpr const char* c_Hierarchy = "Hierarchy";
		constexpr const char* c_Inspector = "Inspector";
		constexpr const char* c_ContentBrowser = "ContentBrowser";
		constexpr const char* c_Console = "Console";
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
		// Version of the default layout: a saved layout of another version is replaced by the default one.
		static constexpr int c_LayoutVersion = 2;

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

		// EditorShell
		void RequestDiscardChanges(std::function<void()> action) override;
		void ShowNewProjectDialog(const std::string& templateId) override;
		void ShowOpenSampleDialog(const std::string& sampleId) override;
		void ShowOpenProjectDialog() override;
		void ShowAboutDialog() override;
		void DismissLauncher() override;
		EditorEnvironment GetEnvironment() const override;
		EditorAutomationState GetAutomationState() const override;
	private:
		void StartAutomation();
		void RegisterBuiltinPanels();
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
		// The status bar's pills of automation and scripts; true when the pill was clicked to show the Console.
		bool DrawAutomationPill();
		bool DrawScriptsPill();
		void DrawUnsavedChangesModal();
		void HandleShortcuts();
		void UpdateWindowTitle();
		// Advances the startup command script; once it finished, reports the result (a failed script fails the process).
		void UpdateCommandScript();
		// Notes input that arrived this frame (ImGui's input events, held keys and buttons).
		void TrackInput();
		// Chooses the frame rate for the next frames (idle throttling) and records frame times.
		void UpdateFrameRate();
		bool IsBusy(double now) const;
		// Fills m_FrameStats for this frame's status bar and panels.
		void UpdateFrameStats();
		// Whether the open project has a script build (EditorContext::HasScriptBuild), looked up at most once per
		// c_ScriptBuildCheckSeconds.
		bool HasScriptBuild();

		bool SaveScene();
		bool SaveSceneAs();
		// Opens a project file (project.open) once unsaved changes are dealt with; failures go to ReportError.
		void OpenProjectFile(const std::filesystem::path& projectFile);
		// Logs an error of a UI action, and shows it on the launcher while that is what the user sees.
		void ReportError(const std::string& message);
		void DeleteSelection();
		void DuplicateSelection();
		// Builds the project's scripts through the command runner (script.build); the build reports to the log.
		void BuildScripts();
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
		EditorPanelRegistry m_Panels;
		ProjectDialogs m_ProjectDialogs;
		AboutDialog m_AboutDialog;

		std::function<void()> m_PendingDiscardAction;
		bool m_OpenUnsavedChangesModal = false;
		bool m_ShowImGuiDemo = false;
		bool m_ResetLayout = false;
		bool m_LayoutChecked = false;
		bool m_UIDrawn = false; // OnImGuiRender ran at least once
		std::string m_WindowTitle;
		std::string m_AutomationError; // Why automation did not start (when it was asked for)
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
		bool m_Idle = false;
		std::deque<double> m_FrameTimes; // Within the last c_FrameRateWindowSeconds

		// Frame timing.
		std::array<double, c_FrameWorkSamples> m_FrameWorkTimes = {}; // Seconds, a ring of the last frames' work times
		size_t m_FrameWorkCount = 0;
		size_t m_NextFrameWork = 0;
		EditorFrameStats m_FrameStats;
	};

}
