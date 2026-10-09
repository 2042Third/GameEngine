#include <doctest/doctest.h>

#include "Scripting/ScriptTestUtils.h"
#include "Strata/Core/DynamicLibrary.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/StringUtils.h"
#include "Strata/Scripting/ScriptModule.h"
#include "TestHelpers.h"

#include "StrataScript/ScriptABI.h"

#include <set>
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
			"ComponentAPI", "PropertyProbe", "TransformAPI", "Spawned", "Spawner", "Listener", "Talker", "InputProbe", "TimeProbe", "SceneProbe"
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

	TEST_CASE("Modules run from a private copy that is removed when unloaded")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("ScriptCopy");
		const std::filesystem::path module = directory / FileSystem::FromUTF8(ScriptEngine::GetModuleFileName("Game"));
		REQUIRE(FileSystem::Copy(GetTestScriptModule(STRATA_TEST_SCRIPTS_API), module));

		ScriptEngine engine;
		REQUIRE(engine.LoadModule(module));
		REQUIRE(engine.GetModule());
		const std::filesystem::path loadedPath = engine.GetModule()->GetLoadedPath();
		CHECK(loadedPath != module);
		CHECK(FileSystem::Exists(loadedPath));
		CHECK(engine.GetModule()->GetSourcePath() == module.lexically_normal());

		// The build can replace or delete the original while the module is in use.
		CHECK(FileSystem::WriteBytes(module, CreateGarbage(128)));
		CHECK(FileSystem::Remove(module));
		CHECK(engine.FindClass("Lifecycle") != nullptr);

		engine.UnloadModule();
		CHECK_FALSE(FileSystem::Exists(loadedPath));
		CHECK_FALSE(engine.IsModuleLoaded());
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
