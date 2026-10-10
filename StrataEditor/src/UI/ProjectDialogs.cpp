#include "UI/ProjectDialogs.h"

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/ProjectTemplates.h"
#include "UI/EditorFonts.h"
#include "UI/FileDialogs.h"
#include "UI/Icons.h"
#include "UI/TextFormat.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Project/Project.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <system_error>

namespace Strata
{

	namespace
	{

		// Template cards, in text heights.
		constexpr float c_TemplateCardWidthInFontSizes = 16.0f;
		constexpr float c_TemplateCardMinHeightInFontSizes = 7.0f;
		// The Open Sample dialog's content width, in text heights.
		constexpr float c_SampleDialogWidthInFontSizes = 32.0f;
		// Suggested names count up to here before giving up on a free one.
		constexpr int c_MaxNameSuffix = 999;

		const char* GetTemplateIcon(std::string_view id)
		{
			if (id == ProjectTemplates::c_Basic3D)
				return Icons::MountainSnow;
			if (id == ProjectTemplates::c_Empty)
				return Icons::Box;
			return Icons::LayoutTemplate;
		}

		// A name nothing in the location has taken: "My Project", "My Project 2", ...
		std::string MakeUniqueName(const std::filesystem::path& location, const std::string& base)
		{
			for (int number = 1; number <= c_MaxNameSuffix; number++)
			{
				std::string name = number == 1 ? base : fmt::format("{} {}", base, number);
				if (!FileSystem::Exists(location / FileSystem::FromUTF8(name)))
					return name;
			}
			return base;
		}

		bool IsEmptyDirectory(const std::filesystem::path& path)
		{
			std::error_code error;
			const bool empty = std::filesystem::is_directory(path, error) && std::filesystem::is_empty(path, error);
			return empty && !error;
		}

		// A label over a group of fields, like the text fields' own labels.
		void FieldLabel(std::string_view text)
		{
			UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Caption);
			ImGui::PushStyleColor(ImGuiCol_Text, UI::GetThemeColors().TextSecondary);
			ImGui::TextUnformatted(text.data(), text.data() + text.size());
			ImGui::PopStyleColor();
			ImGui::PopFont();
		}

