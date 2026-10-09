#include <doctest/doctest.h>

#include "Scripting/ScriptTestUtils.h"
#include "Strata/Core/CrashGuard.h"
#include "Strata/Core/DynamicLibrary.h"
#include "Strata/Core/FileLock.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/Process.h"
#include "Strata/Core/StringUtils.h"
#include "Strata/Scripting/ScriptHostAPI.h"
#include "Strata/Scripting/ScriptModule.h"
#include "TestHelpers.h"

#include "StrataScript/ScriptABI.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <set>
#include <thread>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	std::vector<uint8_t> CreateGarbage(size_t size)
	{
		std::vector<uint8_t> bytes(size);
		uint32_t state = 0x12345678u;
		for (uint8_t& byte : bytes)
		{
			state = state * 1664525u + 1013904223u;
			byte = static_cast<uint8_t>(state >> 24);
		}
		return bytes;
	}

	bool Contains(const std::string& text, const std::string& part)
	{
		return text.find(part) != std::string::npos;
	}

	// Restores an environment variable read by the Malformed test module.
	struct ScopedMalformedCase
	{
		explicit ScopedMalformedCase(const std::string& testCase)
		{
			Platform::SetEnvVar("STRATA_TEST_MALFORMED_CASE", testCase);
		}

		~ScopedMalformedCase()
		{
			Platform::SetEnvVar("STRATA_TEST_MALFORMED_CASE", "");
		}
	};

}

