#include "UI/ExportDialog.h"

#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/GameExport.h"
#include "UI/EditorActions.h"
#include "UI/EditorFonts.h"
#include "UI/FileDialogs.h"
#include "UI/EditorShell.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/SettingsPages.h"
#include "UI/TextFormat.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Platform.h>
#include <Strata/Project/Project.h>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace Strata
{

	namespace
	{

		// The dialog's content width and the width of its number fields, in text heights.
		constexpr float c_WidthInFontSizes = 34.0f;
		constexpr float c_NumberFieldWidthInFontSizes = 6.0f;
		constexpr int c_MaxWindowSize = 16384;
		// The loading scene's choice for none.
		constexpr const char* c_NoLoadingScene = "None";

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
		for (const std::string& warning : result.Warnings)
			m_Outcome += fmt::format(" {}.", warning);
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

		if (m_CloseRequested)
		{
			open = false;
			m_CloseRequested = false;
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

		// What plays while the start scene loads.
		DrawLoadingScene(context, width);
		ImGui::Spacing();

		// Who the game is.
		DrawIdentity(context, width);
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

	void ExportDialog::DrawIdentity(EditorPanelContext& context, float width)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const ProjectConfig& config = context.Context.GetProject()->GetConfig();
		const ProjectGameInfo& game = config.Game;
		std::string summary = fmt::format("{} {}", game.Title.empty() ? config.Name : game.Title, game.Version);
		if (!game.Company.empty())
			summary += fmt::format(" by {}", game.Company);
		std::optional<AssetMetadata> icon;
		if (game.Icon.IsValid() && context.Context.GetAssetManager())
			icon = context.Context.GetAssetManager()->GetMetadata(game.Icon);
		summary += icon ? fmt::format(", with the icon {}", icon->Path) : std::string(", with the strata mark as its icon");
		Caption(summary, colors.Text, width);
		UI::ItemProbe::Record("Export.Identity", true, 0, summary);
		if (UI::LinkButton("Export.EditIdentity", "Title, version and icon in Project Settings", Icons::ArrowRight) && context.Shell)
		{
			context.Shell->ShowProjectSettings(SettingsBrowser::c_GamePage);
			m_CloseRequested = true;
		}
	}

	void ExportDialog::DrawLoadingScene(EditorPanelContext& context, float width)
	{
		EditorContext& editor = context.Context;
		const Ref<Project>& project = editor.GetProject();
		EditorAssetManager* assets = editor.GetAssetManager();
		if (!project || !assets)
			return;
		const ProjectConfig& config = project->GetConfig();
		const AssetHandle current = config.Export.LoadingScene;
		std::string preview = c_NoLoadingScene;
		if (current.IsValid())
		{
			const std::optional<AssetMetadata> metadata = assets->GetMetadata(current);
			preview = metadata && metadata->Type == AssetType::Scene ? metadata->Path : fmt::format("Missing scene {}", current.ToString());
		}

		const char* label = "Loading scene";
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(label);
		ImGui::SameLine();
		const float fieldWidth = std::max(width - ImGui::CalcTextSize(label).x - ImGui::GetStyle().ItemSpacing.x, 1.0f);
		const ImVec2 min = ImGui::GetCursorScreenPos();
		ImGui::SetNextItemWidth(fieldWidth);
		const bool open = ImGui::BeginCombo("##LoadingScene", preview.c_str());
		// Its own rectangle: while the list is open, its entries come after it.
		UI::ItemProbe::RecordRect("Export.LoadingScene", min, ImVec2(min.x + fieldWidth, min.y + ImGui::GetFrameHeight()), true, 0, preview);
		if (!open && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
		{
			ImGui::SetTooltip("A scene the game plays while its start scene loads (the project's export setting, saved in the project file); "
				"without one the game shows a cleared frame meanwhile");
		}
		if (!open)
			return;

		// The project's scenes other than the start scene, listed while the list is open (the asset index, not the disk).
		std::optional<nlohmann::json> picked;
		UI::PushSelectionColors(!current.IsValid());
		if (ImGui::Selectable(c_NoLoadingScene, !current.IsValid()))
			picked = nlohmann::json(nullptr);
		UI::PopSelectionColors();
		UI::ItemProbe::Record("Export.LoadingScene.None", true, 0, c_NoLoadingScene);
		size_t index = 0;
		for (const AssetListing& listing : assets->ListAssets("", true))
		{
			if (listing.Type != AssetType::Scene || listing.Handle == config.StartScene)
				continue;
			const bool selected = listing.Handle == current;
			ImGui::PushID(static_cast<int>(index));
			UI::PushSelectionColors(selected);
			if (ImGui::Selectable(listing.Path.c_str(), selected))
				picked = nlohmann::json(listing.Path);
			UI::PopSelectionColors();
			ImGui::PopID();
			UI::ItemProbe::Record(UI::NumberedProbeKey("Export.LoadingScene.", index), true, 0, listing.Path);
			index++;
		}
		ImGui::EndCombo();
		// A project setting, saved in the project file like project.setExportSettings saves it for agents.
		if (picked)
			RunEditorCommand(editor, context.Commands, "project.setExportSettings", { { "loadingScene", *picked } });
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
			if (const nlohmann::json* warnings = JsonUtils::Find(value, "warnings"); warnings && warnings->is_array())
			{
				for (const nlohmann::json& warning : *warnings)
				{
					if (warning.is_string())
						m_Outcome += fmt::format(" {}.", warning.get<std::string>());
				}
			}
		}, CommandOrigin { CommandSource::UI });
	}

}
