#include <doctest/doctest.h>

#include "Audio/AudioTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"

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
		AudioProject project;
		EditorContext context(EditorContextSpecification { false });
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
}
