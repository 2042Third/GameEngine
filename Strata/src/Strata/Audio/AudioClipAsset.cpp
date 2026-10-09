#include "stpch.h"
#include "Strata/Audio/AudioClipAsset.h"

#include "Strata/Core/BinaryStream.h"

namespace Strata
{

	namespace
	{
		constexpr uint32_t c_AudioMagic = 0x55415453; // "STAU"
	}

	std::vector<uint8_t> AudioClipAsset::Cook(std::span<const uint8_t> encodedData, AudioClipLoadMode mode)
	{
		BinaryWriter writer;
		writer.Write(c_AudioMagic);
		writer.Write(c_CookedVersion);
		writer.Write(static_cast<uint32_t>(mode));
		writer.WriteBytes(encodedData.data(), encodedData.size());
		return writer.TakeData();
	}

	Ref<AudioClipAsset> AudioClipAsset::Deserialize(std::span<const uint8_t> data, const std::string& debugName, std::string* outError)
	{
		BinaryReader reader(data);
		const uint32_t magic = reader.Read<uint32_t>();
		const uint32_t version = reader.Read<uint32_t>();
		const uint32_t mode = reader.Read<uint32_t>();
		if (!reader.IsValid() || magic != c_AudioMagic || version != c_CookedVersion || mode > static_cast<uint32_t>(AudioClipLoadMode::Streamed))
		{
			if (outError)
				*outError = "Not a cooked Strata audio clip";
			return nullptr;
		}

		std::span<const uint8_t> encoded = data.subspan(reader.GetPosition());
		Ref<AudioClip> clip = AudioClip::LoadFromMemory(std::vector<uint8_t>(encoded.begin(), encoded.end()), debugName, static_cast<AudioClipLoadMode>(mode));
		if (!clip)
		{
			if (outError)
				*outError = "Audio data could not be decoded (supported: WAV, MP3, FLAC, Ogg Vorbis)";
			return nullptr;
		}

		Ref<AudioClipAsset> asset = CreateRef<AudioClipAsset>();
		asset->m_Clip = clip;
		return asset;
	}

}
