#pragma once

#include "Strata/Core/Base.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace Strata
{

	enum class FileChangeType : uint8_t
	{
		Added,
		Modified,
		Removed
	};

	const char* FileChangeTypeToString(FileChangeType type);

	struct FileChange
	{
		std::filesystem::path Path; // Absolute path of the changed file
		FileChangeType Type = FileChangeType::Modified;
	};

	struct FileWatcherSettings
	{
		// Rescan interval when the platform offers no change notifications (and a safety-net interval otherwise).
		std::chrono::milliseconds PollInterval = std::chrono::milliseconds(500);
		// A change is reported only after the file has been stable (size and timestamp) for this long,
		// so consumers never see a file that is still being written.
		std::chrono::milliseconds Debounce = std::chrono::milliseconds(150);
		bool Recursive = true;
		// Directory names skipped during scans (matched against each path component).
		std::vector<std::string> IgnoredDirectories = { ".git", ".strata" };
	};

	// Watches a directory tree for file additions, modifications and removals on a background thread.
	// Changes are queued and collected on the caller's thread with PollChanges(); no callbacks run on the
	// background thread. Only regular files are reported.
	class FileWatcher
	{
	public:
		FileWatcher();
		~FileWatcher();

		FileWatcher(const FileWatcher&) = delete;
		FileWatcher& operator=(const FileWatcher&) = delete;

		bool Start(const std::filesystem::path& directory, const FileWatcherSettings& settings = {});
		void Stop();
		bool IsRunning() const;
		const std::filesystem::path& GetDirectory() const;

		// Returns the changes detected since the previous call, coalesced per file, oldest first.
		std::vector<FileChange> PollChanges();
	private:
		struct Impl;
		Scope<Impl> m_Impl;
	};

}
