#include "stpch.h"
#include "Strata/Audio/AudioRegistration.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Audio/AudioClipAsset.h"
#include "Strata/Audio/AudioSystem.h"
#include "Strata/Scene/SceneSystem.h"

namespace Strata
{

	void RegisterAudioModule()
	{
		// Audio clips are stored as a header followed by the encoded file.
		AssetLoaderRegistry::Register(AssetType::AudioClip, [](const AssetMetadata& metadata, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
		{
			return AudioClipAsset::Deserialize(data, metadata.Path.empty() ? metadata.Name : metadata.Path, outError);
		});

		// Audio updates last (in OnLateUpdate), so sources and the listener follow the transforms scripts and physics produced
		// this frame. Like scripts, it does not run in simulate mode.
		SceneSystemRegistry::Register({ "Audio", false, [](Scene& scene) -> Scope<SceneSystem> { return CreateScope<AudioSystem>(scene); } });
	}

}
