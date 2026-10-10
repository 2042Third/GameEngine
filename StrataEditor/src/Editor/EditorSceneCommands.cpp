#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/ProjectTemplates.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/ComponentAccess.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Prefab.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		// Scene settings are not entity data: their undo step stores the settings before and after.
		class SceneSettingsAction final : public EditorAction
		{
		public:
			SceneSettingsAction(Scene& scene, const SceneSettings& before, const SceneSettings& after)
				: m_Scene(scene), m_Before(before), m_After(after)
			{
			}

			const std::string& GetName() const override
			{
				static const std::string s_Name = "Change Scene Settings";
				return s_Name;
			}

			bool Execute(std::string*) override
			{
				m_Scene.GetSettings() = m_After;
				return true;
			}

			void Undo() override
			{
				m_Scene.GetSettings() = m_Before;
			}
		private:
			Scene& m_Scene;
			SceneSettings m_Before;
			SceneSettings m_After;
		};

		nlohmann::json DescribeSettings(const SceneSettings& settings)
		{
			return {
				{ "gravity", { settings.Gravity.x, settings.Gravity.y, settings.Gravity.z } },
				{ "fixedTimestep", settings.FixedTimestep },
				{ "maxFixedStepsPerFrame", settings.MaxFixedStepsPerFrame }
			};
		}

		const ComponentInfo* GetComponentInfo(CommandArguments& arguments, std::string_view name, bool forEditing)
		{
			const std::string componentName = arguments.GetString(name);
			if (!arguments.IsValid())
				return nullptr;
			const ComponentInfo* info = ComponentRegistry::Find(componentName);
			if (!info || HasFlag(info->Flags, ComponentFlags::NoSerialize) || (forEditing && info->IsHidden()))
			{
				arguments.SetError(fmt::format("Unknown component '{}' (see component.list)", componentName));
				return nullptr;
			}
			return info;
		}

		// Ends a mutating command: records the undo step (edit mode) and reports the result. While playing, edits change
		// the running copy only; the result says so, because play.stop discards them.
		EditorCommandResult Finish(EditorContext& context, SceneEditTransaction& transaction, nlohmann::json value = nullptr)
		{
			context.CommitEdit(transaction);
			if (context.IsPlaying())
			{
				if (!value.is_object())
					value = nlohmann::json::object();
				value["warning"] = fmt::format("The scene is running ({}): this change applies to the running copy and is discarded by play.stop",
					SceneStateToString(context.GetSceneState()));
			}
			return EditorCommandResult::Ok(std::move(value));
		}

		EditorCommandResult RollBack(SceneEditTransaction& transaction, std::string error, EditorCommandError kind)
		{
			transaction.Rollback();
			return EditorCommandResult::Fail(std::move(error), kind);
		}

		// The entities of a command that takes "entity" (one) or "entities" (several, each once, in the order given):
		// exactly one of the two.
		std::vector<Entity> GetTargets(CommandArguments& arguments, Scene& scene)
		{
			const bool one = arguments.Has("entity");
			const bool many = arguments.Has("entities");
			if (one == many)
			{
				arguments.SetError(one ? "Pass either 'entity' or 'entities', not both" : "Missing parameter 'entity' (or 'entities')");
				return {};
			}
			if (many)
			{
				std::vector<Entity> entities = arguments.GetEntities(scene, "entities");
				std::unordered_set<UUID> seen;
				std::erase_if(entities, [&seen](const Entity entity) { return !seen.insert(entity.GetUUID()).second; });
				return entities;
			}
			Entity entity = arguments.GetEntity(scene, "entity");
			return arguments.IsValid() ? std::vector<Entity> { entity } : std::vector<Entity>();
		}

		// The entities in their order, without repeats and without those below another listed one (they go with it).
		std::vector<Entity> RemoveNested(const std::vector<Entity>& entities)
		{
			std::unordered_set<UUID> listed;
			for (const Entity entity : entities)
				listed.insert(entity.GetUUID());
			std::unordered_set<UUID> seen;
			std::vector<Entity> result;
			for (const Entity entity : entities)
			{
				if (!seen.insert(entity.GetUUID()).second)
					continue;
				bool nested = false;
				for (Entity ancestor = entity.GetParent(); ancestor && !nested; ancestor = ancestor.GetParent())
					nested = listed.contains(ancestor.GetUUID());
				if (!nested)
					result.push_back(entity);
			}
			return result;
		}

		nlohmann::json ToJson(const std::vector<Entity>& entities)
		{
			nlohmann::json ids = nlohmann::json::array();
			for (const Entity entity : entities)
				ids.push_back(UUIDToJson(entity.GetUUID()));
			return ids;
		}

		std::vector<UUID> GetIds(const std::vector<Entity>& entities)
		{
			std::vector<UUID> ids;
			ids.reserve(entities.size());
			for (const Entity entity : entities)
				ids.push_back(entity.GetUUID());
			return ids;
		}

	}

	void RegisterSceneCommands(EditorCommandRegistry& registry)
	{
		////////////////////////////////////////////////////////////////////////////////
		// Scene
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "scene.info", "Name, asset, modification and play state, entity count and settings of the open scene.", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				const Ref<Scene>& scene = context.GetActiveScene();
				return EditorCommandResult::Ok({
					{ "name", context.GetEditScene()->GetName() },
					{ "scene", context.GetSceneHandle().IsValid() ? UUIDToJson(context.GetSceneHandle()) : nlohmann::json(nullptr) },
					{ "modified", context.IsSceneModified() },
					{ "state", SceneStateToString(context.GetSceneState()) },
					{ "paused", context.IsPaused() },
					{ "entityCount", scene->GetEntityCount() },
					{ "settings", DescribeSettings(scene->GetSettings()) } });
			} });

		registry.Register({ "scene.hierarchy", "Every entity of the active scene in hierarchy order, with parent, depth and component names.", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				Scene& scene = *context.GetActiveScene();
				entt::registry& registry = scene.GetRegistry();
				nlohmann::json entities = nlohmann::json::array();
				// Depth-first with the depth carried along (walking up per entity would be quadratic on deep chains).
				std::vector<std::pair<UUID, uint32_t>> stack;
				const std::vector<UUID>& roots = scene.GetRootEntities();
				for (auto it = roots.rbegin(); it != roots.rend(); ++it)
					stack.emplace_back(*it, 0);
				while (!stack.empty())
				{
					const auto [id, depth] = stack.back();
					stack.pop_back();
					Entity entity = scene.GetEntityByUUID(id);
					if (!entity)
						continue;
					nlohmann::json components = nlohmann::json::array();
					for (const ComponentInfo* info : ComponentRegistry::GetAll())
					{
						if (!info->IsHidden() && !HasFlag(info->Flags, ComponentFlags::NoSerialize) && info->Has(registry, entity.GetHandle()))
							components.push_back(info->Name);
					}
					const RelationshipComponent& relationship = entity.GetComponent<RelationshipComponent>();
					entities.push_back({
						{ "id", UUIDToJson(id) },
						{ "name", entity.GetName() },
						{ "parent", relationship.Parent.IsValid() ? UUIDToJson(relationship.Parent) : nlohmann::json(nullptr) },
						{ "depth", depth },
						{ "active", entity.IsActive() },
						{ "components", std::move(components) } });
					for (auto it = relationship.Children.rbegin(); it != relationship.Children.rend(); ++it)
						stack.emplace_back(*it, depth + 1);
				}
				return EditorCommandResult::Ok({ { "entities", std::move(entities) } });
			} });

		registry.Register({ "scene.new",
			"Replaces the edited scene with a new, unsaved one (unsaved changes are discarded): empty by default, or with a template's content, e.g. "
			"\"basic3d\" (a lit start: camera, sun, procedural sky, ground, post-processing; the editor camera then looks through the scene's camera; "
			"the ground's material Materials/Ground.stmat is added to a project that lacks it). Save it with scene.saveAs.",
			ObjectSchema({
				{ "name", StringSchema("Scene name") },
				{ "template", { { "type", "string" }, { "enum", { ProjectTemplates::c_Empty, ProjectTemplates::c_Basic3D } },
					{ "description", "What the scene starts with (default \"empty\"; see project.templates)" } } } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string name = arguments.GetString("name", "Untitled");
				const std::string templateId = arguments.GetString("template", std::string(ProjectTemplates::c_Empty));
				if (arguments.IsValid() && !ProjectTemplates::Find(templateId))
					arguments.SetError(fmt::format("Unknown template '{}' (templates: {})", templateId, ProjectTemplates::ListIds()));
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!context.NewScene(name, templateId))
					return EditorCommandResult::Fail(fmt::format("The template '{}' could not be applied", templateId));
				return EditorCommandResult::Ok();
			} });

		registry.Register({ "scene.open", "Opens a scene asset for editing (unsaved changes are discarded).",
			ObjectSchema({ { "scene", AssetSchema("Scene to open") } }, { "scene" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const AssetHandle handle = ResolveAsset(context, arguments, "scene", AssetType::Scene);
				if (!arguments.IsValid())
					return arguments.Fail();
				std::string error;
				if (!context.OpenScene(handle, &error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok();
			} });

		registry.Register({ "scene.save", "Saves the edited scene to its asset. New scenes need scene.saveAs first.", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				std::string error;
				if (!context.SaveScene(&error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok({ { "scene", UUIDToJson(context.GetSceneHandle()) } });
			} });

		registry.Register({ "scene.saveAs", "Saves the edited scene as a new .stscene asset and continues editing it.",
			ObjectSchema({ { "path", StringSchema("Path relative to the asset directory, e.g. \"Scenes/Level1.stscene\"") } }, { "path" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string path = arguments.GetString("path");
				if (!arguments.IsValid())
					return arguments.Fail();
				std::string error;
				if (!context.SaveSceneAs(path, &error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok({ { "scene", UUIDToJson(context.GetSceneHandle()) } });
			} });

		registry.Register({ "scene.setSettings", "Changes physics settings of the edited scene (undoable; while playing it changes the running copy only).",
			ObjectSchema({
				{ "gravity", Vec3Schema("Gravity in m/s^2") },
				{ "fixedTimestep", NumberSchema("Fixed simulation step in seconds (1/1000 to 1/10)") },
				{ "maxFixedStepsPerFrame", IntegerSchema("Upper bound of fixed steps per frame", 1, 64) } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				const SceneSettings before = scene.GetSettings();
				SceneSettings after = before;
				if (auto gravity = parameters.find("gravity"); gravity != parameters.end())
				{
					if (!gravity->is_array() || gravity->size() != 3 || !(*gravity)[0].is_number() || !(*gravity)[1].is_number() || !(*gravity)[2].is_number())
						return EditorCommandResult::InvalidParameters("Parameter 'gravity' must be an array of three numbers");
					after.Gravity = { (*gravity)[0].get<float>(), (*gravity)[1].get<float>(), (*gravity)[2].get<float>() };
					if (!std::isfinite(after.Gravity.x) || !std::isfinite(after.Gravity.y) || !std::isfinite(after.Gravity.z))
						return EditorCommandResult::InvalidParameters("Parameter 'gravity' must be finite");
				}
				if (auto step = parameters.find("fixedTimestep"); step != parameters.end())
				{
					if (!step->is_number() || !(step->get<float>() >= 0.001f && step->get<float>() <= 0.1f))
						return EditorCommandResult::InvalidParameters("Parameter 'fixedTimestep' must be between 0.001 and 0.1");
					after.FixedTimestep = step->get<float>();
				}
				CommandArguments arguments(parameters);
				after.MaxFixedStepsPerFrame = static_cast<uint32_t>(arguments.GetInt("maxFixedStepsPerFrame", before.MaxFixedStepsPerFrame, 1, 64));
				if (!arguments.IsValid())
					return arguments.Fail();

				const bool changed = after.Gravity != before.Gravity || after.FixedTimestep != before.FixedTimestep
					|| after.MaxFixedStepsPerFrame != before.MaxFixedStepsPerFrame;
				scene.GetSettings() = after;
				if (changed && !context.IsPlaying())
					context.GetUndoStack().Record(CreateScope<SceneSettingsAction>(scene, before, after));
				return EditorCommandResult::Ok({ { "settings", DescribeSettings(after) } });
			} });

		////////////////////////////////////////////////////////////////////////////////
		// Entities
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "entity.create", "Creates an entity, optionally under a parent and with components (undoable; while playing it changes the running copy only). Returns its ID.",
			ObjectSchema({
				{ "name", StringSchema("Entity name (default \"Entity\")") },
				{ "parent", OptionalEntitySchema("Parent; null or omitted for a top-level entity") },
				{ "components", AnyObjectSchema("Components and property values, e.g. {\"Transform\": {\"Translation\": [0, 1, 0]}, \"MeshRenderer\": {\"Mesh\": \"<handle>\"}}") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const std::string name = arguments.GetString("name", "Entity");
				Entity parent = arguments.FindEntity(scene, "parent");
				const nlohmann::json* components = arguments.FindObject("components");
				if (!arguments.IsValid())
					return arguments.Fail();

				SceneEditTransaction transaction(scene, "Create Entity", {});
				Entity entity = parent ? scene.CreateChildEntity(parent, name) : scene.CreateEntity(name);
				if (!entity)
					return RollBack(transaction, fmt::format("The scene is full: it holds at most {} entities", Scene::c_MaxEntities), EditorCommandError::Failed);
				transaction.TrackCreated(entity.GetUUID());
				std::string error;
				if (components && !ApplyComponents(entity, *components, &error))
					return RollBack(transaction, error, EditorCommandError::InvalidParameters);
				return Finish(context, transaction, { { "id", UUIDToJson(entity.GetUUID()) } });
			} });

		registry.Register({ "entity.delete", "Deletes entities and all their descendants (undoable; while playing it changes the running copy only).",
			ObjectSchema({ { "entities", EntityArraySchema("Entities to delete") } }, { "entities" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const std::vector<Entity> entities = arguments.GetEntities(scene, "entities");
				if (!arguments.IsValid())
					return arguments.Fail();

				SceneEditTransaction transaction(scene, entities.size() == 1 ? "Delete Entity" : "Delete Entities", {});
				for (Entity entity : entities)
					transaction.TrackSubtree(entity.GetUUID());
				// One batch: each sibling list is compacted once (entities nested in another listed one go with it).
				scene.DestroyEntities(entities);
				context.PruneSelection(); // Deleted descendants too
				return Finish(context, transaction);
			} });

		registry.Register({ "entity.duplicate",
			"Duplicates entities with their descendants, each copy placed right after its original, in one undo step (while playing it changes the "
			"running copy only). References between the copied entities point at the copies. Returns the copies' IDs (entities, in the order "
			"given; id: the first).",
			ObjectSchema({
				{ "entity", EntitySchema("Entity to duplicate") },
				{ "entities", EntityArraySchema("Entities to duplicate (instead of entity); those below another listed one are copied with it") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const std::vector<Entity> originals = RemoveNested(GetTargets(arguments, scene));
				if (!arguments.IsValid())
					return arguments.Fail();
				size_t entityCount = 0;
				for (const Entity original : originals)
					entityCount += SceneEdit::CollectSubtree(scene, original.GetUUID()).size();
				if (scene.GetEntityCount() + entityCount > Scene::c_MaxEntities)
					return EditorCommandResult::Fail(fmt::format("The scene is full: it holds at most {} entities", Scene::c_MaxEntities));

				// Siblings after an original shift by one; removing the copies (undo) or reinserting them at their recorded
				// positions (redo) shifts them back, so only the copies are part of the edit. All copies are made in one batch,
				// so references between any of them point at the copies, also across sibling lists; they are made at the top
				// level (the copies at the end of the roots, which leaves every original's position as it was) and then placed in
				// one batch under their originals' parents, keeping the local transforms they had there. Both batches stay linear
				// when many entities of a long list are duplicated.
				SceneEditTransaction transaction(scene, originals.size() == 1 ? "Duplicate Entity" : "Duplicate Entities", {});
				EntityInstantiationOptions options;
				options.GenerateNewUUIDs = true;
				options.ReportUnknownComponents = false; // Entities of this scene: their unknown components are no news
				std::string error;
				const std::vector<Entity> copies = SceneSerializer::DeserializeEntities(scene, SceneSerializer::SerializeEntities(scene, originals), options, &error);
				for (const Entity copy : copies)
					transaction.TrackCreated(copy.GetUUID());
				if (!error.empty() || copies.size() != originals.size())
					return RollBack(transaction, fmt::format("Duplicating failed: {}", error.empty() ? std::string("the copies do not match") : error), EditorCommandError::Failed);

				// Each copy right after its original: the n-th original of a sibling list (in list order) moves n copies further.
				std::unordered_map<UUID, std::vector<std::pair<size_t, size_t>>> lists; // Parent -> (original's position, index into originals)
				std::vector<UUID> parents;                                            // The lists in the order their first original was given
				for (size_t index = 0; index < originals.size(); index++)
				{
					const Entity parent = originals[index].GetParent();
					const UUID parentId = parent ? parent.GetUUID() : UUID::Null();
					auto [list, added] = lists.try_emplace(parentId);
					if (added)
						parents.push_back(parentId);
					list->second.emplace_back(scene.GetSiblingIndex(originals[index]), index);
				}
				std::vector<Scene::EntityPlacement> placements;
				placements.reserve(copies.size());
				for (const UUID parentId : parents)
				{
					std::vector<std::pair<size_t, size_t>>& positions = lists.at(parentId);
					std::sort(positions.begin(), positions.end());
					for (size_t rank = 0; rank < positions.size(); rank++)
						placements.push_back({ copies[positions[rank].second].GetUUID(), parentId, positions[rank].first + rank + 1 });
				}
				if (!scene.PlaceEntities(placements))
					return RollBack(transaction, "Placing the copies failed", EditorCommandError::Failed);
				return Finish(context, transaction, { { "id", UUIDToJson(copies.front().GetUUID()) }, { "entities", ToJson(copies) } });
			} });

		registry.Register({ "entity.copy",
			"Copies entities with their descendants (as the clipboard holds them): returns data for entity.paste, which makes new entities from it "
			"in this or another scene. Changes nothing.",
			ObjectSchema({ { "entities", EntityArraySchema("Entities to copy; those below another listed one are copied with it") } }, { "entities" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const std::vector<Entity> roots = RemoveNested(arguments.GetEntities(scene, "entities"));
				if (!arguments.IsValid())
					return arguments.Fail();
				nlohmann::json data = SceneSerializer::SerializeEntities(scene, roots);
				const size_t count = data["Entities"].size();
				data["Strata"] = { { "Format", c_EntityDataFormat }, { "Version", c_EntityDataFormatVersion } };
				return EditorCommandResult::Ok({ { "data", std::move(data) }, { "roots", ToJson(roots) }, { "count", count } });
			} });

		registry.Register({ "entity.paste",
			"Creates entities from entity.copy's data, under a parent (default: the top level) at a position among its children (default: last), "
			"in one undo step (while playing it changes the running copy only). The new entities get fresh IDs; references between them point at "
			"each other, references to other entities stay. Returns the IDs of the pasted top-level entities and warnings about data this build "
			"cannot read.",
			ObjectSchema({
				{ "data", AnyObjectSchema("What entity.copy returned in 'data': {\"Strata\": {\"Format\": \"Entities\", ...}, \"Entities\": [...]}") },
				{ "parent", OptionalEntitySchema("Parent of the pasted entities; null or omitted for the top level") },
				{ "index", IntegerSchema("Position of the first pasted entity among the parent's children (default: last)", 0, std::numeric_limits<int32_t>::max()) } },
				{ "data" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const nlohmann::json& data = arguments.GetObject("data");
				Entity parent = arguments.FindEntity(scene, "parent");
				const int64_t index = arguments.GetInt("index", -1, 0, std::numeric_limits<int32_t>::max());
				if (arguments.IsValid())
				{
					const nlohmann::json* header = JsonUtils::Find(data, "Strata");
					const nlohmann::json* format = header ? JsonUtils::Find(*header, "Format") : nullptr;
					const nlohmann::json* entities = JsonUtils::Find(data, "Entities");
					if (header && (!format || *format != c_EntityDataFormat))
						arguments.SetError("Parameter 'data': not entity data (its \"Strata\" header names another format)");
					else if (!entities || !entities->is_array() || entities->empty())
						arguments.SetError("Parameter 'data': expected entity.copy's data, with a non-empty \"Entities\" array");
				}
				if (!arguments.IsValid())
					return arguments.Fail();
				if (scene.GetEntityCount() + data["Entities"].size() > Scene::c_MaxEntities)
					return EditorCommandResult::Fail(fmt::format("The scene is full: it holds at most {} entities", Scene::c_MaxEntities));

				SceneEditTransaction transaction(scene, "Paste Entities", {});
				EntityInstantiationOptions options;
				options.GenerateNewUUIDs = true;
				options.Parent = parent;
				std::string error;
				std::vector<std::string> warnings;
				const std::vector<Entity> roots = SceneSerializer::DeserializeEntities(scene, data, options, &error, &warnings);
				for (const Entity root : roots)
					transaction.TrackCreated(root.GetUUID());
				if (!error.empty())
					return RollBack(transaction, fmt::format("Parameter 'data': {}", error), EditorCommandError::InvalidParameters);
				if (index >= 0)
				{
					std::vector<Scene::EntityPlacement> placements;
					for (size_t position = 0; position < roots.size(); position++)
						placements.push_back({ roots[position].GetUUID(), parent ? parent.GetUUID() : UUID::Null(), static_cast<size_t>(index) + position });
					if (!scene.PlaceEntities(placements))
						return RollBack(transaction, "Placing the pasted entities failed", EditorCommandError::Failed);
				}
				return Finish(context, transaction, { { "entities", ToJson(roots) }, { "warnings", warnings } });
			} });

		registry.Register({ "entity.get", "An entity's name, parent, children and all component values.",
			ObjectSchema({ { "entity", EntitySchema("Entity") } }, { "entity" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				Entity entity = arguments.GetEntity(*context.GetActiveScene(), "entity");
				if (!arguments.IsValid())
					return arguments.Fail();
				return EditorCommandResult::Ok(DescribeEntity(entity));
			} });

		registry.Register({ "entity.find", "IDs of the entities matching all given filters: exact name, tag, component type.",
			ObjectSchema({ { "name", StringSchema("Exact entity name") }, { "tag", StringSchema("Tag") }, { "component", StringSchema("Component type, e.g. \"PointLight\"") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const std::optional<std::string> name = arguments.Has("name") ? std::optional(arguments.GetString("name")) : std::nullopt;
				const std::optional<std::string> tag = arguments.Has("tag") ? std::optional(arguments.GetString("tag")) : std::nullopt;
				const ComponentInfo* component = arguments.Has("component") ? GetComponentInfo(arguments, "component", false) : nullptr;
				if (!arguments.IsValid())
					return arguments.Fail();

				nlohmann::json ids = nlohmann::json::array();
				for (Entity entity : scene.GetEntitiesInHierarchyOrder())
				{
					if (name && entity.GetName() != *name)
						continue;
					if (tag && (!entity.HasComponent<TagComponent>() || entity.GetComponent<TagComponent>().Tag != *tag))
						continue;
					if (component && !component->Has(scene.GetRegistry(), entity.GetHandle()))
						continue;
					ids.push_back(UUIDToJson(entity.GetUUID()));
				}
				return EditorCommandResult::Ok({ { "entities", std::move(ids) } });
			} });

		registry.Register({ "entity.setParent",
			"Moves entities under another one, or to the top level, and/or reorders them among their siblings, in one undo step (while playing it "
			"changes the running copy only). With index, the entities end up at positions index, index + 1, ... among the parent's children, in "
			"the order given (also to reorder top-level entities or children of their current parent); without it, entities that change parent "
			"go last and the others stay where they are. Fails when the parent is one of the entities or below one.",
			ObjectSchema({
				{ "entity", EntitySchema("Entity to move") },
				{ "entities", EntityArraySchema("Entities to move (instead of entity), in the order they end up in; those below another listed one move with it") },
				{ "parent", OptionalEntitySchema("New parent; null or omitted for the top level") },
				{ "index", IntegerSchema("Final position of the first entity among the parent's children (default: last)", 0, std::numeric_limits<int32_t>::max()) },
				{ "keepWorldTransform", BoolSchema("Keep the world-space placement of entities that change parent (default true)") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const std::vector<Entity> entities = RemoveNested(GetTargets(arguments, scene));
				Entity parent = arguments.FindEntity(scene, "parent");
				const bool keepWorld = arguments.GetBool("keepWorldTransform", true);
				const int64_t index = arguments.GetInt("index", -1, 0, std::numeric_limits<int32_t>::max());
				if (!arguments.IsValid())
					return arguments.Fail();
				for (const Entity entity : entities)
				{
					if (parent && (parent == entity || scene.IsDescendantOf(parent, entity)))
						return EditorCommandResult::InvalidParameters("The new parent is one of the moved entities or one of their descendants");
				}

				SceneEditTransaction transaction(scene, entities.size() == 1 ? "Reparent Entity" : "Reparent Entities", GetIds(entities));
				const UUID parentId = parent ? parent.GetUUID() : UUID::Null();
				std::vector<Scene::EntityPlacement> placements;
				std::vector<std::pair<Entity, glm::mat4>> reparented; // With the world transform to keep
				for (size_t position = 0; position < entities.size(); position++)
				{
					const Entity entity = entities[position];
					const bool changesParent = entity.GetParent() != parent;
					if (!changesParent && index < 0)
						continue;
					if (changesParent)
						reparented.emplace_back(entity, keepWorld ? scene.GetWorldTransform(entity) : glm::mat4(1.0f));
					// One batch: each sibling list is rebuilt once; positions past the end land last, in the order given.
					const size_t target = index >= 0 ? static_cast<size_t>(index) + position : std::numeric_limits<size_t>::max();
					placements.push_back({ entity.GetUUID(), parentId, target });
				}
				if (!scene.PlaceEntities(placements))
					return RollBack(transaction, "The entities could not be placed under the new parent", EditorCommandError::Failed);
				// The new parent is not among the moved entities, so its world transform is final: the local transforms follow.
				if (keepWorld)
				{
					for (const auto& [entity, world] : reparented)
						scene.SetWorldTransform(entity, world);
				}
				return Finish(context, transaction, { { "entities", ToJson(entities) } });
			} });

		registry.Register({ "entity.rename", "Renames an entity (undoable; while playing it changes the running copy only).",
			ObjectSchema({ { "entity", EntitySchema("Entity") }, { "name", StringSchema("New name") } }, { "entity", "name" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				Entity entity = arguments.GetEntity(scene, "entity");
				const std::string name = arguments.GetString("name");
				if (!arguments.IsValid())
					return arguments.Fail();
				SceneEditTransaction transaction(scene, "Rename Entity", { entity.GetUUID() });
				entity.GetComponent<NameComponent>().Name = name;
				entity.MarkModified<NameComponent>();
				return Finish(context, transaction);
			} });

		registry.Register({ "entity.setActive",
			"Activates or deactivates entities; inactive entities and their descendants are not simulated or drawn (one undo step; while playing "
			"it changes the running copy only).",
			ObjectSchema({
				{ "entity", EntitySchema("Entity") },
				{ "entities", EntityArraySchema("Entities (instead of entity)") },
				{ "active", BoolSchema("Whether the entities are active") } },
				{ "active" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const std::vector<Entity> entities = GetTargets(arguments, scene);
				if (!arguments.Has("active"))
					arguments.SetError("Missing parameter 'active'");
				const bool active = arguments.GetBool("active", true);
				if (!arguments.IsValid())
					return arguments.Fail();
				const std::string name = entities.size() == 1 ? (active ? "Activate Entity" : "Deactivate Entity")
					: (active ? "Activate Entities" : "Deactivate Entities");
				SceneEditTransaction transaction(scene, name, GetIds(entities));
				for (Entity entity : entities)
					entity.SetActive(active);
				return Finish(context, transaction);
			} });

		////////////////////////////////////////////////////////////////////////////////
		// Components
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "component.list", "Every component type that can be added to entities, with its properties (types, ranges, enum values).", ObjectSchema({}),
			[](EditorContext&, const nlohmann::json&)
			{
				nlohmann::json components = nlohmann::json::array();
				for (const ComponentInfo* info : ComponentRegistry::GetAll())
				{
					if (info->IsHidden() || HasFlag(info->Flags, ComponentFlags::NoSerialize))
						continue;
					nlohmann::json properties = nlohmann::json::array();
					for (const PropertyInfo& property : info->Properties)
					{
						if (!property.IsHidden() && !property.IsTransient())
							properties.push_back(DescribeProperty(property));
					}
					components.push_back({
						{ "name", info->Name },
						{ "displayName", info->DisplayName },
						{ "category", info->Category },
						{ "description", info->Description },
						{ "addable", info->IsAddable() },
						{ "removable", info->IsRemovable() },
						{ "properties", std::move(properties) } });
				}
				return EditorCommandResult::Ok({ { "components", std::move(components) } });
			} });

		registry.Register({ "component.add",
			"Adds a component to an entity (or to every one of the entities) where it is missing and sets the given property values, in one undo "
			"step (while playing it changes the running copy only).",
			ObjectSchema({
				{ "entity", EntitySchema("Entity") },
				{ "entities", EntityArraySchema("Entities (instead of entity)") },
				{ "component", StringSchema("Component type") },
				{ "values", AnyObjectSchema("Property values") } },
				{ "component" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const std::vector<Entity> entities = GetTargets(arguments, scene);
				const ComponentInfo* info = GetComponentInfo(arguments, "component", true);
				const nlohmann::json* values = arguments.FindObject("values");
				if (!arguments.IsValid())
					return arguments.Fail();

				SceneEditTransaction transaction(scene, fmt::format("Add {}", info->DisplayName), GetIds(entities));
				std::string error;
				for (Entity entity : entities)
				{
					if (!ApplyComponents(entity, { { info->Name, values ? *values : nlohmann::json::object() } }, &error))
						return RollBack(transaction, error, EditorCommandError::InvalidParameters);
				}
				return Finish(context, transaction);
			} });

		registry.Register({ "component.remove",
			"Removes a component from an entity (or from every one of the entities, which must all have it), in one undo step (while playing it "
			"changes the running copy only).",
			ObjectSchema({
				{ "entity", EntitySchema("Entity") },
				{ "entities", EntityArraySchema("Entities (instead of entity)") },
				{ "component", StringSchema("Component type") } },
				{ "component" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const std::vector<Entity> entities = GetTargets(arguments, scene);
				const ComponentInfo* info = GetComponentInfo(arguments, "component", true);
				if (!arguments.IsValid())
					return arguments.Fail();
				for (const Entity entity : entities)
				{
					if (!info->Has(scene.GetRegistry(), entity.GetHandle()))
						return EditorCommandResult::Fail(fmt::format("'{}' has no {} component", entity.GetName(), info->Name));
				}

				SceneEditTransaction transaction(scene, fmt::format("Remove {}", info->DisplayName), GetIds(entities));
				std::string error;
				for (Entity entity : entities)
				{
					if (!ComponentAccess::RemoveComponent(entity, *info, &error))
						return RollBack(transaction, error, EditorCommandError::Failed);
				}
				return Finish(context, transaction);
			} });

		registry.Register({ "component.get", "The property values of one component of an entity.",
			ObjectSchema({ { "entity", EntitySchema("Entity") }, { "component", StringSchema("Component type") } }, { "entity", "component" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				Entity entity = arguments.GetEntity(scene, "entity");
				const ComponentInfo* info = GetComponentInfo(arguments, "component", false);
				if (!arguments.IsValid())
					return arguments.Fail();
				const void* component = info->Get(scene.GetRegistry(), entity.GetHandle());
				if (!component)
					return EditorCommandResult::Fail(fmt::format("The entity has no {} component", info->Name));
				return EditorCommandResult::Ok({ { "values", ComponentAccess::Serialize(*info, component) } });
			} });

		registry.Register({ "component.set",
			"Sets property values of a component that the entity (or every one of the entities) has, in one undo step (while playing it changes "
			"the running copy only). Invalid values change nothing. Returns the component's values (of the first entity).",
			ObjectSchema({
				{ "entity", EntitySchema("Entity") },
				{ "entities", EntityArraySchema("Entities to set the same values on (instead of entity)") },
				{ "component", StringSchema("Component type") },
				{ "values", AnyObjectSchema("Property values, e.g. {\"Intensity\": 2}") } },
				{ "component", "values" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const std::vector<Entity> entities = GetTargets(arguments, scene);
				const ComponentInfo* info = GetComponentInfo(arguments, "component", true);
				const nlohmann::json& values = arguments.GetObject("values");
				if (!arguments.IsValid())
					return arguments.Fail();
				for (const Entity entity : entities)
				{
					if (!info->Has(scene.GetRegistry(), entity.GetHandle()))
						return EditorCommandResult::Fail(fmt::format("'{}' has no {} component (use component.add)", entity.GetName(), info->Name));
				}

				SceneEditTransaction transaction(scene, fmt::format("Edit {}", info->DisplayName), GetIds(entities));
				std::string error;
				for (const Entity entity : entities)
				{
					if (!ApplyComponents(entity, { { info->Name, values } }, &error))
						return RollBack(transaction, error, EditorCommandError::InvalidParameters);
				}
				const Entity first = entities.front();
				return Finish(context, transaction, { { "values", ComponentAccess::Serialize(*info, info->Get(scene.GetRegistry(), first.GetHandle())) },
					{ "count", entities.size() } });
			} });

		////////////////////////////////////////////////////////////////////////////////
		// Prefabs
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "prefab.instantiate", "Instantiates a prefab or imported model, optionally under a parent and with component values on its root (undoable; while playing it changes the running copy only).",
			ObjectSchema({
				{ "prefab", AssetSchema("Prefab or model") },
				{ "parent", OptionalEntitySchema("Parent; null or omitted for the top level") },
				{ "components", AnyObjectSchema("Component values applied to the instance's root, e.g. {\"Transform\": {\"Translation\": [2, 0, 0]}}") } }, { "prefab" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const AssetHandle handle = ResolveAsset(context, arguments, "prefab", AssetType::None);
				Entity parent = arguments.FindEntity(scene, "parent");
				const nlohmann::json* components = arguments.FindObject("components");
				if (!arguments.IsValid())
					return arguments.Fail();
				const AssetType type = AssetManager::GetAssetType(handle);
				if (type != AssetType::Prefab && type != AssetType::Model)
					return EditorCommandResult::InvalidParameters(fmt::format("'{}' is a {}, not a prefab or model", parameters["prefab"].get<std::string>(), AssetTypeToString(type)));

				Ref<EntityTemplate> asset = std::static_pointer_cast<EntityTemplate>(AssetManager::GetActive()->LoadAssetSync(handle));
				if (!asset)
					return EditorCommandResult::Fail("Loading the prefab failed");

				SceneEditTransaction transaction(scene, "Instantiate Prefab", {});
				std::string instantiateError;
				const std::vector<Entity> roots = asset->Instantiate(scene, parent, &instantiateError);
				// Instantiation fails as a whole, e.g. when the scene cannot hold the entities.
				if (!instantiateError.empty())
					return RollBack(transaction, fmt::format("Instantiating '{}' failed: {}", parameters["prefab"].get<std::string>(), instantiateError), EditorCommandError::Failed);
				nlohmann::json ids = nlohmann::json::array();
				for (Entity root : roots)
				{
					transaction.TrackCreated(root.GetUUID());
					ids.push_back(UUIDToJson(root.GetUUID()));
				}
				std::string error;
				if (components && !roots.empty() && !ApplyComponents(roots.front(), *components, &error))
					return RollBack(transaction, error, EditorCommandError::InvalidParameters);
				return Finish(context, transaction, { { "entities", std::move(ids) } });
			} });
	}

}
