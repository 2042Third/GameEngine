#pragma once

#include "Strata/Renderer/Texture.h"

#include <cstdint>
#include <string>
#include <vector>

// Texture cooking for the asset pipeline: mip chains filtered with stb_image_resize2 and levels encoded into their stored
// format, block-compressed with stb_dxt on the job system's workers. Only importers use it; games load cooked textures.
namespace Strata::TextureCooker
{

	// Texels of one level, 8 bits per channel, tightly packed rows. One channel is gray, two are gray and alpha, three RGB
	// and four RGBA.
	struct Image
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t Channels = 0;
		std::vector<uint8_t> Pixels;
	};

	// The same with four 32-bit float channels (HDR).
	struct FloatImage
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::vector<float> Pixels;
	};

	struct CookSettings
	{
		TextureFormat Format = TextureFormat::RGBA8;
		bool GenerateMips = true;
		bool SRGB = false;          // Color: filtered in linear space
		bool AlphaWeighted = false; // Color with alpha: alpha is coverage, so color is weighted by it while filtering
		bool NormalMap = false;     // RGB holds unit vectors: renormalized in every level
		TextureWrap Wrap = TextureWrap::Repeat; // How filters read past the edges
		bool HighQuality = true;    // Spend more time on block endpoints
	};

	// Whether CookLevels can store levels in `format`: the uncompressed 8-bit formats and BC1, BC3, BC4 and BC5.
	bool CanEncode(TextureFormat format);

	// Cooks `level0` into a mip chain in settings.Format: the level is encoded, then each further level is filtered from
	// the previous one with a Mitchell filter and encoded, so only two levels' texels are held at a time. False with the
	// reason when the format cannot be encoded or filtering fails.
	bool CookLevels(Image level0, const CookSettings& settings, std::vector<TextureMip>& outMips, std::string* outError = nullptr);
	// The same for HDR images into RGBA16F, filtered with a box filter (a sharper filter rings around bright suns into
	// negative light).
	bool CookHDRLevels(FloatImage level0, bool generateMips, TextureWrap wrap, std::vector<TextureMip>& outMips, std::string* outError = nullptr);

	// Encodes one level into `format` (CanEncode); block rows are encoded in parallel on the job system. Edge blocks
	// repeat the level's last row and column.
	bool EncodeLevel(const Image& image, TextureFormat format, bool highQuality, std::vector<uint8_t>& outData);

	// Makes the RGB texels of a normal map unit vectors again (filtering shortens them; mapped from [0, 255] to [-1, 1]).
	void NormalizeNormalMap(Image& image);

}
