#include <doctest/doctest.h>

#include "Editor/AudioPreview.h"
#include "Editor/ProjectThumbnail.h"
#include "Editor/ThumbnailCache.h"
#include "TestHelpers.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Asset/TextureImporter.h>
#include <Strata/Audio/AudioClip.h>
#include <Strata/Audio/AudioEngine.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Renderer/CookedTexture.h>
#include <Strata/Renderer/Renderer.h>
#include <Strata/Renderer/Texture.h>

#include <glm/gtc/packing.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;

namespace
{

	// A texture of `format` whose every level is filled with `texel` (the bytes of one texel, or of one 4x4 block).
	Ref<Texture> CreateUniformTexture(TextureFormat format, uint32_t width, uint32_t height, std::span<const uint8_t> texel, bool mips = true)
	{
		std::vector<TextureMip> levels;
		const uint32_t count = mips ? TextureUtils::CalculateMipCount(width, height) : 1;
		for (uint32_t level = 0; level < count; level++)
		{
			TextureMip mip;
			mip.Width = std::max(1u, width >> level);
			mip.Height = std::max(1u, height >> level);
			mip.Data.resize(static_cast<size_t>(GetMipByteSize(format, mip.Width, mip.Height)));
			for (size_t offset = 0; offset < mip.Data.size(); offset += texel.size())
				std::memcpy(mip.Data.data() + offset, texel.data(), texel.size());
			levels.push_back(std::move(mip));
		}
		TextureSpecification specification;
		specification.Format = format;
		std::string error;
		Ref<Texture> texture = Texture::Create(specification, std::move(levels), &error);
		REQUIRE_MESSAGE(texture, error);
		return texture;
	}

	// The thumbnail of a texture from its header and mip tail only, as the cache reads it.
	ThumbnailImage MakeFromTail(const Texture& texture)
	{
		const std::vector<uint8_t> cooked = texture.Serialize();
		const CookedTextureByteRange tail = texture.GetMipTailByteRange(ThumbnailCache::c_Size);
		const CookedTextureLevel& smallest = texture.GetCookedLayout().Levels[texture.GetMipCount() - 1];
		const uint64_t end = tail.LevelCount > 0 ? tail.Offset + tail.Size : smallest.Offset + smallest.Size;
		std::string error;
		const std::optional<CookedTextureView> view = ParseCookedTexturePrefix(std::span<const uint8_t>(cooked.data(), static_cast<size_t>(end)), cooked.size(), &error);
		REQUIRE_MESSAGE(view, error);
		std::optional<ThumbnailImage> image = ThumbnailCache::MakeTextureThumbnail(*view, &error);
		REQUIRE_MESSAGE(image, error);
		REQUIRE(image->Pixels.size() == static_cast<size_t>(image->Width) * image->Height * 4);
		return *image;
	}

	glm::ivec4 PixelAt(const ThumbnailImage& image, uint32_t x, uint32_t y)
	{
		const uint8_t* pixel = image.Pixels.data() + (static_cast<size_t>(y) * image.Width + x) * 4;
		return glm::ivec4(pixel[0], pixel[1], pixel[2], pixel[3]);
	}

	bool IsNear(const glm::ivec4& value, const glm::ivec4& expected, int tolerance)
	{
		const glm::ivec4 difference = glm::abs(value - expected);
		return difference.x <= tolerance && difference.y <= tolerance && difference.z <= tolerance && difference.w <= tolerance;
	}

}

