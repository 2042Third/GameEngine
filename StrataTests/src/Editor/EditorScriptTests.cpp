#include <doctest/doctest.h>

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Network/NetworkTestHelpers.h"
#include "Scripting/ScriptTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Project/GameManifest.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Runtime/GameRuntime.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scripting/ScriptModule.h>
#include <Strata/Scripting/ScriptSystem.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// An editor with a new project, driven by commands.
	struct ScriptHarness
	{
		EditorContext Context;
		EditorCommandRegistry Commands;
		std::filesystem::path Directory;

		explicit ScriptHarness(const EditorContextSpecification& specification = EditorContextSpecification { false, false })
			: Context(specification), Directory(CreateTemporaryDirectory("EditorScripts") / "Script Game")
		{
			Run("project.create", { { "directory", FileSystem::ToUTF8(Directory) }, { "name", "Script Game" } });
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			INFO(std::string(name), ": ", result.Error);
			REQUIRE(result.Success);
			REQUIRE_FALSE(result.IsPending());
			return result.Value;
		}

		std::string Error(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			REQUIRE_FALSE(result.IsPending());
			CHECK_FALSE(result.Success);
			return result.Error;
		}

		// The kind of a failure: a mistake in the request, or a state that does not allow the command.
		EditorCommandError ErrorKind(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			REQUIRE_FALSE(result.IsPending());
			CHECK_FALSE(result.Success);
			return result.ErrorKind;
		}

		std::string CreateEntity(const std::string& name)
		{
			return Run("entity.create", { { "name", name } })["id"].get<std::string>();
		}

		Entity GetEntity(const std::string& id)
		{
			return Context.GetActiveScene()->GetEntityByUUID(*UUIDFromJson(id));
		}

		void LoadModule(const char* fileName)
		{
			Run("script.load", { { "path", FileSystem::ToUTF8(GetTestScriptModule(fileName)) } });
		}

		void Frames(int count)
		{
			for (int frame = 0; frame < count; frame++)
				Context.Update(Timestep(1.0f / 60.0f));
		}
	};

	const nlohmann::json* FindClass(const nlohmann::json& status, std::string_view name)
	{
		for (const nlohmann::json& info : status["classes"])
		{
			if (info["name"] == name)
				return &info;
		}
		return nullptr;
	}

}

