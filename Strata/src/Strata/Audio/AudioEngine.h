#pragma once

#include "Strata/Core/Base.h"

#include <glm/glm.hpp>

namespace Strata
{

	class AudioClip;
	class AudioSource;
	class AudioVoice;
	struct AudioSourceSettings;

	struct AudioEngineSpecification
	{
		// Mix without an output device. Nothing is played; the mix only advances when it is pulled with
		// AudioEngine::ReadFrames or AdvanceNullDevice. For tests, dedicated servers and headless runs.
		bool NullDevice = false;
		uint32_t SampleRate = 48000; // Mixing rate in Hz; clips at other rates are resampled per voice
		uint32_t Channels = 2;       // Output channels (2 = stereo)
		// Fire-and-forget voices are reclaimed once they finish; beyond this many (at least 1), the oldest one is cut
		// off so new one-shots always play and a flood of them cannot exhaust memory.
		uint32_t MaxOneShots = 128;
	};

	struct AudioStats
	{
		uint32_t ActiveVoices = 0;    // Voices currently playing (sources and one-shots)
		uint32_t ActiveOneShots = 0;  // One-shot voices held by the engine, including finished ones Update() has not reclaimed yet
		uint32_t AllocatedVoices = 0; // Voices holding mixer resources (sources with a clip, and one-shots)
		uint32_t SourceCount = 0;     // Live AudioSource objects
	};

	// Audio output: owns the miniaudio mixer and output device, the listener and fire-and-forget voices.
	//
	// The API is main-thread only (mixing runs on miniaudio's internal audio thread). Uses the engine's coordinate
	// system: right-handed, +Y up, -Z forward.
	//
	// Engine-wide settings (master volume, pause, listener) may be changed while the engine is not initialized; the
	// next Init applies them. Shutdown resets them to their defaults.
	class AudioEngine
	{
	public:
		// Opens the default output device. If no device is available, logs a warning and falls back to the null
		// device so the game still runs. Returns false only if the mixer itself cannot be created.
		static bool Init(const AudioEngineSpecification& specification = {});
		// Releases every voice, including the mixer voices of live AudioSources (which become inert, see AudioSource),
		// and resets the engine-wide settings.
		static void Shutdown();
		static bool IsInitialized();
		// True while mixing without an output device, whether requested or because no device was available.
		static bool IsNullDevice();
		static uint32_t GetSampleRate();   // 0 while not initialized
		static uint32_t GetChannelCount(); // 0 while not initialized

		static void SetMasterVolume(float volume); // Linear gain applied to the final mix, >= 0
		static float GetMasterVolume();
		// Pauses or resumes all audio output (e.g. while the editor pauses the game). Sources and one-shots keep their
		// state and continue where they were when resumed.
		static void SetPaused(bool paused);
		static bool IsPaused();

		// Places the listener, typically from the active camera every frame. forward and up need not be normalized
		// or exactly perpendicular, but must not be zero; up only matters for rolling around forward, so listeners
		// that do not roll can pass +Y. velocity (world units per second) drives the Doppler effect.
		static void SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up, const glm::vec3& velocity = glm::vec3(0.0f));

		// Fire-and-forget voices owned by the engine. PlayOneShot plays the clip without spatialization (UI, music
		// stingers); PlayOneShotAt plays it at a world position. Return false if the engine is not initialized or the
		// voice could not be created.
		static bool PlayOneShot(const Ref<AudioClip>& clip, float volume = 1.0f, float pitch = 1.0f);
		static bool PlayOneShotAt(const Ref<AudioClip>& clip, const glm::vec3& position, float volume = 1.0f, float pitch = 1.0f);

		// Per-frame housekeeping on the main thread: reclaims one-shots that have finished playing.
		static void Update();

		// Pulls the next frameCount frames of the mix into interleavedOutput, which must hold frameCount *
		// GetChannelCount() floats. Null device only: with a real device the device pulls the mix itself, and this
		// returns 0 and logs an error. Returns the number of frames written. While paused, writes silence.
		static uint64_t ReadFrames(float* interleavedOutput, uint64_t frameCount);
		// Null device only: mixes and discards the next `seconds` of audio, so that playback advances in real time when nothing
		// else pulls the mix (the Application calls it every frame while mixing without a device: headless runs and machines
		// without audio output). Fractions of a frame carry over to the next call; at most one second is mixed per call. Does
		// nothing with an output device, while paused or while not initialized.
		static void AdvanceNullDevice(float seconds);

		static AudioStats GetStats();
	private:
		// AudioSource integration: sources register for their whole lifetime so Shutdown can release their voices.
		static void RegisterSource(AudioSource* source);
		static void UnregisterSource(AudioSource* source);
		// Returns nullptr while the engine is not initialized or if the voice could not be created.
		static Scope<AudioVoice> CreateVoice(const Ref<AudioClip>& clip, const AudioSourceSettings& settings);

		friend class AudioSource;
	};

}
