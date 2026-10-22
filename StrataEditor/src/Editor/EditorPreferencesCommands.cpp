#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorPreferences.h"
#include "Editor/SettingsFileWriter.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/SettingsRegistry.h>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <utility>

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		// The registered user sections' ids, for messages.
		std::string ListPreferenceSections(const SettingsRegistry& registry)
		{
			std::string ids;
			for (const SettingsSection& section : registry.GetAll())
			{
				if (section.Scope == SettingsScope::User)
					ids += fmt::format("{}{}", ids.empty() ? "" : ", ", section.Id);
			}
			return ids.empty() ? std::string("none") : ids;
		}

		// The user section a command names, or the InvalidParameters failure that names the known ones.
		const SettingsSection* FindPreferenceSection(const SettingsRegistry& registry, const std::string& id, std::optional<EditorCommandResult>& outFailure)
		{
			const SettingsSection* section = registry.Find(id);
			if (!section)
			{
				outFailure = EditorCommandResult::InvalidParameters(fmt::format("Parameter 'section': there is no preferences section '{}' (sections: {})", id,
					ListPreferenceSections(registry)));
				return nullptr;
			}
			if (section->Scope != SettingsScope::User)
			{
				outFailure = EditorCommandResult::InvalidParameters(fmt::format("Parameter 'section': '{}' is a project setting (project.setSettings), not a "
					"preference", id));
				return nullptr;
			}
			return section;
		}

		// A section as preferences.get reports it: what the registry says of it and what the preferences hold.
		nlohmann::json DescribeSection(const SettingsSection& section, const EditorPreferences& preferences)
		{
			const nlohmann::json* value = preferences.FindSection(section.Id);
			const std::optional<std::string> problem = preferences.FindProblem(section.Id);
			return {
				{ "id", section.Id },
				{ "title", section.Title },
				{ "category", section.Category },
				{ "description", section.Description },
				{ "version", section.Version },
				{ "schema", section.GetValueSchema() },
				{ "value", value ? *value : section.GetDefault() },
				{ "isDefault", value == nullptr },
				{ "problem", problem ? nlohmann::json(*problem) : nlohmann::json(nullptr) } };
		}

	}

	void RegisterPreferencesCommands(EditorCommandRegistry& registry)
	{
		registry.Register({ "preferences.get",
			"The person's preferences, which hold for every project and are kept in their own preferences file (not the project's): every "
			"preference section the editor and the engine's modules register (sections: id, title, category, description, version, schema: the "
			"JSON Schema of its value, value: what applies, isDefault: whether it is left at its default, problem: why a stored value is refused, "
			"null when none), in registration order; unclaimed lists sections the file stores that this editor does not know (kept as stored); "
			"file (null while the preferences are kept in memory only), fileState (written, writing or unwritten: its last write failed, "
			"preferences.save) and fileError; readOnly says why the file is left as it is (a later editor wrote it: this editor uses the "
			"defaults and changes nothing), null otherwise. section narrows sections to one. Works without a project. Set one with "
			"preferences.set.",
			ObjectSchema({ { "section", StringSchema("A section's id (\"<Module>.<Name>\", e.g. \"Editor.ExternalEditor\") to describe alone") } }),
			[](EditorContext& context, const nlohmann::json& parameters) -> EditorCommandResult
			{
				CommandArguments arguments(parameters);
				const std::string only = arguments.GetString("section", "");
				if (!arguments.IsValid())
					return arguments.Fail();
				const EditorPreferences& preferences = context.GetPreferences();
				const SettingsRegistry& settings = preferences.GetRegistry();
				nlohmann::json sections = nlohmann::json::array();
				if (!only.empty())
				{
					std::optional<EditorCommandResult> failure;
					const SettingsSection* section = FindPreferenceSection(settings, only, failure);
					if (!section)
						return std::move(*failure);
					sections.push_back(DescribeSection(*section, preferences));
				}
				else
				{
					for (const SettingsSection& section : settings.GetAll())
					{
						if (section.Scope == SettingsScope::User)
							sections.push_back(DescribeSection(section, preferences));
					}
				}
				return EditorCommandResult::Ok({
					{ "sections", std::move(sections) },
					{ "unclaimed", preferences.GetUnclaimedSections() },
					{ "file", preferences.GetFile().empty() ? nlohmann::json(nullptr) : nlohmann::json(FileSystem::ToUTF8(preferences.GetFile())) },
					{ "fileState", SettingsFileStateToString(preferences.GetFileState()) },
					{ "fileError", preferences.HasUnwrittenPreferences() ? nlohmann::json(preferences.GetFileError()) : nlohmann::json(nullptr) },
					{ "readOnly", preferences.GetReadOnlyReason().empty() ? nlohmann::json(nullptr) : nlohmann::json(preferences.GetReadOnlyReason()) } });
			}, EditorCommandKind::Query, EditorCommandResults::Session });

		registry.Register({ "preferences.set",
			"Sets one of the person's preference sections (preferences.get lists them with their schemas) to a value, or with reset: true back to "
			"its default. Preferences are the person's own and hold for every project: the change is not an undo step and leaves the project and "
			"the scene alone; the preferences file is written in the background (preferences.save waits for it). The value replaces the "
			"section's whole value and is checked by the section first: an invalid one changes nothing and fails naming what is wrong; it also "
			"fails while the file is left as it is (preferences.get's readOnly). Works without a project and while playing. Returns section, "
			"value (what applies now) and isDefault.",
			ObjectSchema({
				{ "section", StringSchema("The section's id, \"<Module>.<Name>\"") },
				{ "value", { { "description", "The section's new value, matching its schema (preferences.get)" } } },
				{ "reset", BoolSchema("Back to the section's default instead of a value (default false)") } }, { "section" }),
			[](EditorContext& context, const nlohmann::json& parameters) -> EditorCommandResult
			{
				CommandArguments arguments(parameters);
				const std::string id = arguments.GetString("section");
				const bool reset = arguments.GetBool("reset", false);
				const nlohmann::json* value = JsonUtils::Find(parameters, "value");
				if (arguments.IsValid() && reset == (value != nullptr))
					arguments.SetError("Give either 'value' or 'reset': true");
				if (!arguments.IsValid())
					return arguments.Fail();
				EditorPreferences& preferences = context.GetPreferences();
				std::optional<EditorCommandResult> failure;
				const SettingsSection* section = FindPreferenceSection(preferences.GetRegistry(), id, failure);
				if (!section)
					return std::move(*failure);
				if (!preferences.GetReadOnlyReason().empty())
					return EditorCommandResult::Fail(fmt::format("Cannot change '{}': {}", id, preferences.GetReadOnlyReason()));
				std::string error;
				if (reset)
					preferences.Reset(id);
				else if (!preferences.Set(id, *value, &error))
					return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'value': {}", error));
				const nlohmann::json* applied = preferences.FindSection(id);
				return EditorCommandResult::Ok({ { "section", id }, { "value", applied ? *applied : section->GetDefault() }, { "isDefault", applied == nullptr } });
			}, EditorCommandKind::Session });

		registry.Register(MayDefer({ "preferences.save",
			"Writes the preferences file now and answers once it is written: preferences are written in the background after every change "
			"anyway, so this is for after a write failed (preferences.get's fileState \"unwritten\", fileError says why) once its cause is fixed, "
			"and for waiting until the file holds the latest change. Fails with the reason when the write fails, or when the file is left as it "
			"is (preferences.get's readOnly). Returns file (null while the preferences are kept in memory only: nothing is written then).",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				EditorPreferences& preferences = context.GetPreferences();
				if (preferences.GetFile().empty())
					return EditorCommandResult::Ok({ { "file", nullptr } });
				if (!preferences.GetReadOnlyReason().empty())
					return EditorCommandResult::Fail(fmt::format("Nothing is written: {}", preferences.GetReadOnlyReason()));
				preferences.Save();
				return EditorCommandResult::Defer([](EditorContext& context) -> std::optional<EditorCommandResult>
				{
					// Writes coalesce: once none is in flight, the last one held this command's state or a newer one.
					const EditorPreferences& preferences = context.GetPreferences();
					if (preferences.IsWriting())
						return std::nullopt;
					const std::string file = FileSystem::ToUTF8(preferences.GetFile());
					if (preferences.HasUnwrittenPreferences())
						return EditorCommandResult::Fail(fmt::format("Writing '{}' failed: {}", file, preferences.GetFileError()));
					return EditorCommandResult::Ok({ { "file", file } });
				});
			}, EditorCommandKind::Session }));
	}

}
