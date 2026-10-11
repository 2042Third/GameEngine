#include "Editor/EditorNotifications.h"

#include "Editor/EditorCommands.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <utility>

namespace Strata
{

	uint64_t EditorNotifications::Post(EditorNotification notification)
	{
		const auto now = std::chrono::steady_clock::now();
		m_Version++;
		const auto same = std::find_if(m_Active.begin(), m_Active.end(), [&notification](const EditorNotification& active)
		{
			return active.Level == notification.Level && active.Title == notification.Title && active.Message == notification.Message;
		});
		if (same != m_Active.end())
		{
			// Again while it shows: counted, kept longer, and pointing at the latest journal entry.
			EditorNotification merged = std::move(*same);
			m_Active.erase(same);
			merged.Count++;
			merged.Expires = now + GetLifetime(merged.Level);
			if (notification.JournalEntry != 0)
				merged.JournalEntry = notification.JournalEntry;
			m_Active.push_back(std::move(merged));
			return m_Active.back().Id;
		}
		notification.Id = m_NextId++;
		notification.Count = 1;
		notification.Expires = now + GetLifetime(notification.Level);
		m_Active.push_back(std::move(notification));
		while (m_Active.size() > c_MaxActive)
			m_Active.pop_front();
		return m_Active.back().Id;
	}

	void EditorNotifications::Dismiss(uint64_t id)
	{
		const size_t before = m_Active.size();
		std::erase_if(m_Active, [id](const EditorNotification& notification) { return notification.Id == id; });
		if (m_Active.size() != before)
			m_Version++;
	}

	void EditorNotifications::KeepAlive(uint64_t id, std::chrono::steady_clock::duration extra)
	{
		const auto until = std::chrono::steady_clock::now() + extra;
		for (EditorNotification& notification : m_Active)
		{
			if (notification.Id == id)
				notification.Expires = std::max(notification.Expires, until);
		}
	}

	void EditorNotifications::Expire(std::chrono::steady_clock::time_point now)
	{
		const size_t before = m_Active.size();
		std::erase_if(m_Active, [now](const EditorNotification& notification) { return notification.Expires <= now; });
		if (m_Active.size() != before)
			m_Version++;
	}

	void EditorNotifications::Clear()
	{
		if (m_Active.empty())
			return;
		m_Active.clear();
		m_Version++;
	}

	std::chrono::steady_clock::duration EditorNotifications::GetLifetime(NotificationLevel level)
	{
		switch (level)
		{
			case NotificationLevel::Info:
			case NotificationLevel::Success: return c_InfoLifetime;
			case NotificationLevel::Warning: return c_WarningLifetime;
			case NotificationLevel::Error:   return c_ErrorLifetime;
		}
		return c_InfoLifetime;
	}

	void NotifyCommandOutcome(EditorNotifications& notifications, std::string_view command, const EditorCommandResult& result)
	{
		if (result.IsPending())
			return;
		EditorNotification notification;
		notification.JournalEntry = result.JournalId;
		if (!result.Success)
		{
			if (result.ErrorKind == EditorCommandError::Cancelled)
				return;
			notification.Level = NotificationLevel::Error;
			notification.Title = fmt::format("{} failed", command);
			notification.Message = result.Error;
		}
		else if (result.Value.is_object() && result.Value.contains("warning") && result.Value["warning"].is_string())
		{
			notification.Level = NotificationLevel::Warning;
			notification.Title = std::string(command);
			notification.Message = result.Value["warning"].get<std::string>();
		}
		else
		{
			return;
		}
		notification.ShowConsole = true;
		notifications.Post(std::move(notification));
	}

}
