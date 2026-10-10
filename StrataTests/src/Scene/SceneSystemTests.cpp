#include <doctest/doctest.h>

#include "Strata/Audio/AudioSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSystem.h"
#include "Strata/Scripting/ScriptSystem.h"

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
	TEST_CASE("The built-in systems update in the order Scripting, Physics, Audio")
	{
		CHECK(GetSystemNames() == std::vector<std::string> { "Scripting", "Physics", "Audio" });

		const std::vector<SceneSystemDescriptor>& descriptors = SceneSystemRegistry::GetAll();
		REQUIRE(descriptors.size() == 3);
		CHECK(descriptors[0].After.empty());
		CHECK(descriptors[1].After == std::vector<std::string> { "Scripting" });
		CHECK(descriptors[2].After == std::vector<std::string> { "Physics" });
		CHECK(descriptors[0].Type == entt::type_id<ScriptSystem>().hash());
		CHECK(descriptors[1].Type == entt::type_id<PhysicsSystem>().hash());
		CHECK(descriptors[2].Type == entt::type_id<AudioSystem>().hash());
	}

	TEST_CASE("After and Before are honored, and registration order decides the rest")
	{
		RecordingSystem::Updates.clear();
		ScopedSystems systems;
		REQUIRE(systems.Register(MakeRecording("TestA")));
		REQUIRE(systems.Register(MakeRecording("TestB", {}, { "TestA" })));
		REQUIRE(systems.Register(MakeRecording("TestC", { "TestA" }, { "Audio" })));
		REQUIRE(systems.Register(MakeRecording("TestD")));

		// TestB goes before TestA, TestC between TestA and Audio; unconstrained choices follow registration order.
		CHECK(GetSystemNames() == std::vector<std::string> { "Scripting", "Physics", "TestB", "TestA", "TestC", "Audio", "TestD" });

		{
			Scene scene;
			scene.OnRuntimeStart();
			scene.OnUpdateRuntime(0.016f);
			scene.OnRuntimeStop();
		}
		CHECK(RecordingSystem::Updates == std::vector<std::string> { "TestB", "TestA", "TestC", "TestD" });

		// A system registered again with the same name replaces the old one and counts as registered last.
		REQUIRE(SceneSystemRegistry::Register(MakeRecording("TestA")));
		CHECK(GetSystemNames() == std::vector<std::string> { "Scripting", "Physics", "TestB", "TestD", "TestA", "TestC", "Audio" });
	}

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
		CHECK_FALSE(SceneSystemRegistry::Unregister("Audio"));
		CHECK(FindError(before, "Scene system 'Audio' is not unregistered: ").find("scene(s) are running") != std::string::npos);
		CHECK(GetSystemNames() == names);

		scene.OnRuntimeStop();
		CHECK(Scene::GetRunningSceneCount() == runningBefore);
		REQUIRE(SceneSystemRegistry::Register(MakeRecording("TestA")));
		CHECK(SceneSystemRegistry::Unregister("TestA"));
		CHECK(GetSystemNames() == names);
	}

	TEST_CASE("GetSystem finds the running systems by their class")
	{
		ScopedSystems systems;
		REQUIRE(systems.Register(MakeSceneSystemDescriptor<RecordingSystem>("TestTyped", true, [](Scene&) { return CreateScope<RecordingSystem>("TestTyped"); })));

		Scene scene;
		CHECK(scene.GetSystem<PhysicsSystem>() == nullptr);

		scene.OnRuntimeStart();
		CHECK(scene.GetSystem<ScriptSystem>() != nullptr);
		CHECK(scene.GetSystem<PhysicsSystem>() != nullptr);
		CHECK(scene.GetSystem<AudioSystem>() != nullptr);
		CHECK(scene.GetSystem<RecordingSystem>() != nullptr);
		// Exact classes only: neither base classes nor unregistered classes are found.
		CHECK(scene.GetSystem<SceneSystem>() == nullptr);
		CHECK(scene.GetSystem<OtherRecordingSystem>() == nullptr);
		scene.OnRuntimeStop();
		CHECK(scene.GetSystem<PhysicsSystem>() == nullptr);
		CHECK(scene.GetSystem<RecordingSystem>() == nullptr);

		// Simulate mode creates only the systems that run in it.
		scene.OnRuntimeStart(SceneRuntimeMode::Simulate);
		CHECK(scene.GetSystem<ScriptSystem>() == nullptr);
		CHECK(scene.GetSystem<PhysicsSystem>() != nullptr);
		CHECK(scene.GetSystem<AudioSystem>() == nullptr);
		CHECK(scene.GetSystem<RecordingSystem>() != nullptr);
		scene.OnRuntimeStop();
	}
}
