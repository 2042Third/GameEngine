#include <doctest/doctest.h>

#include "Audio/AudioTestUtils.h"
#include "Strata/Audio/AudioClip.h"
#include "Strata/Audio/AudioEngine.h"
#include "Strata/Audio/AudioSource.h"
#include "Strata/Core/FileSystem.h"
#include "TestHelpers.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	Ref<AudioClip> CreateSineClip(float durationSeconds, AudioClipLoadMode mode = AudioClipLoadMode::Decompressed, uint32_t sampleRate = c_SampleRate)
	{
		return AudioClip::LoadFromMemory(CreateSineWav(durationSeconds, sampleRate), "Sine", mode);
	}

	// Sign changes on the first channel; proportional to the frequency of a sine.
	uint32_t CountZeroCrossings(const std::vector<float>& samples)
	{
		uint32_t crossings = 0;
		for (size_t index = c_Channels; index < samples.size(); index += c_Channels)
		{
			if ((samples[index - c_Channels] < 0.0f) != (samples[index] < 0.0f))
				crossings++;
		}
		return crossings;
	}

}

TEST_SUITE("Audio.Engine")
{
	TEST_CASE("The null device initializes, mixes silence and shuts down")
	{
		REQUIRE_FALSE(AudioEngine::IsInitialized());
		CHECK(AudioEngine::GetSampleRate() == 0);
		CHECK(AudioEngine::GetChannelCount() == 0);

		{
			ScopedAudioEngine engine;
			REQUIRE(engine.Initialized);
			CHECK(AudioEngine::IsInitialized());
			CHECK(AudioEngine::IsNullDevice());
			CHECK(AudioEngine::GetSampleRate() == c_SampleRate);
			CHECK(AudioEngine::GetChannelCount() == c_Channels);

			// Every frame is written, and an empty mix is exact silence.
			CHECK(ComputeRms(Render(1024)) == 0.0f);

			const AudioStats stats = AudioEngine::GetStats();
			CHECK(stats.ActiveVoices == 0);
			CHECK(stats.ActiveOneShots == 0);
			CHECK(stats.AllocatedVoices == 0);
			CHECK(stats.SourceCount == 0);
			AudioEngine::Update();

			CHECK(AudioEngine::ReadFrames(nullptr, 16) == 0);
			std::vector<float> buffer(c_Channels);
			CHECK(AudioEngine::ReadFrames(buffer.data(), 0) == 0);
		}

		CHECK_FALSE(AudioEngine::IsInitialized());
		CHECK_FALSE(AudioEngine::IsNullDevice());
		std::vector<float> buffer(256 * c_Channels);
		CHECK(AudioEngine::ReadFrames(buffer.data(), 256) == 0);
		AudioEngine::Shutdown(); // A second shutdown is a no-op
		AudioEngine::Update();

		// The engine can be initialized again, with a different format.
		AudioEngineSpecification specification;
		specification.NullDevice = true;
		specification.SampleRate = 44100;
		specification.Channels = 1;
		REQUIRE(AudioEngine::Init(specification));
		CHECK(AudioEngine::GetSampleRate() == 44100);
		CHECK(AudioEngine::GetChannelCount() == 1);
		{
			Ref<AudioClip> clip = CreateSineClip(0.5f);
			AudioSource source;
			REQUIRE(source.SetClip(clip));
			source.Play();
			std::vector<float> output(4410, 0.0f);
			CHECK(AudioEngine::ReadFrames(output.data(), output.size()) == output.size());
			double sum = 0.0;
			for (float sample : output)
				sum += static_cast<double>(sample) * static_cast<double>(sample);
			CHECK(static_cast<float>(std::sqrt(sum / static_cast<double>(output.size()))) == doctest::Approx(c_SineRms).epsilon(0.05));
		}
		AudioEngine::Shutdown();
	}

	TEST_CASE("Invalid specifications are rejected")
	{
		AudioEngineSpecification specification;
		specification.NullDevice = true;

		specification.Channels = 0;
		CHECK_FALSE(AudioEngine::Init(specification));
		CHECK_FALSE(AudioEngine::IsInitialized());

		specification.Channels = 2;
		specification.SampleRate = 1000;
		CHECK_FALSE(AudioEngine::Init(specification));
		specification.SampleRate = 1000000;
		CHECK_FALSE(AudioEngine::Init(specification));
		CHECK_FALSE(AudioEngine::IsInitialized());

		// Init while initialized keeps the running engine.
		specification.SampleRate = c_SampleRate;
		REQUIRE(AudioEngine::Init(specification));
		CHECK(AudioEngine::Init(specification));
		CHECK(AudioEngine::IsInitialized());
		AudioEngine::Shutdown();
		CHECK_FALSE(AudioEngine::IsInitialized());
	}

	TEST_CASE("The default output device is used when available, the null device otherwise")
	{
		Ref<AudioClip> clip = CreateSineClip(0.5f);
		AudioSource source;
		source.SetVolume(0.0f); // Stay silent on machines with speakers
		REQUIRE(source.SetClip(clip));

		REQUIRE(AudioEngine::Init());
		CHECK(AudioEngine::IsInitialized());
		CHECK(AudioEngine::GetChannelCount() > 0);
		if (!AudioEngine::IsNullDevice())
		{
			// The device pulls the mix itself.
			std::vector<float> buffer(256 * AudioEngine::GetChannelCount());
			CHECK(AudioEngine::ReadFrames(buffer.data(), 256) == 0);

			AudioEngine::SetPaused(true);
			CHECK(AudioEngine::IsPaused());
			AudioEngine::SetPaused(false);
			CHECK_FALSE(AudioEngine::IsPaused());
		}

		source.Play();
		CHECK(source.IsPlaying());
		CHECK(AudioEngine::PlayOneShot(clip, 0.0f));

		// Voices can come and go while the device is mixing.
		for (int index = 0; index < 16; index++)
		{
			AudioSource transient;
			transient.SetVolume(0.0f);
			REQUIRE(transient.SetClip(clip));
			transient.Play();
			CHECK(transient.IsPlaying());
		}

		AudioEngine::Shutdown();
		CHECK_FALSE(source.IsPlaying());
		CHECK(source.GetClip() == clip);
	}

	TEST_CASE("Master volume scales the mix")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		Ref<AudioClip> clip = CreateSineClip(1.0f);
		AudioSource source;
		REQUIRE(source.SetClip(clip));
		source.Play();

		const float fullRms = ComputeRms(Render(9600));
		CHECK(fullRms == doctest::Approx(c_SineRms).epsilon(0.05));

		AudioEngine::SetMasterVolume(0.25f);
		CHECK(AudioEngine::GetMasterVolume() == 0.25f);
		CHECK(ComputeRms(Render(9600)) / fullRms == doctest::Approx(0.25f).epsilon(0.03));

		AudioEngine::SetMasterVolume(0.0f);
		CHECK(ComputeRms(Render(4800)) == 0.0f);

		AudioEngine::SetMasterVolume(-1.0f);
		CHECK(AudioEngine::GetMasterVolume() == 0.0f);
		AudioEngine::SetMasterVolume(std::numeric_limits<float>::quiet_NaN());
		CHECK(AudioEngine::GetMasterVolume() == 0.0f);
	}

	TEST_CASE("Pausing the engine silences the mix and freezes playback")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		Ref<AudioClip> clip = CreateSineClip(1.0f);
		AudioSource source;
		REQUIRE(source.SetClip(clip));
		source.Play();
		Render(4800);

		AudioEngine::SetPaused(true);
		CHECK(AudioEngine::IsPaused());
		const float pausedPosition = source.GetPlaybackPosition();
		CHECK(AudioEngine::PlayOneShot(clip));
		CHECK(ComputeRms(Render(4800)) == 0.0f);
		CHECK(source.GetPlaybackPosition() == pausedPosition);
		// Engine pause does not change the state of individual sources.
		CHECK(source.IsPlaying());
		CHECK_FALSE(source.IsPaused());

		AudioEngine::SetPaused(false);
		CHECK_FALSE(AudioEngine::IsPaused());
		// The source and the one-shot started while paused now both play in phase.
		CHECK(ComputeRms(Render(4800)) > c_SineRms * 1.5f);
		CHECK(source.GetPlaybackPosition() == doctest::Approx(pausedPosition + 0.1f).epsilon(0.1));
	}

	TEST_CASE("The null device advances playback by the time it is given")
	{
		// Without the engine, or with nothing to advance, nothing happens.
		AudioEngine::AdvanceNullDevice(1.0f);

		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioSource source;
		REQUIRE(source.SetClip(CreateSineClip(0.5f)));
		source.Play();

		AudioEngine::AdvanceNullDevice(0.2f);
		CHECK(std::abs(source.GetPlaybackPosition() - 0.2f) < c_PositionTolerance);

		// Fractions of a frame add up over many short frames.
		for (int frame = 0; frame < 5000; frame++)
			AudioEngine::AdvanceNullDevice(0.00002f); // 0.96 frames at 48 kHz
		CHECK(std::abs(source.GetPlaybackPosition() - 0.3f) < c_PositionTolerance);

		// Invalid times and a paused engine leave playback where it is.
		const float position = source.GetPlaybackPosition();
		AudioEngine::AdvanceNullDevice(-1.0f);
		AudioEngine::AdvanceNullDevice(std::numeric_limits<float>::quiet_NaN());
		AudioEngine::AdvanceNullDevice(std::numeric_limits<float>::infinity());
		AudioEngine::SetPaused(true);
		AudioEngine::AdvanceNullDevice(0.1f);
		CHECK(source.GetPlaybackPosition() == position);
		AudioEngine::SetPaused(false);

		// A long hitch advances at most one second, like an output device that underruns.
		AudioSource longSource;
		REQUIRE(longSource.SetClip(CreateSineClip(3.0f)));
		longSource.Play();
		AudioEngine::AdvanceNullDevice(5.0f);
		CHECK(std::abs(longSource.GetPlaybackPosition() - 1.0f) < c_PositionTolerance);
		CHECK(longSource.IsPlaying());

		// Sounds end, so finished one-shots are reclaimed.
		REQUIRE(AudioEngine::PlayOneShot(CreateSineClip(0.1f)));
		AudioEngine::AdvanceNullDevice(0.25f);
		CHECK_FALSE(source.IsPlaying());
		AudioEngine::Update();
		CHECK(AudioEngine::GetStats().ActiveOneShots == 0);
	}

	TEST_CASE("Settings made before Init are applied and Shutdown resets them")
	{
		REQUIRE_FALSE(AudioEngine::IsInitialized());
		AudioEngine::SetMasterVolume(0.5f);
		AudioEngine::SetPaused(true);
		AudioEngine::SetListener(glm::vec3(0.0f, 0.0f, -18.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		CHECK(AudioEngine::GetMasterVolume() == 0.5f);
		CHECK(AudioEngine::IsPaused());

		{
			ScopedAudioEngine engine;
			REQUIRE(engine.Initialized);
			CHECK(AudioEngine::IsPaused());
			CHECK(AudioEngine::GetMasterVolume() == 0.5f);

			Ref<AudioClip> clip = CreateSineClip(1.0f);
			AudioSource source;
			REQUIRE(source.SetClip(clip));
			source.SetLooping(true);
			source.Play();
			CHECK(ComputeRms(Render(4800)) == 0.0f);

			AudioEngine::SetPaused(false);
			CHECK(ComputeRms(Render(4800)) == doctest::Approx(c_SineRms * 0.5f).epsilon(0.05));

			// The listener sits at z = -18: a voice at z = -20 is 2 units away, one at z = -38 is 20 units away
			// (inverse attenuation: gains 0.5 and 0.05). From the origin the ratio would only be about 1.9.
			source.SetSpatial(true);
			source.SetPosition(glm::vec3(0.0f, 0.0f, -20.0f));
			const float nearRms = MeasureRms();
			source.SetPosition(glm::vec3(0.0f, 0.0f, -38.0f));
			const float farRms = MeasureRms();
			CHECK(nearRms / farRms == doctest::Approx(10.0f).epsilon(0.1));
		}

		CHECK(AudioEngine::GetMasterVolume() == 1.0f);
		CHECK_FALSE(AudioEngine::IsPaused());
	}
}

TEST_SUITE("Audio.Clip")
{
	TEST_CASE("Clips decode from memory without the engine")
	{
		REQUIRE_FALSE(AudioEngine::IsInitialized());

		Ref<AudioClip> mono = AudioClip::LoadFromMemory(CreateSineWav(1.0f, 44100, 1), "Mono");
		REQUIRE(mono);
		CHECK(mono->GetChannels() == 1);
		CHECK(mono->GetSampleRate() == 44100);
		CHECK(mono->GetFrameCount() == 44100);
		CHECK(mono->GetLength() == doctest::Approx(1.0f));
		CHECK(mono->GetLoadMode() == AudioClipLoadMode::Decompressed);
		CHECK(mono->GetDebugName() == "Mono");
		CHECK(mono->GetSamples().size() == 44100);
		CHECK(mono->GetEncodedData().empty());
		CHECK(mono->GetMemoryUsage() >= 44100 * sizeof(float));

		// The decoded samples are the sine that was encoded.
		const uint32_t frame = 30;
		const double expected = c_SineAmplitude * std::sin(2.0 * std::numbers::pi * 440.0 * frame / 44100.0);
		CHECK(mono->GetSamples()[frame] == doctest::Approx(expected).epsilon(0.001));

		Ref<AudioClip> stereo = AudioClip::LoadFromMemory(CreateSineWav(0.5f, 22050, 2), "Stereo");
		REQUIRE(stereo);
		CHECK(stereo->GetChannels() == 2);
		CHECK(stereo->GetSampleRate() == 22050);
		CHECK(stereo->GetFrameCount() == 11025);
		CHECK(stereo->GetLength() == doctest::Approx(0.5f));
		CHECK(stereo->GetSamples().size() == 22050);
	}

	TEST_CASE("Streamed clips keep the encoded data")
	{
		const std::vector<uint8_t> wav = CreateSineWav(2.0f);
		Ref<AudioClip> streamed = AudioClip::LoadFromMemory(wav, "Music", AudioClipLoadMode::Streamed);
		REQUIRE(streamed);
		CHECK(streamed->GetLoadMode() == AudioClipLoadMode::Streamed);
		CHECK(streamed->GetChannels() == 1);
		CHECK(streamed->GetSampleRate() == c_SampleRate);
		CHECK(streamed->GetFrameCount() == 2 * c_SampleRate);
		CHECK(streamed->GetLength() == doctest::Approx(2.0f));
		CHECK(streamed->GetSamples().empty());
		CHECK(streamed->GetEncodedData().size() == wav.size());

		// 16-bit encoded data takes half the memory of decoded 32-bit floats.
		Ref<AudioClip> decompressed = AudioClip::LoadFromMemory(wav, "Effect");
		REQUIRE(decompressed);
		CHECK(streamed->GetMemoryUsage() < decompressed->GetMemoryUsage());
	}

	TEST_CASE("Empty, truncated and unsupported data is rejected")
	{
		for (AudioClipLoadMode mode : { AudioClipLoadMode::Decompressed, AudioClipLoadMode::Streamed })
		{
			CHECK_FALSE(AudioClip::LoadFromMemory({}, "Empty", mode));

			const std::string text = "This is a text file, not audio.";
			CHECK_FALSE(AudioClip::LoadFromMemory(std::vector<uint8_t>(text.begin(), text.end()), "Text", mode));

			std::vector<uint8_t> truncated = CreateSineWav(0.5f);
			truncated.resize(30);
			CHECK_FALSE(AudioClip::LoadFromMemory(truncated, "Truncated", mode));

			// A well-formed file without a single frame is not a usable clip.
			CHECK_FALSE(AudioClip::LoadFromMemory(CreateSineWav(0.0f), "Silent", mode));
		}
	}

	TEST_CASE("Clips load from files")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("AudioClip");
		const std::string name = "\xE7\x88\x86\xE7\x99\xBA_explosion.wav"; // "爆発_explosion.wav"
		const std::filesystem::path path = directory / FileSystem::FromUTF8(name);
		REQUIRE(FileSystem::WriteBytes(path, CreateSineWav(0.5f)));

		Ref<AudioClip> clip = AudioClip::LoadFromFile(path);
		REQUIRE(clip);
		CHECK(clip->GetFrameCount() == c_SampleRate / 2);
		CHECK(clip->GetDebugName() == FileSystem::ToUTF8(path));

		Ref<AudioClip> streamed = AudioClip::LoadFromFile(path, AudioClipLoadMode::Streamed);
		REQUIRE(streamed);
		CHECK(streamed->GetLoadMode() == AudioClipLoadMode::Streamed);
		CHECK(streamed->GetFrameCount() == c_SampleRate / 2);

		CHECK_FALSE(AudioClip::LoadFromFile(directory / "missing.wav"));
		CHECK_FALSE(AudioClip::LoadFromFile(directory));
	}

	TEST_CASE("Clips load concurrently on worker threads")
	{
		REQUIRE_FALSE(AudioEngine::IsInitialized());
		const std::vector<uint8_t> wav = CreateSineWav(0.25f);

		std::vector<Ref<AudioClip>> clips(8);
		std::vector<std::thread> threads;
		for (size_t index = 0; index < clips.size(); index++)
		{
			threads.emplace_back([&wav, &clips, index]()
			{
				const AudioClipLoadMode mode = index % 2 == 0 ? AudioClipLoadMode::Decompressed : AudioClipLoadMode::Streamed;
				clips[index] = AudioClip::LoadFromMemory(wav, "Clip " + std::to_string(index), mode);
			});
		}
		for (std::thread& thread : threads)
			thread.join();

		// Clips decoded elsewhere play on the main thread.
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		for (const Ref<AudioClip>& clip : clips)
		{
			REQUIRE(clip);
			CHECK(clip->GetFrameCount() == c_SampleRate / 4);
			CHECK(AudioEngine::PlayOneShot(clip, 1.0f / static_cast<float>(clips.size())));
		}
		CHECK(ComputeRms(Render(4800)) == doctest::Approx(c_SineRms).epsilon(0.05));
	}
}

