#include <doctest/doctest.h>

#include "Core/SettingsTestUtils.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SettingsRegistry.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	// The error a section's validation reports for `value` (REQUIREs that it refuses it).
	std::string GetValidationError(const SettingsSection& section, const nlohmann::json& value, uint32_t version = 1)
	{
		std::string error;
		REQUIRE_FALSE(section.Validate(value, version, &error));
		REQUIRE_FALSE(error.empty());
		return error;
	}

}

TEST_SUITE("Core.Settings")
{
	TEST_CASE("A simple section validates its fields and reports the first bad one")
	{
		const SettingsSection section = Tests::MakeSimpleTestSection();
		std::string error;

		// Missing fields take their defaults.
		std::optional<nlohmann::json> defaults = section.Validate(nlohmann::json::object(), 1, &error);
		REQUIRE_MESSAGE(defaults, error);
		CHECK(*defaults == section.GetDefault());
		CHECK((*defaults)["Speed"] == 1.0);
		CHECK((*defaults)["Count"] == 4);
		CHECK((*defaults)["Enabled"] == true);
		CHECK((*defaults)["Label"] == "Hello");
		CHECK((*defaults)["Mode"] == "Fast");
		CHECK((*defaults)["Icon"].is_null());

		// Values are kept in their stored form: Floats as numbers with a fraction, handles in their canonical text.
		std::optional<nlohmann::json> value = section.Validate({ { "Speed", 2 }, { "Count", 8 }, { "Label", "" }, { "Mode", "Slow" }, { "Icon", "ab" } }, 1, &error);
		REQUIRE_MESSAGE(value, error);
		CHECK((*value)["Speed"].is_number_float());
		CHECK((*value)["Speed"] == 2.0);
		CHECK((*value)["Count"] == 8);
		CHECK((*value)["Mode"] == "Slow");
		CHECK((*value)["Icon"] == "00000000000000AB");
		CHECK(section.Validate({ { "Icon", "0000000000000000" } }, 1)->at("Icon").is_null());

		// Every type refuses what it does not accept, naming the field and what it takes.
		CHECK(GetValidationError(section, { { "Speed", 10.5 } }) == "'Speed' must be a number from 0 to 10");
		CHECK(GetValidationError(section, { { "Speed", "fast" } }).starts_with("'Speed' must be"));
		CHECK(GetValidationError(section, { { "Count", 9 } }) == "'Count' must be a whole number from 1 to 8");
		CHECK(GetValidationError(section, { { "Count", 2.5 } }).starts_with("'Count' must be"));
		CHECK(GetValidationError(section, { { "Enabled", 1 } }) == "'Enabled' must be true or false");
		CHECK(GetValidationError(section, { { "Label", "Seventeen bytes!!" } }) == "'Label' must be a string of at most 16 bytes");
		CHECK(GetValidationError(section, { { "Mode", "Medium" } }) == "'Mode' must be one of 'Fast', 'Slow'");
		CHECK(GetValidationError(section, { { "Icon", "not a handle" } }).starts_with("'Icon' must be the handle of a Texture asset"));
		// The first bad field in the order of the section's fields, whatever the value's order.
		CHECK(GetValidationError(section, { { "Mode", "Medium" }, { "Count", 0 }, { "Speed", -1 } }).starts_with("'Speed' must be"));
		CHECK(GetValidationError(section, { { "Mode", "Medium" }, { "Count", 0 } }).starts_with("'Count' must be"));
		// Whole numbers are compared exactly, also next to the widest range an Int may have (+-2^53), where a double would round
		// the next whole number onto the bound.
		SettingsSection wide = Tests::MakeSimpleTestSection();
		wide.Fields[1].Min = -9007199254740992.0;
		wide.Fields[1].Max = 9007199254740992.0;
		CHECK(wide.Validate({ { "Count", int64_t { 9007199254740992 } } }, 1));
		CHECK(wide.Validate({ { "Count", int64_t { -9007199254740992 } } }, 1));
		CHECK_FALSE(wide.Validate({ { "Count", int64_t { 9007199254740993 } } }, 1));
		CHECK_FALSE(wide.Validate({ { "Count", uint64_t { 9007199254740993 } } }, 1));
		CHECK_FALSE(wide.Validate({ { "Count", int64_t { -9007199254740993 } } }, 1));
		CHECK_FALSE(wide.Validate({ { "Count", std::numeric_limits<uint64_t>::max() } }, 1));

		// A member that names no field, a value that is not an object, a newer version and an oversized value are refused.
		CHECK(GetValidationError(section, { { "Sped", 2.0 } }).starts_with("'Sped' is no field of the section"));
		CHECK(GetValidationError(section, nlohmann::json::array()) == "the value must be an object of the section's fields");
		CHECK(GetValidationError(section, nlohmann::json::object(), 2).find("newer") != std::string::npos);
		CHECK(GetValidationError(section, nlohmann::json::object(), 0).find("version 0") != std::string::npos);
		CHECK(GetValidationError(section, { { "Label", std::string(SettingsSection::c_MaxValueBytes, 'x') } }).find("512 KiB") != std::string::npos);

		// The schema describes every field for agents.
		const nlohmann::json schema = section.GetValueSchema();
		CHECK(schema["type"] == "object");
		CHECK(schema["additionalProperties"] == false);
		CHECK(schema["properties"].size() == section.Fields.size());
		CHECK(schema["properties"]["Count"]["type"] == "integer");
		CHECK(schema["properties"]["Count"]["minimum"] == 1);
		CHECK(schema["properties"]["Count"]["maximum"] == 8);
		CHECK(schema["properties"]["Mode"]["enum"] == nlohmann::json::array({ "Fast", "Slow" }));
		CHECK(schema["properties"]["Label"]["maxLength"] == 16);
		CHECK(schema["properties"]["Speed"]["description"] == "The Speed of tests.");

		// A section whose default its own fields refuse is not registered.
		SettingsRegistry registry;
		SettingsSection badDefault = Tests::MakeSimpleTestSection();
		badDefault.Fields[1].Default = 20;
		CHECK_FALSE(registry.Register(badDefault));
		SettingsSection twice = Tests::MakeSimpleTestSection();
		twice.Fields[1].Name = "Speed";
		CHECK_FALSE(registry.Register(twice));
		CHECK(registry.GetAll().empty());

		// Registered defaults are in the stored form too: a Float default given as a whole number, an Asset default in short.
		SettingsSection shortDefaults = Tests::MakeSimpleTestSection();
		shortDefaults.Fields[0].Default = 2;
		shortDefaults.Fields[5].Default = "ab";
		REQUIRE(registry.Register(shortDefaults));
		const SettingsSection& registered = *registry.Find("Test.Simple");
		CHECK(registered.GetDefault()["Speed"].is_number_float());
		CHECK(registered.GetDefault()["Icon"] == "00000000000000AB");
		CHECK(registered.Validate(nlohmann::json::object(), 1) == registered.Validate(registered.GetDefault(), 1));
		CHECK(registered.Validate(nlohmann::json::object(), 1)->at("Speed").is_number_float());
		CHECK(registered.GetValueSchema()["properties"]["Icon"]["default"] == "00000000000000AB");
	}

	TEST_CASE("A section's Read upgrades an older version")
	{
		SettingsRegistry registry;
		Tests::RegisterTestSections(registry);
		const SettingsSection& section = *registry.Find("Test.Complex");
		std::string error;

		std::optional<nlohmann::json> upgraded = section.Validate({ { "Name", "Old" } }, 1, &error);
		REQUIRE_MESSAGE(upgraded, error);
		CHECK(*upgraded == nlohmann::json { { "Names", { "Old" } }, { "Targets", nlohmann::json::array() } });
		CHECK(section.Validate({ { "Names", { "A", "B" } } }, 2, &error));
		CHECK(GetValidationError(section, { { "Names", 5 } }, 2) == "\"Names\" must be an array of at most 4 strings");
		CHECK(GetValidationError(section, { { "Names", nlohmann::json::array() } }, 3).find("newer") != std::string::npos);
		CHECK(section.GetDefault() == nlohmann::json { { "Names", nlohmann::json::array() }, { "Targets", nlohmann::json::array() } });
		CHECK(section.GetValueSchema() == section.Schema);

		// Stored values are upgraded when validated: provided at the section's version, and written back so.
		std::optional<ProjectSettingsValues> values = ProjectSettingsValues::FromJson({ { "Test.Complex", { { "Version", 1 }, { "Value", { { "Name", "Old" } } } } } }, &error);
		REQUIRE_MESSAGE(values, error);
		CHECK_FALSE(values->FindSection("Test.Complex"));
		CHECK(values->Validate(registry).empty());
		REQUIRE(values->FindSection("Test.Complex"));
		CHECK(*values->FindSection("Test.Complex") == *upgraded);
		CHECK(values->ToJson() == nlohmann::json { { "Test.Complex", { { "Version", 2 }, { "Value", *upgraded } } } });
	}

	TEST_CASE("Registering after the registry froze fails")
	{
		SettingsRegistry registry;
		CHECK_FALSE(registry.IsFrozen());
		CHECK(registry.Register(Tests::MakeSimpleTestSection()));
		// Ids are unique.
		CHECK_FALSE(registry.Register(Tests::MakeSimpleTestSection()));
		registry.Freeze();
		CHECK(registry.IsFrozen());
		CHECK_FALSE(registry.Register(Tests::MakeComplexTestSection()));
		CHECK_FALSE(registry.Find("Test.Complex"));
		CHECK(registry.GetAll().size() == 1);
		CHECK(registry.Find("Test.Simple"));

		// The process's registry froze with the modules' registration.
		CHECK(SettingsRegistry::GetProcessRegistry().IsFrozen());
		CHECK_FALSE(SettingsRegistry::GetProcessRegistry().Register(Tests::MakeComplexTestSection()));
		CHECK_FALSE(SettingsRegistry::GetProcessRegistry().Find("Test.Complex"));

		// Sections are complete and do not contradict themselves.
		SettingsRegistry open;
		SettingsSection noRead = Tests::MakeComplexTestSection();
		noRead.Read = nullptr;
		CHECK_FALSE(open.Register(noRead));
		SettingsSection fieldsAndRead = Tests::MakeSimpleTestSection();
		fieldsAndRead.Read = Tests::MakeComplexTestSection().Read;
		CHECK_FALSE(open.Register(fieldsAndRead));
		SettingsSection refusedDefault = Tests::MakeComplexTestSection();
		refusedDefault.MakeDefault = []() { return nlohmann::json { { "Names", 1 } }; };
		CHECK_FALSE(open.Register(refusedDefault));
		SettingsSection untitled = Tests::MakeSimpleTestSection();
		untitled.Description.clear();
		CHECK_FALSE(open.Register(untitled));
		CHECK(open.GetAll().empty());
	}

	TEST_CASE("Section ids follow <Module>.<Name>")
	{
		for (const char* id : { "Scene.CollisionLayers", "Input.Actions", "Editor.Interface", "Renderer2.Quality3" })
		{
			CAPTURE(id);
			CHECK(SettingsRegistry::IsValidSectionId(id));
		}
		const std::string longId = "Module." + std::string(SettingsRegistry::c_MaxIdSize, 'A');
		for (const std::string& id : { std::string(), std::string("Scene"), std::string("scene.Layers"), std::string("Scene.layers"),
			std::string("Scene.Collision.Layers"), std::string(".Layers"), std::string("Scene."), std::string("Scene.Col-Layers"),
			std::string("Scene.Col Layers"), std::string("Sc\xC3\xA8ne.Layers"), std::string("2D.Layers"), longId })
		{
			CAPTURE(id);
			CHECK_FALSE(SettingsRegistry::IsValidSectionId(id));
			SettingsSection section = Tests::MakeSimpleTestSection();
			section.Id = id;
			SettingsRegistry registry;
			CHECK_FALSE(registry.Register(section));
		}
	}

	TEST_CASE("Asset fields and References report a section's references, each once")
	{
		SettingsRegistry registry;
		Tests::RegisterTestSections(registry);
		const SettingsSection& simple = *registry.Find("Test.Simple");
		const SettingsSection& complex = *registry.Find("Test.Complex");

		CHECK(simple.FindReferences(*simple.Validate({ { "Icon", "00000000000000AB" } }, 1)) == std::vector<UUID> { UUID(0xAB) });
		CHECK(simple.FindReferences(simple.GetDefault()).empty());
		const nlohmann::json targets = { { "Names", nlohmann::json::array() },
			{ "Targets", { "0000000000000002", "0000000000000001", "0000000000000002", "0000000000000000" } } };
		CHECK(complex.FindReferences(targets) == std::vector<UUID> { UUID(2), UUID(1) });

		// A project's references are every validated section's, each once.
		ProjectSettingsValues values;
		std::string error;
		REQUIRE_MESSAGE(values.Set("Test.Complex", targets, registry, &error), error);
		REQUIRE_MESSAGE(values.Set("Test.Simple", { { "Icon", "0000000000000001" } }, registry, &error), error);
		CHECK(values.GetReferences(registry) == std::vector<UUID> { UUID(2), UUID(1) });
		REQUIRE(values.Set("Test.Simple", { { "Icon", "0000000000000003" } }, registry, &error));
		CHECK(values.GetReferences(registry) == std::vector<UUID> { UUID(2), UUID(1), UUID(3) });
		// Sections the registry does not validate name nothing.
		SettingsRegistry other;
		CHECK(values.GetReferences(other).empty());
	}

	TEST_CASE("Project settings values provide validated sections, and every change gets a version of its own")
	{
		SettingsRegistry registry;
		Tests::RegisterTestSections(registry);
		ProjectSettingsValues values;
		CHECK(values.IsEmpty());
		CHECK_FALSE(values.FindSection("Test.Simple"));
		std::string error;

		uint64_t version = values.GetVersion();
		REQUIRE_MESSAGE(values.Set("Test.Simple", { { "Count", 2 } }, registry, &error), error);
		CHECK(values.GetVersion() != version);
		REQUIRE(values.FindSection("Test.Simple"));
		CHECK((*values.FindSection("Test.Simple"))["Count"] == 2);
		CHECK((*values.FindSection("Test.Simple"))["Speed"] == 1.0);

		// A refused change changes nothing.
		version = values.GetVersion();
		CHECK_FALSE(values.Set("Test.Simple", { { "Count", 20 } }, registry, &error));
		CHECK(error == "settings section 'Test.Simple': 'Count' must be a whole number from 1 to 8");
		CHECK_FALSE(values.Set("Test.Unknown", nlohmann::json::object(), registry, &error));
		CHECK(error == "there is no settings section 'Test.Unknown'");
		SettingsRegistry preferences;
		SettingsSection preference = Tests::MakeSimpleTestSection();
		preference.Scope = SettingsScope::User;
		REQUIRE(preferences.Register(preference));
		CHECK_FALSE(values.Set("Test.Simple", nlohmann::json::object(), preferences, &error));
		CHECK(error.find("preference") != std::string::npos);
		CHECK(values.GetVersion() == version);
		CHECK((*values.FindSection("Test.Simple"))["Count"] == 2);

		// A copy is another provider; resetting restores the default.
		ProjectSettingsValues copy = values;
		CHECK(copy.GetVersion() != values.GetVersion());
		CHECK(*copy.FindSection("Test.Simple") == *values.FindSection("Test.Simple"));
		CHECK(copy.Reset("Test.Simple"));
		CHECK_FALSE(copy.Reset("Test.Simple"));
		CHECK_FALSE(copy.FindSection("Test.Simple"));
		CHECK(copy.IsEmpty());
		CHECK(values.IsStored("Test.Simple"));
		CHECK(values.GetStoredSections() == std::vector<std::string> { "Test.Simple" });

		// A stored section of a user's preference is reported and not provided.
		std::optional<ProjectSettingsValues> stored = ProjectSettingsValues::FromJson(values.ToJson(), &error);
		REQUIRE_MESSAGE(stored, error);
		const std::vector<SettingsProblem> problems = stored->Validate(preferences);
		REQUIRE(problems.size() == 1);
		CHECK(problems[0].Section == "Test.Simple");
		CHECK_FALSE(stored->FindSection("Test.Simple"));

		// The stored form has its shape.
		CHECK_FALSE(ProjectSettingsValues::FromJson(nlohmann::json::array()));
		CHECK_FALSE(ProjectSettingsValues::FromJson({ { "Test.Simple", { { "Value", nlohmann::json::object() } } } }));
		CHECK_FALSE(ProjectSettingsValues::FromJson({ { "Test.Simple", { { "Version", 0 }, { "Value", nlohmann::json::object() } } } }));
		CHECK_FALSE(ProjectSettingsValues::FromJson({ { "Test.Simple", { { "Version", -1 }, { "Value", nlohmann::json::object() } } } }));
		CHECK_FALSE(ProjectSettingsValues::FromJson({ { "Test.Simple", { { "Version", 1 } } } }));
		CHECK_FALSE(ProjectSettingsValues::FromJson({ { "not an id", { { "Version", 1 }, { "Value", nullptr } } } }));
		CHECK(ProjectSettingsValues::FromJson(nlohmann::json::object())->IsEmpty());
	}
}
