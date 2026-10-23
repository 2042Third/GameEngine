#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Asset/SyntheticSky.h"
#include "Strata/Asset/HDRSun.h"
#include "Strata/Asset/TextureCooker.h"
#include "Strata/Asset/TextureImporter.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Renderer/BlockCompression.h"
#include "TestHelpers.h"

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#include <nlohmann/json.hpp>
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Strata;

namespace
{

	Ref<Texture> Import(const std::vector<uint8_t>& encoded, const TextureImportSettings& settings, const std::string& name = "Image.png")
	{
		std::string error;
		Ref<Texture> texture = TextureImporter::Decode(encoded, settings, name, &error);
		REQUIRE_MESSAGE(texture, error);
		return texture;
	}

	// The level's texels in the format the texture decodes to (the texture itself when it is not block-compressed).
	std::vector<uint8_t> DecodeLevel(const Texture& texture, uint32_t level)
	{
		if (!IsBlockCompressedFormat(texture.GetSpecification().Format))
		{
			const std::span<const uint8_t> pixels = texture.GetMipData(level);
			return std::vector<uint8_t>(pixels.begin(), pixels.end());
		}
		std::string error;
		const Ref<Texture> decoded = texture.Decompress(&error);
		REQUIRE_MESSAGE(decoded, error);
		const std::span<const uint8_t> pixels = decoded->GetMipData(level);
		return std::vector<uint8_t>(pixels.begin(), pixels.end());
	}

	// Peak signal-to-noise ratio of the first three channels of two images with `channels` per texel.
	double ComputePSNR(const std::vector<uint8_t>& reference, const std::vector<uint8_t>& image, uint32_t channels)
	{
		REQUIRE(reference.size() == image.size());
		double squaredError = 0.0;
		size_t samples = 0;
		for (size_t index = 0; index < reference.size(); index++)
		{
			if (index % channels >= 3)
				continue;
			const double difference = static_cast<double>(reference[index]) - static_cast<double>(image[index]);
			squaredError += difference * difference;
			samples++;
		}
		const double meanSquaredError = squaredError / static_cast<double>(samples);
		return meanSquaredError == 0.0 ? 100.0 : 10.0 * std::log10(255.0 * 255.0 / meanSquaredError);
	}

	std::vector<uint8_t> CreateGradient(uint32_t size)
	{
		std::vector<uint8_t> pixels;
		for (uint32_t y = 0; y < size; y++)
		{
			for (uint32_t x = 0; x < size; x++)
				Tests::AppendPixel(pixels, static_cast<uint8_t>(x * 255 / (size - 1)), static_cast<uint8_t>(y * 255 / (size - 1)),
					static_cast<uint8_t>((x + y) * 255 / (2 * (size - 1))), 255);
		}
		return pixels;
	}

	TextureCooker::Image CreateImage(uint32_t width, uint32_t height, uint32_t channels, const std::vector<uint8_t>& pixels)
	{
		TextureCooker::Image image;
		image.Width = width;
		image.Height = height;
		image.Channels = channels;
		image.Pixels = pixels;
		return image;
	}

	glm::vec3 DecodeNormal(uint8_t x, uint8_t y, uint8_t z)
	{
		return glm::vec3(x, y, z) / 255.0f * 2.0f - 1.0f;
	}

	// A normal map of a hemisphere bulging out of a flat surface.
	std::vector<uint8_t> CreateSphereNormalMap(uint32_t size)
	{
		std::vector<uint8_t> pixels;
		for (uint32_t y = 0; y < size; y++)
		{
			for (uint32_t x = 0; x < size; x++)
			{
				const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
				const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
				const float radiusSquared = (u * u + v * v) / 0.81f;
				const glm::vec3 normal = radiusSquared < 1.0f ? glm::normalize(glm::vec3(u / 0.9f, -v / 0.9f, std::sqrt(1.0f - radiusSquared))) : glm::vec3(0.0f, 0.0f, 1.0f);
				for (int channel = 0; channel < 3; channel++)
					pixels.push_back(static_cast<uint8_t>(std::lround((normal[channel] * 0.5f + 0.5f) * 255.0f)));
			}
		}
		return pixels;
	}

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

}

