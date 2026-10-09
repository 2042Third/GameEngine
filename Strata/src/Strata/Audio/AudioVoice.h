#pragma once

// Internal to the audio module: included only by Strata/Audio/*.cpp, never by public headers.

#include "Strata/Core/Base.h"
#include "Strata/Audio/AudioSource.h"

#include <glm/glm.hpp>

#include <cmath>

struct ma_engine;

namespace Strata
{

	class AudioClip;

	namespace AudioUtils
	{

		// Validation shared by the audio module's setters, which ignore non-finite input.
		inline bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

	}

	// One instance of an AudioClip in the miniaudio node graph: an ma_sound reading from a data source private to
	// this voice (a cursor over a decompressed clip's shared frames, or a decoder over a streamed clip's encoded
	// bytes). The voice keeps its clip alive. Backs both AudioSource and the engine's one-shots.
	//
	// Main thread only. miniaudio's audio thread mixes the voice concurrently; every setter below goes through
	// miniaudio's lock-free parameter updates, and the playback cursor is published through an atomic.
	class AudioVoice
	{
	public:
		// Creates a stopped voice at the start of the clip with every setting applied. Returns nullptr (and logs)
		// on failure.
		static Scope<AudioVoice> Create(ma_engine* engine, const Ref<AudioClip>& clip, const AudioSourceSettings& settings);
		~AudioVoice();

		AudioVoice(const AudioVoice&) = delete;
		AudioVoice& operator=(const AudioVoice&) = delete;

		const Ref<AudioClip>& GetClip() const;

		// Starts or resumes playback; rewinds first if the clip has already played to its end.
		bool Start();
		// Halts playback, keeping the position.
		void Stop();
		// Started and not yet at the end of the clip.
		bool IsPlaying() const;
		// Whether Start() ever succeeded, i.e. whether the mixer may have read audio from the voice.
		bool HasStarted() const;

		// Seeks are applied by the audio thread before it next reads the voice; the cursor reports the target
		// immediately.
		void SeekToFrame(uint64_t frame);
		uint64_t GetCursorInFrames() const;

		void SetVolume(float volume);
		void SetPitch(float pitch);
		void SetLooping(bool looping);
		void SetSpatial(bool spatial);
		void SetPosition(const glm::vec3& position);
		void SetVelocity(const glm::vec3& velocity);
		void SetAttenuation(AudioAttenuationModel model, float minDistance, float maxDistance, float rolloff);
	private:
		AudioVoice();

		// The miniaudio objects live in the source file so this header does not depend on miniaudio. They are
		// heap-allocated together because the node graph keeps pointers to them.
		struct Data;
		Scope<Data> m_Data;
	};

}
