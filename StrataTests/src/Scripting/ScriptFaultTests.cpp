#include <doctest/doctest.h>

#include "Scripting/ScriptTestUtils.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Process.h"
#include "TestHelpers.h"

#include <chrono>
#include <csignal>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	std::vector<const char*> GetFaultKinds()
	{
		std::vector<const char*> faults = { "NullDereference", "StackOverflow" };
#if defined(ST_PLATFORM_WINDOWS)
		// abort(): directly, from a failed assert() (active in every configuration) and from std::terminate. Only Windows
		// contains it; elsewhere it ends the process (see "abort() in a script ...").
		faults.insert(faults.end(), { "Abort", "Assert", "Terminate", "TerminateFromNoexcept" });
#endif
#if !defined(__aarch64__) && !defined(_M_ARM64)
		// Integer division by zero does not trap on ARM64 (it returns 0).
		faults.push_back("DivideByZero");
#endif
		return faults;
	}

	Entity CreateFaultyEntity(Scene& scene, const char* fault, const char* callback)
	{
		Entity entity = scene.CreateEntity("Faulty");
		ScriptEntry& entry = AddScriptEntry(entity, "Faulty");
		AddFieldOverride(entry, "Fault", PropertyType::String, std::string(fault));
		AddFieldOverride(entry, "FaultIn", PropertyType::String, std::string(callback));
		return entity;
	}

}

