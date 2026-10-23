#include <doctest/doctest.h>

#include "TestHelpers.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Renderer/ImageReader.h"
#include "Strata/Renderer/ImageWriter.h"

#include <stb_image.h>

#include <limits>
#include <optional>
#include <string>
#include <vector>

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

	TEST_CASE("ImageWriter encodes linear floats as Radiance HDR")
	{
		// Values above 1 survive (RGBE: 8 bits of mantissa per channel, one exponent per pixel), negative and non-finite ones
		// become 0.
		const std::vector<float> rgb = { 0.5f, 2.0f, 1.0f, 1000.0f, -1.0f, std::numeric_limits<float>::quiet_NaN() };
		std::string error;
		const std::optional<std::vector<uint8_t>> hdr = ImageWriter::EncodeHDR(2, 1, rgb, &error);
		REQUIRE_MESSAGE(hdr, error);
		REQUIRE(stbi_is_hdr_from_memory(hdr->data(), static_cast<int>(hdr->size())));
		int width = 0;
		int height = 0;
		int channels = 0;
		float* pixels = stbi_loadf_from_memory(hdr->data(), static_cast<int>(hdr->size()), &width, &height, &channels, 3);
		REQUIRE(pixels != nullptr);
		CHECK(width == 2);
		CHECK(height == 1);
		CHECK(pixels[0] == doctest::Approx(0.5f).epsilon(0.01));
		CHECK(pixels[1] == doctest::Approx(2.0f).epsilon(0.01));
		CHECK(pixels[2] == doctest::Approx(1.0f).epsilon(0.01));
		CHECK(pixels[3] == doctest::Approx(1000.0f).epsilon(0.01));
		CHECK(pixels[4] == 0.0f);
		CHECK(pixels[5] == 0.0f);
		stbi_image_free(pixels);

		CHECK_FALSE(ImageWriter::EncodeHDR(0, 1, rgb, &error));
		CHECK_FALSE(ImageWriter::EncodeHDR(4, 1, rgb, &error));
		CHECK(error.find("less pixel data") != std::string::npos);
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

	TEST_CASE("ImageReader decodes what ImageWriter encodes, and refuses what is no image or too large")
	{
		const ReadbackImage image = CreateTwoPixelImage(nvrhi::Format::RGBA8_UNORM);
		const std::optional<std::vector<uint8_t>> png = ImageWriter::EncodePNG(image, false);
		REQUIRE(png.has_value());
		std::string error;
		const std::optional<ReadbackImage> decoded = ImageReader::DecodeRGBA8(*png, &error);
		REQUIRE_MESSAGE(decoded.has_value(), error);
		CHECK(decoded->Width == 2);
		CHECK(decoded->Height == 1);
		CHECK(decoded->Format == nvrhi::Format::RGBA8_UNORM);
		CHECK(decoded->BytesPerPixel == 4);
		CHECK(decoded->Pixels == image.Pixels);

		CHECK_FALSE(ImageReader::DecodeRGBA8({}, &error).has_value());
		const std::vector<uint8_t> garbage = { 1, 2, 3, 4, 5, 6, 7, 8 };
		error.clear();
		CHECK_FALSE(ImageReader::DecodeRGBA8(garbage, &error).has_value());
		CHECK_FALSE(error.empty());
		// A truncated PNG.
		CHECK_FALSE(ImageReader::DecodeRGBA8(std::span<const uint8_t>(png->data(), png->size() / 2)).has_value());
		// Larger than any image the engine writes: refused before it is decoded.
		const std::vector<uint8_t> huge = Tests::CreateSolidPNG(ImageReader::c_MaxDimension + 1, 1, 1, 2, 3);
		error.clear();
		CHECK_FALSE(ImageReader::DecodeRGBA8(huge, &error).has_value());
		CHECK(error.find("at most") != std::string::npos);
	}
}
