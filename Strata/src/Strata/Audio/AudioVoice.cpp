#include "stpch.h"
#include "Strata/Audio/AudioVoice.h"

#include "Strata/Audio/AudioClip.h"

#include <miniaudio.h>

#include <cstddef>
#include <type_traits>

namespace Strata
{

	namespace
	{

		// The inverse and exponential curves divide by the minimum distance, so it must stay positive.
		constexpr float c_MinAttenuationDistance = 0.01f;

		// Per-voice miniaudio data source over a clip's shared data. Decompressed clips are read straight from the
		// clip's shared frames (no copy, no decoding); streamed clips are decoded by this voice's private decoder.
		//
		// The cursor is mirrored in an atomic: the audio thread advances it while the main thread queries the playback
		// position (the decoder's own cursor is a plain integer, so reading it while mixing would be a data race).
		struct ClipDataSource
		{
			ma_data_source_base Base; // Must be first: miniaudio accesses every data source as an ma_data_source_base
			const float* Samples = nullptr; // Decompressed clips: the clip's interleaved frames
			ma_decoder* Decoder = nullptr;  // Streamed clips: the voice's decoder
			uint64_t FrameCount = 0;
			uint32_t Channels = 0;
			uint32_t SampleRate = 0;
			std::atomic<uint64_t> Cursor = 0;
		};

		static_assert(std::is_standard_layout_v<ClipDataSource> && offsetof(ClipDataSource, Base) == 0, "miniaudio requires the data source base at offset 0");
		static_assert(std::atomic<uint64_t>::is_always_lock_free, "The audio thread must never block on the playback cursor");

		ClipDataSource& GetClipDataSource(ma_data_source* dataSource)
		{
			return *static_cast<ClipDataSource*>(dataSource);
		}

		// Audio thread (or the main thread while the voice is not being mixed).
		ma_result ReadClipDataSource(ma_data_source* dataSource, void* framesOut, ma_uint64 frameCount, ma_uint64* framesRead)
		{
			ClipDataSource& source = GetClipDataSource(dataSource);
			const uint64_t cursor = source.Cursor.load(std::memory_order_relaxed);

			ma_result result = MA_SUCCESS;
			ma_uint64 framesAdvanced = 0;
			if (source.Decoder)
			{
				// Decoding from memory cannot fail transiently, so treat a decoding error (corrupt data past the
				// header) as the end of the clip: the voice then finishes instead of playing silence forever.
				result = ma_decoder_read_pcm_frames(source.Decoder, framesOut, frameCount, &framesAdvanced);
				if (result != MA_SUCCESS)
					result = MA_AT_END;
			}
			else
			{
				const uint64_t framesAvailable = cursor < source.FrameCount ? source.FrameCount - cursor : 0;
				framesAdvanced = std::min<uint64_t>(frameCount, framesAvailable);
				// A null output buffer asks for a forward seek.
				if (framesOut && framesAdvanced > 0)
					std::memcpy(framesOut, source.Samples + cursor * source.Channels, static_cast<size_t>(framesAdvanced * source.Channels * sizeof(float)));
			}

			// Reporting the end together with the final frames lets a looping voice wrap around within the same read.
			if (result == MA_SUCCESS && framesAdvanced < frameCount)
				result = MA_AT_END;

			source.Cursor.store(cursor + framesAdvanced, std::memory_order_relaxed);
			if (framesRead)
				*framesRead = framesAdvanced;
			return result;
		}

		ma_result SeekClipDataSource(ma_data_source* dataSource, ma_uint64 frameIndex)
		{
			ClipDataSource& source = GetClipDataSource(dataSource);
			if (frameIndex > source.FrameCount)
				return MA_BAD_SEEK;

			if (source.Decoder)
			{
				const ma_result result = ma_decoder_seek_to_pcm_frame(source.Decoder, frameIndex);
				if (result != MA_SUCCESS)
					return result;
			}

			source.Cursor.store(frameIndex, std::memory_order_relaxed);
			return MA_SUCCESS;
		}

		ma_result GetClipDataSourceFormat(ma_data_source* dataSource, ma_format* format, ma_uint32* channels, ma_uint32* sampleRate, ma_channel* channelMap, size_t channelMapCapacity)
		{
			const ClipDataSource& source = GetClipDataSource(dataSource);
			if (format)
				*format = ma_format_f32;
			if (channels)
				*channels = source.Channels;
			if (sampleRate)
				*sampleRate = source.SampleRate;
			if (channelMap)
				ma_channel_map_init_standard(ma_standard_channel_map_default, channelMap, channelMapCapacity, source.Channels);
			return MA_SUCCESS;
		}

		ma_result GetClipDataSourceCursor(ma_data_source* dataSource, ma_uint64* cursor)
		{
			if (!cursor)
				return MA_INVALID_ARGS;
			*cursor = GetClipDataSource(dataSource).Cursor.load(std::memory_order_relaxed);
			return MA_SUCCESS;
		}

