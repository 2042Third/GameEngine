#include "stpch.h"
#include "Strata/Renderer/ImageReader.h"

#include <stb_image.h>

#include <climits>
#include <cstring>

namespace Strata
{

	std::optional<ReadbackImage> ImageReader::DecodeRGBA8(std::span<const uint8_t> encoded, std::string* outError)
	{
		auto fail = [outError](std::string message) -> std::optional<ReadbackImage>
		{
			if (outError)
				*outError = std::move(message);
			return std::nullopt;
		};

		if (encoded.empty() || encoded.size() > static_cast<size_t>(INT_MAX))
			return fail("The image is empty or too large");
		const int size = static_cast<int>(encoded.size());
		int width = 0;
		int height = 0;
		int channels = 0;
		// The size first, so that a huge image is refused before anything is allocated for it.
		if (!stbi_info_from_memory(encoded.data(), size, &width, &height, &channels))
			return fail(fmt::format("Not an image: {}", stbi_failure_reason() ? stbi_failure_reason() : "unknown format"));
		if (width <= 0 || height <= 0 || static_cast<uint32_t>(width) > c_MaxDimension || static_cast<uint32_t>(height) > c_MaxDimension)
			return fail(fmt::format("The image is {}x{}: at most {} pixels on a side are read", width, height, c_MaxDimension));

		stbi_uc* pixels = stbi_load_from_memory(encoded.data(), size, &width, &height, &channels, 4);
		if (!pixels)
			return fail(fmt::format("The image could not be decoded: {}", stbi_failure_reason() ? stbi_failure_reason() : "unknown error"));
		ReadbackImage image;
		image.Width = static_cast<uint32_t>(width);
		image.Height = static_cast<uint32_t>(height);
		image.Format = nvrhi::Format::RGBA8_UNORM;
		image.BytesPerPixel = 4;
		image.Pixels.resize(static_cast<size_t>(image.Width) * image.Height * 4);
		std::memcpy(image.Pixels.data(), pixels, image.Pixels.size());
		stbi_image_free(pixels);
		return image;
	}

}