		// What the dialog will do, or what keeps it from doing it, wrapped at the dialog's width.
		void Outcome(const std::string& text, const ImVec4& color, float width)
		{
			UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Caption);
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
			ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			ImGui::PopFont();
		}

		// Cancel and the primary button at the right end of the dialog's width.
		void PlaceDialogButtons(float width, const char* cancel, const char* confirm)
		{
			const float buttons = UI::DialogButtonWidth(cancel) + ImGui::GetStyle().ItemSpacing.x + UI::DialogButtonWidth(confirm);
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(width - buttons, 0.0f));
		}

		// Runs a command that finishes at once; the error of a failure goes to outError.
		bool RunNow(EditorPanelContext& context, std::string_view name, const nlohmann::json& parameters, nlohmann::json* outValue, std::string& outError)
		{
			EditorCommandResult result = context.Commands.Execute(context.Context, name, parameters);
			if (result.IsPending())
			{
				ST_ASSERT(false, "{} finishes over several frames: run it through the EditorCommandRunner", name);
				outError = fmt::format("{} cannot be run from this dialog", name);
				return false;
			}
			if (!result.Success)
			{
				outError = result.Error;
				return false;
			}
			if (outValue)
				*outValue = std::move(result.Value);
			return true;
		}

		bool IsEnterPressed()
		{
			return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
		}

	}

	ProjectDialogs::~ProjectDialogs()
	{
		RemoveSettingsHandler();
	}

	std::filesystem::path ProjectDialogs::GetDefaultLocation()
	{
		if (const std::optional<std::filesystem::path> home = Platform::FindHomeDirectory())
			return *home / c_DefaultLocationName;
		return Platform::GetUserDataDirectory("Strata") / "Projects";
	}

	std::filesystem::path ProjectDialogs::GetLocation() const
	{
		return m_RememberedLocation.empty() ? GetDefaultLocation() : m_RememberedLocation;
	}

	void ProjectDialogs::SetLocation(const std::filesystem::path& location)
	{
		if (location == m_RememberedLocation)
			return;
		m_RememberedLocation = location;
		if (ImGui::GetCurrentContext())
			ImGui::MarkIniSettingsDirty();
	}

	void ProjectDialogs::OpenNewProject(EditorPanelContext& context, const std::string& templateId)
	{
		m_Templates.clear();
		nlohmann::json value;
		std::string error;
		if (RunNow(context, "project.templates", nlohmann::json::object(), &value, error) && value.contains("templates"))
		{
			for (const nlohmann::json& entry : value["templates"])
				m_Templates.push_back({ entry.value("id", ""), entry.value("name", ""), entry.value("description", "") });
		}
		else
		{
			ST_ERROR("The project templates are unavailable: {}", error);
		}
		// People start from the lit template, so it comes first and is chosen.
		std::stable_partition(m_Templates.begin(), m_Templates.end(), [](const TemplateCard& card) { return card.Id == ProjectTemplates::c_Basic3D; });
		const bool known = std::any_of(m_Templates.begin(), m_Templates.end(), [&templateId](const TemplateCard& card) { return card.Id == templateId; });
		m_TemplateId = known ? templateId : (m_Templates.empty() ? std::string() : m_Templates.front().Id);

		const std::filesystem::path location = GetLocation();
		m_LocationText = UI::DisplayPath(location);
		m_Name = MakeUniqueName(location, c_DefaultProjectName);
		m_Error.clear();
		m_CheckedName.clear();
		m_CheckedLocation.clear();
		m_Dialog = Dialog::NewProject;
		m_OpenRequested = true;
	}

	bool ProjectDialogs::OpenSample(EditorPanelContext& context, const std::string& sampleId, std::string* outError)
	{
		nlohmann::json value;
		std::string error;
		if (!RunNow(context, "project.samples", nlohmann::json::object(), &value, error))
		{
			if (outError)
				*outError = error;
			return false;
		}
		const nlohmann::json& samples = value["samples"];
		const auto sample = std::find_if(samples.begin(), samples.end(), [&sampleId](const nlohmann::json& entry) { return entry.value("id", "") == sampleId; });
		if (sample == samples.end())
		{
			if (outError)
				*outError = fmt::format("There is no sample '{}'", sampleId);
			return false;
		}
		m_SampleId = sampleId;
		m_SampleName = sample->value("name", sampleId);
		m_SampleDescription = sample->value("description", "");

		const std::filesystem::path location = GetLocation();
		m_LocationText = UI::DisplayPath(location);
		m_Name = MakeUniqueName(location, sampleId);
		m_Error.clear();
		m_CheckedName.clear();
		m_CheckedLocation.clear();
		m_Dialog = Dialog::OpenSample;
		m_OpenRequested = true;
		return true;
	}

	void ProjectDialogs::Draw(EditorPanelContext& context)
	{
		if (m_OpenRequested)
		{
			UI::OpenModal(m_Dialog == Dialog::NewProject ? c_NewProjectPopup : c_OpenSamplePopup);
			m_OpenRequested = false;
		}
		if (m_Dialog == Dialog::NewProject)
			DrawNewProject(context);
		else if (m_Dialog == Dialog::OpenSample)
			DrawOpenSample(context);
	}

	void ProjectDialogs::DrawNewProject(EditorPanelContext& context)
	{
		bool open = true;
		if (!UI::BeginModal(c_NewProjectPopup, "New Project", &open))
		{
			m_Dialog = Dialog::None;
			return;
		}
		// Escape leaves a field first, then closes the dialog.
		const bool editing = ImGui::IsAnyItemActive();
		const ImGuiStyle& style = ImGui::GetStyle();
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const float fontSize = ImGui::GetFontSize();
		// As tall as the longest description needs.
		ImVec2 cardSize(std::round(fontSize * c_TemplateCardWidthInFontSizes), std::round(fontSize * c_TemplateCardMinHeightInFontSizes));
		for (const TemplateCard& card : m_Templates)
			cardSize.y = std::max(cardSize.y, UI::GetCardHeight(cardSize.x, true, card.Name, card.Description));
		const float cardCount = static_cast<float>(std::max<size_t>(m_Templates.size(), 2));
		const float width = cardSize.x * cardCount + style.ItemSpacing.x * (cardCount - 1.0f);

		FieldLabel("Template");
		for (size_t index = 0; index < m_Templates.size(); index++)
		{
			const TemplateCard& card = m_Templates[index];
			if (index > 0)
				ImGui::SameLine();
			const std::string id = "NewProject.Template." + card.Id;
			if (UI::Card(id.c_str(), GetTemplateIcon(card.Id), card.Name, card.Description, cardSize, card.Id == m_TemplateId))
				m_TemplateId = card.Id;
		}
		ImGui::Spacing();

		UI::TextField("NewProject.Name", "Name", m_Name, "Project name", width);
		bool submit = ImGui::IsItemDeactivated() && IsEnterPressed();
		submit |= DrawLocationField(width);

		std::string problem;
		const std::filesystem::path target = GetTarget(m_Name, problem);
		ImGui::Spacing();
		if (!m_Error.empty())
			Outcome(m_Error, colors.Error, width);
		else if (!problem.empty())
			Outcome(problem, colors.Warning, width);
		else
			Outcome("Creates " + UI::DisplayPath(target), colors.TextSecondary, width);
		ImGui::Spacing();

		PlaceDialogButtons(width, "Cancel", "Create");
		if (UI::DialogButton("NewProject.Cancel", "Cancel") || (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !editing))
			open = false;
		ImGui::SameLine();
		submit |= UI::DialogButton("NewProject.Create", "Create", true, problem.empty());
		if (submit && problem.empty() && open)
		{
			nlohmann::json parameters = { { "directory", FileSystem::ToUTF8(target) }, { "name", m_Name } };
			if (!m_TemplateId.empty())
				parameters["template"] = m_TemplateId;
			std::string error;
			if (RunNow(context, "project.create", parameters, nullptr, error))
			{
				SetLocation(target.parent_path());
				open = false;
			}
			else
			{
				m_Error = error;
			}
		}
		if (!open)
		{
			ImGui::CloseCurrentPopup();
			m_Dialog = Dialog::None;
		}
		UI::EndModal();
	}

	void ProjectDialogs::DrawOpenSample(EditorPanelContext& context)
	{
		bool open = true;
		if (!UI::BeginModal(c_OpenSamplePopup, "Open Sample", &open))
		{
			m_Dialog = Dialog::None;
			return;
		}
		const bool editing = ImGui::IsAnyItemActive();
		const ImGuiStyle& style = ImGui::GetStyle();
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const float width = ImGui::GetFontSize() * c_SampleDialogWidthInFontSizes;

		// The sample: its icon, name and what it is.
		const ImVec2 start = ImGui::GetCursorPos();
		UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Display);
		ImGui::PushStyleColor(ImGuiCol_Text, colors.Accent);
		ImGui::TextUnformatted(Icons::Gamepad2);
		ImGui::PopStyleColor();
		const float iconWidth = ImGui::GetItemRectSize().x;
		ImGui::PopFont();
		ImGui::SetCursorPos(ImVec2(start.x + iconWidth + style.ItemSpacing.x * 2.0f, start.y));
		ImGui::BeginGroup();
		UI::Heading(m_SampleName, UI::TextSize::Title);
		ImGui::PushStyleColor(ImGuiCol_Text, colors.TextSecondary);
		ImGui::PushTextWrapPos(start.x + width);
		ImGui::TextUnformatted(m_SampleDescription.c_str(), m_SampleDescription.c_str() + m_SampleDescription.size());
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
		ImGui::EndGroup();
		ImGui::Spacing();
		ImGui::Spacing();

		UI::TextField("OpenSample.Folder", "Folder", m_Name, "Folder for the copy", width);
		bool submit = ImGui::IsItemDeactivated() && IsEnterPressed();
		submit |= DrawLocationField(width);

		std::string problem;
		const std::filesystem::path target = GetTarget(m_Name, problem);
		ImGui::Spacing();
		if (!m_Error.empty())
			Outcome(m_Error, colors.Error, width);
		else if (!problem.empty())
			Outcome(problem, colors.Warning, width);
		else
			Outcome(fmt::format("Copies the sample to {} and opens the copy; the sample itself stays as it is. Build its scripts (Ctrl+B) to play it.",
				UI::DisplayPath(target)), colors.TextSecondary, width);
		ImGui::Spacing();

		PlaceDialogButtons(width, "Cancel", "Copy and Open");
		if (UI::DialogButton("OpenSample.Cancel", "Cancel") || (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !editing))
			open = false;
		ImGui::SameLine();
		submit |= UI::DialogButton("OpenSample.Open", "Copy and Open", true, problem.empty());
		if (submit && problem.empty() && open)
		{
			std::string error;
			if (RunNow(context, "project.openSample", { { "sample", m_SampleId }, { "directory", FileSystem::ToUTF8(target) } }, nullptr, error))
			{
				SetLocation(target.parent_path());
				open = false;
			}
			else
			{
				m_Error = error;
			}
		}
		if (!open)
		{
			ImGui::CloseCurrentPopup();
			m_Dialog = Dialog::None;
		}
		UI::EndModal();
	}

	bool ProjectDialogs::DrawLocationField(float width)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const char* browseLabel = "Browse...";
		const float browseWidth = UI::DialogButtonWidth(browseLabel);
		UI::TextField("ProjectDialogs.Location", "Location", m_LocationText, "Folder the project goes into", std::max(width - browseWidth - style.ItemSpacing.x, 1.0f));
		const bool entered = ImGui::IsItemDeactivated() && IsEnterPressed();
		ImGui::SameLine();
		if (UI::DialogButton("ProjectDialogs.Browse", browseLabel, false, m_FolderPickerAvailable))
		{
			if (const std::optional<std::filesystem::path> folder = FileDialogs::PickFolder(FileSystem::FromUTF8(m_LocationText)))
				m_LocationText = UI::DisplayPath(*folder);
		}
		return entered;
	}

	std::filesystem::path ProjectDialogs::GetTarget(const std::string& folderName, std::string& outProblem)
	{
		if (folderName == m_CheckedName && m_LocationText == m_CheckedLocation)
		{
			outProblem = m_CheckedProblem;
			return m_CheckedTarget;
		}
		m_CheckedName = folderName;
		m_CheckedLocation = m_LocationText;
		m_Error.clear();

		const std::filesystem::path location = FileSystem::FromUTF8(m_LocationText);
		std::string problem;
		std::filesystem::path target;
		if (m_LocationText.empty())
			problem = "Choose the folder the project goes into.";
		else if (!location.is_absolute())
			problem = "The location must be a full path.";
		else if (FileSystem::Exists(location) && !FileSystem::IsDirectory(location))
			problem = "The location is a file, not a folder.";
		else if (folderName.empty())
			problem = "Enter a name.";
		else if (!Project::IsValidName(folderName))
			problem = "The name becomes a folder and a file name: leave out <>:\"/\\|?*, spaces or a dot at the end, and names such as CON.";
		else
		{
			target = location / FileSystem::FromUTF8(folderName);
			if (FileSystem::Exists(target) && !IsEmptyDirectory(target))
				problem = fmt::format("\"{}\" already exists in this location: choose another name.", folderName);
		}
		m_CheckedTarget = problem.empty() ? target : std::filesystem::path();
		m_CheckedProblem = problem;
		outProblem = problem;
		return m_CheckedTarget;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Settings (imgui.ini)
	////////////////////////////////////////////////////////////////////////////////

	void ProjectDialogs::InstallSettingsHandler()
	{
		ImGuiContext* context = ImGui::GetCurrentContext();
		if (!context)
			return;
		ImGui::RemoveSettingsHandler(c_SettingsName);
		ImGuiSettingsHandler handler;
		handler.TypeName = c_SettingsName;
		handler.TypeHash = ImHashStr(c_SettingsName);
		handler.ReadOpenFn = &ProjectDialogs::SettingsReadOpen;
		handler.ReadLineFn = &ProjectDialogs::SettingsReadLine;
		handler.WriteAllFn = &ProjectDialogs::SettingsWriteAll;
		handler.UserData = this;
		ImGui::AddSettingsHandler(&handler);
		m_SettingsContext = context;
	}

	void ProjectDialogs::RemoveSettingsHandler()
	{
		// Only this handler, and only while its context is current (it may be gone already).
		if (m_SettingsContext && ImGui::GetCurrentContext() == m_SettingsContext)
		{
			const ImGuiSettingsHandler* handler = ImGui::FindSettingsHandler(c_SettingsName);
			if (handler && handler->UserData == this)
				ImGui::RemoveSettingsHandler(c_SettingsName);
		}
		m_SettingsContext = nullptr;
	}

	void* ProjectDialogs::SettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler* handler, const char* name)
	{
		return std::strcmp(name, "Settings") == 0 ? handler->UserData : nullptr;
	}

	void ProjectDialogs::SettingsReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line)
	{
		constexpr std::string_view key = "ProjectLocation=";
		const std::string_view text(line);
		if (text.substr(0, key.size()) != key)
			return;
		const std::filesystem::path location = FileSystem::FromUTF8(text.substr(key.size()));
		if (location.is_absolute())
			static_cast<ProjectDialogs*>(entry)->m_RememberedLocation = location;
	}

	void ProjectDialogs::SettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buffer)
	{
		const ProjectDialogs& dialogs = *static_cast<const ProjectDialogs*>(handler->UserData);
		if (dialogs.m_RememberedLocation.empty())
			return;
		// One line per setting: a location with a line break cannot be kept.
		const std::string location = FileSystem::ToUTF8(dialogs.m_RememberedLocation);
		if (location.find_first_of("\r\n") != std::string::npos)
			return;
		buffer->appendf("[%s][Settings]\nProjectLocation=%s\n\n", c_SettingsName, location.c_str());
	}

}