		ma_result GetClipDataSourceLength(ma_data_source* dataSource, ma_uint64* length)
		{
			if (!length)
				return MA_INVALID_ARGS;
			*length = GetClipDataSource(dataSource).FrameCount;
			return MA_SUCCESS;
		}

		constexpr ma_data_source_vtable c_ClipDataSourceVTable = {
			ReadClipDataSource,
			SeekClipDataSource,
			GetClipDataSourceFormat,
			GetClipDataSourceCursor,
			GetClipDataSourceLength,
			nullptr, // onSetLooping: ma_data_source_read_pcm_frames implements looping by seeking back to the start
			0        // flags
		};

	}

	struct AudioVoice::Data
	{
		Ref<AudioClip> Clip;
		ClipDataSource Source;
		ma_decoder Decoder;
		ma_sound Sound;
		bool SourceInitialized = false;
		bool DecoderInitialized = false;
		bool SoundInitialized = false;
		bool Started = false;

		Data() = default;
		Data(const Data&) = delete;
		Data& operator=(const Data&) = delete;

		~Data()
		{
			// Uninitializing the sound detaches it from the node graph and waits for the audio thread to finish
			// with it; only then may the data source and decoder it reads from be released.
			if (SoundInitialized)
				ma_sound_uninit(&Sound);
			if (DecoderInitialized)
				ma_decoder_uninit(&Decoder);
			if (SourceInitialized)
				ma_data_source_uninit(&Source);
		}
	};

	AudioVoice::AudioVoice()
		: m_Data(CreateScope<Data>())
	{
	}

	AudioVoice::~AudioVoice() = default;

	Scope<AudioVoice> AudioVoice::Create(ma_engine* engine, const Ref<AudioClip>& clip, const AudioSourceSettings& settings)
	{
		ST_PROFILE_FUNCTION();
		ST_CORE_ASSERT(engine, "AudioVoice::Create: engine is null");
		ST_CORE_ASSERT(clip, "AudioVoice::Create: clip is null");

		// The constructor is private (voices are only created here), which rules out CreateScope.
		Scope<AudioVoice> voice(new AudioVoice());
		Data& data = *voice->m_Data;
		data.Clip = clip;

		ma_data_source_config sourceConfig = ma_data_source_config_init();
		sourceConfig.vtable = &c_ClipDataSourceVTable;
		ma_result result = ma_data_source_init(&sourceConfig, &data.Source);
		if (result != MA_SUCCESS)
		{
			ST_CORE_ERROR("AudioVoice: failed to create a data source for '{}' ({})", clip->GetDebugName(), ma_result_description(result));
			return nullptr;
		}
		data.SourceInitialized = true;
		data.Source.FrameCount = clip->GetFrameCount();
		data.Source.Channels = clip->GetChannels();
		data.Source.SampleRate = clip->GetSampleRate();

		if (clip->GetLoadMode() == AudioClipLoadMode::Streamed)
		{
			// Request the format the clip reported when it was loaded, so streamed and decompressed voices of the
			// same file produce identical data.
			const std::span<const uint8_t> encodedData = clip->GetEncodedData();
			const ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, clip->GetChannels(), clip->GetSampleRate());
			result = ma_decoder_init_memory(encodedData.data(), encodedData.size(), &decoderConfig, &data.Decoder);
			if (result != MA_SUCCESS)
			{
				ST_CORE_ERROR("AudioVoice: failed to create a decoder for '{}' ({})", clip->GetDebugName(), ma_result_description(result));
				return nullptr;
			}
			data.DecoderInitialized = true;
			data.Source.Decoder = &data.Decoder;
		}
		else
		{
			data.Source.Samples = clip->GetSamples().data();
		}

		// Sounds are created stopped, so the audio thread does not mix this one before its settings are applied.
		result = ma_sound_init_from_data_source(engine, &data.Source, 0, nullptr, &data.Sound);
		if (result != MA_SUCCESS)
		{
			ST_CORE_ERROR("AudioVoice: failed to create a mixer voice for '{}' ({})", clip->GetDebugName(), ma_result_description(result));
			return nullptr;
		}
		data.SoundInitialized = true;

