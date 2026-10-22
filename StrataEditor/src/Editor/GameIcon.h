#pragma once

#include <Strata/Asset/AssetTypes.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	class EditorAssetManager;

	// The game's icon (ProjectGameInfo::Icon): a texture of the project whose source file the export converts into the
	// images an exported game shows (Docs/DeveloperGuide.md, "Project settings").
	namespace GameIcon
	{

		// The source file of `icon` when it can be the game's icon: a texture of the project (not built-in) with an image
		// file of its own (not a texture inside a model) in a format the conversion decodes (PNG, JPEG, TGA or BMP). Otherwise
		// nullopt, with the reason, which names the asset. Reads only the asset's metadata (no file), so commands call it in
		// the frame.
		std::optional<std::filesystem::path> FindSource(const EditorAssetManager& assets, AssetHandle icon, std::string* outError = nullptr);

		// The sizes of the icon's images: the strata mark's (c_EditorIconSizes: Windows' small and large icons at 100 to 200%
		// display scaling) and 256 (large views; the PNG exported games show as their window icon).
		constexpr std::array<uint32_t, 10> c_Sizes = { 16, 20, 24, 28, 32, 40, 48, 56, 64, 256 };
		constexpr uint32_t c_LargestSize = 256;
		// Larger source files are refused without being read whole: an icon's image is far smaller.
		constexpr uint64_t c_MaxSourceBytes = 16ull << 20;

		// A square image of the icon: RGBA8 with straight alpha, rows top first.
		struct Image
		{
			uint32_t Size = 0;
			std::vector<uint8_t> Pixels;
		};

		struct Images
		{
			std::vector<Image> Sizes; // One per c_Sizes, in that order
			uint32_t SourceWidth = 0;
			uint32_t SourceHeight = 0;
			std::vector<std::string> Warnings; // What a person should know about the result (an enlarged source)

			const Image& GetLargest() const { return Sizes.back(); }
		};

		// Decodes an image (PNG, JPEG, TGA or BMP of at most ImageReader::c_MaxDimension pixels a side), makes it square by
		// centering it on transparent pixels and resizes it to every size of c_Sizes, in linear light with premultiplied alpha
		// (a size equal to the square's is copied as it is). A source under c_LargestSize pixels is enlarged, with a warning.
		// nullopt, with the reason, for data that does not decode. Thread-safe.
		std::optional<Images> Convert(std::span<const uint8_t> encoded, std::string* outError = nullptr);
		// Reads a source file (at most c_MaxSourceBytes) and converts it; the reasons name the file. Thread-safe.
		std::optional<Images> ConvertFile(const std::filesystem::path& source, std::string* outError = nullptr);
		// A PNG of an image (an exported game's <File>.png holds the largest).
		std::optional<std::vector<uint8_t>> EncodePNG(const Image& image, std::string* outError = nullptr);

	}

}