TEST_SUITE("Editor.Thumbnails")
{
	TEST_CASE("Texture thumbnails come from the largest level of the mip tail that fits, as their content looks")
	{
		// Color: the 128x64 level of a 512x256 chain, its bytes as they are.
		const uint8_t color[] = { 200, 100, 50, 255 };
		ThumbnailImage image = MakeFromTail(*CreateUniformTexture(TextureFormat::RGBA8SRGB, 512, 256, color));
		CHECK(image.Width == 128);
		CHECK(image.Height == 64);
		CHECK(PixelAt(image, 10, 10) == glm::ivec4(200, 100, 50, 255));

		// One channel: gray.
		const uint8_t gray[] = { 77 };
		image = MakeFromTail(*CreateUniformTexture(TextureFormat::R8, 64, 64, gray));
		CHECK(image.Width == 64);
		CHECK(PixelAt(image, 3, 3) == glm::ivec4(77, 77, 77, 255));

		// A two-channel normal map pointing straight out: Z reconstructed, shown as (n * 0.5 + 0.5).
		const uint8_t flat[] = { 128, 128 };
		image = MakeFromTail(*CreateUniformTexture(TextureFormat::RG8, 256, 256, flat));
		CHECK(IsNear(PixelAt(image, 0, 0), glm::ivec4(128, 128, 255, 255), 1));

		// HDR: tone mapped (1.0 becomes 0.5) and sRGB-encoded (188).
		const uint16_t one = static_cast<uint16_t>(glm::packHalf1x16(1.0f));
		const uint16_t hdrTexel[] = { one, one, one, one };
		image = MakeFromTail(*CreateUniformTexture(TextureFormat::RGBA16F, 32, 32, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(hdrTexel), sizeof(hdrTexel))));
		CHECK(IsNear(PixelAt(image, 1, 1), glm::ivec4(188, 188, 188, 255), 1));
	}

	TEST_CASE("Block-compressed textures are decoded, and textures without a small level are made smaller")
	{
		// BC7 as the importer cooks it.
		TextureImportSettings settings;
		std::string error;
		const Ref<Texture> bc7 = TextureImporter::Decode(Tests::CreateSolidPNG(300, 300, 30, 160, 220), settings, "Solid", &error);
		REQUIRE_MESSAGE(bc7, error);
		REQUIRE(bc7->GetSpecification().Format == TextureFormat::BC7SRGB);
		ThumbnailImage image = MakeFromTail(*bc7);
		CHECK(image.Width == 75); // 300 >> 2
		CHECK(IsNear(PixelAt(image, 30, 30), glm::ivec4(30, 160, 220, 255), 2));

		// A single 512x256 level: read whole and boxed down to fit.
		const uint8_t color[] = { 10, 20, 30, 255 };
		image = MakeFromTail(*CreateUniformTexture(TextureFormat::RGBA8, 512, 256, color, false));
		CHECK(image.Width == 128);
		CHECK(image.Height == 64);
		CHECK(PixelAt(image, 127, 63) == glm::ivec4(10, 20, 30, 255));

		// Long, thin levels (a texture imported without mips): boxed within the image, never past its last row or column.
		struct Strip
		{
			uint32_t Width, Height, FittedWidth, FittedHeight;
		};
		for (const Strip& strip : { Strip { 256, 1, 128, 1 }, Strip { 1, 4096, 1, 128 }, Strip { 4096, 1, 128, 1 }, Strip { 300, 2, 100, 1 }, Strip { 130, 3, 65, 1 } })
		{
			CAPTURE(strip.Width);
			CAPTURE(strip.Height);
			image = MakeFromTail(*CreateUniformTexture(TextureFormat::RGBA8, strip.Width, strip.Height, color, false));
			CHECK(image.Width == strip.FittedWidth);
			CHECK(image.Height == strip.FittedHeight);
			CHECK(image.Pixels.size() == static_cast<size_t>(image.Width) * image.Height * 4);
			for (uint32_t y = 0; y < image.Height; y++)
			{
				for (uint32_t x = 0; x < image.Width; x++)
					REQUIRE(PixelAt(image, x, y) == glm::ivec4(10, 20, 30, 255));
			}
		}

		// A view of data that holds none of its levels (the header alone) has nothing to show.
		const Ref<Texture> chain = CreateUniformTexture(TextureFormat::RGBA8, 256, 256, color);
		const std::vector<uint8_t> cooked = chain->Serialize();
		std::optional<CookedTextureView> header = ParseCookedTexture(cooked);
		REQUIRE(header);
		header->Data = std::span<const uint8_t>(cooked.data(), static_cast<size_t>(chain->GetCookedLayout().HeaderSize));
		error.clear();
		CHECK_FALSE(ThumbnailCache::MakeTextureThumbnail(*header, &error));
		CHECK_FALSE(error.empty());
	}

	TEST_CASE("Only textures, materials, meshes, models and prefabs get thumbnails")
	{
		for (AssetType type : { AssetType::Texture, AssetType::Material, AssetType::Mesh, AssetType::Model, AssetType::Prefab })
			CHECK(ThumbnailCache::HasThumbnail(type));
		for (AssetType type : { AssetType::Scene, AssetType::AudioClip, AssetType::Font, AssetType::None })
			CHECK_FALSE(ThumbnailCache::HasThumbnail(type));
	}

	TEST_CASE("Content stamps last across sessions and change with the source and its settings")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ThumbnailStamps");
		REQUIRE(FileSystem::WriteBytes(directory / "Assets" / "Brick.png", Tests::CreateSolidPNG(8, 8, 1, 2, 3)));
		EditorAssetManagerSpecification specification;
		specification.AssetDirectory = directory / "Assets";
		specification.CacheDirectory = directory / "Cache";
		specification.WatchFiles = false;
		uint64_t stamp = 0;
		AssetHandle brick;
		{
			const Ref<EditorAssetManager> assets = CreateRef<EditorAssetManager>(specification);
			assets->Scan();
			brick = assets->FindAssetByPath("Brick.png");
			REQUIRE(brick.IsValid());
			stamp = assets->GetContentStamp(brick);
			CHECK(stamp != 0);
			CHECK(assets->GetContentStamp(brick) == stamp);
			CHECK(assets->GetContentStamp(UUID(0x02)) == 0); // Built in
			CHECK(assets->GetContentStamp(UUID()) == 0);     // Unknown
		}
		{
			// The next session: the same stamp.
			const Ref<EditorAssetManager> assets = CreateRef<EditorAssetManager>(specification);
			assets->Scan();
			CHECK(assets->GetContentStamp(brick) == stamp);
			// New settings, then a new file.
			REQUIRE(assets->SetImportSettings(brick, { { "Compression", "Uncompressed" } }));
			const uint64_t settingsStamp = assets->GetContentStamp(brick);
			CHECK(settingsStamp != stamp);
			REQUIRE(FileSystem::WriteBytes(directory / "Assets" / "Brick.png", Tests::CreateSolidPNG(16, 16, 1, 2, 3)));
			REQUIRE(assets->ReimportAsset(brick));
			CHECK(assets->GetContentStamp(brick) != settingsStamp);
		}
	}

	TEST_CASE("Waveforms hold each column's lowest and highest sample, also of streamed clips")
	{
		// A quarter second of silence, then half a second of a tone at half amplitude.
		const std::vector<uint8_t> wav = Tests::CreateSineWav(0.75f, 48000, 2, 440.0f, 0.25f, 0.5f);
		for (AudioClipLoadMode mode : { AudioClipLoadMode::Decompressed, AudioClipLoadMode::Streamed })
		{
			CAPTURE(static_cast<int>(mode));
			const Ref<AudioClip> clip = AudioClip::LoadFromMemory(wav, "Sine", mode);
			REQUIRE(clip);
			const AudioWaveform waveform = AudioPreview::ComputeWaveform(*clip, 75);
			REQUIRE(waveform.Min.size() == 75);
			REQUIRE(waveform.Max.size() == 75);
			CHECK(waveform.Length == doctest::Approx(0.75f).epsilon(0.01));
			CHECK(std::abs(waveform.Max[10]) < 0.01f);
			CHECK(std::abs(waveform.Min[10]) < 0.01f);
			CHECK(waveform.Max[70] == doctest::Approx(0.5f).epsilon(0.05));
			CHECK(waveform.Min[70] == doctest::Approx(-0.5f).epsilon(0.05));
		}
		// No columns, no waveform.
		const Ref<AudioClip> clip = AudioClip::LoadFromMemory(wav, "Sine");
		REQUIRE(clip);
		CHECK(AudioPreview::ComputeWaveform(*clip, 0).Min.empty());
	}

	TEST_CASE("The preview voice needs audio output, and the waveform arrives from a job")
	{
		AudioPreview preview;
		const Ref<AudioClip> clip = AudioClip::LoadFromMemory(Tests::CreateSineWav(0.2f), "Beep");
		REQUIRE(clip);
		const AssetHandle handle = UUID();
		// The test process runs no audio engine: nothing plays.
		if (!AudioEngine::IsInitialized())
		{
			CHECK_FALSE(preview.Play(handle, clip));
			CHECK_FALSE(preview.IsPlaying(handle));
		}
		CHECK(preview.GetPosition() == 0.0f);
		const AudioWaveform* waveform = nullptr;
		for (int attempt = 0; attempt < 2000 && !waveform; attempt++)
		{
			waveform = preview.RequestWaveform(handle, clip, 32);
			if (!waveform)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		REQUIRE(waveform);
		CHECK(waveform->Min.size() == 32);
	}

	TEST_CASE("The project's picture is averaged down from the game window's size it is rendered at")
	{
		// 6x3 RGBA8 averaged over 3x3 blocks: two pixels, each the rounded mean of its block.
		ReadbackImage image;
		image.Width = 6;
		image.Height = 3;
		image.BytesPerPixel = 4;
		image.Pixels.resize(6 * 3 * 4);
		for (uint32_t y = 0; y < 3; y++)
		{
			for (uint32_t x = 0; x < 6; x++)
			{
				uint8_t* pixel = image.Pixels.data() + (static_cast<size_t>(y) * 6 + x) * 4;
				pixel[0] = x < 3 ? 255 : 0;                                // Left block white, right black
				pixel[1] = static_cast<uint8_t>(10 * (y * 6 + x));           // A ramp
				pixel[2] = (x + y) % 2 == 0 ? 200 : 100;                     // A checker
				pixel[3] = 255;
			}
		}
		const std::optional<ReadbackImage> downscaled = ProjectThumbnail::Downscale(image, 3);
		REQUIRE(downscaled);
		CHECK(downscaled->Width == 2);
		CHECK(downscaled->Height == 1);
		CHECK(downscaled->BytesPerPixel == 4);
		REQUIRE(downscaled->Pixels.size() == 8);
		CHECK(downscaled->Pixels[0] == 255);
		CHECK(downscaled->Pixels[4] == 0);
		CHECK(downscaled->Pixels[1] == 70);  // (0+10+20+60+70+80+120+130+140) / 9
		CHECK(downscaled->Pixels[5] == 100); // (30+40+50+90+100+110+150+160+170) / 9
		CHECK(downscaled->Pixels[2] == 156); // 5 x 200 + 4 x 100 over 9, rounded
		CHECK(downscaled->Pixels[6] == 144); // 4 x 200 + 5 x 100 over 9, rounded
		CHECK(downscaled->Pixels[3] == 255);

		// Sizes that are not multiples of the factor, other formats and a factor of 0 are refused.
		CHECK_FALSE(ProjectThumbnail::Downscale(image, 4));
		CHECK_FALSE(ProjectThumbnail::Downscale(image, 0));
		ReadbackImage other = image;
		other.BytesPerPixel = 8;
		CHECK_FALSE(ProjectThumbnail::Downscale(other, 3));
		other = image;
		other.Pixels.pop_back();
		CHECK_FALSE(ProjectThumbnail::Downscale(other, 3));
		CHECK(ProjectThumbnail::c_Width * ProjectThumbnail::c_RenderScale == 1440);
	}
}
