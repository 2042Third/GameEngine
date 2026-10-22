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
#include <Strata/Scene/PhysicsLayerSettings.h>
#include <Strata/Scene/ProjectCollisionLayers.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

		// A layer given by index (0 to c_PhysicsLayerCount - 1) or by name (its name, or "Layer <index>" without one).
		std::optional<uint32_t> ReadLayer(const nlohmann::json& value, const PhysicsLayerSettings& layers)
		{
			if (value.is_number_unsigned() || (value.is_number_integer() && value.get<int64_t>() >= 0))
			{
				const uint64_t index = value.get<uint64_t>();
				return index < c_PhysicsLayerCount ? std::optional<uint32_t>(static_cast<uint32_t>(index)) : std::nullopt;
			}
			if (value.is_string())
				return layers.FindLayer(value.get<std::string>());
			return std::nullopt;
		}

		// Reads project.setCollisionLayers' "layerNames" ({"<index>": name}; "" or null clears a name) into `layers`. The names
		// are checked once all of them are in place, so layers can swap names in one call. Returns an error message, empty on
		// success.
		std::string ReadLayerNames(const nlohmann::json& value, PhysicsLayerSettings& layers)
		{
			if (!value.is_object())
				return R"(Parameter 'layerNames' must be an object of layer indices to names, e.g. {"3": "Player"})";
			std::vector<std::pair<uint32_t, std::string>> names;
			for (const auto& [key, name] : value.items())
			{
				// Indices without leading zeros, so that one layer cannot be named twice ("3" and "03").
				const bool isIndex = !key.empty() && key.size() <= 2 && (key.size() == 1 || key[0] != '0')
					&& std::all_of(key.begin(), key.end(), [](char character) { return character >= '0' && character <= '9'; });
				uint32_t layer = c_PhysicsLayerCount;
				if (isIndex)
				{
					layer = 0;
					for (const char character : key)
						layer = layer * 10 + static_cast<uint32_t>(character - '0');
				}
				if (layer >= c_PhysicsLayerCount)
					return fmt::format("Parameter 'layerNames': '{}' is no layer index (0 to {})", key, c_PhysicsLayerCount - 1);
				if (!name.is_string() && !name.is_null())
					return fmt::format("Parameter 'layerNames.{}' must be a string (empty or null clears the name)", key);
				names.emplace_back(layer, name.is_string() ? name.get<std::string>() : std::string());
				layers.Names[layer].clear();
			}
			for (const auto& [layer, name] : names)
			{
				if (std::optional<std::string> error = layers.ValidateName(layer, name))
					return fmt::format("Parameter 'layerNames.{}': {}", layer, *error);
				layers.Names[layer] = name;
			}
			return {};
		}

		// Reads project.setCollisionLayers' "layerCollisions" ([{"layers": [a, b], "collide": bool}], layers by index or name,
		// applied in order) into `layers`. Returns an error message, empty on success.
		std::string ReadLayerCollisions(const nlohmann::json& value, PhysicsLayerSettings& layers)
		{
			constexpr std::string_view c_Form = R"({"layers": [a, b], "collide": true | false})";
			if (!value.is_array())
				return fmt::format("Parameter 'layerCollisions' must be an array of {}", c_Form);
			for (const nlohmann::json& entry : value)
			{
				if (!entry.is_object())
					return fmt::format("Parameter 'layerCollisions': {} is not of the form {}", entry.dump(), c_Form);
				for (const auto& [key, unused] : entry.items())
				{
					if (key != "layers" && key != "collide")
						return fmt::format("Parameter 'layerCollisions': unknown key '{}' (entries are {})", key, c_Form);
				}
				const auto pair = entry.find("layers");
				const auto collide = entry.find("collide");
				if (pair == entry.end() || !pair->is_array() || pair->size() != 2 || collide == entry.end() || !collide->is_boolean())
					return fmt::format("Parameter 'layerCollisions': {} is not of the form {}", entry.dump(), c_Form);
				std::optional<uint32_t> found[2];
				for (size_t side = 0; side < 2; side++)
				{
					found[side] = ReadLayer((*pair)[side], layers);
					if (!found[side])
					{
						return fmt::format("Parameter 'layerCollisions': no layer {} (layers are indices from 0 to {} or their names)", (*pair)[side].dump(),
							c_PhysicsLayerCount - 1);
					}
				}
				layers.SetCollide(*found[0], *found[1], collide->get<bool>());
			}
			return {};
		}

		// The project's collision layers as its settings hold them (the section's default when it is not set or its stored value
		// is refused).
		PhysicsLayerSettings GetProjectLayers(const Project& project)
		{
			const nlohmann::json* value = project.GetSettings().FindSection(ProjectCollisionLayers::c_SectionId);
			return value ? PhysicsLayerSettings::FromJson(*value) : PhysicsLayerSettings();
		}

	}

	nlohmann::json CommandUtils::DescribePhysicsLayers(const PhysicsLayerSettings& layers)
	{
		nlohmann::json names = nlohmann::json::object();
		for (uint32_t layer = 0; layer < c_PhysicsLayerCount; layer++)
		{
			if (!layers.Names[layer].empty())
				names[std::to_string(layer)] = layers.Names[layer];
		}
		nlohmann::json noCollisions = nlohmann::json::array();
		for (uint32_t a = 0; a < c_PhysicsLayerCount; a++)
		{
			for (uint32_t b = a; b < c_PhysicsLayerCount; b++)
			{
				if (!layers.DoCollide(a, b))
					noCollisions.push_back({ layers.GetDisplayName(a), layers.GetDisplayName(b) });
			}
		}
		return { { "names", std::move(names) }, { "noCollisions", std::move(noCollisions) } };
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

		const nlohmann::json layerNamesSchema = {
			{ "type", "object" },
			{ "description", R"(Names of collision layers by index ("0" to "31"), e.g. {"1": "Player", "2": "Debris"}; "" or null clears )"
				"a name. Names are unique (ignoring case), at most 64 characters, and RigidBody.Layer, CharacterController.Layer, scripts' "
				"Physics::GetLayerIndex and layerCollisions accept them" },
			{ "propertyNames", { { "pattern", "^([0-9]|[1-2][0-9]|3[0-1])$" } } },
			{ "additionalProperties", { { "type", nlohmann::json::array({ "string", "null" }) } } } };
		const nlohmann::json layerCollisionsSchema = {
			{ "type", "array" },
			{ "description", "Changes of the layer matrix, applied in order after layerNames: whether two layers (indices 0-31 or names) collide. "
				"Every pair collides by default; bodies and character controllers collide only when their layers do" },
			{ "items", {
				{ "type", "object" },
				{ "properties", {
					{ "layers", { { "type", "array" }, { "minItems", 2 }, { "maxItems", 2 }, { "description", "The two layers (the same twice: a layer with itself)" },
						{ "items", { { "type", nlohmann::json::array({ "integer", "string" }) } } } } },
					{ "collide", BoolSchema("Whether they collide") } } },
				{ "required", nlohmann::json::array({ "layers", "collide" }) },
				{ "additionalProperties", false } } } };
		registry.Register({ "project.setCollisionLayers",
			"Names the project's collision layers and sets which of them collide, as one undo step that leaves the scene saved, and writes the "
			"project file in the background (project.save waits for it). The layers are the project's: every scene and prefab uses the same 32 "
			"layers, names and matrix (scene.info's settings.physicsLayers shows them as the active scene runs them). layerNames apply first, "
			"then resetLayerCollisions, then layerCollisions in order; an invalid request changes nothing. Allowed while playing: the running "
			"game takes the new table at once (replacing what its scripts changed with Physics::SetLayersCollide) and the change stays after "
			"play.stop. Returns physicsLayers ({names, noCollisions}, pairs by name).",
			ObjectSchema({
				{ "layerNames", layerNamesSchema },
				{ "resetLayerCollisions", BoolSchema("Lets every pair of layers collide again before layerCollisions applies") },
				{ "layerCollisions", layerCollisionsSchema } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const bool resetCollisions = arguments.GetBool("resetLayerCollisions", false);
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!context.HasProject())
					return EditorCommandResult::Fail("No project is open: collision layers belong to a project (project.open or project.create first)");
				const SettingsRegistry& settings = context.GetSettingsRegistry();
				if (!settings.Find(ProjectCollisionLayers::c_SectionId))
				{
					return EditorCommandResult::Fail(fmt::format("This editor has no '{}' settings section (the Scene module registers it)",
						ProjectCollisionLayers::c_SectionId));
				}
				const Ref<Project>& project = context.GetProject();
				PhysicsLayerSettings layers = GetProjectLayers(*project);
				if (auto names = parameters.find("layerNames"); names != parameters.end())
				{
					if (std::string error = ReadLayerNames(*names, layers); !error.empty())
						return EditorCommandResult::InvalidParameters(error);
				}
				if (resetCollisions)
					layers.ResetMatrix();
				if (auto collisions = parameters.find("layerCollisions"); collisions != parameters.end())
				{
					if (std::string error = ReadLayerCollisions(*collisions, layers); !error.empty())
						return EditorCommandResult::InvalidParameters(error);
				}

				// The default table is no section: a project without layers stores none. Otherwise the change is checked on a copy
				// (its section's validation, the project file's bound: ChangeProjectSection) before anything changes.
				ProjectSettingsValues values = project->GetSettings();
				std::string error;
				if (layers.IsDefault())
					values.Reset(ProjectCollisionLayers::c_SectionId);
				else if (!values.Set(ProjectCollisionLayers::c_SectionId, layers.ToJson(), settings, &error))
					return EditorCommandResult::Fail(error);
				if (!ChangeProjectSection(context, "Set Collision Layers", ProjectCollisionLayers::c_SectionId,
					values.GetStoredEntry(ProjectCollisionLayers::c_SectionId), &error))
				{
					return EditorCommandResult::Fail(error);
				}
				return EditorCommandResult::Ok({ { "physicsLayers", DescribePhysicsLayers(GetProjectLayers(*project)) } });
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