TEST_SUITE("Editor.Scripts")
{
	TEST_CASE("New projects come with a script build and report their script state")
	{
		ScriptHarness harness;
		const Ref<Project>& project = harness.Context.GetProject();
		const std::filesystem::path scripts = project->GetScriptSourceDirectory();
		CHECK(FileSystem::IsRegularFile(scripts / "CMakeLists.txt"));
		CHECK(FileSystem::IsRegularFile(scripts / "Spinner.cpp"));
		CHECK(project->GetScriptModuleName() == "ScriptGameScripts");
		REQUIRE(harness.Context.GetScriptEngine());
		CHECK(ScriptEngine::GetActive() == harness.Context.GetScriptEngine());

		const nlohmann::json status = harness.Run("script.status");
		CHECK(status["project"] == true);
		CHECK(status["loaded"] == false);
		CHECK(status["module"].is_null());
		CHECK(status["classes"].empty());
		CHECK(status["projectModuleBuilt"] == false);
		CHECK(status["projectModule"] == FileSystem::ToUTF8(project->GetScriptModulePath()));
		CHECK(status["build"]["running"] == false);
		CHECK(status["build"]["last"].is_null());
		CHECK(status["fault"].is_null());

		// script.init only writes what is missing.
		CHECK(harness.Run("script.init", { { "example", true } })["created"].empty());
		REQUIRE(FileSystem::Remove(scripts / "Spinner.cpp"));
		CHECK(harness.Run("script.init")["created"].empty());
		const nlohmann::json created = harness.Run("script.init", { { "example", true } })["created"];
		REQUIRE(created.size() == 1);
		CHECK(created[0] == FileSystem::ToUTF8(scripts / "Spinner.cpp"));

		// Nothing to reload yet, and without a module classes cannot be checked.
		CHECK(harness.Error("script.reload").find("script.build") != std::string::npos);
		const std::string entity = harness.CreateEntity("Spinner Host");
		CHECK(harness.Error("script.add", { { "entity", entity }, { "class", "Spinner" } }).find("script.build") != std::string::npos);
		CHECK(harness.Error("script.load", { { "path", "Missing.dll" } }).find("Missing") != std::string::npos);

		// Closing the project releases the engine.
		harness.Context.CloseProject();
		CHECK_FALSE(harness.Context.GetScriptEngine());
		CHECK(ScriptEngine::GetActive() == nullptr);
		CHECK(harness.Run("script.status")["project"] == false);
		CHECK_FALSE(harness.Error("script.build").empty());
	}

	TEST_CASE("Scripts are attached, edited and detached through commands with undo")
	{
		ScriptHarness harness;
		harness.LoadModule(STRATA_TEST_SCRIPTS_API);
		const nlohmann::json status = harness.Run("script.status");
		CHECK(status["loaded"] == true);
		CHECK(status["loadCount"] == 1);
		const nlohmann::json* fieldTypes = FindClass(status, "FieldTypes");
		REQUIRE(fieldTypes);
		CHECK((*fieldTypes)["callbacks"] == nlohmann::json::array({ "OnCreate" }));
		bool sawInt = false;
		for (const nlohmann::json& field : (*fieldTypes)["fields"])
		{
			if (field["name"] == "IntField")
			{
				CHECK(field["type"] == "Int");
				CHECK(field["default"] == 42);
				sawInt = true;
			}
		}
		CHECK(sawInt);

		const std::string target = harness.CreateEntity("Target");
		const std::string id = harness.CreateEntity("Holder");
		const nlohmann::json added = harness.Run("script.add", { { "entity", id }, { "class", "FieldTypes" },
			{ "fields", { { "IntField", 7 }, { "StringField", "Hi" }, { "AssetField", "Builtin/Cube" }, { "EntityField", target }, { "QuatField", { 0, 90, 0 } } } } });
		CHECK(added["script"]["class"] == "FieldTypes");
		CHECK(added["script"]["fields"]["IntField"] == 7);
		CHECK(added["script"]["fields"]["AssetField"] == "0000000000000001");
		CHECK_FALSE(added.contains("warning"));

		Entity holder = harness.GetEntity(id);
		REQUIRE(holder.HasComponent<ScriptComponent>());
		const ScriptEntry* entry = holder.GetComponent<ScriptComponent>().FindScript("FieldTypes");
		REQUIRE(entry);
		CHECK(entry->Fields.size() == 5);

		// Invalid requests change nothing.
		CHECK(harness.Error("script.add", { { "entity", id }, { "class", "FieldTypes" } }).find("already") != std::string::npos);
		CHECK(harness.Error("script.add", { { "entity", id }, { "class", "NoSuchClass" } }).find("FieldTypes") != std::string::npos);
		CHECK(harness.Error("script.add", { { "entity", target }, { "class", "Idle" }, { "fields", { { "Missing", 1 } } } }).find("no field") != std::string::npos);
		CHECK(harness.Error("script.add", { { "entity", target }, { "class", "FieldTypes" }, { "fields", { { "IntField", "seven" } } } }).find("IntField")
			!= std::string::npos);
		CHECK(harness.Error("script.setField", { { "entity", id }, { "class", "FieldTypes" }, { "field", "EntityField" }, { "value", "123456" } })
			.find("no entity") != std::string::npos);
		CHECK(harness.Error("script.setField", { { "entity", id }, { "class", "FieldTypes" }, { "field", "AssetField" }, { "value", "Missing/Asset" } })
			.find("no asset") != std::string::npos);
		CHECK(harness.Error("script.setField", { { "entity", id }, { "class", "FieldTypes" }, { "field", "Nope" }, { "value", 1 } }).find("no field")
			!= std::string::npos);
		CHECK(harness.Error("script.setField", { { "entity", target }, { "class", "FieldTypes" }, { "field", "IntField" }, { "value", 1 } })
			.find("has no script") != std::string::npos);
		CHECK_FALSE(harness.GetEntity(target).HasComponent<ScriptComponent>());

		// Overrides change and reset (null) with one undo step each.
		const size_t undoBefore = harness.Context.GetUndoStack().GetPosition();
		harness.Run("script.setField", { { "entity", id }, { "class", "FieldTypes" }, { "field", "Vec3Field" }, { "value", { 4, 5, 6 } } });
		harness.Run("script.setField", { { "entity", id }, { "class", "FieldTypes" }, { "field", "IntField" }, { "value", nullptr } });
		entry = holder.GetComponent<ScriptComponent>().FindScript("FieldTypes");
		REQUIRE(entry);
		CHECK(entry->FindField("IntField") == nullptr);
		REQUIRE(entry->FindField("Vec3Field"));
		CHECK(std::get<glm::vec3>(entry->FindField("Vec3Field")->Value) == glm::vec3(4.0f, 5.0f, 6.0f));
		CHECK(harness.Context.GetUndoStack().GetPosition() == undoBefore + 2);

		harness.Run("edit.undo");
		entry = harness.GetEntity(id).GetComponent<ScriptComponent>().FindScript("FieldTypes");
		REQUIRE(entry);
		REQUIRE(entry->FindField("IntField"));
		CHECK(std::get<int32_t>(entry->FindField("IntField")->Value) == 7);
		harness.Run("edit.redo");

		// Every command is its own undo step, even for the same field twice in a row.
		const size_t undoBeforeRepeat = harness.Context.GetUndoStack().GetPosition();
		harness.Run("script.setField", { { "entity", id }, { "class", "FieldTypes" }, { "field", "FloatField" }, { "value", 3.5 } });
		harness.Run("script.setField", { { "entity", id }, { "class", "FieldTypes" }, { "field", "FloatField" }, { "value", 2.5 } });
		CHECK(harness.Context.GetUndoStack().GetPosition() == undoBeforeRepeat + 2);
		harness.Run("edit.undo");
		entry = harness.GetEntity(id).GetComponent<ScriptComponent>().FindScript("FieldTypes");
		REQUIRE(entry);
		REQUIRE(entry->FindField("FloatField"));
		CHECK(std::get<float>(entry->FindField("FloatField")->Value) == 3.5f);
		CHECK(harness.Error("script.setField", { { "entity", id }, { "class", "FieldTypes" }, { "field", "FloatField" } }).find("value") != std::string::npos);

		// The last script takes the Script component with it; undo brings both back.
		harness.Run("script.remove", { { "entity", id }, { "class", "FieldTypes" } });
		CHECK_FALSE(harness.GetEntity(id).HasComponent<ScriptComponent>());
		CHECK(harness.Error("script.remove", { { "entity", id }, { "class", "FieldTypes" } }).find("has no script") != std::string::npos);
		harness.Run("edit.undo");
		REQUIRE(harness.GetEntity(id).HasComponent<ScriptComponent>());
		CHECK(harness.GetEntity(id).GetComponent<ScriptComponent>().FindScript("FieldTypes"));
	}

	TEST_CASE("Playing runs attached scripts and field edits reach the live instances")
	{
		ScriptHarness harness;
		harness.LoadModule(STRATA_TEST_SCRIPTS_API);
		const std::string id = harness.CreateEntity("Holder");
		harness.Run("script.add", { { "entity", id }, { "class", "FieldTypes" }, { "fields", { { "IntField", 9 } } } });

		// script.get in edit mode: the stored override, the class defaults for the rest.
		nlohmann::json stored = harness.Run("script.get", { { "entity", id } });
		REQUIRE(stored["scripts"].size() == 1);
		CHECK(stored["scripts"][0]["class"] == "FieldTypes");
		CHECK(stored["scripts"][0]["live"] == false);
		CHECK(stored["scripts"][0]["known"] == true);
		CHECK(stored["scripts"][0]["fields"]["IntField"] == 9);
		CHECK(stored["scripts"][0]["fields"]["StringField"] == "Hello");
		CHECK(stored["scripts"][0]["fields"]["IntSeenInCreate"] == 0);
		CHECK(harness.ErrorKind("script.get", { { "entity", id }, { "class", "Idle" } }) == EditorCommandError::InvalidParameters);

		harness.Run("play.start");
		harness.Frames(2);
		// While playing: the live instance's values, including what the script itself changed.
		const nlohmann::json liveFields = harness.Run("script.get", { { "entity", id }, { "class", "FieldTypes" } });
		REQUIRE(liveFields["scripts"].size() == 1);
		CHECK(liveFields["scripts"][0]["live"] == true);
		CHECK(liveFields["scripts"][0]["fields"]["IntSeenInCreate"] == 9);
		Ref<Scene> running = harness.Context.GetActiveScene();
		REQUIRE(running != harness.Context.GetEditScene());
		ScriptSystem& system = GetScriptSystem(*running);
		Entity live = running->GetEntityByUUID(*UUIDFromJson(id));
		CHECK(GetField<int32_t>(system, live, "FieldTypes", "IntSeenInCreate") == 9);

		const nlohmann::json edited = harness.Run("script.setField", { { "entity", id }, { "class", "FieldTypes" }, { "field", "StringField" }, { "value", "Live" } });
		CHECK(edited.contains("warning"));
		CHECK(GetField<std::string>(system, live, "FieldTypes", "StringField") == "Live");
		harness.Run("script.setField", { { "entity", id }, { "class", "FieldTypes" }, { "field", "IntField" }, { "value", nullptr } });
		CHECK(GetField<int32_t>(system, live, "FieldTypes", "IntField") == 42);

		// Scripts added while playing start right away.
		const std::string other = harness.Run("entity.create", { { "name", "Late" } })["id"].get<std::string>();
		harness.Run("script.add", { { "entity", other }, { "class", "Idle" } });
		harness.Frames(1);
		CHECK(system.HasInstance(running->GetEntityByUUID(*UUIDFromJson(other)), "Idle"));

		harness.Run("play.stop");
		const ScriptEntry* entry = harness.GetEntity(id).GetComponent<ScriptComponent>().FindScript("FieldTypes");
		REQUIRE(entry);
		REQUIRE(entry->FindField("IntField"));
		CHECK(std::get<int32_t>(entry->FindField("IntField")->Value) == 9); // Edits while playing were discarded
		CHECK(entry->FindField("StringField") == nullptr);
		stored = harness.Run("script.get", { { "entity", id } });
		CHECK(stored["scripts"][0]["live"] == false);
		CHECK(stored["scripts"][0]["fields"]["IntSeenInCreate"] == 0);

		// A class the module does not have: only the stored overrides are known.
		const std::string orphan = harness.Run("entity.create", { { "name", "Orphan" }, { "components", { { "Script", { { "Scripts", { {
			{ "Class", "Missing" }, { "Fields", { { "Speed", { { "Type", "Float" }, { "Value", 2.5 } } } } } } } } } } } } })["id"].get<std::string>();
		const nlohmann::json unknown = harness.Run("script.get", { { "entity", orphan } });
		REQUIRE(unknown["scripts"].size() == 1);
		CHECK(unknown["scripts"][0]["known"] == false);
		CHECK(unknown["scripts"][0]["fields"] == nlohmann::json { { "Speed", 2.5 } });
		CHECK(harness.Run("script.get", { { "entity", harness.CreateEntity("Plain") } })["scripts"].empty());
	}

	TEST_CASE("A script crash stops play mode and keeps the module disabled until it is reloaded")
	{
		ScriptHarness harness;
		harness.LoadModule(STRATA_TEST_SCRIPTS_FAULTS);
		const std::string id = harness.CreateEntity("Crasher");
		harness.Run("script.add", { { "entity", id }, { "class", "Faulty" }, { "fields", { { "Fault", "NullDereference" }, { "FaultIn", "OnUpdate" } } } });
		harness.Run("script.add", { { "entity", harness.CreateEntity("Bystander") }, { "class", "Healthy" } });

		harness.Run("play.start");
		harness.Frames(3);
		CHECK_FALSE(harness.Context.IsPlaying());
		const nlohmann::json status = harness.Run("script.status");
		REQUIRE(status["fault"].is_object());
		CHECK(status["fault"]["class"] == "Faulty");
		CHECK(status["fault"]["method"] == "OnUpdate");
		CHECK(status["fault"]["entityName"] == "Crasher");
		CHECK(status["lastFault"]["class"] == "Faulty");

		// The faulted module does not run again until it is reloaded.
		CHECK(harness.Error("play.start").find("crashed") != std::string::npos);
		harness.Run("script.reload");
		CHECK(harness.Run("script.status")["fault"].is_null());
		CHECK(harness.Run("script.status")["lastFault"].is_null());

		// A crash while the scene starts fails play.start itself.
		harness.Run("script.setField", { { "entity", id }, { "class", "Faulty" }, { "field", "FaultIn" }, { "value", "OnCreate" } });
		CHECK(harness.Error("play.start").find("crashed") != std::string::npos);
		CHECK_FALSE(harness.Context.IsPlaying());
		CHECK(harness.Run("script.status")["lastFault"]["method"] == "OnCreate");
	}

	TEST_CASE("A build that leaves a crashed module unchanged loads it again")
	{
		// The test executable stands in for CMake and builds nothing (see TestMain.cpp), so the module file stays the same:
		// the build succeeds without changing the module, as when only data caused the crash.
		ScopedEnvironmentVariable fakeCMake("STRATA_TEST_FAKE_CMAKE", "succeed");
		EditorContextSpecification specification { false, false };
		specification.ScriptBuild.CMake = GetTestExecutablePath();
		ScriptHarness harness(specification);
		const std::filesystem::path module = harness.Context.GetProject()->GetScriptModulePath();
		REQUIRE(FileSystem::CreateDirectories(module.parent_path()));
		REQUIRE(FileSystem::Copy(GetTestScriptModule(STRATA_TEST_SCRIPTS_FAULTS), module));
		harness.Run("script.reload"); // Loads the project's built module
		const std::string id = harness.CreateEntity("Crasher");
		harness.Run("script.add", { { "entity", id }, { "class", "Faulty" }, { "fields", { { "Fault", "NullDereference" }, { "FaultIn", "OnUpdate" } } } });
		harness.Run("play.start");
		harness.Frames(2);
		REQUIRE_FALSE(harness.Context.IsPlaying());
		REQUIRE(harness.Context.GetScriptEngine()->IsFaulted());

		// The data is fixed and the scripts are built: the module did not change, yet it runs again.
		harness.Run("script.setField", { { "entity", id }, { "class", "Faulty" }, { "field", "Fault" }, { "value", "" } });
		std::string error;
		REQUIRE_MESSAGE(harness.Context.BuildScripts(&error), error);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (harness.Context.GetScriptBuilder().IsRunning() && std::chrono::steady_clock::now() < deadline)
		{
			harness.Frames(1);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		REQUIRE_FALSE(harness.Context.GetScriptBuilder().IsRunning());
		const ScriptBuildResult& result = harness.Context.GetScriptBuilder().GetLastResult();
		REQUIRE_MESSAGE(result.Success, result.Error);
		CHECK_FALSE(result.ModuleChanged);
		CHECK(harness.Context.GetLastScriptBuildLoad().Loaded);
		CHECK_FALSE(harness.Context.GetScriptEngine()->IsFaulted());
		harness.Run("play.start");
		harness.Frames(2);
		CHECK(harness.Context.IsPlaying());
	}

	TEST_CASE("Modules the editor loads run from a private copy, so builds can replace them")
	{
		// With hot reload (the editor's default) modules must not run from their file, or script.build could not replace
		// it (Windows locks loaded modules) and moving them to a copy later would reload them a second time.
		ScopedEnvironmentVariable fakeCMake("STRATA_TEST_FAKE_CMAKE", "succeed");
		EditorContextSpecification specification { false, true };
		specification.ScriptBuild.CMake = GetTestExecutablePath();
		ScriptHarness harness(specification);
		const Ref<Project> project = harness.Context.GetProject();
		const std::filesystem::path module = project->GetScriptModulePath();
		REQUIRE(FileSystem::CreateDirectories(module.parent_path()));
		REQUIRE(FileSystem::Copy(GetTestScriptModule(STRATA_TEST_SCRIPTS_FAULTS), module));

		auto checkRunsFromCopy = [&]()
		{
			const Ref<ScriptEngine>& engine = harness.Context.GetScriptEngine();
			REQUIRE(engine->GetModule());
			CHECK(engine->GetModule()->IsLoadedFromCopy());
			const uint64_t loads = engine->GetLoadCount();
			harness.Frames(3);
			CHECK(engine->GetLoadCount() == loads);
		};

		// Loaded by a build (the fake CMake leaves the file the test put there).
		std::string error;
		REQUIRE_MESSAGE(harness.Context.BuildScripts(&error), error);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (harness.Context.GetScriptBuilder().IsRunning() && std::chrono::steady_clock::now() < deadline)
		{
			harness.Frames(1);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		REQUIRE(harness.Context.GetLastScriptBuildLoad().Loaded);
		checkRunsFromCopy();

		// Loaded when the project opens.
		REQUIRE_MESSAGE(harness.Context.OpenProject(project->GetProjectFile(), &error), error);
		REQUIRE(harness.Context.GetScriptEngine()->IsModuleLoaded());
		checkRunsFromCopy();
	}

	TEST_CASE("Exported games carry the script module and run it in the game runtime")
	{
		ScriptHarness harness;
		harness.LoadModule(STRATA_TEST_SCRIPTS_FAULTS);
		const std::string healthy = harness.CreateEntity("Healthy Host");
		harness.Run("script.add", { { "entity", healthy }, { "class", "Healthy" } });
		harness.Run("scene.saveAs", { { "path", "Scenes/Main.stscene" } });
		harness.Run("project.setStartScene", { { "scene", "Scenes/Main.stscene" } });

		// The module (and, outside Dist builds, its symbols) goes next to the game, named in the manifest.
		const std::filesystem::path build = harness.Directory.parent_path() / "Build";
		const nlohmann::json exported = harness.Run("project.export", { { "directory", FileSystem::ToUTF8(build) }, { "includeRuntime", false } });
		const std::filesystem::path module = FileSystem::FromUTF8(exported["scriptModule"].get<std::string>());
		CHECK(module == build / GetTestScriptModule(STRATA_TEST_SCRIPTS_FAULTS).filename());
		CHECK(FileSystem::IsRegularFile(module));
		std::filesystem::path symbols = GetTestScriptModule(STRATA_TEST_SCRIPTS_FAULTS);
		symbols.replace_extension(".pdb");
		if (FileSystem::IsRegularFile(symbols))
			CHECK(FileSystem::IsRegularFile(build / symbols.filename()));
		const std::filesystem::path manifestPath = FileSystem::FromUTF8(exported["manifest"].get<std::string>());
		const std::optional<GameManifest> manifest = GameManifest::Load(manifestPath);
		REQUIRE(manifest);
		CHECK(manifest->ScriptModule == FileSystem::ToUTF8(module.filename()));

		// Without symbols on request.
		const std::filesystem::path bare = harness.Directory.parent_path() / "Bare";
		harness.Run("project.export", { { "directory", FileSystem::ToUTF8(bare) }, { "includeRuntime", false }, { "includeScriptSymbols", false } });
		CHECK(FileSystem::IsRegularFile(bare / module.filename()));
		CHECK_FALSE(FileSystem::Exists(bare / symbols.filename()));

		{
			// The runtime loads its own copy of the module and makes it the active engine while the game runs.
			std::string error;
			Scope<GameRuntime> game = GameRuntime::Create(manifestPath, &error);
			REQUIRE_MESSAGE(game, error);
			const Ref<ScriptEngine> engine = game->GetScriptEngine();
			REQUIRE(engine);
			CHECK(engine != harness.Context.GetScriptEngine());
			CHECK(ScriptEngine::GetActive() == engine);
			CHECK(engine->GetModulePath() == std::filesystem::absolute(module).lexically_normal());
			for (int frame = 0; frame < 3; frame++)
				game->Update(Timestep(1.0f / 60.0f));
			ScriptSystem& system = GetScriptSystem(*game->GetScene());
			const Entity host = game->GetScene()->FindEntityByName("Healthy Host");
			CHECK(GetField<int32_t>(system, host, "Healthy", "Updates") == 3);
			CHECK_FALSE(game->GetScriptFault());
		}
		// The host's engine is active again once the game ends.
		CHECK(ScriptEngine::GetActive() == harness.Context.GetScriptEngine());

		// A crash disables the game's scripts; the runtime reports it and keeps the scene running.
		const std::string crasher = harness.CreateEntity("Crasher");
		// A null dereference crashes on every platform (integer division by zero does not trap on ARM64).
		harness.Run("script.add", { { "entity", crasher }, { "class", "Faulty" }, { "fields", { { "Fault", "NullDereference" }, { "FaultIn", "OnUpdate" } } } });
		harness.Run("scene.save");
		const nlohmann::json crashing = harness.Run("project.export", { { "directory", FileSystem::ToUTF8(build) }, { "includeRuntime", false } });
		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(FileSystem::FromUTF8(crashing["manifest"].get<std::string>()), &error);
		REQUIRE_MESSAGE(game, error);
		game->Update(Timestep(1.0f / 60.0f));
		REQUIRE(game->GetScriptFault());
		CHECK(game->GetScriptFault()->ClassName == "Faulty");
		game->Update(Timestep(1.0f / 60.0f));
		CHECK(game->GetScene()->IsRunning());
	}

	TEST_CASE("The game runtime reports how many script classes its module has")
	{
		ScriptHarness harness;
		harness.Run("scene.saveAs", { { "path", "Scenes/Main.stscene" } });
		harness.Run("project.setStartScene", { { "scene", "Scenes/Main.stscene" } });
		const std::filesystem::path build = harness.Directory.parent_path() / "Build";
		// The runtime reports the start at info level, with the module's number of classes.
		const ScopedLogLevel infoLog(LogLevel::Info);
		auto exportAndStart = [&harness, &build]() -> uint64_t
		{
			const nlohmann::json exported = harness.Run("project.export", { { "directory", FileSystem::ToUTF8(build) }, { "includeRuntime", false } });
			const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
			std::string error;
			const Scope<GameRuntime> game = GameRuntime::Create(FileSystem::FromUTF8(exported["manifest"].get<std::string>()), &error);
			REQUIRE_MESSAGE(game, error);
			return logStart;
		};

		// One class is counted in the singular.
		harness.LoadModule(STRATA_TEST_SCRIPTS_NEWERSDK);
		uint64_t logStart = exportAndStart();
		CHECK(CountLogMessages(logStart, ", 1 script class)") == 1);

		harness.LoadModule(STRATA_TEST_SCRIPTS_FAULTS);
		const size_t classes = harness.Context.GetScriptEngine()->GetClasses().size();
		REQUIRE(classes > 1);
		logStart = exportAndStart();
		CHECK(CountLogMessages(logStart, fmt::format(", {} script classes)", classes)) == 1);
	}

	TEST_CASE("Exports ship the module file the editor loaded, not a newer one")
	{
		// A module the editor loaded from a file that later changes (as a build whose module fails to load leaves it).
		ScriptHarness harness;
		const std::filesystem::path modules = harness.Directory.parent_path() / "Modules";
		const std::filesystem::path module = modules / GetTestScriptModule(STRATA_TEST_SCRIPTS_FAULTS).filename();
		REQUIRE(FileSystem::CreateDirectories(modules));
		REQUIRE(FileSystem::Copy(GetTestScriptModule(STRATA_TEST_SCRIPTS_FAULTS), module));
		const std::optional<std::vector<uint8_t>> loadedBytes = FileSystem::ReadBytes(module);
		REQUIRE(loadedBytes);
		// As in the editor, hot reload is on: the module runs from a private copy, so its file can change. The test never
		// updates the context, so the watcher does not reload it.
		harness.Context.GetScriptEngine()->SetHotReloadEnabled(true);
		harness.Run("script.load", { { "path", FileSystem::ToUTF8(module) } });
		harness.Run("script.add", { { "entity", harness.CreateEntity("Healthy Host") }, { "class", "Healthy" } });
		harness.Run("scene.saveAs", { { "path", "Scenes/Main.stscene" } });
		harness.Run("project.setStartScene", { { "scene", "Scenes/Main.stscene" } });
		const std::filesystem::path build = harness.Directory.parent_path() / "Build";
		const nlohmann::json parameters = { { "directory", FileSystem::ToUTF8(build) }, { "includeRuntime", false } };

		// Another file in its place: refused, nothing is written.
		const std::optional<std::vector<uint8_t>> otherBytes = FileSystem::ReadBytes(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		REQUIRE(otherBytes);
		REQUIRE(FileSystem::WriteBytes(module, *otherBytes));
		const std::string error = harness.Error("project.export", parameters);
		CHECK(error.find("changed since the editor loaded it") != std::string::npos);
		CHECK(error.find("script.reload") != std::string::npos);
		CHECK_FALSE(FileSystem::Exists(build));

		// The same bytes as loaded again: exported as they are.
		REQUIRE(FileSystem::WriteBytes(module, *loadedBytes));
		const nlohmann::json exported = harness.Run("project.export", parameters);
		CHECK(FileSystem::ReadBytes(FileSystem::FromUTF8(exported["scriptModule"].get<std::string>())) == loadedBytes);

		// Reloading makes the new file the module the editor runs, and the one the game ships.
		REQUIRE(FileSystem::WriteBytes(module, *otherBytes));
		harness.Run("script.reload");
		const nlohmann::json reexported = harness.Run("project.export", parameters);
		CHECK(FileSystem::ReadBytes(FileSystem::FromUTF8(reexported["scriptModule"].get<std::string>())) == otherBytes);
	}

	TEST_CASE("Games whose scenes use scripts are not exported without a script module")
	{
		ScriptHarness harness;
		harness.LoadModule(STRATA_TEST_SCRIPTS_API);
		harness.Run("script.add", { { "entity", harness.CreateEntity("Holder") }, { "class", "Idle" } });
		harness.Run("scene.saveAs", { { "path", "Scenes/Main.stscene" } });
		harness.Context.GetScriptEngine()->UnloadModule();

		const std::filesystem::path build = harness.Directory.parent_path() / "Build";
		const std::string error = harness.Error("project.export", { { "directory", FileSystem::ToUTF8(build) }, { "includeRuntime", false } });
		CHECK(error.find("Scenes/Main.stscene") != std::string::npos);
		CHECK(error.find("script.build") != std::string::npos);

		// Without scripts in its scenes the game needs no module.
		harness.Run("script.remove", { { "entity", harness.Run("entity.find", { { "name", "Holder" } })["entities"][0] }, { "class", "Idle" } });
		harness.Run("scene.save");
		const nlohmann::json exported = harness.Run("project.export", { { "directory", FileSystem::ToUTF8(build) }, { "includeRuntime", false } });
		CHECK(exported["scriptModule"].is_null());
		const std::optional<GameManifest> manifest = GameManifest::Load(FileSystem::FromUTF8(exported["manifest"].get<std::string>()));
		REQUIRE(manifest);
		CHECK(manifest->ScriptModule.empty());
	}

	TEST_CASE("Script commands describe themselves for automation")
	{
		ScriptHarness harness;
		for (const char* name : { "script.status", "script.build", "script.reload", "script.load", "script.init", "script.add", "script.remove", "script.setField" })
		{
			INFO(name);
			const EditorCommand* command = harness.Commands.Find(name);
			REQUIRE(command);
			CHECK(command->Description.size() > 40);
			CHECK(command->Parameters["type"] == "object");
		}
		CHECK(harness.Error("script.setField", { { "entity", "1" }, { "class", "A" }, { "field", "B" } }).find("value") != std::string::npos);

		// Mistakes in a request are InvalidParameters; requests the editor's state does not allow are Failed.
		const std::string entity = harness.CreateEntity("Holder");
		CHECK(harness.ErrorKind("script.add", { { "entity", entity }, { "class", "Idle" } }) == EditorCommandError::Failed); // No module yet
		CHECK(harness.ErrorKind("script.load", { { "path", "Missing.dll" } }) == EditorCommandError::InvalidParameters);
		harness.LoadModule(STRATA_TEST_SCRIPTS_API);
		CHECK(harness.ErrorKind("script.add", { { "entity", entity }, { "class", "NoSuchClass" } }) == EditorCommandError::InvalidParameters);
		CHECK(harness.ErrorKind("script.add", { { "entity", entity }, { "class", "FieldTypes" }, { "fields", { { "Missing", 1 } } } })
			== EditorCommandError::InvalidParameters);
		CHECK(harness.ErrorKind("script.add", { { "entity", entity }, { "class", "FieldTypes" }, { "fields", { { "IntField", "seven" } } } })
			== EditorCommandError::InvalidParameters);
		CHECK(harness.ErrorKind("script.remove", { { "entity", entity }, { "class", "FieldTypes" } }) == EditorCommandError::InvalidParameters);
		harness.Run("script.add", { { "entity", entity }, { "class", "FieldTypes" } });
		CHECK(harness.ErrorKind("script.add", { { "entity", entity }, { "class", "FieldTypes" } }) == EditorCommandError::Failed); // Attached already
		CHECK(harness.ErrorKind("script.setField", { { "entity", entity }, { "class", "FieldTypes" }, { "field", "Nope" }, { "value", 1 } })
			== EditorCommandError::InvalidParameters);
		CHECK(harness.ErrorKind("script.setField", { { "entity", entity }, { "class", "FieldTypes" }, { "field", "IntField" }, { "value", "seven" } })
			== EditorCommandError::InvalidParameters);
		CHECK(harness.ErrorKind("script.setField", { { "entity", entity }, { "class", "Idle" }, { "field", "Nope" }, { "value", 1 } })
			== EditorCommandError::InvalidParameters);
	}
}
