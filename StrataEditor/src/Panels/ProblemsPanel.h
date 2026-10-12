#pragma once

#include "UI/EditorPanelRegistry.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Strata
{

	// The project's script problems: the last script build's diagnostics (errors and warnings with their file, line,
	// column and message) and the crash that stopped play mode (where it happened, then its stack). A double click opens an
	// entry's source in the external editor (EditorContext::OpenScriptSource). The status bar's "Build failed" and
	// "Scripts crashed" pills open the panel, and so does a play whose build failed (EditorContext::RequestProblemsPanel).
	// The rows are rebuilt only when the build or the crash changed; an idle frame compares two numbers.
	class ProblemsPanel : public EditorPanel
	{
	public:
		enum class RowKind : uint8_t
		{
			Error = 0,
			Warning,
			Crash,
			Frame // A frame of the crash's stack
		};

		struct Row
		{
			RowKind Kind = RowKind::Error;
			std::string Text;            // The message, or the crash's description, or the frame's function
			std::string File;            // As the compiler or the symbolizer named it (empty when unknown)
			std::string ShownFile;       // What the File column shows: relative to the project when it is the project's
			uint32_t Line = 0;
			uint32_t Column = 0;
			std::filesystem::path Source; // The file to open (resolved against the project), empty when none exists
		};

		void OnUpdate(EditorPanelContext& context) override;
		void OnImGuiRender(EditorPanelContext& context) override;

		const std::vector<Row>& GetRows() const { return m_Rows; }
		size_t GetErrorCount() const { return m_ErrorCount; }
		// The line over the rows: the crash first (where it happened), then the last build's outcome.
		const std::string& GetSummary() const { return m_Summary; }
		// Opens a row's source (as a double click does). False, with the reason in GetLastOpenError, when it cannot.
		bool OpenRow(EditorPanelContext& context, size_t index);
		const std::string& GetLastOpenError() const { return m_LastOpenError; }
	private:
		void Rebuild(EditorPanelContext& context);
	private:
		std::vector<Row> m_Rows;
		size_t m_ErrorCount = 0;
		size_t m_WarningCount = 0;
		bool m_HasCrash = false;
		std::string m_Summary;
		// What the rows were built from.
		uint64_t m_BuildID = UINT64_MAX;
		std::string m_FaultKey;
		bool m_Built = false;
		// The File column's texts, cut at their start to the column's width (their file names stay visible), computed again
		// only when the rows or the width change.
		std::vector<std::string> m_FileTexts;
		float m_FileTextsWidth = -1.0f;
		std::string m_LastOpenError; // Cleared when the rows are rebuilt
	};

}
