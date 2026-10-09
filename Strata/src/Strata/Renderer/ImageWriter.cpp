#include "stpch.h"
#include "Strata/Renderer/ImageWriter.h"

#include "Strata/Core/FileSystem.h"

#include <stb_image_write.h>

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
