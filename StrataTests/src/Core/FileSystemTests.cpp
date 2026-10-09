#include <doctest/doctest.h>

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Log.h"
#include "TestHelpers.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace Strata;

TEST_SUITE("Core.FileSystem")
{
	TEST_CASE("Text and binary files round trip")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("FileSystem");

		const std::filesystem::path textPath = directory / "nested" / "file.txt";
		REQUIRE(FileSystem::WriteText(textPath, "Hello, Strata!\nSecond line"));
		CHECK(FileSystem::ReadText(textPath).value() == "Hello, Strata!\nSecond line");

		const std::vector<uint8_t> bytes = { 0, 1, 2, 255, 128, 0 };
		const std::filesystem::path binaryPath = directory / "data.bin";
		REQUIRE(FileSystem::WriteBytes(binaryPath, bytes));
		CHECK(FileSystem::ReadBytes(binaryPath).value() == bytes);
		CHECK(FileSystem::GetFileSize(binaryPath).value() == bytes.size());

		// Overwrites replace the whole file.
		REQUIRE(FileSystem::WriteText(textPath, "short"));
		CHECK(FileSystem::ReadText(textPath).value() == "short");

		// Empty files are valid.
		REQUIRE(FileSystem::WriteText(directory / "empty.txt", ""));
		CHECK(FileSystem::ReadText(directory / "empty.txt").value().empty());

		// No temporary files are left behind by atomic writes.
		size_t fileCount = 0;
		for (const auto& entry : std::filesystem::directory_iterator(directory))
		{
			if (entry.is_regular_file())
				fileCount++;
		}
		CHECK(fileCount == 2);
	}

	TEST_CASE("Reading a missing file fails gracefully")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("FileSystemMissing");
		CHECK_FALSE(FileSystem::ReadText(directory / "missing.txt").has_value());
		CHECK_FALSE(FileSystem::ReadBytes(directory / "missing.bin").has_value());
		CHECK_FALSE(FileSystem::GetLastWriteTime(directory / "missing.bin").has_value());
	}

	TEST_CASE("Reading a directory as a file fails gracefully")
	{
		// On Linux a stream opens a directory, and seeking to its end can report an enormous size.
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("FileSystemDirectory");
		CHECK_FALSE(FileSystem::ReadText(directory).has_value());
		CHECK_FALSE(FileSystem::ReadBytes(directory).has_value());
	}

	TEST_CASE("UTF-8 byte order mark is stripped")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("FileSystemBom");
		const std::vector<uint8_t> bytes = { 0xEF, 0xBB, 0xBF, 'h', 'i' };
		REQUIRE(FileSystem::WriteBytes(directory / "bom.txt", bytes));
		CHECK(FileSystem::ReadText(directory / "bom.txt").value() == "hi");
	}

	TEST_CASE("UTF-8 paths are preserved")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("FileSystemUnicode");
		const std::string name = "\xE6\xB5\x8B\xE8\xAF\x95_\xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB.txt"; // "测试_файл.txt"
		const std::filesystem::path path = directory / FileSystem::FromUTF8(name);
		REQUIRE(FileSystem::WriteText(path, "unicode"));
		CHECK(FileSystem::Exists(path));
		CHECK(FileSystem::ToUTF8(path.filename()) == name);
		CHECK(FileSystem::ReadText(path).value() == "unicode");
	}

	TEST_CASE("Trailing separators are removed, roots are kept")
	{
		using std::filesystem::path;
		CHECK(FileSystem::RemoveTrailingSeparators(path("a/b/")) == path("a/b"));
		CHECK(FileSystem::RemoveTrailingSeparators(path("a/b//")) == path("a/b"));
		CHECK(FileSystem::RemoveTrailingSeparators(path("a/b")) == path("a/b"));
		CHECK(FileSystem::RemoveTrailingSeparators(path("a/")) == path("a"));
		CHECK(FileSystem::RemoveTrailingSeparators(path("/")) == path("/"));
		CHECK(FileSystem::RemoveTrailingSeparators(path()).empty());
#if defined(ST_PLATFORM_WINDOWS)
		CHECK(FileSystem::RemoveTrailingSeparators(path(L"C:\\Data\\")) == path(L"C:\\Data"));
		CHECK(FileSystem::RemoveTrailingSeparators(path(L"C:\\")) == path(L"C:\\"));
#endif
	}

	TEST_CASE("Relative paths, containment and unique names")
	{
		const std::filesystem::path base = FileSystem::FromUTF8("/project/Assets");
		CHECK(FileSystem::ToUTF8(FileSystem::GetRelativePath(FileSystem::FromUTF8("/project/Assets/Models/a.gltf"), base)) == "Models/a.gltf");
		CHECK(FileSystem::GetRelativePath(FileSystem::FromUTF8("/project/Other/a.gltf"), base).empty());
		CHECK(FileSystem::IsInside(FileSystem::FromUTF8("/project/Assets/x/../y.png"), base));
		CHECK_FALSE(FileSystem::IsInside(FileSystem::FromUTF8("/project/Assets/../y.png"), base));

		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("FileSystemUnique");
		const std::filesystem::path desired = directory / "Scene.stscene";
		CHECK(FileSystem::GetUniquePath(desired) == desired);
		REQUIRE(FileSystem::WriteText(desired, "{}"));
		CHECK(FileSystem::GetUniquePath(desired).filename() == "Scene (1).stscene");
	}

	TEST_CASE("Resolved containment follows symbolic links")
	{
		const std::filesystem::path root = Tests::CreateTemporaryDirectory("FileSystemLinks");
		const std::filesystem::path base = root / "Assets";
		REQUIRE(FileSystem::WriteText(base / "Inside.txt", "inside"));
		REQUIRE(FileSystem::WriteText(root / "Outside.txt", "outside"));
		CHECK(FileSystem::IsInsideResolved(base / "Inside.txt", base));
		CHECK(FileSystem::IsInsideResolved(base / "Missing.txt", base)); // Not yet existing files resolve lexically
		CHECK_FALSE(FileSystem::IsInsideResolved(base / ".." / "Outside.txt", base));

		// Creating links needs privileges on Windows (developer mode); the check is skipped without them.
		std::error_code error;
		std::filesystem::create_symlink(root / "Outside.txt", base / "Escape.txt", error);
		if (error)
		{
			MESSAGE("Symbolic links cannot be created here; skipping the link checks: " << error.message());
			return;
		}
		CHECK(FileSystem::IsInside(base / "Escape.txt", base)); // Lexically inside...
		CHECK_FALSE(FileSystem::IsInsideResolved(base / "Escape.txt", base)); // ...but leads outside
	}

	TEST_CASE("Copy, rename and remove")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("FileSystemOps");
		REQUIRE(FileSystem::WriteText(directory / "a" / "one.txt", "1"));
		REQUIRE(FileSystem::Copy(directory / "a" / "one.txt", directory / "b" / "two.txt"));
		CHECK(FileSystem::ReadText(directory / "b" / "two.txt").value() == "1");

		REQUIRE(FileSystem::Rename(directory / "b" / "two.txt", directory / "c" / "three.txt"));
		CHECK_FALSE(FileSystem::Exists(directory / "b" / "two.txt"));
		CHECK(FileSystem::Exists(directory / "c" / "three.txt"));

		REQUIRE(FileSystem::CopyDirectory(directory / "a", directory / "d"));
		CHECK(FileSystem::Exists(directory / "d" / "one.txt"));

		REQUIRE(FileSystem::Remove(directory / "a"));
		CHECK_FALSE(FileSystem::Exists(directory / "a"));
		CHECK(FileSystem::IsDirectory(directory / "d"));
		CHECK(FileSystem::IsRegularFile(directory / "d" / "one.txt"));
	}

	TEST_CASE("CopyDirectory copies nested directories into existing ones")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("FileSystemCopyDirectory");
		REQUIRE(FileSystem::WriteText(directory / "Source" / "Top.txt", "top"));
		REQUIRE(FileSystem::WriteText(directory / "Source" / "Nested" / "Deeper" / "Leaf.txt", "leaf"));
		REQUIRE(FileSystem::CreateDirectories(directory / "Source" / "Empty"));
		REQUIRE(FileSystem::WriteText(directory / "Target" / "Top.txt", "old"));
		REQUIRE(FileSystem::WriteText(directory / "Target" / "Kept.txt", "kept"));

		REQUIRE(FileSystem::CopyDirectory(directory / "Source", directory / "Target"));
		CHECK(FileSystem::ReadText(directory / "Target" / "Top.txt") == "top"); // Overwritten
		CHECK(FileSystem::ReadText(directory / "Target" / "Nested" / "Deeper" / "Leaf.txt") == "leaf");
		CHECK(FileSystem::IsDirectory(directory / "Target" / "Empty"));
		CHECK(FileSystem::ReadText(directory / "Target" / "Kept.txt") == "kept");

		CHECK_FALSE(FileSystem::CopyDirectory(directory / "Missing", directory / "Other"));
		CHECK_FALSE(FileSystem::CopyDirectory(directory / "Source" / "Top.txt", directory / "Other"));
	}

	TEST_CASE("CopyDirectory copies from the source tree's file system")
	{
		// The checkout may live on another volume and file system than the temporary directory (on Windows the
		// standard library's directory copy failed for exFAT sources).
		const std::filesystem::path source = FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "StrataScriptCore" / "Include";
		const std::filesystem::path target = Tests::CreateTemporaryDirectory("FileSystemCopySourceTree") / "Include";
		std::string error;
		REQUIRE_MESSAGE(FileSystem::CopyDirectory(source, target, &error), error);
		CHECK(FileSystem::ReadBytes(target / "StrataScript" / "ScriptABI.h") == FileSystem::ReadBytes(source / "StrataScript" / "ScriptABI.h"));

		// Again, overwriting every file.
		REQUIRE(FileSystem::WriteText(target / "StrataScript" / "ScriptABI.h", "stale"));
		REQUIRE_MESSAGE(FileSystem::CopyDirectory(source, target, &error), error);
		CHECK(FileSystem::ReadBytes(target / "StrataScript" / "ScriptABI.h") == FileSystem::ReadBytes(source / "StrataScript" / "ScriptABI.h"));
	}

	TEST_CASE("CopyDirectory fails when an entry cannot be copied")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("FileSystemCopyDirectoryErrors");
		REQUIRE(FileSystem::WriteText(directory / "Source" / "Top.txt", "top"));
		REQUIRE(FileSystem::WriteText(directory / "Source" / "Nested" / "Leaf.txt", "leaf"));

		// A directory where a file goes.
		REQUIRE(FileSystem::CreateDirectories(directory / "FileBlocked" / "Top.txt"));
		std::string error;
		CHECK_FALSE(FileSystem::CopyDirectory(directory / "Source", directory / "FileBlocked", &error));
		CHECK(error.find("Top.txt") != std::string::npos);
		CHECK(FileSystem::IsDirectory(directory / "FileBlocked" / "Top.txt"));

		// A file where a directory goes.
		REQUIRE(FileSystem::WriteText(directory / "DirectoryBlocked" / "Nested", "file"));
		error.clear();
		CHECK_FALSE(FileSystem::CopyDirectory(directory / "Source", directory / "DirectoryBlocked", &error));
		CHECK(error.find("Nested") != std::string::npos);

		// A file where the destination goes.
		REQUIRE(FileSystem::WriteText(directory / "TargetIsFile", "file"));
		error.clear();
		CHECK_FALSE(FileSystem::CopyDirectory(directory / "Source", directory / "TargetIsFile", &error));
		CHECK_FALSE(error.empty());
		CHECK(FileSystem::ReadText(directory / "TargetIsFile") == "file");

		error.clear();
		CHECK_FALSE(FileSystem::CopyDirectory(directory / "Missing", directory / "Other", &error));
		CHECK(error.find("Missing") != std::string::npos);
		CHECK_FALSE(FileSystem::Exists(directory / "Other"));
	}

	TEST_CASE("CopyDirectory refuses to copy a directory into itself")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("FileSystemCopyIntoSource");
		REQUIRE(FileSystem::WriteText(directory / "Source" / "Nested" / "Leaf.txt", "leaf"));

		std::string error;
		CHECK_FALSE(FileSystem::CopyDirectory(directory / "Source", directory / "Source" / "Copy", &error));
		CHECK(error.find("into itself") != std::string::npos);
		CHECK_FALSE(FileSystem::Exists(directory / "Source" / "Copy"));
		CHECK_FALSE(FileSystem::CopyDirectory(directory / "Source", directory / "Source" / "Nested" / ".." / "Nested" / "Copy"));
		CHECK_FALSE(FileSystem::CopyDirectory(directory / "Source", directory / "Source"));
		CHECK_FALSE(FileSystem::CopyDirectory(directory / "Source" / "Nested" / "..", directory / "Source" / "Nested"));

		// Next to the source is fine.
		REQUIRE_MESSAGE(FileSystem::CopyDirectory(directory / "Source", directory / "SourceCopy", &error), error);
		CHECK(FileSystem::ReadText(directory / "SourceCopy" / "Nested" / "Leaf.txt") == "leaf");
	}

	TEST_CASE("CopyDirectory neither follows nor writes through links")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("FileSystemCopyLinks");
		const std::filesystem::path source = directory / "Source";
		const std::filesystem::path outside = directory / "Outside";
		REQUIRE(FileSystem::WriteText(source / "Real.txt", "real"));
		REQUIRE(FileSystem::WriteText(outside / "Secret.txt", "secret"));

		// Each kind of link the platform lets this process create, as `Source/<name>`.
		std::vector<std::string> links;
		std::error_code linkError;
		std::filesystem::create_symlink(outside / "Secret.txt", source / "FileLink.txt", linkError);
		if (!linkError)
			links.push_back("FileLink.txt");
		std::filesystem::create_directory_symlink(outside, source / "DirectoryLink", linkError);
		if (!linkError)
			links.push_back("DirectoryLink");
