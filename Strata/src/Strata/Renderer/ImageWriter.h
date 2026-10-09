#pragma once

#include "Strata/Renderer/Renderer.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	// Encodes read-back images. Supports 8-bit RGBA/BGRA formats (UNORM and sRGB); alpha can be forced opaque,
	// which is what screenshots of the back buffer want.
	class ImageWriter
	{
	public:
		static std::optional<std::vector<uint8_t>> EncodePNG(const ReadbackImage& image, bool forceOpaque = true, std::string* outError = nullptr);
		static bool SavePNG(const ReadbackImage& image, const std::filesystem::path& path, bool forceOpaque = true, std::string* outError = nullptr);
	};

}
