#pragma once

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Core/Base.h>
#include <Strata/Core/UUID.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace Strata
{

	class AudioClip;
	class AudioSource;

	// The shape of a sound for a preview: per column, the lowest and highest sample of any channel in its share of the clip
	// (in [-1, 1]).
	struct AudioWaveform
	{
		std::vector<float> Min;
		std::vector<float> Max;
		float Length = 0.0f; // Seconds
	};

	// Listening to an audio clip in the editor (the Asset tab): one preview voice of the AudioEngine that belongs to no scene,
	// so playing, pausing or stopping the game never touches it, and the clip's waveform, computed on a job. Main thread.
	class AudioPreview
	{
	public:
		// Clips longer than this get no waveform: a streamed clip is decoded whole to compute it.
		static constexpr float c_MaxWaveformSeconds = 600.0f;

		AudioPreview();
		~AudioPreview();

		AudioPreview(const AudioPreview&) = delete;
		AudioPreview& operator=(const AudioPreview&) = delete;

		// Plays the clip from its start on the preview voice, stopping what it played. False without audio output (the engine
		// is not initialized) or for a null clip.
		bool Play(AssetHandle handle, const Ref<AudioClip>& clip);
		void Stop();
		// Whether the voice plays this asset's clip now (it stops at the clip's end).
		bool IsPlaying(AssetHandle handle) const;
		// Seconds into the clip the voice plays; 0 when it plays nothing.
		float GetPosition() const;

		// The clip's waveform with `columns` columns: started on a job the first time it is asked for (for another clip or
		// column count, again), null until it is ready and for clips that get none (see c_MaxWaveformSeconds).
		const AudioWaveform* RequestWaveform(AssetHandle handle, const Ref<AudioClip>& clip, uint32_t columns);

		// The waveform of a clip (decompressed: its samples; streamed: decoded whole first); empty Min and Max for clips
		// without frames or longer than c_MaxWaveformSeconds. Thread-safe.
		static AudioWaveform ComputeWaveform(const AudioClip& clip, uint32_t columns);
	private:
		// What the job fills; the preview takes it once Done is set.
		struct WaveformJob;

		Scope<AudioSource> m_Voice;
		AssetHandle m_Playing = UUID::Null();
		AssetHandle m_WaveformClip = UUID::Null();
		uint32_t m_WaveformColumns = 0;
		std::shared_ptr<WaveformJob> m_WaveformJob;
		Scope<AudioWaveform> m_Waveform;
	};

}
