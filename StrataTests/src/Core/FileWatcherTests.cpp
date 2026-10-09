#include <doctest/doctest.h>

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/FileWatcher.h"
#include "TestHelpers.h"

#include <map>

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
}