TEST_SUITE("Audio.Source")
{
	TEST_CASE("A playing source produces output")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		Ref<AudioClip> clip = CreateSineClip(1.0f);
		REQUIRE(clip);

		AudioSource source;
		REQUIRE(source.SetClip(clip));
		CHECK(source.GetClip() == clip);
		CHECK_FALSE(source.IsPlaying());
		CHECK(ComputeRms(Render(4800)) == 0.0f);

		source.Play();
		CHECK(source.IsPlaying());
		CHECK_FALSE(source.IsPaused());
		const std::vector<float> output = Render(9600);
		// A mono clip plays on both channels of the stereo mix.
		CHECK(ComputeRms(output, 0) == doctest::Approx(c_SineRms).epsilon(0.05));
		CHECK(ComputeRms(output, 1) == doctest::Approx(c_SineRms).epsilon(0.05));
		CHECK(std::abs(source.GetPlaybackPosition() - 0.2f) < c_PositionTolerance);

		const AudioStats stats = AudioEngine::GetStats();
		CHECK(stats.ActiveVoices == 1);
		CHECK(stats.AllocatedVoices == 1);
		CHECK(stats.SourceCount == 1);

		// Play on a playing source changes nothing.
		source.Play();
		CHECK(source.IsPlaying());
		CHECK(std::abs(source.GetPlaybackPosition() - 0.2f) < c_PositionTolerance);
	}

	TEST_CASE("Stop silences and rewinds")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioSource source;
		REQUIRE(source.SetClip(CreateSineClip(1.0f)));
		source.Play();
		Render(4800);
		CHECK(source.GetPlaybackPosition() > 0.05f);

		source.Stop();
		CHECK_FALSE(source.IsPlaying());
		CHECK_FALSE(source.IsPaused());
		CHECK(source.GetPlaybackPosition() == 0.0f);
		CHECK(ComputeRms(Render(4800)) == 0.0f);

		// Playing again starts from the beginning.
		source.Play();
		CHECK(ComputeRms(Render(4800)) == doctest::Approx(c_SineRms).epsilon(0.05));
		CHECK(std::abs(source.GetPlaybackPosition() - 0.1f) < c_PositionTolerance);
	}

	TEST_CASE("Restarting after Stop or a Seek plays nothing from the old position")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		// Half a second of silence, then half a second of sine.
		Ref<AudioClip> clip = AudioClip::LoadFromMemory(CreateSineWav(1.0f, c_SampleRate, 1, 440.0f, 0.5f), "SilenceThenSine");
		REQUIRE(clip);

		AudioSource source;
		REQUIRE(source.SetClip(clip));
		source.Seek(0.6f);
		source.Play();
		CHECK(ComputeRms(Render(4800)) == doctest::Approx(c_SineRms).epsilon(0.05));

		// The beginning of the clip is silent, so any sound right after restarting would be stale audio.
		source.Stop();
		source.Play();
		CHECK(ComputeRms(Render(4800)) == 0.0f);

		source.Seek(0.6f);
		CHECK(ComputeRms(Render(4800)) == doctest::Approx(c_SineRms).epsilon(0.05));
		source.Pause();
		source.Seek(0.1f);
		CHECK(source.IsPaused());
		CHECK(source.GetPlaybackPosition() == doctest::Approx(0.1f));
		source.Play();
		CHECK(ComputeRms(Render(4800)) == 0.0f);
		CHECK(std::abs(source.GetPlaybackPosition() - 0.2f) < c_PositionTolerance);
	}

	TEST_CASE("Pause keeps the position and Play resumes")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioSource source;
		REQUIRE(source.SetClip(CreateSineClip(1.0f)));
		source.Play();
		Render(12000);

		source.Pause();
		CHECK(source.IsPaused());
		CHECK_FALSE(source.IsPlaying());
		const float pausedPosition = source.GetPlaybackPosition();
		CHECK(std::abs(pausedPosition - 0.25f) < c_PositionTolerance);
		CHECK(ComputeRms(Render(4800)) == 0.0f);
		CHECK(source.GetPlaybackPosition() == pausedPosition);

		source.Play();
		CHECK(source.IsPlaying());
		CHECK_FALSE(source.IsPaused());
		CHECK(ComputeRms(Render(4800)) == doctest::Approx(c_SineRms).epsilon(0.05));
		CHECK(std::abs(source.GetPlaybackPosition() - (pausedPosition + 0.1f)) < c_PositionTolerance);

		// Pausing a source that is not playing does nothing.
		source.Stop();
		source.Pause();
		CHECK_FALSE(source.IsPaused());
		CHECK_FALSE(source.IsPlaying());
	}

	TEST_CASE("Volume scales the output")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioSource source;
		REQUIRE(source.SetClip(CreateSineClip(1.0f)));
		source.Play();
		const float fullRms = ComputeRms(Render(9600));

		source.SetVolume(0.5f);
		CHECK(source.GetVolume() == 0.5f);
		CHECK(ComputeRms(Render(9600)) / fullRms == doctest::Approx(0.5f).epsilon(0.03));

		source.SetVolume(0.0f);
		CHECK(ComputeRms(Render(4800)) == 0.0f);
	}

	TEST_CASE("Non-looping sources end; looping sources continue")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		Ref<AudioClip> clip = CreateSineClip(0.25f);

		AudioSource oneShot;
		REQUIRE(oneShot.SetClip(clip));
		CHECK_FALSE(oneShot.IsLooping());
		oneShot.Play();
		Render(c_SampleRate / 2);
		CHECK_FALSE(oneShot.IsPlaying());
		CHECK_FALSE(oneShot.IsPaused());
		CHECK(oneShot.GetPlaybackPosition() == doctest::Approx(0.25f));
		CHECK(ComputeRms(Render(4800)) == 0.0f);

		AudioSource looping;
		REQUIRE(looping.SetClip(clip));
		looping.SetLooping(true);
		CHECK(looping.IsLooping());
		looping.Play();
		for (int pass = 0; pass < 4; pass++)
			Render(c_SampleRate / 4);
		CHECK(looping.IsPlaying());
		CHECK(looping.GetPlaybackPosition() < 0.25f);
		CHECK(ComputeRms(Render(4800)) == doctest::Approx(c_SineRms).epsilon(0.05));

		// Without looping, the clip ends the next time it reaches its end.
		looping.SetLooping(false);
		Render(c_SampleRate / 2);
		CHECK_FALSE(looping.IsPlaying());
	}

	TEST_CASE("A finished source restarts on Play")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		// Streamed clips decode on the audio thread: their finished voices must be replaced, never rewound from here.
		AudioClipLoadMode mode = AudioClipLoadMode::Decompressed;
		SUBCASE("Decompressed") {}
		SUBCASE("Streamed") { mode = AudioClipLoadMode::Streamed; }
		AudioSource source;
		REQUIRE(source.SetClip(CreateSineClip(0.1f, mode)));
		source.Play();

		// Play again the moment the source reports that it finished, before the mixer has processed its end.
		uint64_t renderedFrames = 0;
		while (source.IsPlaying() && renderedFrames < c_SampleRate)
		{
			Render(256);
			renderedFrames += 256;
		}
		REQUIRE_FALSE(source.IsPlaying());
		source.Play();
		CHECK(source.IsPlaying());
		CHECK(ComputeRms(Render(2400)) == doctest::Approx(c_SineRms).epsilon(0.05));
		CHECK(std::abs(source.GetPlaybackPosition() - 0.05f) < c_PositionTolerance);
		CHECK(AudioEngine::GetStats().AllocatedVoices == 1); // The finished voice was released
	}

	TEST_CASE("Seek moves the playback position")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioSource source;
		REQUIRE(source.SetClip(CreateSineClip(2.0f)));

		// Seeking a stopped source sets where Play starts.
		source.Seek(1.0f);
		CHECK(source.GetPlaybackPosition() == doctest::Approx(1.0f));
		source.Play();
		Render(12000);
		CHECK(std::abs(source.GetPlaybackPosition() - 1.25f) < c_PositionTolerance);

		// Seeking a playing source takes effect immediately.
		source.Seek(0.5f);
		CHECK(source.GetPlaybackPosition() == doctest::Approx(0.5f));
		Render(4800);
		CHECK(std::abs(source.GetPlaybackPosition() - 0.6f) < c_PositionTolerance);
		CHECK(source.IsPlaying());

		// Positions are clamped to the clip; seeking to the end finishes the clip.
		source.Seek(10.0f);
		CHECK(source.GetPlaybackPosition() == doctest::Approx(2.0f));
		Render(4800);
		CHECK_FALSE(source.IsPlaying());
		source.Seek(-1.0f);
		CHECK(source.GetPlaybackPosition() == 0.0f);
		source.Play();
		CHECK(ComputeRms(Render(4800)) == doctest::Approx(c_SineRms).epsilon(0.05));
	}

	TEST_CASE("Pitch changes the playback speed")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		Ref<AudioClip> clip = CreateSineClip(2.0f);

		AudioSource normal;
		REQUIRE(normal.SetClip(clip));
		normal.Play();
		const std::vector<float> normalOutput = Render(c_SampleRate / 2);
		CHECK(std::abs(normal.GetPlaybackPosition() - 0.5f) < c_PositionTolerance);
		normal.Stop();

		AudioSource fast;
		REQUIRE(fast.SetClip(clip));
		fast.SetPitch(2.0f);
		CHECK(fast.GetPitch() == 2.0f);
		fast.Play();
		const std::vector<float> fastOutput = Render(c_SampleRate / 2);
		CHECK(std::abs(fast.GetPlaybackPosition() - 1.0f) < 2.0f * c_PositionTolerance);
		// Twice the playback rate doubles the frequency of the sine.
		const float crossingRatio = static_cast<float>(CountZeroCrossings(fastOutput)) / static_cast<float>(CountZeroCrossings(normalOutput));
		CHECK(crossingRatio == doctest::Approx(2.0f).epsilon(0.05));

		// At double speed the two-second clip finishes after one second.
		Render(c_SampleRate * 6 / 10);
		CHECK_FALSE(fast.IsPlaying());

		// Clips at other sample rates are resampled to play at their natural speed.
		AudioSource lowRate;
		REQUIRE(lowRate.SetClip(CreateSineClip(1.0f, AudioClipLoadMode::Decompressed, 24000)));
		lowRate.Play();
		const std::vector<float> lowRateOutput = Render(c_SampleRate / 2);
		CHECK(std::abs(lowRate.GetPlaybackPosition() - 0.5f) < c_PositionTolerance);
		const float lowRateRatio = static_cast<float>(CountZeroCrossings(lowRateOutput)) / static_cast<float>(CountZeroCrossings(normalOutput));
		CHECK(lowRateRatio == doctest::Approx(1.0f).epsilon(0.05));
	}

	TEST_CASE("Streamed and decompressed clips play identically")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		const std::vector<uint8_t> wav = CreateSineWav(0.5f);
		Ref<AudioClip> decompressed = AudioClip::LoadFromMemory(wav, "Decompressed", AudioClipLoadMode::Decompressed);
		Ref<AudioClip> streamed = AudioClip::LoadFromMemory(wav, "Streamed", AudioClipLoadMode::Streamed);
		REQUIRE(decompressed);
		REQUIRE(streamed);

		AudioSource decompressedSource;
		REQUIRE(decompressedSource.SetClip(decompressed));
		decompressedSource.Play();
		const std::vector<float> decompressedOutput = Render(12000);
		decompressedSource.Stop();

		AudioSource streamedSource;
		REQUIRE(streamedSource.SetClip(streamed));
		streamedSource.Play();
		const std::vector<float> streamedOutput = Render(12000);
		CHECK(std::abs(streamedSource.GetPlaybackPosition() - 0.25f) < c_PositionTolerance);

		REQUIRE(decompressedOutput.size() == streamedOutput.size());
		float maxDifference = 0.0f;
		for (size_t index = 0; index < decompressedOutput.size(); index++)
			maxDifference = std::max(maxDifference, std::abs(decompressedOutput[index] - streamedOutput[index]));
		CHECK(maxDifference < 1e-5f);
		CHECK(ComputeRms(streamedOutput) == doctest::Approx(c_SineRms).epsilon(0.05));

		// Seeking, looping and ending work on the streaming decoder too.
		streamedSource.Seek(0.1f);
		Render(2400);
		CHECK(std::abs(streamedSource.GetPlaybackPosition() - 0.15f) < c_PositionTolerance);
		streamedSource.SetLooping(true);
		Render(c_SampleRate);
		CHECK(streamedSource.IsPlaying());
		CHECK(ComputeRms(Render(4800)) == doctest::Approx(c_SineRms).epsilon(0.05));
		streamedSource.SetLooping(false);
		Render(c_SampleRate);
		CHECK_FALSE(streamedSource.IsPlaying());

		CHECK(AudioEngine::PlayOneShot(streamed));
		CHECK(ComputeRms(Render(4800)) == doctest::Approx(c_SineRms).epsilon(0.05));
	}

	TEST_CASE("Changing and clearing the clip")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		Ref<AudioClip> first = CreateSineClip(1.0f);
		Ref<AudioClip> second = CreateSineClip(0.5f);

		AudioSource source;
		CHECK_FALSE(source.GetClip());
		source.Play();
		CHECK_FALSE(source.IsPlaying());
		CHECK(source.GetPlaybackPosition() == 0.0f);

		REQUIRE(source.SetClip(first));
		source.Play();
		Render(4800);
		// Setting the current clip again keeps it playing.
		REQUIRE(source.SetClip(first));
		CHECK(source.IsPlaying());

		// A different clip stops playback and rewinds.
		REQUIRE(source.SetClip(second));
		CHECK(source.GetClip() == second);
		CHECK_FALSE(source.IsPlaying());
		CHECK(source.GetPlaybackPosition() == 0.0f);
		CHECK(ComputeRms(Render(4800)) == 0.0f);
		source.Play();
		CHECK(source.IsPlaying());

		CHECK(source.SetClip(nullptr));
		CHECK_FALSE(source.GetClip());
		CHECK_FALSE(source.IsPlaying());
		CHECK(ComputeRms(Render(4800)) == 0.0f);
		CHECK(AudioEngine::GetStats().AllocatedVoices == 0);

		// The source keeps its clip alive until the clip is replaced.
		WeakRef<AudioClip> weakClip;
		{
			Ref<AudioClip> temporary = CreateSineClip(0.5f);
			weakClip = temporary;
			REQUIRE(source.SetClip(temporary));
		}
		CHECK_FALSE(weakClip.expired());
		source.Play();
		CHECK(ComputeRms(Render(4800)) == doctest::Approx(c_SineRms).epsilon(0.05));
		REQUIRE(source.SetClip(first));
		CHECK(weakClip.expired());
	}

	TEST_CASE("Invalid parameters are ignored or clamped")
	{
		AudioSource source;
		const float nan = std::numeric_limits<float>::quiet_NaN();
		const float infinity = std::numeric_limits<float>::infinity();

		source.SetPitch(0.0f);
		source.SetPitch(-2.0f);
		source.SetPitch(nan);
		CHECK(source.GetPitch() == 1.0f);
		source.SetPitch(1.5f);
		CHECK(source.GetPitch() == 1.5f);

		source.SetVolume(-1.0f);
		CHECK(source.GetVolume() == 0.0f);
		source.SetVolume(0.75f);
		source.SetVolume(nan);
		source.SetVolume(infinity);
		CHECK(source.GetVolume() == 0.75f);

		source.SetMinDistance(-3.0f);
		CHECK(source.GetMinDistance() == 0.0f);
		source.SetMaxDistance(nan);
		CHECK(source.GetMaxDistance() == AudioSourceSettings().MaxDistance);
		source.SetRolloff(-1.0f);
		CHECK(source.GetRolloff() == 0.0f);

		source.SetPosition(glm::vec3(1.0f, 2.0f, 3.0f));
		source.SetPosition(glm::vec3(nan, 0.0f, 0.0f));
		CHECK(source.GetPosition() == glm::vec3(1.0f, 2.0f, 3.0f));
		source.SetVelocity(glm::vec3(0.0f, infinity, 0.0f));
		CHECK(source.GetVelocity() == glm::vec3(0.0f));
		source.Seek(nan);
		CHECK(source.GetPlaybackPosition() == 0.0f);

		// SetSettings validates every field the same way.
		AudioSourceSettings settings;
		settings.Volume = 0.25f;
		settings.Pitch = -1.0f;
		settings.Looping = true;
		settings.Spatial = true;
		settings.Position = glm::vec3(4.0f, 5.0f, 6.0f);
		settings.AttenuationModel = AudioAttenuationModel::Linear;
		source.SetSettings(settings);
		CHECK(source.GetVolume() == 0.25f);
		CHECK(source.GetPitch() == 1.5f);
		CHECK(source.IsLooping());
		CHECK(source.IsSpatial());
		CHECK(source.GetPosition() == glm::vec3(4.0f, 5.0f, 6.0f));
		CHECK(source.GetAttenuationModel() == AudioAttenuationModel::Linear);

		// Invalid listener states are ignored.
		AudioEngine::SetListener(glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		AudioEngine::SetListener(glm::vec3(nan), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	}

	TEST_CASE("Sources work before Init and survive Shutdown")
	{
		REQUIRE_FALSE(AudioEngine::IsInitialized());
		Ref<AudioClip> clip = CreateSineClip(1.0f);

		// Configured before the engine exists.
		Scope<AudioSource> early = CreateScope<AudioSource>();
		REQUIRE(early->SetClip(clip));
		early->SetVolume(0.5f);
		early->SetLooping(true);
		early->Seek(0.5f);
		CHECK(early->GetPlaybackPosition() == doctest::Approx(0.5f));
		early->Play();
		CHECK_FALSE(early->IsPlaying());
		CHECK(AudioEngine::GetStats().SourceCount == 1);

		{
			ScopedAudioEngine engine;
			REQUIRE(engine.Initialized);
			early->Play();
			CHECK(early->IsPlaying());
			CHECK(ComputeRms(Render(4800)) == doctest::Approx(c_SineRms * 0.5f).epsilon(0.05));
			CHECK(std::abs(early->GetPlaybackPosition() - 0.6f) < c_PositionTolerance);

			// Destroying a playing source releases its voice right away.
			Scope<AudioSource> late = CreateScope<AudioSource>();
			REQUIRE(late->SetClip(clip));
			late->Play();
			CHECK(AudioEngine::GetStats().ActiveVoices == 2);
			late.reset();
			CHECK(AudioEngine::GetStats().AllocatedVoices == 1);
			CHECK(AudioEngine::GetStats().SourceCount == 1);
		} // Shuts down while the source is playing

		CHECK_FALSE(AudioEngine::IsInitialized());
		CHECK_FALSE(early->IsPlaying());
		CHECK_FALSE(early->IsPaused());
		CHECK(early->GetClip() == clip);
		CHECK(early->GetVolume() == 0.5f);
		CHECK(early->IsLooping());
		CHECK(early->GetPlaybackPosition() == 0.0f);

		// An inert source accepts every call.
		early->Play();
		early->Pause();
		early->Stop();
		early->SetPitch(1.5f);
		early->Seek(0.25f);
		CHECK_FALSE(early->IsPlaying());
		CHECK(early->GetPitch() == 1.5f);

		// It plays again once an engine is running, with the position and pitch set while it was inert.
		{
			ScopedAudioEngine engine;
			REQUIRE(engine.Initialized);
			CHECK_FALSE(early->IsPlaying());
			early->Play();
			CHECK(early->IsPlaying());
			CHECK(ComputeRms(Render(4800)) > 0.1f);
			CHECK(std::abs(early->GetPlaybackPosition() - (0.25f + 0.1f * 1.5f)) < 1.5f * c_PositionTolerance);
		}

		early.reset();
		CHECK(AudioEngine::GetStats().SourceCount == 0);
	}

	TEST_CASE("Many concurrent voices mix together")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		Ref<AudioClip> clip = CreateSineClip(1.0f);
		constexpr uint32_t sourceCount = 64;

		std::vector<Scope<AudioSource>> sources;
		for (uint32_t index = 0; index < sourceCount; index++)
		{
			Scope<AudioSource> source = CreateScope<AudioSource>();
			REQUIRE(source->SetClip(clip));
			source->SetVolume(1.0f / static_cast<float>(sourceCount));
			sources.push_back(std::move(source));
		}
		for (const Scope<AudioSource>& source : sources)
			source->Play();

		AudioStats stats = AudioEngine::GetStats();
		CHECK(stats.ActiveVoices == sourceCount);
		CHECK(stats.AllocatedVoices == sourceCount);
		CHECK(stats.SourceCount == sourceCount);
		// Every voice starts on the same frame, so the sines add up in phase.
		CHECK(ComputeRms(Render(9600)) == doctest::Approx(c_SineRms).epsilon(0.05));

		for (uint32_t index = 0; index < sourceCount; index++)
			CHECK(AudioEngine::PlayOneShot(clip, 1.0f / static_cast<float>(sourceCount)));
		stats = AudioEngine::GetStats();
		CHECK(stats.ActiveVoices == 2 * sourceCount);
		CHECK(stats.ActiveOneShots == sourceCount);
		CHECK(ComputeRms(Render(4800)) > c_SineRms * 1.5f);

		sources.clear();
		CHECK(AudioEngine::GetStats().SourceCount == 0);
		CHECK(AudioEngine::GetStats().AllocatedVoices == sourceCount);
	}
}

