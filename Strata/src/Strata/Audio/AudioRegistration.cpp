#include "stpch.h"
#include "Strata/Audio/AudioRegistration.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Audio/AudioClipAsset.h"
#include "Strata/Audio/AudioEngine.h"
#include "Strata/Audio/AudioSystem.h"
#include "Strata/Core/EngineServices.h"
#include "Strata/Scene/SceneServices.h"
#include "Strata/Scene/SceneSystem.h"

namespace Strata
{

	namespace
	{

		// The engine service IAudioOutput: the AudioEngine's master volume, for modules that may not include Audio (scripts).
		class AudioOutputService final : public IAudioOutput
		{
		public:
			void SetMasterVolume(float volume) override { AudioEngine::SetMasterVolume(volume); }
			float GetMasterVolume() override { return AudioEngine::GetMasterVolume(); }
		};

	}

	void RegisterAudioModule()
	{
		RegisterAudioComponents();

		static AudioOutputService s_AudioOutput;
		EngineServices::Register<IAudioOutput>(s_AudioOutput);

		// Audio clips are stored as a header followed by the encoded file.
		AssetLoaderRegistry::Register(AssetType::AudioClip, [](const AssetMetadata& metadata, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
		{
			return AudioClipAsset::Deserialize(data, metadata.Path.empty() ? metadata.Name : metadata.Path, outError);
		});

		// Audio updates last (in OnLateUpdate), so sources and the listener follow the transforms scripts, animation (root
		// motion) and physics produced this frame: after each of them, whichever an engine has (soft constraints: they are
		// other modules, and audio runs without them). Like scripts, it does not run in simulate mode.
		SceneSystemDescriptor descriptor = MakeSceneSystemDescriptor<AudioSystem>("Audio", false, [](Scene& scene) { return CreateScope<AudioSystem>(scene); });
		descriptor.AfterIfPresent = { "Scripting", "Animation", "Physics" };
		descriptor.ProfileStage = "Scene.Audio";
		const bool registered = SceneSystemRegistry::Register(std::move(descriptor));
		ST_CORE_VERIFY(registered, "The audio module could not register its scene system");
	}

}
