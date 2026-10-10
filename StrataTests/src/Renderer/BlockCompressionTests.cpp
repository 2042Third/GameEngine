#include <doctest/doctest.h>

#include "Strata/Renderer/BlockCompression.h"

#include <array>
#include <cstdint>
#include <vector>

using namespace Strata;

namespace
{

	using Texel = std::array<uint8_t, 4>;

	// Writes the fields of a 128-bit BC7 block, least significant bit first, as the format stores them.
	class BC7BlockWriter
	{
	public:
		void Write(uint32_t value, uint32_t bits)
		{
			for (uint32_t bit = 0; bit < bits; bit++, m_Position++)
			{
				if ((value >> bit) & 1u)
					m_Bytes[m_Position / 8] = static_cast<uint8_t>(m_Bytes[m_Position / 8] | (1u << (m_Position % 8)));
			}
		}

		// Mode m starts with m zero bits and a one.
		void WriteMode(uint32_t mode) { Write(1u << mode, mode + 1); }

		uint32_t GetPosition() const { return m_Position; }
		const std::array<uint8_t, 16>& GetBytes() const { return m_Bytes; }
	private:
		std::array<uint8_t, 16> m_Bytes {};
		uint32_t m_Position = 0;
	};

	std::array<Texel, 16> DecodeRGBABlock(TextureFormat format, const uint8_t* block)
	{
		std::array<Texel, 16> texels {};
		BlockCompression::DecodeBlock(format, block, texels[0].data(), 16);
		return texels;
	}

}