TEST_SUITE("Audio.Spatial")
{
	TEST_CASE("Distance attenuation follows the attenuation model")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		Ref<AudioClip> clip = CreateSineClip(1.0f);

		// Level of a looping spatial source straight ahead of the listener.
		const auto measureAtDistance = [&clip](AudioAttenuationModel model, float distance, float rolloff = 1.0f)
		{
			AudioSource source;
			REQUIRE(source.SetClip(clip));
			source.SetLooping(true);
			source.SetSpatial(true);
			source.SetAttenuationModel(model);
			source.SetMinDistance(1.0f);
			source.SetMaxDistance(50.0f);
			source.SetRolloff(rolloff);
			source.SetPosition(glm::vec3(0.0f, 0.0f, -distance));
			source.Play();
			return MeasureRms();
		};

		// Inverse: 1 / (1 + (d - 1)) gives 0.5 at 2 units and 0.05 at 20 units.
		CHECK(measureAtDistance(AudioAttenuationModel::Inverse, 2.0f) / measureAtDistance(AudioAttenuationModel::Inverse, 20.0f) == doctest::Approx(10.0f).epsilon(0.1));
		// Rolloff 2: 1 / (1 + 2 * (2 - 1)) = 1/3 against 1/2.
		CHECK(measureAtDistance(AudioAttenuationModel::Inverse, 2.0f, 2.0f) / measureAtDistance(AudioAttenuationModel::Inverse, 2.0f) == doctest::Approx(2.0f / 3.0f).epsilon(0.05));
		// Full volume inside the minimum distance.
		CHECK(measureAtDistance(AudioAttenuationModel::Inverse, 0.5f) / measureAtDistance(AudioAttenuationModel::Inverse, 1.0f) == doctest::Approx(1.0f).epsilon(0.05));
		// Linear: 1 - (d - 1) / 49 reaches silence at the maximum distance.
		CHECK(measureAtDistance(AudioAttenuationModel::Linear, 2.0f) / measureAtDistance(AudioAttenuationModel::Linear, 20.0f) == doctest::Approx((48.0f / 49.0f) / (30.0f / 49.0f)).epsilon(0.05));
		CHECK(measureAtDistance(AudioAttenuationModel::Linear, 60.0f) < 1e-4f);
		// Exponential: d ^ -1 with rolloff 1.
		CHECK(measureAtDistance(AudioAttenuationModel::Exponential, 2.0f) / measureAtDistance(AudioAttenuationModel::Exponential, 20.0f) == doctest::Approx(10.0f).epsilon(0.1));
		// No attenuation: the same level at any distance.
		CHECK(measureAtDistance(AudioAttenuationModel::NoAttenuation, 2.0f) / measureAtDistance(AudioAttenuationModel::NoAttenuation, 40.0f) == doctest::Approx(1.0f).epsilon(0.05));
	}

	TEST_CASE("Moving the listener changes the attenuation")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioSource source;
		REQUIRE(source.SetClip(CreateSineClip(1.0f)));
		source.SetLooping(true);
		source.SetSpatial(true);
		source.SetPosition(glm::vec3(0.0f, 0.0f, -20.0f));
		source.Play();
		const float farRms = MeasureRms();

		AudioEngine::SetListener(glm::vec3(0.0f, 0.0f, -18.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		const float nearRms = MeasureRms();
		CHECK(nearRms / farRms == doctest::Approx(10.0f).epsilon(0.1));
	}

	TEST_CASE("Panning follows the right-handed listener frame")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioSource source;
		REQUIRE(source.SetClip(CreateSineClip(1.0f)));
		source.SetLooping(true);
		source.SetSpatial(true);
		source.SetPosition(glm::vec3(5.0f, 0.0f, 0.0f));
		source.Play();

		// Default listener: at the origin facing -Z with +Y up, so +X is to the right.
		float left = MeasureRms(0);
		float right = MeasureRms(1);
		CHECK(right > left * 1.5f);

		// Facing +Z, +X is to the left.
		AudioEngine::SetListener(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		left = MeasureRms(0);
		right = MeasureRms(1);
		CHECK(left > right * 1.5f);

		// Facing -Z but upside down (rolled 180 degrees), +X is to the left as well.
		AudioEngine::SetListener(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f));
		left = MeasureRms(0);
		right = MeasureRms(1);
		CHECK(left > right * 1.5f);

		// Straight ahead is centered.
		AudioEngine::SetListener(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		source.SetPosition(glm::vec3(0.0f, 0.0f, -5.0f));
		left = MeasureRms(0);
		right = MeasureRms(1);
		CHECK(left == doctest::Approx(right).epsilon(0.01));
	}

	TEST_CASE("The listener's up vector reaches the mix through the mixing thread")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioSource source;
		REQUIRE(source.SetClip(CreateSineClip(1.0f)));
		source.SetLooping(true);
		source.SetSpatial(true);
		source.SetPosition(glm::vec3(5.0f, 0.0f, 0.0f));
		source.Play();
		CHECK(MeasureRms(1) > MeasureRms(0) * 1.5f);

		// Rolled upside down, +X is on the left. The vector is applied after a mixing period, so the next ones use it.
		const glm::vec3 position(0.0f, 0.0f, 1.0f);
		AudioEngine::SetListener(position, glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
		const AudioListenerState listener = AudioEngine::GetListener();
		CHECK(listener.Position == position);
		CHECK(listener.Forward == glm::vec3(0.0f, 0.0f, -1.0f));
		CHECK(listener.Up == glm::vec3(0.0f, -1.0f, 0.0f));
		CHECK(listener.Velocity == glm::vec3(1.0f, 0.0f, 0.0f));
		CHECK(AudioEngine::GetMixedListenerUp() == glm::vec3(0.0f, 1.0f, 0.0f)); // Not written from this call
		Render(64);
		CHECK(AudioEngine::GetMixedListenerUp() == glm::vec3(0.0f, -1.0f, 0.0f));
		CHECK(MeasureRms(0) > MeasureRms(1) * 1.5f);

		// Changed many times between periods, the latest vector wins.
		for (int index = 0; index < 100; index++)
			AudioEngine::SetListener(position, glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, index % 2 == 0 ? 1.0f : -1.0f, 0.0f));
		CHECK(MeasureRms(0) > MeasureRms(1) * 1.5f);
		CHECK(AudioEngine::GetMixedListenerUp() == glm::vec3(0.0f, -1.0f, 0.0f));
	}

	TEST_CASE("Non-spatial sources ignore their position")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		AudioSource source;
		REQUIRE(source.SetClip(CreateSineClip(1.0f)));
		source.SetLooping(true);
		source.SetPosition(glm::vec3(100.0f, 0.0f, 0.0f));
		CHECK_FALSE(source.IsSpatial());
		source.Play();
		CHECK(MeasureRms(0) == doctest::Approx(c_SineRms).epsilon(0.05));
		CHECK(MeasureRms(1) == doctest::Approx(c_SineRms).epsilon(0.05));

		source.SetSpatial(true);
		CHECK(source.IsSpatial());
		CHECK(MeasureRms() < c_SineRms * 0.05f);
	}
}

