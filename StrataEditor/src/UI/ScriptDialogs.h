#pragma once

#include "UI/EditorPanelRegistry.h"

#include <optional>
#include <string>

namespace Strata
{

	// Scripts > New Script...: a class name and a template (a behaviour with OnCreate, OnUpdate and a field, or an empty
	// class). The name is checked as it is typed (ValidateNewScriptClass, which reads the script directory: only when the
	// name changed); Create runs script.create and opens the new file in the external editor. Drawn by EditorLayer.
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

	// Scripts > External Editor...: the command that opens script sources (script.setExternalEditor), with examples, and
	// whether STRATA_EXTERNAL_EDITOR overrides it. Drawn by EditorLayer.
	class ExternalEditorDialog
	{
	public:
		static constexpr const char* c_Popup = "External Editor";

		void Open(EditorPanelContext& context);
		void Draw(EditorPanelContext& context);
		bool IsOpen() const { return m_Open; }
	private:
		bool m_OpenRequested = false;
		bool m_Open = false;
		std::string m_Command;
		std::string m_Error;
	};

}
