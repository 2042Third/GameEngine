#include <doctest/doctest.h>

#include "Strata/Renderer/Font.h"

#include <cstring>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	void WriteU16(std::vector<uint8_t>& data, size_t offset, uint16_t value)
	{
		data[offset] = static_cast<uint8_t>(value >> 8);
		data[offset + 1] = static_cast<uint8_t>(value);
	}

	void WriteU32(std::vector<uint8_t>& data, size_t offset, uint32_t value)
	{
		for (size_t byte = 0; byte < 4; byte++)
			data[offset + byte] = static_cast<uint8_t>(value >> (24 - byte * 8));
	}

	// Minimal sfnt container: an offset table plus 4-byte tables with the given tags.
	std::vector<uint8_t> BuildFontFile(const std::vector<std::string>& tags, uint32_t version = 0x00010000)
	{
		const size_t directorySize = 12 + tags.size() * 16;
		std::vector<uint8_t> data(directorySize + tags.size() * 4, 0);
		WriteU32(data, 0, version);
		WriteU16(data, 4, static_cast<uint16_t>(tags.size()));
		for (size_t index = 0; index < tags.size(); index++)
		{
			const size_t record = 12 + index * 16;
			std::memcpy(data.data() + record, tags[index].data(), 4);
			WriteU32(data, record + 8, static_cast<uint32_t>(directorySize + index * 4));
			WriteU32(data, record + 12, 4);
		}
		return data;
	}

	const std::vector<std::string> c_RequiredTables = { "cmap", "glyf", "head", "hhea", "hmtx", "loca", "maxp" };

}

TEST_SUITE("Renderer.Font")
{
	TEST_CASE("TrueType and OpenType fonts are accepted")
	{
		std::string error;
		Ref<Font> trueType = Font::Create(BuildFontFile(c_RequiredTables), &error);
		REQUIRE_MESSAGE(trueType, error);
		CHECK(trueType->GetMemoryUsage() == trueType->GetData().size());

		std::vector<std::string> cffTables = { "CFF ", "cmap", "head", "hhea", "hmtx", "maxp" };
		CHECK(Font::Create(BuildFontFile(cffTables, 0x4F54544F /* OTTO */), &error));

		// Collection whose first font is valid.
		std::vector<uint8_t> inner = BuildFontFile(c_RequiredTables);
		std::vector<uint8_t> collection(16, 0);
		std::memcpy(collection.data(), "ttcf", 4);
		WriteU32(collection, 8, 1);
		WriteU32(collection, 12, 16);
		// Table offsets inside the collection are relative to the file start.
		for (size_t index = 0; index < c_RequiredTables.size(); index++)
		{
			const size_t record = 12 + index * 16;
			const uint32_t offset = (static_cast<uint32_t>(inner[record + 8]) << 24) | (inner[record + 9] << 16) | (inner[record + 10] << 8) | inner[record + 11];
			WriteU32(inner, record + 8, offset + 16);
		}
		collection.insert(collection.end(), inner.begin(), inner.end());
		CHECK(Font::Create(collection, &error));
	}

	TEST_CASE("Invalid font files are rejected")
	{
		std::string error;
		CHECK_FALSE(Font::Create({}, &error));
		CHECK_FALSE(error.empty());
		CHECK_FALSE(Font::Create({ 'n', 'o', 't', ' ', 'a', ' ', 'f', 'o', 'n', 't', '!', '!' }, &error));

		// Missing glyph outlines.
		CHECK_FALSE(Font::Create(BuildFontFile({ "cmap", "head", "hhea", "hmtx", "maxp" }), &error));
		CHECK(error.find("required tables") != std::string::npos);

		// A table pointing past the end of the file.
		std::vector<uint8_t> outside = BuildFontFile(c_RequiredTables);
		WriteU32(outside, 12 + 16 + 12, 0x7FFFFFFF);
		CHECK_FALSE(Font::Create(outside, &error));

		// Directory larger than the file.
		std::vector<uint8_t> truncated = BuildFontFile(c_RequiredTables);
		truncated.resize(40);
		CHECK_FALSE(Font::Create(truncated, &error));

		std::vector<uint8_t> noTables = BuildFontFile({});
		CHECK_FALSE(Font::Create(noTables, &error));

		std::vector<uint8_t> badCollection = { 't', 't', 'c', 'f', 0, 1, 0, 0, 0, 0, 0, 1, 0xFF, 0xFF, 0xFF, 0xF0 };
		CHECK_FALSE(Font::Create(badCollection, &error));
	}
}
