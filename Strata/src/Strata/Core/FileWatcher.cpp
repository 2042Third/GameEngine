#include "stpch.h"
#include "Strata/Core/FileWatcher.h"

#include "Strata/Core/FileChangeNotifier.h"
#include "Strata/Core/FileSystem.h"

#include <condition_variable>
#include <map>
#include <thread>

namespace Strata
{

	const char* FileChangeTypeToString(FileChangeType type)
	{
		switch (type)
		{
			case FileChangeType::Added:    return "Added";
			case FileChangeType::Modified: return "Modified";
			case FileChangeType::Removed:  return "Removed";
		}
		return "Unknown";
	}

	namespace
	{

		struct FileState
		{
			uint64_t Size = 0;
			int64_t WriteTime = 0;

			bool operator==(const FileState& other) const = default;
		};

		using Snapshot = std::map<std::filesystem::path, FileState>;

		struct PendingChange
		{
			FileChangeType Type = FileChangeType::Modified;
			FileState LastState;
			std::chrono::steady_clock::time_point LastChange;
		};

		// Merges two consecutive changes to the same file. Returns nullopt when they cancel out
		// (a file created and deleted again before anyone observed it).
		std::optional<FileChangeType> CoalesceChanges(FileChangeType previous, FileChangeType next)
		{
			if (previous == FileChangeType::Added && next == FileChangeType::Removed)
				return std::nullopt;
			if (previous == FileChangeType::Added && next == FileChangeType::Modified)
				return FileChangeType::Added;
			if (previous == FileChangeType::Removed && next == FileChangeType::Added)
				return FileChangeType::Modified;
			return next;
		}

		bool IsIgnored(const std::filesystem::path& relativePath, const std::vector<std::string>& ignoredDirectories)
		{
			for (const std::filesystem::path& component : relativePath)
			{
				const std::string name = component.string();
				for (const std::string& ignored : ignoredDirectories)
				{
					if (name == ignored)
						return true;
				}
			}
			return false;
		}

