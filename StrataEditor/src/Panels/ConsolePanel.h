#pragma once

#include "Editor/ScriptBuild.h"
#include "UI/EditorPanelRegistry.h"

#include <Strata/Core/Log.h>

#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace Strata
{

	// Recent log messages of the engine, editor and scripts, with level filters and search. Messages that point to a source
	// file and line (compiler diagnostics, script crash locations: FindSourceReference) open it in the external editor when
	// clicked.
	class ConsolePanel : public EditorPanel
	{
	public:
		// Takes new messages from the log, also while the panel is hidden (the status bar counts unread errors).
		void OnUpdate(EditorPanelContext& context) override;
		void OnImGuiRender(EditorPanelContext& context) override;

		// Error and critical messages that arrived since the console was last viewed (for the status bar).
		uint32_t GetUnreadErrors() const { return m_UnreadErrors; }
		// The source the newest message pointing to one points to (tests), or null.
		const ScriptDiagnostic* FindLatestReference() const;
		// Opens the source of the message at an index of the shown messages, as a click does. False when it has none or it
		// cannot be opened.
		bool OpenReference(EditorPanelContext& context, size_t visibleIndex);
	private:
		bool IsVisible(const LogEntry& entry, const std::string& lowerFilter) const;
	private:
		std::deque<LogEntry> m_Entries;
		// The source each entry points to (null for most), parsed once when the entry arrives.
		std::deque<std::shared_ptr<const ScriptDiagnostic>> m_References;
		std::vector<size_t> m_Visible; // Indices of the entries that pass the filters
		std::string m_VisibleKey;      // Filter settings m_Visible was built with
		bool m_EntriesChanged = true;  // Since m_Visible was built
		uint64_t m_LastSequence = 0;
		uint32_t m_UnreadErrors = 0;
		std::string m_Filter;
		bool m_ShowTrace = false;
		bool m_ShowInfo = true;
		bool m_ShowWarnings = true;
		bool m_ShowErrors = true;
		bool m_AutoScroll = true;
	};

}
