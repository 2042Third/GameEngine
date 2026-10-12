#pragma once

#include <Strata/Asset/AssetTypes.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

namespace Strata
{

	struct EditorCommandResult;

	enum class NotificationLevel : uint8_t
	{
		Info = 0,
		Success,
		Warning,
		Error
	};

	// Something the person at the editor should hear about without looking for it: a command of theirs that failed or warned,
	// a finished export or script build, a script crash, an import that failed, recovery data still on offer. The UI shows
	// notifications as toasts (UI/Notifications) with the actions they carry.
	struct EditorNotification
	{
		uint64_t Id = 0;
		NotificationLevel Level = NotificationLevel::Info;
		std::string Title;   // Short: "entity.rename failed"
		std::string Message; // The details: an error, what was written
		uint64_t JournalEntry = 0; // Show in Activity: the activity journal's entry, when there is one
		bool ShowConsole = false;  // Show in Console: the log holds more (build output, a crash's details)
		bool ShowRecovery = false; // Recover...: the recovery dialog
		AssetHandle Asset = UUID::Null(); // Show Asset: the asset it is about, in the Asset tab
		// Notifications of one group (when not empty) merge into one that shows the latest of them, e.g. one toast for the
		// imports that fail while a project opens.
		std::string Group;
		// How many times it happened while it showed: identical notifications, and those of a group, add up. Posting one that
		// stands for several sets it (at least 1).
		uint32_t Count = 1;
		std::chrono::steady_clock::time_point Expires;
	};

	// The notifications that show now, newest last: at most c_MaxActive (posting more drops the oldest), each for its
	// level's lifetime unless the person keeps it (hovers it) or dismisses it. Identical notifications (level, title and
	// message), and notifications of a group, merge into one with a count. Costs nothing while empty. Main thread only.
	class EditorNotifications
	{
	public:
		static constexpr size_t c_MaxActive = 4;
		static constexpr std::chrono::seconds c_InfoLifetime { 5 };
		static constexpr std::chrono::seconds c_WarningLifetime { 8 };
		static constexpr std::chrono::seconds c_ErrorLifetime { 12 };

		// Shows a notification (its Id and Expires are set here, its Count added to one it merges with); returns its id.
		uint64_t Post(EditorNotification notification);
		void Dismiss(uint64_t id);
		// The person looks at it: it stays at least `extra` from now.
		void KeepAlive(uint64_t id, std::chrono::steady_clock::duration extra = std::chrono::seconds(2));
		// Drops the notifications whose time is up.
		void Expire(std::chrono::steady_clock::time_point now);
		void Clear();

		bool IsEmpty() const { return m_Active.empty(); }
		const std::deque<EditorNotification>& GetActive() const { return m_Active; }
		// Changes whenever the notifications that show change.
		uint64_t GetVersion() const { return m_Version; }

		static std::chrono::steady_clock::duration GetLifetime(NotificationLevel level);
	private:
		std::deque<EditorNotification> m_Active;
		uint64_t m_NextId = 1;
		uint64_t m_Version = 0;
	};

	// Posts what a command the person ran ended with, when it failed or warned (an edit of the running game, which play.stop
	// discards): an error or a warning with the command's journal entry. Successes and cancellations post nothing.
	void NotifyCommandOutcome(EditorNotifications& notifications, std::string_view command, const EditorCommandResult& result);

}
