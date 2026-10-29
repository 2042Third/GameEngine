#include <doctest/doctest.h>

#include "Scripting/ScriptTestUtils.h"
#include "Strata/Core/EngineServices.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SaveFiles.h"
#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Renderer/RendererQuality.h"
#include "Strata/Runtime/GameGraphicsService.h"
#include "StrataScript/ScriptABI.h"
#include "TestHelpers.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// The renderer's section in a registry of its own, a project's values, a save directory and the graphics options over
	// them, provided while it lives (as the program running a game provides them).
	class GraphicsGame
	{
	public:
		explicit GraphicsGame(const std::string& name, bool withSaves = true)
			: m_Saves(withSaves ? CreateTemporaryDirectory(name) : std::filesystem::path())
		{
			REQUIRE(m_Registry.Register(RendererQuality::MakeSection()));
			m_Registry.Freeze();
			m_Graphics = CreateScope<GameGraphicsService>(&m_Values, &m_Saves, &m_Registry);
			m_Provider = CreateScope<EngineServices::ScopedProvider<IGameGraphics>>(*m_Graphics);
		}

		GameGraphicsService& GetGraphics() { return *m_Graphics; }
		const SettingsSection& GetSection() const { return *m_Registry.Find(RendererQuality::c_SectionId); }
		std::optional<std::filesystem::path> GetFile() const { return m_Graphics->GetOptionsFile(); }
	private:
		SettingsRegistry m_Registry;
		ProjectSettingsValues m_Values;
		SaveDirectory m_Saves;
		Scope<GameGraphicsService> m_Graphics;
		Scope<EngineServices::ScopedProvider<IGameGraphics>> m_Provider;
	};

	constexpr int32_t c_List = 1;
	constexpr int32_t c_SetChoice = 2;
	constexpr int32_t c_SetInt = 3;
	constexpr int32_t c_SetFloat = 4;
	constexpr int32_t c_Reset = 5;
	constexpr int32_t c_Save = 6;
	constexpr int32_t c_DescribeRaw = 7;
	constexpr int32_t c_SetPreset = 9;

	// A scene with the API test module's GraphicsProbe, started.
	struct ProbeScene
	{
		ScopedScriptEngine Engine { GetTestScriptModule(STRATA_TEST_SCRIPTS_API) };
		Scene Level;
		Entity Probe;

		ProbeScene()
		{
			Probe = Level.CreateEntity("Probe");
			AddScriptEntry(Probe, "GraphicsProbe");
			Level.OnRuntimeStart();
		}

		// The request done in the probe's next update, with Option and its value.
		void Ask(int32_t request, const std::string& option, const std::string& value = {}, int32_t intValue = 0, float floatValue = 0.0f)
		{
			ScriptSystem& system = GetScriptSystem(Level);
			REQUIRE(system.SetFieldValue(Probe, "GraphicsProbe", "Request", PropertyValue(request)));
			REQUIRE(system.SetFieldValue(Probe, "GraphicsProbe", "Option", PropertyValue(option)));
			REQUIRE(system.SetFieldValue(Probe, "GraphicsProbe", "Value", PropertyValue(value)));
			REQUIRE(system.SetFieldValue(Probe, "GraphicsProbe", "IntValue", PropertyValue(intValue)));
			REQUIRE(system.SetFieldValue(Probe, "GraphicsProbe", "FloatValue", PropertyValue(floatValue)));
			Level.OnUpdateRuntime(1.0f / 60.0f);
		}

		template<typename T>
		T Get(std::string_view field)
		{
			return GetField<T>(GetScriptSystem(Level), Probe, "GraphicsProbe", field);
		}

		void CheckScriptChecks()
		{
			INFO(Get<std::string>("Failure"));
			CHECK(Get<std::string>("Failure").empty());
		}
	};

	// Warnings of a host function (reported as "<function> (called by script ...): <message>") holding the text.
	size_t CountWarnings(const std::vector<LogEntry>& entries, std::string_view function, std::string_view text)
	{
		const std::string prefix = std::string(function) + " (called by script";
		return static_cast<size_t>(std::count_if(entries.begin(), entries.end(), [&](const LogEntry& entry)
		{
			return entry.Level == LogLevel::Warn && entry.Message.starts_with(prefix) && entry.Message.find(text) != std::string::npos;
		}));
	}

	size_t CountAllWarnings(const std::vector<LogEntry>& entries)
	{
		return static_cast<size_t>(std::count_if(entries.begin(), entries.end(), [](const LogEntry& entry) { return entry.Level == LogLevel::Warn; }));
	}

}

