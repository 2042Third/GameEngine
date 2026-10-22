#pragma once

#include <doctest/doctest.h>

#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Process.h"
#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Core/UUID.h"
#include "TestHelpers.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <regex>
#include <string>
#include <vector>

// Settings sections of tests, registered in local registries (the process registry is frozen once the modules register).
namespace Strata::Tests
{

	// "Test.Simple", a simple project section with a field of every type: Speed (Float 0 to 10, 1), Count (Int 1 to 8, 4),
	// Enabled (Bool, true), Label (String of at most 16 bytes, "Hello"), Mode (Enum Fast or Slow, "Fast") and Icon (a Texture,
	// none).
	inline SettingsSection MakeSimpleTestSection()
	{
		SettingsSection section;
		section.Id = "Test.Simple";
		section.Title = "Simple";
		section.Category = "Tests";
		section.Description = "A section of every field type, for tests.";

		auto addField = [&section](std::string name, SettingsFieldType type, nlohmann::json value) -> SettingsField&
		{
			SettingsField& field = section.Fields.emplace_back();
			field.Name = std::move(name);
			field.Title = field.Name;
			field.Description = "The " + field.Name + " of tests.";
			field.Type = type;
			field.Default = std::move(value);
			return field;
		};
		SettingsField& speed = addField("Speed", SettingsFieldType::Float, 1.0);
		speed.Min = 0.0;
		speed.Max = 10.0;
		SettingsField& count = addField("Count", SettingsFieldType::Int, 4);
		count.Min = 1.0;
		count.Max = 8.0;
		addField("Enabled", SettingsFieldType::Bool, true);
		addField("Label", SettingsFieldType::String, "Hello").MaxLength = 16;
		addField("Mode", SettingsFieldType::Enum, "Fast").Options = { "Fast", "Slow" };
		addField("Icon", SettingsFieldType::Asset, nullptr).AssetType = "Texture";
		return section;
	}

	// "Test.Complex", a complex project section of version 2: { "Names": [at most 4 strings], "Targets": [asset handles] },
	// whose References are the targets. Version 1 stored one name ({ "Name": "..." }), which Read upgrades.
	inline SettingsSection MakeComplexTestSection()
	{
		SettingsSection section;
		section.Id = "Test.Complex";
		section.Title = "Complex";
		section.Category = "Tests";
		section.Description = "A section described by its own schema, for tests.";
		section.Version = 2;
		section.Schema = {
			{ "type", "object" },
			{ "properties", { { "Names", { { "type", "array" }, { "items", { { "type", "string" } } }, { "maxItems", 4 } } },
				{ "Targets", { { "type", "array" }, { "items", { { "type", "string" } } } } } } },
			{ "additionalProperties", false } };
		section.MakeDefault = []() { return nlohmann::json { { "Names", nlohmann::json::array() }, { "Targets", nlohmann::json::array() } }; };
		section.Read = [](const nlohmann::json& value, uint32_t version, std::string* error) -> std::optional<nlohmann::json>
		{
			auto fail = [error](std::string message) -> std::optional<nlohmann::json>
			{
				if (error)
					*error = std::move(message);
				return std::nullopt;
			};
			if (!value.is_object())
				return fail("the value must be an object");
			nlohmann::json result = { { "Names", nlohmann::json::array() }, { "Targets", nlohmann::json::array() } };
			if (version == 1)
			{
				const nlohmann::json* name = JsonUtils::Find(value, "Name");
				if (!name || !name->is_string())
					return fail("version 1 holds a \"Name\" string");
				result["Names"].push_back(*name);
				return result;
			}
			const nlohmann::json* names = JsonUtils::Find(value, "Names");
			if (!names || !names->is_array() || names->size() > 4)
				return fail("\"Names\" must be an array of at most 4 strings");
			for (const nlohmann::json& name : *names)
			{
				if (!name.is_string())
					return fail("\"Names\" must be an array of at most 4 strings");
				result["Names"].push_back(name);
			}
			if (const nlohmann::json* targets = JsonUtils::Find(value, "Targets"))
			{
				if (!targets->is_array())
					return fail("\"Targets\" must be an array of asset handles");
				for (const nlohmann::json& target : *targets)
				{
					if (!target.is_string() || !UUID::FromString(target.get<std::string>()))
						return fail("\"Targets\" must be an array of asset handles");
					result["Targets"].push_back(target);
				}
			}
			return result;
		};
		section.References = [](const nlohmann::json& value)
		{
			std::vector<UUID> handles;
			if (const nlohmann::json* targets = JsonUtils::Find(value, "Targets"); targets && targets->is_array())
			{
				for (const nlohmann::json& target : *targets)
				{
					if (std::optional<UUID> handle = target.is_string() ? UUID::FromString(target.get<std::string>()) : std::nullopt)
						handles.push_back(*handle);
				}
			}
			return handles;
		};
		return section;
	}

