#include "Editor/AudioPreview.h"

#include <Strata/Audio/AudioClip.h>
#include <Strata/Audio/AudioEngine.h>
#include <Strata/Audio/AudioSource.h>
#include <Strata/Core/JobSystem.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <span>

namespace Strata
{

	struct AudioPreview::WaveformJob
	{
		std::atomic<bool> Done = false;
		AudioWaveform Result;
	};

	AudioPreview::AudioPreview() = default;

	AudioPreview::~AudioPreview()
	{
		Stop();
	}

	bool AudioPreview::Play(AssetHandle handle, const Ref<AudioClip>& clip)
	{
		Stop();
		if (!clip || !AudioEngine::IsInitialized())
			return false;
		m_Voice = CreateScope<AudioSource>();
		// A plain voice: not spatial, so it sounds the same wherever the listener is.
		m_Voice->SetSpatial(false);
		if (!m_Voice->SetClip(clip))
		{
			m_Voice.reset();
			return false;
		}
		m_Voice->Play();
		m_Playing = handle;
		return true;
	}

	void AudioPreview::Stop()
	{
		m_Voice.reset();
		m_Playing = UUID::Null();
	}

	bool AudioPreview::IsPlaying(AssetHandle handle) const
	{
		return m_Voice && m_Playing == handle && m_Voice->IsPlaying();
	}

	float AudioPreview::GetPosition() const
	{
		return m_Voice && m_Voice->IsPlaying() ? m_Voice->GetPlaybackPosition() : 0.0f;
	}

	const AudioWaveform* AudioPreview::RequestWaveform(AssetHandle handle, const Ref<AudioClip>& clip, uint32_t columns)
	{
		if (!clip || columns == 0)
			return nullptr;
		if (handle != m_WaveformClip || columns != m_WaveformColumns)
		{
			m_WaveformClip = handle;
			m_WaveformColumns = columns;
			m_Waveform.reset();
			// A job still running for the clip shown before finishes on its own; its result is dropped with it.
			m_WaveformJob = std::make_shared<WaveformJob>();
			JobSystem::Submit([job = m_WaveformJob, clip, columns]()
			{
				job->Result = ComputeWaveform(*clip, columns);
				job->Done.store(true, std::memory_order_release);
			}, JobPriority::Low);
		}
		if (m_WaveformJob && m_WaveformJob->Done.load(std::memory_order_acquire))
		{
			m_Waveform = CreateScope<AudioWaveform>(std::move(m_WaveformJob->Result));
			m_WaveformJob.reset();
		}
		return m_Waveform && !m_Waveform->Min.empty() ? m_Waveform.get() : nullptr;
	}

	AudioWaveform AudioPreview::ComputeWaveform(const AudioClip& clip, uint32_t columns)
	{
		AudioWaveform waveform;
		waveform.Length = clip.GetLength();
		if (columns == 0 || clip.GetFrameCount() == 0 || clip.GetChannels() == 0 || waveform.Length > c_MaxWaveformSeconds)
			return waveform;

		// A streamed clip keeps only its encoded bytes: decoded whole here, on the job.
		Ref<AudioClip> decoded;
		std::span<const float> samples = clip.GetSamples();
		if (samples.empty())
		{
			const std::span<const uint8_t> encoded = clip.GetEncodedData();
			decoded = AudioClip::LoadFromMemory(std::vector<uint8_t>(encoded.begin(), encoded.end()), clip.GetDebugName(), AudioClipLoadMode::Decompressed);
			if (!decoded)
				return waveform;
			samples = decoded->GetSamples();
		}
		const uint64_t channels = clip.GetChannels();
		const uint64_t frames = samples.size() / channels;
		if (frames == 0)
			return waveform;

		waveform.Min.assign(columns, 0.0f);
		waveform.Max.assign(columns, 0.0f);
		// Each column looks at its share of the clip, but at no fewer than c_MinWindowFrames around it: in a short clip a
		// share of a few frames would catch a tone's cycle partly and draw an envelope that jumps.
		constexpr uint64_t c_MinWindowFrames = 64;
		const uint64_t window = std::min(frames, std::max<uint64_t>(frames / columns, c_MinWindowFrames));
		for (uint32_t column = 0; column < columns; column++)
		{
			const uint64_t center = (frames * (2 * static_cast<uint64_t>(column) + 1)) / (2 * static_cast<uint64_t>(columns));
			const uint64_t begin = std::min(center - std::min(center, window / 2), frames - window);
			const uint64_t end = begin + window;
			float low = std::numeric_limits<float>::max();
			float high = std::numeric_limits<float>::lowest();
			for (uint64_t sample = begin * channels; sample < std::min(end, frames) * channels; sample++)
			{
				low = std::min(low, samples[sample]);
				high = std::max(high, samples[sample]);
			}
			if (low > high)
				low = high = 0.0f;
			waveform.Min[column] = std::clamp(low, -1.0f, 1.0f);
			waveform.Max[column] = std::clamp(high, -1.0f, 1.0f);
		}
		return waveform;
	}

}
