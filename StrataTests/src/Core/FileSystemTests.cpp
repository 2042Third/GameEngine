#include <doctest/doctest.h>

#include "Strata/Core/FileSystem.h"
#include "TestHelpers.h"

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
}
