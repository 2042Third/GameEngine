#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "TestHelpers.h"

#include <Strata/Core/EngineServices.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/ProjectSettings.h>
#include <Strata/Core/SettingsRegistry.h>
#include <Strata/Input/Input.h>
#include <Strata/Project/Project.h>
#include <Strata/Renderer/RendererQuality.h>
#include <Strata/Runtime/GameGraphicsService.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	EditorContextSpecification MakeSpecification()
	{
		EditorContextSpecification specification;
		specification.WatchAssetFiles = false;
		return specification;
	}

	// Runs commands against an editor context without UI, with a project of its own.
	struct GraphicsCommands
	{
		EditorContext Context { MakeSpecification() };
		EditorCommandRegistry Commands;

		explicit GraphicsCommands(const std::string& name)
		{
			Input::Reset();
			const std::filesystem::path directory = CreateTemporaryDirectory(name) / "Game";
			Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });
		}

		~GraphicsCommands()
		{
			Context.Stop();
			Input::Reset();
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return RunCommandToCompletion(Context, Commands, name, parameters);
		}

		void SetProjectQuality(const nlohmann::json& value)
		{
			Run("project.setSettings", { { "section", RendererQuality::c_SectionId }, { "value", value } });
		}

		std::filesystem::path GetFile() { return *Context.GetGameSaveDirectory() / GraphicsOptionsFile::c_FileName; }

		// Writes the players' file as a game's options menu saves it.
		void SavePlayerOptions(const nlohmann::json& options)
		{
			const SettingsSection& section = *Context.GetSettingsRegistry().Find(RendererQuality::c_SectionId);
			REQUIRE(FileSystem::WriteText(GetFile(), GraphicsOptionsFile::Write(options, section)));
		}

		IGameGraphics& Graphics()
		{
			IGameGraphics* graphics = EngineServices::Get<IGameGraphics>();
			REQUIRE(graphics);
			return *graphics;
		}
	};

}

