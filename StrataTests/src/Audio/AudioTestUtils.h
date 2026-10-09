#pragma once

#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/EditorAssetManager.h"
#include "Strata/Audio/AudioClipAsset.h"
#include "Strata/Audio/AudioEngine.h"
#include "Strata/Core/FileSystem.h"
#include "TestHelpers.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Strata::Tests
{

	constexpr uint32_t c_SampleRate = 48000;
	constexpr uint32_t c_Channels = 2;
	constexpr float c_SineAmplitude = 0.5f;
	constexpr float c_SineRms = 0.35355339f; // c_SineAmplitude / sqrt(2)
	// The playback cursor runs ahead of the mixed output by up to one mixing period (~11 ms at 48 kHz).
	constexpr float c_PositionTolerance = 0.025f;
	// Frames to mix after moving a spatial voice or the listener so the spatializer's gain smoothing settles.
	constexpr uint64_t c_SettleFrames = 2400;

	// Initializes the audio engine without an output device for the duration of a test.
	struct ScopedAudioEngine
	{
		explicit ScopedAudioEngine(uint32_t maxOneShots = 128)
		{
			AudioEngineSpecification specification;
			specification.NullDevice = true;
			specification.SampleRate = c_SampleRate;
			specification.Channels = c_Channels;
			specification.MaxOneShots = maxOneShots;
			Initialized = AudioEngine::Init(specification);
		}

		~ScopedAudioEngine()
		{
			AudioEngine::Shutdown();
		}

		ScopedAudioEngine(const ScopedAudioEngine&) = delete;
		ScopedAudioEngine& operator=(const ScopedAudioEngine&) = delete;

		bool Initialized = false;
	};

	// Pulls the next frames of the mix. The buffer is pre-filled with a non-zero value so tests notice unwritten frames.
	inline std::vector<float> Render(uint64_t frameCount)
	{
		std::vector<float> output(static_cast<size_t>(frameCount) * c_Channels, -1.0f);
		CHECK(AudioEngine::ReadFrames(output.data(), frameCount) == frameCount);
		return output;
	}

	// Root mean square of every sample, or of one channel of the interleaved stereo mix.
	inline float ComputeRms(const std::vector<float>& samples, int channel = -1)
	{
		double sum = 0.0;
		size_t count = 0;
		for (size_t index = 0; index < samples.size(); index++)
		{
			if (channel >= 0 && index % c_Channels != static_cast<size_t>(channel))
				continue;
			sum += static_cast<double>(samples[index]) * static_cast<double>(samples[index]);
			count++;
		}
		return count > 0 ? static_cast<float>(std::sqrt(sum / static_cast<double>(count))) : 0.0f;
	}

	// Steady-state level of the mix: lets gain smoothing settle, then measures.
	inline float MeasureRms(int channel = -1)
	{
		Render(c_SettleFrames);
		return ComputeRms(Render(4800), channel);
	}

	// A sine clip asset (mono, 440 Hz).
	inline Ref<AudioClipAsset> CreateClipAsset(float seconds, float amplitude = c_SineAmplitude)
	{
		const std::vector<uint8_t> wav = CreateSineWav(seconds, c_SampleRate, 1, 440.0f, 0.0f, amplitude);
		std::string error;
		Ref<AudioClipAsset> asset = AudioClipAsset::Deserialize(AudioClipAsset::Cook(wav, AudioClipLoadMode::Decompressed), "Sine", &error);
		REQUIRE_MESSAGE(asset, error);
		return asset;
	}

	// An empty project whose editor asset manager is the active one while the project exists.
	struct AudioProject
	{
		std::filesystem::path Sounds;
		Ref<EditorAssetManager> Manager;

		AudioProject()
		{
			const std::filesystem::path root = CreateTemporaryDirectory("AudioSystemProject");
			Sounds = root / "Assets" / "Sounds";
			REQUIRE(FileSystem::CreateDirectories(Sounds));
			EditorAssetManagerSpecification specification;
			specification.AssetDirectory = root / "Assets";
			specification.CacheDirectory = root / ".strata" / "Cache";
			specification.WatchFiles = false;
			Manager = CreateRef<EditorAssetManager>(specification);
			Manager->Scan();
			AssetManager::SetActive(Manager);
		}

		~AudioProject()
		{
			AssetManager::SetActive(nullptr);
		}

		AudioProject(const AudioProject&) = delete;
		AudioProject& operator=(const AudioProject&) = delete;

		// A clip that is loaded from the start.
		AssetHandle AddClip(float seconds, float amplitude = c_SineAmplitude)
		{
			AssetMetadata metadata;
			metadata.Name = "Sine";
			return Manager->AddMemoryAsset(CreateClipAsset(seconds, amplitude), metadata);
		}

		// A clip file of the project: it loads in the background once requested, until the asset manager finishes the
		// load (Update, WaitForPendingLoads).
		AssetHandle AddClipFile(const std::string& name, float seconds, float amplitude = c_SineAmplitude)
		{
			WriteClipFile(name, seconds, amplitude);
			Manager->Scan();
			const AssetHandle handle = Manager->FindAssetByPath("Sounds/" + name + ".wav");
			REQUIRE(handle.IsValid());
			return handle;
		}

		void WriteClipFile(const std::string& name, float seconds, float amplitude) const
		{
			REQUIRE(FileSystem::WriteBytes(Sounds / (name + ".wav"), CreateSineWav(seconds, c_SampleRate, 1, 440.0f, 0.0f, amplitude)));
		}
	};

}