	// "Game.Tuning", the section a game registers through ModuleRegistrationOptions::Extra in the child processes of
	// --strata-test-helper=settings-section (RunInSettingsSectionProcess): Difficulty (Enum Easy, Normal or Hard, "Normal"),
	// Lives (Int 1 to 9, 3) and Banner (a Texture, none).
	inline constexpr const char* c_GameTestSection = "Game.Tuning";

	inline SettingsSection MakeGameTestSection()
	{
		SettingsSection section;
		section.Id = c_GameTestSection;
		section.Title = "Tuning";
		section.Category = "Game";
		section.Description = "How hard the test game is, and the picture its menu shows.";
		SettingsField& difficulty = section.Fields.emplace_back();
		difficulty.Name = "Difficulty";
		difficulty.Title = "Difficulty";
		difficulty.Description = "How hard the game starts.";
		difficulty.Type = SettingsFieldType::Enum;
		difficulty.Options = { "Easy", "Normal", "Hard" };
		difficulty.Default = "Normal";
		SettingsField& lives = section.Fields.emplace_back();
		lives.Name = "Lives";
		lives.Title = "Lives";
		lives.Description = "Lives a player starts with.";
		lives.Type = SettingsFieldType::Int;
		lives.Min = 1.0;
		lives.Max = 9.0;
		lives.Default = 3;
		SettingsField& banner = section.Fields.emplace_back();
		banner.Name = "Banner";
		banner.Title = "Banner";
		banner.Description = "The picture the menu shows.";
		banner.Type = SettingsFieldType::Asset;
		banner.AssetType = "Texture";
		banner.Default = nullptr;
		return section;
	}

	// Whether this process is a child of --strata-test-helper=settings-section: its process registry has the game's section.
	inline bool IsSettingsSectionProcess()
	{
		return SettingsRegistry::GetProcessRegistry().Find(c_GameTestSection) != nullptr;
	}

	// For test cases about a section a game registers, which only a process registry can have: runs the current test case in
	// a child process whose registry has the game's section (Engine/ModuleRegistrationHelpers.cpp) and checks that it passed
	// there. A case calls it first when !IsSettingsSectionProcess() and returns; in the child it goes on.
	inline void RunInSettingsSectionProcess()
	{
		// doctest's filters split at commas: '?' matches any one character, a comma too.
		std::string name = doctest::getContextOptions()->currentTest->m_name;
		std::replace(name.begin(), name.end(), ',', '?');
		ProcessSpecification specification;
		specification.Executable = GetTestExecutablePath();
		specification.Arguments = { "--strata-test-helper=settings-section", "--test-case=" + name };
		Process::RunResult result = Process::Run(specification, std::chrono::minutes(5));
		std::erase(result.Output, '\r');
		INFO(result.Output);
		CHECK_FALSE(result.TimedOut);
		CHECK(result.ExitCode == 0);
		// The case ran there, alone (doctest pads its counts).
		CHECK(std::regex_search(result.Output, std::regex(R"(test cases:\s+1 \|\s+1 passed)")));
	}

	// Registers both test sections in `registry`; REQUIREs that it accepts them.
	inline void RegisterTestSections(SettingsRegistry& registry)
	{
		REQUIRE(registry.Register(MakeSimpleTestSection()));
		REQUIRE(registry.Register(MakeComplexTestSection()));
	}

}
