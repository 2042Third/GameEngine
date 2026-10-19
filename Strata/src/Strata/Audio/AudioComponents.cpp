#include "stpch.h"
#include "Strata/Audio/AudioComponents.h"

#include "Strata/Audio/AudioRegistration.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyOptions.h"

namespace Strata
{

	void RegisterAudioComponents()
	{
		ComponentRegistry::Register<AudioSourceComponent>("AudioSource")
			.DisplayName("Audio Source")
			.Category("Audio")
			.AssetProperty("Clip", &AudioSourceComponent::Clip, AssetType::AudioClip)
			.Property("Volume", &AudioSourceComponent::Volume, Slider(0.0f, 4.0f))
			.Property("Pitch", &AudioSourceComponent::Pitch, Range(0.01f, 8.0f, 0.01f))
			.Property("Loop", &AudioSourceComponent::Loop)
			.Property("PlayOnStart", &AudioSourceComponent::PlayOnStart)
			.Property("Spatial", &AudioSourceComponent::Spatial)
			.Property("MinDistance", &AudioSourceComponent::MinDistance, Range(0.0f, c_Unbounded, 0.1f))
			.Property("MaxDistance", &AudioSourceComponent::MaxDistance, Range(0.0f, c_Unbounded, 0.5f))
			.Property("Rolloff", &AudioSourceComponent::Rolloff, Range(0.0f, 100.0f, 0.01f));

		ComponentRegistry::Register<AudioListenerComponent>("AudioListener")
			.DisplayName("Audio Listener")
			.Category("Audio")
			.Property("Active", &AudioListenerComponent::Active);
	}

}
