#include <doctest/doctest.h>

#include "Strata/Core/BinaryStream.h"

#include <string>
#include <vector>

using namespace Strata;

TEST_SUITE("Core")
{
	TEST_CASE("Binary streams round trip values, arrays and strings")
	{
		struct Header
		{
			uint32_t Magic;
			float Scale;
		};

		BinaryWriter writer;
		writer.Write(Header { 0x12345678u, 2.5f });
		writer.Write(static_cast<uint16_t>(7));
		writer.WriteArray(std::span<const int32_t>(std::vector<int32_t> { -1, 0, 1 }));
		writer.WriteString("Strata");
		writer.WriteString("");
		const uint8_t raw[] = { 9, 8, 7 };
		writer.WriteBytes(raw, sizeof(raw));
		CHECK(writer.GetSize() == 8 + 2 + (8 + 12) + (4 + 6) + 4 + 3);

		const std::vector<uint8_t> data = writer.TakeData();
		BinaryReader reader(data);
		const Header header = reader.Read<Header>();
		CHECK(header.Magic == 0x12345678u);
		CHECK(header.Scale == 2.5f);
		CHECK(reader.Read<uint16_t>() == 7);
		std::vector<int32_t> values;
		REQUIRE(reader.ReadArray(values));
		CHECK(values == std::vector<int32_t> { -1, 0, 1 });
		CHECK(reader.ReadString() == "Strata");
		CHECK(reader.ReadString().empty());
		const std::span<const uint8_t> view = reader.ReadView(3);
		REQUIRE(view.size() == 3);
		CHECK(view[0] == 9);
		CHECK(view[2] == 7);
		CHECK(reader.IsValid());
		CHECK(reader.GetRemaining() == 0);
		CHECK(reader.GetPosition() == data.size());
	}

	TEST_CASE("Binary reader fails safely on truncated or corrupt data")
	{
		BinaryWriter writer;
		writer.Write(static_cast<uint32_t>(42));
		writer.WriteString("Hello");
		const std::vector<uint8_t> data = writer.TakeData();

		// Reading past the end zero-fills and stays failed.
		BinaryReader truncated(std::span<const uint8_t>(data.data(), 2));
		CHECK(truncated.Read<uint32_t>() == 0);
		CHECK_FALSE(truncated.IsValid());
		CHECK(truncated.ReadString().empty());
		CHECK(truncated.ReadView(1).empty());

		// String lengths beyond the data or the limit fail without allocating.
		BinaryReader shortString(std::span<const uint8_t>(data.data(), data.size() - 1));
		shortString.Read<uint32_t>();
		CHECK(shortString.ReadString().empty());
		CHECK_FALSE(shortString.IsValid());

		BinaryReader limited(data);
		limited.Read<uint32_t>();
		CHECK(limited.ReadString(4).empty());
		CHECK_FALSE(limited.IsValid());

		// Array counts larger than the remaining data are rejected.
		BinaryWriter arrayWriter;
		arrayWriter.Write(static_cast<uint64_t>(1ull << 60));
		const std::vector<uint8_t> arrayData = arrayWriter.TakeData();
		BinaryReader arrayReader(arrayData);
		std::vector<uint64_t> values;
		CHECK_FALSE(arrayReader.ReadArray(values));
		CHECK(values.empty());

		BinaryReader seeker(data);
		CHECK(seeker.Seek(data.size()));
		CHECK(seeker.GetRemaining() == 0);
		CHECK_FALSE(seeker.Seek(data.size() + 1));
		CHECK_FALSE(seeker.IsValid());
	}
}
