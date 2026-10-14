#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HierarchyView.h"
#include "Editor/SceneBounds.h"
#include "Editor/SceneEdit.h"
#include "Editor/SceneRaycast.h"

#include <Strata/Core/JsonUtils.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/ComponentAccess.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// The entity.* and component.* commands (moved out of EditorSceneCommands.cpp, whose registration function had grown past
// 800 lines).

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		// entity.dropToGround leaves entities closer than this to the surface below them where they are.
		constexpr float c_DropTolerance = 1.0e-4f;

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

		// The undo step of component.set: the properties it sets by their display names, of the entities ("Set Material of
		// 'Ground'", "Set Intensity and Color of 3 entities"), or the component for more than two ("Edit Point Light of 'Lamp'").
		std::string DescribeSetValues(const ComponentInfo& info, const nlohmann::json& values, std::span<const Entity> entities)
		{
			std::vector<std::string> names;
			for (const auto& [key, value] : values.items())
			{
				const PropertyInfo* property = info.FindProperty(key);
				names.push_back(property ? property->DisplayName : key);
			}
			const std::string target = SceneEdit::DescribeEntities(entities);
			if (names.empty() || names.size() > 2)
				return fmt::format("Edit {} of {}", info.DisplayName, target);
			return fmt::format("Set {} of {}", names.size() == 1 ? names[0] : fmt::format("{} and {}", names[0], names[1]), target);
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

		// The entities in the order given, each once.
		std::vector<Entity> RemoveDuplicates(const std::vector<Entity>& entities)
		{
			std::unordered_set<UUID> seen;
			std::vector<Entity> result;
			for (const Entity entity : entities)
			{
				if (seen.insert(entity.GetUUID()).second)
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

	void RegisterEntityCommands(EditorCommandRegistry& registry)
	{
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

				SceneEditTransaction transaction(scene, fmt::format("Create {}", SceneEdit::QuoteName(name)), {});
				Entity entity = parent ? scene.CreateChildEntity(parent, name) : scene.CreateEntity(name);
				if (!entity)
					return RollBackEdit(transaction, fmt::format("The scene is full: it holds at most {} entities", Scene::c_MaxEntities), EditorCommandError::Failed);
				transaction.TrackCreated(entity.GetUUID());
				std::string error;
				if (components && !ApplyComponents(entity, *components, &error))
					return RollBackEdit(transaction, error, EditorCommandError::InvalidParameters);
				return FinishEdit(context, transaction, { { "id", UUIDToJson(entity.GetUUID()) } });
			} });

		registry.Register({ "entity.delete",
			"Deletes entities and all their descendants (undoable; while playing it changes the running copy only). Returns deleted (the IDs "
			"given) and count (the entities deleted, descendants included).",
			ObjectSchema({ { "entities", EntityArraySchema("Entities to delete") } }, { "entities" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const std::vector<Entity> entities = arguments.GetEntities(scene, "entities");
				if (!arguments.IsValid())
					return arguments.Fail();

				nlohmann::json deleted = ToJson(RemoveDuplicates(entities));
				SceneEditTransaction transaction(scene, fmt::format("Delete {}", SceneEdit::DescribeEntities(entities)), {});
				for (Entity entity : entities)
					transaction.TrackSubtree(entity.GetUUID());
				// Every entity that goes, each once (descendants and entities listed below another listed one included).
				const size_t count = transaction.GetBefore().size();
				// One batch: each sibling list is compacted once (entities nested in another listed one go with it).
				scene.DestroyEntities(entities);
				context.PruneSelection(); // Deleted descendants too
				return FinishEdit(context, transaction, { { "deleted", std::move(deleted) }, { "count", count } });
			} });

		registry.Register({ "entity.duplicate",
			"Duplicates entities with their descendants, each copy placed right after its original, in one undo step (while playing it changes the "
			"running copy only). References between the copied entities point at the copies. Copies get names unique among their siblings "
			"('Crate' -> 'Crate (1)'), so read names back from the returned IDs rather than finding copies by name. Returns the copies' IDs "
			"(entities, in the order given; id: the first).",
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
				SceneEditTransaction transaction(scene, fmt::format("Duplicate {}", SceneEdit::DescribeEntities(originals)), {});
				EntityInstantiationOptions options;
				options.GenerateNewUUIDs = true;
				options.ReportUnknownComponents = false; // Entities of this scene: their unknown components are no news
				std::string error;
				const std::vector<Entity> copies = SceneSerializer::DeserializeEntities(scene, SceneSerializer::SerializeEntities(scene, originals), options, &error);
				for (const Entity copy : copies)
					transaction.TrackCreated(copy.GetUUID());
				if (!error.empty() || copies.size() != originals.size())
					return RollBackEdit(transaction, fmt::format("Duplicating failed: {}", error.empty() ? std::string("the copies do not match") : error), EditorCommandError::Failed);

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
					return RollBackEdit(transaction, "Placing the copies failed", EditorCommandError::Failed);
				// "Crate" -> "Crate (1)": a copy is told apart from its original among their siblings.
				SceneEdit::NameNumbers numbers;
				for (const Entity copy : copies)
					SceneEdit::MakeNameUnique(scene, copy, &numbers);
				return FinishEdit(context, transaction, { { "id", UUIDToJson(copies.front().GetUUID()) }, { "entities", ToJson(copies) } });
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
			}, EditorCommandKind::Query });

		registry.Register({ "entity.paste",
			"Creates entities from entity.copy's data, under a parent (default: the top level) at a position among its children (default: last), "
			"in one undo step (while playing it changes the running copy only). The new entities get fresh IDs; references between them point at "
			"each other, references to other entities stay. Top-level entities whose names their new siblings have get a number ('Crate' -> "
			"'Crate (1)'). Returns the IDs of the pasted top-level entities and warnings about data this build cannot read.",
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
					return RollBackEdit(transaction, fmt::format("Parameter 'data': {}", error), EditorCommandError::InvalidParameters);
				if (index >= 0)
				{
					std::vector<Scene::EntityPlacement> placements;
					for (size_t position = 0; position < roots.size(); position++)
						placements.push_back({ roots[position].GetUUID(), parent ? parent.GetUUID() : UUID::Null(), static_cast<size_t>(index) + position });
					if (!scene.PlaceEntities(placements))
						return RollBackEdit(transaction, "Placing the pasted entities failed", EditorCommandError::Failed);
				}
				SceneEdit::NameNumbers numbers;
				for (const Entity root : roots)
					SceneEdit::MakeNameUnique(scene, root, &numbers);
				transaction.SetName(fmt::format("Paste {}", SceneEdit::DescribeEntities(roots)));
				return FinishEdit(context, transaction, { { "entities", ToJson(roots) }, { "warnings", warnings } });
			} });

		registry.Register({ "entity.get",
			"An entity's name, parent, children and all component values. With world: true also where it is in the world, decomposed from its "
			"world transform: world: {position, rotation: {euler (degrees, as the Inspector shows them), quaternion [x, y, z, w]}, scale}, and what "
			"it and its descendants take up: bounds: {min, max, size, center, pending} (what renders, skinned meshes in their pose; else physical "
			"shapes; else boxes around the entities; pending: a mesh still loading stood in as a box: ask again).",
			ObjectSchema({ { "entity", EntitySchema("Entity") }, { "world", BoolSchema("Add world (the world transform) and bounds (default false)") } },
				{ "entity" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				Entity entity = arguments.GetEntity(scene, "entity");
				const bool world = arguments.GetBool("world", false);
				if (!arguments.IsValid())
					return arguments.Fail();
				nlohmann::json value = DescribeEntity(entity);
				if (world)
				{
					std::vector<AssetHandle> pending;
					const AABB bounds = SceneBounds::GetEntityBounds(scene, entity, true, &pending);
					value["world"] = DescribeWorldTransform(scene.GetWorldTransform(entity));
					value["bounds"] = DescribeBounds(bounds, !pending.empty());
				}
				return EditorCommandResult::Ok(std::move(value));
			}, EditorCommandKind::Query });

		registry.Register({ "entity.find",
			"IDs of the entities matching all given filters, in hierarchy order: exact name, tag, component type, and box: entities whose own "
			"world bounds (what they render, else their physical shape, else their origin) intersect the box. limit returns at most that many.",
			ObjectSchema({
				{ "name", StringSchema("Exact entity name") },
				{ "tag", StringSchema("Tag") },
				{ "component", StringSchema("Component type, e.g. \"PointLight\"") },
				{ "box", { { "type", "object" }, { "description", "A world-space box {min: [x, y, z], max: [x, y, z]}" },
					{ "properties", { { "min", Vec3Schema("Lowest corner") }, { "max", Vec3Schema("Highest corner") } } },
					{ "required", nlohmann::json::array({ "min", "max" }) }, { "additionalProperties", false } } },
				{ "limit", IntegerSchema("Most entities returned (default: all)", 1, Scene::c_MaxEntities) } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const std::optional<std::string> name = arguments.Has("name") ? std::optional(arguments.GetString("name")) : std::nullopt;
				const std::optional<std::string> tag = arguments.Has("tag") ? std::optional(arguments.GetString("tag")) : std::nullopt;
				const ComponentInfo* component = arguments.Has("component") ? GetComponentInfo(arguments, "component", false) : nullptr;
				const int64_t limit = arguments.GetInt("limit", Scene::c_MaxEntities, 1, Scene::c_MaxEntities);
				std::optional<AABB> box;
				if (const nlohmann::json* boxJson = arguments.FindObject("box"))
				{
					const std::optional<glm::vec3> min = ReadVec3(boxJson->value("min", nlohmann::json()));
					const std::optional<glm::vec3> max = ReadVec3(boxJson->value("max", nlohmann::json()));
					if (!min || !max || boxJson->size() != 2)
						arguments.SetError("Parameter 'box' must be {\"min\": [x, y, z], \"max\": [x, y, z]} with finite numbers");
					else if (min->x > max->x || min->y > max->y || min->z > max->z)
						arguments.SetError("Parameter 'box': min must not lie above max on any axis");
					else
						box = AABB(*min, *max);
				}
				if (!arguments.IsValid())
					return arguments.Fail();

				if (box)
					scene.UpdateWorldTransforms();
				nlohmann::json ids = nlohmann::json::array();
				for (Entity entity : scene.GetEntitiesInHierarchyOrder())
				{
					if (name && entity.GetName() != *name)
						continue;
					if (tag && (!entity.HasComponent<TagComponent>() || entity.GetComponent<TagComponent>().Tag != *tag))
						continue;
					if (component && !component->Has(scene.GetRegistry(), entity.GetHandle()))
						continue;
					if (box)
					{
						const AABB bounds = SceneBounds::GetOwnBounds(scene, entity);
						if (!bounds.IsValid() || !bounds.Intersects(*box))
							continue;
					}
					ids.push_back(UUIDToJson(entity.GetUUID()));
					if (ids.size() >= static_cast<size_t>(limit))
						break;
				}
				return EditorCommandResult::Ok({ { "entities", std::move(ids) } });
			}, EditorCommandKind::Query });

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

				SceneEditTransaction transaction(scene, parent ? fmt::format("Move {} under {}", SceneEdit::DescribeEntities(entities), SceneEdit::QuoteName(parent.GetName()))
					: fmt::format("Move {} to the top level", SceneEdit::DescribeEntities(entities)), GetIds(entities));
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
					return RollBackEdit(transaction, "The entities could not be placed under the new parent", EditorCommandError::Failed);
				// The new parent is not among the moved entities, so its world transform is final: the local transforms follow.
				if (keepWorld)
				{
					for (const auto& [entity, world] : reparented)
						scene.SetWorldTransform(entity, world);
				}
				return FinishEdit(context, transaction, { { "entities", ToJson(entities) } });
			} });

		registry.Register({ "entity.rename", "Renames an entity (undoable; while playing it changes the running copy only). Returns {entity, name}.",
			ObjectSchema({ { "entity", EntitySchema("Entity") }, { "name", StringSchema("New name") } }, { "entity", "name" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				Entity entity = arguments.GetEntity(scene, "entity");
				const std::string name = arguments.GetString("name");
				if (!arguments.IsValid())
					return arguments.Fail();
				SceneEditTransaction transaction(scene, fmt::format("Rename {} to {}", SceneEdit::QuoteName(entity.GetName()), SceneEdit::QuoteName(name)), { entity.GetUUID() });
				entity.GetComponent<NameComponent>().Name = name;
				entity.MarkModified<NameComponent>();
				return FinishEdit(context, transaction, { { "entity", UUIDToJson(entity.GetUUID()) }, { "name", name } });
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
				SceneEditTransaction transaction(scene, fmt::format("{} {}", active ? "Activate" : "Deactivate", SceneEdit::DescribeEntities(entities)), GetIds(entities));
				for (Entity entity : entities)
					entity.SetActive(active);
				return FinishEdit(context, transaction);
			} });

		registry.Register({ "entity.dropToGround",
			"Moves entities straight down until what they draw (with their children) rests on the surface below them: the mesh renderers "
			"of the scene, hit where their meshes are loaded (their bounds otherwise), their own left out. Entities listed below another "
			"listed one move with it; those with nothing below stay. One undo step (while playing it changes the running copy only). "
			"Returns {moved: [ids], unchanged: [ids]}.",
			ObjectSchema({ { "entities", EntityArraySchema("Entities to drop") } }, { "entities" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const std::vector<Entity> entities = arguments.GetEntities(scene, "entities");
				if (!arguments.IsValid())
					return arguments.Fail();
				std::vector<UUID> ids = GetIds(entities);
				const std::vector<UUID> roots = HierarchyView::GetTopmostInHierarchyOrder(scene, ids);
				SceneEditTransaction transaction(scene, roots.size() == 1 ? "Drop Entity to Ground" : "Drop Entities to Ground", roots);
				nlohmann::json moved = nlohmann::json::array();
				nlohmann::json unchanged = nlohmann::json::array();
				for (const UUID id : roots)
				{
					const Entity entity = scene.GetEntityByUUID(id);
					// Rays straight down from the bottom of what the entity draws: its center and, a little inside, its corners, so
					// an entity whose middle hangs over an edge still lands on what is below its side. The nearest hit is where it
					// stops.
					const AABB bounds = SceneBounds::GetEntityBounds(scene, entity);
					const Entity ignore[] = { entity };
					std::optional<float> drop;
					if (bounds.IsValid())
					{
						const glm::vec3 center = bounds.GetCenter();
						const glm::vec3 inset = bounds.GetExtents() * 0.9f;
						const glm::vec2 offsets[] = { { 0.0f, 0.0f }, { -1.0f, -1.0f }, { 1.0f, -1.0f }, { -1.0f, 1.0f }, { 1.0f, 1.0f } };
						for (const glm::vec2& offset : offsets)
						{
							const glm::vec3 start(center.x + offset.x * inset.x, bounds.Min.y, center.z + offset.y * inset.z);
							if (const std::optional<SceneRayHit> hit = SceneRaycast::Cast(scene, Ray(start, glm::vec3(0.0f, -1.0f, 0.0f)), std::numeric_limits<float>::max(), ignore))
								drop = drop ? std::min(*drop, hit->Distance) : hit->Distance;
						}
					}
					if (!drop || *drop <= c_DropTolerance)
					{
						unchanged.push_back(UUIDToJson(id));
						continue;
					}
					const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -*drop, 0.0f)) * scene.GetWorldTransform(entity);
					if (!scene.SetWorldTransform(entity, world))
					{
						unchanged.push_back(UUIDToJson(id));
						continue;
					}
					moved.push_back(UUIDToJson(id));
				}
				return FinishEdit(context, transaction, { { "moved", std::move(moved) }, { "unchanged", std::move(unchanged) } });
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
			}, EditorCommandKind::Query });

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

				SceneEditTransaction transaction(scene, fmt::format("Add {} to {}", info->DisplayName, SceneEdit::DescribeEntities(entities)), GetIds(entities));
				std::string error;
				for (Entity entity : entities)
				{
					if (!ApplyComponents(entity, { { info->Name, values ? *values : nlohmann::json::object() } }, &error))
						return RollBackEdit(transaction, error, EditorCommandError::InvalidParameters);
				}
				return FinishEdit(context, transaction);
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

				SceneEditTransaction transaction(scene, fmt::format("Remove {} from {}", info->DisplayName, SceneEdit::DescribeEntities(entities)), GetIds(entities));
				std::string error;
				for (Entity entity : entities)
				{
					if (!ComponentAccess::RemoveComponent(entity, *info, &error))
						return RollBackEdit(transaction, error, EditorCommandError::Failed);
				}
				return FinishEdit(context, transaction);
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
			}, EditorCommandKind::Query });

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

				SceneEditTransaction transaction(scene, DescribeSetValues(*info, values, entities), GetIds(entities));
				std::string error;
				for (const Entity entity : entities)
				{
					if (!ApplyComponents(entity, { { info->Name, values } }, &error))
						return RollBackEdit(transaction, error, EditorCommandError::InvalidParameters);
				}
				const Entity first = entities.front();
				return FinishEdit(context, transaction, { { "values", ComponentAccess::Serialize(*info, info->Get(scene.GetRegistry(), first.GetHandle())) },
					{ "count", entities.size() } });
			} });
	}

}
