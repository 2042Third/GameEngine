#include <doctest/doctest.h>

#include "Scripting/ScriptTestUtils.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Process.h"
#include "TestHelpers.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <optional>
#include <string>
#include <thread>
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

	// The engine's fault once symbolization (a job; inline without a job system) finished, or as it was after 30 s.
	std::optional<ScriptFault> WaitForSymbolizedFault(const ScriptEngine& engine)
	{
		std::optional<ScriptFault> fault = engine.GetFault();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (fault && !fault->Symbolized && std::chrono::steady_clock::now() < deadline)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			fault = engine.GetFault();
		}
		return fault;
	}

	// Line `line` (from 1) of a source file, empty when the file or the line does not exist.
	std::string ReadSourceLine(const std::string& file, uint32_t line)
	{
		const std::optional<std::string> source = FileSystem::ReadText(FileSystem::FromUTF8(file));
		if (!source || line == 0)
			return {};
		size_t start = 0;
		for (uint32_t current = 1; current < line; current++)
		{
			start = source->find('\n', start);
			if (start == std::string::npos)
				return {};
			start++;
		}
		const size_t end = source->find('\n', start);
		return source->substr(start, end == std::string::npos ? std::string::npos : end - start);
	}

	std::string DescribeStack(const std::vector<ScriptCodeLocation>& stack)
	{
		std::string text;
		for (const ScriptCodeLocation& frame : stack)
			text += "\n  " + DescribeScriptCodeLocation(frame) + (frame.Inlined ? " (inlined)" : "");
		return text;
	}

}

TEST_SUITE("Scripting.Faults")
{
	TEST_CASE("A crash names where it happened: the source file and line on Windows, the function elsewhere")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_FAULTS));
		Scene scene;
		CreateFaultyEntity(scene, "NullDereference", "OnUpdate");
		scene.OnRuntimeStart();
		RunFrames(scene, 1);
		REQUIRE(engine->IsFaulted());

		// Symbolization runs on a job (inline without a job system); the fault has its stack at once.
		const std::optional<ScriptFault> fault = WaitForSymbolizedFault(*engine);
		REQUIRE(fault);
		REQUIRE(fault->Symbolized);
		REQUIRE_FALSE(fault->Stack.empty());
		REQUIRE(fault->Location);
		const ScriptCodeLocation& location = *fault->Location;
		INFO("Location: ", DescribeScriptCodeLocation(location));
		CHECK(location.Module == FileSystem::ToUTF8(GetTestScriptModule(STRATA_TEST_SCRIPTS_FAULTS).filename()));
		CHECK(location.ModuleOffset != 0);
#if defined(ST_PLATFORM_WINDOWS)
		// The null write itself, from the module's PDB.
		CHECK(FileSystem::FromUTF8(location.File).filename() == "FaultScripts.cpp");
		CHECK(ReadSourceLine(location.File, location.Line).find("*pointer = 42;") != std::string::npos);
		CHECK_FALSE(location.Function.empty());
		CHECK(DescribeScriptCodeLocation(location).find("FaultScripts.cpp(" + std::to_string(location.Line) + ")") != std::string::npos);
#else
		// No DWARF reader: the function, from the module's symbol table.
		CHECK(location.File.empty());
		// The function that wrote, or the one it was inlined into (optimized builds).
		const std::string& function = location.Function;
		CHECK((function.find("Crash") != std::string::npos || function.find("Faulty") != std::string::npos));
#endif
		scene.OnRuntimeStop();
	}

	TEST_CASE("A crash in code the compiler inlined names the line that ran, then the call it was inlined at")
	{
		// The Faults module's WriteThroughNull is forced inline wherever the compiler inlines (Release, Dist): its null write
		// then runs inside Crash, at an address whose debug information knows both lines.
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_FAULTS));
		Scene scene;
		CreateFaultyEntity(scene, "InlinedNullDereference", "OnUpdate");
		scene.OnRuntimeStart();
		RunFrames(scene, 1);
		REQUIRE(engine->IsFaulted());

		const std::optional<ScriptFault> fault = WaitForSymbolizedFault(*engine);
		REQUIRE(fault);
		REQUIRE(fault->Symbolized);
		REQUIRE(fault->Location);
		const ScriptCodeLocation& location = *fault->Location;
		INFO("Location: ", DescribeScriptCodeLocation(location), "\nStack:", DescribeStack(fault->Stack));
#if defined(ST_PLATFORM_WINDOWS)
		CHECK(ReadSourceLine(location.File, location.Line).find("*pointer = 7;") != std::string::npos);
		CHECK(location.Function.find("WriteThroughNull") != std::string::npos);
		// The call follows: the caller's own frame where nothing was inlined, the frame of the function the write was inlined
		// into otherwise (at the same address).
		const auto call = std::find_if(fault->Stack.begin(), fault->Stack.end(), [](const ScriptCodeLocation& frame)
		{
			return ReadSourceLine(frame.File, frame.Line).find("WriteThroughNull();") != std::string::npos;
		});
		REQUIRE(call != fault->Stack.end());
		CHECK(call->Function.find("Crash") != std::string::npos);
		const auto write = std::find_if(fault->Stack.begin(), fault->Stack.end(), [&](const ScriptCodeLocation& frame)
		{
			return frame.Address == location.Address && frame.Line == location.Line;
		});
		REQUIRE(write != fault->Stack.end());
		CHECK(write < call);
		if (location.Inlined)
			CHECK(call->Address == location.Address);
#else
		// No DWARF reader: the function holding the write, which is the caller when the write was inlined.
		CHECK(location.File.empty());
		const std::string& function = location.Function;
		CHECK((function.find("WriteThroughNull") != std::string::npos || function.find("Crash") != std::string::npos || function.find("Faulty") != std::string::npos));
#endif
		scene.OnRuntimeStop();
	}

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
