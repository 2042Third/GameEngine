#include <doctest/doctest.h>

#include "Physics/PhysicsScenes.h"
#include "Physics/PhysicsTestUtils.h"
#include "Strata/Core/EngineServices.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Process.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Physics/PhysicsComponents.h"
#include "Strata/Physics/PhysicsSimulationSettings.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/SceneServices.h"
#include "Strata/Scene/SceneWorkBudget.h"
#include "TestHelpers.h"

#include <box3d/box3d.h>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A project whose settings provide the Physics.Simulation section while it lives.
	class SimulationProject
	{
	public:
		SimulationProject()
		{
			REQUIRE(m_Registry.Register(PhysicsSimulationSettings::MakeSection()));
		}

		void Set(double recycleDistance, double sleepThreshold, double contactHertz = c_DefaultContactHertz)
		{
			std::string error;
			REQUIRE_MESSAGE(m_Values.Set(PhysicsSimulationSettings::c_SectionId, { { "ContactRecycleDistance", recycleDistance },
				{ "SleepThreshold", sleepThreshold }, { "ContactHertz", contactHertz } }, m_Registry, &error), error);
		}
	private:
		SettingsRegistry m_Registry;
		ProjectSettingsValues m_Values;
		EngineServices::ScopedProvider<IProjectSettings> m_Provider { m_Values };
	};

	Entity CreateCharacter(Scene& scene, const glm::vec3& position)
	{
		Entity character = scene.CreateEntity("Character");
		character.GetTransform().Translation = position;
		character.AddComponent<CharacterControllerComponent>();
		return character;
	}

}

