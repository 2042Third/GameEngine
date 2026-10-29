#include <doctest/doctest.h>

#include "Renderer/QualityTestModel.h"
#include "TestHelpers.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SaveFiles.h"
#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Renderer/RendererQuality.h"
#include "Strata/Runtime/GameGraphicsService.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;

namespace
{

	// The renderer's section in a registry of its own, a project's values and a save directory of the test's.
	struct GraphicsProject
	{
		SettingsRegistry Registry;
		ProjectSettingsValues Values;
		std::filesystem::path Directory;
		SaveDirectory Saves;

		explicit GraphicsProject(const std::string& name)
			: Directory(Tests::CreateTemporaryDirectory(name)), Saves(Directory)
		{
			REQUIRE(Registry.Register(RendererQuality::MakeSection()));
			Registry.Freeze();
		}

		const SettingsSection& GetSection() const { return *Registry.Find(RendererQuality::c_SectionId); }

		void Set(const nlohmann::json& value)
		{
			std::string error;
			REQUIRE_MESSAGE(Values.Set(RendererQuality::c_SectionId, value, Registry, &error), error);
		}

		std::filesystem::path GetFile() const { return Directory / GraphicsOptionsFile::c_FileName; }

		void WriteFile(std::string_view text) const
		{
			REQUIRE(FileSystem::WriteText(GetFile(), text));
		}

		// The project's value as the service sees it, spelled out.
		nlohmann::json GetProjectEffective() const
		{
			const nlohmann::json* value = Values.FindSection(RendererQuality::c_SectionId);
			return RendererQuality::Normalize(value ? *value : GetSection().GetDefault(), RendererQuality::GetOptionModel(GetSection()));
		}
	};

	// The test section of QualityTestModel in a registry, with a project's values and a save directory.
	struct TestModelProject
	{
		Tests::QualityTestModel Test;
		SettingsRegistry Registry;
		ProjectSettingsValues Values;
		std::filesystem::path Directory;
		SaveDirectory Saves;
		RendererQuality::OptionModel Model;

		TestModelProject(const std::string& name, uint32_t version = 1)
			: Test(version), Directory(Tests::CreateTemporaryDirectory(name)), Saves(Directory)
		{
			REQUIRE(Registry.Register(Test.Section));
			Registry.Freeze();
			Model = Test.GetModel();
			Model.Section = Registry.Find(Tests::QualityTestModel::c_SectionId);
		}
	};

	nlohmann::json PresetValue(RendererQualityPreset preset)
	{
		return RendererQuality::GetPresetValues()[static_cast<size_t>(preset)];
	}

	// Every field's source: `expected` for the preset and the rows that follow it; the project's for the players' rows the
	// presets leave alone (anti-aliasing, motion blur...), which no case that checks sources this way chooses.
	void CheckSources(GameGraphicsService& service, const SettingsSection& section, GameGraphicsSource expected)
	{
		for (const SettingsField& field : section.Fields)
		{
			CAPTURE(field.Name);
			bool followsPreset = true;
			for (const RendererQuality::RowTraits& row : RendererQuality::GetRowTraits())
				followsPreset = row.Field == field.Name ? row.FollowsPreset : followsPreset;
			CHECK(service.GetSource(field.Name) == (followsPreset ? expected : GameGraphicsSource::Project));
		}
	}

	std::string ExpectedFile(std::string_view options)
	{
		return fmt::format("{{\n\t\"Options\": {}\n\t\"Section\": {{\n\t\t\"Id\": \"Renderer.Quality\",\n\t\t\"Version\": {}\n\t}},\n\t\"Strata\": {{\n"
			"\t\t\"Format\": \"GraphicsOptions\",\n\t\t\"Version\": 1\n\t}}\n}}\n", options, RendererQuality::c_SectionVersion);
	}

	size_t CountWarnings(const Tests::LogCapture& log, std::string_view text)
	{
		size_t count = 0;
		for (const LogEntry& entry : log.GetEntries())
		{
			if (entry.Level == LogLevel::Warn && entry.Message.find(text) != std::string::npos)
				count++;
		}
		return count;
	}

}

