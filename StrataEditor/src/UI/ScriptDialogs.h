#pragma once

#include "Editor/ExternalEditor.h"
#include "UI/EditorPanelRegistry.h"
#include "UI/SettingsPages.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	class EditorContext;

	// Scripts > New Script...: a class name and a template (a behaviour with OnCreate, OnUpdate and a field, or an empty
	// class). The name is checked as it is typed (ValidateNewScriptClass, which reads the script directory: only when the
	// name changed); Create runs script.create, opens the new file in the external editor and starts building the scripts
	// (script.build), so that the class can be attached once the build finished. Drawn by EditorLayer.
	class NewScriptDialog
	{
	public:
		static constexpr const char* c_Popup = "New Script";

		void Open();
		void Draw(EditorPanelContext& context);
		bool IsOpen() const { return m_Open; }
		// What the last Create did: the file it wrote, or why it failed (empty before one).
		const std::string& GetOutcome() const { return m_Outcome; }
	private:
		bool m_OpenRequested = false;
		bool m_Open = false;
		std::string m_ClassName;
		bool m_Empty = false;
		// The name the problem was checked for, and the problem (empty when the name is fine).
		std::optional<std::string> m_CheckedName;
		std::string m_Problem;
		std::string m_Outcome;
	};

	// The External Editor page of the Preferences dialog (the Editor.ExternalEditor section; Scripts > External Editor...
	// opens it): the command that opens script sources (script.setExternalEditor, which also checks that its program is
	// there), the program it starts (or why it cannot start), the code editors found on this machine
	// (ExternalEditor::FindChoices, looked for once per page; examples without any), each a click away, and whether
	// STRATA_EXTERNAL_EDITOR overrides it.
	class ExternalEditorPage final : public SettingsPage
	{
	public:
		explicit ExternalEditorPage(EditorContext& context);

		void Draw(EditorPanelContext& context, float width) override;
		const std::vector<ExternalEditorChoice>& GetChoices() const { return m_Choices; }
	private:
		void DrawChoices(float width);
	private:
		std::string m_Command;
		std::string m_Error;
		uint64_t m_PreferencesVersion = 0; // Of the preferences m_Command was read from (EditorPreferences::GetVersion)
		// The command the program was looked for, and the program (or why there is none).
		std::optional<std::string> m_CheckedCommand;
		std::string m_Program;
		std::string m_ProgramProblem;
		std::vector<ExternalEditorChoice> m_Choices;
		bool m_ChoicesFound = false; // Found on this machine, rather than examples
	};

}
