#include <doctest/doctest.h>

#include "Audio/AudioTestUtils.h"
#include "Scripting/ScriptTestUtils.h"
#include "Strata/Audio/AudioSystem.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/SceneSystem.h"

#include <algorithm>
#include <optional>
#include <string>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// The scene the audio test scripts expect (StrataTests/Scripts/API/AudioScripts.cpp): "Speaker" runs `className` with
	// the clip in its Clip field and plays it (looping, not on start); "Mute" has no AudioSource; "Sleeper" is inactive.
	Entity CreateAudioScene(Scene& scene, AssetHandle clip, const std::string& className)
	{
		Entity speaker = scene.CreateEntity("Speaker");
		AudioSourceComponent& source = speaker.AddComponent<AudioSourceComponent>();
		source.Clip = clip;
		source.PlayOnStart = false;
		source.Loop = true;
		source.Spatial = false;
		ScriptEntry& entry = AddScriptEntry(speaker, className);
		AddFieldOverride(entry, "Clip", PropertyType::Asset, PropertyValue(UUID(clip)));

		scene.CreateEntity("Mute");
		Entity sleeper = scene.CreateEntity("Sleeper");
		sleeper.AddComponent<AudioSourceComponent>().Clip = clip;
		sleeper.SetActive(false);
		return speaker;
	}

	// Restores the engine-wide master volume a test changed.
	struct ScopedMasterVolume
	{
		float Previous = AudioEngine::GetMasterVolume();

		~ScopedMasterVolume()
		{
			AudioEngine::SetMasterVolume(Previous);
		}
	};

	// Scenes play without their audio system while this lives (as in simulate mode, where audio does not run).
	class ScopedWithoutAudioSystem
	{
	public:
		ScopedWithoutAudioSystem()
		{
			const std::vector<SceneSystemDescriptor>& descriptors = SceneSystemRegistry::GetAll();
			// Re-registering appends, which keeps the order only for the last system.
			REQUIRE_FALSE(descriptors.empty());
			REQUIRE(descriptors.back().Name == "Audio");
			m_Descriptor = descriptors.back();
			REQUIRE(SceneSystemRegistry::Unregister("Audio"));
		}

		~ScopedWithoutAudioSystem()
		{
			CHECK(SceneSystemRegistry::Register(m_Descriptor));
		}

		ScopedWithoutAudioSystem(const ScopedWithoutAudioSystem&) = delete;
		ScopedWithoutAudioSystem& operator=(const ScopedWithoutAudioSystem&) = delete;
	private:
		SceneSystemDescriptor m_Descriptor;
	};

	// Plays `className` on the speaker for one frame and checks its checks.
	void RunAudioScript(AssetHandle clip, const std::string& className, int32_t minimumChecks, std::optional<int32_t> mode = std::nullopt)
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity speaker = CreateAudioScene(scene, clip, className);
		if (mode)
			AddFieldOverride(speaker.GetComponent<ScriptComponent>().Scripts.back(), "Mode", PropertyType::Int, PropertyValue(*mode));
		scene.OnRuntimeStart();
		RunFrames(scene, 1);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, speaker, className, "Done"));
		CheckScriptChecks(system, speaker, className, minimumChecks);
		CHECK_FALSE(engine->IsFaulted());
		scene.OnRuntimeStop();
	}

}

TEST_SUITE("Scripting.Audio")
{
	TEST_CASE("Scripts play sources and one-shots and set the master volume")
	{
		ScopedAudioEngine audioEngine;
		REQUIRE(audioEngine.Initialized);
		ScopedMasterVolume masterVolume;
		AudioProject project;
		const AssetHandle clip = project.AddClip(2.0f);
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity speaker = CreateAudioScene(scene, clip, "AudioAPI");
		scene.OnRuntimeStart();
		RunFrames(scene, 1);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, speaker, "AudioAPI", "Done"));
		CheckScriptChecks(system, speaker, "AudioAPI", 13);
		CHECK_FALSE(engine->IsFaulted());

		// The engine plays what the script asked for: the looping source and three one-shots, at the restored volume.
		AudioSystem* audio = scene.GetSystem<AudioSystem>();
		REQUIRE(audio != nullptr);
		CHECK(audio->IsPlaying(speaker));
		CHECK(audio->GetStats().OneShotCount == 3);
		CHECK(AudioEngine::GetMasterVolume() == 1.0f);
		CHECK(MeasureRms() > 0.1f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Audio functions refuse misuse harmlessly")
	{
		ScopedAudioEngine audioEngine;
		REQUIRE(audioEngine.Initialized);
		ScopedMasterVolume masterVolume;
		AudioProject project;
		const AssetHandle clip = project.AddClip(2.0f);
		RunAudioScript(clip, "AudioMisuse", 16);
	}

	TEST_CASE("Audio functions play nothing without audio output, in scenes without audio and on older engines")
	{
		ScopedMasterVolume masterVolume;
		AudioProject project;
		const AssetHandle clip = project.AddClip(2.0f);

		SUBCASE("Without audio output")
		{
			REQUIRE_FALSE(AudioEngine::IsInitialized());
			RunAudioScript(clip, "AudioUnavailable", 3, 0);
		}
		SUBCASE("In a scene without its audio system")
		{
			ScopedAudioEngine audioEngine;
			REQUIRE(audioEngine.Initialized);
			ScopedWithoutAudioSystem withoutAudio;
			RunAudioScript(clip, "AudioUnavailable", 3, 1);
		}
		SUBCASE("On an engine without the audio functions")
		{
			ScopedAudioEngine audioEngine;
			REQUIRE(audioEngine.Initialized);
			RunAudioScript(clip, "AudioUnavailable", 3, 2);
		}
	}
}
