#include "Editor/GameIcon.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Renderer/ImageReader.h>
#include <Strata/Renderer/ImageWriter.h>

#include <spdlog/fmt/fmt.h>
#include <stb_image_resize2.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <utility>

namespace Strata
{

	namespace
	{

		// The formats the icon is made from: of what the texture importer reads, PSD, GIF and HDR files make no icon (layers,
		// animation frames, HDR pixels).
		constexpr std::array<std::string_view, 5> c_IconExtensions = { ".png", ".jpg", ".jpeg", ".tga", ".bmp" };

		void SetError(std::string* outError, std::string message)
		{
			if (outError)
				*outError = std::move(message);
		}

		// The image centered on transparent pixels in a square as large as its longer side.
		GameIcon::Image MakeSquare(const ReadbackImage& image)
		{
			GameIcon::Image square;
			square.Size = std::max(image.Width, image.Height);
			square.Pixels.assign(static_cast<size_t>(square.Size) * square.Size * 4, 0);
			const uint32_t left = (square.Size - image.Width) / 2;
			const uint32_t top = (square.Size - image.Height) / 2;
			const size_t rowBytes = static_cast<size_t>(image.Width) * 4;
			for (uint32_t row = 0; row < image.Height; row++)
			{
				std::memcpy(square.Pixels.data() + (static_cast<size_t>(top + row) * square.Size + left) * 4, image.Pixels.data() + row * rowBytes,
					rowBytes);
			}
			return square;
		}

		// The square at `size` pixels: filtered in linear light, with the colors weighted by alpha (STBIR_RGBA), so that the
		// transparent border neither darkens nor tints the edges.
		bool Resize(const GameIcon::Image& square, uint32_t size, GameIcon::Image& outImage)
		{
			outImage.Size = size;
			if (size == square.Size)
			{
				outImage.Pixels = square.Pixels;
				return true;
			}
			outImage.Pixels.resize(static_cast<size_t>(size) * size * 4);
			return stbir_resize_uint8_srgb(square.Pixels.data(), static_cast<int>(square.Size), static_cast<int>(square.Size), 0, outImage.Pixels.data(),
				static_cast<int>(size), static_cast<int>(size), 0, STBIR_RGBA) != nullptr;
		}

	}

	std::optional<std::filesystem::path> GameIcon::FindSource(const EditorAssetManager& assets, AssetHandle icon, std::string* outError)
	{
		const std::optional<AssetMetadata> metadata = assets.GetMetadata(icon);
		if (!metadata || metadata->IsBuiltin())
		{
			SetError(outError, fmt::format("{} is no asset of the project: the game's icon is a texture of the project", icon.ToString()));
			return std::nullopt;
		}
		if (metadata->Type != AssetType::Texture)
		{
			SetError(outError, fmt::format("'{}' is a {}, not a texture", metadata->Path, AssetTypeToString(metadata->Type)));
			return std::nullopt;
		}
		if (metadata->IsSubAsset())
		{
			SetError(outError, fmt::format("'{}' ({}) is a texture inside '{}', which has no image file of its own: import the image as a texture of "
				"its own (a PNG, JPEG, TGA or BMP file in the asset directory) and use that", metadata->Name, icon.ToString(), metadata->Path));
			return std::nullopt;
		}
		const std::string extension = StringUtils::ToLower(FileSystem::ToUTF8(FileSystem::FromUTF8(metadata->Path).extension()));
		if (std::find(c_IconExtensions.begin(), c_IconExtensions.end(), extension) == c_IconExtensions.end())
		{
			SetError(outError, fmt::format("'{}' is not a PNG, JPEG, TGA or BMP image, which is what the game's icon is made from: convert it to PNG "
				"(at least 256 x 256 pixels)", metadata->Path));
			return std::nullopt;
		}
		std::filesystem::path source = assets.GetAbsolutePath(icon);
		if (source.empty())
		{
			SetError(outError, fmt::format("'{}' has no source file in the project", metadata->Path));
			return std::nullopt;
		}
		return source;
	}

	std::optional<GameIcon::Images> GameIcon::Convert(std::span<const uint8_t> encoded, std::string* outError)
	{
		std::string error;
		const std::optional<ReadbackImage> decoded = ImageReader::DecodeRGBA8(encoded, &error);
		if (!decoded)
		{
			SetError(outError, fmt::format("it is no image the icon can be made from ({})", error));
			return std::nullopt;
		}
		Images images;
		images.SourceWidth = decoded->Width;
		images.SourceHeight = decoded->Height;
		const Image square = MakeSquare(*decoded);
		for (const uint32_t size : c_Sizes)
		{
			Image& image = images.Sizes.emplace_back();
			if (!Resize(square, size, image))
			{
				SetError(outError, fmt::format("it could not be resized to {} x {} pixels", size, size));
				return std::nullopt;
			}
		}
		if (square.Size < c_LargestSize)
		{
			images.Warnings.push_back(fmt::format("The game's icon is {} x {} pixels, so its {} x {} image is enlarged and may look blurred: use an image "
				"of at least {} x {} pixels", decoded->Width, decoded->Height, c_LargestSize, c_LargestSize, c_LargestSize, c_LargestSize));
		}
		return images;
	}

	std::optional<GameIcon::Images> GameIcon::ConvertFile(const std::filesystem::path& source, std::string* outError)
	{
		const std::string name = FileSystem::ToUTF8(source);
		const std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytesPrefix(source, c_MaxSourceBytes + 1);
		if (!bytes)
		{
			SetError(outError, fmt::format("The game's icon '{}' cannot be read", name));
			return std::nullopt;
		}
		if (bytes->size() > c_MaxSourceBytes)
		{
			SetError(outError, fmt::format("The game's icon '{}' is larger than the {} MiB an icon's image may take", name, c_MaxSourceBytes >> 20));
			return std::nullopt;
		}
		std::string error;
		std::optional<Images> images = Convert(*bytes, &error);
		if (!images)
			SetError(outError, fmt::format("The game's icon '{}' cannot be used: {}", name, error));
		return images;
	}

	std::optional<std::vector<uint8_t>> GameIcon::EncodePNG(const Image& image, std::string* outError)
	{
		ReadbackImage readback;
		readback.Width = image.Size;
		readback.Height = image.Size;
		readback.Format = nvrhi::Format::RGBA8_UNORM;
		readback.BytesPerPixel = 4;
		readback.Pixels = image.Pixels;
		return ImageWriter::EncodePNG(readback, false, outError);
	}

}