		voice->SetVolume(settings.Volume);
		voice->SetPitch(settings.Pitch);
		voice->SetLooping(settings.Looping);
		voice->SetSpatial(settings.Spatial);
		voice->SetPosition(settings.Position);
		voice->SetVelocity(settings.Velocity);
		voice->SetAttenuation(settings.AttenuationModel, settings.MinDistance, settings.MaxDistance, settings.Rolloff);
		return voice;
	}

	const Ref<AudioClip>& AudioVoice::GetClip() const
	{
		return m_Data->Clip;
	}

	bool AudioVoice::Start()
	{
		ma_sound& sound = m_Data->Sound;

		// A voice that played to its end stays started until the audio thread processes it once more, and that pass stops
		// it. A restart from here (ma_sound_start rewinds it and marks it started) can be undone by that deferred stop, and
		// would be lost silently, so finished voices are replaced instead (see AudioSource::Play). The audio thread may
		// mark the voice finished at any moment, including right after a caller checked HasEnded: then this returns false.
		if (ma_sound_at_end(&sound))
			return false;

		const ma_result result = ma_sound_start(&sound);
		if (result != MA_SUCCESS)
		{
			ST_CORE_ERROR("AudioVoice: failed to start '{}' ({})", m_Data->Clip->GetDebugName(), ma_result_description(result));
			return false;
		}
		m_Data->Started = true;
		return true;
	}

	void AudioVoice::Stop()
	{
		const ma_result result = ma_sound_stop(&m_Data->Sound);
		if (result != MA_SUCCESS)
			ST_CORE_ERROR("AudioVoice: failed to stop '{}' ({})", m_Data->Clip->GetDebugName(), ma_result_description(result));
	}

	bool AudioVoice::IsPlaying() const
	{
		const ma_sound& sound = m_Data->Sound;
		return ma_sound_is_playing(&sound) && !ma_sound_at_end(&sound);
	}

	bool AudioVoice::HasEnded() const
	{
		return ma_sound_at_end(&m_Data->Sound);
	}

	bool AudioVoice::HasStarted() const
	{
		return m_Data->Started;
	}

	void AudioVoice::SeekToFrame(uint64_t frame)
	{
		const ma_result result = ma_sound_seek_to_pcm_frame(&m_Data->Sound, std::min(frame, m_Data->Source.FrameCount));
		if (result != MA_SUCCESS)
			ST_CORE_ERROR("AudioVoice: failed to seek '{}' ({})", m_Data->Clip->GetDebugName(), ma_result_description(result));
	}

	uint64_t AudioVoice::GetCursorInFrames() const
	{
		ma_uint64 cursor = 0;
		const ma_result result = ma_sound_get_cursor_in_pcm_frames(&m_Data->Sound, &cursor);
		if (result != MA_SUCCESS)
		{
			ST_CORE_ERROR("AudioVoice: failed to query the position of '{}' ({})", m_Data->Clip->GetDebugName(), ma_result_description(result));
			return 0;
		}
		return std::min<uint64_t>(cursor, m_Data->Source.FrameCount);
	}

	void AudioVoice::SetVolume(float volume)
	{
		// The volume of the voice's output bus, applied by the node graph, is atomic; ma_sound_set_volume writes a plain
		// float the audio thread reads while mixing.
		const ma_result result = ma_node_set_output_bus_volume(&m_Data->Sound, 0, volume);
		if (result != MA_SUCCESS)
			ST_CORE_ERROR("AudioVoice: failed to set the volume of '{}' ({})", m_Data->Clip->GetDebugName(), ma_result_description(result));
	}

	void AudioVoice::SetPitch(float pitch)
	{
		ma_sound_set_pitch(&m_Data->Sound, pitch);
	}

	void AudioVoice::SetLooping(bool looping)
	{
		ma_sound_set_looping(&m_Data->Sound, looping ? MA_TRUE : MA_FALSE);
	}

	void AudioVoice::SetSpatial(bool spatial)
	{
		ma_sound_set_spatialization_enabled(&m_Data->Sound, spatial ? MA_TRUE : MA_FALSE);
	}

	void AudioVoice::SetPosition(const glm::vec3& position)
	{
		ma_sound_set_position(&m_Data->Sound, position.x, position.y, position.z);
	}

	void AudioVoice::SetVelocity(const glm::vec3& velocity)
	{
		ma_sound_set_velocity(&m_Data->Sound, velocity.x, velocity.y, velocity.z);
	}

	void AudioVoice::SetAttenuation(AudioAttenuationModel model, float minDistance, float maxDistance, float rolloff)
	{
		ma_attenuation_model attenuationModel = ma_attenuation_model_inverse;
		float effectiveRolloff = std::max(rolloff, 0.0f);
		switch (model)
		{
			case AudioAttenuationModel::NoAttenuation:
				// miniaudio's own "none" model also disables panning and the Doppler effect. Inverse attenuation with
				// zero rolloff keeps the gain at 1 at every distance while the voice stays fully spatialized.
				attenuationModel = ma_attenuation_model_inverse;
				effectiveRolloff = 0.0f;
				break;
			case AudioAttenuationModel::Inverse: attenuationModel = ma_attenuation_model_inverse; break;
			case AudioAttenuationModel::Linear: attenuationModel = ma_attenuation_model_linear; break;
			case AudioAttenuationModel::Exponential: attenuationModel = ma_attenuation_model_exponential; break;
		}

		const float effectiveMinDistance = std::max(minDistance, c_MinAttenuationDistance);
		ma_sound& sound = m_Data->Sound;
		ma_sound_set_attenuation_model(&sound, attenuationModel);
		ma_sound_set_min_distance(&sound, effectiveMinDistance);
		ma_sound_set_max_distance(&sound, std::max(maxDistance, effectiveMinDistance));
		ma_sound_set_rolloff(&sound, effectiveRolloff);
	}

}
