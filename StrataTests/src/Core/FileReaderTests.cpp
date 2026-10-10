#include <doctest/doctest.h>

#include "Strata/Core/FileReader.h"
#include "Strata/Core/FileSystem.h"
#include "TestHelpers.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace Strata;

namespace
{

	std::vector<uint8_t> MakeBytes(size_t size)
	{
		std::vector<uint8_t> bytes(size);
		for (size_t index = 0; index < size; index++)
			bytes[index] = static_cast<uint8_t>((index * 31 + index / 251) & 0xFF);
		return bytes;
	}

}

TEST_SUITE("Core.FileReader")
{
	TEST_CASE("A file reader reads ranges at any offset and refuses ranges outside the file")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("FileReader");
		const std::filesystem::path path = directory / "Data.bin";
		const std::vector<uint8_t> bytes = MakeBytes(100'000);
		REQUIRE(FileSystem::WriteBytes(path, bytes));

		const uint64_t opened = FileReader::GetOpenCount();
		Scope<FileReader> reader = FileReader::Open(path);
		REQUIRE(reader);
		CHECK(FileReader::GetOpenCount() == opened + 1);
		CHECK(reader->GetSize() == bytes.size());

		std::vector<uint8_t> range(1000);
		REQUIRE(reader->ReadAt(12'345, range));
		CHECK(std::equal(range.begin(), range.end(), bytes.begin() + 12'345));
		REQUIRE(reader->ReadAt(bytes.size() - range.size(), range)); // Up to the end
		CHECK(std::equal(range.begin(), range.end(), bytes.end() - 1000));
		CHECK(reader->ReadAt(bytes.size(), std::span<uint8_t>())); // Nothing, at the end
		CHECK_FALSE(reader->ReadAt(bytes.size() - 999, range));    // One byte past the end
		CHECK_FALSE(reader->ReadAt(UINT64_MAX, range));
		CHECK_FALSE(reader->ReadAt(UINT64_MAX - 10, range));

		// Missing files and directories cannot be opened.
		CHECK_FALSE(FileReader::Open(directory / "Missing.bin"));
		CHECK_FALSE(FileReader::Open(directory));
		CHECK(FileReader::GetOpenCount() == opened + 1);

		// Another program may rename (or delete) the file while it is open: the reader keeps reading what it opened.
		const std::filesystem::path moved = directory / "Moved.bin";
		REQUIRE(FileSystem::Rename(path, moved));
		REQUIRE(FileSystem::WriteBytes(path, MakeBytes(10)));
		REQUIRE(reader->ReadAt(500, range));
		CHECK(std::equal(range.begin(), range.end(), bytes.begin() + 500));
	}

	TEST_CASE("Threads read one file reader at once without sharing a position")
	{
		const std::filesystem::path path = Tests::CreateTemporaryDirectory("FileReaderThreads") / "Data.bin";
		const std::vector<uint8_t> bytes = MakeBytes(1 << 20);
		REQUIRE(FileSystem::WriteBytes(path, bytes));
		Scope<FileReader> reader = FileReader::Open(path);
		REQUIRE(reader);

		std::atomic<uint32_t> mismatches = 0;
		std::vector<std::thread> threads;
		for (uint32_t thread = 0; thread < 8; thread++)
		{
			threads.emplace_back([&, thread]()
			{
				uint32_t state = thread * 7919u + 1u;
				std::vector<uint8_t> range;
				for (int read = 0; read < 500; read++)
				{
					state = state * 1664525u + 1013904223u;
					const size_t offset = state % bytes.size();
					const size_t size = std::min<size_t>((state >> 8) % 8192, bytes.size() - offset);
					range.resize(size);
					if (!reader->ReadAt(offset, range) || !std::equal(range.begin(), range.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset)))
						mismatches++;
				}
			});
		}
		for (std::thread& thread : threads)
			thread.join();
		CHECK(mismatches == 0);
	}
}
