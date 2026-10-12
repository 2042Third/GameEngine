#include "UI/ScriptDialogs.h"

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/ExternalEditor.h"
#include "Editor/ScriptProject.h"
#include "UI/EditorFonts.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>

#include <imgui.h>

#include <algorithm>

namespace Strata
{

	namespace
	{

		// The dialogs' content width, in text heights.
		constexpr float c_WidthInFontSizes = 30.0f;

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

		// Buttons at the right end of the dialog's width.
		void PlaceButtons(float width, std::initializer_list<const char*> labels)
		{
			float buttons = 0.0f;
			for (const char* label : labels)
				buttons += UI::DialogButtonWidth(label) + (buttons > 0.0f ? ImGui::GetStyle().ItemSpacing.x : 0.0f);
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(width - buttons, 0.0f));
		}

		bool IsEnterPressed()
		{
			return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// New Script
	////////////////////////////////////////////////////////////////////////////////

	void NewScriptDialog::Open()
	{
		m_OpenRequested = true;
		m_CheckedName.reset();
		m_Outcome.clear();
	}

	void NewScriptDialog::Draw(EditorPanelContext& context)
	{
		if (m_OpenRequested)
		{
			UI::OpenModal(c_Popup);
			m_OpenRequested = false;
		}
		bool open = true;
		m_Open = UI::BeginModal(c_Popup, "New Script", &open);
		if (!m_Open)
			return;
		const float width = ImGui::GetFontSize() * c_WidthInFontSizes;
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const Ref<Project>& project = context.Context.GetProject();
		if (!project)
		{
			ImGui::CloseCurrentPopup();
			UI::EndModal();
			return;
		}

		// The name is what a person types first: the field takes the keyboard when the dialog appears.
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		UI::TextField("NewScript.Class", "Class name", m_ClassName, "PlayerController", width);
		const bool submitted = ImGui::IsItemDeactivatedAfterEdit() && IsEnterPressed();
		// The script directory is read only when the name changed.
		if (!m_CheckedName || *m_CheckedName != m_ClassName)
		{
			m_CheckedName = m_ClassName;
			const std::optional<std::string> problem = m_ClassName.empty() ? std::optional<std::string>()
				: ValidateNewScriptClass(*project, context.Context.GetScriptBuildSettings().EngineDirectory, m_ClassName);
			m_Problem = problem.value_or(std::string());
		}

		ImGui::Spacing();
		ImGui::PushID("NewScript.Empty");
		ImGui::Checkbox("Empty class (no OnCreate, OnUpdate or example field)", &m_Empty);
		ImGui::PopID();
		UI::ItemProbe::Record("NewScript.Empty", true, 0, m_Empty ? "Checked" : "Unchecked");

		const std::filesystem::path file = project->GetScriptSourceDirectory() / FileSystem::FromUTF8((m_ClassName.empty() ? std::string("<Class>") : m_ClassName) + ".cpp");
		ImGui::Spacing();
		Caption(m_Problem.empty() ? fmt::format("Writes {} and builds the scripts. Once the build finished, attach the class to an entity (Add "
			"Component > Script); Play rebuilds what changes.", FileSystem::ToUTF8(file.lexically_relative(project->GetProjectDirectory()))) : m_Problem,
			m_Problem.empty() ? colors.TextSecondary : colors.Error, width);
		if (!m_Outcome.empty())
			Caption(m_Outcome, colors.Error, width);

		ImGui::Spacing();
		PlaceButtons(width, { "Create", "Cancel" });
		const bool canCreate = !m_ClassName.empty() && m_Problem.empty();
		if (UI::DialogButton("NewScript.Create", "Create", true, canCreate) || (submitted && canCreate))
		{
			const EditorCommandResult result = context.Commands.Execute(context.Context, "script.create",
				{ { "class", m_ClassName }, { "template", m_Empty ? "empty" : "behaviour" }, { "open", true } });
			if (result.Success)
			{
				// The file exists either way; say why it did not open rather than leave the person waiting for their editor.
				if (result.Value.contains("openError"))
					ST_WARN("New Script: '{}' was written, but it did not open: {}", result.Value["relativePath"].get<std::string>(), result.Value["openError"].get<std::string>());
				// The class can be attached once a module holds it: the build starts now (the status bar shows it), and the
				// Inspector offers the class when it finished.
				const EditorCommandResult build = context.Commands.Execute(context.Context, "script.build", { { "wait", false } });
				if (!build.Success)
					ST_WARN("New Script: the scripts did not start building ({}); build them before attaching the class (Scripts > Build Scripts)", build.Error);
				m_Outcome.clear();
				m_ClassName.clear();
				m_CheckedName.reset();
				open = false;
			}
			else
			{
				m_Outcome = result.Error;
			}
		}
		ImGui::SameLine();
		if (UI::DialogButton("NewScript.Cancel", "Cancel"))
			open = false;
		if (!open)
			ImGui::CloseCurrentPopup();
		UI::EndModal();
	}

	////////////////////////////////////////////////////////////////////////////////
	// External Editor
	////////////////////////////////////////////////////////////////////////////////

	void ExternalEditorDialog::Open(EditorPanelContext& context)
	{
		m_Command = context.Context.GetExternalEditor().GetUserCommand();
		m_Error.clear();
		m_OpenRequested = true;
	}

	void ExternalEditorDialog::Draw(EditorPanelContext& context)
	{
		if (m_OpenRequested)
		{
			UI::OpenModal(c_Popup);
			m_OpenRequested = false;
		}
		bool open = true;
		m_Open = UI::BeginModal(c_Popup, "External Editor", &open);
		if (!m_Open)
			return;
		const float width = ImGui::GetFontSize() * c_WidthInFontSizes;
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const ExternalEditorLauncher& launcher = context.Context.GetExternalEditor();

		Caption("The command that opens script sources at a line: from the Problems panel, crash reports and New Script. {file}, {line}, "
			"{column} and {directory} are filled in; without {file}, the file goes last. Empty: the system's default application.", colors.TextSecondary, width);
		ImGui::Spacing();
		UI::TextField("ExternalEditor.Command", "Command", m_Command, "code -g {file}:{line}:{column}", width);
		const bool submitted = ImGui::IsItemDeactivatedAfterEdit() && IsEnterPressed();
		Caption("For example:\n  code -g {file}:{line}:{column}\n  \"C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/devenv.exe\" /Edit {file}\n"
			"  clion --line {line} {file}\nBatch files (.cmd, .bat) do not run: name the program (VS Code: Code.exe).", colors.TextSecondary, width);
		if (launcher.GetSource() == ExternalEditorSource::Environment)
			Caption(fmt::format("STRATA_EXTERNAL_EDITOR is set and wins over this setting: {}", launcher.GetCommand()), colors.Warning, width);
		if (!m_Error.empty())
			Caption(m_Error, colors.Error, width);

		ImGui::Spacing();
		PlaceButtons(width, { "Save", "Cancel" });
		if (UI::DialogButton("ExternalEditor.Save", "Save", true) || submitted)
		{
			const EditorCommandResult result = context.Commands.Execute(context.Context, "script.setExternalEditor", { { "command", m_Command } });
			if (result.Success)
				open = false;
			else
				m_Error = result.Error;
		}
		ImGui::SameLine();
		if (UI::DialogButton("ExternalEditor.Cancel", "Cancel"))
			open = false;
		if (!open)
			ImGui::CloseCurrentPopup();
		UI::EndModal();
	}

}
