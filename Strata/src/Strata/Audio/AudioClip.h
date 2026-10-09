#pragma once

#include "Strata/Core/Base.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	enum class AudioClipLoadMode : uint8_t
	{
		// Decodes the whole clip once into shared 32-bit float PCM. Every voice reads the shared frames through its
		// own cursor, so starting a voice is cheap and playback does no decoding. Best for short sound effects.
		Decompressed = 0,
		// Keeps only the encoded bytes; every voice decodes them on the fly with its own decoder. Memory stays small
		// regardless of the clip's length at the cost of decoding during playback. Best for music and ambience.
		Streamed
	};

	// Immutable audio data shared by any number of voices (AudioSource, one-shots). Decodes WAV, FLAC, MP3 and
	// Ogg Vorbis.
	//
	// Clips are independent of the AudioEngine: they can be loaded before it is initialized and may outlive it.
	// Loading is thread-safe (clips may be decoded on worker threads) and a loaded clip is never modified, so it can
	// be shared freely between threads.
	class AudioClip
	{
	public:
		// Return nullptr (and log) when the data is empty, corrupt or in an unsupported format.
		static Ref<AudioClip> LoadFromMemory(std::vector<uint8_t> encodedData, std::string debugName, AudioClipLoadMode mode = AudioClipLoadMode::Decompressed);
		static Ref<AudioClip> LoadFromFile(const std::filesystem::path& path, AudioClipLoadMode mode = AudioClipLoadMode::Decompressed);

		AudioClip(const AudioClip&) = delete;
		AudioClip& operator=(const AudioClip&) = delete;

		float GetLength() const; // Seconds
		uint64_t GetFrameCount() const { return m_FrameCount; }
		uint32_t GetChannels() const { return m_Channels; }
		uint32_t GetSampleRate() const { return m_SampleRate; }
		AudioClipLoadMode GetLoadMode() const { return m_LoadMode; }
		const std::string& GetDebugName() const { return m_DebugName; }
		// Memory owned by the clip in bytes (each voice of a streamed clip additionally owns a small decoder).
		uint64_t GetMemoryUsage() const;

		// Interleaved 32-bit float frames at the clip's native channel count and sample rate (decompressed clips;
		// empty for streamed clips).
		std::span<const float> GetSamples() const { return m_Samples; }
		// The original encoded file contents (streamed clips; empty for decompressed clips).
		std::span<const uint8_t> GetEncodedData() const { return m_EncodedData; }
	private:
		AudioClip() = default;

		std::string m_DebugName;
		AudioClipLoadMode m_LoadMode = AudioClipLoadMode::Decompressed;
		uint32_t m_Channels = 0;
		uint32_t m_SampleRate = 0;
		uint64_t m_FrameCount = 0;
		std::vector<float> m_Samples;
		std::vector<uint8_t> m_EncodedData;
	};

}
