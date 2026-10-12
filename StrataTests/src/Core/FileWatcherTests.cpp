#include <doctest/doctest.h>

#include "Strata/Core/FileChangeNotifier.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/FileWatcher.h"
#include "Strata/Core/PlatformDetection.h"
#include "TestHelpers.h"

#include <chrono>
#include <map>
#include <thread>

using namespace Strata;

namespace
{
	FileWatcherSettings FastSettings()
	{
		FileWatcherSettings settings;
		settings.PollInterval = std::chrono::milliseconds(25);
		settings.Debounce = std::chrono::milliseconds(50);
		return settings;
	}

	// Collects changes until predicate is satisfied (or a timeout elapses). Returns the latest type per file name.
	std::map<std::string, FileChangeType> CollectChanges(FileWatcher& watcher, size_t expectedFiles)
	{
		std::map<std::string, FileChangeType> changes;
		Tests::WaitUntil([&]()
		{
			for (const FileChange& change : watcher.PollChanges())
				changes[FileSystem::ToUTF8(change.Path.filename())] = change.Type;
			return changes.size() >= expectedFiles;
		});
		return changes;
	}
}

TEST_SUITE("Core.FileWatcher")
{
	TEST_CASE("Detects added, modified and removed files")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("FileWatcher");
		REQUIRE(FileSystem::WriteText(directory / "existing.txt", "original"));

		FileWatcher watcher;
		REQUIRE(watcher.Start(directory, FastSettings()));
		CHECK(watcher.IsRunning());
		CHECK(watcher.PollChanges().empty());

		REQUIRE(FileSystem::WriteText(directory / "sub" / "added.txt", "new"));
		std::map<std::string, FileChangeType> added = CollectChanges(watcher, 1);
		REQUIRE(added.count("added.txt") == 1);
		CHECK(added["added.txt"] == FileChangeType::Added);

		REQUIRE(FileSystem::WriteText(directory / "existing.txt", "modified contents"));
		std::map<std::string, FileChangeType> modified = CollectChanges(watcher, 1);
		REQUIRE(modified.count("existing.txt") == 1);
		CHECK(modified["existing.txt"] == FileChangeType::Modified);

		REQUIRE(FileSystem::Remove(directory / "existing.txt"));
		std::map<std::string, FileChangeType> removed = CollectChanges(watcher, 1);
		REQUIRE(removed.count("existing.txt") == 1);
		CHECK(removed["existing.txt"] == FileChangeType::Removed);

		watcher.Stop();
		CHECK_FALSE(watcher.IsRunning());
	}

	TEST_CASE("Ignored directories are not reported")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("FileWatcherIgnored");
		FileWatcher watcher;
		REQUIRE(watcher.Start(directory, FastSettings()));

		REQUIRE(FileSystem::WriteText(directory / ".strata" / "cache.bin", "ignored"));
		REQUIRE(FileSystem::WriteText(directory / "visible.txt", "visible"));
		std::map<std::string, FileChangeType> changes = CollectChanges(watcher, 1);
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		for (const FileChange& change : watcher.PollChanges())
			changes[FileSystem::ToUTF8(change.Path.filename())] = change.Type;

		CHECK(changes.count("visible.txt") == 1);
		CHECK(changes.count("cache.bin") == 0);
	}

	TEST_CASE("Directories that come and go are reported when the settings ask for them, never as modified")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("FileWatcherDirectories");
		REQUIRE(FileSystem::CreateDirectories(directory / "Old"));
		FileWatcherSettings settings = FastSettings();
		settings.ReportDirectories = true;
		FileWatcher watcher;
		REQUIRE(watcher.Start(directory, settings));
		FileWatcher filesOnly;
		REQUIRE(filesOnly.Start(directory, FastSettings()));

		// The latest change per name, with whether it was a directory.
		std::map<std::string, FileChange> changes;
		auto collect = [&](size_t expected)
		{
			return Tests::WaitUntil([&]()
			{
				for (FileChange& change : watcher.PollChanges())
					changes[FileSystem::ToUTF8(change.Path.filename())] = std::move(change);
				return changes.size() >= expected;
			});
		};

		// Empty folders made in a file manager, one inside the other; an ignored one is left out.
		REQUIRE(FileSystem::CreateDirectories(directory / "Made" / "Inner"));
		REQUIRE(FileSystem::CreateDirectories(directory / ".strata" / "Cache"));
		REQUIRE(collect(2));
		REQUIRE(changes.count("Made") == 1);
		CHECK(changes["Made"].Type == FileChangeType::Added);
		CHECK(changes["Made"].Directory);
		REQUIRE(changes.count("Inner") == 1);
		CHECK(changes["Inner"].Directory);

		// A file added in one: the file, not the folder.
		changes.clear();
		REQUIRE(FileSystem::WriteText(directory / "Made" / "file.txt", "x"));
		REQUIRE(collect(1));
		REQUIRE(changes.count("file.txt") == 1);
		CHECK_FALSE(changes["file.txt"].Directory);

		// A folder removed.
		changes.clear();
		REQUIRE(FileSystem::Remove(directory / "Old"));
		REQUIRE(collect(1));
		REQUIRE(changes.count("Old") == 1);
		CHECK(changes["Old"].Type == FileChangeType::Removed);
		CHECK(changes["Old"].Directory);

		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		for (FileChange& change : watcher.PollChanges())
			changes[FileSystem::ToUTF8(change.Path.filename())] = std::move(change);
		CHECK(changes.count("Made") == 0); // Not modified by the file in it
		CHECK(changes.count(".strata") == 0);
		CHECK(changes.count("Cache") == 0);

		// Without the setting only the file was seen.
		for (const FileChange& change : filesOnly.PollChanges())
		{
			CHECK(FileSystem::ToUTF8(change.Path.filename()) == "file.txt");
			CHECK_FALSE(change.Directory);
		}
	}

	TEST_CASE("A file created and deleted before being observed produces no event")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("FileWatcherTransient");
		FileWatcherSettings settings = FastSettings();
		settings.Debounce = std::chrono::milliseconds(400);
		FileWatcher watcher;
		REQUIRE(watcher.Start(directory, settings));

		REQUIRE(FileSystem::WriteText(directory / "transient.txt", "x"));
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		REQUIRE(FileSystem::Remove(directory / "transient.txt"));
		REQUIRE(FileSystem::WriteText(directory / "marker.txt", "y"));

		std::map<std::string, FileChangeType> changes = CollectChanges(watcher, 1);
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		for (const FileChange& change : watcher.PollChanges())
			changes[FileSystem::ToUTF8(change.Path.filename())] = change.Type;

		CHECK(changes.count("marker.txt") == 1);
		CHECK(changes.count("transient.txt") == 0);
	}

	TEST_CASE("Starting on a missing directory fails")
	{
		FileWatcher watcher;
		CHECK_FALSE(watcher.Start(Tests::CreateTemporaryDirectory("FileWatcherMissing") / "missing"));
		CHECK_FALSE(watcher.IsRunning());
	}

	TEST_CASE("Stopping does not wait for the poll interval, and a stopped watcher starts again")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("FileWatcherStop");
		FileWatcherSettings settings;
		settings.PollInterval = std::chrono::hours(1);
		FileWatcher watcher;
		REQUIRE(watcher.Start(directory, settings));
		// Long enough for the thread to scan and settle into its wait.
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		const auto stopStart = std::chrono::steady_clock::now();
		watcher.Stop();
		CHECK(std::chrono::steady_clock::now() - stopStart < std::chrono::seconds(10));
		CHECK_FALSE(watcher.IsRunning());

		REQUIRE(watcher.Start(directory, FastSettings()));
		REQUIRE(FileSystem::WriteText(directory / "after-restart.txt", "x"));
		std::map<std::string, FileChangeType> changes = CollectChanges(watcher, 1);
		CHECK(changes.count("after-restart.txt") == 1);
	}

	TEST_CASE("Change notifications end waits early where the platform reports changes")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("FileChangeNotifier");
		const Scope<FileChangeNotifier> notifier = FileChangeNotifier::Create(directory, true);
