#include "stpch.h"
#include "Strata/Renderer/TextureFormat.h"

namespace Strata
{

	TextureFormatInfo GetTextureFormatInfo(TextureFormat format)
	{
		auto uncompressed = [](uint32_t bytesPerTexel, uint32_t channels, bool srgb)
		{
			return TextureFormatInfo { 1, 1, bytesPerTexel, channels, srgb, false };
		};
		auto compressed = [](uint32_t bytesPerBlock, uint32_t channels, bool srgb)
		{
			return TextureFormatInfo { 4, 4, bytesPerBlock, channels, srgb, true };
		};
		switch (format)
		{
			case TextureFormat::RGBA8:     return uncompressed(4, 4, false);
			case TextureFormat::RGBA8SRGB: return uncompressed(4, 4, true);
			case TextureFormat::RGBA16F:   return uncompressed(8, 4, false);
			case TextureFormat::RGBA32F:   return uncompressed(16, 4, false);
			case TextureFormat::R8:        return uncompressed(1, 1, false);
			case TextureFormat::RG8:       return uncompressed(2, 2, false);
			case TextureFormat::BC1:       return compressed(8, 4, false);
			case TextureFormat::BC1SRGB:   return compressed(8, 4, true);
			case TextureFormat::BC3:       return compressed(16, 4, false);
			case TextureFormat::BC3SRGB:   return compressed(16, 4, true);
			case TextureFormat::BC4:       return compressed(8, 1, false);
			case TextureFormat::BC5:       return compressed(16, 2, false);
			case TextureFormat::BC7:       return compressed(16, 4, false);
			case TextureFormat::BC7SRGB:   return compressed(16, 4, true);
			case TextureFormat::None:      return TextureFormatInfo { 1, 1, 0, 0, false, false };
		}
		return TextureFormatInfo { 1, 1, 0, 0, false, false };
	}

	bool IsValidTextureFormat(uint32_t value)
	{
		return value >= static_cast<uint32_t>(TextureFormat::RGBA8) && value <= static_cast<uint32_t>(c_LastTextureFormat);
	}

	bool IsBlockCompressedFormat(TextureFormat format)
	{
		return GetTextureFormatInfo(format).IsBlockCompressed;
	}

	bool IsHDRFormat(TextureFormat format)
	{
		return format == TextureFormat::RGBA16F || format == TextureFormat::RGBA32F;
	}

	uint64_t GetMipByteSize(TextureFormat format, uint32_t width, uint32_t height)
	{
		const TextureFormatInfo info = GetTextureFormatInfo(format);
		const uint64_t blocksX = (static_cast<uint64_t>(width) + info.BlockWidth - 1) / info.BlockWidth;
		const uint64_t blocksY = (static_cast<uint64_t>(height) + info.BlockHeight - 1) / info.BlockHeight;
		return blocksX * blocksY * info.BytesPerBlock;
	}

	uint64_t GetBlockRowByteSize(TextureFormat format, uint32_t width)
	{
		const TextureFormatInfo info = GetTextureFormatInfo(format);
		return (static_cast<uint64_t>(width) + info.BlockWidth - 1) / info.BlockWidth * info.BytesPerBlock;
	}

	TextureFormat GetDecodedFormat(TextureFormat format)
	{
		switch (format)
		{
			case TextureFormat::BC1:
			case TextureFormat::BC3:
			case TextureFormat::BC7:     return TextureFormat::RGBA8;
			case TextureFormat::BC1SRGB:
			case TextureFormat::BC3SRGB:
			case TextureFormat::BC7SRGB: return TextureFormat::RGBA8SRGB;
			case TextureFormat::BC4:     return TextureFormat::R8;
			case TextureFormat::BC5:     return TextureFormat::RG8;
			default:                     return format;
		}
	}

	const char* TextureFormatToString(TextureFormat format)
	{
		switch (format)
		{
			case TextureFormat::None:      return "None";
			case TextureFormat::RGBA8:     return "RGBA8";
			case TextureFormat::RGBA8SRGB: return "RGBA8SRGB";
			case TextureFormat::RGBA16F:   return "RGBA16F";
			case TextureFormat::RGBA32F:   return "RGBA32F";
			case TextureFormat::R8:        return "R8";
			case TextureFormat::RG8:       return "RG8";
			case TextureFormat::BC1:       return "BC1";
			case TextureFormat::BC1SRGB:   return "BC1SRGB";
			case TextureFormat::BC3:       return "BC3";
			case TextureFormat::BC3SRGB:   return "BC3SRGB";
			case TextureFormat::BC4:       return "BC4";
			case TextureFormat::BC5:       return "BC5";
			case TextureFormat::BC7:       return "BC7";
			case TextureFormat::BC7SRGB:   return "BC7SRGB";
		}
		return "Unknown";
	}

	nvrhi::Format ToNvrhiFormat(TextureFormat format)
	{
		switch (format)
		{
			case TextureFormat::RGBA8:     return nvrhi::Format::RGBA8_UNORM;
			case TextureFormat::RGBA8SRGB: return nvrhi::Format::SRGBA8_UNORM;
			case TextureFormat::RGBA16F:   return nvrhi::Format::RGBA16_FLOAT;
			case TextureFormat::RGBA32F:   return nvrhi::Format::RGBA32_FLOAT;
			case TextureFormat::R8:        return nvrhi::Format::R8_UNORM;
			case TextureFormat::RG8:       return nvrhi::Format::RG8_UNORM;
			case TextureFormat::BC1:       return nvrhi::Format::BC1_UNORM;
			case TextureFormat::BC1SRGB:   return nvrhi::Format::BC1_UNORM_SRGB;
			case TextureFormat::BC3:       return nvrhi::Format::BC3_UNORM;
			case TextureFormat::BC3SRGB:   return nvrhi::Format::BC3_UNORM_SRGB;
			case TextureFormat::BC4:       return nvrhi::Format::BC4_UNORM;
			case TextureFormat::BC5:       return nvrhi::Format::BC5_UNORM;
			case TextureFormat::BC7:       return nvrhi::Format::BC7_UNORM;
			case TextureFormat::BC7SRGB:   return nvrhi::Format::BC7_UNORM_SRGB;
			case TextureFormat::None:      return nvrhi::Format::UNKNOWN;
		}
		return nvrhi::Format::UNKNOWN;
	}

}
