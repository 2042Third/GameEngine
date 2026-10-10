#include "Panels/ConsolePanel.h"

#include <Strata/Core/StringUtils.h>

#include <imgui.h>
#include <imgui_stdlib.h>

namespace Strata
{

	void ConsolePanel::OnUpdate(EditorPanelContext&)
	{
		LogBuffer& buffer = Log::GetBuffer();
		for (LogEntry& entry : buffer.GetEntries(m_LastSequence))
		{
			m_LastSequence = entry.Sequence;
			if (entry.Level >= LogLevel::Error)
				m_UnreadErrors++;
			m_Entries.push_back(std::move(entry));
			m_EntriesChanged = true;
		}
		while (m_Entries.size() > buffer.GetCapacity())
			m_Entries.pop_front();
	}

	bool ConsolePanel::IsVisible(const LogEntry& entry, const std::string& lowerFilter) const
	{
		const bool levelShown = (entry.Level == LogLevel::Trace && m_ShowTrace) || (entry.Level == LogLevel::Info && m_ShowInfo)
			|| (entry.Level == LogLevel::Warn && m_ShowWarnings) || (entry.Level >= LogLevel::Error && m_ShowErrors);
		return levelShown && (lowerFilter.empty() || StringUtils::ToLower(entry.Message).find(lowerFilter) != std::string::npos);
	}

	void ConsolePanel::OnImGuiRender(EditorPanelContext&)
	{
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
			m_UnreadErrors = 0;

		if (ImGui::Button("Clear"))
		{
			m_Entries.clear();
			m_UnreadErrors = 0;
			m_EntriesChanged = true;
		}
		ImGui::SameLine();
		ImGui::Checkbox("Trace", &m_ShowTrace);
		ImGui::SameLine();
		ImGui::Checkbox("Info", &m_ShowInfo);
		ImGui::SameLine();
		ImGui::Checkbox("Warnings", &m_ShowWarnings);
		ImGui::SameLine();
		ImGui::Checkbox("Errors", &m_ShowErrors);
		ImGui::SameLine();
		ImGui::Checkbox("Auto-scroll", &m_AutoScroll);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##Filter", "Search", &m_Filter);
		ImGui::Separator();

		// The filtered view is rebuilt only when the entries or the filters change.
		const std::string filter = StringUtils::ToLower(m_Filter);
		const std::string key = fmt::format("{}{}{}{}|{}", m_ShowTrace, m_ShowInfo, m_ShowWarnings, m_ShowErrors, filter);
		if (m_EntriesChanged || key != m_VisibleKey)
		{
			m_Visible.clear();
			for (size_t index = 0; index < m_Entries.size(); index++)
			{
				if (IsVisible(m_Entries[index], filter))
					m_Visible.push_back(index);
			}
			m_VisibleKey = key;
			m_EntriesChanged = false;
		}

		if (ImGui::BeginChild("Messages", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
		{
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(m_Visible.size()));
			while (clipper.Step())
			{
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
				{
					const LogEntry& entry = m_Entries[m_Visible[static_cast<size_t>(row)]];
					ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
					if (entry.Level == LogLevel::Trace)
						color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
					else if (entry.Level == LogLevel::Warn)
						color = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
					else if (entry.Level >= LogLevel::Error)
						color = ImVec4(1.0f, 0.4f, 0.35f, 1.0f);
					ImGui::PushStyleColor(ImGuiCol_Text, color);
					ImGui::TextUnformatted(fmt::format("[{:8.2f}] [{}] {}", entry.Timestamp, entry.Logger, entry.Message).c_str());
					ImGui::PopStyleColor();
				}
			}
			if (m_AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
				ImGui::SetScrollHereY(1.0f);
		}
		ImGui::EndChild();
	}

}
