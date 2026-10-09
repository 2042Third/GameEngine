#include <doctest/doctest.h>

#include "Strata/Asset/TextureImporter.h"
#include "Strata/Renderer/Texture.h"
#include "TestHelpers.h"

#include <glm/gtc/packing.hpp>
#include <stb_image_write.h>

#include <cmath>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	std::vector<uint8_t> EncodeHDR(uint32_t width, uint32_t height, const std::vector<float>& rgb)
	{
		std::vector<uint8_t> encoded;
		auto write = [](void* context, void* data, int size)
		{
			auto* output = static_cast<std::vector<uint8_t>*>(context);
			output->insert(output->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
		};
		REQUIRE(stbi_write_hdr_to_func(write, &encoded, static_cast<int>(width), static_cast<int>(height), 3, rgb.data()) != 0);
		return encoded;
	}

	// 4x2 image: top row red, bottom row blue (opaque).
	std::vector<uint8_t> CreateTwoRowImage()
	{
		std::vector<uint8_t> pixels;
		for (int index = 0; index < 4; index++)
			Tests::AppendPixel(pixels, 255, 0, 0, 255);
		for (int index = 0; index < 4; index++)
			Tests::AppendPixel(pixels, 0, 0, 255, 255);
		return pixels;
	}

	TextureMip CreateMip(uint32_t width, uint32_t height, uint32_t bytesPerPixel, uint8_t fill = 0)
	{
		TextureMip mip;
		mip.Width = width;
		mip.Height = height;
		mip.Data.assign(static_cast<size_t>(width) * height * bytesPerPixel, fill);
		return mip;
	}

}

