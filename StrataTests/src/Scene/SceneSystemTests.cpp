#include <doctest/doctest.h>

#include "Strata/Core/Log.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSystem.h"
#include "Strata/Scripting/ScriptSystem.h"
#include "TestHelpers.h"

#if ST_WITH_PHYSICS
	#include "Strata/Physics/PhysicsSystem.h"
#endif
#if ST_WITH_AUDIO
	#include "Strata/Audio/AudioSystem.h"
#endif
#if ST_WITH_ANIMATION
	#include "Strata/Animation/AnimationSystem.h"
#endif

#include <algorithm>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	// Records the order in which the test systems update.
	class RecordingSystem : public SceneSystem
	{
	public:
		explicit RecordingSystem(std::string name)
			: m_Name(std::move(name))
		{
		}

		void OnUpdate(Timestep) override { Updates.push_back(m_Name); }

		static inline std::vector<std::string> Updates;
	private:
		std::string m_Name;
	};

	class OtherRecordingSystem : public RecordingSystem
	{
	public:
		using RecordingSystem::RecordingSystem;
	};

	struct PhaseCounts
	{
		uint32_t Updates = 0;
		uint32_t FixedUpdates = 0;
		uint32_t LateUpdates = 0;
		uint32_t PausedUpdates = 0;
	};

	// Counts the per-frame callbacks the scene calls.
	class PhaseCountingSystem : public SceneSystem
	{
	public:
		explicit PhaseCountingSystem(PhaseCounts& counts)
			: m_Counts(counts)
		{
		}

		void OnUpdate(Timestep) override { m_Counts.Updates++; }
		void OnFixedUpdate(float) override { m_Counts.FixedUpdates++; }
		void OnLateUpdate(Timestep) override { m_Counts.LateUpdates++; }
		void OnPausedUpdate() override { m_Counts.PausedUpdates++; }
	private:
		PhaseCounts& m_Counts;
	};

	// Takes part in the late update only.
	class LateOnlySystem : public SceneSystem
	{
	public:
		void OnLateUpdate(Timestep) override {}
	};

	SceneSystemDescriptor MakeRecording(const std::string& name, std::vector<std::string> after = {}, std::vector<std::string> before = {})
	{
		// Typeless: several recording systems may be registered at once.
		SceneSystemDescriptor descriptor;
		descriptor.Name = name;
		descriptor.Create = [name](Scene&) -> Scope<SceneSystem> { return CreateScope<RecordingSystem>(name); };
		descriptor.After = std::move(after);
		descriptor.Before = std::move(before);
		return descriptor;
	}

#if ST_TESTS_HAVE_EVERY_MODULE
	// A recording system with soft constraints only (its test orders it among every module's systems).
	SceneSystemDescriptor MakeSoftRecording(const std::string& name, std::vector<std::string> afterIfPresent, std::vector<std::string> beforeIfPresent = {})
	{
		SceneSystemDescriptor descriptor = MakeRecording(name);
		descriptor.AfterIfPresent = std::move(afterIfPresent);
		descriptor.BeforeIfPresent = std::move(beforeIfPresent);
		return descriptor;
	}
#endif

	std::vector<std::string> GetSystemNames()
	{
		std::vector<std::string> names;
		for (const SceneSystemDescriptor& descriptor : SceneSystemRegistry::GetAll())
			names.push_back(descriptor.Name);
		return names;
	}

	// The newest error after `after` that contains `text`, or an empty string.
	std::string FindError(uint64_t after, std::string_view text)
	{
		std::string found;
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(after))
		{
			if (entry.Level == LogLevel::Error && entry.Message.find(text) != std::string::npos)
				found = entry.Message;
		}
		return found;
	}

	// Unregisters the named systems (in reverse order, so that systems naming earlier ones go first) when it goes away.
	struct ScopedSystems
	{
		~ScopedSystems()
		{
			for (auto it = Names.rbegin(); it != Names.rend(); ++it)
				CHECK(SceneSystemRegistry::Unregister(*it));
		}

		bool Register(SceneSystemDescriptor descriptor)
		{
			const std::string name = descriptor.Name;
			if (!SceneSystemRegistry::Register(std::move(descriptor)))
				return false;
			Names.push_back(name);
			return true;
		}

		std::vector<std::string> Names;
	};

}