TEST_SUITE("Audio.OneShot")
{
	TEST_CASE("One-shots play and are reclaimed by Update once finished")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		Ref<AudioClip> clip = CreateSineClip(0.1f);

		REQUIRE(AudioEngine::PlayOneShot(clip));
		AudioStats stats = AudioEngine::GetStats();
		CHECK(stats.ActiveOneShots == 1);
		CHECK(stats.ActiveVoices == 1);
		CHECK(ComputeRms(Render(2400)) == doctest::Approx(c_SineRms).epsilon(0.05));
		AudioEngine::Update();
		CHECK(AudioEngine::GetStats().ActiveOneShots == 1);

		Render(c_SampleRate / 10);
		stats = AudioEngine::GetStats();
		CHECK(stats.ActiveVoices == 0);
		CHECK(stats.ActiveOneShots == 1);
		AudioEngine::Update();
		stats = AudioEngine::GetStats();
		CHECK(stats.ActiveOneShots == 0);
		CHECK(stats.AllocatedVoices == 0);

		// The engine releases the clip together with its last voice.
		WeakRef<AudioClip> weakClip = clip;
		REQUIRE(AudioEngine::PlayOneShot(clip, 0.5f, 2.0f));
		clip.reset();
		CHECK_FALSE(weakClip.expired());
		CHECK(ComputeRms(Render(1200)) == doctest::Approx(c_SineRms * 0.5f).epsilon(0.05));
		// At double speed the 0.1 second clip is over after 0.05 seconds.
		Render(2400);
		AudioEngine::Update();
		CHECK(AudioEngine::GetStats().ActiveOneShots == 0);
		CHECK(weakClip.expired());
	}

	TEST_CASE("Positional one-shots are attenuated")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		Ref<AudioClip> clip = CreateSineClip(0.2f);

		REQUIRE(AudioEngine::PlayOneShotAt(clip, glm::vec3(0.0f, 0.0f, -2.0f)));
		const float nearRms = MeasureRms();
		Render(c_SampleRate / 5);
		AudioEngine::Update();
		REQUIRE(AudioEngine::GetStats().ActiveOneShots == 0);

		REQUIRE(AudioEngine::PlayOneShotAt(clip, glm::vec3(0.0f, 0.0f, -20.0f)));
		const float farRms = MeasureRms();
		CHECK(nearRms / farRms == doctest::Approx(10.0f).epsilon(0.1));
	}

	TEST_CASE("One-shots beyond the limit replace the oldest")
	{
		ScopedAudioEngine engine(4);
		REQUIRE(engine.Initialized);
		Ref<AudioClip> clip = CreateSineClip(1.0f);
		for (int index = 0; index < 10; index++)
			CHECK(AudioEngine::PlayOneShot(clip, 0.1f));

		const AudioStats stats = AudioEngine::GetStats();
		CHECK(stats.ActiveOneShots == 4);
		CHECK(stats.ActiveVoices == 4);
	}

	TEST_CASE("One-shots need an initialized engine, a clip and valid parameters")
	{
		Ref<AudioClip> clip = CreateSineClip(0.1f);
		CHECK_FALSE(AudioEngine::PlayOneShot(clip));
		CHECK_FALSE(AudioEngine::PlayOneShotAt(clip, glm::vec3(1.0f)));

		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		CHECK_FALSE(AudioEngine::PlayOneShot(nullptr));
		CHECK_FALSE(AudioEngine::PlayOneShot(clip, 1.0f, 0.0f));
		CHECK_FALSE(AudioEngine::PlayOneShot(clip, std::numeric_limits<float>::quiet_NaN()));
		CHECK_FALSE(AudioEngine::PlayOneShotAt(clip, glm::vec3(std::numeric_limits<float>::infinity())));
		CHECK(AudioEngine::GetStats().ActiveOneShots == 0);
	}
}
