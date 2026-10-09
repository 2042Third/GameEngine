#pragma once

#include <Strata/Core/Log.h>

#include <deque>
#include <string>
#include <vector>

namespace Strata
{

	// Recent log messages of the engine, editor and scripts, with level filters and search.
	class ConsolePanel
	{
	public:
		void OnImGuiRender();
		// Error and critical messages that arrived since the console was last viewed (for the status bar).
		uint32_t GetUnreadErrors() const { return m_UnreadErrors; }
	private:
		// Takes new entries from the log buffer; returns true when the entries changed.
		bool Poll();
		bool IsVisible(const LogEntry& entry, const std::string& lowerFilter) const;
	private:
		std::deque<LogEntry> m_Entries;
		std::vector<size_t> m_Visible; // Indices of the entries that pass the filters
		std::string m_VisibleKey;      // Filter settings m_Visible was built with
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
