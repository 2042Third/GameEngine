#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Audio/AudioClip.h"

#include <span>
#include <string>
#include <vector>

namespace Strata
{

	// Asset wrapper around an AudioClip. Cooked format: "STAU" header with the load mode, followed by the encoded
	// source file (WAV, MP3, FLAC or Ogg Vorbis), which is decoded at load time.
	class AudioClipAsset : public Asset
	{
	public:
		static AssetType GetStaticType() { return AssetType::AudioClip; }
		AssetType GetType() const override { return GetStaticType(); }

		static constexpr uint32_t c_CookedVersion = 1;

		static std::vector<uint8_t> Cook(std::span<const uint8_t> encodedData, AudioClipLoadMode mode);
		static Ref<AudioClipAsset> Deserialize(std::span<const uint8_t> data, const std::string& debugName, std::string* outError = nullptr);

		uint64_t GetMemoryUsage() const override { return m_Clip ? m_Clip->GetMemoryUsage() : 0; }
		const Ref<AudioClip>& GetClip() const { return m_Clip; }
	private:
		Ref<AudioClip> m_Clip;
	};

}
