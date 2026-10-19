#include <doctest/doctest.h>

#include "Audio/AudioTestUtils.h"
#include "Editor/AudioPreview.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"

#include <Strata/Audio/AudioComponents.h>
#include <Strata/Audio/AudioEngine.h>
#include <Strata/Audio/AudioSystem.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>

#include <string_view>

using namespace Strata;
using namespace Strata::Tests;

TEST_SUITE("Editor.Audio")
{
	TEST_CASE("Play mode plays the scene's audio, pauses it and releases it on stop")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		// An editor without a project makes its built-in assets active, so the clip project is activated after it.
		EditorContext context(EditorContextSpecification { false });
		AudioProject project;
		EditorCommandRegistry commands;
		const auto run = [&](std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			const EditorCommandResult result = commands.Execute(context, name, parameters);
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
		};

		Entity music = context.GetEditScene()->CreateEntity("Music");
		AudioSourceComponent& source = music.AddComponent<AudioSourceComponent>();
		source.Clip = project.AddClip(2.0f);
		source.Loop = true;
		source.Spatial = false;

		// Editor sessions start and stop play mode many times.
		for (int session = 0; session < 3; session++)
		{
			run("play.start");
			context.Update(Timestep(1.0f / 60.0f));
			CHECK(MeasureRms() == doctest::Approx(c_SineRms).epsilon(0.05));
			// A game may turn the volume down while it plays...
			AudioSystem::SetMasterVolume(0.5f);
			CHECK(MeasureRms() == doctest::Approx(c_SineRms * 0.5f).epsilon(0.05));

			run("play.pause", { { "paused", true } });
			CHECK(ComputeRms(Render(4800)) == 0.0f);
			run("play.step", { { "frames", 1 } });
			context.Update(Timestep(1.0f / 60.0f));
			CHECK(ComputeRms(Render(4800)) == 0.0f);
			run("play.pause", { { "paused", false } });
			CHECK(MeasureRms() == doctest::Approx(c_SineRms * 0.5f).epsilon(0.05));

			// ...but stopping restores the editor's volume, and releases every voice.
			run("play.stop");
			CHECK(AudioEngine::GetMasterVolume() == 1.0f);
			const AudioStats stats = AudioEngine::GetStats();
			CHECK(stats.SourceCount == 0);
			CHECK(stats.AllocatedVoices == 0);
			CHECK(ComputeRms(Render(4800)) == 0.0f);
		}

		// Simulate mode runs no audio.
		run("play.simulate");
		context.Update(Timestep(1.0f / 60.0f));
		CHECK(context.GetActiveScene()->GetSystem<AudioSystem>() == nullptr);
		CHECK(AudioEngine::GetStats().SourceCount == 0);
		run("play.stop");
	}

	TEST_CASE("The Asset tab's preview voice plays a clip through the mix until it ends or stops, whatever the scene does")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		const Ref<AudioClipAsset> asset = CreateClipAsset(0.5f);
		const AssetHandle handle = UUID();
		AudioPreview preview;

		REQUIRE(preview.Play(handle, asset->GetClip()));
		CHECK(preview.IsPlaying(handle));
		CHECK_FALSE(preview.IsPlaying(UUID()));
		// A plain voice at full volume: the clip's level, wherever the listener is.
		AudioEngine::SetListener(glm::vec3(50.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		CHECK(ComputeRms(Render(4800)) == doctest::Approx(c_SineRms).epsilon(0.05));
		CHECK(preview.GetPosition() == doctest::Approx(0.1f).epsilon(0.25));

		// Stop silences it at once; Play starts the clip from its beginning again.
		preview.Stop();
		CHECK_FALSE(preview.IsPlaying(handle));
		CHECK(preview.GetPosition() == 0.0f);
		CHECK(ComputeRms(Render(4800)) == 0.0f);
		REQUIRE(preview.Play(handle, asset->GetClip()));
		CHECK(preview.GetPosition() < 0.05f);

		// At the clip's end it stops on its own, and the voice is idle.
		Render(c_SampleRate);
		CHECK_FALSE(preview.IsPlaying(handle));
		CHECK(ComputeRms(Render(4800)) == 0.0f);
		CHECK_FALSE(preview.Play(handle, nullptr));
	}
}