TEST_SUITE("Scripting.Graphics")
{
	TEST_CASE("Scripts list, describe and read the options by name")
	{
		GraphicsGame game("ScriptGraphicsList");
		ProbeScene probe;
		LogCapture log;
		probe.Ask(c_List, "shadowresolution");
		const SettingsSection& section = game.GetSection();
		CHECK(probe.Get<int32_t>("Count") == static_cast<int32_t>(section.Fields.size()));
		std::string names;
		for (const SettingsField& field : section.Fields)
			names += (names.empty() ? "" : "\n") + field.Name;
		CHECK(probe.Get<std::string>("Names") == names);
		CHECK(probe.Get<std::string>("PresetChoices") == "Low\nMedium\nHigh\nUltra\nCustom");
		// The project's default, High: the automatic shadow resolution, from the project.
		CHECK(probe.Get<std::string>("Effective") == "Auto");
		CHECK(probe.Get<int32_t>("Source") == StrataScriptGraphicsOptionSource_Project);
		CHECK(probe.Get<int32_t>("Preset") == 2);
		probe.Ask(0, "ShadowCascades");
		CHECK(probe.Get<std::string>("Effective") == "4");
		probe.CheckScriptChecks();
		CHECK(CountAllWarnings(log.GetEntries()) == 0);
	}

	TEST_CASE("Scripts set a preset and a row, and the service follows")
	{
		GraphicsGame game("ScriptGraphicsSet");
		ProbeScene probe;
		probe.Ask(c_SetChoice, "Preset", "low");
		CHECK(probe.Get<bool>("Accepted"));
		CHECK(probe.Get<std::string>("Effective") == "Low");
		CHECK(probe.Get<int32_t>("Source") == StrataScriptGraphicsOptionSource_Player);
		CHECK(game.GetGraphics().GetEffectiveValue()["Preset"] == "Low");
		CHECK(game.GetGraphics().GetQuality().HasSameKnobs(RendererQuality::GetPreset(RendererQualityPreset::Low)));

		// A row under Low: Custom from Low's look.
		probe.Ask(c_SetInt, "ShadowCascades", {}, 2);
		CHECK(probe.Get<bool>("Accepted"));
		CHECK(probe.Get<std::string>("Effective") == "2");
		CHECK(probe.Get<int32_t>("Preset") == 4);
		const nlohmann::json& effective = game.GetGraphics().GetEffectiveValue();
		CHECK(effective["Preset"] == "Custom");
		CHECK(effective["ShadowCascades"] == 2);
		CHECK(effective["ShadowResolution"] == "1024");
		// A Float, a choice by name, an Int for a Float, and the preset through SetPreset.
		probe.Ask(c_SetFloat, "LODErrorPixels", {}, 0, 0.75f);
		CHECK(probe.Get<bool>("Accepted"));
		CHECK(game.GetGraphics().GetEffectiveValue()["LODErrorPixels"] == 0.75);
		// A float the binary form cannot hold exactly is kept as the decimal the script wrote, as the players' file shows it.
		probe.Ask(c_SetFloat, "LODErrorPixels", {}, 0, 0.6f);
		CHECK(probe.Get<bool>("Accepted"));
		CHECK(game.GetGraphics().GetEffectiveValue()["LODErrorPixels"] == 0.6);
		CHECK(game.GetGraphics().GetPlayerOptions()["LODErrorPixels"].dump() == "0.6");
		probe.Ask(c_SetChoice, "ShadowResolution", "4096");
		CHECK(probe.Get<bool>("Accepted"));
		CHECK(probe.Get<std::string>("Effective") == "4096");
		probe.Ask(c_SetInt, "MinProjectedPixels", {}, 1);
		CHECK(probe.Get<bool>("Accepted"));
		CHECK(game.GetGraphics().GetEffectiveValue()["MinProjectedPixels"] == 1.0);
		probe.Ask(c_SetPreset, "Preset", {}, 3);
		CHECK(probe.Get<bool>("Accepted"));
		CHECK(probe.Get<int32_t>("Preset") == 3);
		CHECK(game.GetGraphics().GetPlayerOptions() == nlohmann::json { { "Preset", "Ultra" } });
		probe.CheckScriptChecks();
	}

	TEST_CASE("Invalid names, types and values fail, are reported once and change nothing")
	{
		GraphicsGame game("ScriptGraphicsInvalid");
		ProbeScene probe;
		probe.Ask(c_SetChoice, "Preset", "Medium");
		REQUIRE(probe.Get<bool>("Accepted"));
		const nlohmann::json before = game.GetGraphics().GetPlayerOptions();
		LogCapture log;
		const auto refused = [&](int32_t request, const std::string& option, const std::string& value, int32_t intValue, float floatValue,
			std::string_view function, std::string_view reported)
		{
			CAPTURE(option);
			CAPTURE(std::string(reported));
			// Twice: the second identical problem is not reported again.
			for (int attempt = 0; attempt < 2; attempt++)
			{
				probe.Ask(request, option, value, intValue, floatValue);
				CHECK_FALSE(probe.Get<bool>("Accepted"));
			}
			CHECK(CountWarnings(log.GetEntries(), function, reported) == 1);
			CHECK(game.GetGraphics().GetPlayerOptions() == before);
		};
		// Choices are named: a number is no choice, whatever its position (the same message for both: reported once).
		refused(c_SetInt, "ShadowResolution", {}, 2048, 0.0f, "GraphicsSetOption", "'ShadowResolution' takes the name of a choice (Auto, AutoFine, 1024, 2048, 4096, 8192)");
		refused(c_SetInt, "ShadowResolution", {}, 2, 0.0f, "GraphicsSetOption", "'ShadowResolution' takes the name of a choice");
		refused(c_SetChoice, "ShadowResolution", "3000", 0, 0.0f, "GraphicsSetOption", "ShadowResolution: 'ShadowResolution' must be one of");
		refused(c_SetFloat, "ShadowCascades", {}, 0, 2.0f, "GraphicsSetOption", "'ShadowCascades' takes an Int");
		refused(c_SetInt, "ShadowCascades", {}, 9, 0.0f, "GraphicsSetOption", "ShadowCascades: 'ShadowCascades' must be a whole number from 1 to 4");
		refused(c_SetChoice, "Preset", "Epic", 0, 0.0f, "GraphicsSetOption", "Preset: 'Preset' must be one of");
		refused(c_SetChoice, "Resolution", "1080", 0, 0.0f, "GraphicsSetOption", "there is no graphics option 'Resolution'");
		// A preset the SDK does not know is refused before any call.
		probe.Ask(c_SetPreset, "Preset", {}, 7);
		CHECK_FALSE(probe.Get<bool>("Accepted"));
		probe.Ask(c_Reset, "NoSuchOption");
		CHECK_FALSE(probe.Get<bool>("ResetDone"));
		CHECK(CountWarnings(log.GetEntries(), "GraphicsResetOptions", "there is no graphics option 'NoSuchOption'") == 1);
		CHECK(game.GetGraphics().GetPlayerOptions() == before);
	}

	TEST_CASE("The info struct is filled up to the smaller size and says so; a struct smaller than the first version is refused")
	{
		GraphicsGame game("ScriptGraphicsInfo");
		ProbeScene probe;
		LogCapture log;
		probe.Ask(c_DescribeRaw, "Preset");
		CHECK(probe.Get<int32_t>("InfoStructSize") == 80);
		CHECK(probe.Get<bool>("SmallInfoRefused"));
		CHECK(CountWarnings(log.GetEntries(), "GraphicsDescribeOption", "the info's StructSize 40 is smaller than its first version's 80 bytes") == 1);
		probe.CheckScriptChecks();
	}

	TEST_CASE("Reset and Save reach the service; Save without a save directory fails and is reported")
	{
		{
			GraphicsGame game("ScriptGraphicsSave");
			ProbeScene probe;
			probe.Ask(c_SetChoice, "Preset", "Ultra");
			probe.Ask(c_Save, "Preset");
			CHECK(probe.Get<bool>("Saved"));
			REQUIRE(game.GetFile());
			CHECK(FileSystem::Exists(*game.GetFile()));
			nlohmann::json saved;
			REQUIRE(GraphicsOptionsFile::Read(*FileSystem::ReadText(*game.GetFile()), game.GetSection(), game.GetSection().GetDefault(), saved));
			CHECK(saved == nlohmann::json { { "Preset", "Ultra" } });
			probe.Ask(c_Reset, "");
			CHECK(probe.Get<bool>("ResetDone"));
			CHECK(game.GetGraphics().GetPlayerOptions().empty());
			probe.Ask(0, "Preset");
			CHECK(probe.Get<int32_t>("Source") == StrataScriptGraphicsOptionSource_Project);
		}
		GraphicsGame nowhere("ScriptGraphicsNoSaves", false);
		ProbeScene probe;
		LogCapture log;
		probe.Ask(c_Save, "Preset");
		CHECK_FALSE(probe.Get<bool>("Saved"));
		CHECK(CountWarnings(log.GetEntries(), "GraphicsSaveOptions", "the game has no save directory") == 1);
	}

	TEST_CASE("Without a graphics service every function fails and the count is 0")
	{
		REQUIRE(EngineServices::Get<IGameGraphics>() == nullptr);
		ProbeScene probe;
		LogCapture log;
		probe.Ask(c_List, "Preset");
		CHECK(probe.Get<int32_t>("Count") == 0);
		CHECK(probe.Get<std::string>("Names").empty());
		CHECK(probe.Get<std::string>("Effective").empty());
		CHECK(probe.Get<int32_t>("Source") == -1);
		probe.Ask(c_SetChoice, "Preset", "Low");
		CHECK_FALSE(probe.Get<bool>("Accepted"));
		probe.Ask(c_Reset, "");
		CHECK_FALSE(probe.Get<bool>("ResetDone"));
		probe.Ask(c_Save, "Preset");
		CHECK_FALSE(probe.Get<bool>("Saved"));
		for (const char* function : { "GraphicsGetOptionCount", "GraphicsGetOption", "GraphicsSetOption", "GraphicsResetOptions", "GraphicsSaveOptions" })
		{
			CAPTURE(function);
			CHECK(CountWarnings(log.GetEntries(), function, "the program running the game has no graphics options") == 1);
		}
	}
}
