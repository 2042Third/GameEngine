#include <doctest/doctest.h>

#include "Core/SettingsTestUtils.h"
#include "Scene/SceneLoaderTestUtils.h"
#include "Strata/Core/EngineServices.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/ProjectSceneTiming.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneLoader.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

// The project's fixed rate (ProjectSceneTiming, the project settings section Scene.Timing): how scenes take it
// (Scene::ApplyProjectTiming) and how they step at it.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// The project's settings of a test, provided as IProjectSettings while it lives: a registry with the fixed rate and a simple
	// test section (an edit of another section). Written here rather than shared with Physics.Layers' TestProject, whose file
	// builds without physics are left without.
	struct TestProject
	{
		SettingsRegistry Registry;
		ProjectSettingsValues Values;
		Scope<EngineServices::ScopedProvider<IProjectSettings>> Provider;

		TestProject()
		{
			REQUIRE(Registry.Register(ProjectSceneTiming::MakeSection()));
			REQUIRE(Registry.Register(MakeSimpleTestSection()));
			Provider = CreateScope<EngineServices::ScopedProvider<IProjectSettings>>(Values);
		}

		void SetRate(const char* rate)
		{
			std::string error;
			REQUIRE_MESSAGE(Values.Set(ProjectSceneTiming::c_SectionId, { { ProjectSceneTiming::c_FixedRateField, rate } }, Registry, &error), error);
		}
	};

	// A scene that steps every 0.02 s and runs at most 8 steps per update: its own settings, which a project rate overrides.
	void SetOwnStep(Scene& scene)
	{
		scene.GetSettings().FixedTimestep = 0.02f;
		scene.GetSettings().MaxFixedStepsPerFrame = 8;
	}

	struct UpdateSteps
	{
		uint64_t Steps = 0;
		uint64_t Dropped = 0;
	};

	// The steps one runtime update of `seconds` runs and drops.
	UpdateSteps RunUpdate(Scene& scene, float seconds)
	{
		const uint64_t steps = scene.GetFixedStepCount();
		const uint64_t dropped = scene.GetDroppedFixedStepCount();
		scene.OnUpdateRuntime(Timestep(seconds));
		return UpdateSteps { scene.GetFixedStepCount() - steps, scene.GetDroppedFixedStepCount() - dropped };
	}

}

