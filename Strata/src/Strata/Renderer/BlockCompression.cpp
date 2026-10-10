#include "stpch.h"
#include "Strata/Renderer/BlockCompression.h"

namespace Strata::BlockCompression
{

	namespace
	{

		// BC7 partitions of two subsets: bit i is the subset of texel i (row-major).
		constexpr uint16_t c_BC7Partitions2[64] = {
			0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
			0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
			0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
			0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22 };

		// BC7 partitions of three subsets: the subset of each texel (row-major). Mode 0 uses the first 16.
		constexpr uint8_t c_BC7Partitions3[64][16] = {
			{ 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2 }, { 0, 0, 0, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1 },
			{ 0, 0, 0, 0, 2, 0, 0, 1, 2, 2, 1, 1, 2, 2, 1, 1 }, { 0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 1, 1, 1 },
			{ 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2 }, { 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2 },
			{ 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1 }, { 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1 },
			{ 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2 }, { 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2 },
			{ 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2 }, { 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2 },
			{ 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2 }, { 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2 },
			{ 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2 }, { 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0, 2, 2, 2, 0 },
			{ 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2 }, { 0, 1, 1, 1, 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0 },
			{ 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2 }, { 0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1 },
			{ 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 0, 2, 2, 2 }, { 0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 1, 2, 2, 2, 1 },
			{ 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2 }, { 0, 0, 0, 0, 1, 1, 0, 0, 2, 2, 1, 0, 2, 2, 1, 0 },
			{ 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0 }, { 0, 0, 1, 2, 0, 0, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2 },
			{ 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0 }, { 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1 },
			{ 0, 0, 2, 2, 1, 1, 0, 2, 1, 1, 0, 2, 0, 0, 2, 2 }, { 0, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 2, 2, 2, 2 },
			{ 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1 }, { 0, 0, 0, 0, 2, 0, 0, 0, 2, 2, 1, 1, 2, 2, 2, 1 },
			{ 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 2, 2, 2 }, { 0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 2, 0, 0, 1, 1 },
			{ 0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0, 2, 2, 2 }, { 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0 },
			{ 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0 }, { 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0 },
			{ 0, 1, 2, 0, 2, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 0 }, { 0, 0, 1, 1, 2, 2, 0, 0, 1, 1, 2, 2, 0, 0, 1, 1 },
			{ 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1 }, { 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2 },
			{ 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1 }, { 0, 0, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 1, 1, 2, 2 },
			{ 0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 1, 1 }, { 0, 2, 2, 0, 1, 2, 2, 1, 0, 2, 2, 0, 1, 2, 2, 1 },
			{ 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 0, 1 }, { 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1 },
			{ 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2 }, { 0, 2, 2, 2, 0, 1, 1, 1, 0, 2, 2, 2, 0, 1, 1, 1 },
			{ 0, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0, 2, 1, 1, 1, 2 }, { 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2 },
			{ 0, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2 }, { 0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 2 },
			{ 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2 }, { 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2 },
			{ 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2 }, { 0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2 },
			{ 0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2 }, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2 },
			{ 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1 }, { 0, 2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 1, 2, 2, 2 },
			{ 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 }, { 0, 1, 1, 1, 2, 0, 1, 1, 2, 2, 0, 1, 2, 2, 2, 0 } };

		// The texel of each subset after the first whose index is stored with one bit less (its top bit is zero); subset 0's
		// is always texel 0.
		constexpr uint8_t c_BC7Anchors2[64] = {
			15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
			15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
			15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
			6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15 };
		constexpr uint8_t c_BC7Anchors3Second[64] = {
			3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3,
			3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
			8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15,
			3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3 };
		constexpr uint8_t c_BC7Anchors3Third[64] = {
			15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8,
			15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
			15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8,
			15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8 };

		constexpr uint8_t c_BC7Weights2[4] = { 0, 21, 43, 64 };
		constexpr uint8_t c_BC7Weights3[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };
		constexpr uint8_t c_BC7Weights4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

		struct BC7Mode
		{
			uint8_t Subsets;
			uint8_t PartitionBits;
			uint8_t RotationBits;
			uint8_t IndexSelectionBits;
			uint8_t ColorBits;
			uint8_t AlphaBits;
			uint8_t EndpointPBits; // One p-bit per endpoint
			uint8_t SharedPBits;   // One p-bit per subset, shared by its endpoints
			uint8_t IndexBits;
			uint8_t SecondaryIndexBits;
		};

