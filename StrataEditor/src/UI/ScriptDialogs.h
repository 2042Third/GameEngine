#pragma once

#include "Editor/ExternalEditor.h"
#include "UI/EditorPanelRegistry.h"

#include <optional>
#include <string>
#include <vector>

namespace Strata
{

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

	// Scripts > External Editor...: the command that opens script sources (script.setExternalEditor), the program it starts
	// (or why it cannot start), the code editors found on this machine (ExternalEditor::FindChoices; examples without any),
	// each a click away, and whether STRATA_EXTERNAL_EDITOR overrides it. Drawn by EditorLayer.
	class ExternalEditorDialog
	{
	public:
		static constexpr const char* c_Popup = "External Editor";

		void Open(EditorPanelContext& context);
		void Draw(EditorPanelContext& context);
		bool IsOpen() const { return m_Open; }
		const std::vector<ExternalEditorChoice>& GetChoices() const { return m_Choices; }
	private:
		void DrawChoices(float width);
	private:
		bool m_OpenRequested = false;
		bool m_Open = false;
		std::string m_Command;
		std::string m_Error;
		// The command the program was looked for, and the program (or why there is none).
		std::optional<std::string> m_CheckedCommand;
		std::string m_Program;
		std::string m_ProgramProblem;
		std::vector<ExternalEditorChoice> m_Choices;
		bool m_ChoicesFound = false; // Found on this machine, rather than examples
	};

}