TEST_SUITE("Renderer.BlockCompression")
{
	TEST_CASE("A known BC1 block decodes bit-exactly")
	{
		// Endpoints red (0xF800) and blue (0x001F) in descending order: four colors, the middle two at exactly one and two
		// thirds. Every row holds indices 0, 1, 2, 3.
		const uint8_t block[8] = { 0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4 };
		std::vector<uint8_t> texels(4 * 4 * 4);
		REQUIRE(BlockCompression::DecodeLevel(TextureFormat::BC1, 4, 4, block, texels));
		const Texel row[4] = { { 255, 0, 0, 255 }, { 0, 0, 255, 255 }, { 170, 0, 85, 255 }, { 85, 0, 170, 255 } };
		for (uint32_t texel = 0; texel < 16; texel++)
		{
			CAPTURE(texel);
			const Texel& expected = row[texel % 4];
			CHECK(std::equal(expected.begin(), expected.end(), texels.begin() + texel * 4));
		}

		// Endpoints whose thirds are not whole: 0x7BEF expands to (123, 125, 123), and the interpolated colors round to the
		// nearest value.
		const uint8_t rounded[8] = { 0xEF, 0x7B, 0x00, 0x00, 0xE4, 0xE4, 0xE4, 0xE4 };
		const std::array<Texel, 16> roundedTexels = DecodeRGBABlock(TextureFormat::BC1SRGB, rounded);
		CHECK(roundedTexels[0] == Texel { 123, 125, 123, 255 });
		CHECK(roundedTexels[1] == Texel { 0, 0, 0, 255 });
		CHECK(roundedTexels[2] == Texel { 82, 83, 82, 255 });
		CHECK(roundedTexels[3] == Texel { 41, 42, 41, 255 });

		// Ascending endpoints select three colors and transparent black.
		const uint8_t threeColors[8] = { 0x1F, 0x00, 0x00, 0xF8, 0xE4, 0xE4, 0xE4, 0xE4 };
		const std::array<Texel, 16> threeColorTexels = DecodeRGBABlock(TextureFormat::BC1, threeColors);
		CHECK(threeColorTexels[0] == Texel { 0, 0, 255, 255 });
		CHECK(threeColorTexels[1] == Texel { 255, 0, 0, 255 });
		CHECK(threeColorTexels[2] == Texel { 128, 0, 128, 255 });
		CHECK(threeColorTexels[3] == Texel { 0, 0, 0, 0 });
	}

	TEST_CASE("BC3, BC4 and BC5 blocks decode their channels")
	{
		// BC4 with descending endpoints interpolates six values; texel i uses index i % 8.
		const uint8_t eightValues[8] = { 200, 100, 0x88, 0xC6, 0xFA, 0x88, 0xC6, 0xFA };
		uint8_t channel[16] = {};
		BlockCompression::DecodeBC4Block(eightValues, channel, 1, 4);
		const uint8_t expectedEight[8] = { 200, 100, 186, 171, 157, 143, 129, 114 };
		for (uint32_t texel = 0; texel < 16; texel++)
		{
			CAPTURE(texel);
			CHECK(channel[texel] == expectedEight[texel % 8]);
		}
		// Ascending endpoints interpolate four values and add 0 and 255.
		const uint8_t sixValues[8] = { 50, 250, 0x88, 0xC6, 0xFA, 0x88, 0xC6, 0xFA };
		BlockCompression::DecodeBC4Block(sixValues, channel, 1, 4);
		const uint8_t expectedSix[8] = { 50, 250, 90, 130, 170, 210, 0, 255 };
		for (uint32_t texel = 0; texel < 16; texel++)
		{
			CAPTURE(texel);
			CHECK(channel[texel] == expectedSix[texel % 8]);
		}

		// BC5 is two BC4 blocks, red then green.
		uint8_t bc5[16];
		std::copy(std::begin(eightValues), std::end(eightValues), bc5);
		std::copy(std::begin(sixValues), std::end(sixValues), bc5 + 8);
		std::vector<uint8_t> redGreen(4 * 4 * 2);
		REQUIRE(BlockCompression::DecodeLevel(TextureFormat::BC5, 4, 4, bc5, redGreen));
		CHECK(redGreen[2 * 2] == 186);
		CHECK(redGreen[2 * 2 + 1] == 90);
		CHECK(redGreen[7 * 2] == 114);
		CHECK(redGreen[7 * 2 + 1] == 255);

		// BC3: the BC4 alpha block first, then a color block that interpolates four colors even with ascending endpoints.
		uint8_t bc3[16];
		std::copy(std::begin(sixValues), std::end(sixValues), bc3);
		const uint8_t color[8] = { 0x1F, 0x00, 0x00, 0xF8, 0xE4, 0xE4, 0xE4, 0xE4 };
		std::copy(std::begin(color), std::end(color), bc3 + 8);
		const std::array<Texel, 16> bc3Texels = DecodeRGBABlock(TextureFormat::BC3SRGB, bc3);
		CHECK(bc3Texels[0] == Texel { 0, 0, 255, 50 });
		CHECK(bc3Texels[2] == Texel { 85, 0, 170, 90 });
		CHECK(bc3Texels[3] == Texel { 170, 0, 85, 130 });
		CHECK(bc3Texels[7] == Texel { 170, 0, 85, 255 });
	}

	TEST_CASE("BC7 blocks decode by mode, partition, rotation and index selection")
	{
		SUBCASE("Mode 6: one subset with p-bits and four-bit indices")
		{
			BC7BlockWriter writer;
			writer.WriteMode(6);
			for (uint32_t value : { 127u, 0u, 0u, 127u, 64u, 64u, 127u, 127u })
				writer.Write(value, 7);
			writer.Write(1, 1);
			writer.Write(0, 1);
			for (uint32_t texel = 0; texel < 16; texel++)
				writer.Write(texel == 1 ? 15 : (texel == 2 ? 8 : 0), texel == 0 ? 3 : 4);
			REQUIRE(writer.GetPosition() == 128);

			// Endpoints (7 bits and the p-bit): (255, 1, 129, 255) and (0, 254, 128, 254).
			const std::array<Texel, 16> texels = DecodeRGBABlock(TextureFormat::BC7, writer.GetBytes().data());
			CHECK(texels[0] == Texel { 255, 1, 129, 255 });
			CHECK(texels[1] == Texel { 0, 254, 128, 254 });
			CHECK(texels[2] == Texel { 120, 135, 128, 254 }); // Weight 34 of 64
			CHECK(texels[15] == Texel { 255, 1, 129, 255 });
		}
		SUBCASE("Mode 5: separate alpha indices and a channel rotation")
		{
			BC7BlockWriter writer;
			writer.WriteMode(5);
			writer.Write(1, 2); // Rotation: swap red and alpha
			for (uint32_t value : { 127u, 0u, 0u, 0u, 0u, 0u })
				writer.Write(value, 7);
			writer.Write(10, 8);
			writer.Write(200, 8);
			for (uint32_t texel = 0; texel < 16; texel++)
				writer.Write(0, texel == 0 ? 1 : 2);
			for (uint32_t texel = 0; texel < 16; texel++)
				writer.Write(texel == 1 ? 3 : 0, texel == 0 ? 1 : 2);
			REQUIRE(writer.GetPosition() == 128);

			const std::array<Texel, 16> texels = DecodeRGBABlock(TextureFormat::BC7SRGB, writer.GetBytes().data());
			CHECK(texels[0] == Texel { 10, 0, 0, 255 });
			CHECK(texels[1] == Texel { 200, 0, 0, 255 });
		}
		SUBCASE("Mode 4: the index selection bit gives color the three-bit indices")
		{
			BC7BlockWriter writer;
			writer.WriteMode(4);
			writer.Write(0, 2); // No rotation
			writer.Write(1, 1); // Color uses the three-bit indices, alpha the two-bit ones
			for (uint32_t value : { 31u, 0u, 0u, 0u, 0u, 0u })
				writer.Write(value, 5);
			writer.Write(0, 6);
			writer.Write(63, 6);
			for (uint32_t texel = 0; texel < 16; texel++)
				writer.Write(texel == 1 ? 3 : 0, texel == 0 ? 1 : 2);
			for (uint32_t texel = 0; texel < 16; texel++)
				writer.Write(texel == 1 ? 7 : (texel == 2 ? 4 : 0), texel == 0 ? 2 : 3);
			REQUIRE(writer.GetPosition() == 128);

			const std::array<Texel, 16> texels = DecodeRGBABlock(TextureFormat::BC7, writer.GetBytes().data());
			CHECK(texels[0] == Texel { 255, 0, 0, 0 });
			CHECK(texels[1] == Texel { 0, 0, 0, 255 });
			CHECK(texels[2] == Texel { 108, 0, 0, 0 }); // Color weight 37 of 64, alpha index 0
		}
		SUBCASE("Mode 1: two subsets of a partition, shared p-bits and anchor indices")
		{
			BC7BlockWriter writer;
			writer.WriteMode(1);
			writer.Write(0, 6); // Partition 0: columns 2 and 3 are subset 1, whose anchor is texel 15
			for (uint32_t value : { 63u, 0u, 0u, 63u })
				writer.Write(value, 6); // Red of subset 0 (both endpoints), then of subset 1
			for (uint32_t value : { 0u, 63u, 0u, 63u })
				writer.Write(value, 6);
			for (uint32_t value : { 0u, 0u, 63u, 63u })
				writer.Write(value, 6);
			writer.Write(1, 1); // Shared p-bit of subset 0
			writer.Write(0, 1); // Of subset 1
			for (uint32_t texel = 0; texel < 16; texel++)
			{
				const uint32_t index = texel == 1 || texel == 3 ? 7 : (texel == 15 ? 3 : 0);
				writer.Write(index, texel == 0 || texel == 15 ? 2 : 3);
			}
			REQUIRE(writer.GetPosition() == 128);

			// Subset 0: (255, 2, 2) and (2, 255, 2); subset 1: (0, 0, 253) and (253, 253, 253).
			const std::array<Texel, 16> texels = DecodeRGBABlock(TextureFormat::BC7, writer.GetBytes().data());
			CHECK(texels[0] == Texel { 255, 2, 2, 255 });
			CHECK(texels[1] == Texel { 2, 255, 2, 255 });
			CHECK(texels[2] == Texel { 0, 0, 253, 255 });
			CHECK(texels[3] == Texel { 253, 253, 253, 255 });
			CHECK(texels[4] == Texel { 255, 2, 2, 255 });
			CHECK(texels[6] == Texel { 0, 0, 253, 255 });
			CHECK(texels[15] == Texel { 107, 107, 253, 255 }); // Anchor index 3: weight 27 of 64
		}
		SUBCASE("The reserved mode decodes to zero")
		{
			const uint8_t reserved[16] = {};
			const std::array<Texel, 16> texels = DecodeRGBABlock(TextureFormat::BC7, reserved);
			for (const Texel& texel : texels)
				CHECK(texel == Texel { 0, 0, 0, 0 });
		}
	}

	TEST_CASE("Levels decode whole blocks and keep the texels inside the level")
	{
		// A 5x3 BC1 level is two blocks; the second one holds one column of the level.
		const uint8_t blocks[16] = { 0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4, 0x1F, 0x00, 0x00, 0xF8, 0x00, 0x00, 0x00, 0x00 };
		std::vector<uint8_t> texels(5 * 3 * 4, 0xCD);
		REQUIRE(BlockCompression::DecodeLevel(TextureFormat::BC1, 5, 3, blocks, texels));
		for (uint32_t y = 0; y < 3; y++)
		{
			CAPTURE(y);
			CHECK(texels[(y * 5 + 3) * 4] == 85);      // Index 3 of the first block
			CHECK(texels[(y * 5 + 4) * 4 + 2] == 255); // Index 0 of the second block: blue
		}

		// BC4 and BC5 decode to one and two channels.
		const uint8_t bc4[8] = { 7, 7, 0, 0, 0, 0, 0, 0 };
		std::vector<uint8_t> gray(2 * 2);
		REQUIRE(BlockCompression::DecodeLevel(TextureFormat::BC4, 2, 2, bc4, gray));
		CHECK(gray == std::vector<uint8_t> { 7, 7, 7, 7 });

		// Sizes that do not match, and formats without blocks, are refused.
		CHECK_FALSE(BlockCompression::DecodeLevel(TextureFormat::BC1, 5, 3, std::span<const uint8_t>(blocks, 8), texels));
		CHECK_FALSE(BlockCompression::DecodeLevel(TextureFormat::BC1, 5, 3, blocks, std::span<uint8_t>(texels.data(), 4)));
		CHECK_FALSE(BlockCompression::DecodeLevel(TextureFormat::RGBA8, 2, 2, blocks, texels));
	}
}