TEST_SUITE("Runtime.GameGraphics")
{
	TEST_CASE("Without choices the value in effect is the project's, spelled out")
	{
		GraphicsProject project("GameGraphicsProject");
		GameGraphicsService service(&project.Values, &project.Saves, &project.Registry);
		REQUIRE(service.GetSection() == &project.GetSection());
		CHECK(service.GetEffectiveValue() == PresetValue(RendererQualityPreset::High));
		CHECK(service.GetPlayerOptions().empty());
		CheckSources(service, project.GetSection(), GameGraphicsSource::Project);
		CHECK(service.GetQuality().HasSameKnobs(RendererQuality::GetPreset(RendererQualityPreset::High)));

		project.Set({ { "Preset", "Medium" }, { "ShadowCascades", 2 } });
		CHECK(service.GetEffectiveValue() == project.GetProjectEffective());
		CHECK(service.GetEffectiveValue()["ShadowCascades"] == 4);
		CHECK(service.GetQuality().HasSameKnobs(RendererQuality::GetPreset(RendererQualityPreset::Medium)));
		CHECK_FALSE(service.GetSource("NoSuchOption").has_value());

		// Without the renderer's section there are no options.
		SettingsRegistry empty;
		GameGraphicsService none(&project.Values, &project.Saves, &empty);
		CHECK(none.GetSection() == nullptr);
		std::string error;
		CHECK_FALSE(none.SetOption("Preset", "Low", &error));
		CHECK_FALSE(error.empty());
		CHECK_FALSE(none.SaveOptions());
		CHECK_FALSE(none.GetSource("Preset").has_value());
	}

	TEST_CASE("A preset choice keeps only the preset and the choices the presets leave alone")
	{
		TestModelProject project("GameGraphicsComfort");
		GameGraphicsService service(&project.Values, &project.Saves, project.Model);
		REQUIRE(service.SetOption("comfort", false));
		CHECK(service.GetPlayerOptions() == nlohmann::json { { "Comfort", false } });
		CHECK(service.GetSource("Comfort") == GameGraphicsSource::Player);
		CHECK(service.GetSource("Detail") == GameGraphicsSource::Project);

		REQUIRE(service.SetOption("Preset", "low"));
		CHECK(service.GetPlayerOptions() == nlohmann::json { { "Preset", "Low" }, { "Comfort", false } });
		CHECK(service.GetEffectiveValue() == nlohmann::json { { "Preset", "Low" }, { "Detail", 1 }, { "Comfort", false } });
		CHECK(service.GetSource("Detail") == GameGraphicsSource::Player);

		// Custom starts from the look in effect, Low's.
		REQUIRE(service.SetOption("Preset", "Custom"));
		CHECK(service.GetPlayerOptions() == nlohmann::json { { "Preset", "Custom" }, { "Detail", 1 }, { "Comfort", false } });
		// A named preset again: the rows that follow it carry no meaning and go; the comfort row stays.
		REQUIRE(service.SetOption("Preset", "Ultra"));
		CHECK(service.GetPlayerOptions() == nlohmann::json { { "Preset", "Ultra" }, { "Comfort", false } });
		CHECK(service.GetEffectiveValue()["Detail"] == 4);
		// A row that does not follow the preset never switches it.
		REQUIRE(service.SetOption("Comfort", true));
		CHECK(service.GetEffectiveValue()["Preset"] == "Ultra");
	}

	TEST_CASE("Setting a row under a named preset switches to Custom from the look in effect")
	{
		GraphicsProject project("GameGraphicsRow");
		GameGraphicsService service(&project.Values, &project.Saves, &project.Registry);
		REQUIRE(service.SetOption("Preset", "Low"));
		CHECK(service.GetQuality().HasSameKnobs(RendererQuality::GetPreset(RendererQualityPreset::Low)));
		std::string error;
		REQUIRE_MESSAGE(service.SetOption("shadowcascades", 2, &error), error);
		const nlohmann::json& effective = service.GetEffectiveValue();
		CHECK(effective["Preset"] == "Custom");
		CHECK(effective["ShadowCascades"] == 2);
		CHECK(effective["ShadowResolution"] == "1024");
		CHECK(effective["LocalShadowAtlasSize"] == "2048");
		CheckSources(service, project.GetSection(), GameGraphicsSource::Player);
		RendererQualitySettings expected = RendererQuality::GetPreset(RendererQualityPreset::Low);
		expected.ShadowCascades = 2;
		CHECK(service.GetQuality().HasSameKnobs(expected));
		CHECK(service.GetQuality().Preset == RendererQualityPreset::Custom);
		// Under the player's Custom a row is set alone.
		REQUIRE(service.SetOption("LODErrorPixels", 0.75));
		CHECK(service.GetEffectiveValue()["ShadowCascades"] == 2);
		CHECK(service.GetEffectiveValue()["LODErrorPixels"] == 0.75);
	}

	TEST_CASE("The renderer's players' rows are chosen apart from the preset, also under the command line's")
	{
		GraphicsProject project("GameGraphicsPlayersRows");
		GameGraphicsService service(&project.Values, &project.Saves, &project.Registry);
		REQUIRE(service.SetOption("MotionBlur", "Off"));
		REQUIRE(service.SetOption("antialiasing", "fxaa"));
		CHECK(service.GetPlayerOptions() == nlohmann::json { { "AntiAliasing", "FXAA" }, { "MotionBlur", "Off" } });
		CHECK(service.GetEffectiveValue()["Preset"] == "High");
		CHECK(service.GetSource("MotionBlur") == GameGraphicsSource::Player);
		CHECK(service.GetSource("DepthOfField") == GameGraphicsSource::Project);
		CHECK(service.GetSource("MotionBlurQuality") == GameGraphicsSource::Project);
		CHECK(service.GetQuality().MotionBlur == SceneEffectChoice::Off);
		CHECK(service.GetQuality().AntiAliasing == AntiAliasingChoice::FXAA);
		// A preset choice keeps them, and so does the command line's preset, which decides the rows that follow it.
		REQUIRE(service.SetOption("Preset", "Low"));
		CHECK(service.GetPlayerOptions() == nlohmann::json { { "Preset", "Low" }, { "AntiAliasing", "FXAA" }, { "MotionBlur", "Off" } });
		service.SetCommandLinePreset(RendererQualityPreset::Ultra);
		CHECK(service.GetQuality().MotionBlurQuality == MotionBlurSamples::Samples16);
		CHECK(service.GetQuality().MotionBlur == SceneEffectChoice::Off);
		CHECK(service.GetSource("MotionBlurQuality") == GameGraphicsSource::CommandLine);
		CHECK(service.GetSource("MotionBlur") == GameGraphicsSource::Player);
		CHECK(service.GetSource("FilmGrain") == GameGraphicsSource::Project);
	}

	TEST_CASE("A value the section refuses changes nothing and names the field")
	{
		GraphicsProject project("GameGraphicsRefused");
		GameGraphicsService service(&project.Values, &project.Saves, &project.Registry);
		REQUIRE(service.SetOption("Preset", "Low"));
		const nlohmann::json before = service.GetPlayerOptions();
		const uint64_t version = service.GetVersion();
		const auto refused = [&](std::string_view option, const nlohmann::json& value, std::string_view named)
		{
			CAPTURE(std::string(option));
			CAPTURE(value.dump());
			std::string error;
			CHECK_FALSE(service.SetOption(option, value, &error));
			CHECK(error.find(named) != std::string::npos);
			CHECK(service.GetPlayerOptions() == before);
			CHECK(service.GetVersion() == version);
		};
		refused("ShadowCascades", 9, "ShadowCascades");
		refused("Preset", "Epic", "Preset");
		refused("ShadowCascades", true, "ShadowCascades");
		refused("ShadowResolution", 2048, "ShadowResolution");
		refused("LODErrorPixels", 100.0, "LODErrorPixels");
		refused("Resolution", 1080, "there is no graphics option 'Resolution' (options: Preset, ShadowResolution");
	}

	TEST_CASE("Resetting the preset brings the project's quality back; resetting a row under the player's Custom takes the project's")
	{
		GraphicsProject project("GameGraphicsReset");
		project.Set({ { "Preset", "Medium" } });
		GameGraphicsService service(&project.Values, &project.Saves, &project.Registry);
		REQUIRE(service.SetOption("Preset", "Ultra"));
		REQUIRE(service.ResetOptions("preset"));
		CHECK(service.GetPlayerOptions().empty());
		CHECK(service.GetEffectiveValue() == PresetValue(RendererQualityPreset::Medium));
		CheckSources(service, project.GetSection(), GameGraphicsSource::Project);

		// A row under the player's Custom: the project's value for it (Medium's), the player's Custom kept.
		REQUIRE(service.SetOption("Preset", "Ultra"));
		REQUIRE(service.SetOption("ShadowCascades", 2));
		REQUIRE(service.GetEffectiveValue()["Preset"] == "Custom");
		REQUIRE(service.ResetOptions("ShadowCascades"));
		CHECK(service.GetPlayerOptions()["ShadowCascades"] == 4);
		CHECK(service.GetPlayerOptions()["ShadowResolution"] == "AutoFine");
		CHECK(service.GetEffectiveValue()["Preset"] == "Custom");
		CHECK(service.GetSource("ShadowCascades") == GameGraphicsSource::Player);

		// Every choice; an unknown name fails and changes nothing.
		REQUIRE(service.ResetOptions(""));
		CHECK(service.GetPlayerOptions().empty());
		REQUIRE(service.SetOption("Preset", "Low"));
		std::string error;
		CHECK_FALSE(service.ResetOptions("Resolution", &error));
		CHECK(error.find("Resolution") != std::string::npos);
		CHECK(service.GetPlayerOptions() == nlohmann::json { { "Preset", "Low" } });
	}

	TEST_CASE("The command line's preset wins for the session, is its rows' source, and is never written")
	{
		GraphicsProject project("GameGraphicsCommandLine");
		GameGraphicsService service(&project.Values, &project.Saves, &project.Registry);
		service.SetCommandLinePreset(RendererQualityPreset::Low);
		CHECK(service.GetCommandLinePreset() == RendererQualityPreset::Low);
		CHECK(service.GetEffectiveValue() == PresetValue(RendererQualityPreset::Low));
		CheckSources(service, project.GetSection(), GameGraphicsSource::CommandLine);
		CHECK(service.GetQuality().HasSameKnobs(RendererQuality::GetPreset(RendererQualityPreset::Low)));

		// A player's choice is kept as theirs, the command line's preset still in effect.
		REQUIRE(service.SetOption("Preset", "Ultra"));
		CHECK(service.GetPlayerOptions() == nlohmann::json { { "Preset", "Ultra" } });
		CHECK(service.GetEffectiveValue() == PresetValue(RendererQualityPreset::Low));
		CHECK(service.GetSource("Preset") == GameGraphicsSource::CommandLine);
		REQUIRE(service.SaveOptions());
		const std::optional<std::string> text = FileSystem::ReadText(project.GetFile());
		REQUIRE(text);
		CHECK(*text == ExpectedFile("{\n\t\t\"Preset\": \"Ultra\"\n\t},"));

		// Custom is no preset of a command line; none: the player's choice applies.
		service.SetCommandLinePreset(RendererQualityPreset::Custom);
		CHECK_FALSE(service.GetCommandLinePreset().has_value());
		CHECK(service.GetEffectiveValue() == PresetValue(RendererQualityPreset::Ultra));
		CHECK(service.GetSource("Preset") == GameGraphicsSource::Player);
	}

	TEST_CASE("Saving writes GraphicsOptions.json and loading reads it back")
	{
		GraphicsProject project("GameGraphicsSave");
		{
			GameGraphicsService service(&project.Values, &project.Saves, &project.Registry);
			REQUIRE(service.GetOptionsFile() == project.GetFile());
			REQUIRE(service.SetOption("Preset", "Low"));
			std::string error;
			REQUIRE_MESSAGE(service.SaveOptions(&error), error);
		}
		const std::optional<std::string> text = FileSystem::ReadText(project.GetFile());
		REQUIRE(text);
		CHECK(*text == ExpectedFile("{\n\t\t\"Preset\": \"Low\"\n\t},"));
		// Written atomically: nothing but the file is left.
		size_t files = 0;
		for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(project.Directory))
		{
			CAPTURE(FileSystem::ToUTF8(entry.path()));
			CHECK(entry.path().filename() == GraphicsOptionsFile::c_FileName);
			files++;
		}
		CHECK(files == 1);

		GameGraphicsService service(&project.Values, &project.Saves, &project.Registry);
		CHECK(service.ReadSavedOptions() == nlohmann::json { { "Preset", "Low" } });
		CHECK(service.GetPlayerOptions().empty());
		{
			Tests::LogCapture log;
			service.LoadPlayerOptions();
			CHECK(log.GetEntries().empty());
		}
		CHECK(service.GetPlayerOptions() == nlohmann::json { { "Preset", "Low" } });
		CHECK(service.GetEffectiveValue() == PresetValue(RendererQualityPreset::Low));
		// A Custom set round-trips with every row, a reset saved as no choice.
		REQUIRE(service.SetOption("ShadowCascades", 3));
		REQUIRE(service.SaveOptions());
		const nlohmann::json custom = service.GetPlayerOptions();
		GameGraphicsService again(&project.Values, &project.Saves, &project.Registry);
		again.LoadPlayerOptions();
		CHECK(again.GetPlayerOptions() == custom);
		REQUIRE(again.ResetOptions(""));
		REQUIRE(again.SaveOptions());
		CHECK(FileSystem::ReadText(project.GetFile()) == ExpectedFile("{},"));
		// The session ends: the choices go, the file stays.
		REQUIRE(again.SetOption("Preset", "Ultra"));
		again.ResetSession();
		CHECK(again.GetPlayerOptions().empty());
		CHECK(FileSystem::Exists(project.GetFile()));
		REQUIRE(again.DeleteSavedOptions());
		CHECK_FALSE(FileSystem::Exists(project.GetFile()));
		CHECK(again.DeleteSavedOptions());
	}

	TEST_CASE("A file that is no options file is ignored with one warning and the project's quality applies")
	{
		GraphicsProject project("GameGraphicsBadFiles");
		project.Set({ { "Preset", "Medium" } });
		const std::string section = fmt::format("\"Section\": {{\"Id\": \"Renderer.Quality\", \"Version\": {}}}", RendererQuality::c_SectionVersion);
		const std::string strata = "\"Strata\": {\"Format\": \"GraphicsOptions\", \"Version\": 1}";
		const std::string large = fmt::format("{{\"Options\": {{\"Preset\": \"Low\"}}, {}, {}}}", section, strata)
			+ std::string(GraphicsOptionsFile::c_MaxFileBytes, ' ');
		const std::vector<std::pair<std::string, std::string>> files = {
			{ large, "larger than 64 KiB" },
			{ "{\"Options\": ", "no JSON" },
			{ fmt::format("{{\"Options\": {{\"Preset\": \"Low\"}}, {}, \"Strata\": {{\"Format\": \"InputBindings\", \"Version\": 1}}}}", section),
				"no graphics options file" },
			{ fmt::format("{{\"Options\": {{\"Preset\": \"Low\"}}, {}, \"Strata\": {{\"Format\": \"GraphicsOptions\", \"Version\": 2}}}}", section),
				"later version of the engine" },
			{ fmt::format("{{\"Options\": {{\"Preset\": \"Low\"}}, \"Section\": {{\"Id\": \"Renderer.Quality\", \"Version\": {}}}, {}}}",
				RendererQuality::c_SectionVersion + 1, strata), "later version of the game" },
			{ fmt::format("{{\"Options\": {{\"Preset\": \"Low\"}}, \"Section\": {{\"Id\": \"Input.Actions\", \"Version\": 1}}, {}}}", strata),
				"another settings section" },
			{ fmt::format("{{\"Options\": [], {}, {}}}", section, strata), "no Options object" },
			{ fmt::format("{{\"Options\": {{\"Preset\": \"Low\", \"preset\": \"High\"}}, {}, {}}}", section, strata), "two members name 'Preset'" },
		};
		for (const auto& [text, reason] : files)
		{
			CAPTURE(reason);
			project.WriteFile(text);
			GameGraphicsService service(&project.Values, &project.Saves, &project.Registry);
			Tests::LogCapture log;
			service.LoadPlayerOptions();
			CHECK(log.GetEntries().size() == 1);
			CHECK(CountWarnings(log, "are ignored (") == 1);
			CHECK(CountWarnings(log, reason) == 1);
			CHECK(service.GetPlayerOptions().empty());
			CHECK(service.GetEffectiveValue() == PresetValue(RendererQualityPreset::Medium));
			std::string problem;
			CHECK(service.ReadSavedOptions(&problem).empty());
			CHECK(problem.find(reason) != std::string::npos);
			// A load never rewrites the file.
			CHECK(FileSystem::ReadText(project.GetFile()) == text);
		}

		// A member spelled "preset" is read as Preset.
		project.WriteFile(fmt::format("{{\"Options\": {{\"preset\": \"Low\"}}, {}, {}}}", section, strata));
		GameGraphicsService service(&project.Values, &project.Saves, &project.Registry);
		Tests::LogCapture log;
		service.LoadPlayerOptions();
		CHECK(log.GetEntries().empty());
		CHECK(service.GetPlayerOptions() == nlohmann::json { { "Preset", "Low" } });
	}

	TEST_CASE("An older section version's file is upgraded and what the section no longer has is dropped with one warning")
	{
		TestModelProject project("GameGraphicsUpgrade", 2);
		REQUIRE(FileSystem::WriteText(project.Directory / GraphicsOptionsFile::c_FileName,
			"{\"Options\": {\"Detail\": 7, \"OldRow\": 1, \"Comfort\": \"yes\"}, \"Section\": {\"Id\": \"Test.Quality\", \"Version\": 1}, "
			"\"Strata\": {\"Format\": \"GraphicsOptions\", \"Version\": 1}}"));
		GameGraphicsService service(&project.Values, &project.Saves, project.Model);
		Tests::LogCapture log;
		service.LoadPlayerOptions();
		const std::vector<LogEntry> entries = log.GetEntries();
		REQUIRE(entries.size() == 1);
		CHECK(entries[0].Level == LogLevel::Warn);
		CHECK(entries[0].Message.find("2 dropped") != std::string::npos);
		CHECK(entries[0].Message.find("'OldRow' is no graphics option of this game") != std::string::npos);
		CHECK(service.GetPlayerOptions() == nlohmann::json { { "Detail", 7 } });
		// The upgraded choices are written at the section's version.
		REQUIRE(service.SaveOptions());
		const std::optional<nlohmann::json> written = JsonUtils::Parse(*FileSystem::ReadText(project.Directory / GraphicsOptionsFile::c_FileName));
		REQUIRE(written);
		CHECK((*written)["Section"]["Version"] == 2);
		CHECK((*written)["Options"] == nlohmann::json { { "Detail", 7 } });
	}

	TEST_CASE("The version changes exactly when the value in effect, a source or the player's choices change, and is unique across services")
	{
		GraphicsProject project("GameGraphicsVersion");
		GameGraphicsService service(&project.Values, &project.Saves, &project.Registry);
		GameGraphicsService other(&project.Values, &project.Saves, &project.Registry);
		const uint64_t first = service.GetVersion();
		CHECK(first != 0);
		CHECK(other.GetVersion() != first);
		CHECK(service.GetVersion() == first);

		REQUIRE(service.SetOption("Preset", "Low"));
		const uint64_t low = service.GetVersion();
		CHECK(low != first);
		// The same choice again changes nothing.
		REQUIRE(service.SetOption("Preset", "Low"));
		CHECK(service.GetVersion() == low);
		// Under the command line a choice changes only the player's choices: the version moves all the same.
		service.SetCommandLinePreset(RendererQualityPreset::Ultra);
		const uint64_t commandLine = service.GetVersion();
		CHECK(commandLine != low);
		const nlohmann::json effective = service.GetEffectiveValue();
		REQUIRE(service.SetOption("Preset", "Medium"));
		CHECK(service.GetEffectiveValue() == effective);
		CHECK(service.GetVersion() != commandLine);
		// A project change that the player's preset hides changes neither the value nor a source.
		const uint64_t before = service.GetVersion();
		project.Set({ { "Preset", "High" } });
		CHECK(service.GetVersion() == before);
	}

	TEST_CASE("A project change reaches the value in effect while the player's choices stay on top")
	{
		GraphicsProject project("GameGraphicsProjectChange");
		project.Set({ { "Preset", "High" } });
		GameGraphicsService service(&project.Values, &project.Saves, &project.Registry);
		// Without choices the project's change shows.
		project.Set({ { "Preset", "Low" } });
		CHECK(service.GetEffectiveValue() == PresetValue(RendererQualityPreset::Low));
		CHECK(service.GetQuality().HasSameKnobs(RendererQuality::GetPreset(RendererQualityPreset::Low)));

		// The player's preset stays over the project's.
		REQUIRE(service.SetOption("Preset", "Ultra"));
		project.Set({ { "Preset", "Medium" } });
		CHECK(service.GetEffectiveValue() == PresetValue(RendererQualityPreset::Ultra));
		// Under the player's Custom the player's rows stay too.
		REQUIRE(service.SetOption("ShadowCascades", 2));
		const nlohmann::json custom = service.GetEffectiveValue();
		project.Set({ { "Preset", "Custom" }, { "ShadowCascades", 1 }, { "LODErrorPixels", 4.0 } });
		CHECK(service.GetEffectiveValue() == custom);
	}

	TEST_CASE("A row set under the project's Custom keeps its meaning when the project's preset changes")
	{
		GraphicsProject project("GameGraphicsProjectCustom");
		project.Set({ { "Preset", "Custom" }, { "ShadowResolution", "1024" }, { "ShadowCascades", 3 } });
		{
			GameGraphicsService service(&project.Values, &project.Saves, &project.Registry);
			REQUIRE(service.SetOption("ShadowCascades", 2));
			// The player's Custom, from the project's tuning, holds every row that follows the preset; the players' rows the
			// presets leave alone stay the project's until the player chooses them.
			const nlohmann::json& player = service.GetPlayerOptions();
			CHECK(player.value("Preset", std::string()) == "Custom");
			CHECK(player.value("ShadowResolution", std::string()) == "1024");
			CHECK(player.value("ShadowCascades", 0) == 2);
			for (const RendererQuality::RowTraits& row : RendererQuality::GetRowTraits())
			{
				CAPTURE(std::string(row.Field));
				CHECK(player.contains(std::string(row.Field)) == row.FollowsPreset);
			}
			REQUIRE(service.SaveOptions());

			// The project's preset changes: the player's tuning stays in effect, and is reported as theirs.
			project.Set({ { "Preset", "High" } });
			CHECK(service.GetEffectiveValue()["Preset"] == "Custom");
			CHECK(service.GetEffectiveValue()["ShadowCascades"] == 2);
			CHECK(service.GetEffectiveValue()["ShadowResolution"] == "1024");
			CheckSources(service, project.GetSection(), GameGraphicsSource::Player);
		}
		// So in a later session.
		GameGraphicsService service(&project.Values, &project.Saves, &project.Registry);
		service.LoadPlayerOptions();
		CHECK(service.GetEffectiveValue()["ShadowCascades"] == 2);
		CHECK(service.GetSource("ShadowCascades") == GameGraphicsSource::Player);
	}

	TEST_CASE("A player's row the project's named preset overrules is the project's in effect and source")
	{
		// A file written by hand: a row without the player's preset.
		GraphicsProject project("GameGraphicsOverruledRow");
		project.WriteFile(fmt::format("{{\"Options\": {{\"ShadowCascades\": 2}}, \"Section\": {{\"Id\": \"Renderer.Quality\", \"Version\": {}}}, "
			"\"Strata\": {{\"Format\": \"GraphicsOptions\", \"Version\": 1}}}}", RendererQuality::c_SectionVersion));
		GameGraphicsService service(&project.Values, &project.Saves, &project.Registry);
		service.LoadPlayerOptions();
		REQUIRE(service.GetPlayerOptions() == nlohmann::json { { "ShadowCascades", 2 } });
		// Under High the member does nothing: High's cascades, the project's.
		CHECK(service.GetEffectiveValue()["ShadowCascades"] == 4);
		CheckSources(service, project.GetSection(), GameGraphicsSource::Project);
		// Under the project's Custom it is in effect: the player's.
		project.Set({ { "Preset", "Custom" }, { "ShadowCascades", 3 } });
		CHECK(service.GetEffectiveValue()["ShadowCascades"] == 2);
		CHECK(service.GetSource("ShadowCascades") == GameGraphicsSource::Player);
		CHECK(service.GetSource("ShadowResolution") == GameGraphicsSource::Project);
		CHECK(service.GetSource("Preset") == GameGraphicsSource::Project);
	}

	TEST_CASE("Without a save directory Save fails with the reason and the choices stay")
	{
		GraphicsProject project("GameGraphicsNoSaves");
		SaveDirectory nowhere;
		GameGraphicsService service(&project.Values, &nowhere, &project.Registry);
		CHECK_FALSE(service.GetOptionsFile().has_value());
		REQUIRE(service.SetOption("Preset", "Low"));
		std::string error;
		CHECK_FALSE(service.SaveOptions(&error));
		CHECK(error.find("no save directory") != std::string::npos);
		CHECK(service.GetPlayerOptions() == nlohmann::json { { "Preset", "Low" } });
		// Loading without one leaves no choice and logs nothing.
		Tests::LogCapture log;
		service.LoadPlayerOptions();
		CHECK(log.GetEntries().empty());
		CHECK(service.GetPlayerOptions().empty());
		GameGraphicsService withoutSaves(&project.Values, nullptr, &project.Registry);
		CHECK_FALSE(withoutSaves.SaveOptions());
		CHECK(withoutSaves.DeleteSavedOptions());
	}
}

