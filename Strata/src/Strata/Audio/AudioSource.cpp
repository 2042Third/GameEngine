#include "stpch.h"
#include "Strata/Audio/AudioSource.h"

#include "Strata/Audio/AudioClip.h"
#include "Strata/Audio/AudioEngine.h"
#include "Strata/Audio/AudioVoice.h"

namespace Strata
{

	AudioSource::AudioSource()
	{
		AudioEngine::RegisterSource(this);
	}

	AudioSource::~AudioSource()
	{
		ReleaseVoice();
		AudioEngine::UnregisterSource(this);
	}

	bool AudioSource::SetClip(const Ref<AudioClip>& clip)
	{
		if (clip == m_Clip)
			return true;

		ReleaseVoice();
		m_Clip = clip;

		// Create the voice right away so that a failure is reported here instead of silently on Play().
		if (m_Clip && AudioEngine::IsInitialized() && !EnsureVoice())
		{
			m_Clip = nullptr;
			return false;
		}
		return true;
	}

	void AudioSource::Play()
	{
		if (!EnsureVoice())
			return;

		if (m_Voice->Start())
			m_Paused = false;
	}

	void AudioSource::Pause()
	{
		if (!m_Voice || !m_Voice->IsPlaying())
			return;

		m_Voice->Stop();
		m_Paused = true;
	}

	void AudioSource::Stop()
	{
		m_Paused = false;
		m_StartPosition = 0.0f;
		ResetVoice();
	}

	bool AudioSource::IsPlaying() const
	{
		return m_Voice && m_Voice->IsPlaying();
	}

	void AudioSource::SetVolume(float volume)
	{
		if (!std::isfinite(volume))
		{
			ST_CORE_WARN("AudioSource::SetVolume: ignoring non-finite volume");
			return;
		}

		m_Settings.Volume = std::max(volume, 0.0f);
		if (m_Voice)
			m_Voice->SetVolume(m_Settings.Volume);
	}

	void AudioSource::SetPitch(float pitch)
	{
		if (!std::isfinite(pitch) || pitch <= 0.0f)
		{
			ST_CORE_WARN("AudioSource::SetPitch: pitch must be positive and finite (got {})", pitch);
			return;
		}

		m_Settings.Pitch = pitch;
		if (m_Voice)
			m_Voice->SetPitch(pitch);
	}

	void AudioSource::SetLooping(bool looping)
	{
		m_Settings.Looping = looping;
		if (m_Voice)
			m_Voice->SetLooping(looping);
	}

	void AudioSource::SetSpatial(bool spatial)
	{
		m_Settings.Spatial = spatial;
		if (m_Voice)
			m_Voice->SetSpatial(spatial);
	}

	void AudioSource::SetPosition(const glm::vec3& position)
	{
		if (!AudioUtils::IsFinite(position))
		{
			ST_CORE_WARN("AudioSource::SetPosition: ignoring non-finite position");
			return;
		}

		m_Settings.Position = position;
		if (m_Voice)
			m_Voice->SetPosition(position);
	}

	void AudioSource::SetVelocity(const glm::vec3& velocity)
	{
		if (!AudioUtils::IsFinite(velocity))
		{
			ST_CORE_WARN("AudioSource::SetVelocity: ignoring non-finite velocity");
			return;
		}

		m_Settings.Velocity = velocity;
		if (m_Voice)
			m_Voice->SetVelocity(velocity);
	}

	void AudioSource::SetMinDistance(float distance)
	{
		if (!std::isfinite(distance))
		{
			ST_CORE_WARN("AudioSource::SetMinDistance: ignoring non-finite distance");
			return;
		}

		m_Settings.MinDistance = std::max(distance, 0.0f);
		ApplyAttenuation();
	}

	void AudioSource::SetMaxDistance(float distance)
	{
		if (!std::isfinite(distance))
		{
			ST_CORE_WARN("AudioSource::SetMaxDistance: ignoring non-finite distance");
			return;
		}

		m_Settings.MaxDistance = std::max(distance, 0.0f);
		ApplyAttenuation();
	}

	void AudioSource::SetRolloff(float rolloff)
	{
		if (!std::isfinite(rolloff))
		{
			ST_CORE_WARN("AudioSource::SetRolloff: ignoring non-finite rolloff");
			return;
		}

		m_Settings.Rolloff = std::max(rolloff, 0.0f);
		ApplyAttenuation();
	}

	void AudioSource::SetAttenuationModel(AudioAttenuationModel model)
	{
		m_Settings.AttenuationModel = model;
		ApplyAttenuation();
	}

	void AudioSource::SetSettings(const AudioSourceSettings& settings)
	{
		SetVolume(settings.Volume);
		SetPitch(settings.Pitch);
		SetLooping(settings.Looping);
		SetSpatial(settings.Spatial);
		SetPosition(settings.Position);
		SetVelocity(settings.Velocity);
		SetMinDistance(settings.MinDistance);
		SetMaxDistance(settings.MaxDistance);
		SetRolloff(settings.Rolloff);
		SetAttenuationModel(settings.AttenuationModel);
	}

	void AudioSource::Seek(float seconds)
	{
		if (!std::isfinite(seconds))
		{
			ST_CORE_WARN("AudioSource::Seek: ignoring non-finite position");
			return;
		}
		if (!m_Clip)
			return;

		const float position = std::clamp(seconds, 0.0f, m_Clip->GetLength());
		if (m_Voice && m_Voice->IsPlaying())
		{
			m_Voice->SeekToFrame(SecondsToFrames(position));
			return;
		}

		m_StartPosition = position;
		ResetVoice();
	}

	float AudioSource::GetPlaybackPosition() const
	{
		if (!m_Clip)
			return 0.0f;
		if (!m_Voice)
			return m_StartPosition;
		return static_cast<float>(static_cast<double>(m_Voice->GetCursorInFrames()) / static_cast<double>(m_Clip->GetSampleRate()));
	}

	bool AudioSource::EnsureVoice()
	{
		if (m_Voice)
			return true;
		if (!m_Clip)
			return false;

		m_Voice = AudioEngine::CreateVoice(m_Clip, m_Settings);
		if (!m_Voice)
			return false;

		if (m_StartPosition > 0.0f)
			m_Voice->SeekToFrame(SecondsToFrames(m_StartPosition));
		m_StartPosition = 0.0f;
		return true;
	}

	void AudioSource::ReleaseVoice()
	{
		m_Voice.reset();
		m_Paused = false;
		m_StartPosition = 0.0f;
	}

	void AudioSource::ResetVoice()
	{
		// Without a voice, m_StartPosition applies when one is created.
		if (!m_Voice)
			return;

		// A voice that has played holds up to one mixing period of audio it already read from its old position,
		// which it would play first when started again, so it is replaced by a fresh voice. A voice that never
		// played holds no audio yet and can simply be moved.
		if (m_Voice->HasStarted())
		{
			m_Voice.reset();
			EnsureVoice();
			return;
		}

		m_Voice->SeekToFrame(SecondsToFrames(m_StartPosition));
		m_StartPosition = 0.0f;
	}

	void AudioSource::ApplyAttenuation()
	{
		if (m_Voice)
			m_Voice->SetAttenuation(m_Settings.AttenuationModel, m_Settings.MinDistance, m_Settings.MaxDistance, m_Settings.Rolloff);
	}

	uint64_t AudioSource::SecondsToFrames(float seconds) const
	{
		const double frames = std::round(static_cast<double>(seconds) * static_cast<double>(m_Clip->GetSampleRate()));
		return std::min(static_cast<uint64_t>(std::max(frames, 0.0)), m_Clip->GetFrameCount());
	}

}