TEST_SUITE("Physics.Settings")
{
	TEST_CASE("The Physics.Simulation section is box3d's defaults, described for agents")
	{
		const SettingsSection* section = SettingsRegistry::GetProcessRegistry().Find(PhysicsSimulationSettings::c_SectionId);
		REQUIRE(section != nullptr);
		CHECK(section->Title == "Simulation");
		CHECK(section->Category == "Physics");
		CHECK(section->Scope == SettingsScope::Project);
		REQUIRE(section->Fields.size() == 5);
		const PhysicsSettings defaults;
		for (const SettingsField& field : section->Fields)
		{
			CAPTURE(field.Name);
			CHECK(field.Type == SettingsFieldType::Float);
			CHECK(field.Description.size() > 100);
		}
		CHECK(section->Fields[0].Name == "ContactRecycleDistance");
		CHECK(section->Fields[0].Max == 1.0); // Every reading of "0.7", 0.7 m included, is one command away
		CHECK(section->Fields[0].Description.find("meters") != std::string::npos);
		CHECK(section->Fields[0].Description.find("Character controllers never recycle") != std::string::npos);
		CHECK(section->Fields[1].Name == "SleepThreshold");
		CHECK(section->Fields[1].Max == 1.0);
		CHECK(section->Fields[2].Name == "ContactHertz");
		CHECK(section->Fields[2].Min == 1.0);
		CHECK(section->Fields[2].Max == 240.0);
		CHECK(section->Fields[3].Name == "ContactDampingRatio");
		CHECK(section->Fields[3].Max == 100.0);
		CHECK(section->Fields[4].Name == "ContactPushSpeed");
		CHECK(section->Fields[4].Min == 0.01);
		const nlohmann::json value = section->GetDefault();
		CHECK(value["ContactRecycleDistance"].get<double>() == doctest::Approx(defaults.ContactRecycleDistance));
		CHECK(value["SleepThreshold"].get<double>() == doctest::Approx(defaults.SleepThreshold));
		CHECK(value["ContactHertz"].get<double>() == doctest::Approx(defaults.ContactHertz));
		CHECK(value["ContactDampingRatio"].get<double>() == doctest::Approx(defaults.ContactDampingRatio));
		CHECK(value["ContactPushSpeed"].get<double>() == doctest::Approx(defaults.ContactPushSpeed));
		// The contact tuning and the recycle distance are box3d's own (the sleep threshold: DeveloperGuide "Contact recycling").
		const b3WorldDef world = b3DefaultWorldDef();
		CHECK(defaults.ContactHertz == world.contactHertz);
		CHECK(defaults.ContactDampingRatio == world.contactDampingRatio);
		CHECK(defaults.ContactPushSpeed == world.contactSpeed);
		// Out of range, refused.
		std::string error;
		CHECK_FALSE(section->Validate({ { "ContactRecycleDistance", 1.5 }, { "SleepThreshold", 0.05 } }, 1, &error).has_value());
		CHECK_FALSE(section->Validate({ { "ContactRecycleDistance", 0.05 }, { "SleepThreshold", -0.1 } }, 1, &error).has_value());
	}

	TEST_CASE("Worlds use the project's section, else the process defaults")
	{
		Scene scene;
		CreateGround(scene);
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 0.5f, 0.0f));
		Entity character = CreateCharacter(scene, glm::vec3(5.0f, 0.0f, 0.0f));

		SUBCASE("Without a provider, the defaults")
		{
			PhysicsSettings settings;
			settings.ContactRecycleDistance = 0.02f;
			settings.SleepThreshold = 0.07f;
			ScopedPhysicsSettings scoped(settings);
			scene.OnRuntimeStart();
			const PhysicsWorld& world = *GetPhysics(scene).GetWorld();
			CHECK(world.GetContactRecycleDistance() == doctest::Approx(0.02f));
			CHECK(*world.GetSleepThreshold(box) == doctest::Approx(0.07f));
			CHECK(*world.GetSleepThreshold(character) == doctest::Approx(0.07f));
			CHECK(GetPhysics(scene).GetSimulationSource() == PhysicsSimulationSettings::Source::Defaults);
			scene.OnRuntimeStop();
		}
		SUBCASE("The project's section over the defaults")
		{
			SimulationProject project;
			project.Set(0.04, 0.06, 45.0);
			scene.OnRuntimeStart();
			const PhysicsWorld& world = *GetPhysics(scene).GetWorld();
			CHECK(world.GetSettings().ContactHertz == doctest::Approx(45.0f));
			CHECK(world.GetSettings().ContactDampingRatio == doctest::Approx(c_DefaultContactDampingRatio));
			CHECK(world.GetContactRecycleDistance() == doctest::Approx(0.04f));
			CHECK(world.GetSettings().ContactRecycleDistance == doctest::Approx(0.04f));
			CHECK(*world.GetSleepThreshold(box) == doctest::Approx(0.06f));
			CHECK(*world.GetSleepThreshold(character) == doctest::Approx(0.06f));
			CHECK(GetPhysics(scene).GetSimulationSource() == PhysicsSimulationSettings::Source::Project);
			scene.OnRuntimeStop();
		}
		SUBCASE("A world prepared while its scene loaded")
		{
			SimulationProject project;
			project.Set(0.03, 0.08);
			// A minute's budget: the few bodies are built in one call.
			SceneWorkBudget budget = SceneWorkBudget::ForFrame(scene.GetSettings(), 60'000.0);
			REQUIRE(PhysicsSystem::PrepareLoadedScene(scene, budget));
			scene.OnRuntimeStart();
			const PhysicsWorld& world = *GetPhysics(scene).GetWorld();
			CHECK(world.GetContactRecycleDistance() == doctest::Approx(0.03f));
			CHECK(*world.GetSleepThreshold(box) == doctest::Approx(0.08f));
			// A change after the preparation reaches it at its first fixed step.
			project.Set(0.01, 0.08);
			CHECK(world.GetContactRecycleDistance() == doctest::Approx(0.03f));
			StepScene(scene, 1);
			CHECK(world.GetContactRecycleDistance() == doctest::Approx(0.01f));
			scene.OnRuntimeStop();
		}
	}

	TEST_CASE("A running world takes a changed section at its next fixed step")
	{
		SimulationProject project;
		project.Set(0.05, 0.05);
		Scene scene;
		CreateGround(scene);
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 0.5f, 0.0f));
		Entity hidden = CreateDynamicBox(scene, "Hidden", glm::vec3(3.0f, 0.5f, 0.0f));
		Entity platform = CreateDynamicBox(scene, "Platform", glm::vec3(-3.0f, 0.5f, 0.0f));
		platform.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		Entity floor = CreateStaticBox(scene, "Floor", glm::vec3(0.0f, -2.0f, 0.0f), glm::vec3(1.0f));
		Entity character = CreateCharacter(scene, glm::vec3(5.0f, 0.0f, 0.0f));
		scene.OnRuntimeStart();
		PhysicsWorld& world = *GetPhysics(scene).GetWorld();
		StepScene(scene, 2);
		hidden.SetActive(false); // Out of the simulation: it keeps the threshold when it comes back
		StepScene(scene, 1);

		project.Set(0.007, 0.12);
		// Not before the step: the section is read on the main thread before a fixed step.
		CHECK(world.GetContactRecycleDistance() == doctest::Approx(0.05f));
		CHECK(*world.GetSleepThreshold(box) == doctest::Approx(0.05f));
		StepScene(scene, 1);
		CHECK(world.GetContactRecycleDistance() == doctest::Approx(0.007f));
		for (const Entity entity : { box, hidden, platform, character })
		{
			CAPTURE(entity.GetName());
			REQUIRE(world.GetSleepThreshold(entity).has_value());
			CHECK(*world.GetSleepThreshold(entity) == doctest::Approx(0.12f));
		}
		CHECK_FALSE(world.GetSleepThreshold(Entity()).has_value());
		hidden.SetActive(true);
		StepScene(scene, 1);
		CHECK(*world.GetSleepThreshold(hidden) == doctest::Approx(0.12f));
		// Static bodies never sleep: theirs is box3d's, untouched.
		CHECK(world.GetSleepThreshold(floor).has_value());
		// New bodies get the new threshold.
		Entity spawned = CreateDynamicBox(scene, "Spawned", glm::vec3(0.0f, 3.0f, 3.0f));
		StepScene(scene, 1);
		CHECK(*world.GetSleepThreshold(spawned) == doctest::Approx(0.12f));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Tuning out of range is clamped, non-finite values take the defaults")
	{
		Scene scene;
		CreateGround(scene);
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 0.5f, 0.0f));
		PhysicsSettings settings;
		settings.ContactRecycleDistance = 4.0f;
		settings.SleepThreshold = std::nanf("");
		settings.ContactHertz = 1000.0f;
		settings.ContactDampingRatio = -1.0f;
		settings.ContactPushSpeed = 0.0f;
		ScopedPhysicsSettings scoped(settings);
		scene.OnRuntimeStart();
		PhysicsWorld& world = *GetPhysics(scene).GetWorld();
		CHECK(world.GetContactRecycleDistance() == 1.0f);
		CHECK(world.GetSettings().SleepThreshold == c_DefaultSleepThreshold);
		CHECK(*world.GetSleepThreshold(box) == c_DefaultSleepThreshold);
		CHECK(world.GetSettings().ContactHertz == 240.0f);
		CHECK(world.GetSettings().ContactDampingRatio == 0.0f);
		CHECK(world.GetSettings().ContactPushSpeed == 0.01f);
		PhysicsSettings tuning = world.GetSettings();
		tuning.ContactRecycleDistance = -1.0f;
		tuning.SleepThreshold = 2.0f;
		world.SetSimulationTuning(tuning);
		CHECK(world.GetContactRecycleDistance() == 0.0f);
		CHECK(*world.GetSleepThreshold(box) == 1.0f);
		scene.OnRuntimeStop();
	}
}