TEST_SUITE("Scripting.Faults")
{
	TEST_CASE("Crashes in scripts are contained")
	{
		const char* callbacks[] = { "OnCreate", "OnUpdate", "OnFixedUpdate", "OnLateUpdate", "OnDestroy" };
		for (const char* fault : GetFaultKinds())
		{
			for (const char* callback : callbacks)
			{
				INFO(fault, " in ", callback);
				ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_FAULTS));
				Scene scene;
				scene.GetSettings().FixedTimestep = 0.25f;
				Entity healthy = scene.CreateEntity("Healthy");
				AddScriptEntry(healthy, "Healthy");
				const Entity faulty = CreateFaultyEntity(scene, fault, callback);
				AddScriptEntry(scene.CreateEntity("Bystander"), "Healthy");

				scene.OnRuntimeStart();
				RunFrames(scene, 2, 0.25f); // One fixed step per frame
				const bool faultsWhilePlaying = std::string(callback) != "OnDestroy";
				CHECK(engine->IsFaulted() == faultsWhilePlaying);
				if (faultsWhilePlaying)
				{
					// The module is inert: none of its scripts is called (or even readable) any more.
					CHECK(GetScriptSystem(scene).GetInstanceCount() > 0);
					CHECK_FALSE(GetScriptSystem(scene).GetFieldValue(healthy, "Healthy", "Updates").has_value());
				}

				// The engine keeps running: the scene updates and stops normally.
				RunFrames(scene, 3, 0.25f);
				scene.OnRuntimeStop();
				CHECK(scene.GetEntityCount() == 3);
				CHECK_FALSE(scene.IsRunning());

				REQUIRE(engine->IsFaulted());
				CHECK(engine->GetFaultCount() == 1);
				const std::optional<ScriptFault> report = engine->GetFault();
				REQUIRE(report.has_value());
				CHECK(report->ModuleName == "StrataTestScriptsFaults");
				CHECK(report->ClassName == "Faulty");
				CHECK(report->Method == callback);
				CHECK(report->Entity == faulty.GetUUID());
				CHECK(report->EntityName == "Faulty");
				CHECK_FALSE(report->Description.empty());
				if (std::string(fault) != "NullDereference" && std::string(fault) != "StackOverflow" && std::string(fault) != "DivideByZero")
					CHECK(report->Description.find("abort()") != std::string::npos);

				// A faulted module refuses further work but can still be unloaded.
				engine->UnloadModule();
				CHECK_FALSE(engine->IsModuleLoaded());
				CHECK(engine->GetFaultCount() == 1);
			}
		}
	}

	TEST_CASE("abort() in a script is contained on Windows and ends the process with a report elsewhere")
	{
		// In a child process, since it may end. POSIX cannot contain abort() safely: the C library also aborts this way
		// on heap corruption, holding allocator locks that would never be released.
		for (const char* fault : { "Abort", "Assert", "Terminate" })
		{
			ProcessSpecification specification;
			specification.Executable = GetTestExecutablePath();
			specification.Arguments = { "--strata-test-helper=play-faulty-script", FileSystem::ToUTF8(GetTestScriptModule(STRATA_TEST_SCRIPTS_FAULTS)), fault };
			const Process::RunResult result = Process::Run(specification, std::chrono::milliseconds(60000));
			INFO("Fault ", fault, ", output: ", result.Output);
			REQUIRE(result.Started);
			CHECK_FALSE(result.TimedOut);
#if defined(ST_PLATFORM_WINDOWS)
			CHECK(result.ExitCode == 0);
			CHECK(result.Output.find("contained") != std::string::npos);
#else
			CHECK(result.ExitCode == 128 + SIGABRT);
			CHECK(result.Output.find("called abort()") != std::string::npos);
#endif
		}
	}

	TEST_CASE("A crash in a nested call faults the module")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_FAULTS));
		Scene scene;
		Entity entity = scene.CreateEntity("Nested");
		AddScriptEntry(entity, "NestedFault");
		scene.OnRuntimeStart();
		scene.OnUpdateRuntime(0.0f);

		REQUIRE(engine->IsFaulted());
		const std::optional<ScriptFault> report = engine->GetFault();
		REQUIRE(report.has_value());
		CHECK(report->ClassName == "FaultInConstructor");
		CHECK(report->Method == "Create");
		CHECK(report->Entity == entity.GetUUID());
		CHECK_FALSE(GetScriptSystem(scene).HasInstance(entity, "FaultInConstructor"));
		RunFrames(scene, 2);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Other scenes and engines are unaffected by a crash")
	{
		// Scene A runs the crashing module, scene B a healthy module in another engine.
		Ref<ScriptEngine> crashing = CreateRef<ScriptEngine>();
		REQUIRE(crashing->LoadModule(GetTestScriptModule(STRATA_TEST_SCRIPTS_FAULTS)));
		Ref<ScriptEngine> healthy = CreateRef<ScriptEngine>();
		REQUIRE(healthy->LoadModule(GetTestScriptModule(STRATA_TEST_SCRIPTS_API)));

		Scene sceneA;
		CreateFaultyEntity(sceneA, "NullDereference", "OnUpdate");
		Entity moving = sceneA.CreateEntity("Moving");
		Scene sceneB;
		Entity counter = sceneB.CreateEntity("Counter");
		ScriptEntry& entry = AddScriptEntry(counter, "Lifecycle");
		AddFieldOverride(entry, "RecordUpdates", PropertyType::Bool, false);

		const Ref<ScriptEngine> previous = ScriptEngine::GetActive();
		ScriptEngine::SetActive(crashing);
		sceneA.OnRuntimeStart();
		ScriptEngine::SetActive(healthy);
		sceneB.OnRuntimeStart();
		ScriptEngine::SetActive(previous);

		for (int frame = 0; frame < 3; frame++)
		{
			sceneA.OnUpdateRuntime(1.0f / 60.0f);
			sceneB.OnUpdateRuntime(1.0f / 60.0f);
		}
		CHECK(crashing->IsFaulted());
		CHECK_FALSE(healthy->IsFaulted());
		CHECK(GetField<int32_t>(GetScriptSystem(sceneB), counter, "Lifecycle", "Updates") == 3);

		// The crashed scene itself still works.
		moving.GetComponent<TransformComponent>().Translation = { 1.0f, 2.0f, 3.0f };
		moving.MarkModified<TransformComponent>();
		sceneA.OnUpdateRuntime(1.0f / 60.0f);
		CHECK(moving.GetComponent<WorldTransformComponent>().Matrix[3] == glm::vec4(1.0f, 2.0f, 3.0f, 1.0f));

		sceneA.OnRuntimeStop();
		sceneB.OnRuntimeStop();
	}

	TEST_CASE("A module that crashes while loading is refused")
	{
		ScriptEngine engine;
		std::string error;
		CHECK_FALSE(engine.LoadModule(GetTestScriptModule(STRATA_TEST_SCRIPTS_LOADFAULT), &error));
		CHECK(error.find("crashed") != std::string::npos);
		CHECK_FALSE(engine.IsModuleLoaded());
		CHECK_FALSE(engine.IsFaulted());
	}

	TEST_CASE("Reloading a crashed module recovers")
	{
		const std::filesystem::path path = CreateTemporaryDirectory("ScriptFaultReload") / FileSystem::FromUTF8(ScriptEngine::GetModuleFileName("Game"));
		REQUIRE(FileSystem::Copy(GetTestScriptModule(STRATA_TEST_SCRIPTS_FAULTS), path));
		ScopedScriptEngine engine(path);

		Scene scene;
		Entity healthy = scene.CreateEntity("Healthy");
		AddScriptEntry(healthy, "Healthy");
		Entity faulty = CreateFaultyEntity(scene, "NullDereference", "OnUpdate");
		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		scene.OnUpdateRuntime(0.0f);
		REQUIRE(engine->IsFaulted());

		// "Fix the bug" (remove the crashing script) and reload: the scripts start over, their old state is lost.
		faulty.RemoveComponent<ScriptComponent>();
		REQUIRE(engine->Reload());
		CHECK_FALSE(engine->IsFaulted());
		CHECK(engine->GetFaultCount() == 1);
		scene.OnUpdateRuntime(0.0f);
		CHECK(system.GetInstanceCount() == 1);
		CHECK(GetField<int32_t>(system, healthy, "Healthy", "Creates") == 1);
		CHECK(GetField<int32_t>(system, healthy, "Healthy", "Updates") == 1);
		CHECK_FALSE(system.HasInstance(faulty, "Faulty"));
		scene.OnRuntimeStop();
	}

	TEST_CASE("The watchdog reports long-running script calls")
	{
		// Calls that return at once stay far below the timeout even on a busy machine; the slow call stays far above it
		// (the watchdog checks every quarter of the timeout).
		constexpr std::chrono::milliseconds c_Timeout(250);
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_FAULTS));
		CHECK(engine->GetWatchdogTimeout().count() == 0);
		engine->SetWatchdogTimeout(c_Timeout);
		CHECK(engine->GetWatchdogTimeout() == c_Timeout);

		Scene scene;
		Entity entity = scene.CreateEntity("Slow");
		ScriptEntry& entry = AddScriptEntry(entity, "Slow");
		AddFieldOverride(entry, "Milliseconds", PropertyType::Int, int32_t(0));
		scene.OnRuntimeStart();
		scene.OnUpdateRuntime(0.0f);
		CHECK(engine->GetWatchdogReportCount() == 0);
		CHECK(engine->GetWatchdogActiveCallCount() == 0);

		// A call far longer than the timeout is reported (once), and it still completes normally.
		REQUIRE(GetScriptSystem(scene).SetFieldValue(entity, "Slow", "Milliseconds", int32_t(c_Timeout.count() * 6)));
		scene.OnUpdateRuntime(0.0f);
		CHECK(engine->GetWatchdogReportCount() == 1);
		CHECK(engine->GetWatchdogActiveCallCount() == 0);
		CHECK_FALSE(engine->IsFaulted());

		engine->SetWatchdogTimeout(std::chrono::milliseconds(0));
		CHECK(engine->GetWatchdogReportCount() == 0);
		scene.OnRuntimeStop();
	}
}

