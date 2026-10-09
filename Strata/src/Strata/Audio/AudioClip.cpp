#include "stpch.h"
#include "Strata/Audio/AudioClip.h"

#include "Strata/Core/FileSystem.h"

#include <miniaudio.h>

namespace Strata
{

	namespace
	{

		constexpr ma_uint64 c_DecodeChunkFrames = 16384;

		// Owns an ma_decoder for the duration of a load so every early return releases it.
		struct ScopedDecoder
		{
			ma_decoder Decoder;
			bool Initialized = false;

			ScopedDecoder() = default;
			ScopedDecoder(const ScopedDecoder&) = delete;
			ScopedDecoder& operator=(const ScopedDecoder&) = delete;

			~ScopedDecoder()
			{
				if (Initialized)
					ma_decoder_uninit(&Decoder);
			}
		};

		// Decodes every remaining frame. The buffer grows chunk by chunk instead of being sized from the stream's
		// declared length, because a corrupt header could otherwise request an arbitrarily large allocation.
		bool DecodeAllFrames(ma_decoder& decoder, uint32_t channels, std::vector<float>& outSamples, const std::string& debugName)
		{
			outSamples.clear();
			while (true)
			{
				const size_t offset = outSamples.size();
				outSamples.resize(offset + static_cast<size_t>(c_DecodeChunkFrames) * channels);

				ma_uint64 framesRead = 0;
				const ma_result result = ma_decoder_read_pcm_frames(&decoder, outSamples.data() + offset, c_DecodeChunkFrames, &framesRead);
				outSamples.resize(offset + static_cast<size_t>(framesRead) * channels);

				if (result == MA_AT_END || (result == MA_SUCCESS && framesRead == 0))
					break;
				if (result != MA_SUCCESS)
				{
					ST_CORE_ERROR("AudioClip: failed to decode '{}' ({})", debugName, ma_result_description(result));
					return false;
				}
			}

			outSamples.shrink_to_fit();
			return true;
		}

		// Fallback for streams that cannot report their length up front: decode through the stream and count.
		std::optional<uint64_t> CountRemainingFrames(ma_decoder& decoder, const std::string& debugName)
		{
			uint64_t frameCount = 0;
			while (true)
			{
				ma_uint64 framesRead = 0;
				const ma_result result = ma_decoder_read_pcm_frames(&decoder, nullptr, c_DecodeChunkFrames, &framesRead);
				frameCount += framesRead;

				if (result == MA_AT_END || (result == MA_SUCCESS && framesRead == 0))
					return frameCount;
				if (result != MA_SUCCESS)
				{
					ST_CORE_ERROR("AudioClip: failed to decode '{}' ({})", debugName, ma_result_description(result));
					return std::nullopt;
				}
			}
		}

	}

	Ref<AudioClip> AudioClip::LoadFromMemory(std::vector<uint8_t> encodedData, std::string debugName, AudioClipLoadMode mode)
	{
		ST_PROFILE_FUNCTION();

		if (encodedData.empty())
		{
			ST_CORE_ERROR("AudioClip: '{}' contains no data", debugName);
			return nullptr;
		}

		// Decode to 32-bit float at the stream's native channel count and sample rate. Conversion to the mixer's
		// format happens per voice, so clips never depend on how (or whether) the AudioEngine is configured.
		const ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 0, 0);
		ScopedDecoder decoder;
		ma_result result = ma_decoder_init_memory(encodedData.data(), encodedData.size(), &decoderConfig, &decoder.Decoder);
		if (result != MA_SUCCESS)
		{
			ST_CORE_ERROR("AudioClip: '{}' is not a supported audio file or is corrupt ({})", debugName, ma_result_description(result));
			return nullptr;
		}
		decoder.Initialized = true;

		ma_format format = ma_format_unknown;
		ma_uint32 channels = 0;
		ma_uint32 sampleRate = 0;
		result = ma_decoder_get_data_format(&decoder.Decoder, &format, &channels, &sampleRate, nullptr, 0);
		if (result != MA_SUCCESS || channels == 0 || channels > MA_MAX_CHANNELS || sampleRate == 0)
		{
			ST_CORE_ERROR("AudioClip: '{}' has an invalid format ({} channels, {} Hz)", debugName, channels, sampleRate);
			return nullptr;
		}

		// The constructor is private (clips are only created by the loaders), which rules out CreateRef.
		Ref<AudioClip> clip(new AudioClip());
		clip->m_DebugName = std::move(debugName);
		clip->m_LoadMode = mode;
		clip->m_Channels = channels;
		clip->m_SampleRate = sampleRate;

		if (mode == AudioClipLoadMode::Decompressed)
		{
			if (!DecodeAllFrames(decoder.Decoder, channels, clip->m_Samples, clip->m_DebugName))
				return nullptr;
			clip->m_FrameCount = clip->m_Samples.size() / channels;
		}
		else
		{
			// Voices need the length for seeking and position queries. Every vendored backend reports it when
			// decoding from memory; counting by decoding is only a fallback.
			ma_uint64 frameCount = 0;
			result = ma_decoder_get_length_in_pcm_frames(&decoder.Decoder, &frameCount);
			if (result != MA_SUCCESS || frameCount == 0)
			{
				const std::optional<uint64_t> countedFrames = CountRemainingFrames(decoder.Decoder, clip->m_DebugName);
				if (!countedFrames)
					return nullptr;
				frameCount = *countedFrames;
			}
			clip->m_FrameCount = frameCount;
		}

		if (clip->m_FrameCount == 0)
		{
			ST_CORE_ERROR("AudioClip: '{}' contains no audio frames", clip->m_DebugName);
			return nullptr;
		}

		// Moving the vector keeps its heap buffer, which the decoder still references until it is released.
		if (mode == AudioClipLoadMode::Streamed)
			clip->m_EncodedData = std::move(encodedData);

		return clip;
	}

	Ref<AudioClip> AudioClip::LoadFromFile(const std::filesystem::path& path, AudioClipLoadMode mode)
	{
		ST_PROFILE_FUNCTION();

		std::string debugName = FileSystem::ToUTF8(path);
		std::optional<std::vector<uint8_t>> data = FileSystem::ReadBytes(path);
		if (!data)
		{
			ST_CORE_ERROR("AudioClip: failed to read '{}'", debugName);
			return nullptr;
		}

		return LoadFromMemory(std::move(*data), std::move(debugName), mode);
	}

	float AudioClip::GetLength() const
	{
		if (m_SampleRate == 0)
			return 0.0f;
		return static_cast<float>(static_cast<double>(m_FrameCount) / static_cast<double>(m_SampleRate));
	}

	uint64_t AudioClip::GetMemoryUsage() const
	{
		return sizeof(AudioClip) + m_DebugName.capacity() + m_Samples.capacity() * sizeof(float) + m_EncodedData.capacity();
	}

}