TEST_SUITE("Physics.Recycling")
{
	TEST_CASE("Character controllers' presence bodies and kinematic bodies never recycle contacts; other bodies do")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 0.5f, 0.0f));
		Entity platform = CreateDynamicBox(scene, "Platform", glm::vec3(-3.0f, 0.5f, 0.0f));
		platform.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		Entity character = CreateCharacter(scene, glm::vec3(5.0f, 0.0f, 0.0f));
		scene.OnRuntimeStart();
		const PhysicsWorld& world = *GetPhysics(scene).GetWorld();
		CHECK_FALSE(world.IsContactRecyclingEnabled(character));
		CHECK_FALSE(world.IsContactRecyclingEnabled(platform));
		CHECK(world.IsContactRecyclingEnabled(box));
		CHECK(world.IsContactRecyclingEnabled(ground));
		CHECK_FALSE(world.IsContactRecyclingEnabled(Entity()));
		// A rebuilt presence body too.
		character.GetComponent<CharacterControllerComponent>().Radius = 0.4f;
		character.MarkModified<CharacterControllerComponent>();
		StepScene(scene, 1);
		CHECK_FALSE(world.IsContactRecyclingEnabled(character));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Characters carry and push crates without recycled contacts")
	{
		// The crate push of the settle scenes: a character walks a crate over tiles for 2 s, another stands with a crate on its
		// capsule, then walks. Before presence bodies stopped recycling, the pushed crate travelled 3.00 m (Release and Debug
		// alike); the head crate rested on the standing character and slid off its walk (a teleported presence body carries
		// nothing), 1.05 m back.
		Scene scene;
		const SettleScene push = CreateCratePush(scene);
		const SettlingSummary summary = MeasureSettling(scene, push, 4.0f);
		MESSAGE("Crate pushed ", summary.Probes.at("crate.distance"), " m; head crate slipped ", summary.Probes.at("headCrate.slip"), " m");
		CHECK(summary.Probes.at("headCrate.onTopStanding") == 1.0);
		CHECK(summary.Probes.at("crate.distance") == doctest::Approx(3.00).epsilon(0.05));
	}
}

