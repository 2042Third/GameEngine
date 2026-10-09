#pragma once

#include "Strata/Core/Base.h"

#include <glm/glm.hpp>

namespace Strata
{

	class AudioClip;
	class AudioVoice;

	// How a spatial voice's volume falls off with its distance to the listener. The distance is clamped to
	// [MinDistance, MaxDistance] before the curve is evaluated (OpenAL's "clamped" models), so voices play at full
	// volume inside MinDistance and stop getting quieter beyond MaxDistance.
	enum class AudioAttenuationModel : uint8_t
	{
		NoAttenuation = 0, // Constant volume at any distance; still panned and Doppler shifted. (`None` is an X11 macro.)
		Inverse,           // MinDistance / (MinDistance + Rolloff * (distance - MinDistance)). Physically plausible; the default.
		Linear,            // 1 - Rolloff * (distance - MinDistance) / (MaxDistance - MinDistance). Silent at MaxDistance with Rolloff 1.
		Exponential        // (distance / MinDistance) ^ -Rolloff
	};

	// Playback parameters of an AudioSource (also used for one-shot voices).
	struct AudioSourceSettings
	{
		float Volume = 1.0f; // Linear gain, >= 0
		float Pitch = 1.0f;  // Playback rate, > 0. 2 plays twice as fast and one octave higher.
		bool Looping = false;
		// Spatial voices are positioned in the world relative to the listener (attenuated, panned, Doppler shifted).
		// Non-spatial (2D) voices play as-is, e.g. music and UI sounds.
		bool Spatial = false;
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Velocity = glm::vec3(0.0f); // World units per second; only used for the Doppler effect
		float MinDistance = 1.0f;
		float MaxDistance = 500.0f; // Values below MinDistance behave like MinDistance
		float Rolloff = 1.0f;       // Steepness of the attenuation curve, >= 0
		AudioAttenuationModel AttenuationModel = AudioAttenuationModel::Inverse;
	};

	// A voice that plays an AudioClip with its own volume, pitch, looping and 3D settings. Scene components drive
	// sources; hold them in a Scope or Ref (they are neither copyable nor movable).
	//
	// Sources can be created, configured and destroyed whether or not the AudioEngine is initialized. Every setting
	// is remembered and applied when the underlying mixer voice is created, which happens when a clip is set while
	// the engine is running, or on the first Play() after the engine starts. AudioEngine::Shutdown releases the mixer
	// voice of every live source: the source keeps its clip and settings, reports itself stopped at the beginning
	// and ignores playback calls until Play() is called on a newly initialized engine.
	//
	// Main thread only, like the rest of the audio API. Mixing happens on miniaudio's audio thread, which reads the
	// voice's parameters through lock-free state.
	class AudioSource
	{
	public:
		AudioSource();
		~AudioSource();

		AudioSource(const AudioSource&) = delete;
		AudioSource& operator=(const AudioSource&) = delete;

		// Sets the clip to play (nullptr clears it) and keeps it alive. Replacing the clip stops playback and rewinds;
		// setting the current clip again changes nothing. Returns false (and leaves the source without a clip) if the
		// mixer could not create a voice for the clip.
		bool SetClip(const Ref<AudioClip>& clip);
		const Ref<AudioClip>& GetClip() const { return m_Clip; }

		// Starts playback from the current position: resumes a paused source, starts a stopped one from the
		// beginning (or from the position set with Seek), and restarts a source whose clip has finished. Has no
		// effect on a source that is already playing, without a clip, or while the engine is not initialized.
		void Play();
		// Halts playback and keeps the position; Play() resumes.
		void Pause();
		// Halts playback and rewinds to the beginning.
		void Stop();
		// True from Play() until the source is paused, stopped or reaches the end of a non-looping clip. Pausing the
		// whole engine (AudioEngine::SetPaused) does not change this.
		bool IsPlaying() const;
		// True from Pause() until Play(), Stop() or a clip change.
		bool IsPaused() const { return m_Paused; }

		void SetVolume(float volume);
		float GetVolume() const { return m_Settings.Volume; }
		void SetPitch(float pitch);
		float GetPitch() const { return m_Settings.Pitch; }
		void SetLooping(bool looping);
		bool IsLooping() const { return m_Settings.Looping; }
		void SetSpatial(bool spatial);
		bool IsSpatial() const { return m_Settings.Spatial; }

		void SetPosition(const glm::vec3& position);
		const glm::vec3& GetPosition() const { return m_Settings.Position; }
		void SetVelocity(const glm::vec3& velocity);
		const glm::vec3& GetVelocity() const { return m_Settings.Velocity; }
		void SetMinDistance(float distance);
		float GetMinDistance() const { return m_Settings.MinDistance; }
		void SetMaxDistance(float distance);
		float GetMaxDistance() const { return m_Settings.MaxDistance; }
		void SetRolloff(float rolloff);
		float GetRolloff() const { return m_Settings.Rolloff; }
		void SetAttenuationModel(AudioAttenuationModel model);
		AudioAttenuationModel GetAttenuationModel() const { return m_Settings.AttenuationModel; }

		// Applies every field through the individual setters (with the same validation).
		void SetSettings(const AudioSourceSettings& settings);
		const AudioSourceSettings& GetSettings() const { return m_Settings; }

		// Moves the playback position, clamped to the clip's length. Takes effect immediately when playing, and
		// otherwise becomes the position the next Play() starts from. Ignored without a clip.
		void Seek(float seconds);
		// Seconds into the clip. While playing this is the position the mixer has read up to, which runs ahead of the
		// audible output by at most one mixing period.
		float GetPlaybackPosition() const;
	private:
		bool EnsureVoice();
		// Replaces a voice that played to its end with a fresh one at m_StartPosition. Returns false if none could be created.
		bool ReplaceFinishedVoice();
		void ReleaseVoice();
		void ResetVoice(); // Moves a voice that is not playing to m_StartPosition
		void ApplyAttenuation();
		uint64_t SecondsToFrames(float seconds) const;

		Ref<AudioClip> m_Clip;
		Scope<AudioVoice> m_Voice; // Mixer voice; exists only while the engine is initialized and a clip is set
		AudioSourceSettings m_Settings;
		bool m_Paused = false;
		float m_StartPosition = 0.0f; // Seconds; where the voice starts when it is created (Seek without a voice)

		// Links of the engine's intrusive list of live sources (see AudioEngine.cpp).
		AudioSource* m_PreviousSource = nullptr;
		AudioSource* m_NextSource = nullptr;

		friend class AudioEngine;
	};

}