TEST_SUITE("Asset.TextureImport")
{
	TEST_CASE("The stored format follows usage, channels and alpha")
	{
		TextureImportSettings settings;
		auto choose = [&](TextureUsage usage, TextureCompression compression, uint32_t channels, bool hasAlpha, TextureQuality quality = TextureQuality::High)
		{
			settings.Usage = usage;
			settings.Compression = compression;
			settings.Quality = quality;
			return TextureImporter::ChooseFormat(settings, channels, hasAlpha);
		};
		CHECK(choose(TextureUsage::Color, TextureCompression::Auto, 3, false) == TextureFormat::BC7SRGB);
		CHECK(choose(TextureUsage::Color, TextureCompression::Auto, 4, true) == TextureFormat::BC7SRGB);
		// Fast stores opaque color in BC1, half of BC7's bytes.
		CHECK(choose(TextureUsage::Color, TextureCompression::Auto, 3, false, TextureQuality::Fast) == TextureFormat::BC1SRGB);
		CHECK(choose(TextureUsage::Color, TextureCompression::Auto, 4, false, TextureQuality::Fast) == TextureFormat::BC1SRGB);
		CHECK(choose(TextureUsage::Color, TextureCompression::Auto, 4, true, TextureQuality::Fast) == TextureFormat::BC7SRGB);
		CHECK(choose(TextureUsage::Color, TextureCompression::Uncompressed, 4, true) == TextureFormat::RGBA8SRGB);
		CHECK(choose(TextureUsage::Color, TextureCompression::Uncompressed, 3, false, TextureQuality::Fast) == TextureFormat::RGBA8SRGB);
		CHECK(choose(TextureUsage::NormalMap, TextureCompression::Auto, 3, false) == TextureFormat::BC5);
		CHECK(choose(TextureUsage::NormalMap, TextureCompression::Auto, 3, false, TextureQuality::Fast) == TextureFormat::BC5);
		CHECK(choose(TextureUsage::NormalMap, TextureCompression::Uncompressed, 3, false) == TextureFormat::RGBA8);
		CHECK(choose(TextureUsage::Data, TextureCompression::Auto, 1, false) == TextureFormat::BC4);
		CHECK(choose(TextureUsage::Data, TextureCompression::Uncompressed, 1, false) == TextureFormat::R8);
		// Data with several channels is BC7, which keeps them apart (see "Data channels stay apart").
		CHECK(choose(TextureUsage::Data, TextureCompression::Auto, 3, false) == TextureFormat::BC7);
		CHECK(choose(TextureUsage::Data, TextureCompression::Auto, 4, true) == TextureFormat::BC7);
		CHECK(choose(TextureUsage::Data, TextureCompression::Auto, 3, false, TextureQuality::Fast) == TextureFormat::BC7);
		CHECK(choose(TextureUsage::Data, TextureCompression::Uncompressed, 3, false) == TextureFormat::RGBA8);
		CHECK(choose(TextureUsage::HDR, TextureCompression::Auto, 4, false) == TextureFormat::RGBA16F);
		CHECK(choose(TextureUsage::HDR, TextureCompression::Uncompressed, 4, false) == TextureFormat::RGBA16F);

		// Images decode with the channels the format needs.
		const std::vector<uint8_t> rgb = Tests::EncodePNG(8, 8, 3, std::vector<uint8_t>(8 * 8 * 3, 90));
		const std::vector<uint8_t> gray = Tests::EncodePNG(8, 8, 1, std::vector<uint8_t>(8 * 8, 200));
		std::vector<uint8_t> grayAlphaPixels(8 * 8 * 2, 60);
		grayAlphaPixels[1] = 10;
		const std::vector<uint8_t> grayAlpha = Tests::EncodePNG(8, 8, 2, grayAlphaPixels);
		std::vector<uint8_t> translucentPixels(8 * 8 * 4, 255);
		translucentPixels[3] = 128;
		const std::vector<uint8_t> translucent = Tests::EncodePNG(8, 8, translucentPixels);
		const std::vector<uint8_t> opaque = Tests::CreateSolidPNG(8, 8, 10, 20, 30, 255);

		TextureImportSettings color;
		CHECK(Import(rgb, color)->GetSpecification().Format == TextureFormat::BC7SRGB);
		CHECK(Import(opaque, color)->GetSpecification().Format == TextureFormat::BC7SRGB);
		CHECK(Import(translucent, color)->GetSpecification().Format == TextureFormat::BC7SRGB);
		// Gray color stays color (sRGB): it is stored as RGB (flat, so exactly).
		const Ref<Texture> grayColor = Import(gray, color);
		CHECK(grayColor->GetSpecification().Format == TextureFormat::BC7SRGB);
		CHECK_FALSE(grayColor->GetSpecification().Grayscale);
		CHECK(DecodeLevel(*grayColor, 0)[1] == 200);
		TextureImportSettings fastColor;
		fastColor.Quality = TextureQuality::Fast;
		CHECK(Import(opaque, fastColor)->GetSpecification().Format == TextureFormat::BC1SRGB);
		CHECK(Import(translucent, fastColor)->GetSpecification().Format == TextureFormat::BC7SRGB);

		TextureImportSettings data;
		data.Usage = TextureUsage::Data;
		const Ref<Texture> grayData = Import(gray, data);
		CHECK(grayData->GetSpecification().Format == TextureFormat::BC4);
		CHECK(grayData->GetSpecification().Grayscale);
		CHECK(DecodeLevel(*grayData, 0)[0] == 200);
		CHECK(Import(grayAlpha, data)->GetSpecification().Format == TextureFormat::BC7);
		CHECK(Import(rgb, data)->GetSpecification().Format == TextureFormat::BC7);

		// Uncompressed one-channel data keeps one byte per texel.
		data.Compression = TextureCompression::Uncompressed;
		data.GenerateMips = false;
		const Ref<Texture> grayBytes = Import(gray, data);
		CHECK(grayBytes->GetSpecification().Format == TextureFormat::R8);
		CHECK(grayBytes->GetSpecification().Grayscale);
		CHECK(grayBytes->GetMemoryUsage().Cpu == 8 * 8);

		TextureImportSettings normalMap;
		normalMap.Usage = TextureUsage::NormalMap;
		CHECK(Import(rgb, normalMap)->GetSpecification().Format == TextureFormat::BC5);

		TextureImportSettings hdr;
		hdr.Usage = TextureUsage::HDR;
		const Ref<Texture> environment = Import(EncodeHDR(4, 2, std::vector<float>(4 * 2 * 3, 2.0f)), hdr, "Sky.hdr");
		CHECK(environment->GetSpecification().Format == TextureFormat::RGBA16F);
		CHECK(environment->GetMipCount() == 3);
		CHECK(glm::unpackHalf1x16(reinterpret_cast<const uint16_t*>(environment->GetMipData(2).data())[0]) == doctest::Approx(2.0f).epsilon(0.02));
	}

	TEST_CASE("Mips are filtered in the texture's color space, with its edges and alpha")
	{
		// A checkerboard of single black and white texels averages to linear 0.5 in every texel of level 1: 188 in sRGB.
		std::vector<uint8_t> checker;
		for (uint32_t y = 0; y < 64; y++)
		{
			for (uint32_t x = 0; x < 64; x++)
			{
				const uint8_t value = (x + y) % 2 == 0 ? 0 : 255;
				Tests::AppendPixel(checker, value, value, value, 255);
			}
		}
		TextureCooker::CookSettings settings;
		settings.Format = TextureFormat::RGBA8SRGB;
		settings.SRGB = true;
		std::vector<TextureMip> mips;
		std::string error;
		REQUIRE_MESSAGE(TextureCooker::CookLevels(CreateImage(64, 64, 4, checker), settings, mips, &error), error);
		REQUIRE(mips.size() == 7);
		CHECK(mips[0].Data == checker);
		for (size_t index = 0; index < mips[1].Data.size(); index += 4)
			REQUIRE(std::abs(static_cast<int>(mips[1].Data[index]) - 188) <= 2);
		// Linear data averages to 128.
		settings.Format = TextureFormat::RGBA8;
		settings.SRGB = false;
		REQUIRE(TextureCooker::CookLevels(CreateImage(64, 64, 4, checker), settings, mips, &error));
		for (size_t index = 0; index < mips[1].Data.size(); index += 4)
			REQUIRE(std::abs(static_cast<int>(mips[1].Data[index]) - 128) <= 2);

		// A repeating texture's right edge filters in its left column; a clamped one's does not.
		std::vector<uint8_t> leftColumn(8 * 8, 0);
		for (uint32_t y = 0; y < 8; y++)
			leftColumn[y * 8] = 255;
		settings.Format = TextureFormat::R8;
		settings.Wrap = TextureWrap::Repeat;
		REQUIRE(TextureCooker::CookLevels(CreateImage(8, 8, 1, leftColumn), settings, mips, &error));
		CHECK(mips[1].Data[3] > 10);
		settings.Wrap = TextureWrap::Clamp;
		REQUIRE(TextureCooker::CookLevels(CreateImage(8, 8, 1, leftColumn), settings, mips, &error));
		CHECK(mips[1].Data[3] == 0);
		CHECK(mips[1].Data[0] > 10);

		// Normal maps: columns tilted to +X and to +Y average to a unit normal between them.
		std::vector<uint8_t> normals;
		for (uint32_t y = 0; y < 8; y++)
		{
			for (uint32_t x = 0; x < 8; x++)
			{
				const glm::vec3 normal = x % 2 == 0 ? glm::normalize(glm::vec3(0.8f, 0.0f, 0.6f)) : glm::normalize(glm::vec3(0.0f, 0.8f, 0.6f));
				for (int channel = 0; channel < 3; channel++)
					normals.push_back(static_cast<uint8_t>(std::lround((normal[channel] * 0.5f + 0.5f) * 255.0f)));
			}
		}
		settings = {};
		settings.Format = TextureFormat::RGBA8;
		settings.NormalMap = true;
		REQUIRE(TextureCooker::CookLevels(CreateImage(8, 8, 3, normals), settings, mips, &error));
		for (size_t level = 1; level < mips.size(); level++)
		{
			CAPTURE(level);
			const glm::vec3 normal = DecodeNormal(mips[level].Data[0], mips[level].Data[1], mips[level].Data[2]);
			CHECK(glm::length(normal) == doctest::Approx(1.0f).epsilon(0.02));
			CHECK(normal.x == doctest::Approx(normal.y).epsilon(0.05));
		}

		// Color with alpha is weighted by it: transparent red does not bleed into opaque green.
		std::vector<uint8_t> coverage;
		for (uint32_t texel = 0; texel < 16; texel++)
		{
			if (texel % 2 == 0)
				Tests::AppendPixel(coverage, 255, 0, 0, 0);
			else
				Tests::AppendPixel(coverage, 0, 255, 0, 255);
		}
		settings = {};
		settings.Format = TextureFormat::RGBA8SRGB;
		settings.SRGB = true;
		settings.AlphaWeighted = true;
		REQUIRE(TextureCooker::CookLevels(CreateImage(4, 4, 4, coverage), settings, mips, &error));
		CHECK(mips[1].Data[0] < 8);
		CHECK(mips[1].Data[1] > 240);
		CHECK(std::abs(static_cast<int>(mips[1].Data[3]) - 128) <= 3);

		// Without mips there is one level, and formats without an encoder are refused.
		settings.GenerateMips = false;
		REQUIRE(TextureCooker::CookLevels(CreateImage(4, 4, 4, coverage), settings, mips, &error));
		CHECK(mips.size() == 1);
		settings.Format = TextureFormat::RGBA16F;
		CHECK_FALSE(TextureCooker::CookLevels(CreateImage(4, 4, 4, coverage), settings, mips, &error));
		CHECK(error.find("RGBA16F") != std::string::npos);
		settings.Format = TextureFormat::RGBA8;
		CHECK_FALSE(TextureCooker::CookLevels(CreateImage(4, 4, 4, {}), settings, mips, &error));
	}

	TEST_CASE("Block compression keeps color close to the source")
	{
		// The feature test's checker and a smooth gradient: imported with the default settings they are BC7, which reaches
		// the 40 dB the cooking work asks of compressed color and is never worse than BC1 at High (what color was before
		// BC7 could be encoded). BC7 at Fast (what translucent color gets at Fast) reaches 36 dB; opaque color at Fast is
		// BC1.
		const std::optional<std::vector<uint8_t>> checker = FileSystem::ReadBytes(
			FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "StrataTests" / "FeatureTest" / "Assets" / "Textures" / "Checker.png");
		REQUIRE(checker);
		const std::vector<uint8_t> gradientPixels = CreateGradient(256);
		const std::vector<uint8_t> gradient = Tests::EncodePNG(256, 256, gradientPixels);

		TextureImportSettings reference;
		reference.Compression = TextureCompression::Uncompressed;
		reference.GenerateMips = false;
		for (const auto& [name, image] : { std::pair { std::string("Checker"), *checker }, std::pair { std::string("Gradient"), gradient } })
		{
			CAPTURE(name);
			const Ref<Texture> uncompressed = Import(image, reference);
			const std::vector<uint8_t> source = DecodeLevel(*uncompressed, 0);
			TextureImportSettings settings;
			settings.GenerateMips = false;
			const Ref<Texture> high = Import(image, settings);
			CHECK(high->GetSpecification().Format == TextureFormat::BC7SRGB);
			settings.Quality = TextureQuality::Fast;
			const Ref<Texture> fast = Import(image, settings);
			CHECK(fast->GetSpecification().Format == TextureFormat::BC1SRGB);

			// The same level encoded by BC7 at Fast, and by BC1 at High.
			auto encode = [&](TextureFormat format, bool highQuality)
			{
				TextureCooker::CookSettings cook;
				cook.Format = format;
				cook.GenerateMips = false;
				cook.SRGB = true;
				cook.HighQuality = highQuality;
				std::vector<TextureMip> mips;
				std::string error;
				REQUIRE_MESSAGE(TextureCooker::CookLevels(CreateImage(uncompressed->GetWidth(), uncompressed->GetHeight(), 4, source), cook, mips, &error), error);
				std::vector<uint8_t> decoded(source.size());
				REQUIRE(BlockCompression::DecodeLevel(format, uncompressed->GetWidth(), uncompressed->GetHeight(), mips[0].Data, decoded));
				return decoded;
			};
			const double highPSNR = ComputePSNR(source, DecodeLevel(*high, 0), 4);
			const double fastBC7PSNR = ComputePSNR(source, encode(TextureFormat::BC7SRGB, false), 4);
			const double bc1PSNR = ComputePSNR(source, encode(TextureFormat::BC1SRGB, true), 4);
			const double fastPSNR = ComputePSNR(source, DecodeLevel(*fast, 0), 4);
			MESSAGE(name, ": PSNR ", highPSNR, " dB as BC7 at High, ", fastBC7PSNR, " dB as BC7 at Fast, ", bc1PSNR, " dB as BC1 at High, ", fastPSNR,
				" dB as BC1 at Fast");
			CHECK(highPSNR >= 40.0);
			CHECK(highPSNR >= bc1PSNR);
			CHECK(fastBC7PSNR >= 36.0);
			CHECK(fastPSNR >= 36.0);
		}
	}

	TEST_CASE("Data channels stay apart")
	{
		// An occlusion-roughness-metallic texture: radial occlusion in red, roughness waves in green and metallic tiles of
		// 7 x 9 texels in blue, whose edges cut through most blocks. BC1 keeps a block's colors on one line through RGB space,
		// so it would shift the roughness along the metallic edges by up to 32 of 255 (31 dB); BC7 keeps every channel, with
		// partitions along the edges or the metallic channel stored apart. At either quality: mode 6 alone (Fast's BC7 for
		// blocks of color whose alpha does not vary) has BC1's line and would lose the roughness the same way (31 dB).
		constexpr uint32_t c_Size = 256;
		std::vector<uint8_t> pixels;
		for (uint32_t y = 0; y < c_Size; y++)
		{
			for (uint32_t x = 0; x < c_Size; x++)
			{
				const float u = static_cast<float>(x) / static_cast<float>(c_Size - 1) * 2.0f - 1.0f;
				const float v = static_cast<float>(y) / static_cast<float>(c_Size - 1) * 2.0f - 1.0f;
				const float occlusion = std::clamp(1.0f - 0.6f * (u * u + v * v), 0.0f, 1.0f);
				const float roughness = 0.5f + 0.4f * std::sin(static_cast<float>(x) * 0.15f) * std::cos(static_cast<float>(y) * 0.11f);
				const bool metallic = (x / 7 + y / 9) % 2 == 0;
				pixels.push_back(static_cast<uint8_t>(std::lround(occlusion * 255.0f)));
				pixels.push_back(static_cast<uint8_t>(std::lround(roughness * 255.0f)));
				pixels.push_back(metallic ? 255 : 0);
			}
		}
		const std::vector<uint8_t> image = Tests::EncodePNG(c_Size, c_Size, 3, pixels);
		for (TextureQuality quality : { TextureQuality::High, TextureQuality::Fast })
		{
			const std::string qualityName = TextureQualityToString(quality);
			CAPTURE(qualityName);
			TextureImportSettings settings;
			settings.Usage = TextureUsage::Data;
			settings.Quality = quality;
			const Ref<Texture> texture = Import(image, settings, "Platform_orm.png");
			CHECK(texture->GetSpecification().Format == TextureFormat::BC7);
			CHECK_FALSE(texture->GetSpecification().Grayscale);

			const std::vector<uint8_t> stored = DecodeLevel(*texture, 0);
			const uint32_t storedChannels = GetTextureFormatInfo(GetDecodedFormat(texture->GetSpecification().Format)).Channels;
			REQUIRE(stored.size() == static_cast<size_t>(c_Size) * c_Size * storedChannels);
			for (uint32_t channel = 0; channel < 3; channel++)
			{
				CAPTURE(channel);
				double squaredError = 0.0;
				int maxError = 0;
				for (size_t texel = 0; texel < static_cast<size_t>(c_Size) * c_Size; texel++)
				{
					const int error = std::abs(static_cast<int>(stored[texel * storedChannels + channel]) - static_cast<int>(pixels[texel * 3 + channel]));
					squaredError += static_cast<double>(error) * error;
					maxError = std::max(maxError, error);
				}
				const double meanSquaredError = squaredError / (static_cast<double>(c_Size) * c_Size);
				const double psnr = meanSquaredError == 0.0 ? 100.0 : 10.0 * std::log10(255.0 * 255.0 / meanSquaredError);
				MESSAGE("Data channel ", channel, " stored as ", std::string(TextureFormatToString(texture->GetSpecification().Format)), " at ",
					qualityName, ": PSNR ", psnr, " dB, error at most ", maxError);
				CHECK(psnr >= 40.0);
				CHECK(maxError <= 16);
			}
		}
	}

	TEST_CASE("BC5 normal maps keep the shading normal within two degrees")
	{
		constexpr uint32_t c_Size = 256;
		const std::vector<uint8_t> image = Tests::EncodePNG(c_Size, c_Size, 3, CreateSphereNormalMap(c_Size));
		TextureImportSettings settings;
		settings.Usage = TextureUsage::NormalMap;
		const Ref<Texture> compressed = Import(image, settings, "Sphere_normal.png");
		REQUIRE(compressed->GetSpecification().Format == TextureFormat::BC5);
		settings.Compression = TextureCompression::Uncompressed;
		const Ref<Texture> reference = Import(image, settings, "Sphere_normal.png");
		REQUIRE(reference->GetMipCount() == compressed->GetMipCount());

		// Every level against the same level filtered without compression; Z is reconstructed like Forward.frag does.
		for (uint32_t level = 0; level < compressed->GetMipCount(); level++)
		{
			CAPTURE(level);
			const std::vector<uint8_t> expected = DecodeLevel(*reference, level);
			const std::vector<uint8_t> actual = DecodeLevel(*compressed, level);
			const size_t texels = expected.size() / 4;
			REQUIRE(actual.size() == texels * 2);
			double totalDegrees = 0.0;
			double maxDegrees = 0.0;
			for (size_t texel = 0; texel < texels; texel++)
			{
				const glm::vec3 wanted = glm::normalize(DecodeNormal(expected[texel * 4], expected[texel * 4 + 1], expected[texel * 4 + 2]));
				const glm::vec2 xy = glm::vec2(actual[texel * 2], actual[texel * 2 + 1]) / 255.0f * 2.0f - 1.0f;
				const glm::vec3 shaded = glm::normalize(glm::vec3(xy, std::sqrt(std::clamp(1.0f - glm::dot(xy, xy), 0.0f, 1.0f))));
				const double degrees = std::acos(std::clamp(static_cast<double>(glm::dot(wanted, shaded)), -1.0, 1.0)) * 180.0 / std::numbers::pi;
				totalDegrees += degrees;
				maxDegrees = std::max(maxDegrees, degrees);
			}
			const double meanDegrees = totalDegrees / static_cast<double>(texels);
			MESSAGE("BC5 sphere normal map, level ", level, ": mean angular error ", meanDegrees, " degrees, at most ", maxDegrees);
			// The smallest levels hold the sphere's silhouette, where Z changes fastest, in a few blocks of eight values per
			// channel: their error is larger, but still small.
			CHECK(meanDegrees <= (level == 0 ? 2.0 : 4.0));
		}
	}

	TEST_CASE("Cooking gives the same bytes on every run and with and without worker threads")
	{
		// Large enough that the first levels split into several batches (rows of blocks, texels uncompressed), so workers
		// encode parts of them; the sizes are not multiples of four, so edge blocks are among them. Noise over the gradient
		// gives the BC7 search blocks where many modes compete; the alpha ramp gives it alpha.
		constexpr uint32_t c_Width = 1030;
		constexpr uint32_t c_Height = 262;
		std::vector<uint8_t> pixels = CreateGradient(c_Width);
		pixels.resize(static_cast<size_t>(c_Width) * c_Height * 4);
		uint32_t random = 0x12345678u;
		for (size_t texel = 0; texel < static_cast<size_t>(c_Width) * c_Height; texel++)
		{
			random ^= random << 13;
			random ^= random >> 17;
			random ^= random << 5;
			pixels[texel * 4] = static_cast<uint8_t>(pixels[texel * 4] ^ (random & 0x0F));
			pixels[texel * 4 + 3] = static_cast<uint8_t>(texel % c_Width * 255 / (c_Width - 1));
		}
		const std::vector<uint8_t> image = Tests::EncodePNG(c_Width, c_Height, pixels);
		for (TextureFormat format : { TextureFormat::BC1SRGB, TextureFormat::BC7SRGB })
		{
			CAPTURE(std::string(TextureFormatToString(format)));
			REQUIRE(TextureCooker::GetEncodeBatchCount(format, c_Width, c_Height) > 1);
			REQUIRE(TextureCooker::GetEncodeBatchCount(format, c_Width / 2, c_Height / 2) > 1);
		}
		REQUIRE(TextureCooker::GetEncodeBatchCount(TextureFormat::RGBA8SRGB, c_Width, c_Height) > 1);

		TextureImportSettings high;
		TextureImportSettings fast;
		fast.Quality = TextureQuality::Fast;
		TextureImportSettings uncompressed;
		uncompressed.Compression = TextureCompression::Uncompressed;
		for (const TextureImportSettings& settings : { high, fast, uncompressed })
		{
			INFO("Compression ", std::string(TextureCompressionToString(settings.Compression)), ", quality ", std::string(TextureQualityToString(settings.Quality)));
			const Ref<Texture> first = Import(image, settings);
			const bool compressed = settings.Compression == TextureCompression::Auto;
			CHECK(first->GetSpecification().Format == (compressed ? TextureFormat::BC7SRGB : TextureFormat::RGBA8SRGB));
			const std::vector<uint8_t> sequential = first->Serialize();
			CHECK(Import(image, settings)->Serialize() == sequential);
			std::vector<uint8_t> parallel;
			{
				Tests::ScopedJobSystem jobs(4, 1);
				parallel = Import(image, settings)->Serialize();
			}
			CHECK(parallel == sequential);
		}
		CHECK(TextureCooker::GetEncodeBatchCount(TextureFormat::BC1SRGB, 4, 4) == 1);
		CHECK(TextureCooker::GetEncodeBatchCount(TextureFormat::RGBA16F, 1024, 1024) == 0);
	}

	TEST_CASE("A cancelled cook stops at its next check and fails as cancelled")
	{
		const std::vector<uint8_t> gradient = CreateGradient(64);
		std::atomic<bool> cancel = true;

		// Cancelled before it starts: nothing is encoded or filtered, whatever the format.
		std::vector<uint8_t> data;
		for (TextureFormat format : { TextureFormat::BC7SRGB, TextureFormat::BC1SRGB, TextureFormat::RGBA8SRGB })
		{
			CAPTURE(std::string(TextureFormatToString(format)));
			CHECK_FALSE(TextureCooker::EncodeLevel(CreateImage(64, 64, 4, gradient), format, true, data, &cancel));
		}
		TextureCooker::CookSettings settings;
		settings.Format = TextureFormat::BC7SRGB;
		settings.SRGB = true;
		settings.Cancel = &cancel;
		std::vector<TextureMip> mips;
		std::string error;
		CHECK_FALSE(TextureCooker::CookLevels(CreateImage(64, 64, 4, gradient), settings, mips, &error));
		CHECK(error == TextureCooker::c_CancelledError);
		TextureCooker::FloatImage hdr;
		hdr.Width = 8;
		hdr.Height = 8;
		hdr.Pixels.assign(8 * 8 * 4, 1.0f);
		CHECK_FALSE(TextureCooker::CookHDRLevels(hdr, true, TextureWrap::Repeat, mips, &error, &cancel));
		CHECK(error == TextureCooker::c_CancelledError);
		const std::vector<uint8_t> png = Tests::EncodePNG(64, 64, gradient);
		CHECK_FALSE(TextureImporter::Decode(png, TextureImportSettings(), "Gradient.png", &error, &cancel));
		CHECK(error == "Gradient.png: cancelled");

		// The same calls with the flag clear cook as usual.
		cancel = false;
		REQUIRE_MESSAGE(TextureCooker::CookLevels(CreateImage(64, 64, 4, gradient), settings, mips, &error), error);
		CHECK(mips.size() == 7);
		REQUIRE_MESSAGE(TextureImporter::Decode(png, TextureImportSettings(), "Gradient.png", &error, &cancel), error);

		// Cancelled while a large level is encoded on several threads: every thread stops at its next block instead of
		// finishing the level (which takes seconds at High quality).
		constexpr uint32_t c_Size = 2048;
		std::vector<uint8_t> noise(static_cast<size_t>(c_Size) * c_Size * 4);
		uint32_t random = 0x2468ACE1u;
		for (uint8_t& value : noise)
		{
			random ^= random << 13;
			random ^= random >> 17;
			random ^= random << 5;
			value = static_cast<uint8_t>(random);
		}
		const TextureCooker::Image large = CreateImage(c_Size, c_Size, 4, noise);
		Tests::ScopedJobSystem jobs(4, 1);
		std::atomic<bool> stop = false;
		std::atomic<bool> finished = false;
		bool encoded = true;
		std::thread cook([&]()
		{
			std::vector<uint8_t> blocks;
			encoded = TextureCooker::EncodeLevel(large, TextureFormat::BC7SRGB, true, blocks, &stop);
			finished = true;
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		const bool finishedEarly = finished;
		const auto cancelled = std::chrono::steady_clock::now();
		stop = true;
		cook.join();
		const double stoppedAfter = std::chrono::duration<double>(std::chrono::steady_clock::now() - cancelled).count();
		REQUIRE_FALSE(finishedEarly);
		CHECK_FALSE(encoded);
		CHECK_MESSAGE(stoppedAfter < 2.0, "The encode went on for ", stoppedAfter, " s after it was cancelled");
	}

	TEST_CASE("Texture names give their usage by their tokens, wherever they stand, and DirectX normal maps flip green")
	{
		const struct
		{
			const char* Name;
			TextureUsage Usage;
			bool FlipGreen;
		} cases[] = {
			{ "aerial_rocks_02_nor_gl_1k.png", TextureUsage::NormalMap, false },
			{ "rocks_nor_dx_2k.png", TextureUsage::NormalMap, true },
			{ "T_Brick_NormalDX.png", TextureUsage::NormalMap, true },
			{ "Bricks_NormalGL.png", TextureUsage::NormalMap, false },
			{ "stone_n.png", TextureUsage::NormalMap, false },
			{ "stone_n_2048.png", TextureUsage::NormalMap, false },
			{ "Brick_normal.png", TextureUsage::NormalMap, false },
			{ "brick_Rough_4k.png", TextureUsage::Data, false },
			{ "wood_ao_1k.png", TextureUsage::Data, false },
			{ "metal_plate_metallic_2048x2048.png", TextureUsage::Data, false },
			{ "wood_diff_1k.png", TextureUsage::Color, false },
			{ "banner_1k.png", TextureUsage::Color, false },
			{ "snow.png", TextureUsage::Color, false },
			{ "n_logo.png", TextureUsage::Color, false }, // "n" counts only as the last token
			{ "Sky.hdr", TextureUsage::HDR, false } };
		for (const auto& testCase : cases)
		{
			CAPTURE(testCase.Name);
			const TextureImportSettings settings = TextureImportSettings::GetDefaults(testCase.Name);
			CHECK(settings.Usage == testCase.Usage);
			CHECK(settings.FlipGreen == testCase.FlipGreen);
		}
		CHECK(TextureImportSettings::GetNameTokens("T_Brick_NormalDX") == std::vector<std::string> { "t", "brick", "normal" });

		// The setting round trips, and applies to normal maps only.
		TextureImportSettings flipped;
		flipped.FlipGreen = true;
		CHECK(TextureImportSettings::FromJson(flipped.ToJson()).FlipGreen);
		TextureImporter importer;
		nlohmann::json values = flipped.ToJson();
		CHECK_FALSE(importer.GetInactiveReason("FlipGreen", values).empty());
		values["Usage"] = "NormalMap";
		CHECK(importer.GetInactiveReason("FlipGreen", values).empty());
	}

	TEST_CASE("FlipGreen inverts the green channel of normal maps stored as RGBA8 and BC5, before their mips are filtered")
	{
		constexpr uint32_t c_Size = 64;
		const std::vector<uint8_t> image = Tests::EncodePNG(c_Size, c_Size, 3, CreateSphereNormalMap(c_Size));
		for (const TextureCompression compression : { TextureCompression::Uncompressed, TextureCompression::Auto })
		{
			CAPTURE(TextureCompressionToString(compression));
			TextureImportSettings settings;
			settings.Usage = TextureUsage::NormalMap;
			settings.Compression = compression;
			const Ref<Texture> plain = Import(image, settings, "Sphere_nor_gl.png");
			settings.FlipGreen = true;
			const Ref<Texture> flipped = Import(image, settings, "Sphere_nor_dx.png");
			CHECK(flipped->GetSpecification().Format == (compression == TextureCompression::Auto ? TextureFormat::BC5 : TextureFormat::RGBA8));
			REQUIRE(flipped->GetMipCount() == plain->GetMipCount());
			for (uint32_t level = 0; level < plain->GetMipCount(); level++)
			{
				CAPTURE(level);
				const std::vector<uint8_t> a = DecodeLevel(*plain, level);
				const std::vector<uint8_t> b = DecodeLevel(*flipped, level);
				REQUIRE(a.size() == b.size());
				// Red is the same, green is mirrored: exactly for RGBA8 (within rounding), within BC5's quantization (each block
				// encodes its own range in eight steps; the sphere's edges are steep) for BC5. Unflipped, green would be off by up
				// to 255.
				const bool compressed = compression == TextureCompression::Auto;
				const size_t stride = compressed ? 2 : 4; // BC5 decodes to two channels
				int redDifference = 0;
				int greenDifference = 0;
				double greenTotal = 0.0;
				const size_t texels = a.size() / stride;
				for (size_t texel = 0; texel < texels; texel++)
				{
					redDifference = std::max(redDifference, std::abs(static_cast<int>(a[texel * stride]) - static_cast<int>(b[texel * stride])));
					const int green = std::abs(static_cast<int>(a[texel * stride + 1]) + static_cast<int>(b[texel * stride + 1]) - 255);
					greenDifference = std::max(greenDifference, green);
					greenTotal += green;
				}
				CHECK(redDifference <= (compressed ? 24 : 1));
				CHECK(greenDifference <= (compressed ? 24 : 1));
				CHECK(greenTotal / static_cast<double>(texels) <= (compressed ? 2.0 : 1.0));
			}
		}
	}

	TEST_CASE("RemoveSun is described, off by default, inactive but on HDR textures, and kept by the settings")
	{
		TextureImporter importer;
		const std::vector<PropertyInfo> properties = importer.GetSettingsProperties();
		const auto property = std::find_if(properties.begin(), properties.end(), [](const PropertyInfo& info) { return info.Name == "RemoveSun"; });
		REQUIRE(property != properties.end());
		CHECK(property->Type == PropertyType::Bool);
		CHECK(property->DisplayName == "Remove Sun");
		CHECK_FALSE(property->Tooltip.empty());
		const nlohmann::json empty = nlohmann::json::object();
		CHECK(std::get<bool>(property->GetValue(&empty)) == false);
		// Every texture's defaults name it, off.
		for (const char* source : { "Sky.hdr", "Albedo.png", "Brick_normal.png" })
		{
			CAPTURE(source);
			const nlohmann::json defaults = importer.GetDefaultSettings(source);
			REQUIRE(defaults.contains("RemoveSun"));
			CHECK(defaults["RemoveSun"] == false);
		}
		for (const char* usage : { "Color", "NormalMap", "Data", "HDR" })
		{
			CAPTURE(usage);
			const nlohmann::json values = { { "Usage", usage }, { "RemoveSun", true } };
			CHECK(importer.GetInactiveReason("RemoveSun", values).empty() == (std::string(usage) == "HDR"));
		}
		TextureImportSettings settings;
		settings.RemoveSun = true;
		CHECK(TextureImportSettings::FromJson(settings.ToJson()).RemoveSun);
		CHECK_FALSE(TextureImportSettings::FromJson(nlohmann::json::object()).RemoveSun);
	}

	TEST_CASE("RemoveSun leaves no texel of the cooked sky above the sun's threshold, and off cooks the same bytes as before the setting")
	{
		constexpr uint32_t c_Width = 1024;
		constexpr uint32_t c_Height = 512;
		Tests::SyntheticSun sun;
		sun.Direction = glm::normalize(glm::vec3(-0.3f, 0.5f, 0.8f));
		sun.RadiusDegrees = 1.0f;
		const std::vector<float> sky = Tests::MakeSyntheticSky(c_Width, c_Height, sun);
		const std::vector<uint8_t> encoded = Tests::EncodeSyntheticSky(c_Width, c_Height, sky);
		REQUIRE_FALSE(encoded.empty());
		// The threshold of the sky as the file stores it.
		std::string error;
		const std::optional<HDRSun::Image> decoded = HDRSun::Decode(encoded, &error);
		REQUIRE_MESSAGE(decoded, error);
		const std::optional<HDRSun::Sun> found = HDRSun::Find(decoded->GetPixels(), c_Width, c_Height);
		REQUIRE(found);

		const nlohmann::json plain = { { "Usage", "HDR" }, { "Wrap", "Clamp" } };
		nlohmann::json removed = plain;
		removed["RemoveSun"] = true;
		std::vector<std::string> warnings;
		const Ref<Texture> without = TextureImporter::Decode(encoded, TextureImportSettings::FromJson(removed), "Sky.hdr", &error, nullptr, &warnings);
		REQUIRE_MESSAGE(without, error);
		CHECK(warnings.empty());
		REQUIRE(without->GetSpecification().Format == TextureFormat::RGBA16F);
		const std::span<const uint8_t> level = without->GetMipData(0);
		REQUIRE(level.size() == static_cast<size_t>(c_Width) * c_Height * 8);
		float brightest = 0.0f;
		for (size_t texel = 0; texel < static_cast<size_t>(c_Width) * c_Height; texel++)
		{
			uint16_t half[4];
			std::memcpy(half, level.data() + texel * sizeof(half), sizeof(half));
			const glm::vec3 color(glm::unpackHalf1x16(half[0]), glm::unpackHalf1x16(half[1]), glm::unpackHalf1x16(half[2]));
			brightest = std::max(brightest, 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b);
		}
		// Half floats round by up to one part in 2048.
		CHECK(brightest <= found->Threshold * 1.001f);
		CHECK(brightest > 1.0f); // The sky stays

		// Off, with or without the key: the bytes of a texture imported before the setting existed.
		const Ref<Texture> before = Import(encoded, TextureImportSettings::FromJson(plain), "Sky.hdr");
		nlohmann::json off = plain;
		off["RemoveSun"] = false;
		const Ref<Texture> after = Import(encoded, TextureImportSettings::FromJson(off), "Sky.hdr");
		CHECK(before->Serialize() == after->Serialize());
		CHECK(before->Serialize() != without->Serialize());

		// A sky without a sun imports as it is, with a warning that says so.
		const std::vector<float> overcast(static_cast<size_t>(64) * 32 * 4, 1.0f);
		warnings.clear();
		const Ref<Texture> plainSky = TextureImporter::Decode(Tests::EncodeSyntheticSky(64, 32, overcast), TextureImportSettings::FromJson(removed), "Overcast.hdr",
			&error, nullptr, &warnings);
		REQUIRE_MESSAGE(plainSky, error);
		REQUIRE(warnings.size() == 1);
		CHECK(warnings[0].find("Remove Sun found no sun") != std::string::npos);
	}
}
