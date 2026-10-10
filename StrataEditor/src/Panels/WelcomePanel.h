#pragma once

#include "Editor/RecentProjects.h"
#include "UI/EditorPanelRegistry.h"

#include <Strata/Core/Base.h>
#include <Strata/Core/JobSystem.h>

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
		// How often the recent projects are read again while the launcher shows: other editors add to them, projects that
		// were deleted leave the list (RecentProjects::Forget), and those out of reach are hidden. The file and the projects
		// are looked at on an I/O thread (JobSystem::SubmitIO), never in a frame.
		static constexpr double c_RefreshSeconds = 2.0;

		EditorPanelWindowOptions GetWindowOptions(EditorPanelContext& context) override;
		void OnImGuiRender(EditorPanelContext& context) override;
		void OnHidden(EditorPanelContext& context) override;

		// Shows an error above the recent projects until it is dismissed or the launcher hides.
		void ShowError(std::string message);
		const std::string& GetError() const { return m_Error; }
		// The projects the cards show: the list without the projects found missing when it was last read.
		const std::vector<RecentProject>& GetRecentProjects() const { return m_RecentProjects; }
		// Whether a read of the recent projects is under way.
		bool IsRefreshing() const { return m_RefreshResult != nullptr; }
		// Whether the last read found the project file missing (File > Open Recent leaves it out too).
		bool IsKnownMissing(const std::filesystem::path& projectFile) const;

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

		// The columns the sections' cards share.
		struct Grid
		{
			int Columns = 1;
			float ColumnWidth = 0.0f;
			float Width = 0.0f; // From the left edge of the first column to the right edge of the last one
		};

		// What a read of the recent projects on an I/O thread found.
		struct RefreshResult
		{
			uint64_t ChangeCount = 0; // RecentProjects::GetChangeCount when the read started
			RecentProjectsFile File;
			RecentProjectsCheck Check;
		};

		// Starts reading the recent projects (one read at a time).
		void Refresh(EditorPanelContext& context);
		// Takes the result of a read that has finished.
		void PollRefresh(EditorPanelContext& context);
		void UpdateRecentProjects(const RecentProjects& recent);
		// The templates and samples (project.templates, project.samples), the first time.
		void ReadStarters(EditorPanelContext& context);
		void DrawSidebar(EditorPanelContext& context);
		void DrawMain(EditorPanelContext& context);
		// The grid for a content width and the cards of the sections (in the current font).
		static Grid GetGrid(float width, size_t recentCount, size_t starterCount);
		// The band across the top (over the main area's width): a greeting and the strata, on the content's edges.
		void DrawHero(float width, float contentLeft, float contentWidth);
		void DrawErrorBanner(float width);
		// The cards of the recent projects and of what to start from, in the content's grid.
		void DrawRecentProjects(EditorPanelContext& context, const Grid& grid);
		void DrawEmptyState(float width);
		void DrawStarters(EditorPanelContext& context, const Grid& grid);
		void DrawAgentCard(EditorPanelContext& context, float width);
		void DrawFooter(EditorPanelContext& context);
		void OpenRecentProject(EditorPanelContext& context, const std::filesystem::path& projectFile);
		void RemoveRecentProject(EditorPanelContext& context, const std::filesystem::path& projectFile);
	private:
		bool m_Visible = false;
		double m_RefreshedAt = 0.0;
		JobHandle m_RefreshJob;
		Ref<RefreshResult> m_RefreshResult; // Shared with the read under way
		std::vector<std::filesystem::path> m_MissingProjects; // As the last read found them
		std::vector<RecentProject> m_RecentProjects;
		std::vector<Starter> m_Templates;
		std::vector<Starter> m_Samples;
		bool m_StartersRead = false;
		std::string m_Error;
	};

}
