#pragma once

#include "Editor/RecentProjects.h"
#include "UI/EditorPanelRegistry.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Strata
{

	// The launcher: what the editor shows while no project is open (a launcher panel of the EditorPanelRegistry).
	// - The strata mark and the name, and the actions that start work: New Project, Open Project and the samples, through
	//   the dialogs of the EditorShell (project.create, project.open, project.openSample).
	// - The recent projects (RecentProjects) as cards: a click opens one; the context menu also shows its folder, copies
	//   its path or takes it off the list (editor.removeRecentProject).
	// - "Connect an AI agent": the command line that adds Strata's MCP server (StrataCLI mcp) to Claude Code, with a copy
	//   button, and the automation server's live state.
	// - A footer with the version and commit, the GPU and the measured startup time, and About Strata.
	// Failures of its actions show above the recent projects until dismissed.
	class WelcomePanel : public EditorPanel
	{
	public:
		static constexpr const char* c_Id = "Welcome";
		// How often the recent projects are read again while the launcher shows: other editors add to them, and projects
		// that were deleted drop out.
		static constexpr double c_RefreshSeconds = 2.0;

		EditorPanelWindowOptions GetWindowOptions(EditorPanelContext& context) override;
		void OnImGuiRender(EditorPanelContext& context) override;
		void OnHidden(EditorPanelContext& context) override;

		// Shows an error above the recent projects until it is dismissed or the launcher hides.
		void ShowError(std::string message);
		const std::string& GetError() const { return m_Error; }
		// The projects the cards show, as last read.
		const std::vector<RecentProject>& GetRecentProjects() const { return m_RecentProjects; }

		// The command line that adds Strata's MCP server to Claude Code: claude mcp add strata -- "<StrataCLI>" mcp
		static std::string GetAgentCommandLine(const std::filesystem::path& cliExecutable);
	private:
		// A template or a sample to start from.
		struct Starter
		{
			std::string Id;
			std::string Name;
			std::string Description;
		};

		void Refresh(EditorPanelContext& context);
		void DrawSidebar(EditorPanelContext& context);
		void DrawMain(EditorPanelContext& context);
		// The band across the top: a greeting and the strata.
		void DrawHero(float width);
		void DrawErrorBanner(float width);
		// The cards of the recent projects and of what to start from, in the content's grid.
		void DrawRecentProjects(EditorPanelContext& context, int columns, float columnWidth);
		void DrawEmptyState(float width);
		void DrawStarters(EditorPanelContext& context, int columns, float columnWidth);
		void DrawAgentCard(EditorPanelContext& context, float width);
		void DrawFooter(EditorPanelContext& context);
		void OpenRecentProject(EditorPanelContext& context, const std::filesystem::path& projectFile);
		void RemoveRecentProject(EditorPanelContext& context, const std::filesystem::path& projectFile);
	private:
		bool m_Visible = false;
		double m_RefreshedAt = 0.0;
		std::vector<RecentProject> m_RecentProjects;
		std::vector<Starter> m_Templates;
		std::vector<Starter> m_Samples;
		bool m_StartersRead = false;
		std::string m_Error;
	};

}