		constexpr BC7Mode c_BC7Modes[8] = {
			{ 3, 4, 0, 0, 4, 0, 1, 0, 3, 0 },
			{ 2, 6, 0, 0, 6, 0, 0, 1, 3, 0 },
			{ 3, 6, 0, 0, 5, 0, 0, 0, 2, 0 },
			{ 2, 6, 0, 0, 7, 0, 1, 0, 2, 0 },
			{ 1, 0, 2, 1, 5, 6, 0, 0, 2, 3 },
			{ 1, 0, 2, 0, 7, 8, 0, 0, 2, 2 },
			{ 1, 0, 0, 0, 7, 7, 1, 0, 4, 0 },
			{ 2, 6, 0, 0, 5, 5, 1, 0, 2, 0 } };

		// Reads the fields of a 128-bit block, least significant bit first.
		class BlockBitReader
		{
		public:
			explicit BlockBitReader(const uint8_t* block)
			{
				std::memcpy(&m_Low, block, sizeof(m_Low));
				std::memcpy(&m_High, block + sizeof(m_Low), sizeof(m_High));
			}

			uint64_t GetLow() const { return m_Low; }
			void Skip(uint32_t count) { m_Position += count; }

			// At most 32 bits; bits past the block read as zero.
			uint32_t Read(uint32_t count)
			{
				if (count == 0)
					return 0;
				uint64_t value = 0;
				if (m_Position == 0)
					value = m_Low;
				else if (m_Position < 64)
					value = (m_Low >> m_Position) | (m_High << (64 - m_Position));
				else if (m_Position < 128)
					value = m_High >> (m_Position - 64);
				m_Position += count;
				return static_cast<uint32_t>(value & ((1ull << count) - 1));
			}
		private:
			uint64_t m_Low = 0;
			uint64_t m_High = 0;
			uint32_t m_Position = 0;
		};

		// An n-bit endpoint value (n from 4 to 8) as 8 bits, its top bits replicated into the bottom.
		uint8_t ExpandBits(uint32_t value, uint32_t bits)
		{
			const uint32_t shifted = value << (8 - bits);
			return static_cast<uint8_t>(shifted | (shifted >> bits));
		}

		uint8_t InterpolateBC7(uint8_t first, uint8_t second, uint32_t weight)
		{
			return static_cast<uint8_t>(((64 - weight) * first + weight * second + 32) >> 6);
		}

		uint32_t GetBC7Weight(uint32_t index, uint32_t bits)
		{
			switch (bits)
			{
				case 2:  return c_BC7Weights2[index & 3];
				case 3:  return c_BC7Weights3[index & 7];
				default: return c_BC7Weights4[index & 15];
			}
		}

