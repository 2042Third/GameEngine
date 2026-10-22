#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/ProjectSettingsAction.h"
#include "Editor/SettingsFileWriter.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/ProjectSettings.h>
#include <Strata/Core/SettingsRegistry.h>
#include <Strata/Project/Project.h>
#include <Strata/Reflection/PropertyJson.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		// The registered project sections' ids, for messages.
		std::string ListProjectSections(const SettingsRegistry& registry)
		{
			std::string ids;
			for (const SettingsSection& section : registry.GetAll())
			{
				if (section.Scope == SettingsScope::Project)
					ids += fmt::format("{}{}", ids.empty() ? "" : ", ", section.Id);
			}
			return ids.empty() ? std::string("none") : ids;
		}

		// The project section a command names, or the InvalidParameters failure that names the known ones.
		const SettingsSection* FindProjectSection(const SettingsRegistry& registry, const std::string& id, std::optional<EditorCommandResult>& outFailure)
		{
			const SettingsSection* section = registry.Find(id);
			if (!section)
			{
				outFailure = EditorCommandResult::InvalidParameters(fmt::format("Parameter 'section': there is no project settings section '{}' (sections: {})", id,
					ListProjectSections(registry)));
				return nullptr;
			}
			if (section->Scope != SettingsScope::Project)
			{
				outFailure = EditorCommandResult::InvalidParameters(fmt::format("Parameter 'section': '{}' is a preference of the user, not a project setting",
					id));
				return nullptr;
			}
			return section;
		}

		// A section as project.settings reports it: what the registry says of it and what the project holds.
		nlohmann::json DescribeSection(const SettingsSection& section, const ProjectSettingsValues& values)
		{
			nlohmann::json problem = nullptr;
			const nlohmann::json* value = values.FindSection(section.Id);
			if (!value && values.IsStored(section.Id))
			{
				// Stored but refused (it is kept as stored): why, as when the project opened.
				const nlohmann::json entry = values.GetStoredEntry(section.Id);
				std::string error;
				section.Validate(entry["Value"], entry["Version"].get<uint32_t>(), &error);
				problem = error;
			}
			return {
				{ "id", section.Id },
				{ "title", section.Title },
				{ "category", section.Category },
				{ "description", section.Description },
				{ "version", section.Version },
				{ "schema", section.GetValueSchema() },
				{ "value", value ? *value : section.GetDefault() },
				{ "isDefault", value == nullptr },
				{ "problem", std::move(problem) } };
		}

	}

	void RegisterProjectSettingsCommands(EditorCommandRegistry& registry)
	{
		registry.Register({ "project.settings",
			"The project's settings: its own fields (project: name, startScene, loadingScene, alwaysInclude; set with project.setStartScene and "
			"project.setExportSettings) and every project settings section the engine's modules and the game register (sections: id, title, "
			"category, description, version, schema: the JSON Schema of its value, value: what applies, isDefault: whether the project leaves it "
			"at its default, problem: why a stored value is refused, null when none), in registration order; unclaimed lists sections the file "
			"stores that no module of this editor claims (kept as stored). section narrows sections to one. Set one with project.setSettings.",
			ObjectSchema({ { "section", StringSchema("A section's id (\"<Module>.<Name>\") to describe alone") } }),
			[](EditorContext& context, const nlohmann::json& parameters) -> EditorCommandResult
			{
				CommandArguments arguments(parameters);
				const std::string only = arguments.GetString("section", "");
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!context.HasProject())
					return EditorCommandResult::Fail("No project is open");
				const SettingsRegistry& settings = context.GetSettingsRegistry();
				const Project& project = *context.GetProject();
				nlohmann::json sections = nlohmann::json::array();
				if (!only.empty())
				{
					std::optional<EditorCommandResult> failure;
					const SettingsSection* section = FindProjectSection(settings, only, failure);
					if (!section)
						return std::move(*failure);
					sections.push_back(DescribeSection(*section, project.GetSettings()));
				}
				else
				{
					for (const SettingsSection& section : settings.GetAll())
					{
						if (section.Scope == SettingsScope::Project)
							sections.push_back(DescribeSection(section, project.GetSettings()));
					}
				}
				nlohmann::json unclaimed = nlohmann::json::array();
				for (const std::string& id : project.GetSettings().GetStoredSections())
				{
					if (!settings.Find(id))
						unclaimed.push_back(id);
				}
				const ProjectConfig& config = project.GetConfig();
				return EditorCommandResult::Ok({
					{ "project", {
						{ "name", config.Name },
						{ "startScene", config.StartScene.IsValid() ? UUIDToJson(config.StartScene) : nlohmann::json(nullptr) },
						{ "loadingScene", config.Export.LoadingScene.IsValid() ? UUIDToJson(config.Export.LoadingScene) : nlohmann::json(nullptr) },
						{ "alwaysInclude", config.Export.AlwaysInclude } } },
					{ "sections", std::move(sections) },
					{ "unclaimed", std::move(unclaimed) } });
			}, EditorCommandKind::Query });

		registry.Register({ "project.setSettings",
			"Sets one project settings section (project.settings lists them with their schemas) to a value, or with reset: true back to its "
			"default, as one undo step that leaves the scene saved, and writes the project file in the background (project.save waits for it). "
			"The value replaces the section's whole value (a simple section's fields left out take their defaults) and is checked by the "
			"section first: an invalid one changes nothing and fails naming the first bad field. Allowed while playing: the running game reads "
			"the change at once and it stays after play.stop. Returns section, value (what applies now) and isDefault.",
			ObjectSchema({
				{ "section", StringSchema("The section's id, \"<Module>.<Name>\"") },
				{ "value", { { "description", "The section's new value, matching its schema (project.settings)" } } },
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
				if (!context.HasProject())
					return EditorCommandResult::Fail("No project is open");
				const SettingsRegistry& settings = context.GetSettingsRegistry();
				std::optional<EditorCommandResult> failure;
				const SettingsSection* section = FindProjectSection(settings, id, failure);
				if (!section)
					return std::move(*failure);

				// The change is made on copies first: nothing changes unless all of it is valid, the project file's bound included
				// (ChangeProjectSection).
				const Ref<Project>& project = context.GetProject();
				ProjectSettingsValues values = project->GetSettings();
				std::string error;
				if (reset)
					values.Reset(id);
				else if (!values.Set(id, *value, settings, &error))
					return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'value': {}", error));

				const std::string name = fmt::format("{} {}", reset ? "Reset" : "Set", section->Title);
				if (!ChangeProjectSection(context, name, id, values.GetStoredEntry(id), &error))
					return EditorCommandResult::Fail(error);
				const nlohmann::json* applied = project->GetSettings().FindSection(id);
				return EditorCommandResult::Ok({ { "section", id }, { "value", applied ? *applied : section->GetDefault() }, { "isDefault", applied == nullptr } });
			} });

		registry.Register(MayDefer({ "project.save",
			"Writes the project file now and answers once it is written: the project's settings are written in the background after every change "
			"anyway, so this is for after a write failed (editor.status' project fileState \"unwritten\", fileError says why) once its cause is "
			"fixed, and for waiting until the file holds the latest change. Fails with the reason when the write fails. Returns projectFile.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				if (!context.HasProject())
					return EditorCommandResult::Fail("No project is open");
				context.SaveProjectFile();
				const std::weak_ptr<Project> project = context.GetProject();
				return EditorCommandResult::Defer([project](EditorContext& context) -> std::optional<EditorCommandResult>
				{
					if (project.lock() != context.GetProject())
						return EditorCommandResult::Fail("The project was closed before its file was written", EditorCommandError::Cancelled);
					// Writes coalesce: once none is in flight, the last one held this command's state or a newer one.
					const SettingsFileState state = context.GetProjectFileState();
					if (state == SettingsFileState::Writing)
						return std::nullopt;
					const std::string file = FileSystem::ToUTF8(context.GetProject()->GetProjectFile());
					if (state == SettingsFileState::Unwritten)
						return EditorCommandResult::Fail(fmt::format("Writing '{}' failed: {}", file, context.GetProjectFileError()));
					return EditorCommandResult::Ok({ { "projectFile", file } });
				});
			} }));
	}

}