TEST_SUITE("Scene.Systems")
{
#if ST_TESTS_HAVE_EVERY_MODULE
	TEST_CASE("The built-in systems update in the order Scripting, Animation, Physics, Audio")
	{
		// Animation runs after the scripts (which drive animators) and before physics (root motion moves bodies before the
		// step).
		CHECK(GetSystemNames() == std::vector<std::string> { "Scripting", "Animation", "Physics", "Audio" });

		const std::vector<SceneSystemDescriptor>& descriptors = SceneSystemRegistry::GetAll();
		REQUIRE(descriptors.size() == 4);
		// Each names another module's system: soft constraints, so that the engine runs without that module.
		for (const SceneSystemDescriptor& descriptor : descriptors)
		{
			CAPTURE(descriptor.Name);
			CHECK(descriptor.After.empty());
			CHECK(descriptor.Before.empty());
		}
		CHECK(descriptors[0].AfterIfPresent.empty());
		CHECK(descriptors[1].AfterIfPresent == std::vector<std::string> { "Scripting" });
		CHECK(descriptors[1].BeforeIfPresent == std::vector<std::string> { "Physics" });
		CHECK(descriptors[2].AfterIfPresent == std::vector<std::string> { "Scripting" });
		CHECK(descriptors[3].AfterIfPresent == std::vector<std::string> { "Scripting", "Animation", "Physics" });
		CHECK(SceneSystemRegistry::GetAbsentSoftConstraints().empty());
		CHECK(descriptors[0].Type == entt::type_id<ScriptSystem>().hash());
		CHECK(descriptors[1].Type == entt::type_id<AnimationSystem>().hash());
		CHECK(descriptors[2].Type == entt::type_id<PhysicsSystem>().hash());
		CHECK(descriptors[3].Type == entt::type_id<AudioSystem>().hash());
		CHECK(descriptors[1].ProfileStage == "Scene.Animation");
		CHECK_FALSE(descriptors[1].RunsInSimulateMode);
	}
#endif

#if ST_TESTS_HAVE_EVERY_MODULE
	TEST_CASE("After and Before are honored, and registration order decides the rest")
	{
		RecordingSystem::Updates.clear();
		ScopedSystems systems;
		REQUIRE(systems.Register(MakeRecording("TestA")));
		REQUIRE(systems.Register(MakeRecording("TestB", {}, { "TestA" })));
		REQUIRE(systems.Register(MakeRecording("TestC", { "TestA" }, { "Audio" })));
		REQUIRE(systems.Register(MakeRecording("TestD")));

		// TestB goes before TestA, TestC between TestA and Audio; unconstrained choices follow registration order.
		CHECK(GetSystemNames() == std::vector<std::string> { "Scripting", "Animation", "Physics", "TestB", "TestA", "TestC", "Audio", "TestD" });

		{
			Scene scene;
			scene.OnRuntimeStart();
			scene.OnUpdateRuntime(0.016f);
			scene.OnRuntimeStop();
		}
		CHECK(RecordingSystem::Updates == std::vector<std::string> { "TestB", "TestA", "TestC", "TestD" });

		// A system registered again with the same name replaces the old one and counts as registered last: TestA stays,
		// held between TestB and TestC; TestB, without its constraint now, follows every other system.
		REQUIRE(SceneSystemRegistry::Register(MakeRecording("TestA")));
		CHECK(GetSystemNames() == std::vector<std::string> { "Scripting", "Animation", "Physics", "TestB", "TestA", "TestC", "Audio", "TestD" });
		REQUIRE(SceneSystemRegistry::Register(MakeRecording("TestB")));
		CHECK(GetSystemNames() == std::vector<std::string> { "Scripting", "Animation", "Physics", "TestA", "TestC", "Audio", "TestD", "TestB" });
	}
#endif

#if ST_TESTS_HAVE_EVERY_MODULE
	TEST_CASE("A system moves ahead of earlier registered ones only as far as its constraints require")
	{
		RecordingSystem::Updates.clear();
		ScopedSystems systems;
		// A camera that follows a rigid body has no constraint: it runs after every system registered before it, also when
		// a system registered after it has to run before one of them.
		REQUIRE(systems.Register(MakeRecording("CameraFollow")));
		REQUIRE(systems.Register(MakeRecording("Wind", {}, { "Physics" })));
		CHECK(GetSystemNames() == std::vector<std::string> { "Scripting", "Animation", "Wind", "Physics", "Audio", "CameraFollow" });
		REQUIRE(systems.Register(MakeRecording("EarlyInput", {}, { "Scripting" })));
		CHECK(GetSystemNames() == std::vector<std::string> { "EarlyInput", "Scripting", "Animation", "Wind", "Physics", "Audio", "CameraFollow" });

		{
			Scene scene;
			scene.OnRuntimeStart();
			scene.OnUpdateRuntime(0.016f);
			scene.OnRuntimeStop();
		}
		CHECK(RecordingSystem::Updates == std::vector<std::string> { "EarlyInput", "Wind", "CameraFollow" });
	}
#endif

	TEST_CASE("A cycle or an unknown name is refused with the systems named, and the registry stays unchanged")
	{
		ScopedSystems systems;
		REQUIRE(systems.Register(MakeRecording("TestA")));
		REQUIRE(systems.Register(MakeRecording("TestB", { "TestA" })));
		const std::vector<std::string> names = GetSystemNames();

		uint64_t before = Log::GetBuffer().GetLatestSequence();
		CHECK_FALSE(SceneSystemRegistry::Register(MakeRecording("TestC", { "TestB" }, { "TestA" })));
		CHECK(FindError(before, "Scene system 'TestC' is not registered: the update order of the scene systems has a cycle: TestA -> TestB -> TestC -> TestA") != "");
		CHECK(GetSystemNames() == names);

		// Replacing a system with one that closes a cycle keeps the old one.
		before = Log::GetBuffer().GetLatestSequence();
		CHECK_FALSE(SceneSystemRegistry::Register(MakeRecording("TestA", { "TestB" })));
		CHECK(FindError(before, "Scene system 'TestA' is not registered: the update order of the scene systems has a cycle: TestB -> TestA -> TestB") != "");
		CHECK(GetSystemNames() == names);
		const auto testA = std::find_if(SceneSystemRegistry::GetAll().begin(), SceneSystemRegistry::GetAll().end(), [](const SceneSystemDescriptor& descriptor) { return descriptor.Name == "TestA"; });
		REQUIRE(testA != SceneSystemRegistry::GetAll().end());
		CHECK(testA->After.empty());

		before = Log::GetBuffer().GetLatestSequence();
		CHECK_FALSE(SceneSystemRegistry::Register(MakeRecording("TestD", { "NoSuchSystem" })));
		CHECK(FindError(before, "Scene system 'TestD' is not registered: scene system 'TestD' is to run after 'NoSuchSystem', which is not registered") != "");
		CHECK_FALSE(SceneSystemRegistry::Register(MakeRecording("TestD", {}, { "NoSuchSystem" })));
		CHECK(FindError(before, "is to run before 'NoSuchSystem', which is not registered") != "");
		CHECK_FALSE(SceneSystemRegistry::Register(MakeRecording("TestD", {}, { "TestD" })));
		CHECK(FindError(before, "scene system 'TestD' names itself in its update order") != "");
		CHECK(GetSystemNames() == names);

		// Descriptors without a name or a factory, and a class registered under another name.
		CHECK_FALSE(SceneSystemRegistry::Register(SceneSystemDescriptor()));
		SceneSystemDescriptor withoutFactory = MakeRecording("TestE");
		withoutFactory.Create = nullptr;
		CHECK_FALSE(SceneSystemRegistry::Register(std::move(withoutFactory)));
		REQUIRE(systems.Register(MakeSceneSystemDescriptor<RecordingSystem>("TestTyped", false, [](Scene&) { return CreateScope<RecordingSystem>("TestTyped"); })));
		before = Log::GetBuffer().GetLatestSequence();
		CHECK_FALSE(SceneSystemRegistry::Register(MakeSceneSystemDescriptor<RecordingSystem>("TestTypedAgain", false, [](Scene&) { return CreateScope<RecordingSystem>("TestTypedAgain"); })));
		CHECK(FindError(before, "its class is registered already, as scene system 'TestTyped'") != "");

		// A system other systems name cannot go.
		before = Log::GetBuffer().GetLatestSequence();
		CHECK_FALSE(SceneSystemRegistry::Unregister("TestA"));
		CHECK(FindError(before, "Scene system 'TestA' is not unregistered: the update order of 'TestB' names it") != "");
		CHECK_FALSE(SceneSystemRegistry::Unregister("NoSuchSystem"));
	}

#if ST_TESTS_HAVE_EVERY_MODULE
	TEST_CASE("Soft constraints order the systems that are registered and ignore the others")
	{
		ScopedSystems systems;
		// Neither TestLate nor TestEarly is registered yet: TestSoft runs where registration order puts it.
		REQUIRE(systems.Register(MakeSoftRecording("TestSoft", { "TestLate" }, { "TestEarly" })));
		CHECK(GetSystemNames() == std::vector<std::string> { "Scripting", "Animation", "Physics", "Audio", "TestSoft" });
		const std::vector<std::string> absent = SceneSystemRegistry::GetAbsentSoftConstraints();
		CHECK(absent == std::vector<std::string> { "TestSoft after TestLate", "TestSoft before TestEarly" });

		// Systems registered later pick up the constraints that name them: TestSoft moves after TestLate...
		REQUIRE(systems.Register(MakeRecording("TestLate")));
		CHECK(GetSystemNames() == std::vector<std::string> { "Scripting", "Animation", "Physics", "Audio", "TestLate", "TestSoft" });
		// ...and before TestEarly, wherever that was registered.
		REQUIRE(systems.Register(MakeRecording("TestEarly", {}, { "Physics" })));
		CHECK(GetSystemNames() == std::vector<std::string> { "Scripting", "Animation", "TestLate", "TestSoft", "TestEarly", "Physics", "Audio" });
		CHECK(SceneSystemRegistry::GetAbsentSoftConstraints().empty());

		// Soft constraints that close a cycle are refused like hard ones, and the registry stays unchanged.
		const std::vector<std::string> names = GetSystemNames();
		const uint64_t before = Log::GetBuffer().GetLatestSequence();
		CHECK_FALSE(SceneSystemRegistry::Register(MakeSoftRecording("TestLate", { "TestEarly" })));
		CHECK(FindError(before, "Scene system 'TestLate' is not registered: the update order of the scene systems has a cycle") != "");
		CHECK_FALSE(SceneSystemRegistry::Register(MakeSoftRecording("TestSelf", { "TestSelf" })));
		CHECK(FindError(before, "scene system 'TestSelf' names itself in its update order") != "");
		CHECK(GetSystemNames() == names);

		// Soft references never keep a system from going: TestSoft goes on without the systems it names.
		CHECK(SceneSystemRegistry::Unregister("TestLate"));
		std::erase(systems.Names, "TestLate");
		CHECK(GetSystemNames() == std::vector<std::string> { "Scripting", "Animation", "TestSoft", "TestEarly", "Physics", "Audio" });
		CHECK(SceneSystemRegistry::GetAbsentSoftConstraints() == std::vector<std::string> { "TestSoft after TestLate" });
	}
#endif

	TEST_CASE("The registry refuses changes while a scene runs")
	{
		const std::vector<std::string> names = GetSystemNames();
		const uint32_t runningBefore = Scene::GetRunningSceneCount();
		Scene scene;
		scene.OnRuntimeStart(SceneRuntimeMode::Simulate);
		CHECK(Scene::GetRunningSceneCount() == runningBefore + 1);

		const uint64_t before = Log::GetBuffer().GetLatestSequence();
		CHECK_FALSE(SceneSystemRegistry::Register(MakeRecording("TestA")));
		CHECK(FindError(before, "Scene system 'TestA' is not registered: ").find("scene(s) are running") != std::string::npos);
		CHECK_FALSE(SceneSystemRegistry::Unregister("Scripting"));
		CHECK(FindError(before, "Scene system 'Scripting' is not unregistered: ").find("scene(s) are running") != std::string::npos);
		CHECK(GetSystemNames() == names);

		scene.OnRuntimeStop();
		CHECK(Scene::GetRunningSceneCount() == runningBefore);
		REQUIRE(SceneSystemRegistry::Register(MakeRecording("TestA")));
		CHECK(SceneSystemRegistry::Unregister("TestA"));
		CHECK(GetSystemNames() == names);
	}

	TEST_CASE("A frame calls only the per-frame callbacks a system takes part in")
	{
		// MakeSceneSystemDescriptor asks for the callbacks the class overrides, also those it inherits from a base between it
		// and SceneSystem; a descriptor built by hand asks for all of them.
		const SceneSystemDescriptor lateOnly = MakeSceneSystemDescriptor<LateOnlySystem>("TestLate", false, [](Scene&) { return CreateScope<LateOnlySystem>(); });
		CHECK_FALSE(lateOnly.CallsUpdate);
		CHECK_FALSE(lateOnly.CallsFixedUpdate);
		CHECK(lateOnly.CallsLateUpdate);
		CHECK_FALSE(lateOnly.CallsPausedUpdate);
		const SceneSystemDescriptor derived = MakeSceneSystemDescriptor<OtherRecordingSystem>("TestDerived", false, [](Scene&) { return CreateScope<OtherRecordingSystem>("TestDerived"); });
		CHECK(derived.CallsUpdate);
		CHECK_FALSE(derived.CallsFixedUpdate);
		CHECK_FALSE(derived.CallsLateUpdate);
		const SceneSystemDescriptor typeless = MakeRecording("TestTypeless");
		CHECK(typeless.CallsUpdate);
		CHECK(typeless.CallsFixedUpdate);
		CHECK(typeless.CallsLateUpdate);
		CHECK(typeless.CallsPausedUpdate);

#if ST_TESTS_HAVE_EVERY_MODULE
		// The built-ins take part in seven calls of an idle frame: Scripting in all three phases, Animation in OnUpdate,
		// Physics in OnUpdate and OnFixedUpdate, Audio in OnLateUpdate; a paused frame calls Animation alone.
		const std::vector<SceneSystemDescriptor>& builtins = SceneSystemRegistry::GetAll();
		REQUIRE(builtins.size() == 4);
		CHECK((builtins[0].CallsUpdate && builtins[0].CallsFixedUpdate && builtins[0].CallsLateUpdate && !builtins[0].CallsPausedUpdate));
		CHECK((builtins[1].CallsUpdate && !builtins[1].CallsFixedUpdate && !builtins[1].CallsLateUpdate && builtins[1].CallsPausedUpdate));
		CHECK((builtins[2].CallsUpdate && builtins[2].CallsFixedUpdate && !builtins[2].CallsLateUpdate && !builtins[2].CallsPausedUpdate));
		CHECK((!builtins[3].CallsUpdate && !builtins[3].CallsFixedUpdate && builtins[3].CallsLateUpdate && !builtins[3].CallsPausedUpdate));
#endif

		// The scene calls what the descriptors ask for, in frames and in steps of a paused scene.
		PhaseCounts all;
		PhaseCounts withoutFixed;
		ScopedSystems systems;
		SceneSystemDescriptor allDescriptor;
		allDescriptor.Name = "TestAllPhases";
		allDescriptor.Create = [&all](Scene&) -> Scope<SceneSystem> { return CreateScope<PhaseCountingSystem>(all); };
		REQUIRE(systems.Register(std::move(allDescriptor)));
		SceneSystemDescriptor withoutFixedDescriptor;
		withoutFixedDescriptor.Name = "TestWithoutFixed";
		withoutFixedDescriptor.Create = [&withoutFixed](Scene&) -> Scope<SceneSystem> { return CreateScope<PhaseCountingSystem>(withoutFixed); };
		withoutFixedDescriptor.CallsFixedUpdate = false;
		withoutFixedDescriptor.CallsPausedUpdate = false;
		REQUIRE(systems.Register(std::move(withoutFixedDescriptor)));
		{
			Scene scene;
			scene.OnRuntimeStart();
			scene.OnUpdateRuntime(scene.GetSettings().FixedTimestep);
			scene.SetPaused(true);
			scene.Step(1);
			scene.OnUpdateRuntime(0.016f);
			// Paused frames without a step call the paused update instead.
			scene.OnUpdateRuntime(0.016f);
			scene.OnUpdateRuntime(0.016f);
			scene.OnRuntimeStop();
		}
		CHECK(all.Updates == 2);
		CHECK(all.FixedUpdates == 2);
		CHECK(all.LateUpdates == 2);
		CHECK(all.PausedUpdates == 2);
		CHECK(withoutFixed.Updates == 2);
		CHECK(withoutFixed.FixedUpdates == 0);
		CHECK(withoutFixed.LateUpdates == 2);
		CHECK(withoutFixed.PausedUpdates == 0);
	}

	TEST_CASE("GetSystem finds the running systems by their class")
	{
		ScopedSystems systems;
		REQUIRE(systems.Register(MakeSceneSystemDescriptor<RecordingSystem>("TestTyped", true, [](Scene&) { return CreateScope<RecordingSystem>("TestTyped"); })));

		Scene scene;
		CHECK(scene.GetSystem<ScriptSystem>() == nullptr);

		scene.OnRuntimeStart();
		CHECK(scene.GetSystem<ScriptSystem>() != nullptr);
#if ST_WITH_PHYSICS
		CHECK(scene.GetSystem<PhysicsSystem>() != nullptr);
#endif
#if ST_WITH_AUDIO
		CHECK(scene.GetSystem<AudioSystem>() != nullptr);
#endif
		CHECK(scene.GetSystem<RecordingSystem>() != nullptr);
		// Exact classes only: neither base classes nor unregistered classes are found.
		CHECK(scene.GetSystem<SceneSystem>() == nullptr);
		CHECK(scene.GetSystem<OtherRecordingSystem>() == nullptr);
		scene.OnRuntimeStop();
		CHECK(scene.GetSystem<ScriptSystem>() == nullptr);
		CHECK(scene.GetSystem<RecordingSystem>() == nullptr);

		// Simulate mode creates only the systems that run in it.
		scene.OnRuntimeStart(SceneRuntimeMode::Simulate);
		CHECK(scene.GetSystem<ScriptSystem>() == nullptr);
#if ST_WITH_PHYSICS
		CHECK(scene.GetSystem<PhysicsSystem>() != nullptr);
#endif
#if ST_WITH_AUDIO
		CHECK(scene.GetSystem<AudioSystem>() == nullptr);
#endif
		CHECK(scene.GetSystem<RecordingSystem>() != nullptr);
		scene.OnRuntimeStop();
	}
}
