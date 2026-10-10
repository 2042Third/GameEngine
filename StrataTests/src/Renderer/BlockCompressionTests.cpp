#include <doctest/doctest.h>

#include "Strata/Asset/BC7Encoder.h"
#include "Strata/Renderer/BC7Format.h"
#include "Strata/Renderer/BlockCompression.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	using Texel = std::array<uint8_t, 4>;
	using Block = std::array<Texel, 16>;

	// An endpoint of a test block: its expanded RGBA values.
	using Endpoint = std::array<uint32_t, 4>;

	// The value a mode with `bits` stored bits per endpoint value (and a p-bit) expands `stored` to.
	uint32_t ExpandStored(uint32_t stored, uint32_t bits, int32_t pBit = -1)
	{
		return pBit < 0 ? BC7::ExpandBits(stored, bits) : BC7::ExpandBits((stored << 1) | static_cast<uint32_t>(pBit), bits + 1);
	}

	Endpoint ExpandEndpoint(std::array<uint32_t, 4> stored, uint32_t colorBits, uint32_t alphaBits, int32_t pBit = -1)
	{
		Endpoint endpoint;
		for (uint32_t channel = 0; channel < 3; channel++)
			endpoint[channel] = ExpandStored(stored[channel], colorBits, pBit);
		endpoint[3] = alphaBits == 0 ? 255 : ExpandStored(stored[3], alphaBits, pBit);
		return endpoint;
	}

	Texel InterpolateTexel(const Endpoint& first, const Endpoint& second, uint32_t colorWeight, uint32_t alphaWeight)
	{
		Texel texel;
		for (uint32_t channel = 0; channel < 3; channel++)
			texel[channel] = BC7::Interpolate(first[channel], second[channel], colorWeight);
		texel[3] = BC7::Interpolate(first[3], second[3], alphaWeight);
		return texel;
	}

	// A block of a mode with one index per texel that a partition splits into subsets: subset s interpolates between
	// endpoints[s][0] and endpoints[s][1], and its n-th texel (in row-major order) takes indexPattern[n % size]. Exactly
	// representable in that mode.
	Block CreatePartitionedBlock(uint32_t mode, uint32_t partition, const std::vector<std::array<Endpoint, 2>>& endpoints, const std::vector<uint32_t>& indexPattern)
	{
		const BC7::ModeInfo& info = BC7::c_Modes[mode];
		const uint8_t* weights = BC7::GetWeights(info.IndexBits);
		Block block {};
		uint32_t counts[3] = {};
		for (uint32_t texel = 0; texel < 16; texel++)
		{
			const uint32_t subset = BC7::GetSubset(info.Subsets, partition, texel);
			const uint32_t weight = weights[indexPattern[counts[subset]++ % indexPattern.size()]];
			block[texel] = InterpolateTexel(endpoints[subset][0], endpoints[subset][1], weight, weight);
		}
		return block;
	}

	struct EncodedBlock
	{
		std::array<uint8_t, 16> Bytes {};
		BC7Encoder::BlockInfo Info;
		uint32_t Error = 0;
	};

	EncodedBlock Encode(const Block& block, bool highQuality = true, uint32_t allowedModes = BC7Encoder::c_AllModes)
	{
		EncodedBlock encoded;
		BC7Encoder::EncodeSettings settings;
		settings.HighQuality = highQuality;
		settings.AllowedModes = allowedModes;
		encoded.Error = BC7Encoder::EncodeBlock(block[0].data(), encoded.Bytes.data(), settings, &encoded.Info);
		return encoded;
	}

	Block ToBlock(const uint8_t* rgba)
	{
		Block block {};
		std::memcpy(block.data(), rgba, sizeof(block));
		return block;
	}

	uint32_t GetSquaredError(const Block& first, const Block& second)
	{
		uint32_t error = 0;
		for (uint32_t texel = 0; texel < 16; texel++)
		{
			for (uint32_t channel = 0; channel < 4; channel++)
			{
				const int32_t difference = static_cast<int32_t>(first[texel][channel]) - static_cast<int32_t>(second[texel][channel]);
				error += static_cast<uint32_t>(difference * difference);
			}
		}
		return error;
	}

	// Checks what an encoded block promises against the decoder: the block decodes to the texels the encoder predicted,
	// with the error it reported, in the mode it reported. Returns the decoded texels.
	Block CheckRoundTrip(const Block& source, const EncodedBlock& encoded)
	{
		Block decoded {};
		BlockCompression::DecodeBC7Block(encoded.Bytes.data(), decoded[0].data(), 16);
		CHECK(decoded == ToBlock(encoded.Info.Decoded));
		CHECK(encoded.Error == GetSquaredError(source, decoded));
		CHECK(encoded.Info.Error == encoded.Error);
		uint32_t mode = 0;
		while (mode < 8 && ((encoded.Bytes[0] >> mode) & 1u) == 0)
			mode++;
		CHECK(mode == encoded.Info.Mode);
		return decoded;
	}

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

	TEST_CASE("The BC7 encoder stores blocks that force each mode in that mode, as it predicted")
	{
		// Each block is exactly what one mode decodes from some endpoints and indices, and no other mode can store it
		// exactly: the encoder must find that mode (and partition), with no error, and the block must decode as it said.
		auto check = [](const std::string& name, const Block& source, uint32_t expectedMode, uint32_t expectedPartition = 0)
		{
			CAPTURE(name);
			const EncodedBlock encoded = Encode(source);
			CheckRoundTrip(source, encoded);
			CHECK(encoded.Info.Mode == expectedMode);
			CHECK(encoded.Info.Partition == expectedPartition);
			CHECK(encoded.Error == 0);
		};

		// Flat color: mode 5 reproduces any color, alpha included.
		Block flat {};
		flat.fill(Texel { 17, 128, 255, 99 });
		check("flat", flat, 5);

		// The feature test's checker: two opaque colors with even values, which opaque mode 6 (odd values only) misses by
		// one in every channel; mode 3 stores them exactly in the checkerboard partition.
		Block checker {};
		for (uint32_t texel = 0; texel < 16; texel++)
			checker[texel] = (texel % 4 + texel / 4) % 2 == 0 ? Texel { 230, 120, 30, 255 } : Texel { 240, 230, 200, 255 };
		check("checker", checker, 3, 34);

		// A smooth opaque gradient of sixteen levels: mode 6's four-bit indices (opaque, so its p-bits are 1).
		{
			const Endpoint first = ExpandEndpoint({ 10, 100, 38, 127 }, 7, 7, 1);
			const Endpoint second = ExpandEndpoint({ 116, 7, 90, 127 }, 7, 7, 1);
			Block gradient {};
			for (uint32_t texel = 0; texel < 16; texel++)
				gradient[texel] = InterpolateTexel(first, second, BC7::c_Weights4[texel], BC7::c_Weights4[texel]);
			check("gradient", gradient, 6);
		}

		// Color and alpha that vary independently: mode 5 (seven-bit color, eight-bit alpha, four levels each) and mode 4
		// (five-bit color with eight levels, six-bit alpha with four).
		{
			const Endpoint first = ExpandEndpoint({ 3, 120, 61, 13 }, 7, 8);
			const Endpoint second = ExpandEndpoint({ 101, 9, 77, 230 }, 7, 8);
			Block independent {};
			for (uint32_t texel = 0; texel < 16; texel++)
				independent[texel] = InterpolateTexel(first, second, BC7::c_Weights2[texel % 4], BC7::c_Weights2[texel / 4]);
			check("independent alpha, mode 5", independent, 5);
		}
		{
			const Endpoint first = ExpandEndpoint({ 2, 29, 11, 5 }, 5, 6);
			const Endpoint second = ExpandEndpoint({ 27, 4, 19, 58 }, 5, 6);
			Block independent {};
			for (uint32_t texel = 0; texel < 16; texel++)
				independent[texel] = InterpolateTexel(first, second, BC7::c_Weights3[texel % 8], BC7::c_Weights2[texel / 4]);
			const EncodedBlock encoded = Encode(independent);
			CheckRoundTrip(independent, encoded);
			CHECK(encoded.Info.Mode == 4);
			CHECK(encoded.Info.IndexSelection == 1);
			CHECK(encoded.Error == 0);
		}

		// Hard edges between two gradients in every two-subset partition: mode 3 (eight-bit endpoints, four levels) and
		// mode 1 (seven-bit endpoints, eight levels); with two alpha values, mode 7.
		for (uint32_t partition = 0; partition < 64; partition++)
		{
			const std::vector<std::array<Endpoint, 2>> mode3 = {
				{ ExpandEndpoint({ 1, 125, 20, 0 }, 7, 0, 1), ExpandEndpoint({ 125, 3, 94, 0 }, 7, 0, 0) },
				{ ExpandEndpoint({ 27, 49, 125, 0 }, 7, 0, 0), ExpandEndpoint({ 100, 105, 3, 0 }, 7, 0, 1) } };
			check(fmt::format("mode 3, partition {}", partition), CreatePartitionedBlock(3, partition, mode3, { 0, 3, 1, 2 }), 3, partition);

			const std::vector<std::array<Endpoint, 2>> mode1 = {
				{ ExpandEndpoint({ 1, 61, 10, 0 }, 6, 0, 1), ExpandEndpoint({ 62, 2, 47, 0 }, 6, 0, 1) },
				{ ExpandEndpoint({ 13, 24, 62, 0 }, 6, 0, 0), ExpandEndpoint({ 50, 52, 1, 0 }, 6, 0, 0) } };
			check(fmt::format("mode 1, partition {}", partition), CreatePartitionedBlock(1, partition, mode1, { 0, 7, 3, 5, 1, 6, 2, 4 }), 1, partition);

			const std::vector<std::array<Endpoint, 2>> mode7 = {
				{ ExpandEndpoint({ 1, 30, 5, 9 }, 5, 5, 0), ExpandEndpoint({ 31, 2, 23, 9 }, 5, 5, 0) },
				{ ExpandEndpoint({ 7, 12, 30, 25 }, 5, 5, 1), ExpandEndpoint({ 25, 26, 1, 25 }, 5, 5, 1) } };
			check(fmt::format("mode 7, partition {}", partition), CreatePartitionedBlock(7, partition, mode7, { 0, 3, 1, 2 }), 7, partition);
		}

		// Three gradients in every three-subset partition: mode 2 (five-bit endpoints, four levels) and, in the first 16,
		// mode 0 (four-bit endpoints with p-bits, eight levels).
		for (uint32_t partition = 0; partition < 64; partition++)
		{
			const std::vector<std::array<Endpoint, 2>> mode2 = {
				{ ExpandEndpoint({ 1, 30, 9, 0 }, 5, 0), ExpandEndpoint({ 29, 2, 22, 0 }, 5, 0) },
				{ ExpandEndpoint({ 6, 13, 30, 0 }, 5, 0), ExpandEndpoint({ 25, 27, 2, 0 }, 5, 0) },
				{ ExpandEndpoint({ 30, 18, 1, 0 }, 5, 0), ExpandEndpoint({ 3, 8, 26, 0 }, 5, 0) } };
			check(fmt::format("mode 2, partition {}", partition), CreatePartitionedBlock(2, partition, mode2, { 0, 3, 1, 2 }), 2, partition);
			if (partition >= 16)
				continue;

			const std::vector<std::array<Endpoint, 2>> mode0 = {
				{ ExpandEndpoint({ 0, 15, 4, 0 }, 4, 0, 1), ExpandEndpoint({ 15, 1, 11, 0 }, 4, 0, 0) },
				{ ExpandEndpoint({ 3, 6, 15, 0 }, 4, 0, 0), ExpandEndpoint({ 12, 13, 0, 0 }, 4, 0, 1) },
				{ ExpandEndpoint({ 15, 9, 0, 0 }, 4, 0, 1), ExpandEndpoint({ 1, 4, 13, 0 }, 4, 0, 1) } };
			check(fmt::format("mode 0, partition {}", partition), CreatePartitionedBlock(0, partition, mode0, { 0, 7, 3, 5, 1, 6, 2, 4 }), 0, partition);
		}
	}

	TEST_CASE("The BC7 encoder stores flat colors exactly")
	{
		std::mt19937 random(7);
		for (uint32_t value = 0; value < 256; value++)
		{
			CAPTURE(value);
			Block gray {};
			gray.fill(Texel { static_cast<uint8_t>(value), static_cast<uint8_t>(value), static_cast<uint8_t>(value), 255 });
			Block color {};
			color.fill(Texel { static_cast<uint8_t>(random()), static_cast<uint8_t>(value), static_cast<uint8_t>(random()), static_cast<uint8_t>(random()) });
			for (const Block& source : { gray, color })
			{
				for (bool highQuality : { true, false })
				{
					const EncodedBlock encoded = Encode(source, highQuality);
					CheckRoundTrip(source, encoded);
					CHECK(encoded.Info.Mode == 5);
					CHECK(encoded.Error == 0);
				}
			}
		}
	}

	TEST_CASE("The BC7 encoder never picks a worse mode than mode 6 and keeps opaque blocks opaque")
	{
		// 10,000 blocks of noise, gradients, edges between regions and mixes of them, opaque or with alpha that is
		// constant, noisy or follows the regions. At High the search always fits mode 6 in full besides the others, so no
		// block may decode worse than mode 6 alone gives (encoded with the same settings); every block decodes as the encoder
		// predicted, opaque blocks decode opaque, and every mode wins somewhere.
		std::mt19937 random(20261010);
		auto next = [&random](uint32_t range) { return static_cast<int32_t>(random() % range); };
		uint32_t modeCounts[8] = {};
		uint32_t fastModeCounts[8] = {};
		uint64_t highError = 0;
		uint64_t mode6Error = 0;
		uint64_t fastError = 0;
		uint32_t opaqueBlocks = 0;
		for (uint32_t blockIndex = 0; blockIndex < 10000; blockIndex++)
		{
			CAPTURE(blockIndex);
			// Up to three regions (a random partition of BC7's), each a gradient between random colors plus noise.
			const uint32_t regions = static_cast<uint32_t>(next(3)) + 1;
			const uint32_t partition = static_cast<uint32_t>(next(64));
			const int32_t noise = std::array<int32_t, 4> { 0, 4, 24, 256 }[blockIndex % 4];
			const uint32_t alphaKind = static_cast<uint32_t>(next(4)); // Opaque, constant, noisy, per region
			int32_t colors[3][2][4];
			for (auto& region : colors)
			{
				for (auto& endpoint : region)
				{
					for (int32_t& value : endpoint)
						value = next(256);
				}
			}
			const int32_t constantAlpha = next(256);
			Block source {};
			for (uint32_t texel = 0; texel < 16; texel++)
			{
				const uint32_t region = regions == 1 ? 0 : BC7::GetSubset(regions, partition, texel);
				const int32_t position = next(65);
				for (uint32_t channel = 0; channel < 4; channel++)
				{
					const int32_t base = (colors[region][0][channel] * (64 - position) + colors[region][1][channel] * position) / 64;
					const int32_t jitter = noise == 0 ? 0 : next(static_cast<uint32_t>(noise)) - noise / 2;
					source[texel][channel] = static_cast<uint8_t>(std::clamp(noise == 256 ? next(256) : base + jitter, 0, 255));
				}
				switch (alphaKind)
				{
					case 0: source[texel][3] = 255; break;
					case 1: source[texel][3] = static_cast<uint8_t>(constantAlpha); break;
					case 2: break;
					default: source[texel][3] = static_cast<uint8_t>(colors[region][0][3]); break;
				}
			}

			const EncodedBlock high = Encode(source);
			const Block decoded = CheckRoundTrip(source, high);
			const EncodedBlock mode6 = Encode(source, true, 1u << 6);
			CheckRoundTrip(source, mode6);
			CHECK(mode6.Info.Mode == 6);
			REQUIRE(high.Error <= mode6.Error);
			const EncodedBlock fast = Encode(source, false);
			CheckRoundTrip(source, fast);
			CHECK((fast.Info.Mode == 6 || fast.Info.Mode == 5));
			modeCounts[high.Info.Mode]++;
			fastModeCounts[fast.Info.Mode]++;
			highError += high.Error;
			mode6Error += mode6.Error;
			fastError += fast.Error;

			if (alphaKind == 0)
			{
				opaqueBlocks++;
				for (const Block& opaque : { decoded, ToBlock(mode6.Info.Decoded), ToBlock(fast.Info.Decoded) })
				{
					for (const Texel& texel : opaque)
						REQUIRE(texel[3] == 255);
				}
			}
		}
		MESSAGE(fmt::format("High: modes 0-7 {} {} {} {} {} {} {} {}, total error {} (mode 6 alone {}); Fast: modes 5 and 6 {} {}, total error {}; {} opaque blocks",
			modeCounts[0], modeCounts[1], modeCounts[2], modeCounts[3], modeCounts[4], modeCounts[5], modeCounts[6], modeCounts[7], highError, mode6Error,
			fastModeCounts[5], fastModeCounts[6], fastError, opaqueBlocks));
		// The search reaches every mode.
		for (uint32_t mode = 0; mode < 8; mode++)
		{
			CAPTURE(mode);
			CHECK(modeCounts[mode] > 0);
		}
		CHECK(highError < mode6Error);
		CHECK(highError <= fastError);
	}
}
