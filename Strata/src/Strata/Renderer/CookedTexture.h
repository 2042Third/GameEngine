#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Renderer/TextureFormat.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// The cooked texture format ("STTX"), the stored form of texture assets in the import cache and in packs.
	//
	// Version 2 (written since texture cooking v2), little-endian:
	//
	//   CookedTextureHeader   magic "STTX", version, format, width, height, mip count, flags, filter, wrap, name length,
	//                         data offset (where the first level's pixels start)
	//   mip table             per level, largest (level 0) first: width, height, offset, size (offsets from the start)
	//   debug name            UTF-8, name length bytes
	//   padding               to the data offset, a multiple of c_CookedTextureAlignment
	//   pixel data            the levels smallest first, each at a multiple of c_CookedTextureAlignment: the mip tail is
	//                         one contiguous range right after the header, so streaming can read it (and later the
	//                         larger levels) by byte range
	//
	// Block-compressed levels hold whole 4x4 blocks (GetMipByteSize). Version 1 (uncompressed formats, levels largest first
	// right after their table) is still read: packs exported before version 2 contain it.
	constexpr uint32_t c_CookedTextureMagic = 0x58545453; // "STTX"
	constexpr uint32_t c_CookedTextureVersion = 2;
	constexpr uint32_t c_CookedTextureAlignment = 16;
	constexpr uint32_t c_MaxCookedTextureSize = 16384;
	constexpr uint32_t c_MaxCookedTextureMips = 15; // A full chain of a 16384-texel side
	constexpr uint32_t c_MaxCookedTextureNameLength = 1024;

	// Flags of cooked textures. Readers reject bits they do not know, so a writer that needs a feature older readers would
	// misread sets a new bit.
	enum class CookedTextureFlags : uint32_t
	{
		None = 0,
		// The texture stores one channel that stands for gray (BC4 or R8): it is sampled as (r, r, r, 1).
		Grayscale = 1u << 0
	};
	ST_DEFINE_ENUM_FLAG_OPERATORS(CookedTextureFlags)
	constexpr CookedTextureFlags c_KnownCookedTextureFlags = CookedTextureFlags::Grayscale;

	struct CookedTextureLevel
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint64_t Offset = 0; // From the start of the cooked data
		uint64_t Size = 0;
	};

	// A range of cooked data covering whole mip levels: [Offset, Offset + Size) holds levels FirstLevel to
	// FirstLevel + LevelCount - 1.
	struct CookedTextureByteRange
	{
		uint64_t Offset = 0;
		uint64_t Size = 0;
		uint32_t FirstLevel = 0;
		uint32_t LevelCount = 0;
	};

	// The levels' layout of a cooked texture, without its pixels.
	struct CookedTextureLayout
	{
		uint32_t Version = c_CookedTextureVersion;
		TextureFormat Format = TextureFormat::None; // The format the levels are stored in
		uint64_t HeaderSize = 0; // Bytes before the pixel data: header, mip table, name and padding
		uint32_t MipCount = 0;
		std::array<CookedTextureLevel, c_MaxCookedTextureMips> Levels {}; // Level 0 (the largest) first

		// The levels whose width and height are at most maxDimension, which streaming reads first (the mip tail). In version
		// 2 data they lie right after the header; empty (at HeaderSize) when even the smallest level is larger.
		CookedTextureByteRange GetMipTailByteRange(uint32_t maxDimension = 128) const;
	};

	// A validated cooked texture: its description and where each level's pixels are, referring to the data it was parsed
	// from (nothing is copied).
	struct CookedTextureView
	{
		TextureFormat Format = TextureFormat::None;
		TextureFilter Filter = TextureFilter::Linear;
		TextureWrap Wrap = TextureWrap::Repeat;
		CookedTextureFlags Flags = CookedTextureFlags::None;
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::string_view DebugName;
		CookedTextureLayout Layout;
		std::span<const uint8_t> Data; // The whole cooked texture

		std::span<const uint8_t> GetLevelData(uint32_t level) const;
	};

	// Parses and validates cooked data of either version: the header, the formats and sizes of every level (whole blocks),
	// offsets inside the data, levels that do not overlap and, in version 2, that are stored smallest first after the
	// header. nullopt with the reason in outError for anything else.
	std::optional<CookedTextureView> ParseCookedTexture(std::span<const uint8_t> data, std::string* outError = nullptr);

	struct CookedTextureDescription
	{
		TextureFormat Format = TextureFormat::None;
		TextureFilter Filter = TextureFilter::Linear;
		TextureWrap Wrap = TextureWrap::Repeat;
		CookedTextureFlags Flags = CookedTextureFlags::None;
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t MipCount = 0;
		std::string_view DebugName; // Longer names are cut to c_MaxCookedTextureNameLength bytes
	};

	// Where WriteCookedTexture puts the levels of a texture (version 2). The description must be valid: a format, a size
	// within c_MaxCookedTextureSize and a mip count the size allows.
	CookedTextureLayout ComputeCookedTextureLayout(const CookedTextureDescription& description);
	// Writes version 2 data: `levels` holds each level's bytes, level 0 first, sized as GetMipByteSize says. Empty when the
	// description or a level's size is invalid.
	std::vector<uint8_t> WriteCookedTexture(const CookedTextureDescription& description, std::span<const std::span<const uint8_t>> levels);

}
