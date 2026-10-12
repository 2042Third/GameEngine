#include "UI/ExportDialog.h"

#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/GameExport.h"
#include "UI/EditorActions.h"
#include "UI/EditorFonts.h"
#include "UI/FileDialogs.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/TextFormat.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Platform.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace Strata
{

	namespace
	{

		// The dialog's content width and the width of its number fields, in text heights.
		constexpr float c_WidthInFontSizes = 34.0f;
		constexpr float c_NumberFieldWidthInFontSizes = 6.0f;
		constexpr int c_MaxWindowSize = 16384;

		bool IsEnterPressed()
		{
			return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
		}

		// Text wrapped at the dialog's width, in the caption size and a color.
		void Caption(const std::string& text, const ImVec4& color, float width)
		{
			UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Caption);
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
			ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			ImGui::PopFont();
		}

		// A checkbox recorded in the probe (the kit has none) with its state as the text, with an explanation as its tooltip.
		void Option(const char* id, const char* label, bool* value, const char* tooltip)
		{
			ImGui::PushID(id);
			ImGui::Checkbox(label, value);
			ImGui::PopID();
			UI::ItemProbe::Record(id, true, 0, *value ? "Checked" : "Unchecked");
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				ImGui::SetTooltip("%s", tooltip);
		}

		// Buttons at the right end of the dialog's width.
		void PlaceButtons(float width, std::initializer_list<const char*> labels)
		{
			float buttons = 0.0f;
			for (const char* label : labels)
				buttons += UI::DialogButtonWidth(label) + (buttons > 0.0f ? ImGui::GetStyle().ItemSpacing.x : 0.0f);
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(width - buttons, 0.0f));
		}

		// Sizes in MB, small ones in KB (a game of a few materials packs into kilobytes).
		std::string DescribeSize(uint64_t bytes)
		{
			constexpr double c_KiB = 1024.0;
			constexpr double c_MiB = 1024.0 * 1024.0;
			if (static_cast<double>(bytes) < c_MiB)
				return fmt::format("{:.1f} KB", static_cast<double>(bytes) / c_KiB);
			return fmt::format("{:.1f} MB", static_cast<double>(bytes) / c_MiB);
		}

		// What a successful export wrote.
		std::string DescribeExport(uint64_t assetCount, uint64_t packBytes, double seconds, const std::filesystem::path& directory, size_t excluded)
		{
			return fmt::format("Exported {} assets ({} pack) in {:.1f} s to {}.{}", assetCount, DescribeSize(packBytes), seconds, UI::DisplayPath(directory),
				excluded > 0 ? fmt::format(" {} unused {} left out.", excluded, excluded == 1 ? "asset was" : "assets were") : std::string());
		}

	}

	ExportDialog::ExportDialog()
	{
		const GameExportOptions defaults;
		m_Width = static_cast<int>(defaults.WindowWidth);
		m_Height = static_cast<int>(defaults.WindowHeight);
		m_Fullscreen = defaults.Fullscreen;
		m_IncludeRuntime = defaults.IncludeRuntime;
		m_IncludeScriptSymbols = defaults.IncludeScriptSymbols;
		m_Prune = defaults.Prune;
	}

	void ExportDialog::Open(EditorPanelContext& context)
	{
		const Ref<Project>& project = context.Context.GetProject();
		if (!project)
			return;
		if (m_Directory.empty())
		{
			const std::filesystem::path projectDirectory = project->GetProjectDirectory();
			m_Directory = UI::DisplayPath(projectDirectory.parent_path() / FileSystem::FromUTF8(FileSystem::ToUTF8(projectDirectory.filename()) + " Build"));
		}
		// A finished export's outcome is shown until the dialog opens again.
		if (!m_Exporting)
			m_Finished = false;
		// The file system may have changed while the dialog was closed.
		m_CheckedDirectory.reset();
		m_OpenRequested = true;
	}

	void ExportDialog::WatchExports(EditorPanelContext& context)
	{
		// The dialog's own export reports through its command (StartExport). Another one is shown while the dialog is open or
		// opening, from whatever it showed (the form, or the outcome of an earlier export).
		const Ref<GameExport>& current = context.Context.GetGameExport();
		if (!m_Exporting && (m_Open || m_OpenRequested) && current && !current->IsFinished() && m_Watched.lock() != current)
		{
			m_Watched = current;
			m_Finished = false;
		}

		const Ref<GameExport> watched = m_Watched.lock();
		if (!watched)
		{
			m_Watched.reset();
			return;
		}
		if (!watched->IsFinished())
			return;
		m_Watched.reset();
		m_Finished = true;
		m_Succeeded = watched->GetState() == GameExportState::Succeeded;
		m_ExportedDirectory = watched->GetOptions().Directory;
		if (!m_Succeeded)
		{
			m_Outcome = watched->GetError();
			return;
		}
		const GameExportResult& result = watched->GetResult();
		m_Outcome = DescribeExport(result.AssetCount, result.Pack.FileSize, result.Seconds, m_ExportedDirectory, result.ExcludedAssets.size());
	}

	const std::string& ExportDialog::GetDirectoryProblem(EditorPanelContext& context)
	{
		const Ref<Project>& project = context.Context.GetProject();
		const std::filesystem::path projectDirectory = project ? project->GetProjectDirectory() : std::filesystem::path();
		if (m_CheckedDirectory == m_Directory && m_CheckedProject == projectDirectory)
			return m_DirectoryProblem;
		m_CheckedDirectory = m_Directory;
		m_CheckedProject = projectDirectory;
		m_DirectoryProblem.clear();
		if (!project)
			return m_DirectoryProblem;
		if (std::optional<std::string> problem = CheckExportDirectory(FileSystem::FromUTF8(m_Directory), projectDirectory))
			m_DirectoryProblem = std::move(*problem);
		return m_DirectoryProblem;
	}

	void ExportDialog::Draw(EditorPanelContext& context)
	{
		WatchExports(context);
		if (m_OpenRequested)
		{
			UI::OpenModal(c_Popup);
			m_OpenRequested = false;
		}
		bool open = true;
		m_Open = UI::BeginModal(c_Popup, "Export Game", &open);
		if (!m_Open)
			return;
		const float width = ImGui::GetFontSize() * c_WidthInFontSizes;
		const Ref<GameExport>& current = context.Context.GetGameExport();
		const bool running = current && !current->IsFinished();
		// The dialog's own export shows its progress until its command answered (a frame after the export ended).
		const bool showProgress = m_Exporting || running;
		if (!context.Context.HasProject() && !showProgress)
			open = false;
		else if (showProgress)
			DrawProgress(context, width);
		else if (m_Finished)
			DrawOutcome(width);
		else
			DrawFields(context, width);

		ImGui::Spacing();
		if (showProgress)
		{
			// Closing the dialog leaves the export running; Cancel stops it (whoever started it).
			PlaceButtons(width, { "Cancel Export", "Hide" });
			if (UI::DialogButton("Export.Cancel", "Cancel Export", false, running))
				RunEditorCommand(context.Context, context.Commands, "project.cancelExport");
			ImGui::SameLine();
			if (UI::DialogButton("Export.Hide", "Hide"))
				open = false;
		}
		else if (m_Finished && m_Succeeded)
		{
			PlaceButtons(width, { "Open Folder", "Close" });
			if (UI::DialogButton("Export.OpenFolder", "Open Folder") && !Platform::OpenWithDefaultApplication(FileSystem::ToUTF8(m_ExportedDirectory)))
				m_Outcome = fmt::format("Could not open {}", UI::DisplayPath(m_ExportedDirectory));
			ImGui::SameLine();
			if (UI::DialogButton("Export.Close", "Close", true))
				open = false;
		}
		else if (m_Finished)
		{
			PlaceButtons(width, { "Back", "Close" });
			if (UI::DialogButton("Export.Back", "Back"))
			{
				// The form again, with the directory checked again: the export may have failed because of it.
				m_Finished = false;
				m_CheckedDirectory.reset();
			}
			ImGui::SameLine();
			if (UI::DialogButton("Export.Close", "Close", true))
				open = false;
		}
		else
		{
			const std::string& problem = GetDirectoryProblem(context);
			const bool editing = ImGui::IsAnyItemActive();
			PlaceButtons(width, { "Cancel", "Export" });
			if (UI::DialogButton("Export.Cancel", "Cancel") || (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !editing))
				open = false;
			ImGui::SameLine();
			const bool submit = UI::DialogButton("Export.Export", "Export", true, problem.empty()) || (IsEnterPressed() && !editing && problem.empty());
			if (submit && open)
				StartExport(context);
		}

		if (!open)
			ImGui::CloseCurrentPopup();
		UI::EndModal();
	}

	void ExportDialog::DrawFields(EditorPanelContext& context, float width)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const UI::ThemeColors& colors = UI::GetThemeColors();

		// Where the game goes.
		const char* browseLabel = "Browse...";
		const float browseWidth = UI::DialogButtonWidth(browseLabel);
		UI::TextField("Export.Directory", "Directory", m_Directory, "Folder the game is written to", std::max(width - browseWidth - style.ItemSpacing.x, 1.0f));
		ImGui::SameLine();
		if (UI::DialogButton("Export.Browse", browseLabel, false, m_FolderPickerAvailable))
		{
			if (const std::optional<std::filesystem::path> folder = FileDialogs::PickFolder(FileSystem::FromUTF8(m_Directory)))
				m_Directory = UI::DisplayPath(*folder);
		}
		ImGui::Spacing();

		// How it starts.
		const float numberWidth = ImGui::GetFontSize() * c_NumberFieldWidthInFontSizes;
		ImGui::SetNextItemWidth(numberWidth);
		ImGui::InputInt("##Width", &m_Width, 0, 0);
		UI::ItemProbe::Record("Export.Width", true, 0, std::to_string(m_Width));
		ImGui::SameLine();
		ImGui::TextUnformatted("x");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(numberWidth);
		ImGui::InputInt("##Height", &m_Height, 0, 0);
		UI::ItemProbe::Record("Export.Height", true, 0, std::to_string(m_Height));
		m_Width = std::clamp(m_Width, 1, c_MaxWindowSize);
		m_Height = std::clamp(m_Height, 1, c_MaxWindowSize);
		ImGui::SameLine();
		ImGui::TextUnformatted("window");
		ImGui::SameLine(0.0f, style.ItemSpacing.x * 3.0f);
		Option("Export.Fullscreen", "Fullscreen", &m_Fullscreen, "Start the game in fullscreen (its players can still pass --windowed)");
		ImGui::Spacing();

		// What goes with it.
		Option("Export.IncludeRuntime", "Copy the runtime", &m_IncludeRuntime,
			"Copy StrataRuntime as the game's executable, with the third-party notices; without it, only the game's data is written");
		Option("Export.IncludeScriptSymbols", "Copy the script symbols", &m_IncludeScriptSymbols,
			"Copy the script module's debug symbols (PDB), so that crashes in shipped scripts can be debugged");
		Option("Export.Prune", "Leave out unused assets", &m_Prune,
			"Pack only what the scenes use, and what the project's Export.AlwaysInclude names: scripts that load assets by path need them listed there "
			"(project.setExportSettings)");
		ImGui::Spacing();

		const std::string& problem = GetDirectoryProblem(context);
		if (!m_Outcome.empty() && !m_Succeeded)
			Caption(m_Outcome, colors.Error, width);
		else if (!problem.empty())
			Caption(problem, colors.Warning, width);
		else
			Caption(fmt::format("Writes the game to {}: an asset pack ordered by first use{}, the scripts the editor runs, a manifest{}. The editor keeps "
				"running while it works.", m_Directory, m_Prune ? " (only the assets in use)" : "", m_IncludeRuntime ? " and the runtime" : ""),
				colors.TextSecondary, width);
	}

	void ExportDialog::DrawProgress(EditorPanelContext& context, float width)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const Ref<GameExport>& exporter = context.Context.GetGameExport();
		const float progress = exporter ? exporter->GetProgress() : 0.0f;
		const std::string stage = exporter ? exporter->GetStage() : std::string();
		const std::filesystem::path directory = exporter ? exporter->GetOptions().Directory : m_ExportedDirectory;
		Caption(fmt::format("Exporting to {}", UI::DisplayPath(directory)), colors.TextSecondary, width);
		ImGui::Spacing();
		UI::StrataProgressBar("Export.Progress", progress, fmt::format("{:.0f}%", std::floor(progress * 100.0f)), width);
		ImGui::Spacing();
		Caption(stage, colors.Text, width);
	}

	void ExportDialog::DrawOutcome(float width)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		if (m_Succeeded)
		{
			UI::StrataProgressBar("Export.Progress", 1.0f, "Done", width);
			ImGui::Spacing();
			Caption(m_Outcome, colors.Success, width);
		}
		else
		{
			Caption(m_Outcome, colors.Error, width);
		}
	}

	void ExportDialog::StartExport(EditorPanelContext& context)
	{
		const nlohmann::json parameters = {
			{ "directory", m_Directory },
			{ "width", m_Width },
			{ "height", m_Height },
			{ "fullscreen", m_Fullscreen },
			{ "includeRuntime", m_IncludeRuntime },
			{ "includeScriptSymbols", m_IncludeScriptSymbols },
			{ "prune", m_Prune } };
		m_Outcome.clear();
		m_Succeeded = false;
		m_Exporting = true;
		m_ExportedDirectory = FileSystem::FromUTF8(m_Directory);
		context.Runner.Run(context.Context, context.Commands, "project.export", parameters, [this](const EditorCommandResult& result)
		{
			m_Exporting = false;
			m_Finished = true;
			m_Succeeded = result.Success;
			if (!result.Success)
			{
				m_Outcome = result.Error;
				return;
			}
			const nlohmann::json& value = result.Value;
			const nlohmann::json* excludedAssets = JsonUtils::Find(value, "excludedAssets");
			const size_t excluded = excludedAssets && excludedAssets->is_array() ? excludedAssets->size() : 0;
			m_Outcome = DescribeExport(JsonUtils::GetUInt(value, "assetCount", 0), JsonUtils::GetUInt(value, "packBytes", 0),
				JsonUtils::GetFloat(value, "seconds", 0.0f), m_ExportedDirectory, excluded);
		}, CommandOrigin { CommandSource::UI });
	}

}