// Damaged options files never crash the reader, and what it accepts is valid (a sample of the offsets in the Renderer
// shard, all of them in StrataTests.Fuzz: Tests::SelectSweepOffsets).
TEST_SUITE("Runtime.GameGraphics.Sweep")
{
	TEST_CASE("Every truncation and every single-byte change of a valid options file is read or refused without a crash")
	{
		SettingsRegistry registry;
		REQUIRE(registry.Register(RendererQuality::MakeSection()));
		const SettingsSection& section = *registry.Find(RendererQuality::c_SectionId);
		const nlohmann::json project = section.GetDefault();
		nlohmann::json options = PresetValue(RendererQualityPreset::Ultra);
		options["Preset"] = "Custom";
		options["ShadowCascades"] = 2;
		const std::string file = GraphicsOptionsFile::Write(options, section);
		nlohmann::json read;
		REQUIRE(GraphicsOptionsFile::Read(file, section, project, read));
		REQUIRE(read == options);

		const auto check = [&](std::string_view text)
		{
			nlohmann::json choices;
			std::vector<std::string> dropped;
			std::string error;
			if (GraphicsOptionsFile::Read(text, section, project, choices, &dropped, &error))
			{
				REQUIRE(choices.is_object());
				nlohmann::json merged = project;
				for (const auto& [name, value] : choices.items())
					merged[name] = value;
				CHECK(section.Validate(merged, section.Version).has_value());
			}
			else
			{
				CHECK_FALSE(error.empty());
			}
		};
		for (const size_t length : Tests::SelectSweepOffsets(file.size(), {}, 0, 128))
			check(std::string_view(file).substr(0, length));
		for (const size_t offset : Tests::SelectSweepOffsets(file.size(), {}, 0, 128))
		{
			for (const uint8_t flip : { uint8_t(0xFF), uint8_t(0x20), uint8_t(0x01) })
			{
				std::string damaged = file;
				damaged[offset] = static_cast<char>(static_cast<uint8_t>(damaged[offset]) ^ flip);
				check(damaged);
			}
		}
	}
}
