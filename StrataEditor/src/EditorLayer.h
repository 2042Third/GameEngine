#pragma once

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Panels/ConsolePanel.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/InspectorPanel.h"
#include "Panels/SceneHierarchyPanel.h"

#include <Strata.h>

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
		// JSON array of {"command": name, "parameters": {...}} run after startup (after opening ProjectPath).
		std::filesystem::path CommandScript;
		std::optional<uint64_t> MaxFrames;
		bool ShowImGuiDemo = false;
		bool Headless = false; // No UI: the editor runs for automation only
	};

	// The editor application layer: owns the editor state (EditorContext), the command registry shared with automation,
	// and the ImGui interface (menus, toolbar, panels).
	class EditorLayer : public Layer
	{
	public:
		explicit EditorLayer(const EditorOptions& options);
		~EditorLayer() override = default;

		void OnAttach() override;
		void OnDetach() override;
		void OnUpdate(Timestep timestep) override;
		void OnImGuiRender() override;
		void OnEvent(Event& event) override;

		EditorContext& GetContext() { return m_Context; }
		const EditorCommandRegistry& GetCommands() const { return m_Commands; }
	private:
		void DrawDockspace();
		// Docks the panels into the default arrangement (first run, or Window > Reset Layout).
		void BuildDefaultLayout(unsigned int dockspaceId);
		void DrawMenuBar();
		void DrawToolbar();
		void DrawStatusBar();
		void DrawViewport();
		void DrawUnsavedChangesModal();
		void HandleShortcuts();
		void UpdateWindowTitle();
		// Runs the startup command script; returns false if it could not be read or a command failed.
		bool RunCommandScript(const std::filesystem::path& path);

		// Runs an action that replaces the edited scene, asking first whether unsaved changes should be saved.
		void RequestDiscardChanges(std::function<void()> action);
		bool SaveScene();
		bool SaveSceneAs();
		void NewProject();
		void OpenProject();
		void DeleteSelection();
		void DuplicateSelection();
	private:
		EditorOptions m_Options;
		EditorContext m_Context;
		EditorCommandRegistry m_Commands;

		SceneHierarchyPanel m_Hierarchy;
		InspectorPanel m_Inspector;
		ContentBrowserPanel m_ContentBrowser;
		ConsolePanel m_Console;

		std::function<void()> m_PendingDiscardAction;
		bool m_OpenUnsavedChangesModal = false;
		bool m_ShowImGuiDemo = false;
		bool m_ResetLayout = false;
		bool m_LayoutChecked = false;
		std::string m_WindowTitle;
	};

}
