#include <doctest/doctest.h>

#include "Scripting/ScriptTestUtils.h"
#include "Strata/Core/FileSystem.h"
#include "TestHelpers.h"

#include <chrono>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A module file that the tests "rebuild" by writing another version of the module over it.
	struct RebuildableModule
	{
		std::filesystem::path Path;

		explicit RebuildableModule(const std::string& name)
		{
			Path = CreateTemporaryDirectory(name) / FileSystem::FromUTF8(ScriptEngine::GetModuleFileName("Game"));
		}

		// Writes the built test module (written fresh, so the file's timestamp changes like after a real build).
		void Install(const char* moduleFileName) const
		{
			const std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(GetTestScriptModule(moduleFileName));
			REQUIRE(bytes.has_value());
			REQUIRE(FileSystem::WriteBytes(Path, *bytes));
		}

		void InstallGarbage() const
		{
			REQUIRE(FileSystem::WriteBytes(Path, std::vector<uint8_t>(2048, 0xCD)));
		}
	};

	bool HasField(const ScriptClassInfo* info, const char* name, PropertyType type)
	{
		const ScriptFieldInfo* field = info ? info->FindField(name) : nullptr;
		return field && field->Type == type;
	}

}

TEST_SUITE("Scripting.Reload")
{
	TEST_CASE("Reloading during play keeps field values and adapts to changed classes")
	{
		RebuildableModule module("ScriptReloadPlay");
		module.Install(STRATA_TEST_SCRIPTS_RELOADV1);
		ScopedScriptEngine engine(module.Path, true);

		Scene scene;
		Entity target = scene.CreateEntity("Target");
		Entity first = scene.CreateEntity("First");
		ScriptEntry& counter = AddScriptEntry(first, "Counter");
		AddFieldOverride(counter, "Speed", PropertyType::Float, 3.0f);
		AddFieldOverride(counter, "Target", PropertyType::Entity, target.GetUUID());
		AddScriptEntry(first, "OnlyInV1");
		Entity second = scene.CreateEntity("Second");
		AddScriptEntry(second, "OnlyInV2"); // Unknown to version 1

		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		CHECK(system.HasInstance(first, "OnlyInV1"));
		CHECK_FALSE(system.HasInstance(second, "OnlyInV2"));
		RunFrames(scene, 3);
		CHECK(GetField<int32_t>(system, first, "Counter", "Count") == 3);
		REQUIRE(system.SetFieldValue(first, "Counter", "Label", std::string("runtime")));
		REQUIRE(system.SetFieldValue(first, "Counter", "ChangesType", int32_t(6)));

		module.Install(STRATA_TEST_SCRIPTS_RELOADV2);
		std::string error;
		REQUIRE_MESSAGE(engine->Reload(&error), error);
		CHECK(engine->GetLoadCount() == 2);
		CHECK(engine->FindClass("OnlyInV2") != nullptr);
		CHECK(engine->FindClass("OnlyInV1") == nullptr);

		// Fields that still exist with the same type keep their running values (not the stored overrides); the others
		// start from the new defaults. OnCreate does not run again; OnReload follows at the next update.
		CHECK(GetField<int32_t>(system, first, "Counter", "Count") == 3);
		CHECK(GetField<float>(system, first, "Counter", "Speed") == 3.0f);
		CHECK(GetField<std::string>(system, first, "Counter", "Label") == "runtime");
		CHECK(GetField<UUID>(system, first, "Counter", "Target") == target.GetUUID());
		CHECK(GetField<float>(system, first, "Counter", "ChangesType") == 0.5f);
		CHECK(GetField<int32_t>(system, first, "Counter", "AddedInV2") == 99);
		CHECK(GetField<int32_t>(system, first, "Counter", "Creates") == 1);
		CHECK(GetField<int32_t>(system, first, "Counter", "Reloads") == 0);
		CHECK_FALSE(system.GetFieldValue(first, "Counter", "RemovedInV2").has_value());
		CHECK_FALSE(system.HasInstance(first, "OnlyInV1"));

		scene.OnUpdateRuntime(0.0f);
		CHECK(GetField<int32_t>(system, first, "Counter", "Reloads") == 1);
		CHECK(GetField<int32_t>(system, first, "Counter", "Creates") == 1);
		CHECK(GetField<int32_t>(system, first, "Counter", "Count") == 13); // The new code counts in tens
		// The class that only the new version has gets its instance like any new script.
		CHECK(system.HasInstance(second, "OnlyInV2"));
		CHECK(GetField<int32_t>(system, second, "OnlyInV2", "Creates") == 1);

		// And back: the added field disappears, the changed type returns to its old default, removed classes return.
		module.Install(STRATA_TEST_SCRIPTS_RELOADV1);
		REQUIRE(engine->Reload());
		scene.OnUpdateRuntime(0.0f);
		CHECK(GetField<int32_t>(system, first, "Counter", "Count") == 14);
		CHECK(GetField<int32_t>(system, first, "Counter", "Reloads") == 2);
		CHECK(GetField<int32_t>(system, first, "Counter", "ChangesType") == 5);
		CHECK(GetField<std::string>(system, first, "Counter", "Label") == "runtime");
		CHECK_FALSE(system.HasInstance(second, "OnlyInV2"));
		CHECK(system.HasInstance(first, "OnlyInV1"));
		CHECK(engine->GetLoadCount() == 3);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Reloading in edit mode refreshes the class metadata")
	{
		RebuildableModule module("ScriptReloadEdit");
		module.Install(STRATA_TEST_SCRIPTS_RELOADV1);
		ScriptEngine engine;
		engine.SetHotReloadEnabled(true);
		REQUIRE(engine.LoadModule(module.Path));
		CHECK(engine.FindClass("OnlyInV1") != nullptr);
		CHECK(HasField(engine.FindClass("Counter"), "RemovedInV2", PropertyType::Int));
		CHECK(HasField(engine.FindClass("Counter"), "ChangesType", PropertyType::Int));

		module.Install(STRATA_TEST_SCRIPTS_RELOADV2);
		REQUIRE(engine.Reload());
		const ScriptClassInfo* counter = engine.FindClass("Counter");
		CHECK(engine.FindClass("OnlyInV1") == nullptr);
		CHECK(engine.FindClass("OnlyInV2") != nullptr);
		CHECK(HasField(counter, "AddedInV2", PropertyType::Int));
		CHECK_FALSE(HasField(counter, "RemovedInV2", PropertyType::Int));
		CHECK(HasField(counter, "ChangesType", PropertyType::Float));
		REQUIRE(counter->FindField("Speed"));
		CHECK(counter->FindField("Speed")->DefaultValue == PropertyValue(2.0f));
	}

	TEST_CASE("A broken build keeps the running module")
	{
		RebuildableModule module("ScriptReloadBroken");
		module.Install(STRATA_TEST_SCRIPTS_RELOADV1);
		ScopedScriptEngine engine(module.Path, true);
		Scene scene;
		Entity entity = scene.CreateEntity("Counting");
		AddScriptEntry(entity, "Counter");
		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		RunFrames(scene, 2);

		// A half-written file, a module built against another ABI and a module that crashes while loading.
		module.InstallGarbage();
		CHECK_FALSE(engine->Reload());
		module.Install(STRATA_TEST_SCRIPTS_ABIMISMATCH);
		CHECK_FALSE(engine->Reload());
		module.Install(STRATA_TEST_SCRIPTS_LOADFAULT);
		CHECK_FALSE(engine->Reload());
		CHECK_FALSE(engine->IsFaulted());
		CHECK(engine->GetLoadCount() == 1);
		RunFrames(scene, 1);
		CHECK(GetField<int32_t>(system, entity, "Counter", "Count") == 3);

		// The fixed build loads.
		module.Install(STRATA_TEST_SCRIPTS_RELOADV2);
		REQUIRE(engine->Reload());
		RunFrames(scene, 1);
		CHECK(GetField<int32_t>(system, entity, "Counter", "Count") == 13);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Changed module files are hot reloaded")
	{
		RebuildableModule module("ScriptHotReload");
		module.Install(STRATA_TEST_SCRIPTS_RELOADV1);
		ScopedScriptEngine engine(module.Path, true);
		CHECK(engine->IsHotReloadEnabled());

		Scene scene;
		Entity entity = scene.CreateEntity("Counting");
		AddScriptEntry(entity, "Counter");
		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		RunFrames(scene, 1);
		engine->Update();
		CHECK(engine->GetLoadCount() == 1);

		module.Install(STRATA_TEST_SCRIPTS_RELOADV2);
		CHECK(WaitUntil([&]()
		{
			engine->Update();
			return engine->GetLoadCount() == 2;
		}, std::chrono::milliseconds(15000)));
		CHECK_FALSE(engine->IsReloadPending());

		RunFrames(scene, 1);
		CHECK(GetField<int32_t>(system, entity, "Counter", "Count") == 11);
		CHECK(GetField<int32_t>(system, entity, "Counter", "Reloads") == 1);

		engine->SetHotReloadEnabled(false);
		CHECK_FALSE(engine->IsHotReloadEnabled());
		scene.OnRuntimeStop();
	}
}