		Snapshot ScanDirectory(const std::filesystem::path& root, const FileWatcherSettings& settings)
		{
			Snapshot snapshot;
			std::error_code error;
			auto addEntry = [&](const std::filesystem::directory_entry& entry)
			{
				std::error_code entryError;
				if (!entry.is_regular_file(entryError))
					return;

				const std::filesystem::path relative = entry.path().lexically_relative(root);
				if (IsIgnored(relative.parent_path(), settings.IgnoredDirectories))
					return;

				FileState state;
				state.Size = static_cast<uint64_t>(entry.file_size(entryError));
				state.WriteTime = static_cast<int64_t>(entry.last_write_time(entryError).time_since_epoch().count());
				snapshot.emplace(entry.path(), state);
			};

			if (settings.Recursive)
			{
				std::filesystem::recursive_directory_iterator iterator(root, std::filesystem::directory_options::skip_permission_denied, error);
				for (; !error && iterator != std::filesystem::recursive_directory_iterator(); iterator.increment(error))
				{
					const std::filesystem::directory_entry& entry = *iterator;
					std::error_code entryError;
					if (entry.is_directory(entryError))
					{
						const std::string name = entry.path().filename().string();
						for (const std::string& ignored : settings.IgnoredDirectories)
						{
							if (name == ignored)
							{
								iterator.disable_recursion_pending();
								break;
							}
						}
						continue;
					}
					addEntry(entry);
				}
			}
			else
			{
				for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(root, error))
					addEntry(entry);
			}
			return snapshot;
		}

	}

	struct FileWatcher::Impl
	{
		std::filesystem::path Directory;
		FileWatcherSettings Settings;

		std::thread Thread;
		std::mutex Mutex;
		std::condition_variable Condition;
		bool StopRequested = false;
		bool Running = false;

		// Owned by the watcher thread
		Snapshot CurrentSnapshot;
		std::map<std::filesystem::path, PendingChange> Pending;

		// Shared with PollChanges (guarded by Mutex)
		std::vector<FileChange> ReadyChanges;

		// Ends waits early when the platform reports changes; null where it does not (the thread then polls). Created
		// before the thread starts and destroyed after it ended.
		Scope<FileChangeNotifier> Notifier;

		void Run();
		void Rescan();
		void PromoteStableChanges();
		void WaitForWork(std::chrono::milliseconds timeout);
	};

	void FileWatcher::Impl::Rescan()
	{
		Snapshot snapshot = ScanDirectory(Directory, Settings);
		const auto now = std::chrono::steady_clock::now();

		auto markPending = [&](const std::filesystem::path& path, FileChangeType type, const FileState& state)
		{
			auto it = Pending.find(path);
			if (it == Pending.end())
			{
				Pending.emplace(path, PendingChange { type, state, now });
				return;
			}

			const std::optional<FileChangeType> merged = CoalesceChanges(it->second.Type, type);
			if (!merged)
			{
				Pending.erase(it);
				return;
			}

			it->second.Type = *merged;
			it->second.LastState = state;
			it->second.LastChange = now;
		};

		for (const auto& [path, state] : snapshot)
		{
			auto previous = CurrentSnapshot.find(path);
			if (previous == CurrentSnapshot.end())
				markPending(path, FileChangeType::Added, state);
			else if (!(previous->second == state))
				markPending(path, FileChangeType::Modified, state);
		}

		for (const auto& [path, state] : CurrentSnapshot)
		{
			if (snapshot.find(path) == snapshot.end())
				markPending(path, FileChangeType::Removed, state);
		}

		CurrentSnapshot = std::move(snapshot);
	}

	void FileWatcher::Impl::PromoteStableChanges()
	{
		const auto now = std::chrono::steady_clock::now();
		std::vector<FileChange> stable;
		for (auto it = Pending.begin(); it != Pending.end();)
		{
			if (now - it->second.LastChange >= Settings.Debounce)
			{
				stable.push_back(FileChange { it->first, it->second.Type });
				it = Pending.erase(it);
			}
			else
			{
				++it;
			}
		}

		if (stable.empty())
			return;

		std::scoped_lock<std::mutex> lock(Mutex);
		for (FileChange& change : stable)
		{
			// Keep one queued entry per path by merging with a change that has not been polled yet.
			auto existing = std::find_if(ReadyChanges.begin(), ReadyChanges.end(), [&](const FileChange& queued) { return queued.Path == change.Path; });
			if (existing == ReadyChanges.end())
			{
				ReadyChanges.push_back(std::move(change));
				continue;
			}

			const std::optional<FileChangeType> merged = CoalesceChanges(existing->Type, change.Type);
			if (merged)
				existing->Type = *merged;
			else
				ReadyChanges.erase(existing);
		}
	}

	void FileWatcher::Impl::WaitForWork(std::chrono::milliseconds timeout)
	{
		if (Notifier)
		{
			Notifier->Wait(timeout);
			return;
		}
		std::unique_lock<std::mutex> lock(Mutex);
		Condition.wait_for(lock, timeout, [this]() { return StopRequested; });
	}

	void FileWatcher::Impl::Run()
	{
		while (true)
		{
			{
				std::scoped_lock<std::mutex> lock(Mutex);
				if (StopRequested)
					return;
			}

			Rescan();
			PromoteStableChanges();

			// While changes are settling, wake up in time to confirm them; otherwise sleep a full interval
			// (or until the OS reports a change).
			const std::chrono::milliseconds timeout = Pending.empty() ? Settings.PollInterval : std::min(Settings.PollInterval, Settings.Debounce);
			WaitForWork(timeout);
		}
	}

	FileWatcher::FileWatcher()
		: m_Impl(CreateScope<Impl>())
	{
	}

	FileWatcher::~FileWatcher()
	{
		Stop();
	}

	bool FileWatcher::Start(const std::filesystem::path& directory, const FileWatcherSettings& settings)
	{
		Stop();

		std::error_code error;
		if (!std::filesystem::is_directory(directory, error))
		{
			ST_CORE_ERROR("FileWatcher: '{}' is not a directory", FileSystem::ToUTF8(directory));
			return false;
		}

		m_Impl->Directory = std::filesystem::absolute(directory, error).lexically_normal();
		m_Impl->Settings = settings;
		m_Impl->StopRequested = false;
		m_Impl->Pending.clear();
		m_Impl->ReadyChanges.clear();
		m_Impl->CurrentSnapshot = ScanDirectory(m_Impl->Directory, settings);
		m_Impl->Notifier = FileChangeNotifier::Create(m_Impl->Directory, settings.Recursive);

		m_Impl->Running = true;
		m_Impl->Thread = std::thread([impl = m_Impl.get()]() { impl->Run(); });
		return true;
	}

	void FileWatcher::Stop()
	{
		if (!m_Impl->Running)
			return;

		{
			std::scoped_lock<std::mutex> lock(m_Impl->Mutex);
			m_Impl->StopRequested = true;
		}
		m_Impl->Condition.notify_all();
		if (m_Impl->Notifier)
			m_Impl->Notifier->Wake();
		m_Impl->Thread.join();

		m_Impl->Notifier.reset();
		m_Impl->Running = false;
	}

	bool FileWatcher::IsRunning() const
	{
		return m_Impl->Running;
	}

	const std::filesystem::path& FileWatcher::GetDirectory() const
	{
		return m_Impl->Directory;
	}

	std::vector<FileChange> FileWatcher::PollChanges()
	{
		std::scoped_lock<std::mutex> lock(m_Impl->Mutex);
		return std::exchange(m_Impl->ReadyChanges, {});
	}

}
