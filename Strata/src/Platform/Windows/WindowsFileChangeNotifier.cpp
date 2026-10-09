#include "stpch.h"
#include "Strata/Core/FileChangeNotifier.h"

#include "Strata/Core/FileSystem.h"

#include "Platform/Windows/WindowsUtils.h"

namespace Strata
{

	namespace
	{

		// A change notification for the directory tree, and a manual-reset event that ends waits for good once set.
		class WindowsFileChangeNotifier final : public FileChangeNotifier
		{
		public:
			WindowsFileChangeNotifier(HANDLE notification, HANDLE wakeEvent)
				: m_Notification(notification), m_WakeEvent(wakeEvent)
			{
			}

			~WindowsFileChangeNotifier() override
			{
				FindCloseChangeNotification(m_Notification);
				CloseHandle(m_WakeEvent);
			}

			WindowsFileChangeNotifier(const WindowsFileChangeNotifier&) = delete;
			WindowsFileChangeNotifier& operator=(const WindowsFileChangeNotifier&) = delete;

			void Wait(std::chrono::milliseconds timeout) override
			{
				HANDLE handles[2] = { m_WakeEvent, m_Notification };
				const DWORD result = WaitForMultipleObjects(2, handles, FALSE, static_cast<DWORD>(timeout.count()));
				// A reported change has to be acknowledged before the next one can be reported.
				if (result == WAIT_OBJECT_0 + 1)
					FindNextChangeNotification(m_Notification);
			}

			void Wake() override
			{
				SetEvent(m_WakeEvent);
			}
		private:
			HANDLE m_Notification;
			HANDLE m_WakeEvent;
		};

	}

	Scope<FileChangeNotifier> FileChangeNotifier::Create(const std::filesystem::path& directory, bool recursive)
	{
		HANDLE notification = FindFirstChangeNotificationW(directory.c_str(), recursive ? TRUE : FALSE,
			FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
		HANDLE wakeEvent = notification != INVALID_HANDLE_VALUE ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
		if (!wakeEvent)
		{
			if (notification != INVALID_HANDLE_VALUE)
				FindCloseChangeNotification(notification);
			ST_CORE_WARN("FileWatcher: change notifications unavailable for '{}', falling back to polling", FileSystem::ToUTF8(directory));
			return nullptr;
		}
		return CreateScope<WindowsFileChangeNotifier>(notification, wakeEvent);
	}

}
