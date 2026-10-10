#pragma once

#include "Editor/EditorAutomation.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "EditorHost.h"
#include "Panels/ConsolePanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/InspectorPanel.h"
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/ViewportPanel.h"

#include <Strata.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

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
		bool ShowImGuiDemo = false;
		bool Headless = false; // No UI: the editor runs for automation only
		// A fixed UI scale (--ui-scale) instead of the window's content scale (DPI); unset follows the window.
		std::optional<float> UIScale;
		// Serve the commands to tools and AI agents (EditorAutomation) on loopback, published through a session file.
		bool EnableAutomation = true;
		uint16_t AutomationPort = 0; // 0 picks a free port
		// Close the editor after this long without a connected automation client (0: never), e.g. a headless editor
		// started for an MCP server that went away.
		std::chrono::seconds IdleTimeout = std::chrono::seconds(0);
		// Watch the project's assets and script module for changes made outside the editor (hot reload). UI tests turn it off:
		// they need no watcher threads.
		bool WatchFiles = true;
	};

	// The editor application layer: owns the editor state (EditorContext), the command registry shared with automation,
	// and the ImGui interface (menus, toolbar, panels). It reaches the application only through its EditorHost.
	class EditorLayer : public Layer
	{
	public:
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
		EditorHost& GetHost() { return *m_Host; }
	private:
		void StartAutomation();
		void DrawDockspace();
		// Docks the panels into the default arrangement (first run, or Window > Reset Layout).
		void BuildDefaultLayout(unsigned int dockspaceId);
		void DrawMenuBar();
		void DrawToolbar();
		void DrawStatusBar();
		void DrawUnsavedChangesModal();
		void HandleShortcuts();
		void UpdateWindowTitle();
		// Advances the startup command script; once it finished, reports the result (a failed script fails the process).
		void UpdateCommandScript();

		// Runs an action that replaces the edited scene, asking first whether unsaved changes should be saved.
		void RequestDiscardChanges(std::function<void()> action);
		bool SaveScene();
		bool SaveSceneAs();
		void NewProject();
		void OpenProject();
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

		SceneHierarchyPanel m_Hierarchy;
		InspectorPanel m_Inspector;
		ContentBrowserPanel m_ContentBrowser;
		ConsolePanel m_Console;
		ViewportPanel m_Viewport;

		std::function<void()> m_PendingDiscardAction;
		bool m_OpenUnsavedChangesModal = false;
		bool m_ShowImGuiDemo = false;
		bool m_ResetLayout = false;
		bool m_LayoutChecked = false;
		std::string m_WindowTitle;
	};

}