TEST_SUITE("Scripting.Exceptions")
{
	TEST_CASE("Exceptions disable only the instance that threw")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		CreateLogEntity(scene);
		auto addThrower = [&](const char* name, const char* throwIn)
		{
			Entity entity = scene.CreateEntity(name);
			AddFieldOverride(AddScriptEntry(entity, "Thrower"), "ThrowIn", PropertyType::String, std::string(throwIn));
			return entity;
		};
		const Entity inCreate = addThrower("ThrowsInCreate", "OnCreate");
		const Entity inUpdate = addThrower("ThrowsInUpdate", "OnUpdate");
		const Entity nonStandard = addThrower("ThrowsNonStandard", "NonStandard");
		const Entity inDestroy = addThrower("ThrowsInDestroy", "OnDestroy");
		Entity inConstructor = scene.CreateEntity("ThrowsInConstructor");
		AddScriptEntry(inConstructor, "ThrowingConstructor");
		Entity healthy = scene.CreateEntity("Healthy");
		AddFieldOverride(AddScriptEntry(healthy, "Lifecycle"), "RecordUpdates", PropertyType::Bool, false);

		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		CHECK_FALSE(system.HasInstance(inConstructor, "ThrowingConstructor"));
		RunFrames(scene, 3);

		CHECK_FALSE(engine->IsFaulted());
		CHECK(engine->GetFaultCount() == 0);
		CHECK(GetField<int32_t>(system, inCreate, "Thrower", "Updates") == 0);
		CHECK(GetField<int32_t>(system, inUpdate, "Thrower", "Updates") == 1);
		CHECK(GetField<int32_t>(system, nonStandard, "Thrower", "Updates") == 1);
		CHECK(GetField<int32_t>(system, inDestroy, "Thrower", "Updates") == 3);
		CHECK(GetField<int32_t>(system, healthy, "Lifecycle", "Updates") == 3);

		// Disabled instances get no OnDestroy; an exception in OnDestroy is reported like any other.
		ClearLog(scene);
		scene.OnRuntimeStop();
		CHECK(GetLog(scene) == "Healthy.Lifecycle.Destroy;ThrowsInDestroy.Thrower.Destroy;");
		CHECK_FALSE(engine->IsFaulted());
	}
}
