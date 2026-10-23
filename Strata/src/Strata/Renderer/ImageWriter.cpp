#include "stpch.h"
#include "Strata/Renderer/ImageWriter.h"

#include "Strata/Core/FileSystem.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace Strata
{

	std::optional<std::vector<uint8_t>> ImageWriter::EncodePNG(const ReadbackImage& image, bool forceOpaque, std::string* outError)
	{
		if (image.Width == 0 || image.Height == 0)
		{
			if (outError)
				*outError = "Image is empty";
			return std::nullopt;
		}

		bool swapRedBlue = false;
		switch (image.Format)
		{
			case nvrhi::Format::RGBA8_UNORM:
			case nvrhi::Format::SRGBA8_UNORM:
				break;
			case nvrhi::Format::BGRA8_UNORM:
			case nvrhi::Format::SBGRA8_UNORM:
				swapRedBlue = true;
				break;
			default:
				if (outError)
					*outError = fmt::format("Cannot encode format {} as PNG", nvrhi::getFormatInfo(image.Format).name);
				return std::nullopt;
		}

		if (image.Pixels.size() < static_cast<size_t>(image.Width) * image.Height * 4)
		{
			if (outError)
				*outError = "Image has less pixel data than its size requires";
			return std::nullopt;
		}

		std::vector<uint8_t> pixels = image.Pixels;
		if (swapRedBlue || forceOpaque)
		{
			for (size_t offset = 0; offset + 3 < pixels.size(); offset += 4)
			{
				if (swapRedBlue)
					std::swap(pixels[offset], pixels[offset + 2]);
				if (forceOpaque)
					pixels[offset + 3] = 255;
			}
		}

		std::vector<uint8_t> encoded;
		auto writeCallback = [](void* context, void* data, int size)
		{
			auto* output = static_cast<std::vector<uint8_t>*>(context);
			const auto* bytes = static_cast<const uint8_t*>(data);
			output->insert(output->end(), bytes, bytes + size);
		};
		const int result = stbi_write_png_to_func(writeCallback, &encoded, static_cast<int>(image.Width), static_cast<int>(image.Height), 4,
			pixels.data(), static_cast<int>(image.Width * 4));
		if (result == 0)
		{
			if (outError)
				*outError = "PNG encoding failed";
			return std::nullopt;
		}
		return encoded;
	}

	std::optional<std::vector<uint8_t>> ImageWriter::EncodeHDR(uint32_t width, uint32_t height, const std::vector<float>& rgb, std::string* outError)
	{
		if (width == 0 || height == 0)
		{
			if (outError)
				*outError = "Image is empty";
			return std::nullopt;
		}
		const size_t values = static_cast<size_t>(width) * height * 3;
		if (rgb.size() < values)
		{
			if (outError)
				*outError = "Image has less pixel data than its size requires";
			return std::nullopt;
		}
		std::vector<float> pixels(rgb.begin(), rgb.begin() + static_cast<std::ptrdiff_t>(values));
		for (float& value : pixels)
			value = std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;

		std::vector<uint8_t> encoded;
		auto writeCallback = [](void* context, void* data, int size)
		{
			auto* output = static_cast<std::vector<uint8_t>*>(context);
			const auto* bytes = static_cast<const uint8_t*>(data);
			output->insert(output->end(), bytes, bytes + size);
		};
		if (stbi_write_hdr_to_func(writeCallback, &encoded, static_cast<int>(width), static_cast<int>(height), 3, pixels.data()) == 0)
		{
			if (outError)
				*outError = "HDR encoding failed";
			return std::nullopt;
		}
		return encoded;
	}

	bool ImageWriter::SavePNG(const ReadbackImage& image, const std::filesystem::path& path, bool forceOpaque, std::string* outError)
	{
		std::optional<std::vector<uint8_t>> encoded = EncodePNG(image, forceOpaque, outError);
		if (!encoded)
			return false;

		if (!FileSystem::WriteBytes(path, *encoded))
		{
			if (outError)
				*outError = fmt::format("Failed to write '{}'", FileSystem::ToUTF8(path));
			return false;
		}
		return true;
	}

}