TEST_SUITE("Scene.Timing")
{
	TEST_CASE("The project's fixed rate overrides the scene's step and scales the steps one update may run")
	{
		TestProject project;
		project.SetRate("240");
		Scene scene;
		SetOwnStep(scene);
		CHECK(scene.ApplyProjectTiming());
		CHECK(scene.GetProjectFixedRate() == 240);
		CHECK(scene.GetFixedTimestep() == 1.0f / 240.0f);
		CHECK(scene.GetMaxFixedSteps() == 32);
		// The scene's own settings are kept: they apply again when the project's rate is Scene.
		CHECK(scene.GetSettings().FixedTimestep == 0.02f);
		CHECK(scene.GetSettings().MaxFixedStepsPerFrame == 8);

		scene.OnRuntimeStart();
		// A tenth of a second is 24 steps at 240 (float rounding may leave the last one for the next update), all of them run:
		// the scene's own 8 steps would cover 33 ms.
		const UpdateSteps tenth = RunUpdate(scene, 0.1f);
		CHECK(tenth.Steps >= 23);
		CHECK(tenth.Dropped == 0);
		// A quarter second is longer than 2/15 s: 32 steps run, the backlog is dropped.
		const UpdateSteps quarter = RunUpdate(scene, 0.25f);
		CHECK(quarter.Steps == 32);
		CHECK(quarter.Dropped > 0);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Scene keeps each scene's own step, and a reset gives it back")
	{
		TestProject project;
		Scene scene;
		SetOwnStep(scene);
		// Nothing set: the default, Scene. The scene takes the value, which changes nothing it runs at.
		CHECK_FALSE(scene.ApplyProjectTiming());
		CHECK(scene.GetProjectFixedRate() == 0);
		CHECK(scene.GetFixedTimestep() == 0.02f);
		CHECK(scene.GetMaxFixedSteps() == 8);

		project.SetRate("120");
		CHECK(scene.ApplyProjectTiming());
		CHECK(scene.GetFixedTimestep() == 1.0f / 120.0f);
		CHECK(scene.GetMaxFixedSteps() == 16);
		CHECK_FALSE(scene.ApplyProjectTiming()); // Taken already
		// An edit of another section leaves the rate alone.
		std::string error;
		REQUIRE_MESSAGE(project.Values.Set("Test.Simple", { { "Count", 5 } }, project.Registry, &error), error);
		CHECK_FALSE(scene.ApplyProjectTiming());
		CHECK(scene.GetProjectFixedRate() == 120);

		project.SetRate("Scene");
		CHECK(scene.ApplyProjectTiming());
		CHECK(scene.GetFixedTimestep() == 0.02f);
		CHECK(scene.GetMaxFixedSteps() == 8);

		// A reset of the section is the default again.
		project.SetRate("30");
		CHECK(scene.ApplyProjectTiming());
		CHECK(scene.GetMaxFixedSteps() == 4);
		CHECK(project.Values.Reset(ProjectSceneTiming::c_SectionId));
		CHECK(scene.ApplyProjectTiming());
		CHECK(scene.GetProjectFixedRate() == 0);
		CHECK(scene.GetFixedTimestep() == 0.02f);
	}

	TEST_CASE("A program without project settings keeps the scene's step")
	{
		REQUIRE(ProjectSceneTiming::Find() == nullptr);
		Scene scene;
		SetOwnStep(scene);
		CHECK_FALSE(scene.ApplyProjectTiming());
		CHECK(scene.GetFixedTimestep() == 0.02f);
		CHECK(scene.GetMaxFixedSteps() == 8);
		// A step that is not positive falls back to 1/60 s.
		scene.GetSettings().FixedTimestep = 0.0f;
		CHECK(scene.GetFixedTimestep() == 1.0f / 60.0f);
	}

	TEST_CASE("A copy keeps the project's rate, and a changed section reaches a running scene at its next update")
	{
		TestProject project;
		project.SetRate("120");
		const Ref<Scene> edited = CreateRef<Scene>();
		SetOwnStep(*edited);
		REQUIRE(edited->ApplyProjectTiming());
		const Ref<Scene> running = Scene::Copy(edited);
		CHECK(running->GetProjectFixedRate() == 120);
		CHECK(running->GetFixedTimestep() == 1.0f / 120.0f);
		CHECK_FALSE(running->ApplyProjectTiming()); // The copy came from the same value

		running->OnRuntimeStart();
		CHECK(RunUpdate(*running, 1.0f / 60.0f).Steps >= 1);
		// Changed while it runs (project.setSettings during play): the next update steps at 1/30 s, at most 4 steps of it.
		project.SetRate("30");
		CHECK(running->ApplyProjectTiming());
		CHECK(running->GetFixedTimestep() == 1.0f / 30.0f);
		const UpdateSteps update = RunUpdate(*running, 0.2f);
		CHECK(update.Steps == 4);
		CHECK(update.Dropped > 0);
		running->OnRuntimeStop();
		// The edited scene takes it when it is applied too (EditorContext::ApplyProjectLayers does both).
		CHECK(edited->GetProjectFixedRate() == 120);
		CHECK(edited->ApplyProjectTiming());
		CHECK(edited->GetProjectFixedRate() == 30);
	}

	TEST_CASE("FixedRate 60 changes nothing in a default scene")
	{
		Scene own;
		Scene project;
		{
			TestProject settings;
			settings.SetRate("60");
			REQUIRE(project.ApplyProjectTiming());
		}
		REQUIRE(project.GetProjectFixedRate() == 60);
		CHECK(project.GetFixedTimestep() == own.GetFixedTimestep());
		CHECK(project.GetMaxFixedSteps() == own.GetMaxFixedSteps());
		own.OnRuntimeStart();
		project.OnRuntimeStart();
		// Frames of a 144 Hz display, and a few long ones that reach the cap.
		for (int frame = 0; frame < 1000; frame++)
		{
			const float seconds = frame % 250 == 249 ? 0.2f : 1.0f / 144.0f;
			own.OnUpdateRuntime(Timestep(seconds));
			project.OnUpdateRuntime(Timestep(seconds));
		}
		CHECK(project.GetFixedStepCount() == own.GetFixedStepCount());
		CHECK(project.GetDroppedFixedStepCount() == own.GetDroppedFixedStepCount());
		CHECK(own.GetDroppedFixedStepCount() > 0);
		CHECK(project.GetTime() == own.GetTime());
		own.OnRuntimeStop();
		project.OnRuntimeStop();
	}

	TEST_CASE("The section offers named rates and refuses others")
	{
		SettingsRegistry registry;
		REQUIRE(registry.Register(ProjectSceneTiming::MakeSection()));
		const SettingsSection& section = *registry.Find(ProjectSceneTiming::c_SectionId);
		CHECK(section.Category == "Physics");
		CHECK(section.Scope == SettingsScope::Project);
		CHECK(section.GetDefault() == nlohmann::json { { "FixedRate", "Scene" } });
		REQUIRE(section.Fields.size() == 1);
		const SettingsField& field = section.Fields.front();
		CHECK(field.Description.find("soft") != std::string::npos);
		CHECK(field.Description.find("30") != std::string::npos);

		for (const char* rate : { "Scene", "30", "60", "120", "240" })
		{
			INFO(rate);
			const std::optional<nlohmann::json> value = section.Validate({ { "FixedRate", rate } }, 1);
			REQUIRE(value.has_value());
			CHECK((*value)["FixedRate"] == rate);
		}
		for (const nlohmann::json& refused : { nlohmann::json(120), nlohmann::json("100"), nlohmann::json("scene") })
		{
			INFO(refused.dump());
			std::string error;
			CHECK_FALSE(section.Validate({ { "FixedRate", refused } }, 1, &error).has_value());
			CHECK(error.find("FixedRate") != std::string::npos);
		}

		// The most steps an update runs: 2/15 s at the rate, from 1 to 64.
		CHECK(ProjectSceneTiming::GetMaxSteps(30) == 4);
		CHECK(ProjectSceneTiming::GetMaxSteps(60) == 8);
		CHECK(ProjectSceneTiming::GetMaxSteps(120) == 16);
		CHECK(ProjectSceneTiming::GetMaxSteps(240) == 32);
		CHECK(ProjectSceneTiming::GetMaxSteps(0) == 1);
		CHECK(ProjectSceneTiming::GetMaxSteps(4'000'000'000u) == ProjectSceneTiming::c_MaxStepsLimit);
	}

	TEST_CASE("Every hand-over takes the project's fixed rate, also a rate changed while the scene loaded")
	{
		TestProject project;
		project.SetRate("120");
		Scene stored("Level");
		SetOwnStep(stored);
		for (int index = 0; index < 3; index++)
			stored.CreateEntity("Entity " + std::to_string(index));
		const std::vector<uint8_t> document = SceneToBytes(stored);

		SceneLoaderFixture fixture(document);
		FakeAssetManager& manager = *fixture.Manager;
		manager.CloseGate();
		SceneLoadOptions options;
		std::optional<float> prepared;
		options.Prepare = [&prepared](Scene& scene, SceneWorkBudget&)
		{
			prepared = scene.GetFixedTimestep();
			return true;
		};
		SceneLoadRequest request = SceneLoader::LoadAsync(fixture.SceneHandle, options);
		// Only the scene's read passes: it arrives and waits for what it uses.
		manager.AllowReads(1);
		REQUIRE(WaitUntil([&]()
		{
			manager.Update();
			request.Update();
			return request.GetState() == SceneLoadState::LoadingDependencies;
		}, std::chrono::milliseconds(10000)));

		// The rate changes while the load goes on (an edit in Project Settings).
		project.SetRate("30");
		manager.OpenGate();
		fixture.Finish(request);
		REQUIRE(request.GetState() == SceneLoadState::Ready);
		// The preparation saw the rate the scene took when it arrived; the hand-over gives it the rate as it is now.
		REQUIRE(prepared.has_value());
		CHECK(*prepared == 1.0f / 120.0f);
		const Ref<Scene> scene = request.TakeScene();
		REQUIRE(scene);
		CHECK(scene->GetFixedTimestep() == 1.0f / 30.0f);
		CHECK(scene->GetSettings().FixedTimestep == 0.02f);
		CHECK_FALSE(scene->ApplyProjectTiming()); // Taken already

		// A scene asset's other hand-overs take it too.
		std::string error;
		const Ref<SceneAsset> asset = SceneAsset::Deserialize(document, &error);
		REQUIRE_MESSAGE(asset, error);
		const Ref<Scene> created = asset->CreateScene(&error);
		REQUIRE_MESSAGE(created, error);
		CHECK(created->GetProjectFixedRate() == 30);
		const Ref<Scene> taken = asset->TakePreparedScene(&error);
		REQUIRE_MESSAGE(taken, error);
		CHECK(taken->GetProjectFixedRate() == 30);
		CHECK(taken->GetMaxFixedSteps() == 4);
	}
}