TEST_SUITE("Renderer.Texture")
{
	TEST_CASE("Mip counts cover the full chain")
	{
		CHECK(TextureUtils::CalculateMipCount(1, 1) == 1);
		CHECK(TextureUtils::CalculateMipCount(2, 2) == 2);
		CHECK(TextureUtils::CalculateMipCount(256, 128) == 9);
		CHECK(TextureUtils::CalculateMipCount(5, 3) == 3);
		CHECK(TextureUtils::CalculateMipCount(1, 1000) == 10);
	}

	TEST_CASE("Texture creation validates sizes")
	{
		std::string error;
		CHECK(Texture::Create({}, { CreateMip(4, 4, 4) }, &error));

		CHECK_FALSE(Texture::Create({}, {}, &error));
		CHECK_FALSE(error.empty());
		CHECK_FALSE(Texture::Create({}, { CreateMip(0, 4, 4) }, &error));
		CHECK_FALSE(Texture::Create({}, { CreateMip(4, 4, 3) }, &error));
		CHECK_FALSE(Texture::Create({}, { CreateMip(4, 4, 4), CreateMip(3, 2, 4) }, &error));
		CHECK_FALSE(Texture::Create({}, { CreateMip(1, 1, 4), CreateMip(1, 1, 4) }, &error));
		CHECK_FALSE(Texture::Create({}, { CreateMip(20000, 1, 4) }, &error));

		TextureSpecification noFormat;
		noFormat.Format = TextureFormat::None;
		CHECK_FALSE(Texture::Create(noFormat, { CreateMip(1, 1, 4) }, &error));
	}

	TEST_CASE("Mip generation filters in the right color space")
	{
		// Black and white texels average to linear 0.5, which is 188 in sRGB but 128 (rounded) in linear storage.
		std::vector<TextureMip> srgb = { CreateMip(2, 1, 4) };
		srgb[0].Data = { 0, 0, 0, 255, 255, 255, 255, 255 };
		REQUIRE(TextureUtils::GenerateMips(srgb, TextureFormat::RGBA8SRGB, false));
		REQUIRE(srgb.size() == 2);
		CHECK(srgb[1].Width == 1);
		CHECK(srgb[1].Height == 1);
		CHECK(std::abs(static_cast<int>(srgb[1].Data[0]) - 188) <= 1);
		CHECK(srgb[1].Data[3] == 255);

		std::vector<TextureMip> linear = { CreateMip(2, 1, 4) };
		linear[0].Data = { 0, 0, 0, 255, 255, 255, 255, 255 };
		REQUIRE(TextureUtils::GenerateMips(linear, TextureFormat::RGBA8, false));
		CHECK(std::abs(static_cast<int>(linear[1].Data[0]) - 128) <= 1);

		// Normals +X and +Y average to a renormalized 45 degree normal.
		std::vector<TextureMip> normals = { CreateMip(2, 1, 4) };
		normals[0].Data = { 255, 128, 128, 255, 128, 255, 128, 255 };
		REQUIRE(TextureUtils::GenerateMips(normals, TextureFormat::RGBA8, true));
		const glm::vec3 decoded = glm::vec3(normals[1].Data[0], normals[1].Data[1], normals[1].Data[2]) / 255.0f * 2.0f - 1.0f;
		CHECK(glm::length(decoded) == doctest::Approx(1.0f).epsilon(0.02));
		CHECK(decoded.x == doctest::Approx(decoded.y).epsilon(0.02));

		std::vector<TextureMip> hdr = { CreateMip(2, 2, 8) };
		auto* halves = reinterpret_cast<uint16_t*>(hdr[0].Data.data());
		for (int index = 0; index < 16; index++)
			halves[index] = glm::packHalf1x16(index % 4 == 3 ? 1.0f : static_cast<float>(index / 4) * 10.0f);
		REQUIRE(TextureUtils::GenerateMips(hdr, TextureFormat::RGBA16F, false));
		CHECK(glm::unpackHalf1x16(reinterpret_cast<const uint16_t*>(hdr[1].Data.data())[0]) == doctest::Approx(15.0f));

		std::vector<TextureMip> unsupported = { CreateMip(2, 2, 1) };
		CHECK_FALSE(TextureUtils::GenerateMips(unsupported, TextureFormat::R8, false));
	}

	TEST_CASE("Texture serialization round trips and rejects corrupt data")
	{
		TextureSpecification specification;
		specification.Format = TextureFormat::RGBA8SRGB;
		specification.Filter = TextureFilter::Nearest;
		specification.Wrap = TextureWrap::Mirror;
		specification.DebugName = "Checker";
		std::vector<TextureMip> mips = { CreateMip(4, 2, 4, 7) };
		REQUIRE(TextureUtils::GenerateMips(mips, specification.Format, false));
		Ref<Texture> source = Texture::Create(specification, mips);
		REQUIRE(source);

		const std::vector<uint8_t> cooked = source->Serialize();
		std::string error;
		Ref<Texture> loaded = Texture::Deserialize(cooked, &error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetWidth() == 4);
		CHECK(loaded->GetHeight() == 2);
		CHECK(loaded->GetMipCount() == 3);
		CHECK(loaded->GetSpecification().Format == TextureFormat::RGBA8SRGB);
		CHECK(loaded->GetSpecification().Filter == TextureFilter::Nearest);
		CHECK(loaded->GetSpecification().Wrap == TextureWrap::Mirror);
		CHECK(loaded->GetSpecification().DebugName == "Checker");
		for (uint32_t level = 0; level < 3; level++)
			CHECK(loaded->GetMips()[level].Data == source->GetMips()[level].Data);
		CHECK(loaded->GetMemoryUsage() > 0);

		CHECK_FALSE(Texture::Deserialize({}, &error));
		for (size_t length = 0; length < cooked.size(); length += 5)
			CHECK_FALSE(Texture::Deserialize(std::span<const uint8_t>(cooked.data(), length)));
		std::vector<uint8_t> badFormat = cooked;
		badFormat[8] = 0xEE;
		CHECK_FALSE(Texture::Deserialize(badFormat));
	}

	TEST_CASE("Images decode into textures with the requested usage")
	{
		const std::vector<uint8_t> png = Tests::EncodePNG(4, 2, CreateTwoRowImage());
		REQUIRE_FALSE(png.empty());

		TextureImportSettings color;
		std::string error;
		Ref<Texture> texture = TextureImporter::Decode(png, color, "TwoRows.png", &error);
		REQUIRE_MESSAGE(texture, error);
		CHECK(texture->GetWidth() == 4);
		CHECK(texture->GetHeight() == 2);
		CHECK(texture->GetMipCount() == 3);
		CHECK(texture->GetSpecification().Format == TextureFormat::RGBA8SRGB);
		const std::vector<uint8_t>& top = texture->GetMips()[0].Data;
		CHECK(top[0] == 255);
		CHECK(top[2] == 0);
		CHECK(top[4 * 4 + 2] == 255); // First texel of the bottom row is blue

		TextureImportSettings flipped;
		flipped.FlipVertically = true;
		flipped.GenerateMips = false;
		flipped.Usage = TextureUsage::Data;
		Ref<Texture> flippedTexture = TextureImporter::Decode(png, flipped, "TwoRows.png");
		REQUIRE(flippedTexture);
		CHECK(flippedTexture->GetMipCount() == 1);
		CHECK(flippedTexture->GetSpecification().Format == TextureFormat::RGBA8);
		CHECK(flippedTexture->GetMips()[0].Data[2] == 255); // Blue row now on top

		TextureImportSettings limited;
		limited.MaxSize = 2;
		Ref<Texture> small = TextureImporter::Decode(png, limited, "TwoRows.png");
		REQUIRE(small);
		CHECK(small->GetWidth() == 2);
		CHECK(small->GetHeight() == 1);

		CHECK_FALSE(TextureImporter::Decode({}, color, "Empty.png", &error));
		const std::vector<uint8_t> garbage = { 1, 2, 3, 4, 5, 6, 7, 8 };
		CHECK_FALSE(TextureImporter::Decode(garbage, color, "Garbage.png", &error));
		CHECK(error.find("Garbage.png") != std::string::npos);
		std::vector<uint8_t> truncated(png.begin(), png.begin() + static_cast<std::ptrdiff_t>(png.size() / 2));
		CHECK_FALSE(TextureImporter::Decode(truncated, color, "Truncated.png"));
	}

	TEST_CASE("HDR images import as half-float textures")
	{
		const std::vector<float> rgb = { 4.0f, 2.0f, 1.0f, 0.5f, 0.25f, 0.125f };
		const std::vector<uint8_t> hdr = EncodeHDR(2, 1, rgb);

		TextureImportSettings settings;
		settings.Usage = TextureUsage::HDR;
		std::string error;
		Ref<Texture> texture = TextureImporter::Decode(hdr, settings, "Sky.hdr", &error);
		REQUIRE_MESSAGE(texture, error);
		CHECK(texture->GetSpecification().Format == TextureFormat::RGBA16F);
		const auto* halves = reinterpret_cast<const uint16_t*>(texture->GetMips()[0].Data.data());
		// Radiance HDR stores a shared exponent with 8-bit mantissas: about 1% precision.
		CHECK(glm::unpackHalf1x16(halves[0]) == doctest::Approx(4.0f).epsilon(0.02));
		CHECK(glm::unpackHalf1x16(halves[1]) == doctest::Approx(2.0f).epsilon(0.02));
		CHECK(glm::unpackHalf1x16(halves[4]) == doctest::Approx(0.5f).epsilon(0.02));
		CHECK(glm::unpackHalf1x16(halves[3]) == doctest::Approx(1.0f));
	}

	TEST_CASE("Texture import settings round trip and report invalid values")
	{
		TextureImportSettings settings;
		settings.Usage = TextureUsage::NormalMap;
		settings.GenerateMips = false;
		settings.Filter = TextureFilter::Nearest;
		settings.Wrap = TextureWrap::Clamp;
		settings.MaxSize = 512;
		settings.FlipVertically = true;

		std::vector<std::string> warnings;
		const TextureImportSettings loaded = TextureImportSettings::FromJson(settings.ToJson(), &warnings);
		CHECK(warnings.empty());
		CHECK(loaded.Usage == TextureUsage::NormalMap);
		CHECK_FALSE(loaded.GenerateMips);
		CHECK(loaded.Filter == TextureFilter::Nearest);
		CHECK(loaded.Wrap == TextureWrap::Clamp);
		CHECK(loaded.MaxSize == 512);
		CHECK(loaded.FlipVertically);

		const nlohmann::json invalid = { { "Usage", "Sparkly" }, { "Filter", 3 }, { "Wrap", "Tile" }, { "MaxSize", 99999 } };
		const TextureImportSettings fallback = TextureImportSettings::FromJson(invalid, &warnings);
		CHECK(warnings.size() == 4);
		CHECK(fallback.Usage == TextureUsage::Color);
		CHECK(fallback.Filter == TextureFilter::Linear);
		CHECK(fallback.Wrap == TextureWrap::Repeat);
		CHECK(fallback.MaxSize == TextureImporter::c_MaxImageDimension);
	}

	TEST_CASE("Texture usage is guessed from file names")
	{
		CHECK(TextureImportSettings::GetDefaults("Textures/Brick_Albedo.png").Usage == TextureUsage::Color);
		CHECK(TextureImportSettings::GetDefaults("Textures/Brick_Normal.png").Usage == TextureUsage::NormalMap);
		CHECK(TextureImportSettings::GetDefaults("Textures/brick_n.PNG").Usage == TextureUsage::NormalMap);
		CHECK(TextureImportSettings::GetDefaults("Textures/Brick_Roughness.jpg").Usage == TextureUsage::Data);
		CHECK(TextureImportSettings::GetDefaults("Textures/Brick_ORM.png").Usage == TextureUsage::Data);
		CHECK(TextureImportSettings::GetDefaults("Textures/Metal_Plate.png").Usage == TextureUsage::Data);
		const TextureImportSettings sky = TextureImportSettings::GetDefaults("Environment/Sky.hdr");
		CHECK(sky.Usage == TextureUsage::HDR);
		CHECK(sky.Wrap == TextureWrap::Clamp);

		for (TextureUsage usage : { TextureUsage::Color, TextureUsage::NormalMap, TextureUsage::Data, TextureUsage::HDR })
			CHECK(TextureUsageFromString(TextureUsageToString(usage)) == usage);
		CHECK_FALSE(TextureUsageFromString("Unknown").has_value());
	}
}
