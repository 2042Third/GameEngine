#include "Panels/WelcomePanel.h"

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/ProjectTemplates.h"
#include "UI/EditorFonts.h"
#include "UI/EditorShell.h"
#include "UI/Icons.h"
#include "UI/TextFormat.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Version.h>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>

namespace Strata
{

	namespace
	{

		// Proportions, in text heights.
		constexpr float c_SidebarWidthInFontSizes = 19.0f;
		constexpr float c_SidebarPaddingInFontSizes = 1.6f;
		constexpr float c_MainPaddingInFontSizes = 2.2f;
		constexpr float c_MarkSizeInFontSizes = 2.6f;
		constexpr float c_SectionGapInFontSizes = 1.6f;
		// The content's grid: columns at least this wide; wide blocks span up to three.
		constexpr float c_ColumnMinWidthInFontSizes = 18.0f;
		constexpr int c_WideColumns = 3;
		constexpr float c_RecentCardHeightInFontSizes = 4.6f;
		constexpr float c_EmptyStateWidthInFontSizes = 40.0f;
		constexpr float c_ContentMaxWidthInFontSizes = 96.0f;
		constexpr float c_HeroHeightInFontSizes = 9.0f;
		constexpr float c_HeroTextWidthInFontSizes = 34.0f;
		// The strata of the hero are a little quieter than the mark.
		constexpr float c_HeroBandAlpha = 0.8f;
		constexpr float c_FooterHeightInFrames = 1.5f;

		// Badges cycle through the mark's warm tones and two cool ones, by name, so a project keeps its color.
		ImVec4 GetBadgeColor(std::string_view name)
		{
			const UI::ThemePalette& palette = UI::GetThemePalette();
			const ImVec4 colors[] = { palette.Ochre, palette.Azurite, palette.Sandstone, palette.Malachite, palette.Rust };
			uint32_t hash = 2166136261u; // FNV-1a: the same on every platform and run
			for (const char character : name)
				hash = (hash ^ static_cast<uint8_t>(character)) * 16777619u;
			return colors[hash % std::size(colors)];
		}

		int64_t GetUnixTime()
		{
			return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		}

		void WrappedText(std::string_view text, const ImVec4& color, float wrapWidth = 0.0f)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::PushTextWrapPos(wrapWidth > 0.0f ? ImGui::GetCursorPosX() + wrapWidth : 0.0f);
			ImGui::TextUnformatted(text.data(), text.data() + text.size());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
		}

