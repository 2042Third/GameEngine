#include <doctest/doctest.h>

#include "Audio/AudioTestUtils.h"
#include "Editor/AudioPreview.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"

#include <Strata/Audio/AudioComponents.h>
#include <Strata/Audio/AudioEngine.h>
#include <Strata/Audio/AudioSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Reflection/PropertyJson.h>
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

	TEST_CASE("audio.state lists the running game's voices and counts the one-shots it dropped, warning about them")
	{
		ScopedAudioEngine engine;
		REQUIRE(engine.Initialized);
		EditorContext context(EditorContextSpecification { false });
		AudioProject project;
		EditorCommandRegistry commands;
		const auto run = [&](std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			const EditorCommandResult result = commands.Execute(context, name, parameters);
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		};
		CHECK_FALSE(commands.Execute(context, "audio.state").Success); // Play mode only

		Entity music = context.GetEditScene()->CreateEntity("Music");
		AudioSourceComponent& source = music.AddComponent<AudioSourceComponent>();
		const AssetHandle loop = project.AddClip(2.0f);
		source.Clip = loop;
		source.Loop = true;
		source.Spatial = false;
		const AssetHandle blip = project.AddClip(0.5f);
		run("play.start");
		context.Update(Timestep(1.0f / 60.0f));
		AudioSystem* audio = context.GetActiveScene()->GetSystem<AudioSystem>();
		REQUIRE(audio);
		REQUIRE(audio->PlayOneShot(blip));
		REQUIRE(audio->PlayOneShotAt(blip, glm::vec3(1.0f, 2.0f, 3.0f), 0.5f));

		const nlohmann::json state = run("audio.state");
		REQUIRE(state["voices"].size() == 3);
		const nlohmann::json& first = state["voices"][0];
		CHECK(first["entity"] == UUIDToJson(music.GetUUID()));
		CHECK(first["clip"]["handle"] == UUIDToJson(loop));
		CHECK(first["looping"] == true);
		CHECK(first["playing"] == true);
		CHECK(state["voices"][1]["entity"].is_null());
		CHECK(state["voices"][2]["spatial"] == true);
		CHECK(state["voices"][2]["position"] == nlohmann::json { 1.0, 2.0, 3.0 });
		CHECK(state["voices"][2]["volume"] == 0.5);
		CHECK(state["oneShots"]["playing"] == 2);
		CHECK(state["oneShots"]["played"] == 2);
		CHECK(state["oneShots"]["dropped"] == 0);

		// 65 one-shots in one frame: the oldest is cut off, counted and named in a warning.
		const uint64_t logged = Log::GetBuffer().GetLatestSequence();
		for (int index = 0; index < 63; index++)
			REQUIRE(audio->PlayOneShot(blip));
		const nlohmann::json crowded = run("audio.state");
		CHECK(crowded["oneShots"]["playing"] == AudioSystem::c_MaxOneShots);
		CHECK(crowded["oneShots"]["played"] == 65);
		CHECK(crowded["oneShots"]["dropped"] == 1);
		size_t warnings = 0;
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(logged))
		{
			if (entry.Level == LogLevel::Warn && entry.Message.find("dropped a one-shot") != std::string::npos
				&& entry.Message.find("more than 64 one-shots") != std::string::npos)
				warnings++;
		}
		CHECK(warnings == 1);
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