TEST_SUITE("Scripting.Module")
{
	TEST_CASE("Modules load and describe their classes and fields")
	{
		ScriptEngine engine;
		CHECK_FALSE(engine.IsModuleLoaded());
		CHECK(engine.GetClasses().empty());
		CHECK(engine.GetModuleName().empty());
		CHECK_FALSE(engine.IsFaulted());

		std::string error;
		REQUIRE_MESSAGE(engine.LoadModule(GetTestScriptModule(STRATA_TEST_SCRIPTS_API), &error), error);
		CHECK(engine.IsModuleLoaded());
		CHECK(engine.GetLoadCount() == 1);
		CHECK(engine.GetModuleName() == "StrataTestScriptsAPI");
		CHECK(engine.GetModulePath().is_absolute());
		CHECK(engine.GetModulePath().filename() == FileSystem::FromUTF8(STRATA_TEST_SCRIPTS_API));

		const std::set<std::string> expectedClasses = {
			"Lifecycle", "LifecycleSecond", "Idle", "HiddenCallbacks", "FieldTypes", "Thrower", "ThrowingConstructor", "EntityAPI", "Destroyer", "ScriptAdder",
			"ComponentAPI", "PropertyProbe", "TransformAPI", "Spawned", "Spawner", "Listener", "Talker", "InputProbe", "TimeProbe", "SceneProbe",
			"RemoveOnDestroy", "Fragile", "Readder", "Replicator", "MassSpawner", "PendingSpawner", "InvalidArguments"
		};
		std::set<std::string> classes;
		for (const ScriptClassInfo& info : engine.GetClasses())
			classes.insert(info.Name);
		CHECK(classes == expectedClasses);
		CHECK(engine.FindClass("lifecycle") == nullptr); // Class names are case-sensitive
		CHECK(engine.FindClass("NoSuchClass") == nullptr);

		const ScriptClassInfo* fieldTypes = engine.FindClass("FieldTypes");
		REQUIRE(fieldTypes);
		struct ExpectedField
		{
			const char* Name;
			PropertyType Type;
			PropertyValue Default;
		};
		const ExpectedField expectedFields[] = {
			{ "BoolField", PropertyType::Bool, true },
			{ "IntField", PropertyType::Int, int32_t(42) },
			{ "FloatField", PropertyType::Float, 1.5f },
			{ "Vec2Field", PropertyType::Vec2, glm::vec2(1.0f, 2.0f) },
			{ "Vec3Field", PropertyType::Vec3, glm::vec3(1.0f, 2.0f, 3.0f) },
			{ "Vec4Field", PropertyType::Vec4, glm::vec4(1.0f, 2.0f, 3.0f, 4.0f) },
			{ "QuatField", PropertyType::Quat, glm::quat(0.70710678f, 0.0f, 0.70710678f, 0.0f) },
			{ "StringField", PropertyType::String, std::string("Hello") },
			{ "EntityField", PropertyType::Entity, UUID::Null() },
			{ "AssetField", PropertyType::Asset, UUID(0x1234) },
			{ "IntSeenInCreate", PropertyType::Int, int32_t(0) },
			{ "StringSeenInCreate", PropertyType::String, std::string() }
		};
		REQUIRE(fieldTypes->Fields.size() == std::size(expectedFields));
		for (size_t index = 0; index < std::size(expectedFields); index++)
		{
			const ScriptFieldInfo& field = fieldTypes->Fields[index];
			INFO("Field ", field.Name);
			CHECK(field.Name == expectedFields[index].Name);
			CHECK(field.Type == expectedFields[index].Type);
			CHECK(field.DefaultValue == expectedFields[index].Default);
		}
		CHECK(fieldTypes->FindField("IntField") == &fieldTypes->Fields[1]);
		CHECK(fieldTypes->FindFieldIndex("Missing") == UINT32_MAX);
		CHECK(fieldTypes->Implements(ScriptCallback::OnCreate));
		CHECK_FALSE(fieldTypes->Implements(ScriptCallback::OnUpdate));

		const ScriptClassInfo* lifecycle = engine.FindClass("Lifecycle");
		REQUIRE(lifecycle);
		for (ScriptCallback callback : { ScriptCallback::OnCreate, ScriptCallback::OnUpdate, ScriptCallback::OnFixedUpdate, ScriptCallback::OnLateUpdate, ScriptCallback::OnDestroy })
			CHECK(lifecycle->Implements(callback));
		CHECK_FALSE(lifecycle->Implements(ScriptCallback::OnReload));
		// Inherited implementations count.
		CHECK(engine.FindClass("LifecycleSecond")->Implements(ScriptCallback::OnUpdate));

		const ScriptClassInfo* idle = engine.FindClass("Idle");
		REQUIRE(idle);
		CHECK(idle->Callbacks == 0);
		CHECK(idle->Fields.empty());

		CHECK(StringUtils::EndsWith(ScriptEngine::GetModuleFileName("Game"), DynamicLibrary::GetFileExtension()));
		CHECK(StringUtils::StartsWith(ScriptEngine::GetModuleFileName("Game"), "Game."));

		engine.UnloadModule();
		CHECK_FALSE(engine.IsModuleLoaded());
		CHECK(engine.GetClasses().empty());
		CHECK(engine.GetModulePath().empty());
	}

	TEST_CASE("Modules built against another ABI version are refused")
	{
		ScriptEngine engine;
		std::string error;
		CHECK_FALSE(engine.LoadModule(GetTestScriptModule(STRATA_TEST_SCRIPTS_ABIMISMATCH), &error));
		CHECK(Contains(error, fmt::format("version {}", ST_SCRIPT_ABI_VERSION + 1)));
		CHECK(Contains(error, fmt::format("uses version {}", ST_SCRIPT_ABI_VERSION)));
		CHECK_FALSE(engine.IsModuleLoaded());
		CHECK_FALSE(engine.IsFaulted());
	}

	TEST_CASE("Modules of a newer SDK with a larger module description load")
	{
		ScriptEngine engine;
		std::string error;
		REQUIRE_MESSAGE(engine.LoadModule(GetTestScriptModule(STRATA_TEST_SCRIPTS_NEWERSDK), &error), error);
		CHECK(engine.GetModuleName() == "NewerSDK");
		const ScriptClassInfo* probe = engine.FindClass("Probe");
		REQUIRE(probe);
		REQUIRE(probe->Fields.size() == 1);
		CHECK(probe->Fields[0].DefaultValue == PropertyValue(int32_t(7)));
	}

	TEST_CASE("Modules write no more of their description than the engine's struct holds")
	{
		// The engine's struct followed by memory the module must not touch.
		struct GuardedModuleAPI
		{
			StrataScriptModuleAPI API;
			uint8_t Guard[64];
		};
		auto guardIntact = [](const GuardedModuleAPI& guarded)
		{
			return std::all_of(std::begin(guarded.Guard), std::end(guarded.Guard), [](uint8_t byte) { return byte == 0xA5; });
		};

		for (const char* moduleFile : { STRATA_TEST_SCRIPTS_API, STRATA_TEST_SCRIPTS_NEWERSDK })
		{
			INFO("Module ", moduleFile);
			DynamicLibrary library;
			REQUIRE_MESSAGE(library.Load(GetTestScriptModule(moduleFile)), library.GetLastError());
			const auto load = library.GetFunction<StrataScriptLoadFunction>(ST_SCRIPT_LOAD_SYMBOL);
			REQUIRE(load);

			GuardedModuleAPI guarded;
			std::memset(&guarded, 0xA5, sizeof(guarded));
			guarded.API.StructSize = sizeof(StrataScriptModuleAPI);
			REQUIRE(load(&GetScriptHostAPI(), ST_SCRIPT_ABI_VERSION, &guarded.API) == StrataScriptResult_Ok);
			CHECK(guardIntact(guarded));
			// StructSize reports the members the module knows: a newer SDK knows more than this engine.
			if (std::string(moduleFile) == STRATA_TEST_SCRIPTS_NEWERSDK)
				CHECK(guarded.API.StructSize > sizeof(StrataScriptModuleAPI));
			else
				CHECK(guarded.API.StructSize == sizeof(StrataScriptModuleAPI));
			CHECK(guarded.API.ABIVersion == ST_SCRIPT_ABI_VERSION);
			CHECK(guarded.API.ClassCount > 0);
			REQUIRE(guarded.API.Unload);
			CHECK(guarded.API.Unload() == StrataScriptResult_Ok);

			// A struct without room for the members of this ABI version is refused, and nothing is written.
			std::memset(&guarded, 0xA5, sizeof(guarded));
			guarded.API.StructSize = static_cast<uint32_t>(offsetof(StrataScriptModuleAPI, Unload));
			CHECK(load(&GetScriptHostAPI(), ST_SCRIPT_ABI_VERSION, &guarded.API) == StrataScriptResult_ABIMismatch);
			CHECK(guarded.API.StructSize == offsetof(StrataScriptModuleAPI, Unload));
			CHECK(guardIntact(guarded));
		}
	}

	TEST_CASE("Missing files, garbage and foreign libraries are refused")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("ScriptGarbage");
		ScriptEngine engine;
		std::string error;

		CHECK_FALSE(engine.LoadModule(directory / FileSystem::FromUTF8(ScriptEngine::GetModuleFileName("Missing")), &error));
		CHECK(Contains(error, "does not exist"));
		CHECK_FALSE(engine.LoadModule(directory, &error));

		const std::filesystem::path garbage = directory / FileSystem::FromUTF8(ScriptEngine::GetModuleFileName("Garbage"));
		REQUIRE(FileSystem::WriteBytes(garbage, CreateGarbage(64 * 1024)));
		error.clear();
		CHECK_FALSE(engine.LoadModule(garbage, &error));
		CHECK_FALSE(error.empty());

		const std::filesystem::path empty = directory / FileSystem::FromUTF8(ScriptEngine::GetModuleFileName("Empty"));
		REQUIRE(FileSystem::WriteBytes(empty, {}));
		CHECK_FALSE(engine.LoadModule(empty, &error));

		// The first bytes of a real module: a valid header with nothing behind it.
		const std::optional<std::vector<uint8_t>> module = FileSystem::ReadBytes(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		REQUIRE(module.has_value());
		const std::filesystem::path truncated = directory / FileSystem::FromUTF8(ScriptEngine::GetModuleFileName("Truncated"));
		REQUIRE(FileSystem::WriteBytes(truncated, std::span<const uint8_t>(module->data(), 4096)));
		CHECK_FALSE(engine.LoadModule(truncated, &error));

		error.clear();
		CHECK_FALSE(engine.LoadModule(GetTestScriptModule(STRATA_TEST_LIBRARY_NAME), &error));
		CHECK(Contains(error, "not a Strata script module"));

		CHECK_FALSE(engine.IsModuleLoaded());
		CHECK(engine.GetFaultCount() == 0);
	}

	TEST_CASE("Malformed class tables are refused")
	{
		const char* cases[] = {
			"DuplicateClass", "EmptyClassName", "MissingCreate", "NullClass", "SmallClass", "BadFieldType", "BadDefault", "DuplicateField",
			"SmallModule", "TooManyClasses", "NullClassList", "Exception", "Rejected"
		};
		for (const char* testCase : cases)
		{
			INFO("Case ", testCase);
			ScopedMalformedCase scopedCase(testCase);
			ScriptEngine engine;
			std::string error;
			CHECK_FALSE(engine.LoadModule(GetTestScriptModule(STRATA_TEST_SCRIPTS_MALFORMED), &error));
			CHECK_FALSE(error.empty());
			CHECK_FALSE(engine.IsModuleLoaded());
			if (std::string(testCase) == "Exception")
				CHECK(Contains(error, "Broken on purpose"));
		}

		// Without a defect the hand-written module is valid: the C ABI works without the SDK.
		ScriptEngine engine;
		std::string error;
		REQUIRE_MESSAGE(engine.LoadModule(GetTestScriptModule(STRATA_TEST_SCRIPTS_MALFORMED), &error), error);
		CHECK(engine.GetModuleName() == "Malformed");
		REQUIRE(engine.GetClasses().size() == 2);
		const ScriptClassInfo* second = engine.FindClass("Second");
		REQUIRE(second);
		REQUIRE(second->Fields.size() == 1);
		CHECK(second->Fields[0].Name == "B");
		CHECK(second->Fields[0].DefaultValue == PropertyValue(int32_t(1)));
		CHECK(second->Callbacks == 0);
	}

	TEST_CASE("Exceptions escaping a module without the SDK are contained like crashes")
	{
		// Script calls are bracketed for the host API and the watchdog; an exception must not leave a call open (the
		// watchdog would report it once the timeout passed).
		constexpr std::chrono::milliseconds c_WatchdogTimeout(200);
		{
			ScopedMalformedCase scopedCase("Throws");
			ScriptEngine engine;
			engine.SetWatchdogTimeout(c_WatchdogTimeout);
			std::string error;
			CHECK_FALSE(engine.LoadModule(GetTestScriptModule(STRATA_TEST_SCRIPTS_MALFORMED), &error));
			CHECK(Contains(error, "crashed while initializing"));
			CHECK(Contains(error, "C++ exception"));
			CHECK(ScriptModule::GetCurrentCall() == nullptr);
			std::this_thread::sleep_for(c_WatchdogTimeout * 3);
			CHECK(engine.GetWatchdogReportCount() == 0);
		}
		{
			ScopedMalformedCase scopedCase("ThrowsInCreate");
			ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_MALFORMED));
			engine->SetWatchdogTimeout(c_WatchdogTimeout);
			Scene scene;
			Entity entity = scene.CreateEntity("Entity");
			AddScriptEntry(entity, "First");
			scene.OnRuntimeStart();
			CHECK(ScriptModule::GetCurrentCall() == nullptr);
			REQUIRE(engine->IsFaulted());
			const std::optional<ScriptFault> fault = engine->GetFault();
			REQUIRE(fault.has_value());
			CHECK(fault->ClassName == "First");
			CHECK(fault->Method == "Create");
			CHECK(fault->Entity == entity.GetUUID());
			CHECK(Contains(fault->Description, "C++ exception"));
			RunFrames(scene, 1);
			scene.OnRuntimeStop();
			std::this_thread::sleep_for(c_WatchdogTimeout * 3);
			CHECK(engine->GetWatchdogReportCount() == 0);
		}

		// The crash guard still contains faults afterwards.
		CrashInfo crash;
		CHECK_FALSE(CrashGuard::Invoke([](void*)
		{
			volatile int* pointer = nullptr;
			*pointer = 42;
		}, nullptr, &crash));
	}

	TEST_CASE("Crashes in a module's static initialization and destruction are contained")
	{
		// In a child process: a crash inside the platform's loader may leave it in an undefined state (see ScriptEngine).
		auto run = [](const char* testCase)
		{
			ScopedMalformedCase scopedCase(testCase);
			ProcessSpecification specification;
			specification.Executable = GetTestExecutablePath();
			specification.Arguments = { "--strata-test-helper=script-module-lifecycle", FileSystem::ToUTF8(GetTestScriptModule(STRATA_TEST_SCRIPTS_MALFORMED)),
				FileSystem::ToUTF8(GetTestScriptModule(STRATA_TEST_SCRIPTS_API)) };
			return Process::Run(specification, std::chrono::milliseconds(60000));
		};

		// The load fails; the engine keeps working. (The exit code is not checked: what the failed library left behind
		// can make the process crash while it exits - on Windows, Release builds do.)
		const Process::RunResult initialization = run("CrashInStaticInitialization");
		INFO("Static initialization: ", initialization.Output);
		REQUIRE(initialization.Started);
		CHECK_FALSE(initialization.TimedOut);
		CHECK(Contains(initialization.Output, "first module: unloaded"));
		CHECK_FALSE(Contains(initialization.Output, "first module: loaded"));
#if defined(ST_PLATFORM_WINDOWS)
		// The Windows loader contains exceptions in a library's initialization itself (ERROR_DLL_INIT_FAILED).
		CHECK(Contains(initialization.Output, "error 1114"));
#else
		CHECK(Contains(initialization.Output, "crashed while loading"));
#endif
		CHECK(Contains(initialization.Output, "second module: loaded"));

		// Unloading completes (the library may stay loaded); the engine keeps working.
		const Process::RunResult destruction = run("CrashInStaticDestruction");
		INFO("Static destruction: ", destruction.Output);
		REQUIRE(destruction.Started);
		CHECK_FALSE(destruction.TimedOut);
		CHECK(Contains(destruction.Output, "first module: loaded"));
		CHECK(Contains(destruction.Output, "first module: unloaded"));
#if !defined(ST_PLATFORM_WINDOWS)
		// (The Windows loader contains exceptions while a library unloads itself.)
		CHECK(Contains(destruction.Output, "crashed while unloading"));
#endif
		CHECK(Contains(destruction.Output, "second module: loaded"));
		CHECK(destruction.ExitCode == 0);
	}

	TEST_CASE("Class functions are read once, while the module loads")
	{
		// The module clears the functions in its class descriptors when the first instance is created; the engine keeps
		// calling the ones it copied while loading (and never reads the descriptors outside the crash guard).
		ScopedMalformedCase scopedCase("ChangesDescriptors");
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_MALFORMED));
		Scene scene;
		Entity entity = scene.CreateEntity("Entity");
		AddScriptEntry(entity, "First");
		AddScriptEntry(entity, "Second");
		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		CHECK(system.HasInstance(entity, "First"));
		CHECK(system.HasInstance(entity, "Second"));
		CHECK(system.GetFieldValue(entity, "Second", "B").has_value());
		CHECK(system.SetFieldValue(entity, "Second", "B", int32_t(3)));
		RunFrames(scene, 1);
		scene.OnRuntimeStop();
		CHECK_FALSE(engine->IsFaulted());
	}

	TEST_CASE("A failed load keeps the previous module")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("ScriptFailedLoad");
		const std::filesystem::path garbage = directory / FileSystem::FromUTF8(ScriptEngine::GetModuleFileName("Garbage"));
		REQUIRE(FileSystem::WriteBytes(garbage, CreateGarbage(4096)));

		ScriptEngine engine;
		REQUIRE(engine.LoadModule(GetTestScriptModule(STRATA_TEST_SCRIPTS_API)));
		CHECK_FALSE(engine.LoadModule(garbage));
		CHECK_FALSE(engine.LoadModule(GetTestScriptModule(STRATA_TEST_SCRIPTS_ABIMISMATCH)));
		CHECK(engine.IsModuleLoaded());
		CHECK(engine.GetModuleName() == "StrataTestScriptsAPI");
		CHECK(engine.FindClass("Lifecycle") != nullptr);
		CHECK(engine.GetLoadCount() == 1);
		CHECK(engine.GetModulePath().filename() == FileSystem::FromUTF8(STRATA_TEST_SCRIPTS_API));

		std::string error;
		CHECK_FALSE(ScriptEngine().Reload(&error));
		CHECK(Contains(error, "No script module"));
	}

	TEST_CASE("Hot-reloadable modules run from a private copy that is removed when unloaded")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("ScriptCopy");
		const std::filesystem::path module = directory / FileSystem::FromUTF8(ScriptEngine::GetModuleFileName("Game"));
		REQUIRE(FileSystem::Copy(GetTestScriptModule(STRATA_TEST_SCRIPTS_API), module));

		ScriptEngine engine;
		engine.SetHotReloadEnabled(true);
		REQUIRE(engine.LoadModule(module));
		REQUIRE(engine.GetModule());
		CHECK(engine.GetModule()->IsLoadedFromCopy());
		const std::filesystem::path loadedPath = engine.GetModule()->GetLoadedPath();
		CHECK(loadedPath != module);
		CHECK(FileSystem::Exists(loadedPath));
		CHECK(engine.GetModule()->GetSourcePath() == module.lexically_normal());

		// The copy is in a directory of this process inside the user's private runtime directory; the process holds its
		// owner lock while it runs.
		const std::filesystem::path copyDirectory = loadedPath.parent_path();
		CHECK(copyDirectory.parent_path() == Platform::GetUserRuntimeDirectory("Strata"));
		CHECK(StringUtils::StartsWith(FileSystem::ToUTF8(copyDirectory.filename()), "ScriptModules-"));
		CHECK(FileSystem::Exists(copyDirectory / "Owner.lock"));
		CHECK_FALSE(FileLock::TryAcquire(copyDirectory / "Owner.lock"));

		// The build can replace or delete the original while the module is in use.
		CHECK(FileSystem::WriteBytes(module, CreateGarbage(128)));
		CHECK(FileSystem::Remove(module));
		CHECK(engine.FindClass("Lifecycle") != nullptr);

		engine.UnloadModule();
		CHECK_FALSE(FileSystem::Exists(loadedPath));
		CHECK_FALSE(FileSystem::Exists(copyDirectory)); // Removed with its last copy
		CHECK_FALSE(engine.IsModuleLoaded());
	}

	TEST_CASE("Modules load in place until hot reload is enabled")
	{
		const std::filesystem::path module = CreateTemporaryDirectory("ScriptInPlace") / FileSystem::FromUTF8(ScriptEngine::GetModuleFileName("Game"));
		REQUIRE(FileSystem::Copy(GetTestScriptModule(STRATA_TEST_SCRIPTS_API), module));

		ScriptEngine engine;
		REQUIRE(engine.LoadModule(module));
		REQUIRE(engine.GetModule());
		CHECK_FALSE(engine.GetModule()->IsLoadedFromCopy());
		CHECK(engine.GetModule()->GetLoadedPath() == module.lexically_normal());
		CHECK_FALSE(engine.IsReloadPending());

		// Enabling hot reload moves the module to a private copy at the next update, so the build can replace the file.
		engine.SetHotReloadEnabled(true);
		CHECK(engine.IsReloadPending());
		engine.Update();
		CHECK_FALSE(engine.IsReloadPending());
		CHECK(engine.GetLoadCount() == 2);
		REQUIRE(engine.GetModule());
		CHECK(engine.GetModule()->IsLoadedFromCopy());
		CHECK(FileSystem::WriteBytes(module, CreateGarbage(128)));
		CHECK(engine.FindClass("Lifecycle") != nullptr);
		engine.UnloadModule();
	}

	TEST_CASE("A module file that is already loaded is loaded again from a copy")
	{
		// Loading a loaded file again would return the same library, sharing its module state; each engine gets its own.
		ScriptEngine first;
		ScopedScriptEngine second;
		REQUIRE(first.LoadModule(GetTestScriptModule(STRATA_TEST_SCRIPTS_API)));
		REQUIRE(second->LoadModule(GetTestScriptModule(STRATA_TEST_SCRIPTS_API)));
		CHECK_FALSE(first.GetModule()->IsLoadedFromCopy());
		CHECK(second->GetModule()->IsLoadedFromCopy());

		// A reload of a module that runs in place loads the new version from a copy, next to the running one.
		REQUIRE(first.Reload());
		CHECK(first.GetModule()->IsLoadedFromCopy());
		CHECK(first.GetModule()->GetLoadedPath() != second->GetModule()->GetLoadedPath());
		first.UnloadModule();

		// The other engine's module is unaffected.
		Scene scene;
		Entity entity = scene.CreateEntity("Counter");
		ScriptEntry& entry = AddScriptEntry(entity, "Lifecycle");
		AddFieldOverride(entry, "RecordUpdates", PropertyType::Bool, false);
		scene.OnRuntimeStart();
		RunFrames(scene, 2);
		CHECK(GetField<int32_t>(GetScriptSystem(scene), entity, "Lifecycle", "Updates") == 2);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Libraries a module depends on are found next to it, also when it runs from a copy")
	{
		// The dependency is built into a directory of its own (not next to the test executable), so only the module's
		// directory provides it.
		const std::filesystem::path directory = CreateTemporaryDirectory("ScriptDependency");
		const std::filesystem::path module = directory / FileSystem::FromUTF8(STRATA_TEST_SCRIPTS_DEPENDENT);
		REQUIRE(FileSystem::Copy(GetTestScriptModule(STRATA_TEST_SCRIPTS_DEPENDENT), module));
		const std::filesystem::path dependency = FileSystem::FromUTF8(STRATA_TEST_SCRIPT_DEPENDENCY);
		REQUIRE(FileSystem::Copy(dependency, directory / dependency.filename()));

		for (const bool hotReload : { false, true })
		{
			INFO("Hot reload: ", hotReload);
			ScriptEngine engine;
			engine.SetHotReloadEnabled(hotReload);
			std::string error;
			REQUIRE_MESSAGE(engine.LoadModule(module, &error), error);
			CHECK(engine.GetModule()->IsLoadedFromCopy() == hotReload);
			const ScriptClassInfo* info = engine.FindClass("UsesDependency");
			REQUIRE(info);
			REQUIRE(info->Fields.size() == 1);
			CHECK(info->Fields[0].DefaultValue == PropertyValue(int32_t(42)));
		}
	}

	TEST_CASE("Copy directories are removed once their owner process is gone, and never before")
	{
		const std::filesystem::path runtime = Platform::GetUserRuntimeDirectory("Strata");
		REQUIRE_FALSE(runtime.empty());
		const std::string suffix = UUID().ToString();

		// Left behind by a process that ended without cleaning up: nobody holds its owner lock.
		const std::filesystem::path stale = runtime / FileSystem::FromUTF8("ScriptModules-Stale" + suffix);
		REQUIRE(FileSystem::WriteBytes(stale / "Game-Leftover.dll", CreateGarbage(16)));
		REQUIRE(FileLock::Create(stale / "Owner.lock") != nullptr);

		// Owned by a running process: a child creates and holds its owner lock (so it is never seen unlocked).
		const std::filesystem::path owned = runtime / FileSystem::FromUTF8("ScriptModules-Owned" + suffix);
		REQUIRE(FileSystem::WriteBytes(owned / "Game-InUse.dll", CreateGarbage(16)));
		Process owner;
		ProcessSpecification specification;
		specification.Executable = GetTestExecutablePath();
		specification.Arguments = { "--strata-test-helper=hold-file-lock", FileSystem::ToUTF8(owned / "Owner.lock"), "create" };
		REQUIRE(owner.Start(specification));
		std::string output;
		REQUIRE(WaitUntil([&]()
		{
			output += owner.TakeOutput();
			return output.find("locked") != std::string::npos || !owner.IsRunning();
		}, std::chrono::milliseconds(30000)));
		REQUIRE(output.find("locked") != std::string::npos);

		{
			ScriptEngine engine;
			engine.SetHotReloadEnabled(true);
			REQUIRE(engine.LoadModule(GetTestScriptModule(STRATA_TEST_SCRIPTS_API)));
			CHECK_FALSE(FileSystem::Exists(stale));
			CHECK(FileSystem::Exists(owned / "Game-InUse.dll"));
		}

		// The owner ends without cleaning up (as in a crash); the next session removes its directory.
		CHECK(owner.Terminate());
		REQUIRE(WaitUntil([&]() { return FileLock::TryAcquire(owned / "Owner.lock") != nullptr; }, std::chrono::milliseconds(10000)));
		{
			ScriptEngine engine;
			engine.SetHotReloadEnabled(true);
			REQUIRE(engine.LoadModule(GetTestScriptModule(STRATA_TEST_SCRIPTS_API)));
			CHECK_FALSE(FileSystem::Exists(owned));
		}
	}

	TEST_CASE("Processes load script modules concurrently without disturbing each other")
	{
		// Each process keeps its copies in its own directory and removes only its own (or ones whose owner is gone).
		constexpr int c_Reloads = 25;
		std::vector<Scope<Process>> processes;
		for (int index = 0; index < 3; index++)
		{
			ProcessSpecification specification;
			specification.Executable = GetTestExecutablePath();
			specification.Arguments = { "--strata-test-helper=script-module-reloads", FileSystem::ToUTF8(GetTestScriptModule(STRATA_TEST_SCRIPTS_API)),
				std::to_string(c_Reloads) };
			Scope<Process> process = CreateScope<Process>();
			REQUIRE(process->Start(specification));
			processes.push_back(std::move(process));
		}

		// This process does the same meanwhile.
		{
			ScriptEngine engine;
			engine.SetHotReloadEnabled(true);
			std::string error;
			REQUIRE_MESSAGE(engine.LoadModule(GetTestScriptModule(STRATA_TEST_SCRIPTS_API), &error), error);
			for (int index = 0; index < c_Reloads; index++)
			{
				REQUIRE_MESSAGE(engine.Reload(&error), error);
				CHECK(engine.FindClass("Lifecycle") != nullptr);
			}
		}

		for (const Scope<Process>& process : processes)
		{
			const std::optional<int> exitCode = process->Wait(std::chrono::milliseconds(120000));
			INFO("Output: ", process->TakeOutput());
			REQUIRE(exitCode.has_value());
			CHECK(*exitCode == 0);
		}
	}

	TEST_CASE("Scenes play without a script engine or module")
	{
		const Ref<ScriptEngine> previous = ScriptEngine::GetActive();
		ScriptEngine::SetActive(nullptr);
		{
			Scene scene;
			Entity entity = scene.CreateEntity("Scripted");
			AddScriptEntry(entity, "Lifecycle");
			scene.OnRuntimeStart();
			ScriptSystem& system = GetScriptSystem(scene);
			CHECK(system.GetEngine() == nullptr);
			RunFrames(scene, 3);
			CHECK(system.GetInstanceCount() == 0);
			CHECK_FALSE(system.HasInstance(entity, "Lifecycle"));
			CHECK_FALSE(system.GetFieldValue(entity, "Lifecycle", "Updates").has_value());
			scene.OnRuntimeStop();
		}
		ScriptEngine::SetActive(previous);

		{
			ScopedScriptEngine engine;
			Scene scene;
			Entity entity = scene.CreateEntity("Scripted");
			AddScriptEntry(entity, "Lifecycle");
			scene.OnRuntimeStart();
			RunFrames(scene, 2);
			CHECK(GetScriptSystem(scene).GetInstanceCount() == 0);
			scene.OnRuntimeStop();
		}

		{
			// Simulate mode runs physics only.
			ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
			Scene scene;
			AddScriptEntry(scene.CreateEntity("Scripted"), "Lifecycle");
			scene.OnRuntimeStart(SceneRuntimeMode::Simulate);
			CHECK(scene.GetSystem<ScriptSystem>() == nullptr);
			scene.OnRuntimeStop();
		}
	}
}