TEST_SUITE("Editor.GameGraphics")
{
	TEST_CASE("Play loads the players' graphics options from .strata/Saves and stop forgets them")
	{
		GraphicsCommands harness("GameGraphicsPlay");
		harness.SetProjectQuality({ { "Preset", "Medium" } });
		harness.SavePlayerOptions({ { "Preset", "Low" } });
		// The editor provides the game's options whenever it runs; outside play they are the project's.
		CHECK(&harness.Graphics() == &harness.Context.GetGameGraphics());
		CHECK(harness.Graphics().GetEffectiveValue()["Preset"] == "Medium");
		CHECK(harness.Context.GetGameQuality().HasSameKnobs(RendererQuality::GetPreset(RendererQualityPreset::Medium)));
		// The file the next session starts with.
		CHECK(harness.Context.GetPlayerGraphicsOptions() == nlohmann::json { { "Preset", "Low" } });

		harness.Run("play.start", { { "build", false } });
		CHECK(harness.Graphics().GetEffectiveValue()["Preset"] == "Low");
		CHECK(harness.Graphics().GetSource("Preset") == GameGraphicsSource::Player);
		CHECK(harness.Context.GetGameQuality().HasSameKnobs(RendererQuality::GetPreset(RendererQualityPreset::Low)));
		// A script's choice during play shows at once.
		REQUIRE(harness.Graphics().SetOption("Preset", "Ultra"));
		CHECK(harness.Context.GetPlayerGraphicsOptions() == nlohmann::json { { "Preset", "Ultra" } });

		harness.Run("play.stop");
		CHECK(harness.Graphics().GetEffectiveValue()["Preset"] == "Medium");
		CHECK(harness.Graphics().GetSource("Preset") == GameGraphicsSource::Project);
		CHECK(harness.Graphics().GetPlayerOptions().empty());
		// The unsaved Ultra is gone; the file stays.
		CHECK(harness.Context.GetPlayerGraphicsOptions() == nlohmann::json { { "Preset", "Low" } });
		CHECK(FileSystem::Exists(harness.GetFile()));
	}

	TEST_CASE("Project changes while playing reach the game under the player's choices")
	{
		GraphicsCommands harness("GameGraphicsProjectChange");
		harness.SetProjectQuality({ { "Preset", "High" } });
		harness.Run("play.start", { { "build", false } });
		CHECK(harness.Graphics().GetEffectiveValue()["Preset"] == "High");
		harness.SetProjectQuality({ { "Preset", "Low" } });
		CHECK(harness.Graphics().GetEffectiveValue()["Preset"] == "Low");
		CHECK(harness.Context.GetGameQuality().HasSameKnobs(RendererQuality::GetPreset(RendererQualityPreset::Low)));

		REQUIRE(harness.Graphics().SetOption("Preset", "Medium"));
		harness.SetProjectQuality({ { "Preset", "Ultra" } });
		CHECK(harness.Graphics().GetEffectiveValue()["Preset"] == "Medium");
		REQUIRE(harness.Graphics().SetOption("ShadowCascades", 2));
		harness.SetProjectQuality({ { "Preset", "Custom" }, { "ShadowCascades", 1 } });
		CHECK(harness.Graphics().GetEffectiveValue()["ShadowCascades"] == 2);
		CHECK(harness.Graphics().GetEffectiveValue()["ShadowResolution"] == "2048");
		harness.Run("play.stop");
		CHECK(harness.Graphics().GetEffectiveValue()["ShadowCascades"] == 1);
	}

	TEST_CASE("graphics.resetPlayerOptions deletes the players' file and, while playing, the game's choices")
	{
		GraphicsCommands harness("GameGraphicsResetCommand");
		harness.SetProjectQuality({ { "Preset", "Medium" } });
		// Nothing to reset is no failure.
		CHECK(harness.Run("graphics.resetPlayerOptions")["reset"] == nlohmann::json::array());

		// Outside play: the file goes, and the next session starts with the project's quality.
		harness.SavePlayerOptions({ { "Preset", "Custom" }, { "ShadowCascades", 2 } });
		const size_t position = harness.Context.GetUndoStack().GetPosition();
		CHECK(harness.Run("graphics.resetPlayerOptions")["reset"] == nlohmann::json::array({ "Preset", "ShadowCascades" }));
		CHECK(harness.Context.GetUndoStack().GetPosition() == position); // Not undoable, like the game's saves
		CHECK_FALSE(FileSystem::Exists(harness.GetFile()));
		CHECK(harness.Context.GetPlayerGraphicsOptions().empty());
		harness.Run("play.start", { { "build", false } });
		CHECK(harness.Graphics().GetEffectiveValue()["Preset"] == "Medium");

		// While playing: the file and the running game's choices, the project's quality from the next frame.
		REQUIRE(harness.Graphics().SetOption("Preset", "Low"));
		REQUIRE(harness.Graphics().SaveOptions());
		REQUIRE(FileSystem::Exists(harness.GetFile()));
		CHECK(harness.Run("graphics.resetPlayerOptions")["reset"] == nlohmann::json::array({ "Preset" }));
		CHECK_FALSE(FileSystem::Exists(harness.GetFile()));
		CHECK(harness.Graphics().GetPlayerOptions().empty());
		CHECK(harness.Context.GetGameQuality().HasSameKnobs(RendererQuality::GetPreset(RendererQualityPreset::Medium)));
		harness.Run("play.stop");
	}

	TEST_CASE("graphics.resetPlayerOptions refuses without a project and changes nothing")
	{
		GraphicsCommands harness("GameGraphicsResetNoProject");
		harness.SavePlayerOptions({ { "Preset", "Low" } });
		const std::filesystem::path file = harness.GetFile();
		harness.Run("project.close");
		const EditorCommandResult refused = harness.Commands.Execute(harness.Context, "graphics.resetPlayerOptions", nlohmann::json::object());
		CHECK_FALSE(refused.Success);
		CHECK(refused.Error.find("No project is open") != std::string::npos);
		CHECK(FileSystem::Exists(file));
		CHECK(harness.Context.GetPlayerGraphicsOptions().empty());
	}

	TEST_CASE("editor.status reports the game's graphics: in play the session's, outside play the file's")
	{
		GraphicsCommands harness("GameGraphicsStatus");
		harness.SetProjectQuality({ { "Preset", "High" } });
		const auto status = [&harness]() { return harness.Run("editor.status", { { "sections", { "game" } } })["game"]["graphics"]; };
		nlohmann::json graphics = status();
		CHECK(graphics["preset"] == "High");
		CHECK(graphics["effective"]["Preset"] == "High");
		CHECK(graphics["sources"]["Preset"] == "project");
		CHECK(graphics["sources"]["ShadowResolution"] == "project");
		CHECK(graphics["playerOptions"] == nlohmann::json::object());
		CHECK(graphics["playerFile"].is_null());
		CHECK(graphics["playerFileProblem"].is_null());

		// A game's options menu saves a choice while playing; outside play the file's options show, what the next session starts
		// with, while the value in effect stays the project's.
		harness.Run("play.start", { { "build", false } });
		REQUIRE(harness.Graphics().SetOption("Preset", "Low"));
		REQUIRE(harness.Graphics().SaveOptions());
		harness.Run("play.stop");
		graphics = status();
		CHECK(graphics["playerOptions"] == nlohmann::json { { "Preset", "Low" } });
		CHECK(graphics["playerFile"] == FileSystem::ToUTF8(harness.GetFile()));
		CHECK(graphics["effective"]["Preset"] == "High");

		// In play: the session's, and a choice a script makes shows in the next answer.
		harness.Run("play.start", { { "build", false } });
		graphics = status();
		CHECK(graphics["preset"] == "Low");
		CHECK(graphics["sources"]["Preset"] == "player");
		CHECK(graphics["sources"]["ShadowCascades"] == "player");
		REQUIRE(harness.Graphics().SetOption("Preset", "Ultra"));
		graphics = status();
		CHECK(graphics["playerOptions"] == nlohmann::json { { "Preset", "Ultra" } });
		CHECK(graphics["effective"]["ShadowResolution"] == "8192");
		harness.Run("play.stop");

		// A file that is no options file (read again when a session ends): its problem, and no options.
		REQUIRE(FileSystem::WriteText(harness.GetFile(), "{ not json"));
		harness.Run("play.start", { { "build", false } });
		harness.Run("play.stop");
		graphics = status();
		CHECK(graphics["playerOptions"] == nlohmann::json::object());
		REQUIRE(graphics["playerFileProblem"].is_string());
		CHECK(graphics["playerFileProblem"].get<std::string>().find("are ignored") != std::string::npos);
	}
}
