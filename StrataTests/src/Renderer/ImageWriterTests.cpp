#include <doctest/doctest.h>

#include "TestHelpers.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Renderer/ImageWriter.h"

#include <stb_image.h>

using namespace Strata;

namespace
{

	ReadbackImage CreateTwoPixelImage(nvrhi::Format format)
	{
		ReadbackImage image;
		image.Width = 2;
		image.Height = 1;
		image.Format = format;
		image.BytesPerPixel = 4;
		image.Pixels = { 10, 20, 30, 40, 50, 60, 70, 80 };
		return image;
	}

	std::vector<uint8_t> DecodeRGBA(const std::vector<uint8_t>& png, int& width, int& height)
	{
		int channels = 0;
		stbi_uc* pixels = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &width, &height, &channels, 4);
		REQUIRE(pixels != nullptr);
		std::vector<uint8_t> result(pixels, pixels + static_cast<size_t>(width) * height * 4);
		stbi_image_free(pixels);
		return result;
	}

}

TEST_SUITE("Renderer")
{
	TEST_CASE("ImageWriter encodes RGBA images as PNG")
	{
		const ReadbackImage image = CreateTwoPixelImage(nvrhi::Format::RGBA8_UNORM);

		std::optional<std::vector<uint8_t>> opaque = ImageWriter::EncodePNG(image, true);
		REQUIRE(opaque.has_value());
		int width = 0;
		int height = 0;
		CHECK(DecodeRGBA(*opaque, width, height) == std::vector<uint8_t> { 10, 20, 30, 255, 50, 60, 70, 255 });
		CHECK(width == 2);
		CHECK(height == 1);

		std::optional<std::vector<uint8_t>> withAlpha = ImageWriter::EncodePNG(image, false);
		REQUIRE(withAlpha.has_value());
		CHECK(DecodeRGBA(*withAlpha, width, height) == image.Pixels);
	}

	TEST_CASE("ImageWriter swizzles BGRA back buffers")
	{
		for (nvrhi::Format format : { nvrhi::Format::BGRA8_UNORM, nvrhi::Format::SBGRA8_UNORM })
		{
			std::optional<std::vector<uint8_t>> png = ImageWriter::EncodePNG(CreateTwoPixelImage(format), false);
			REQUIRE(png.has_value());
			int width = 0;
			int height = 0;
			CHECK(DecodeRGBA(*png, width, height) == std::vector<uint8_t> { 30, 20, 10, 40, 70, 60, 50, 80 });
		}
	}

	TEST_CASE("ImageWriter rejects images it cannot encode")
	{
		std::string error;
		CHECK_FALSE(ImageWriter::EncodePNG(CreateTwoPixelImage(nvrhi::Format::RGBA16_FLOAT), true, &error).has_value());
		CHECK_FALSE(error.empty());

		error.clear();
		ReadbackImage truncated = CreateTwoPixelImage(nvrhi::Format::RGBA8_UNORM);
		truncated.Pixels.resize(4);
		CHECK_FALSE(ImageWriter::EncodePNG(truncated, true, &error).has_value());
		CHECK_FALSE(error.empty());

		error.clear();
		CHECK_FALSE(ImageWriter::EncodePNG(ReadbackImage(), true, &error).has_value());
		CHECK_FALSE(error.empty());
	}

	TEST_CASE("ImageWriter saves PNG files")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ImageWriter");
		const std::filesystem::path path = directory / "Nested" / "Screenshot.png";
		REQUIRE(ImageWriter::SavePNG(CreateTwoPixelImage(nvrhi::Format::RGBA8_UNORM), path));

		std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(path);
		REQUIRE(bytes.has_value());
		int width = 0;
		int height = 0;
		CHECK(DecodeRGBA(*bytes, width, height) == std::vector<uint8_t> { 10, 20, 30, 255, 50, 60, 70, 255 });
	}
}
