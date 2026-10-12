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
		// The External Editor dialog's column of editor names before their commands, in text heights.
		constexpr float c_ChoiceNameWidthInFontSizes = 7.0f;

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
				{ { "class", m_ClassName }, { "template", m_Empty ? "empty" : "behaviour" }, { "open", true } }, CommandOrigin { CommandSource::UI });
			if (result.Success)
			{
				// The file exists either way; say why it did not open rather than leave the person waiting for their editor.
				if (result.Value.contains("openError"))
					ST_WARN("New Script: '{}' was written, but it did not open: {}", result.Value["relativePath"].get<std::string>(), result.Value["openError"].get<std::string>());
				// The class can be attached once a module holds it: the build starts now (the status bar shows it), and the
				// Inspector offers the class when it finished.
				const EditorCommandResult build = context.Commands.Execute(context.Context, "script.build", { { "wait", false } }, CommandOrigin { CommandSource::UI });
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
		m_CheckedCommand.reset();
		// The editors on this machine are looked for once per opening (the file system is read).
		const ScriptBuildSettings& toolchain = context.Context.GetScriptBuildSettings();
		const std::filesystem::path toolchainFiles[] = { toolchain.CXXCompiler, toolchain.MakeProgram };
		m_Choices = ExternalEditor::FindChoices(toolchainFiles);
		m_ChoicesFound = !m_Choices.empty();
		if (!m_ChoicesFound)
			m_Choices = ExternalEditor::GetExampleChoices();
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
		// The program the command starts, looked for again only when the command changed (the file system is read then).
		if (!m_CheckedCommand || *m_CheckedCommand != m_Command)
		{
			m_CheckedCommand = m_Command;
			m_Program.clear();
			m_ProgramProblem.clear();
			if (!m_Command.empty())
			{
				if (const std::optional<std::filesystem::path> program = ExternalEditor::ResolveProgram(m_Command, &m_ProgramProblem))
					m_Program = FileSystem::ToUTF8(*program);
			}
		}
		const std::string programText = m_Command.empty() ? std::string("Files open in the system's default application.")
			: (m_Program.empty() ? "Cannot start: " + m_ProgramProblem : "Starts " + m_Program);
		Caption(programText, m_Command.empty() || !m_Program.empty() ? colors.TextSecondary : colors.Warning, width);
		UI::ItemProbe::Record("ExternalEditor.Program", !m_Program.empty(), 0, programText);

		// The editors found here, or examples: one command per line, as it is typed; a click puts it in the field.
		ImGui::Spacing();
		Caption(m_ChoicesFound ? "Found on this machine (click to use):" : "For example (click to use):", colors.TextSecondary, width);
		DrawChoices(width);
		Caption("Batch files (.cmd, .bat) do not run, as nothing runs through a shell: name the program they start.", colors.TextSecondary, width);
		if (launcher.GetSource() == ExternalEditorSource::Environment)
			Caption(fmt::format("STRATA_EXTERNAL_EDITOR is set and wins over this setting: {}", launcher.GetCommand()), colors.Warning, width);
		if (!m_Error.empty())
			Caption(m_Error, colors.Error, width);

		ImGui::Spacing();
		PlaceButtons(width, { "Save", "Cancel" });
		if (UI::DialogButton("ExternalEditor.Save", "Save", true) || submitted)
		{
			const EditorCommandResult result = context.Commands.Execute(context.Context, "script.setExternalEditor", { { "command", m_Command } },
				CommandOrigin { CommandSource::UI });
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

	void ExternalEditorDialog::DrawChoices(float width)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const float nameWidth = ImGui::GetFontSize() * c_ChoiceNameWidthInFontSizes;
		for (size_t index = 0; index < m_Choices.size(); index++)
		{
			const ExternalEditorChoice& choice = m_Choices[index];
			ImGui::PushID(static_cast<int>(index));
			const ImVec2 start = ImGui::GetCursorScreenPos();
			const bool selected = m_Command == choice.Command;
			UI::PushSelectionColors(selected);
			// A choice fills the field: the dialog stays open (a selectable closes the popup it is in by default).
			const bool picked = ImGui::Selectable("##Choice", selected, ImGuiSelectableFlags_NoAutoClosePopups, ImVec2(width, 0.0f));
			UI::PopSelectionColors();
			const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
			UI::ItemProbe::Record(UI::NumberedProbeKey("ExternalEditor.Choice.", index), true, 0, choice.Command);
			// The editor's name, then its command in the monospaced font on one line, shortened at its start where it is too long
			// (a program's folder; the arguments stay), whole in the tooltip.
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Caption);
			drawList->AddText(start, ImGui::GetColorU32(colors.TextSecondary), choice.Name.c_str());
			ImGui::PopFont();
			UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
			const std::string shown = UI::ElideStart(choice.Command, std::max(width - nameWidth, 0.0f));
			drawList->AddText(ImVec2(start.x + nameWidth, start.y), ImGui::GetColorU32(colors.Text), shown.c_str());
			ImGui::PopFont();
			if (hovered)
				ImGui::SetTooltip("%s", choice.Command.c_str());
			if (picked)
				m_Command = choice.Command;
			ImGui::PopID();
		}
	}

}