		void Expand565(uint16_t color, uint8_t* rgba)
		{
			rgba[0] = ExpandBits((color >> 11) & 0x1F, 5);
			rgba[1] = ExpandBits((color >> 5) & 0x3F, 6);
			rgba[2] = ExpandBits(color & 0x1F, 5);
			rgba[3] = 255;
		}

	}

	void DecodeBC1Block(const uint8_t* block, uint8_t* rgba, size_t rowPitch, bool fourColors)
	{
		const uint16_t color0 = static_cast<uint16_t>(block[0] | (block[1] << 8));
		const uint16_t color1 = static_cast<uint16_t>(block[2] | (block[3] << 8));
		uint8_t palette[4][4];
		Expand565(color0, palette[0]);
		Expand565(color1, palette[1]);
		if (fourColors || color0 > color1)
		{
			for (int channel = 0; channel < 3; channel++)
			{
				palette[2][channel] = static_cast<uint8_t>((2 * palette[0][channel] + palette[1][channel] + 1) / 3);
				palette[3][channel] = static_cast<uint8_t>((palette[0][channel] + 2 * palette[1][channel] + 1) / 3);
			}
			palette[2][3] = 255;
			palette[3][3] = 255;
		}
		else
		{
			for (int channel = 0; channel < 3; channel++)
			{
				palette[2][channel] = static_cast<uint8_t>((palette[0][channel] + palette[1][channel] + 1) / 2);
				palette[3][channel] = 0;
			}
			palette[2][3] = 255;
			palette[3][3] = 0;
		}

		const uint32_t indices = static_cast<uint32_t>(block[4]) | (static_cast<uint32_t>(block[5]) << 8) | (static_cast<uint32_t>(block[6]) << 16)
			| (static_cast<uint32_t>(block[7]) << 24);
		for (uint32_t texel = 0; texel < 16; texel++)
		{
			uint8_t* destination = rgba + (texel / 4) * rowPitch + (texel % 4) * 4;
			std::memcpy(destination, palette[(indices >> (texel * 2)) & 3], 4);
		}
	}

	void DecodeBC4Block(const uint8_t* block, uint8_t* channel, size_t stride, size_t rowPitch)
	{
		const uint32_t value0 = block[0];
		const uint32_t value1 = block[1];
		uint8_t palette[8] = { static_cast<uint8_t>(value0), static_cast<uint8_t>(value1) };
		if (value0 > value1)
		{
			for (uint32_t step = 1; step <= 6; step++)
				palette[step + 1] = static_cast<uint8_t>(((7 - step) * value0 + step * value1 + 3) / 7);
		}
		else
		{
			for (uint32_t step = 1; step <= 4; step++)
				palette[step + 1] = static_cast<uint8_t>(((5 - step) * value0 + step * value1 + 2) / 5);
			palette[6] = 0;
			palette[7] = 255;
		}

		uint64_t indices = 0;
		for (int byte = 0; byte < 6; byte++)
			indices |= static_cast<uint64_t>(block[2 + byte]) << (byte * 8);
		for (uint32_t texel = 0; texel < 16; texel++)
			channel[(texel / 4) * rowPitch + (texel % 4) * stride] = palette[(indices >> (texel * 3)) & 7];
	}

	void DecodeBC7Block(const uint8_t* block, uint8_t* rgba, size_t rowPitch)
	{
		BlockBitReader bits(block);
		uint32_t modeIndex = 0;
		while (modeIndex < 8 && ((bits.GetLow() >> modeIndex) & 1) == 0)
			modeIndex++;
		if (modeIndex == 8)
		{
			// The reserved mode: every channel of every texel is zero.
			for (uint32_t row = 0; row < 4; row++)
				std::memset(rgba + row * rowPitch, 0, 16);
			return;
		}
		bits.Skip(modeIndex + 1);

		const BC7Mode& mode = c_BC7Modes[modeIndex];
		const uint32_t partition = bits.Read(mode.PartitionBits);
		const uint32_t rotation = bits.Read(mode.RotationBits);
		const uint32_t indexSelection = bits.Read(mode.IndexSelectionBits);

		// Endpoints in the stored order: all reds (two per subset), then all greens, blues and alphas.
		const uint32_t endpointCount = mode.Subsets * 2u;
		uint32_t raw[6][4] = {};
		for (uint32_t channel = 0; channel < 3; channel++)
		{
			for (uint32_t endpoint = 0; endpoint < endpointCount; endpoint++)
				raw[endpoint][channel] = bits.Read(mode.ColorBits);
		}
		for (uint32_t endpoint = 0; mode.AlphaBits > 0 && endpoint < endpointCount; endpoint++)
			raw[endpoint][3] = bits.Read(mode.AlphaBits);

		uint32_t pBits[6] = {};
		if (mode.EndpointPBits)
		{
			for (uint32_t endpoint = 0; endpoint < endpointCount; endpoint++)
				pBits[endpoint] = bits.Read(1);
		}
		else if (mode.SharedPBits)
		{
			for (uint32_t subset = 0; subset < mode.Subsets; subset++)
				pBits[subset * 2] = pBits[subset * 2 + 1] = bits.Read(1);
		}
		const bool hasPBits = mode.EndpointPBits || mode.SharedPBits;

		uint8_t endpoints[6][4] = {};
		for (uint32_t endpoint = 0; endpoint < endpointCount; endpoint++)
		{
			for (uint32_t channel = 0; channel < 4; channel++)
			{
				if (channel == 3 && mode.AlphaBits == 0)
				{
					endpoints[endpoint][channel] = 255;
					continue;
				}
				uint32_t value = raw[endpoint][channel];
				uint32_t valueBits = channel == 3 ? mode.AlphaBits : mode.ColorBits;
				if (hasPBits)
				{
					value = (value << 1) | pBits[endpoint];
					valueBits++;
				}
				endpoints[endpoint][channel] = ExpandBits(value, valueBits);
			}
		}

		auto getSubset = [&](uint32_t texel) -> uint32_t
		{
			if (mode.Subsets == 2)
				return (c_BC7Partitions2[partition] >> texel) & 1u;
			if (mode.Subsets == 3)
				return c_BC7Partitions3[partition][texel];
			return 0;
		};
		auto isAnchor = [&](uint32_t texel)
		{
			if (texel == 0)
				return true;
			if (mode.Subsets == 2)
				return texel == c_BC7Anchors2[partition];
			if (mode.Subsets == 3)
				return texel == c_BC7Anchors3Second[partition] || texel == c_BC7Anchors3Third[partition];
			return false;
		};

		uint32_t indices[16] = {};
		uint32_t secondaryIndices[16] = {};
		for (uint32_t texel = 0; texel < 16; texel++)
			indices[texel] = bits.Read(mode.IndexBits - (isAnchor(texel) ? 1u : 0u));
		for (uint32_t texel = 0; mode.SecondaryIndexBits > 0 && texel < 16; texel++)
			secondaryIndices[texel] = bits.Read(mode.SecondaryIndexBits - (texel == 0 ? 1u : 0u));

		for (uint32_t texel = 0; texel < 16; texel++)
		{
			const uint32_t subset = getSubset(texel);
			const uint8_t* first = endpoints[subset * 2];
			const uint8_t* second = endpoints[subset * 2 + 1];

			// Modes 4 and 5 have separate color and alpha indices; mode 4's index selection bit swaps their widths.
			uint32_t colorWeight = GetBC7Weight(indices[texel], mode.IndexBits);
			uint32_t alphaWeight = colorWeight;
			if (mode.SecondaryIndexBits > 0)
			{
				const uint32_t secondaryWeight = GetBC7Weight(secondaryIndices[texel], mode.SecondaryIndexBits);
				if (indexSelection == 0)
				{
					alphaWeight = secondaryWeight;
				}
				else
				{
					alphaWeight = colorWeight;
					colorWeight = secondaryWeight;
				}
			}

			uint8_t texelColor[4];
			for (uint32_t channel = 0; channel < 3; channel++)
				texelColor[channel] = InterpolateBC7(first[channel], second[channel], colorWeight);
			texelColor[3] = InterpolateBC7(first[3], second[3], alphaWeight);
			if (rotation > 0)
				std::swap(texelColor[3], texelColor[rotation - 1]);
			std::memcpy(rgba + (texel / 4) * rowPitch + (texel % 4) * 4, texelColor, 4);
		}
	}

	void DecodeBlock(TextureFormat format, const uint8_t* block, uint8_t* texels, size_t rowPitch)
	{
		switch (format)
		{
			case TextureFormat::BC1:
			case TextureFormat::BC1SRGB:
				DecodeBC1Block(block, texels, rowPitch);
				break;
			case TextureFormat::BC3:
			case TextureFormat::BC3SRGB:
				// The alpha block comes first, then a color block that always interpolates four colors.
				DecodeBC1Block(block + 8, texels, rowPitch, true);
				DecodeBC4Block(block, texels + 3, 4, rowPitch);
				break;
			case TextureFormat::BC4:
				DecodeBC4Block(block, texels, 1, rowPitch);
				break;
			case TextureFormat::BC5:
				DecodeBC4Block(block, texels, 2, rowPitch);
				DecodeBC4Block(block + 8, texels + 1, 2, rowPitch);
				break;
			case TextureFormat::BC7:
			case TextureFormat::BC7SRGB:
				DecodeBC7Block(block, texels, rowPitch);
				break;
			default:
				break;
		}
	}

	bool DecodeLevel(TextureFormat format, uint32_t width, uint32_t height, std::span<const uint8_t> blocks, std::span<uint8_t> texels)
	{
		const TextureFormatInfo info = GetTextureFormatInfo(format);
		if (!info.IsBlockCompressed)
			return false;
		const TextureFormatInfo decoded = GetTextureFormatInfo(GetDecodedFormat(format));
		if (blocks.size() != GetMipByteSize(format, width, height)
			|| texels.size() != static_cast<uint64_t>(width) * height * decoded.BytesPerBlock)
			return false;

		const uint32_t blocksX = (width + 3) / 4;
		const uint32_t blocksY = (height + 3) / 4;
		const size_t texelBytes = decoded.BytesPerBlock;
		const size_t rowPitch = static_cast<size_t>(width) * texelBytes;
		uint8_t edge[4 * 4 * 4];
		for (uint32_t blockY = 0; blockY < blocksY; blockY++)
		{
			for (uint32_t blockX = 0; blockX < blocksX; blockX++)
			{
				const uint8_t* block = blocks.data() + (static_cast<size_t>(blockY) * blocksX + blockX) * info.BytesPerBlock;
				const uint32_t x = blockX * 4;
				const uint32_t y = blockY * 4;
				uint8_t* destination = texels.data() + static_cast<size_t>(y) * rowPitch + x * texelBytes;
				if (x + 4 <= width && y + 4 <= height)
				{
					DecodeBlock(format, block, destination, rowPitch);
					continue;
				}

				// Blocks over the level's edge hold texels that do not exist: decode the whole block, keep what is inside.
				DecodeBlock(format, block, edge, 4 * texelBytes);
				const uint32_t columns = std::min(4u, width - x);
				const uint32_t rows = std::min(4u, height - y);
				for (uint32_t row = 0; row < rows; row++)
					std::memcpy(destination + row * rowPitch, edge + row * 4 * texelBytes, columns * texelBytes);
			}
		}
		return true;
	}

}
