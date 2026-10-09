#include <doctest/doctest.h>

#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Scripting/ScriptTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Runtime/GameRuntime.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scripting/ScriptSystem.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

// Script builds with the real toolchain (CMake and the engine's compiler): the suites whose names start with "Package"
// run as the CTest StrataTests.Package (label "package"), like StrataScriptCore.Package.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A build of a whole script module (configure included) stays well within this on a developer machine or CI runner.
	constexpr std::chrono::minutes c_BuildTimeout(10);

	// The value of a CMakeCache.txt entry ("NAME:TYPE=value"), empty when missing.
	std::string GetCacheEntry(const std::string& cache, const std::string& name)
	{
		const std::string prefix = "\n" + name + ":";
		const size_t start = ("\n" + cache).find(prefix);
		if (start == std::string::npos)
			return {};
		const size_t equals = cache.find('=', start);
		if (equals == std::string::npos)
			return {};
		const size_t end = cache.find_first_of("\r\n", equals);
		return cache.substr(equals + 1, end == std::string::npos ? std::string::npos : end - equals - 1);
	}

	// The engine's script build settings in another configuration (empty: the engine's own).
	EditorContextSpecification MakeSpecification(const std::string& configuration)
	{
		EditorContextSpecification specification { false, true };
		if (!configuration.empty())
			specification.ScriptBuild.Configuration = configuration;
		return specification;
	}

	struct BuildHarness
	{
		EditorContext Context;
		EditorCommandRegistry Commands;
		EditorCommandRunner Runner;
		std::filesystem::path Directory;

		explicit BuildHarness(const std::string& configuration = {})
			: Context(MakeSpecification(configuration)), Directory(CreateTemporaryDirectory("ScriptBuildPackage") / "Counter Game")
		{
			Run("project.create", { { "directory", FileSystem::ToUTF8(Directory) }, { "name", "Counter Game" } });
		}

		~BuildHarness()
		{
			Runner.CancelAll("The test ended");
		}

		void Frame()
		{
			Context.Update(Timestep(1.0f / 60.0f));
			Runner.Update(Context);
		}

		// Runs a command through the runner (as the UI and command scripts do) until it completes.
		EditorCommandResult RunToCompletion(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			std::optional<EditorCommandResult> completed;
			Runner.Run(Context, Commands, name, parameters, [&](const EditorCommandResult& result) { completed = result; });
			const auto deadline = std::chrono::steady_clock::now() + c_BuildTimeout;
			while (!completed && std::chrono::steady_clock::now() < deadline)
			{
				Frame();
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			REQUIRE_MESSAGE(completed, std::string(name), " did not complete in time");
			return *completed;
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = RunToCompletion(name, parameters);
			INFO(std::string(name), ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		}

		void WriteScript(const std::string& source)
		{
			REQUIRE(FileSystem::WriteText(Context.GetProject()->GetScriptSourceDirectory() / "Counter.cpp", source));
		}

		void Frames(int count)
		{
			for (int frame = 0; frame < count; frame++)
				Frame();
		}
	};

	// The counter script: version 1 adds Step every update; version 2 adds a hundred times Step.
	std::string MakeCounterScript(int version)
	{
		return std::string(R"(#include "StrataScript/StrataScript.h"

#include <string>

using namespace Strata;

class Counter : public Script
{
public:
	int32_t Count = 0;
	int32_t Step = 1;
	int32_t Reloads = 0;
	std::string Version;

	void OnUpdate(float) override
	{
		Count += Step * )") + (version == 1 ? "1" : "100") + R"(;
		Version = ")" + std::to_string(version) + R"(";
	}

	void OnReload() override
	{
		Reloads++;
	}
};

ST_SCRIPT_CLASS(Counter)
{
	ST_SCRIPT_FIELD(Count);
	ST_SCRIPT_FIELD(Step);
	ST_SCRIPT_FIELD(Reloads);
	ST_SCRIPT_FIELD(Version);
}
)";
	}

	// A compile error on line 4.
	constexpr const char* c_BrokenScript = "#include \"StrataScript/StrataScript.h\"\n\nusing namespace Strata;\nint Broken() { return undeclaredValue; }\n";

}

TEST_SUITE("Package.ScriptBuild")
{
	TEST_CASE("Scripts are built, attached, hot-reloaded while playing and exported")
	{
		BuildHarness harness;
		harness.WriteScript(MakeCounterScript(1));

		// The first build configures the build tree, then loads the module.
		const nlohmann::json first = harness.Run("script.build");
		CHECK(first["success"] == true);
		CHECK(first["configured"] == true);
		CHECK(first["loaded"] == true);
		CHECK(first["reloaded"] == false);
		CHECK(first["moduleChanged"] == true);
		const Ref<ScriptEngine> engine = harness.Context.GetScriptEngine();
		REQUIRE(engine->IsModuleLoaded());
		CHECK(engine->GetModulePath() == harness.Context.GetProject()->GetScriptModulePath());
		CHECK(engine->FindClass("Counter"));
		CHECK(engine->FindClass("Spinner")); // The example script of new projects
		CHECK(engine->IsHotReloadEnabled());

		// One build at a time: a request while one runs fails; the build that runs reuses the configured tree.
		const nlohmann::json started = harness.Run("script.build", { { "wait", false } });
		CHECK(started["running"] == true);
		const EditorCommandResult second = harness.Commands.Execute(harness.Context, "script.build");
		CHECK_FALSE(second.Success);
		CHECK(second.Error.find("still running") != std::string::npos);
		const auto deadline = std::chrono::steady_clock::now() + c_BuildTimeout;
		while (harness.Context.GetScriptBuilder().IsRunning() && std::chrono::steady_clock::now() < deadline)
		{
			harness.Frame();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		REQUIRE_FALSE(harness.Context.GetScriptBuilder().IsRunning());
		CHECK(harness.Context.GetScriptBuilder().GetLastResult().Success);
		CHECK_FALSE(harness.Context.GetScriptBuilder().GetLastResult().Configured);
		CHECK(harness.Context.GetLastScriptBuildLoad().Loaded);

		// Attach and play.
		const std::string id = harness.Run("entity.create", { { "name", "Counter Host" } })["id"].get<std::string>();
		harness.Run("script.add", { { "entity", id }, { "class", "Counter" }, { "fields", { { "Step", 2 } } } });
		harness.Run("scene.saveAs", { { "path", "Scenes/Main.stscene" } });
		harness.Run("project.setStartScene", { { "scene", "Scenes/Main.stscene" } });
		harness.Run("play.start");
		harness.Frames(3);
		Ref<Scene> running = harness.Context.GetActiveScene();
		ScriptSystem* system = &GetScriptSystem(*running);
		const Entity host = running->GetEntityByUUID(*UUIDFromJson(id));
		CHECK(GetField<int32_t>(*system, host, "Counter", "Count") == 6);
		CHECK(GetField<std::string>(*system, host, "Counter", "Version") == "1");

		// Changed code is rebuilt and hot-reloaded into the running scene (which keeps playing during the build): fields
		// survive, OnReload runs, the new code runs.
		harness.WriteScript(MakeCounterScript(2));
		const nlohmann::json reloaded = harness.Run("script.build");
		CHECK(reloaded["reloaded"] == true);
		CHECK(reloaded["moduleChanged"] == true);
		REQUIRE(harness.Context.IsPlaying());
		harness.Frames(1);
		CHECK(GetField<int32_t>(*system, host, "Counter", "Reloads") == 1);
		CHECK(GetField<std::string>(*system, host, "Counter", "Version") == "2");
		const int32_t countAfterReload = GetField<int32_t>(*system, host, "Counter", "Count");
		CHECK(countAfterReload > 6);
		harness.Frames(1);
		CHECK(GetField<int32_t>(*system, host, "Counter", "Count") == countAfterReload + 200);

		// A compile error fails the build with its location; the running module stays.
		const uint64_t loads = engine->GetLoadCount();
		harness.WriteScript(c_BrokenScript);
		const EditorCommandResult broken = harness.RunToCompletion("script.build");
		REQUIRE_FALSE(broken.Success);
		CHECK(broken.Error.find("Counter.cpp") != std::string::npos);
		const nlohmann::json last = harness.Run("script.status")["build"]["last"];
		CHECK(last["success"] == false);
		CHECK(last["errorCount"].get<int>() >= 1);
		CHECK_FALSE(last["log"].get<std::string>().empty());
		const nlohmann::json* error = nullptr;
		for (const nlohmann::json& diagnostic : last["diagnostics"])
		{
			if (diagnostic["severity"] == "error" && !error)
				error = &diagnostic;
		}
		REQUIRE(error);
		INFO("Diagnostics: ", last["diagnostics"].dump());
		CHECK((*error)["file"].get<std::string>().find("Counter.cpp") != std::string::npos);
		CHECK((*error)["line"] == 4);
		CHECK(engine->GetLoadCount() == loads);
		CHECK(harness.Context.IsPlaying());
		const int32_t countBefore = GetField<int32_t>(*system, host, "Counter", "Count");
		harness.Frames(1);
		CHECK(GetField<int32_t>(*system, host, "Counter", "Count") == countBefore + 200); // The previous module still runs

		// Fixed again, stopped, exported: the game runs the scripts in the game runtime.
		harness.WriteScript(MakeCounterScript(2));
		harness.Run("script.build");
		harness.Run("play.stop");
		const std::filesystem::path build = harness.Directory.parent_path() / "Build";
		const nlohmann::json exported = harness.Run("project.export", { { "directory", FileSystem::ToUTF8(build) }, { "includeRuntime", false } });
		REQUIRE(exported["scriptModule"].is_string());
		std::string gameError;
		Scope<GameRuntime> game = GameRuntime::Create(FileSystem::FromUTF8(exported["manifest"].get<std::string>()), &gameError);
		REQUIRE_MESSAGE(game, gameError);
		REQUIRE(game->GetScriptEngine());
		CHECK(game->GetScriptEngine()->FindClass("Counter"));
		for (int frame = 0; frame < 2; frame++)
			game->Update(Timestep(1.0f / 60.0f));
		const Entity gameHost = game->GetScene()->FindEntityByName("Counter Host");
		CHECK(GetField<int32_t>(GetScriptSystem(*game->GetScene()), gameHost, "Counter", "Count") == 2 * 200);
	}

	TEST_CASE("Scripts build in the Dist configuration")
	{
		// A Dist editor builds its scripts in Dist, a configuration CMake does not define. Modules talk to the engine
		// through the C ABI only, so this (Debug or Release) engine loads them as well.
		BuildHarness harness("Dist");
		harness.WriteScript(MakeCounterScript(1));
		const nlohmann::json built = harness.Run("script.build");
		CHECK(built["loaded"] == true);
		CHECK(harness.Context.GetScriptEngine()->FindClass("Counter"));
		CHECK(harness.Context.GetScriptEngine()->GetModulePath() == harness.Context.GetProject()->GetScriptModulePath());

		// Dist compiles with the Release flags (CMake leaves the flags of configurations it does not know empty).
		const std::optional<std::string> cache = FileSystem::ReadText(harness.Context.GetProject()->GetScriptBuildDirectory() / "CMakeCache.txt");
		REQUIRE(cache);
		const std::string distFlags = GetCacheEntry(*cache, "CMAKE_CXX_FLAGS_DIST");
		CHECK_FALSE(distFlags.empty());
		CHECK(distFlags == GetCacheEntry(*cache, "CMAKE_CXX_FLAGS_RELEASE"));
	}
}
