#include <doctest/doctest.h>

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Runtime/GameRuntime.h>
#include <Strata/Scene/Entity.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A project with two saved scenes: "Levels/One.stscene" (with an entity "Hero", the edited scene) and
	// "Levels/Two.stscene" (with "Boss").
	struct GameFlowProject
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		UUID LevelOne = UUID::Null();
		UUID LevelTwo = UUID::Null();
		UUID Material = UUID::Null();

		explicit GameFlowProject(const std::filesystem::path& directory)
		{
			Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Flow" } });
			Material = ToUUID(Run("material.create", { { "path", "Materials/Plain.stmat" } })["asset"]);
			Run("entity.create", { { "name", "Boss" } });
			LevelTwo = ToUUID(Run("scene.saveAs", { { "path", "Levels/Two.stscene" } })["scene"]);
			Run("scene.new");
			Run("entity.create", { { "name", "Hero" } });
			LevelOne = ToUUID(Run("scene.saveAs", { { "path", "Levels/One.stscene" } })["scene"]);
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			INFO(std::string(name), ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		}

		static UUID ToUUID(const nlohmann::json& json)
		{
			const std::optional<UUID> uuid = UUIDFromJson(json);
			REQUIRE(uuid.has_value());
			return *uuid;
		}

		void Update()
		{
			Context.Update(Timestep(1.0f / 60.0f));
		}
	};

	// Lowers the engine's and the application's loggers to Info while it lives, so that the quit messages are captured.
	class ScopedInfoLogging
	{
	public:
		ScopedInfoLogging()
			: m_Core(Log::GetCoreLogger()->level()), m_Client(Log::GetClientLogger()->level())
		{
			Log::GetCoreLogger()->set_level(spdlog::level::info);
			Log::GetClientLogger()->set_level(spdlog::level::info);
		}

		~ScopedInfoLogging()
		{
			Log::GetCoreLogger()->set_level(m_Core);
			Log::GetClientLogger()->set_level(m_Client);
		}

		ScopedInfoLogging(const ScopedInfoLogging&) = delete;
		ScopedInfoLogging& operator=(const ScopedInfoLogging&) = delete;
	private:
		spdlog::level::level_enum m_Core;
		spdlog::level::level_enum m_Client;
	};
	bool Logged(const std::vector<LogEntry>& entries, std::string_view text)
	{
		return std::any_of(entries.begin(), entries.end(), [&](const LogEntry& entry) { return entry.Message.find(text) != std::string::npos; });
	}

}

