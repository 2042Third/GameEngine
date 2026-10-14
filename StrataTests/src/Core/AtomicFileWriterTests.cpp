#include <doctest/doctest.h>

#include "Strata/Core/AtomicFileWriter.h"
#include "Strata/Core/FileSystem.h"
#include "TestHelpers.h"

#include <filesystem>
#include <string>
#include <system_error>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	size_t CountFiles(const std::filesystem::path& directory)
	{
		size_t count = 0;
		std::error_code error;
		for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
			count++;
		return count;
	}

}

TEST_SUITE("Core.AtomicFileWriter")
{
	TEST_CASE("A committed write replaces the file with everything written, through and past its buffer")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("AtomicWriter");
		const std::filesystem::path path = directory / "Folder" / "Document.txt";
		REQUIRE(FileSystem::WriteText(path, "old contents"));

		// Small pieces gather in the buffer; a piece larger than it goes straight out; the total crosses it several times.
		std::string expected;
		AtomicFileWriter writer(path);
		REQUIRE(writer.IsOpen());
		CHECK(writer.GetTemporaryPath().parent_path() == path.parent_path());
		for (int index = 0; index < 1000; index++)
		{
			const std::string piece = "line " + std::to_string(index) + "\n";
			CHECK(writer.Write(piece));
			expected += piece;
		}
		const std::string large(AtomicFileWriter::c_BufferSize + 12345, 'x');
		CHECK(writer.Write(large));
		expected += large;
		CHECK(writer.Write("end"));
		expected += "end";
		CHECK(writer.GetBytesWritten() == expected.size());

		// Nothing replaces the file before the commit.
		CHECK(FileSystem::ReadText(path) == std::optional<std::string>("old contents"));
		std::string error;
		CHECK(writer.Finish(&error));
		CHECK(FileSystem::ReadText(path) == std::optional<std::string>("old contents"));
		CHECK(writer.Commit(&error));
		CHECK(writer.IsCommitted());
		CHECK(FileSystem::ReadText(path) == std::optional<std::string>(expected));
		CHECK_FALSE(FileSystem::Exists(writer.GetTemporaryPath()));
		CHECK(CountFiles(path.parent_path()) == 1);
		// Committed is final: writing more fails, aborting changes nothing.
		CHECK_FALSE(writer.Write("more"));
		writer.Abort();
		CHECK(FileSystem::ReadText(path) == std::optional<std::string>(expected));
	}

	TEST_CASE("An aborted or abandoned write leaves the file as it was and removes its temporary file")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("AtomicWriterAbort");
		const std::filesystem::path path = directory / "Document.txt";
		REQUIRE(FileSystem::WriteText(path, "kept"));
		{
			AtomicFileWriter writer(path);
			REQUIRE(writer.IsOpen());
			CHECK(writer.Write(std::string(3 * AtomicFileWriter::c_BufferSize, 'y')));
			CHECK(FileSystem::Exists(writer.GetTemporaryPath()));
			writer.Abort();
			CHECK_FALSE(FileSystem::Exists(writer.GetTemporaryPath()));
			CHECK_FALSE(writer.Write("after"));
			std::string error;
			CHECK_FALSE(writer.Commit(&error));
			CHECK_FALSE(error.empty());
		}
		{
			// Destroyed without a commit.
			AtomicFileWriter writer(path);
			REQUIRE(writer.IsOpen());
			CHECK(writer.Write("never committed"));
		}
		CHECK(FileSystem::ReadText(path) == std::optional<std::string>("kept"));
		CHECK(CountFiles(directory) == 1);

		// A new file is created by the commit only.
		const std::filesystem::path created = directory / "New.txt";
		{
			AtomicFileWriter writer(created);
			CHECK(writer.Write("new"));
			CHECK_FALSE(FileSystem::Exists(created));
			CHECK(writer.Commit());
		}
		CHECK(FileSystem::ReadText(created) == std::optional<std::string>("new"));
	}

	TEST_CASE("A writer that cannot create its file fails with the reason and writes nothing")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("AtomicWriterFail");
		// The file's folder is a file.
		REQUIRE(FileSystem::WriteText(directory / "NotAFolder", "file"));
		AtomicFileWriter writer(directory / "NotAFolder" / "Document.txt");
		CHECK_FALSE(writer.IsOpen());
		CHECK_FALSE(writer.GetError().empty());
		CHECK_FALSE(writer.Write("text"));
		std::string error;
		CHECK_FALSE(writer.Commit(&error));
		CHECK_FALSE(error.empty());
		CHECK(CountFiles(directory) == 1);

		// A target that cannot be replaced (a folder is there): the commit fails and the temporary file goes.
		REQUIRE(FileSystem::CreateDirectories(directory / "Folder" / "Inside"));
		AtomicFileWriter blocked(directory / "Folder");
		REQUIRE(blocked.IsOpen());
		CHECK(blocked.Write("text"));
		CHECK_FALSE(blocked.Commit(&error));
		CHECK_FALSE(error.empty());
		CHECK_FALSE(FileSystem::Exists(blocked.GetTemporaryPath()));
		CHECK(FileSystem::IsDirectory(directory / "Folder" / "Inside"));
	}
}
