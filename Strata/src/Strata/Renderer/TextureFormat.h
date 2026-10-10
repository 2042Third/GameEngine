#pragma once

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace Strata
{

	// Pixel formats of textures. The numeric values are part of the cooked file format: append only, never reorder.
	enum class TextureFormat : uint32_t
	{
		None = 0,
		RGBA8 = 1,     // Linear 8-bit (normal maps, masks, data)
		RGBA8SRGB = 2, // sRGB-encoded 8-bit color
		RGBA16F = 3,   // HDR (environment maps)
		RGBA32F = 4,
		R8 = 5,
		RG8 = 6,
		// Block-compressed formats store 4x4 texel blocks; GPUs sample them directly.
		BC1 = 7,      // RGB with 1-bit alpha, 8 bytes per block (opaque color, data)
		BC1SRGB = 8,
		BC3 = 9,      // RGBA: BC1 color plus a BC4 alpha block, 16 bytes per block
		BC3SRGB = 10,
		BC4 = 11,     // One channel, 8 bytes per block (masks, grayscale data)
		BC5 = 12,     // Two channels, 16 bytes per block (normal maps: X and Y, Z is reconstructed)
		BC7 = 13,     // RGB or RGBA, 16 bytes per block
		BC7SRGB = 14
	};

	constexpr TextureFormat c_LastTextureFormat = TextureFormat::BC7SRGB;

	// How a format stores texels: uncompressed formats are blocks of 1x1 texels.
	struct TextureFormatInfo
	{
		uint32_t BlockWidth = 1;
		uint32_t BlockHeight = 1;
		uint32_t BytesPerBlock = 0; // 0 for TextureFormat::None
		uint32_t Channels = 0;      // Channels the format stores (BC1 counts as RGBA)
		bool IsSRGB = false;
		bool IsBlockCompressed = false;
	};

	enum class TextureFilter : uint8_t
	{
		Linear = 0,
		Nearest
	};

	enum class TextureWrap : uint8_t
	{
		Repeat = 0,
		Clamp,
		Mirror
	};

	TextureFormatInfo GetTextureFormatInfo(TextureFormat format);
	// Whether a stored number names a format (TextureFormat::None does not).
	bool IsValidTextureFormat(uint32_t value);
	bool IsBlockCompressedFormat(TextureFormat format);
	bool IsHDRFormat(TextureFormat format);
	// Bytes of one mip level of `width` x `height` texels, rounded up to whole blocks (0 for TextureFormat::None).
	uint64_t GetMipByteSize(TextureFormat format, uint32_t width, uint32_t height);
	// Bytes of one row of blocks of a level `width` texels wide: a row of texels for uncompressed formats, four for the
	// block-compressed ones.
	uint64_t GetBlockRowByteSize(TextureFormat format, uint32_t width);
	// The uncompressed format a block-compressed one decodes to (BC4: R8, BC5: RG8, the others RGBA8 or RGBA8SRGB);
	// uncompressed formats are their own.
	TextureFormat GetDecodedFormat(TextureFormat format);
	const char* TextureFormatToString(TextureFormat format);
	nvrhi::Format ToNvrhiFormat(TextureFormat format);

}
