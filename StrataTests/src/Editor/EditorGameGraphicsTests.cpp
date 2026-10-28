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
}
