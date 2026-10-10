#include "stpch.h"
#include "Strata/Renderer/BlockCompression.h"

#include "Strata/Renderer/BC7Format.h"

namespace Strata::BlockCompression
{

	namespace
	{

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

		// BC1 expands its 5- and 6-bit endpoint values like BC7 does: their top bits are replicated into the bottom.
		void Expand565(uint16_t color, uint8_t* rgba)
		{
			rgba[0] = BC7::ExpandBits((color >> 11) & 0x1F, 5);
			rgba[1] = BC7::ExpandBits((color >> 5) & 0x3F, 6);
			rgba[2] = BC7::ExpandBits(color & 0x1F, 5);
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

		const BC7::ModeInfo& mode = BC7::c_Modes[modeIndex];
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
				endpoints[endpoint][channel] = BC7::ExpandBits(value, valueBits);
			}
		}

		auto isAnchor = [&](uint32_t texel)
		{
			for (uint32_t subset = 0; subset < mode.Subsets; subset++)
			{
				if (texel == BC7::GetAnchorTexel(mode.Subsets, partition, subset))
					return true;
			}
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
			const uint32_t subset = BC7::GetSubset(mode.Subsets, partition, texel);
			const uint8_t* first = endpoints[subset * 2];
			const uint8_t* second = endpoints[subset * 2 + 1];

			// Modes 4 and 5 have separate color and alpha indices; mode 4's index selection bit swaps their widths.
			uint32_t colorWeight = BC7::GetWeights(mode.IndexBits)[indices[texel]];
			uint32_t alphaWeight = colorWeight;
			if (mode.SecondaryIndexBits > 0)
			{
				const uint32_t secondaryWeight = BC7::GetWeights(mode.SecondaryIndexBits)[secondaryIndices[texel]];
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
				texelColor[channel] = BC7::Interpolate(first[channel], second[channel], colorWeight);
			texelColor[3] = BC7::Interpolate(first[3], second[3], alphaWeight);
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