#if defined(ST_PLATFORM_WINDOWS)
		REQUIRE(notifier);
		// A change in a subdirectory ends the wait (one made after the notifier started is not lost).
		REQUIRE(FileSystem::WriteText(directory / "sub" / "file.txt", "x"));
		auto waitStart = std::chrono::steady_clock::now();
		notifier->Wait(std::chrono::minutes(2));
		CHECK(std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(30));

		// Waking ends this and every later wait.
		notifier->Wake();
		waitStart = std::chrono::steady_clock::now();
		notifier->Wait(std::chrono::minutes(2));
		notifier->Wait(std::chrono::minutes(2));
		CHECK(std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(30));

		// So a watcher reports changes long before its poll interval.
		FileWatcherSettings settings;
		settings.PollInterval = std::chrono::hours(1);
		settings.Debounce = std::chrono::milliseconds(50);
		FileWatcher watcher;
		REQUIRE(watcher.Start(directory, settings));
		REQUIRE(FileSystem::WriteText(directory / "notified.txt", "x"));
		std::map<std::string, FileChangeType> changes = CollectChanges(watcher, 1);
		CHECK(changes.count("notified.txt") == 1);
#else
		// Linux and macOS: no notifications, the watcher polls.
		CHECK_FALSE(notifier);
#endif
	}
}
