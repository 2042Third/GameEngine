#pragma once

#include "Strata/Renderer/TextureFormat.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace Strata::BlockCompression
{

	// CPU decoders of the block-compressed formats, written from the formats' specifications (the Khronos Data Format
	// Specification, sections "BC1, BC2 and BC3", "BC4 and BC5" and "BC7"). Devices without BC support get textures
	// decoded with them at load time, and tests compare the GPU's sampling against them.
	//
	// Interpolated values round to the nearest 8-bit value; hardware may differ by one step for BC1 and BC3 colors (the
	// specifications leave that rounding open) and decodes BC4 and BC5 at a higher precision. BC7 is exact.

	// One block of `format` into 4x4 texels of GetDecodedFormat(format), rows `rowPitch` bytes apart: RGBA for BC1, BC3 and
	// BC7 (sRGB formats yield their encoded bytes), R for BC4, RG for BC5. Nothing is written for other formats.
	void DecodeBlock(TextureFormat format, const uint8_t* block, uint8_t* texels, size_t rowPitch);

	// BC1 color block (8 bytes) into RGBA texels. With fourColors (the color half of BC2 and BC3) the endpoints are always
	// interpolated; otherwise endpoints in ascending order select three colors and transparent black.
	void DecodeBC1Block(const uint8_t* block, uint8_t* rgba, size_t rowPitch, bool fourColors = false);
	// BC4 block (8 bytes) into one channel: `stride` bytes between texels of a row, `rowPitch` between rows.
	void DecodeBC4Block(const uint8_t* block, uint8_t* channel, size_t stride, size_t rowPitch);
	// BC7 block (16 bytes) into RGBA texels. Blocks of the reserved mode decode to zero in every channel.
	void DecodeBC7Block(const uint8_t* block, uint8_t* rgba, size_t rowPitch);

	// Decodes a level of `width` x `height` texels: `blocks` holds GetMipByteSize(format, width, height) bytes, and
	// `texels` receives the level in GetDecodedFormat(format), tightly packed rows. False (writing nothing) when the format
	// is not block-compressed or a size does not match.
	bool DecodeLevel(TextureFormat format, uint32_t width, uint32_t height, std::span<const uint8_t> blocks, std::span<uint8_t> texels);

}
