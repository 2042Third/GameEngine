#pragma once

#include "Strata/Renderer/Renderer.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace Strata
{

	// Decodes small encoded images the engine wrote itself (ImageWriter's PNGs, e.g. the editor's thumbnail cache) into
	// RGBA8 pixels. Not for asset import: textures go through TextureImporter.
	class ImageReader
	{
	public:
		// Images larger than this on a side are refused (an engine-written image is never that large).
		static constexpr uint32_t c_MaxDimension = 4096;

		// Any format stb_image reads, as RGBA8_UNORM pixels (4 bytes per pixel, top row first). nullopt with the reason
		// for data that is not such an image or that is larger than c_MaxDimension. Thread-safe.
		static std::optional<ReadbackImage> DecodeRGBA8(std::span<const uint8_t> encoded, std::string* outError = nullptr);
	};

}
