#include <doctest/doctest.h>

#include "Strata/Asset/TextureImporter.h"
#include "Strata/Renderer/CookedTexture.h"
#include "Strata/Renderer/Texture.h"
#include "TestHelpers.h"

#include <glm/gtc/packing.hpp>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
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

		// Block-compressed levels hold whole 4x4 blocks: a 5x5 BC1 level is 2x2 blocks of 8 bytes, its 2x2 mip one block.
		TextureSpecification bc1;
		bc1.Format = TextureFormat::BC1;
		CHECK(Texture::Create(bc1, { CreateMip(5, 5, 0), CreateMip(2, 2, 0) }, &error) == nullptr);
		TextureMip blocks5x5 = CreateMip(5, 5, 0);
		blocks5x5.Data.resize(32);
		TextureMip block2x2 = CreateMip(2, 2, 0);
		block2x2.Data.resize(8);
		TextureMip block1x1 = CreateMip(1, 1, 0);
		block1x1.Data.resize(8);
		const Ref<Texture> compressed = Texture::Create(bc1, { blocks5x5, block2x2, block1x1 }, &error);
		REQUIRE_MESSAGE(compressed, error);
		CHECK(compressed->GetMemoryUsage().Cpu == 48);
		blocks5x5.Data.resize(25 * 4);
		CHECK_FALSE(Texture::Create(bc1, { blocks5x5 }, &error));

		// Only one-channel formats can stand for gray.
		TextureSpecification grayscale;
		grayscale.Format = TextureFormat::R8;
		grayscale.Grayscale = true;
		CHECK(Texture::Create(grayscale, { CreateMip(2, 2, 1) }, &error));
		grayscale.Format = TextureFormat::RGBA8;
		CHECK_FALSE(Texture::Create(grayscale, { CreateMip(2, 2, 4) }, &error));
	}

	TEST_CASE("Block-compressed textures decompress for devices without block compression")
	{
		// One BC1 block (red, blue and their thirds per row) and a BC4 block of one value.
		TextureSpecification specification;
		specification.Format = TextureFormat::BC1SRGB;
		specification.Filter = TextureFilter::Nearest;
		specification.DebugName = "Compressed";
		TextureMip level0;
		level0.Width = 4;
		level0.Height = 2;
		level0.Data = { 0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4 };
		TextureMip level1;
		level1.Width = 2;
		level1.Height = 1;
		level1.Data = { 0x00, 0xF8, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00 };
		TextureMip level2 = level1;
		level2.Width = 1;
		const Ref<Texture> texture = Texture::Create(specification, { level0, level1, level2 });
		REQUIRE(texture);

		std::string error;
		const Ref<Texture> decoded = texture->Decompress(&error);
		REQUIRE_MESSAGE(decoded, error);
		CHECK(decoded->GetSpecification().Format == TextureFormat::RGBA8SRGB);
		CHECK(decoded->GetSpecification().Filter == TextureFilter::Nearest);
		CHECK(decoded->GetSpecification().DebugName == "Compressed");
		CHECK(decoded->GetMipCount() == 3);
		// The decoded size is what the texture holds now.
		CHECK(decoded->GetMemoryUsage().Cpu == (4 * 2 + 2 * 1 + 1) * 4);
		const std::span<const uint8_t> pixels = decoded->GetMipData(0);
		CHECK(std::vector<uint8_t>(pixels.begin(), pixels.begin() + 16) == std::vector<uint8_t> { 255, 0, 0, 255, 0, 0, 255, 255, 170, 0, 85, 255, 85, 0, 170, 255 });
		CHECK(decoded->GetMipData(2)[0] == 255);

		// Decoded at load time, a texture keeps the layout of the cooked data it came from: streaming reads the
		// block-compressed levels there again by byte range, not levels of the decoded size.
		const std::vector<uint8_t> cooked = texture->Serialize();
		const std::optional<CookedTextureView> view = ParseCookedTexture(cooked, &error);
		REQUIRE_MESSAGE(view, error);
		CHECK(view->Layout.Format == TextureFormat::BC1SRGB);
		const Ref<Texture> loaded = Texture::Deserialize(std::vector<uint8_t>(cooked), &error);
		REQUIRE_MESSAGE(loaded, error);
		const Ref<Texture> loadedDecoded = loaded->Decompress(&error);
		REQUIRE_MESSAGE(loadedDecoded, error);
		CHECK(loadedDecoded->GetSpecification().Format == TextureFormat::RGBA8SRGB);
		const CookedTextureLayout& layout = loadedDecoded->GetCookedLayout();
		CHECK(layout.Format == TextureFormat::BC1SRGB);
		CHECK(layout.HeaderSize == view->Layout.HeaderSize);
		REQUIRE(layout.MipCount == view->Layout.MipCount);
		for (uint32_t level = 0; level < layout.MipCount; level++)
		{
			CAPTURE(level);
			CHECK(layout.Levels[level].Offset == view->Layout.Levels[level].Offset);
			CHECK(layout.Levels[level].Size == view->Layout.Levels[level].Size);
		}
		const CookedTextureByteRange tail = loadedDecoded->GetMipTailByteRange();
		const CookedTextureByteRange cookedTail = view->Layout.GetMipTailByteRange();
		CHECK(tail.Offset == cookedTail.Offset);
		CHECK(tail.Size == cookedTail.Size);
		CHECK(tail.FirstLevel == cookedTail.FirstLevel);
		CHECK(tail.LevelCount == cookedTail.LevelCount);
		CHECK(cookedTail.Offset + cookedTail.Size <= cooked.size());

		// BC4 decodes to one channel and stays grayscale.
		TextureSpecification gray;
		gray.Format = TextureFormat::BC4;
		gray.Grayscale = true;
		TextureMip grayLevel;
		grayLevel.Width = 1;
		grayLevel.Height = 1;
		grayLevel.Data = { 77, 77, 0, 0, 0, 0, 0, 0 };
		const Ref<Texture> grayDecoded = Texture::Create(gray, { grayLevel })->Decompress(&error);
		REQUIRE_MESSAGE(grayDecoded, error);
		CHECK(grayDecoded->GetSpecification().Format == TextureFormat::R8);
		CHECK(grayDecoded->GetSpecification().Grayscale);
		CHECK(grayDecoded->GetMipData(0)[0] == 77);

		// Only block-compressed textures decompress.
		CHECK_FALSE(decoded->Decompress(&error));
		CHECK(error.find("not block-compressed") != std::string::npos);
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
		// Without a renderer the mip chain stays on the CPU: every byte of it counts there.
		CHECK(loaded->GetMemoryUsage().Cpu == 4 * 2 * 4 + 2 * 1 * 4 + 1 * 1 * 4);
		CHECK(loaded->GetMemoryUsage().GpuTextures == 0);

		CHECK_FALSE(Texture::Deserialize(std::span<const uint8_t>(), &error));
		for (size_t length = 0; length < cooked.size(); length += 5)
		{
			CHECK_FALSE(Texture::Deserialize(std::span<const uint8_t>(cooked.data(), length)));
			CHECK_FALSE(Texture::Deserialize(std::vector<uint8_t>(cooked.begin(), cooked.begin() + static_cast<std::ptrdiff_t>(length))));
		}
		std::vector<uint8_t> badFormat = cooked;
		badFormat[8] = 0xEE;
		CHECK_FALSE(Texture::Deserialize(badFormat));
		CHECK_FALSE(Texture::Deserialize(std::move(badFormat)));

		// Taking the cooked bytes over gives the same texture, its pixels read in place.
		Ref<Texture> takenOver = Texture::Deserialize(std::vector<uint8_t>(cooked), &error);
		REQUIRE_MESSAGE(takenOver, error);
		CHECK(takenOver->GetSpecification().DebugName == "Checker");
		REQUIRE(takenOver->GetMips().size() == 3);
		for (uint32_t level = 0; level < 3; level++)
		{
			CAPTURE(level);
			CHECK(takenOver->GetMips()[level].Width == source->GetMips()[level].Width);
			CHECK(takenOver->GetMips()[level].Data.empty());
			const std::span<const uint8_t> pixels = takenOver->GetMipData(level);
			CHECK(std::vector<uint8_t>(pixels.begin(), pixels.end()) == source->GetMips()[level].Data);
			CHECK(std::ranges::equal(loaded->GetMipData(level), source->GetMips()[level].Data));
		}
		CHECK(takenOver->GetMipData(3).empty());
		CHECK(takenOver->GetMemoryUsage().Cpu == cooked.size());
		CHECK(takenOver->Serialize() == cooked);
		// Sizes that do not match the levels' dimensions are rejected like in copied data.
		std::vector<uint8_t> wrongSize = cooked;
		const size_t firstMipSizeOffset = 40 + 16; // The header, then level 0's width, height and offset
		REQUIRE(wrongSize.size() > firstMipSizeOffset + 8);
		wrongSize[firstMipSizeOffset] = static_cast<uint8_t>(wrongSize[firstMipSizeOffset] - 4);
		CHECK_FALSE(Texture::Deserialize(std::span<const uint8_t>(wrongSize)));
		CHECK_FALSE(Texture::Deserialize(std::move(wrongSize)));
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
		CHECK(texture->GetSpecification().Format == TextureFormat::BC7SRGB); // Color
		Ref<Texture> decoded = texture->Decompress(&error);
		REQUIRE_MESSAGE(decoded, error);
		const std::span<const uint8_t> top = decoded->GetMipData(0);
		CHECK(top[0] == 255);
		CHECK(top[2] == 0);
		CHECK(top[4 * 4 + 2] == 255); // First texel of the bottom row is blue

		TextureImportSettings uncompressed;
		uncompressed.Compression = TextureCompression::Uncompressed;
		Ref<Texture> exact = TextureImporter::Decode(png, uncompressed, "TwoRows.png", &error);
		REQUIRE_MESSAGE(exact, error);
		CHECK(exact->GetSpecification().Format == TextureFormat::RGBA8SRGB);
		CHECK(exact->GetMips()[0].Data == CreateTwoRowImage());

		TextureImportSettings flipped;
		flipped.FlipVertically = true;
		flipped.GenerateMips = false;
		flipped.Usage = TextureUsage::Data;
		flipped.Compression = TextureCompression::Uncompressed;
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
		settings.Compression = TextureCompression::Uncompressed;
		settings.Quality = TextureQuality::Fast;
		settings.GenerateMips = false;
		settings.Filter = TextureFilter::Nearest;
		settings.Wrap = TextureWrap::Clamp;
		settings.MaxSize = 512;
		settings.FlipVertically = true;

		std::vector<std::string> warnings;
		const TextureImportSettings loaded = TextureImportSettings::FromJson(settings.ToJson(), &warnings);
		CHECK(warnings.empty());
		CHECK(loaded.Usage == TextureUsage::NormalMap);
		CHECK(loaded.Compression == TextureCompression::Uncompressed);
		CHECK(loaded.Quality == TextureQuality::Fast);
		CHECK_FALSE(loaded.GenerateMips);
		CHECK(loaded.Filter == TextureFilter::Nearest);
		CHECK(loaded.Wrap == TextureWrap::Clamp);
		CHECK(loaded.MaxSize == 512);
		CHECK(loaded.FlipVertically);

		// Settings written before compression existed keep the defaults: block compression at high quality.
		const TextureImportSettings older = TextureImportSettings::FromJson({ { "Usage", "Data" }, { "GenerateMips", true } }, &warnings);
		CHECK(warnings.empty());
		CHECK(older.Compression == TextureCompression::Auto);
		CHECK(older.Quality == TextureQuality::High);

		const nlohmann::json invalid = { { "Usage", "Sparkly" }, { "Filter", 3 }, { "Wrap", "Tile" }, { "MaxSize", 99999 }, { "Compression", "Zip" },
			{ "Quality", 2 } };
		const TextureImportSettings fallback = TextureImportSettings::FromJson(invalid, &warnings);
		CHECK(warnings.size() == 6);
		CHECK(fallback.Usage == TextureUsage::Color);
		CHECK(fallback.Compression == TextureCompression::Auto);
		CHECK(fallback.Quality == TextureQuality::High);
		CHECK(fallback.Filter == TextureFilter::Linear);
		CHECK(fallback.Wrap == TextureWrap::Repeat);
		CHECK(fallback.MaxSize == TextureImporter::c_MaxImageDimension);

		for (TextureCompression compression : { TextureCompression::Auto, TextureCompression::Uncompressed })
			CHECK(TextureCompressionFromString(TextureCompressionToString(compression)) == compression);
		for (TextureQuality quality : { TextureQuality::High, TextureQuality::Fast })
			CHECK(TextureQualityFromString(TextureQualityToString(quality)) == quality);
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
