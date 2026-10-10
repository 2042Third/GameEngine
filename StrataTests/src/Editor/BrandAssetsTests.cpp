#include <doctest/doctest.h>

#include "EditorIcon.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	std::filesystem::path GetBrandDirectory()
	{
		return FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "StrataEditor" / "Resources" / "Brand";
	}

	std::vector<uint8_t> ReadBrandFile(const std::string& name)
	{
		const std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(GetBrandDirectory() / name);
		REQUIRE_MESSAGE(bytes, name);
		return *bytes;
	}

	// RGBA8 pixels of a PNG.
	std::vector<uint8_t> DecodePNG(const std::vector<uint8_t>& png, int expectedSize)
	{
		int width = 0;
		int height = 0;
		int channels = 0;
		stbi_uc* pixels = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &width, &height, &channels, 4);
		REQUIRE(pixels);
		CHECK(width == expectedSize);
		CHECK(height == expectedSize);
		std::vector<uint8_t> result(pixels, pixels + static_cast<size_t>(width) * height * 4);
		stbi_image_free(pixels);
		return result;
	}

	uint16_t Read16(const std::vector<uint8_t>& data, size_t offset)
	{
		REQUIRE(offset + 2 <= data.size());
		return static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
	}

	uint32_t Read32(const std::vector<uint8_t>& data, size_t offset)
	{
		REQUIRE(offset + 4 <= data.size());
		return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) | (static_cast<uint32_t>(data[offset + 2]) << 16)
			| (static_cast<uint32_t>(data[offset + 3]) << 24);
	}

}

TEST_SUITE("Editor.Brand")
{
	TEST_CASE("The window icon has an exact image for the icon sizes of every common display scale")
	{
		// Windows asks for 16 x 16 (title bar) and 32 x 32 (taskbar) icons times the display scale, and stretches the image
		// whose area is closest when none has the size: at 150% the 16 pixel image became a blurry 24 pixel one.
		for (const float scale : { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f })
		{
			for (const uint32_t base : { 16u, 32u })
			{
				const uint32_t size = static_cast<uint32_t>(std::lround(static_cast<float>(base) * scale));
				CAPTURE(size);
				CHECK(std::find(c_EditorIconSizes.begin(), c_EditorIconSizes.end(), size) != c_EditorIconSizes.end());
			}
		}
		CHECK(std::is_sorted(c_EditorIconSizes.begin(), c_EditorIconSizes.end()));
	}

	TEST_CASE("The committed brand assets show the same mark in every form")
	{
		for (const uint32_t iconSize : c_EditorIconSizes)
		{
			const int size = static_cast<int>(iconSize);
			CAPTURE(size);
			// The window icon's raw pixels are the PNG's.
			const std::vector<uint8_t> raw = ReadBrandFile("StrataMark" + std::to_string(size) + ".rgba");
			REQUIRE(raw.size() == static_cast<size_t>(size) * size * 4);
			CHECK(DecodePNG(ReadBrandFile("StrataMark" + std::to_string(size) + ".png"), size) == raw);

			// A rounded square: (nearly) transparent corners, the opaque Basalt tile around the bands.
			CHECK(raw[3] < 8);
			const size_t edgeMiddle = (static_cast<size_t>(size / 2) * size + 1) * 4;
			CHECK(raw[edgeMiddle + 3] == 255);
			CHECK(raw[edgeMiddle + 0] == 0x0E);
			CHECK(raw[edgeMiddle + 1] == 0x10);
			CHECK(raw[edgeMiddle + 2] == 0x13);
		}
		const std::vector<uint8_t> large = DecodePNG(ReadBrandFile("StrataMark256.png"), 256);
		// The top band is Sandstone (#F2B872) in its middle.
		bool sandstone = false;
		for (size_t pixel = 0; pixel + 4 <= large.size(); pixel += 4)
			sandstone |= large[pixel] == 0xF2 && large[pixel + 1] == 0xB8 && large[pixel + 2] == 0x72 && large[pixel + 3] == 255;
		CHECK(sandstone);

		// The Windows icon: a bitmap at every icon size and the 256 pixel PNG.
		const std::vector<uint8_t> icon = ReadBrandFile("StrataMark.ico");
		CHECK(Read16(icon, 0) == 0);
		CHECK(Read16(icon, 2) == 1);
		const uint16_t count = Read16(icon, 4);
		REQUIRE(count == c_EditorIconSizes.size() + 1);
		std::vector<int> sizes;
		for (uint16_t index = 0; index < count; index++)
		{
			const size_t entry = 6 + static_cast<size_t>(index) * 16;
			const int size = icon[entry] == 0 ? 256 : icon[entry];
			sizes.push_back(size);
			const uint32_t bytes = Read32(icon, entry + 8);
			const uint32_t offset = Read32(icon, entry + 12);
			REQUIRE(static_cast<size_t>(offset) + bytes <= icon.size());
			const std::vector<uint8_t> image(icon.begin() + offset, icon.begin() + offset + bytes);
			if (size == 256)
			{
				CHECK(DecodePNG(image, 256) == large);
				continue;
			}
			// BITMAPINFOHEADER with the doubled height of the color and mask planes, 32 bits per pixel.
			CHECK(Read32(image, 0) == 40);
			CHECK(Read32(image, 4) == static_cast<uint32_t>(size));
			CHECK(Read32(image, 8) == static_cast<uint32_t>(size * 2));
			CHECK(Read16(image, 14) == 32);
			// Bottom-up BGRA rows: the last row of the raw icon comes first.
			const std::vector<uint8_t> raw = ReadBrandFile("StrataMark" + std::to_string(size) + ".rgba");
			const size_t lastRow = static_cast<size_t>(size - 1) * size * 4;
			CHECK(image[40 + 0] == raw[lastRow + 2]);
			CHECK(image[40 + 1] == raw[lastRow + 1]);
			CHECK(image[40 + 2] == raw[lastRow + 0]);
			CHECK(image[40 + 3] == raw[lastRow + 3]);
		}
		std::vector<int> expected(c_EditorIconSizes.begin(), c_EditorIconSizes.end());
		expected.push_back(256);
		CHECK(sizes == expected);
	}
}