#if defined(ST_PLATFORM_WINDOWS)
		if (Tests::CreateJunction(source / "Junction", outside))
			links.push_back("Junction");
#endif
		if (links.empty())
		{
			MESSAGE("Links cannot be created here; skipping the link checks: " << linkError.message());
			return;
		}

		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		std::string error;
		REQUIRE_MESSAGE(FileSystem::CopyDirectory(source, directory / "Target", &error), error);
		CHECK(FileSystem::ReadText(directory / "Target" / "Real.txt") == "real");
		const std::vector<LogEntry> log = Log::GetBuffer().GetEntries(logStart);
		for (const std::string& link : links)
		{
			INFO("Link ", link);
			std::error_code statusError;
			CHECK(std::filesystem::symlink_status(directory / "Target" / link, statusError).type() == std::filesystem::file_type::not_found);
			// Skipping it was reported.
			CHECK(std::any_of(log.begin(), log.end(), [&](const LogEntry& entry)
			{
				return entry.Level == LogLevel::Warn && entry.Message.find("skipped") != std::string::npos && entry.Message.find(link) != std::string::npos;
			}));
		}
		CHECK_FALSE(FileSystem::Exists(directory / "Target" / "Secret.txt"));

		// A link in the destination where a file or directory of the source goes is not written through.
		const std::string& link = links.back();
		REQUIRE(FileSystem::WriteText(directory / "LinkSource" / link / "Secret.txt", "overwritten"));
		CHECK_FALSE(FileSystem::CopyDirectory(directory / "LinkSource", source, &error));
		CHECK(error.find(link) != std::string::npos);
		CHECK(FileSystem::ReadText(outside / "Secret.txt") == "secret");

		// Links are removed themselves, not what they point to.
		for (const std::string& name : links)
		{
			std::error_code removeError;
			std::filesystem::remove(source / name, removeError);
		}
		CHECK(FileSystem::ReadText(outside / "Secret.txt") == "secret");
	}
}
