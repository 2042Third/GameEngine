#include "stpch.h"
#include "Strata/Audio/AudioEngine.h"

#include "Strata/Audio/AudioClip.h"
#include "Strata/Audio/AudioSource.h"
#include "Strata/Audio/AudioVoice.h"
#include "Strata/Core/SequenceLock.h"

#include <miniaudio.h>

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_MinSampleRate = 8000;
		constexpr uint32_t c_MaxSampleRate = 384000;
		constexpr ma_uint32 c_ListenerIndex = 0;
		constexpr float c_MinDirectionLengthSquared = 1e-12f;
		// AdvanceNullDevice mixes in chunks of this many frames, and at most this much time per call (a longer hitch drops the
		// backlog, like an output device that underruns).
		constexpr uint64_t c_NullDeviceChunkFrames = 1024;
		constexpr double c_MaxNullDeviceAdvance = 1.0;

		// Engine-wide settings, kept while the engine is not initialized so the next Init can apply them.
		struct AudioEngineSettings
		{
			float MasterVolume = 1.0f;
			bool Paused = false;
			// miniaudio's listener defaults (right-handed, facing -Z with +Y up) already match the engine's
			// conventions; Init still applies these values explicitly.
			glm::vec3 ListenerPosition = glm::vec3(0.0f);
			glm::vec3 ListenerForward = glm::vec3(0.0f, 0.0f, -1.0f);
			glm::vec3 ListenerUp = glm::vec3(0.0f, 1.0f, 0.0f);
			glm::vec3 ListenerVelocity = glm::vec3(0.0f);
		};

		// Exists while the engine is initialized. Heap-allocated because miniaudio keeps pointers into ma_engine.
		struct AudioEngineData
		{
			ma_engine Engine;
			// The listener's world up vector on its way to the thread that mixes: miniaudio stores it without atomics and reads
			// it while mixing, so only that thread writes it (ApplyPendingWorldUp, after each mixing period).
			SequenceLockedValue<glm::vec3> WorldUp;
			bool NullDevice = false;
			uint32_t MaxOneShots = 0;
			std::vector<Scope<AudioVoice>> OneShots; // Oldest first
			double PendingNullDeviceFrames = 0.0;    // Fraction of a frame AdvanceNullDevice has not mixed yet
			std::vector<float> DiscardedFrames;      // Scratch output of AdvanceNullDevice
		};

		AudioEngineData* s_Data = nullptr;
		AudioEngineSettings s_Settings;

		// Live sources form an intrusive doubly linked list through AudioSource::m_PreviousSource/m_NextSource.
		// Plain pointers have no destructor, so sources with static storage duration can still unregister safely
		// while the program exits, whatever the order of static destruction.
		AudioSource* s_FirstSource = nullptr;
		uint32_t s_SourceCount = 0;

		// miniaudio's onProcess callback, called by the thread that mixes (the device's audio thread, or the caller of
		// ReadFrames for the null device) after each mixing period: the only place the world up vector is written.
		void ApplyPendingWorldUp(void* userData, float*, ma_uint64)
		{
			AudioEngineData& data = *static_cast<AudioEngineData*>(userData);
			// A vector being written right now is taken after the next period.
			glm::vec3 up;
			if (data.WorldUp.TakeNew(up))
				ma_engine_listener_set_world_up(&data.Engine, c_ListenerIndex, up.x, up.y, up.z);
		}

		// Before the engine mixes (nothing reads the listener yet), so the world up vector may be written directly.
		void ApplyListener(ma_engine& engine)
		{
			const AudioEngineSettings& settings = s_Settings;
			ma_engine_listener_set_position(&engine, c_ListenerIndex, settings.ListenerPosition.x, settings.ListenerPosition.y, settings.ListenerPosition.z);
			ma_engine_listener_set_direction(&engine, c_ListenerIndex, settings.ListenerForward.x, settings.ListenerForward.y, settings.ListenerForward.z);
			ma_engine_listener_set_world_up(&engine, c_ListenerIndex, settings.ListenerUp.x, settings.ListenerUp.y, settings.ListenerUp.z);
			ma_engine_listener_set_velocity(&engine, c_ListenerIndex, settings.ListenerVelocity.x, settings.ListenerVelocity.y, settings.ListenerVelocity.z);
		}

		// Creates the mixer (with or without an output device), applies the engine settings and starts the device
		// unless the engine is paused. On failure the engine is left uninitialized.
		ma_result InitializeEngine(AudioEngineData& data, ma_engine_config config, bool useDevice)
		{
			ma_engine& engine = data.Engine;
			config.noDevice = useDevice ? MA_FALSE : MA_TRUE;
			config.onProcess = ApplyPendingWorldUp;
			config.pProcessUserData = &data;
			// Start the device only once the listener and volume are set. This also avoids ma_engine_init's
			// auto-start failure path, which does not uninitialize its internal resource manager.
			config.noAutoStart = MA_TRUE;

			ma_result result = ma_engine_init(&config, &engine);
			if (result != MA_SUCCESS)
				return result;

			result = ma_engine_set_volume(&engine, s_Settings.MasterVolume);
			if (result != MA_SUCCESS)
			{
				ma_engine_uninit(&engine);
				return result;
			}
			ApplyListener(engine);

			if (useDevice && !s_Settings.Paused)
			{
				result = ma_engine_start(&engine);
				if (result != MA_SUCCESS)
				{
					ma_engine_uninit(&engine);
					return result;
				}
			}
			return MA_SUCCESS;
		}

		void ReclaimFinishedOneShots()
		{
			std::erase_if(s_Data->OneShots, [](const Scope<AudioVoice>& voice) { return !voice->IsPlaying(); });
		}

		bool StartOneShot(const Ref<AudioClip>& clip, AudioSourceSettings settings, float volume, float pitch)
		{
			ST_PROFILE_FUNCTION();

			if (!s_Data)
				return false;
			if (!clip)
			{
				ST_CORE_WARN("AudioEngine: cannot play a one-shot without a clip");
				return false;
			}
			if (!std::isfinite(volume) || !std::isfinite(pitch) || pitch <= 0.0f)
			{
				ST_CORE_WARN("AudioEngine: invalid one-shot volume {} or pitch {} for '{}'", volume, pitch, clip->GetDebugName());
				return false;
			}
			settings.Volume = std::max(volume, 0.0f);
			settings.Pitch = pitch;

			std::vector<Scope<AudioVoice>>& oneShots = s_Data->OneShots;
			if (oneShots.size() >= s_Data->MaxOneShots)
			{
				ReclaimFinishedOneShots();
				// Voice stealing: cut off the oldest one-shot so the newest sound always plays.
				if (oneShots.size() >= s_Data->MaxOneShots)
					oneShots.erase(oneShots.begin());
			}

			Scope<AudioVoice> voice = AudioVoice::Create(&s_Data->Engine, clip, settings);
			if (!voice || !voice->Start())
				return false;

			oneShots.push_back(std::move(voice));
			return true;
		}

	}

	bool AudioEngine::Init(const AudioEngineSpecification& specification)
	{
		ST_PROFILE_FUNCTION();

		if (s_Data)
		{
			ST_CORE_WARN("AudioEngine::Init called while already initialized");
			return true;
		}

		if (specification.SampleRate < c_MinSampleRate || specification.SampleRate > c_MaxSampleRate)
		{
			ST_CORE_ERROR("AudioEngine: unsupported sample rate {} Hz (supported: {}-{} Hz)", specification.SampleRate, c_MinSampleRate, c_MaxSampleRate);
			return false;
		}
		if (specification.Channels == 0 || specification.Channels > MA_MAX_CHANNELS)
		{
			ST_CORE_ERROR("AudioEngine: unsupported channel count {} (supported: 1-{})", specification.Channels, MA_MAX_CHANNELS);
			return false;
		}

		Scope<AudioEngineData> data = CreateScope<AudioEngineData>();
		data->MaxOneShots = std::max(specification.MaxOneShots, 1u);

		ma_engine_config config = ma_engine_config_init();
		config.listenerCount = 1;
		config.channels = specification.Channels;
		config.sampleRate = specification.SampleRate;

		bool nullDevice = specification.NullDevice;
		if (!nullDevice)
		{
			const ma_result result = InitializeEngine(*data, config, true);
			if (result != MA_SUCCESS)
			{
				ST_CORE_WARN("AudioEngine: no usable audio output device ({}); continuing without audio output", ma_result_description(result));
				nullDevice = true;
			}
		}
		if (nullDevice)
		{
			const ma_result result = InitializeEngine(*data, config, false);
			if (result != MA_SUCCESS)
			{
				ST_CORE_ERROR("AudioEngine: failed to create the mixer ({})", ma_result_description(result));
				return false;
			}
		}
		data->NullDevice = nullDevice;
		s_Data = data.release();

		if (nullDevice)
		{
			ST_CORE_INFO("AudioEngine: mixing without an output device ({} Hz, {} channels)", GetSampleRate(), GetChannelCount());
		}
		else
		{
			const ma_device* device = ma_engine_get_device(&s_Data->Engine);
			ST_CORE_INFO("AudioEngine: playing on '{}' ({} Hz, {} channels)", device->playback.name, GetSampleRate(), GetChannelCount());
		}
		return true;
	}

	void AudioEngine::Shutdown()
	{
		ST_PROFILE_FUNCTION();

		if (s_Data)
		{
			// Stop the device first so the audio thread is idle while voices are torn down. Releasing voices is
			// safe while mixing too, so a failure here only costs some extra synchronization.
			if (!s_Data->NullDevice && !s_Settings.Paused)
			{
				const ma_result result = ma_engine_stop(&s_Data->Engine);
				if (result != MA_SUCCESS)
					ST_CORE_WARN("AudioEngine: failed to stop the output device ({})", ma_result_description(result));
			}

			// Every voice must be released before the engine whose node graph it belongs to.
			for (AudioSource* source = s_FirstSource; source; source = source->m_NextSource)
				source->ReleaseVoice();
			s_Data->OneShots.clear();

			ma_engine_uninit(&s_Data->Engine);
			delete s_Data;
			s_Data = nullptr;
			ST_CORE_INFO("AudioEngine: shut down");
		}

		s_Settings = AudioEngineSettings();
	}

	bool AudioEngine::IsInitialized()
	{
		return s_Data != nullptr;
	}

	bool AudioEngine::IsNullDevice()
	{
		return s_Data && s_Data->NullDevice;
	}

	uint32_t AudioEngine::GetSampleRate()
	{
		return s_Data ? ma_engine_get_sample_rate(&s_Data->Engine) : 0;
	}

	uint32_t AudioEngine::GetChannelCount()
	{
		return s_Data ? ma_engine_get_channels(&s_Data->Engine) : 0;
	}

	void AudioEngine::SetMasterVolume(float volume)
	{
		if (!std::isfinite(volume))
		{
			ST_CORE_WARN("AudioEngine::SetMasterVolume: ignoring non-finite volume");
			return;
		}

		s_Settings.MasterVolume = std::max(volume, 0.0f);
		if (!s_Data)
			return;

		const ma_result result = ma_engine_set_volume(&s_Data->Engine, s_Settings.MasterVolume);
		if (result != MA_SUCCESS)
			ST_CORE_ERROR("AudioEngine: failed to set the master volume ({})", ma_result_description(result));
	}

	float AudioEngine::GetMasterVolume()
	{
		return s_Settings.MasterVolume;
	}

	void AudioEngine::SetPaused(bool paused)
	{
		if (paused == s_Settings.Paused)
			return;

		// A real device is stopped outright, so a paused engine costs nothing. The null device has no audio thread;
		// ReadFrames outputs silence instead.
		if (s_Data && !s_Data->NullDevice)
		{
			const ma_result result = paused ? ma_engine_stop(&s_Data->Engine) : ma_engine_start(&s_Data->Engine);
			if (result != MA_SUCCESS)
			{
				ST_CORE_ERROR("AudioEngine: failed to {} the output device ({})", paused ? "stop" : "start", ma_result_description(result));
				return;
			}
		}
		s_Settings.Paused = paused;
	}

	bool AudioEngine::IsPaused()
	{
		return s_Settings.Paused;
	}

	void AudioEngine::SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up, const glm::vec3& velocity)
	{
		if (!AudioUtils::IsFinite(position) || !AudioUtils::IsFinite(forward) || !AudioUtils::IsFinite(up) || !AudioUtils::IsFinite(velocity))
		{
			ST_CORE_WARN("AudioEngine::SetListener: ignoring non-finite listener state");
			return;
		}
		// miniaudio normalizes both vectors; (near-)zero vectors would turn the whole mix into NaNs.
		if (glm::dot(forward, forward) < c_MinDirectionLengthSquared || glm::dot(up, up) < c_MinDirectionLengthSquared)
		{
			ST_CORE_WARN("AudioEngine::SetListener: forward and up must not be zero vectors");
			return;
		}

		const bool upChanged = up != s_Settings.ListenerUp;
		s_Settings.ListenerPosition = position;
		s_Settings.ListenerForward = forward;
		s_Settings.ListenerUp = up;
		s_Settings.ListenerVelocity = velocity;
		if (!s_Data)
			return;

		// Position, direction and velocity are atomics in miniaudio; the up vector is not, so the mixing thread applies it.
		ma_engine& engine = s_Data->Engine;
		ma_engine_listener_set_position(&engine, c_ListenerIndex, position.x, position.y, position.z);
		ma_engine_listener_set_direction(&engine, c_ListenerIndex, forward.x, forward.y, forward.z);
		ma_engine_listener_set_velocity(&engine, c_ListenerIndex, velocity.x, velocity.y, velocity.z);
		if (upChanged)
			s_Data->WorldUp.Publish(up);
	}

	AudioListenerState AudioEngine::GetListener()
	{
		return { s_Settings.ListenerPosition, s_Settings.ListenerForward, s_Settings.ListenerUp, s_Settings.ListenerVelocity };
	}

	std::optional<glm::vec3> AudioEngine::GetMixedListenerUp()
	{
		if (!s_Data || !s_Data->NullDevice)
			return std::nullopt;
		const ma_vec3f up = ma_engine_listener_get_world_up(&s_Data->Engine, c_ListenerIndex);
		return glm::vec3(up.x, up.y, up.z);
	}

	bool AudioEngine::PlayOneShot(const Ref<AudioClip>& clip, float volume, float pitch)
	{
		AudioSourceSettings settings;
		settings.Spatial = false;
		return StartOneShot(clip, settings, volume, pitch);
	}

	bool AudioEngine::PlayOneShotAt(const Ref<AudioClip>& clip, const glm::vec3& position, float volume, float pitch)
	{
		if (!AudioUtils::IsFinite(position))
		{
			ST_CORE_WARN("AudioEngine::PlayOneShotAt: ignoring non-finite position");
			return false;
		}

		AudioSourceSettings settings;
		settings.Spatial = true;
		settings.Position = position;
		return StartOneShot(clip, settings, volume, pitch);
	}

	void AudioEngine::Update()
	{
		ST_PROFILE_FUNCTION();

		if (s_Data)
			ReclaimFinishedOneShots();
	}

	uint64_t AudioEngine::ReadFrames(float* interleavedOutput, uint64_t frameCount)
	{
		ST_PROFILE_FUNCTION();

		if (!s_Data)
		{
			ST_CORE_ERROR("AudioEngine::ReadFrames: the engine is not initialized");
			return 0;
		}
		if (!s_Data->NullDevice)
		{
			ST_CORE_ERROR("AudioEngine::ReadFrames: only available with the null device (the output device pulls the mix itself)");
			return 0;
		}
		if (!interleavedOutput || frameCount == 0)
			return 0;

		const uint64_t sampleCount = frameCount * GetChannelCount();
		if (s_Settings.Paused)
		{
			std::fill_n(interleavedOutput, sampleCount, 0.0f);
			return frameCount;
		}

		// miniaudio reports zero frames read while no voice is attached to the mix, but it always pads the buffer
		// with silence, so on success the whole request has been written.
		const ma_result result = ma_engine_read_pcm_frames(&s_Data->Engine, interleavedOutput, frameCount, nullptr);
		if (result != MA_SUCCESS)
		{
			ST_CORE_ERROR("AudioEngine::ReadFrames: mixing failed ({})", ma_result_description(result));
			std::fill_n(interleavedOutput, sampleCount, 0.0f);
			return 0;
		}
		return frameCount;
	}

	void AudioEngine::AdvanceNullDevice(float seconds)
	{
		ST_PROFILE_FUNCTION();

		if (!s_Data || !s_Data->NullDevice || s_Settings.Paused || !std::isfinite(seconds) || seconds <= 0.0f)
			return;

		s_Data->PendingNullDeviceFrames += std::min(static_cast<double>(seconds), c_MaxNullDeviceAdvance) * static_cast<double>(GetSampleRate());
		uint64_t frames = static_cast<uint64_t>(s_Data->PendingNullDeviceFrames);
		s_Data->PendingNullDeviceFrames -= static_cast<double>(frames);

		s_Data->DiscardedFrames.resize(static_cast<size_t>(c_NullDeviceChunkFrames * GetChannelCount()));
		while (frames > 0)
		{
			const uint64_t chunk = std::min(frames, c_NullDeviceChunkFrames);
			if (ReadFrames(s_Data->DiscardedFrames.data(), chunk) != chunk)
				return; // Mixing failed (logged)
			frames -= chunk;
		}
	}

	AudioStats AudioEngine::GetStats()
	{
		AudioStats stats;
		stats.SourceCount = s_SourceCount;
		if (!s_Data)
			return stats;

		for (const Scope<AudioVoice>& voice : s_Data->OneShots)
		{
			stats.ActiveOneShots++;
			stats.AllocatedVoices++;
			if (voice->IsPlaying())
				stats.ActiveVoices++;
		}

		for (const AudioSource* source = s_FirstSource; source; source = source->m_NextSource)
		{
			if (!source->m_Voice)
				continue;
			stats.AllocatedVoices++;
			if (source->m_Voice->IsPlaying())
				stats.ActiveVoices++;
		}
		return stats;
	}

	void AudioEngine::RegisterSource(AudioSource* source)
	{
		source->m_PreviousSource = nullptr;
		source->m_NextSource = s_FirstSource;
		if (s_FirstSource)
			s_FirstSource->m_PreviousSource = source;
		s_FirstSource = source;
		s_SourceCount++;
	}

	void AudioEngine::UnregisterSource(AudioSource* source)
	{
		if (source->m_PreviousSource)
			source->m_PreviousSource->m_NextSource = source->m_NextSource;
		else
			s_FirstSource = source->m_NextSource;
		if (source->m_NextSource)
			source->m_NextSource->m_PreviousSource = source->m_PreviousSource;

		source->m_PreviousSource = nullptr;
		source->m_NextSource = nullptr;
		s_SourceCount--;
	}

	Scope<AudioVoice> AudioEngine::CreateVoice(const Ref<AudioClip>& clip, const AudioSourceSettings& settings)
	{
		if (!s_Data || !clip)
			return nullptr;
		return AudioVoice::Create(&s_Data->Engine, clip, settings);
	}

}