TEST_SUITE("TestHelpers")
{
	TEST_CASE("The physics-settle helper plays with the tuning it is given")
	{
		const std::filesystem::path result = Tests::CreateTemporaryDirectory("PhysicsSettleTuning") / "Tower.json";
		ProcessSpecification specification;
		specification.Executable = Tests::GetTestExecutablePath();
		specification.Arguments = { "--strata-test-helper=physics-settle", "Tower20", "0.5", FileSystem::ToUTF8(result), "--recycle-distance", "0.02",
			"--sleep-threshold", "0.07", "--contact-hertz", "45", "--contact-damping", "5", "--contact-push-speed", "1" };
		const Process::RunResult run = Process::Run(specification, std::chrono::minutes(5));
		INFO("Output: ", run.Output);
		REQUIRE(run.ExitCode == 0);
		const std::optional<std::string> text = FileSystem::ReadText(result);
		REQUIRE(text);
		const nlohmann::json summary = nlohmann::json::parse(*text, nullptr, false);
		REQUIRE(summary.is_object());
		CHECK(summary["recycleDistance"].get<double>() == doctest::Approx(0.02));
		CHECK(summary["sleepThreshold"].get<double>() == doctest::Approx(0.07));
		CHECK(summary["contactHertz"].get<double>() == doctest::Approx(45.0));
		CHECK(summary["contactDamping"].get<double>() == doctest::Approx(5.0));
		CHECK(summary["contactPushSpeed"].get<double>() == doctest::Approx(1.0));
		// The ball pits in another layout; scenes without layouts and layouts out of range are refused.
		specification.Arguments = { "--strata-test-helper=physics-settle", "BallPit500", "0.1", FileSystem::ToUTF8(result), "--layout", "3" };
		REQUIRE(Process::Run(specification, std::chrono::minutes(5)).ExitCode == 0);
		const nlohmann::json pit = nlohmann::json::parse(FileSystem::ReadText(result).value_or(std::string()), nullptr, false);
		REQUIRE(pit.is_object());
		CHECK(pit["layout"] == 3);
		CHECK(pit["movingBodies"] == 500);
		for (const char* layout : { "9", "1.5", "-1" })
		{
			CAPTURE(layout);
			specification.Arguments = { "--strata-test-helper=physics-settle", "BallPit500", "0.1", FileSystem::ToUTF8(result), "--layout", layout };
			CHECK(Process::Run(specification, std::chrono::minutes(1)).ExitCode == 1);
		}
		specification.Arguments = { "--strata-test-helper=physics-settle", "Tower20", "0.1", FileSystem::ToUTF8(result), "--layout", "1" };
		CHECK(Process::Run(specification, std::chrono::minutes(1)).ExitCode == 1);

		// Out of its range, or not wholly a finite number: refused.
		specification.Arguments = { "--strata-test-helper=physics-settle", "Tower20", "0.5", FileSystem::ToUTF8(result), "--recycle-distance", "2" };
		CHECK(Process::Run(specification, std::chrono::minutes(1)).ExitCode == 1);
		for (const char* number : { "0.05m", "", "nan", "inf", "0,05" })
		{
			CAPTURE(number);
			specification.Arguments = { "--strata-test-helper=physics-settle", "Tower20", "0.5", FileSystem::ToUTF8(result), "--sleep-threshold", number };
			CHECK(Process::Run(specification, std::chrono::minutes(1)).ExitCode == 1);
			specification.Arguments = { "--strata-test-helper=physics-settle", "Tower20", number, FileSystem::ToUTF8(result) };
			CHECK(Process::Run(specification, std::chrono::minutes(1)).ExitCode == 1);
		}
	}
}
