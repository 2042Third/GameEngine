#include <doctest/doctest.h>

#include "Strata/Core/BoundedRead.h"

#include <algorithm>
#include <cstring>
#include <string>

using namespace Strata;

namespace
{
	// A source that hands out its data in chunks of at most chunkSize bytes.
	BoundedRead::ReadFunction MakeSource(const std::string& data, size_t chunkSize, size_t& offset)
	{
		return [&data, chunkSize, &offset](std::span<char> buffer) -> std::optional<size_t>
		{
			const size_t count = std::min({ chunkSize, buffer.size(), data.size() - offset });
			std::memcpy(buffer.data(), data.data() + offset, count);
			offset += count;
			return count;
		};
	}
}

TEST_SUITE("Core.BoundedRead")
{
	TEST_CASE("A source is read to its end whatever its size was reported as")
	{
		const std::string data = "0123456789abcdefghijklmnopqrstuvwxyz";
		std::string contents;

		// A file that grew since its size was inspected (or reports no size at all, like /proc files) is read
		// completely, not cut at the stale size.
		for (const size_t sizeHint : { size_t(0), size_t(1), size_t(10), data.size(), size_t(1000) })
		{
			for (const size_t chunkSize : { size_t(1), size_t(7), size_t(64) })
			{
				CAPTURE(sizeHint);
				CAPTURE(chunkSize);
				size_t offset = 0;
				CHECK(BoundedRead::ReadAll(MakeSource(data, chunkSize, offset), sizeHint, 64, contents) == BoundedReadStatus::Complete);
				CHECK(contents == data);
			}
		}

		size_t offset = 0;
		const std::string empty;
		CHECK(BoundedRead::ReadAll(MakeSource(empty, 16, offset), 0, 64, contents) == BoundedReadStatus::Complete);
		CHECK(contents.empty());
	}

	TEST_CASE("A source larger than the limit is refused")
	{
		const std::string data(100, 'x');
		std::string contents;

		size_t offset = 0;
		CHECK(BoundedRead::ReadAll(MakeSource(data, 13, offset), 10, 100, contents) == BoundedReadStatus::Complete);
		CHECK(contents.size() == 100);

		// One byte too many is noticed, also when the reported size was within the limit.
		offset = 0;
		CHECK(BoundedRead::ReadAll(MakeSource(data, 13, offset), 10, 99, contents) == BoundedReadStatus::TooLarge);
		CHECK(contents.empty());
		CHECK(offset == 100); // Reading stopped one byte past the limit

		offset = 0;
		CHECK(BoundedRead::ReadAll(MakeSource(data, 100, offset), 0, 0, contents) == BoundedReadStatus::TooLarge);
		CHECK(offset == 1);
	}

	TEST_CASE("Read failures are reported")
	{
		std::string contents = "stale";
		int calls = 0;
		auto failing = [&calls](std::span<char> buffer) -> std::optional<size_t>
		{
			if (calls++ == 0)
			{
				buffer[0] = 'a';
				return 1;
			}
			return std::nullopt;
		};
		CHECK(BoundedRead::ReadAll(failing, 4, 64, contents) == BoundedReadStatus::Failed);
		CHECK(contents.empty());

		// A read function that claims more bytes than the buffer holds is broken, not trusted.
		auto overreporting = [](std::span<char> buffer) -> std::optional<size_t> { return buffer.size() + 1; };
		CHECK(BoundedRead::ReadAll(overreporting, 4, 64, contents) == BoundedReadStatus::Failed);
	}
}
