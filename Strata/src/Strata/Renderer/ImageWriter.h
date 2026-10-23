#pragma once

#include "Strata/Renderer/Renderer.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	// Encodes read-back images. Supports 8-bit RGBA/BGRA formats (UNORM and sRGB); alpha can be forced opaque,
	// which is what screenshots of the back buffer want. HDR images are encoded from floats (EncodeHDR).
	class ImageWriter
	{
	public:
		static std::optional<std::vector<uint8_t>> EncodePNG(const ReadbackImage& image, bool forceOpaque = true, std::string* outError = nullptr);
		static bool SavePNG(const ReadbackImage& image, const std::filesystem::path& path, bool forceOpaque = true, std::string* outError = nullptr);
		// A Radiance .hdr file (RGBE, run-length encoded) of `width` x `height` linear RGB floats, rows top first: what the HDR
		// texture importer reads. Negative and non-finite values are written as 0. Empty or short pixels fail (outError).
		static std::optional<std::vector<uint8_t>> EncodeHDR(uint32_t width, uint32_t height, const std::vector<float>& rgb, std::string* outError = nullptr);
	};

}