TEST_SUITE("Editor.GameFlow")
{
	TEST_CASE("Play mode stops when the game quits and switches scenes when it asks")
	{
		GameFlowProject project(CreateTemporaryDirectory("EditorGameFlow") / "Flow");
		EditorContext& context = project.Context;
		ScopedInfoLogging infoLogging;
		LogCapture log;

		// Quitting stops play mode.
		project.Run("play.start");
		context.GetActiveScene()->RequestQuit(5);
		project.Update();
		CHECK_FALSE(context.IsPlaying());
		CHECK(context.GetActiveScene() == context.GetEditScene());
		CHECK(Logged(log.GetEntries(), "The game quit with exit code 5"));

		// A scene load replaces the running scene; quitting wins over a load requested in the same frame.
		project.Run("play.start");
		context.GetActiveScene()->RequestSceneLoad(project.LevelTwo);
		project.Update();
		REQUIRE(context.IsPlaying());
		Ref<Scene> running = context.GetActiveScene();
		CHECK(running->IsRunning());
		CHECK(running->FindEntityByName("Boss").IsValid());
		CHECK_FALSE(running->FindEntityByName("Hero").IsValid());

		// The null handle restarts the scene that runs (Two now), from its asset.
		running->DestroyEntity(running->FindEntityByName("Boss"));
		REQUIRE_FALSE(running->FindEntityByName("Boss").IsValid());
		running->RequestSceneLoad(UUID::Null());
		project.Update();
		REQUIRE(context.GetActiveScene() != running);
		running = context.GetActiveScene();
		CHECK(running->IsRunning());
		CHECK(running->FindEntityByName("Boss").IsValid());

		// What cannot be loaded is reported; the scene keeps running.
		running->RequestSceneLoad(project.Material);
		project.Update();
		CHECK(context.GetActiveScene() == running);
		CHECK(running->IsRunning());
		CHECK(Logged(log.GetEntries(), "The game cannot switch scenes"));
		CHECK_FALSE(running->GetSceneLoadRequest().has_value()); // Not retried every frame

		running->RequestSceneLoad(project.LevelOne);
		running->RequestQuit(0);
		project.Update();
		CHECK_FALSE(context.IsPlaying());

		// Stopping returns to the edited scene, untouched; restarting a copy of it includes unsaved changes.
		CHECK(context.GetEditScene()->FindEntityByName("Hero").IsValid());
		project.Run("entity.create", { { "name", "Unsaved" } });
		project.Run("play.start");
		context.GetActiveScene()->RequestSceneLoad(UUID::Null());
		project.Update();
		CHECK(context.IsPlaying());
		CHECK(context.GetActiveScene()->FindEntityByName("Unsaved").IsValid());
		project.Run("play.stop");
		CHECK(context.GetActiveScene() == context.GetEditScene());
	}

	TEST_CASE("A scene switch keeps play mode paused, with the steps still to run")
	{
		GameFlowProject project(CreateTemporaryDirectory("EditorGameFlowPause") / "Flow");
		EditorContext& context = project.Context;
		project.Run("play.start");
		project.Run("play.pause", { { "paused", true } });
		project.Run("play.step", { { "frames", 3 } });
		context.GetActiveScene()->RequestSceneLoad(project.LevelTwo);
		project.Update(); // Runs one step, then the switch

		const Ref<Scene> switched = context.GetActiveScene();
		REQUIRE(switched->FindEntityByName("Boss").IsValid());
		CHECK(context.IsPaused());
		CHECK(switched->GetStepFrames() == 2);
		for (int32_t frame = 0; frame < 3; frame++)
			project.Update();
		CHECK(switched->GetFrameIndex() == 2); // The two remaining steps, then nothing: still paused
		CHECK(context.IsPaused());
		project.Run("play.stop");
	}
	TEST_CASE("Exported games quit with an exit code and switch scenes when their scene asks")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("GameRuntimeFlow");
		GameFlowProject project(directory / "Flow");
		project.Run("project.setStartScene", { { "scene", UUIDToJson(project.LevelOne) } });
		const nlohmann::json exported = project.Run("project.export", { { "directory", FileSystem::ToUTF8(directory / "Build") }, { "includeRuntime", false } });
		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(FileSystem::FromUTF8(exported["manifest"].get<std::string>()), &error);
		REQUIRE_MESSAGE(game, error);
		ScopedInfoLogging infoLogging;
		LogCapture log;
		const Timestep frame(1.0f / 60.0f);

		game->GetScene()->RequestSceneLoad(project.LevelTwo);
		game->Update(frame);
		CHECK(game->GetSceneHandle() == project.LevelTwo);
		CHECK(game->GetScene()->IsRunning());
		CHECK(game->GetScene()->FindEntityByName("Boss").IsValid());

		// The null handle restarts the current scene.
		const Ref<Scene> previous = game->GetScene();
		previous->RequestSceneLoad(UUID::Null());
		game->Update(frame);
		CHECK(game->GetScene() != previous);
		CHECK_FALSE(previous->IsRunning());
		CHECK(game->GetSceneHandle() == project.LevelTwo);

		// What cannot be loaded is reported; the scene keeps running and the request is not retried.
		const Ref<Scene> current = game->GetScene();
		current->RequestSceneLoad(project.Material);
		game->Update(frame);
		CHECK(game->GetScene() == current);
		CHECK(Logged(log.GetEntries(), "cannot switch scenes"));
		CHECK_FALSE(current->GetSceneLoadRequest().has_value());

		// Quitting wins over a load; the game then stands still.
		CHECK_FALSE(game->GetQuitRequest().has_value());
		current->RequestSceneLoad(project.LevelOne);
		current->RequestQuit(7);
		game->Update(frame);
		CHECK(game->GetQuitRequest() == 7);
		CHECK(game->GetScene() == current);
		CHECK(game->GetSceneHandle() == project.LevelTwo);
		const uint64_t frameIndex = current->GetFrameIndex();
		game->Update(frame);
		CHECK(current->GetFrameIndex() == frameIndex);
		CHECK(Logged(log.GetEntries(), "quit with exit code 7"));
	}
}
