#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Runtime/GameRuntime.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/SceneSerializer.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
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
			const EditorCommandResult result = Tests::CompleteCommand(Context, Commands.Execute(Context, name, parameters));
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

	// Calls `update` (one frame) until `done` holds, at most `maxFrames` times; REQUIREs that it came true.
	template<typename Update, typename Done>
	void UpdateUntil(Update&& update, Done&& done, int maxFrames = 600)
	{
		for (int frame = 0; frame < maxFrames && !done(); frame++)
			update();
		REQUIRE(done());
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

		// A scene load replaces the running scene once it is loaded (the running scene goes on meanwhile, and reads the load's
		// progress); quitting wins over a load requested in the same frame.
		project.Run("play.start");
		const Ref<Scene> first = context.GetActiveScene();
		first->RequestSceneLoad(project.LevelTwo);
		project.Update(); // The frame that asks for it starts the load
		CHECK(project.Run("scene.loadStatus")["play"]["scene"] == UUIDToJson(project.LevelTwo));
		UpdateUntil([&] { project.Update(); }, [&]
		{
			if (context.GetActiveScene() == first)
			{
				CHECK(first->IsRunning());
				CHECK(first->GetSceneLoadProgress().has_value());
			}
			return context.GetActiveScene() != first;
		});
		REQUIRE(context.IsPlaying());
		Ref<Scene> running = context.GetActiveScene();
		CHECK(running->IsRunning());
		CHECK_FALSE(first->IsRunning());
		CHECK(running->FindEntityByName("Boss").IsValid());
		CHECK_FALSE(running->FindEntityByName("Hero").IsValid());
		CHECK_FALSE(running->GetSceneLoadProgress().has_value());

		// The null handle restarts the scene that runs (Two now), from its asset.
		running->DestroyEntity(running->FindEntityByName("Boss"));
		REQUIRE_FALSE(running->FindEntityByName("Boss").IsValid());
		running->RequestSceneLoad(UUID::Null());
		UpdateUntil([&] { project.Update(); }, [&] { return context.GetActiveScene() != running; });
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
		const Ref<Scene> previous = context.GetActiveScene();
		previous->RequestSceneLoad(project.LevelTwo);
		// The previous scene runs steps while the next one loads; the steps left go to the next one.
		UpdateUntil([&] { project.Update(); }, [&] { return context.GetActiveScene() != previous; });

		const Ref<Scene> switched = context.GetActiveScene();
		REQUIRE(switched->FindEntityByName("Boss").IsValid());
		CHECK(context.IsPaused());
		CHECK(previous->GetFrameIndex() + switched->GetFrameIndex() + switched->GetStepFrames() == 3);
		CHECK(previous->GetFrameIndex() >= 1); // The step of the frame that asked for the switch
		for (int32_t frame = 0; frame < 3; frame++)
			project.Update();
		// The steps were all run, then nothing: still paused.
		CHECK(switched->GetStepFrames() == 0);
		CHECK(previous->GetFrameIndex() + switched->GetFrameIndex() == 3);
		CHECK(context.IsPaused());
		project.Run("play.stop");
	}
	TEST_CASE("Exported games quit with an exit code and switch scenes when their scene asks")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("GameRuntimeFlow");
		GameFlowProject project(directory / "Flow");
		project.Run("project.setStartScene", { { "scene", UUIDToJson(project.LevelOne) } });
		const nlohmann::json exported = Tests::RunCommandToCompletion(project.Context, project.Commands, "project.export", { { "directory", FileSystem::ToUTF8(directory / "Build") }, { "includeRuntime", false } });
		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(FileSystem::FromUTF8(exported["manifest"].get<std::string>()), &error);
		REQUIRE_MESSAGE(game, error);
		ScopedInfoLogging infoLogging;
		LogCapture log;
		const Timestep frame(1.0f / 60.0f);
		// Nothing plays until the start scene is in.
		UpdateUntil([&] { game->Update(frame); }, [&] { return game->HasStarted(); });
		CHECK(game->GetSceneHandle() == project.LevelOne);

		// The running scene goes on until the next one is in.
		const Ref<Scene> start = game->GetScene();
		start->RequestSceneLoad(project.LevelTwo);
		UpdateUntil([&] { game->Update(frame); }, [&]
		{
			if (game->GetScene() == start)
				CHECK(start->IsRunning());
			return game->GetSceneHandle() == project.LevelTwo;
		});
		CHECK(game->GetScene()->IsRunning());
		CHECK_FALSE(start->IsRunning());
		CHECK(game->GetScene()->FindEntityByName("Boss").IsValid());

		// The null handle restarts the current scene.
		const Ref<Scene> previous = game->GetScene();
		previous->RequestSceneLoad(UUID::Null());
		UpdateUntil([&] { game->Update(frame); }, [&] { return game->GetScene() != previous; });
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

	TEST_CASE("Each scene load reads the scene's document once, in the editor and in exported games")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("SceneLoadReads");
		GameFlowProject project(directory / "Flow");
		EditorContext& context = project.Context;

		// Opening a scene for editing.
		uint64_t reads = SceneSerializer::GetDeserializeCount();
		Tests::RunCommandToCompletion(context, project.Commands, "scene.open", { { "scene", UUIDToJson(project.LevelTwo) } });
		CHECK(SceneSerializer::GetDeserializeCount() - reads == 1);
		REQUIRE(context.GetEditScene()->FindEntityByName("Boss").IsValid());
		// Opening it again reads the file again (once), not a copy kept from before.
		reads = SceneSerializer::GetDeserializeCount();
		Tests::RunCommandToCompletion(context, project.Commands, "scene.open", { { "scene", UUIDToJson(project.LevelOne) } });
		CHECK(SceneSerializer::GetDeserializeCount() - reads == 1);

		// A switch the game asks for in play mode.
		project.Run("play.start");
		reads = SceneSerializer::GetDeserializeCount();
		context.GetActiveScene()->RequestSceneLoad(project.LevelTwo);
		UpdateUntil([&] { project.Update(); }, [&] { return context.GetActiveScene()->FindEntityByName("Boss").IsValid(); });
		CHECK(SceneSerializer::GetDeserializeCount() - reads == 1);
		project.Run("play.stop");

		// The exported game's start scene, and a switch.
		project.Run("project.setStartScene", { { "scene", UUIDToJson(project.LevelOne) } });
		const nlohmann::json exported = Tests::RunCommandToCompletion(context, project.Commands, "project.export",
			{ { "directory", FileSystem::ToUTF8(directory / "Build") }, { "includeRuntime", false } });
		reads = SceneSerializer::GetDeserializeCount();
		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(FileSystem::FromUTF8(exported["manifest"].get<std::string>()), &error);
		REQUIRE_MESSAGE(game, error);
		const Timestep frame(1.0f / 60.0f);
		UpdateUntil([&] { game->Update(frame); }, [&] { return game->GetScene() && game->GetSceneHandle() == project.LevelOne; });
		CHECK(SceneSerializer::GetDeserializeCount() - reads == 1);
		reads = SceneSerializer::GetDeserializeCount();
		game->GetScene()->RequestSceneLoad(project.LevelTwo);
		UpdateUntil([&] { game->Update(frame); }, [&] { return game->GetSceneHandle() == project.LevelTwo; });
		CHECK(SceneSerializer::GetDeserializeCount() - reads == 1);
		CHECK(game->GetScene()->FindEntityByName("Boss").IsValid());
	}

	TEST_CASE("A scene the game asks for in every update loads once and replaces the running scene")
	{
		// Loads on threads of their own, as in the editor: the running scene goes on while the level is read.
		ScopedJobSystem jobs(2, 2);
		const std::filesystem::path directory = CreateTemporaryDirectory("RepeatedSceneSwitch");
		GameFlowProject project(directory / "Flow");
		EditorContext& context = project.Context;
		// A level large enough to take many frames to read.
		project.Run("scene.new");
		Scene& crowd = *context.GetEditScene();
		for (int index = 0; index < 30'000; index++)
			crowd.CreateEntity("Extra");
		const UUID bigLevel = GameFlowProject::ToUUID(project.Run("scene.saveAs", { { "path", "Levels/Big.stscene" } })["scene"]);
		Tests::RunCommandToCompletion(context, project.Commands, "scene.open", { { "scene", UUIDToJson(project.LevelOne) } });

		// As a script does that calls Game::LoadScene in every update while its condition holds: the load goes on (the
		// progress the running scene reads grows, and never falls back), and the level's document is read once.
		project.Run("play.start");
		const Ref<Scene> first = context.GetActiveScene();
		const uint64_t reads = SceneSerializer::GetDeserializeCount();
		float progress = 0.0f;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (context.GetActiveScene() == first)
		{
			REQUIRE(std::chrono::steady_clock::now() < deadline);
			first->RequestSceneLoad(bigLevel);
			project.Update();
			if (const std::optional<float> reported = first->GetSceneLoadProgress(); reported && context.GetActiveScene() == first)
			{
				CHECK(*reported >= progress);
				progress = *reported;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		CHECK(progress > 0.0f);
		CHECK(context.GetActiveScene()->GetEntityCount() == 30'000);
		CHECK(SceneSerializer::GetDeserializeCount() - reads == 1);
		CHECK(project.Run("scene.loadStatus")["play"].is_null());
		project.Run("play.stop");
	}
}