		// A region of the launcher: a child window with its own surface and padding.
		bool BeginRegion(const char* id, const ImVec2& size, const ImVec4& background, const ImVec2& padding, ImGuiWindowFlags flags = ImGuiWindowFlags_None,
			ImGuiChildFlags childFlags = ImGuiChildFlags_None)
		{
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
			ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
			const bool visible = ImGui::BeginChild(id, size, childFlags | ImGuiChildFlags_AlwaysUseWindowPadding, flags);
			ImGui::PopStyleColor();
			ImGui::PopStyleVar();
			return visible;
		}

	}

	std::string WelcomePanel::GetAgentCommandLine(const std::filesystem::path& cliExecutable)
	{
		return "claude mcp add strata -- \"" + UI::DisplayPath(cliExecutable) + "\" mcp";
	}

	EditorPanelWindowOptions WelcomePanel::GetWindowOptions(EditorPanelContext&)
	{
		// The regions scroll on their own; the launcher itself fills the window exactly.
		EditorPanelWindowOptions options;
		options.Flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		return options;
	}

	void WelcomePanel::OnHidden(EditorPanelContext&)
	{
		m_Visible = false;
		m_Error.clear();
	}

	void WelcomePanel::ShowError(std::string message)
	{
		m_Error = std::move(message);
	}

	void WelcomePanel::Refresh(EditorPanelContext& context)
	{
		// Other editors of the user may have changed the list; projects whose files are gone drop out.
		RecentProjects& recent = context.Context.GetRecentProjects();
		recent.Reload();
		m_RecentProjects = recent.GetProjects();
		m_RefreshedAt = ImGui::GetTime();

		// What new projects can start from: read once, they do not change while the editor runs.
		if (!m_StartersRead)
		{
			m_StartersRead = true;
			const EditorCommandResult templates = context.Commands.Execute(context.Context, "project.templates", nlohmann::json::object());
			if (templates.Success && templates.Value.contains("templates"))
			{
				for (const nlohmann::json& entry : templates.Value["templates"])
					m_Templates.push_back({ entry.value("id", ""), entry.value("name", ""), entry.value("description", "") });
				// The lit template first, as in the New Project dialog.
				std::stable_partition(m_Templates.begin(), m_Templates.end(), [](const Starter& starter) { return starter.Id == ProjectTemplates::c_Basic3D; });
			}
			const EditorCommandResult samples = context.Commands.Execute(context.Context, "project.samples", nlohmann::json::object());
			if (samples.Success && samples.Value.contains("samples"))
			{
				for (const nlohmann::json& entry : samples.Value["samples"])
					m_Samples.push_back({ entry.value("id", ""), entry.value("name", ""), entry.value("description", "") });
			}
			else
			{
				// An editor without its samples (e.g. copied without them) still offers the templates.
				ST_WARN("No samples to offer: {}", samples.Error);
			}
		}
	}

	void WelcomePanel::OnImGuiRender(EditorPanelContext& context)
	{
		if (!m_Visible || ImGui::GetTime() - m_RefreshedAt > c_RefreshSeconds)
			Refresh(context);
		m_Visible = true;

		const UI::ThemeColors& colors = UI::GetThemeColors();
		const float fontSize = ImGui::GetFontSize();
		const ImVec2 available = ImGui::GetContentRegionAvail();
		const float footerHeight = std::round(ImGui::GetFrameHeight() * c_FooterHeightInFrames);
		const float sidebarWidth = std::min(std::round(fontSize * c_SidebarWidthInFontSizes), std::max(available.x * 0.5f, 1.0f));
		const float bodyHeight = std::max(available.y - footerHeight, 1.0f);

		const float sidebarPadding = std::round(fontSize * c_SidebarPaddingInFontSizes);
		if (BeginRegion("Sidebar", ImVec2(sidebarWidth, bodyHeight), colors.Chrome, ImVec2(sidebarPadding, sidebarPadding), ImGuiWindowFlags_NoScrollbar))
			DrawSidebar(context);
		ImGui::EndChild();
		// A hairline between the sidebar and the projects.
		const ImVec2 sidebarMax = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddLine(ImVec2(sidebarMax.x - 0.5f, ImGui::GetItemRectMin().y), ImVec2(sidebarMax.x - 0.5f, sidebarMax.y),
			ImGui::GetColorU32(colors.Border));

		ImGui::SameLine(0.0f, 0.0f);
		if (BeginRegion("Main", ImVec2(std::max(available.x - sidebarWidth, 1.0f), bodyHeight), colors.Panel, ImVec2(0.0f, 0.0f),
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
		{
			DrawMain(context);
		}
		ImGui::EndChild();

		if (BeginRegion("Footer", ImVec2(available.x, footerHeight), colors.Chrome, ImVec2(sidebarPadding, 0.0f), ImGuiWindowFlags_NoScrollbar))
			DrawFooter(context);
		ImGui::EndChild();
		const ImVec2 footerMin = ImGui::GetItemRectMin();
		ImGui::GetWindowDrawList()->AddLine(ImVec2(footerMin.x, footerMin.y + 0.5f), ImVec2(footerMin.x + available.x, footerMin.y + 0.5f),
			ImGui::GetColorU32(colors.Border));
	}

	void WelcomePanel::DrawSidebar(EditorPanelContext& context)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const float fontSize = ImGui::GetFontSize();
		EditorShell* shell = context.Shell;

		// The mark and the name.
		const float markSize = std::round(fontSize * c_MarkSizeInFontSizes);
		const ImVec2 markPosition = ImGui::GetCursorPos();
		UI::BrandMark(markSize);
		ImGui::SameLine(0.0f, style.ItemSpacing.x * 1.5f);
		UI::PushFont(UI::EditorFont::SemiBold, UI::TextSize::Display);
		ImGui::SetCursorPosY(markPosition.y + (markSize - ImGui::GetTextLineHeight()) * 0.5f);
		ImGui::TextUnformatted(c_EngineName);
		ImGui::PopFont();
		ImGui::SetCursorPosY(markPosition.y + markSize + style.ItemSpacing.y);
		WrappedText("Worlds built in layers, by people and AI agents together.", colors.TextSecondary);
		ImGui::Dummy(ImVec2(0.0f, fontSize * c_SectionGapInFontSizes));

		// What starts work.
		UI::ButtonStyle primary;
		primary.Primary = true;
		primary.Enabled = shell != nullptr;
		if (UI::ActionButton("Welcome.NewProject", Icons::Plus, "New Project", "Create a project from a template", primary, -1.0f))
			shell->ShowNewProjectDialog({});
		UI::ButtonStyle secondary;
		secondary.Enabled = shell != nullptr;
		if (UI::ActionButton("Welcome.OpenProject", Icons::FolderOpen, "Open Project...", "Open a project file (.stproj)", secondary, -1.0f))
			shell->ShowOpenProjectDialog();
		// A sample at once, or a choice when there are several.
		UI::ButtonStyle sample;
		sample.Enabled = shell != nullptr && !m_Samples.empty();
		if (UI::ActionButton("Welcome.OpenSample", Icons::Gamepad2, "Open Sample...", "Open a copy of a finished game made with Strata", sample, -1.0f))
		{
			if (m_Samples.size() == 1)
				shell->ShowOpenSampleDialog(m_Samples.front().Id);
			else
				ImGui::OpenPopup("Samples");
		}
		if (ImGui::BeginPopup("Samples"))
		{
			for (const Starter& entry : m_Samples)
			{
				if (ImGui::MenuItem(entry.Name.c_str()))
					shell->ShowOpenSampleDialog(entry.Id);
			}
			ImGui::EndPopup();
		}

		// The way past the launcher, at the bottom.
		const float bottom = ImGui::GetWindowHeight() - style.WindowPadding.y - ImGui::GetTextLineHeight();
		if (ImGui::GetCursorPosY() < bottom)
			ImGui::SetCursorPosY(bottom);
		if (UI::LinkButton("Welcome.ContinueWithoutProject", "Continue without a project", Icons::ArrowRight,
			"Edit an untitled scene with the built-in meshes and materials; saving needs a project") && shell)
		{
			shell->DismissLauncher();
		}
	}

	void WelcomePanel::DrawMain(EditorPanelContext& context)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const float fontSize = ImGui::GetFontSize();
		DrawHero(ImGui::GetContentRegionAvail().x);

		// Below the hero, a column of bounded width that scrolls when the window is short.
		const float padding = std::round(fontSize * c_MainPaddingInFontSizes);
		if (BeginRegion("Content", ImVec2(0.0f, 0.0f), UI::WithAlpha(colors.Panel, 0.0f), ImVec2(padding, padding)))
		{
			const float width = std::min(ImGui::GetContentRegionAvail().x, fontSize * c_ContentMaxWidthInFontSizes);
			if (!m_Error.empty())
				DrawErrorBanner(width);

			// The heading and how many there are.
			UI::Heading("Recent projects", UI::TextSize::Title);
			if (!m_RecentProjects.empty())
			{
				ImGui::SameLine(0.0f, style.ItemSpacing.x * 1.5f);
				UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Caption);
				ImGui::SetCursorPosY(ImGui::GetCursorPosY() + style.FramePadding.y * 0.5f);
				ImGui::PushStyleColor(ImGuiCol_Text, colors.TextDisabled);
				ImGui::Text("%zu", m_RecentProjects.size());
				ImGui::PopStyleColor();
				ImGui::PopFont();
			}
			ImGui::Spacing();
			// One grid for every section, so their cards line up.
			const int columns = std::max(1, static_cast<int>((width + style.ItemSpacing.x) / (fontSize * c_ColumnMinWidthInFontSizes + style.ItemSpacing.x)));
			const float columnWidth = std::floor((width - style.ItemSpacing.x * static_cast<float>(columns - 1)) / static_cast<float>(columns));
			// Wide blocks (the agent card, the note without recent projects) span up to three columns.
			const int wideColumns = std::min(columns, c_WideColumns);
			const float wideWidth = columnWidth * static_cast<float>(wideColumns) + style.ItemSpacing.x * static_cast<float>(wideColumns - 1);
			if (m_RecentProjects.empty())
				DrawEmptyState(wideWidth);
			else
				DrawRecentProjects(context, columns, columnWidth);

			ImGui::Dummy(ImVec2(0.0f, fontSize * c_SectionGapInFontSizes));
			DrawStarters(context, columns, columnWidth);
			ImGui::Dummy(ImVec2(0.0f, fontSize * c_SectionGapInFontSizes));
			DrawAgentCard(context, wideWidth);
		}
		ImGui::EndChild();
	}

	void WelcomePanel::DrawHero(float width)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const UI::ThemePalette& palette = UI::GetThemePalette();
		const float fontSize = ImGui::GetFontSize();
		const float height = std::round(fontSize * c_HeroHeightInFontSizes);
		const ImVec2 min = ImGui::GetCursorScreenPos();
		const ImVec2 max(min.x + width, min.y + height);
		ImGui::Dummy(ImVec2(width, height));
		ImDrawList* drawList = ImGui::GetWindowDrawList();

		// A band of the chrome's color that fades into the page, and on its right the strata: the mark's bands, drawn
		// large and staggered, emerging from the page toward the edge.
		const ImU32 chrome = ImGui::GetColorU32(colors.Chrome);
		const ImU32 page = ImGui::GetColorU32(colors.Panel);
		drawList->AddRectFilledMultiColor(min, max, chrome, chrome, page, page);
		drawList->PushClipRect(min, max, true);
		const ImVec4 bandColors[] = { palette.Sandstone, palette.Ochre, palette.Rust, palette.Umber };
		const float thickness = std::round(height * 0.12f);
		const float gap = std::round(height * 0.07f);
		const float length = std::clamp(width * 0.38f, height * 2.5f, height * 6.0f);
		const float stagger = height * 0.32f;
		const float top = min.y + std::round((height - (thickness * 4.0f + gap * 3.0f)) * 0.5f);
		for (int index = 0; index < 4; index++)
		{
			const float right = max.x - height * 0.3f - stagger * static_cast<float>(index);
			const float y = top + static_cast<float>(index) * (thickness + gap);
			const float radius = thickness * 0.5f;
			// Opaque, mixed with the chrome as if translucent: the rounded end overlaps the fade without a seam.
			const ImVec4& color = bandColors[index];
			const ImVec4 mixed(colors.Chrome.x + (color.x - colors.Chrome.x) * c_HeroBandAlpha, colors.Chrome.y + (color.y - colors.Chrome.y) * c_HeroBandAlpha,
				colors.Chrome.z + (color.z - colors.Chrome.z) * c_HeroBandAlpha, 1.0f);
			const ImU32 solid = ImGui::GetColorU32(mixed);
			const ImU32 clear = ImGui::GetColorU32(UI::WithAlpha(mixed, 0.0f));
			const float fadeEnd = right - thickness;
			drawList->AddRectFilledMultiColor(ImVec2(right - length, y), ImVec2(fadeEnd, y + thickness), clear, solid, solid, clear);
			drawList->AddRectFilled(ImVec2(fadeEnd - radius, y), ImVec2(right, y + thickness), solid, radius, ImDrawFlags_RoundCornersRight);
		}
		drawList->PopClipRect();
		drawList->AddLine(ImVec2(min.x, max.y - 0.5f), ImVec2(max.x, max.y - 0.5f), ImGui::GetColorU32(colors.Border));

		// The greeting, at the content's left edge.
		const float padding = std::round(fontSize * c_MainPaddingInFontSizes);
		const float textWidth = std::max(std::min(width * 0.5f, fontSize * c_HeroTextWidthInFontSizes), fontSize * 10.0f);
		UI::PushFont(UI::EditorFont::SemiBold, UI::TextSize::Display);
		const float titleHeight = ImGui::GetTextLineHeight();
		ImGui::PopFont();
		const char* subtitle = "Create a project from a template, pick up where you left off, or let an AI agent build alongside you.";
		const float subtitleHeight = ImGui::CalcTextSize(subtitle, nullptr, false, textWidth).y;
		const float textTop = min.y + std::round((height - titleHeight - ImGui::GetStyle().ItemSpacing.y - subtitleHeight) * 0.5f);
		ImGui::SetCursorScreenPos(ImVec2(min.x + padding, textTop));
		UI::Heading("Welcome to Strata", UI::TextSize::Display);
		ImGui::SetCursorScreenPos(ImVec2(min.x + padding, ImGui::GetCursorScreenPos().y));
		WrappedText(subtitle, colors.TextSecondary, textWidth);
		ImGui::SetCursorScreenPos(ImVec2(min.x, max.y));
	}

	void WelcomePanel::DrawErrorBanner(float width)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const UI::ThemeColors& colors = UI::GetThemeColors();
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, style.FrameRounding * 1.5f);
		if (BeginRegion("Error", ImVec2(width, 0.0f), UI::WithAlpha(colors.Error, 0.14f), ImVec2(style.FramePadding.x * 2.0f, style.FramePadding.y * 2.0f),
			ImGuiWindowFlags_NoScrollbar, ImGuiChildFlags_AutoResizeY))
		{
			const float closeSize = ImGui::GetFrameHeight();
			ImGui::PushStyleColor(ImGuiCol_Text, colors.Error);
			ImGui::TextUnformatted(Icons::CircleAlert);
			ImGui::PopStyleColor();
			ImGui::SameLine();
			WrappedText(m_Error, colors.Text, std::max(ImGui::GetContentRegionAvail().x - closeSize - style.ItemSpacing.x, 1.0f));
			ImGui::SameLine(std::max(ImGui::GetWindowWidth() - style.WindowPadding.x - closeSize, 0.0f));
			if (UI::IconButton("Welcome.DismissError", Icons::X, "Dismiss"))
				m_Error.clear();
		}
		ImGui::EndChild();
		ImGui::PopStyleVar();
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * c_SectionGapInFontSizes * 0.5f));
	}

	void WelcomePanel::DrawRecentProjects(EditorPanelContext& context, int columns, float columnWidth)
	{
		const ImVec2 cardSize(columnWidth, std::round(ImGui::GetFontSize() * c_RecentCardHeightInFontSizes));
		const int64_t now = GetUnixTime();

		// Actions run after the loop: they change the list it walks.
		std::filesystem::path open;
		std::filesystem::path remove;
		for (size_t index = 0; index < m_RecentProjects.size(); index++)
		{
			const RecentProject& project = m_RecentProjects[index];
			if (index % static_cast<size_t>(columns) != 0)
				ImGui::SameLine();
			const std::string id = fmt::format("Welcome.Recent.{}", index);
			const std::string initials = UI::GetInitials(project.Name);
			const std::string path = UI::DisplayPath(project.Path.parent_path());
			std::string detail = "Opened " + UI::DescribeTimeAgo(project.LastOpened, now);
			if (!project.EngineVersion.empty() && project.EngineVersion != c_EngineVersion)
				detail += " \xC2\xB7 Strata " + project.EngineVersion;
			UI::EntryCardContent content;
			content.Badge = initials;
			content.BadgeColor = GetBadgeColor(project.Name);
			content.Title = project.Name;
			content.Subtitle = path;
			content.Detail = detail;
			if (UI::EntryCard(id.c_str(), content, cardSize))
				open = project.Path;
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_DelayNormal))
				ImGui::SetTooltip("%s", UI::DisplayPath(project.Path).c_str());
			if (ImGui::BeginPopupContextItem())
			{
				if (ImGui::MenuItem("Open"))
					open = project.Path;
				if (ImGui::MenuItem("Show in Folder") && !Platform::OpenWithDefaultApplication(FileSystem::ToUTF8(project.Path.parent_path())))
					ShowError(fmt::format("The folder of '{}' could not be shown", project.Name));
				if (ImGui::MenuItem("Copy Path"))
					ImGui::SetClipboardText(UI::DisplayPath(project.Path).c_str());
				ImGui::Separator();
				if (ImGui::MenuItem("Remove from List"))
					remove = project.Path;
				ImGui::EndPopup();
			}
		}
		if (!remove.empty())
			RemoveRecentProject(context, remove);
		if (!open.empty())
			OpenRecentProject(context, open);
	}

	void WelcomePanel::DrawStarters(EditorPanelContext& context, int columns, float columnWidth)
	{
		// The templates (into the New Project dialog) and the samples (opened as copies), as cards in rows.
		struct StarterCard
		{
			std::string ProbeKey;
			const char* Icon;
			std::string Title;
			const Starter* Source;
			bool Sample;
		};
		std::vector<StarterCard> cards;
		for (const Starter& starter : m_Templates)
		{
			const char* icon = starter.Id == ProjectTemplates::c_Basic3D ? Icons::MountainSnow : (starter.Id == ProjectTemplates::c_Empty ? Icons::Box : Icons::LayoutTemplate);
			cards.push_back({ "Welcome.Template." + starter.Id, icon, starter.Name, &starter, false });
		}
		for (const Starter& sample : m_Samples)
			cards.push_back({ "Welcome.Sample." + sample.Id, Icons::Gamepad2, sample.Name + " sample", &sample, true });
		if (cards.empty())
			return;

		UI::Heading("Start something new", UI::TextSize::Title);
		ImGui::Spacing();
		ImVec2 size(columnWidth, 0.0f);
		for (const StarterCard& card : cards)
			size.y = std::max(size.y, UI::GetCardHeight(size.x, true, card.Title, card.Source->Description));
		for (size_t index = 0; index < cards.size(); index++)
		{
			const StarterCard& card = cards[index];
			if (index % static_cast<size_t>(columns) != 0)
				ImGui::SameLine();
			if (UI::Card(card.ProbeKey.c_str(), card.Icon, card.Title, card.Source->Description, size) && context.Shell)
			{
				if (card.Sample)
					context.Shell->ShowOpenSampleDialog(card.Source->Id);
				else
					context.Shell->ShowNewProjectDialog(card.Source->Id);
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_DelayNormal))
				ImGui::SetTooltip("%s", card.Sample ? "Open a copy of this finished game in a folder you choose" : "Create a project from this template");
		}
	}

	void WelcomePanel::DrawEmptyState(float width)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const float fontSize = ImGui::GetFontSize();
		// A quiet well where the cards will be: what goes there and how to start.
		const float height = std::round(fontSize * c_RecentCardHeightInFontSizes * 1.6f);
		const ImVec2 min = ImGui::GetCursorScreenPos();
		const ImVec2 max(min.x + width, min.y + height);
		ImGui::Dummy(ImVec2(width, height));
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const float rounding = style.FrameRounding * 2.0f;
		drawList->AddRectFilled(min, max, ImGui::GetColorU32(UI::WithAlpha(colors.Chrome, 0.5f)), rounding);
		drawList->AddRect(min, max, ImGui::GetColorU32(colors.Border), rounding);

		const float markSize = std::round(fontSize * c_MarkSizeInFontSizes);
		const float textWidth = std::min(fontSize * c_EmptyStateWidthInFontSizes, width - markSize - fontSize * 4.0f);
		const char* note = "Projects you create or open show up here. Start from a template with New Project, or open the Tetris sample to see a "
			"finished game made by an AI agent.";
		UI::PushFont(UI::EditorFont::SemiBold, UI::TextSize::Title);
		const float titleHeight = ImGui::GetTextLineHeight();
		ImGui::PopFont();
		const float noteHeight = ImGui::CalcTextSize(note, nullptr, false, std::max(textWidth, 1.0f)).y;
		const float contentLeft = min.x + fontSize * 1.5f;
		ImGui::SetCursorScreenPos(ImVec2(contentLeft, min.y + (height - markSize) * 0.5f));
		UI::BrandMark(markSize, false);
		const float textLeft = contentLeft + markSize + fontSize * 1.5f;
		ImGui::SetCursorScreenPos(ImVec2(textLeft, min.y + std::round((height - titleHeight - style.ItemSpacing.y - noteHeight) * 0.5f)));
		UI::Heading("No recent projects yet", UI::TextSize::Title);
		ImGui::SetCursorScreenPos(ImVec2(textLeft, ImGui::GetCursorScreenPos().y));
		WrappedText(note, colors.TextSecondary, std::max(textWidth, 1.0f));
		ImGui::SetCursorScreenPos(ImVec2(min.x, max.y + style.ItemSpacing.y));
	}

	void WelcomePanel::DrawAgentCard(EditorPanelContext& context, float width)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const EditorEnvironment environment = context.Shell ? context.Shell->GetEnvironment() : EditorEnvironment();
		const EditorAutomationState automation = context.Shell ? context.Shell->GetAutomationState() : EditorAutomationState();

		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, style.FrameRounding * 2.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x * 2.0f, style.WindowPadding.y * 2.0f));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.Raised);
		const bool visible = ImGui::BeginChild("Agent", ImVec2(width, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
			ImGuiWindowFlags_NoScrollbar);
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);
		if (visible)
		{
			// The title, and on its right whether an agent can connect or is connected.
			UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Title);
			ImGui::PushStyleColor(ImGuiCol_Text, colors.Accent);
			ImGui::TextUnformatted(Icons::Bot);
			ImGui::PopStyleColor();
			ImGui::PopFont();
			ImGui::SameLine();
			UI::Heading("Connect an AI agent", UI::TextSize::Title);

			std::string state;
			ImVec4 stateColor = colors.TextSecondary;
			const char* stateIcon = Icons::Radio;
			if (!automation.Running)
			{
				state = "Automation is off";
				stateColor = colors.Warning;
				stateIcon = Icons::CircleAlert;
			}
			else if (automation.Clients > 0)
			{
				state = fmt::format("{} {} connected", automation.Clients, automation.Clients == 1 ? "agent" : "agents");
				stateColor = colors.Success;
				stateIcon = Icons::PlugZap;
			}
			else
			{
				state = fmt::format("Ready on port {}", automation.Port);
			}
			UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Caption);
			const float pillWidth = style.FramePadding.x * 2.0f + ImGui::CalcTextSize(stateIcon).x + style.ItemInnerSpacing.x + ImGui::CalcTextSize(state.c_str()).x;
			ImGui::SameLine(std::max(ImGui::GetWindowWidth() - style.WindowPadding.x - pillWidth, ImGui::GetCursorPosX()));
			UI::Pill("Welcome.AgentStatus", stateIcon, state, stateColor,
				automation.Running ? "The editor's automation server: agents find it through its session file (StrataCLI)"
								   : "Started with --no-automation: agents cannot reach this editor");
			ImGui::PopFont();

			WrappedText("Claude Code and other MCP clients work in this editor with the same commands as its menus: they create projects, "
				"build scenes and scripts, play, test and export. Add Strata to Claude Code once:", colors.TextSecondary);
			ImGui::Spacing();
			if (!environment.CLIExecutable.empty())
				UI::CopyableCode("Welcome.AgentCommand", GetAgentCommandLine(environment.CLIExecutable));
			else
				WrappedText("StrataCLI is not next to this editor: build the StrataCLI target to connect agents.", colors.Warning);
		}
		ImGui::EndChild();
	}

	void WelcomePanel::DrawFooter(EditorPanelContext& context)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const EditorEnvironment environment = context.Shell ? context.Shell->GetEnvironment() : EditorEnvironment();

		UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Caption);
		const float height = ImGui::GetWindowHeight();
		const float markSize = std::round(ImGui::GetFontSize());
		ImGui::SetCursorPosY((height - markSize) * 0.5f);
		UI::BrandMark(markSize, false);
		ImGui::SameLine();

		std::string text = fmt::format("{} {} \xC2\xB7 {}", c_EngineName, c_EngineVersion, c_EngineCommit);
		if (environment.GraphicsDevice)
			text += fmt::format(" \xC2\xB7 {} \xC2\xB7 {} {}", environment.GraphicsDevice->AdapterName, environment.GraphicsDevice->API, environment.GraphicsDevice->APIVersion);
		else
			text += " \xC2\xB7 no GPU";
		if (environment.StartupSeconds)
			text += fmt::format(" \xC2\xB7 started in {:.2f} s", *environment.StartupSeconds);
		ImGui::SetCursorPosY((height - ImGui::GetTextLineHeight()) * 0.5f);
		ImGui::PushStyleColor(ImGuiCol_Text, colors.TextDisabled);
		ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
		ImGui::PopStyleColor();

		const char* about = "About Strata";
		const float aboutWidth = ImGui::CalcTextSize(about).x;
		ImGui::SameLine(std::max(ImGui::GetWindowWidth() - style.WindowPadding.x - aboutWidth, ImGui::GetCursorPosX() + style.ItemSpacing.x));
		ImGui::SetCursorPosY((height - ImGui::GetTextLineHeight()) * 0.5f);
		if (UI::LinkButton("Welcome.About", about, nullptr, "Version, GPU, startup time and third-party notices") && context.Shell)
			context.Shell->ShowAboutDialog();
		ImGui::PopFont();
	}

	void WelcomePanel::OpenRecentProject(EditorPanelContext& context, const std::filesystem::path& projectFile)
	{
		// The action may run in a later frame (after the question about unsaved changes): it keeps what outlives the frame.
		EditorContext* editor = &context.Context;
		const EditorCommandRegistry* commands = &context.Commands;
		auto open = [this, editor, commands, projectFile]()
		{
			const EditorCommandResult result = commands->Execute(*editor, "project.open", { { "path", FileSystem::ToUTF8(projectFile) } });
			if (!result.Success)
				ShowError(fmt::format("Could not open {}: {}", UI::DisplayPath(projectFile), result.Error));
		};
		if (context.Shell)
			context.Shell->RequestDiscardChanges(std::move(open));
		else
			open();
		m_RefreshedAt = 0.0;
	}

	void WelcomePanel::RemoveRecentProject(EditorPanelContext& context, const std::filesystem::path& projectFile)
	{
		const EditorCommandResult result = context.Commands.Execute(context.Context, "editor.removeRecentProject", { { "path", FileSystem::ToUTF8(projectFile) } });
		if (!result.Success)
			ShowError(result.Error);
		Refresh(context);
	}

}
