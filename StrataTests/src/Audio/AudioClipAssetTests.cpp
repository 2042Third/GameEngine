#include <doctest/doctest.h>

#include "Strata/Audio/AudioClipAsset.h"
#include "TestHelpers.h"

#include <string>
#include <vector>

using namespace Strata;

TEST_SUITE("Audio.Clip")
{
	TEST_CASE("Cooked audio clips keep their load mode")
	{
		const std::vector<uint8_t> wav = Tests::CreateSineWav(0.5f, 22050, 2);
		for (AudioClipLoadMode mode : { AudioClipLoadMode::Decompressed, AudioClipLoadMode::Streamed })
		{
			std::string error;
			Ref<AudioClipAsset> asset = AudioClipAsset::Deserialize(AudioClipAsset::Cook(wav, mode), "Sine", &error);
			REQUIRE_MESSAGE(asset, error);
			REQUIRE(asset->GetClip());
			CHECK(asset->GetType() == AssetType::AudioClip);
			CHECK(asset->GetClip()->GetLoadMode() == mode);
			CHECK(asset->GetClip()->GetChannels() == 2);
			CHECK(asset->GetClip()->GetSampleRate() == 22050);
			CHECK(asset->GetClip()->GetFrameCount() == 11025);
			CHECK(asset->GetMemoryUsage().Cpu > 0);
			CHECK(asset->GetMemoryUsage().GetGpu() == 0);
		}
	}

	TEST_CASE("Corrupt cooked audio clips are rejected")
	{
		const std::vector<uint8_t> cooked = AudioClipAsset::Cook(Tests::CreateSineWav(0.1f), AudioClipLoadMode::Decompressed);
		std::string error;
		CHECK_FALSE(AudioClipAsset::Deserialize({}, "Empty", &error));
		CHECK_FALSE(error.empty());

		std::vector<uint8_t> wrongMagic = cooked;
		wrongMagic[0] ^= 0xFF;
		CHECK_FALSE(AudioClipAsset::Deserialize(wrongMagic, "WrongMagic", &error));
		std::vector<uint8_t> badMode = cooked;
		badMode[8] = 9;
		CHECK_FALSE(AudioClipAsset::Deserialize(badMode, "BadMode", &error));
		const std::vector<uint8_t> headerOnly(cooked.begin(), cooked.begin() + 12);
		CHECK_FALSE(AudioClipAsset::Deserialize(headerOnly, "HeaderOnly", &error));
		std::vector<uint8_t> garbage = AudioClipAsset::Cook(std::vector<uint8_t>(64, 0x42), AudioClipLoadMode::Streamed);
		CHECK_FALSE(AudioClipAsset::Deserialize(garbage, "Garbage", &error));
	}
}
