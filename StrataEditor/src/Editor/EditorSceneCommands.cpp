#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/ComponentAccess.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Prefab.h>
#include <Strata/Scene/Scene.h>

#include <cmath>
#include <limits>

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

		registry.Register({ "scene.new", "Replaces the edited scene with an empty one (unsaved changes are discarded).",
			ObjectSchema({ { "name", StringSchema("Scene name") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string name = arguments.GetString("name", "Untitled");
				if (!arguments.IsValid())
					return arguments.Fail();
				context.NewScene(name);
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

		registry.Register({ "entity.duplicate", "Duplicates an entity with its descendants, placed after it (undoable; while playing it changes the running copy only). Returns the copy's ID.",
			ObjectSchema({ { "entity", EntitySchema("Entity to duplicate") } }, { "entity" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				Entity entity = arguments.GetEntity(scene, "entity");
				if (!arguments.IsValid())
					return arguments.Fail();

				// Siblings after the original shift by one; removing the copy (undo) or reinserting it at its recorded
				// position (redo) shifts them back, so only the copy is part of the edit.
				SceneEditTransaction transaction(scene, "Duplicate Entity", {});
				Entity copy = scene.DuplicateEntity(entity);
				if (!copy)
					return RollBack(transaction, "Duplicating failed", EditorCommandError::Failed);
				transaction.TrackCreated(copy.GetUUID());
				return Finish(context, transaction, { { "id", UUIDToJson(copy.GetUUID()) } });
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

		registry.Register({ "entity.setParent", "Moves an entity under another one, or to the top level (undoable; while playing it changes the running copy only).",
			ObjectSchema({
				{ "entity", EntitySchema("Entity to move") },
				{ "parent", OptionalEntitySchema("New parent; null or omitted for the top level") },
				{ "index", IntegerSchema("Position among the new siblings (default: last)", 0, std::numeric_limits<int32_t>::max()) },
				{ "keepWorldTransform", BoolSchema("Keep the world-space placement (default true)") } }, { "entity" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				Entity entity = arguments.GetEntity(scene, "entity");
				Entity parent = arguments.FindEntity(scene, "parent");
				const bool keepWorld = arguments.GetBool("keepWorldTransform", true);
				const int64_t index = arguments.GetInt("index", -1, 0, std::numeric_limits<int32_t>::max());
				if (!arguments.IsValid())
					return arguments.Fail();

				SceneEditTransaction transaction(scene, "Reparent Entity", { entity.GetUUID() });
				if (!scene.SetParent(entity, parent, keepWorld))
					return RollBack(transaction, "The new parent is the entity itself or one of its descendants", EditorCommandError::InvalidParameters);
				if (index >= 0)
					scene.SetSiblingIndex(entity, static_cast<size_t>(index));
				return Finish(context, transaction);
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

		registry.Register({ "entity.setActive", "Activates or deactivates an entity; inactive entities and their descendants are not simulated or drawn (undoable; while playing it changes the running copy only).",
			ObjectSchema({ { "entity", EntitySchema("Entity") }, { "active", BoolSchema("Whether the entity is active") } }, { "entity", "active" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				Entity entity = arguments.GetEntity(scene, "entity");
				if (!arguments.Has("active"))
					arguments.SetError("Missing parameter 'active'");
				const bool active = arguments.GetBool("active", true);
				if (!arguments.IsValid())
					return arguments.Fail();
				SceneEditTransaction transaction(scene, active ? "Activate Entity" : "Deactivate Entity", { entity.GetUUID() });
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

		registry.Register({ "component.add", "Adds a component (if missing) and sets the given property values (undoable; while playing it changes the running copy only).",
			ObjectSchema({ { "entity", EntitySchema("Entity") }, { "component", StringSchema("Component type") }, { "values", AnyObjectSchema("Property values") } }, { "entity", "component" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				Entity entity = arguments.GetEntity(scene, "entity");
				const ComponentInfo* info = GetComponentInfo(arguments, "component", true);
				const nlohmann::json* values = arguments.FindObject("values");
				if (!arguments.IsValid())
					return arguments.Fail();

				SceneEditTransaction transaction(scene, fmt::format("Add {}", info->DisplayName), { entity.GetUUID() });
				std::string error;
				if (!ApplyComponents(entity, { { info->Name, values ? *values : nlohmann::json::object() } }, &error))
					return RollBack(transaction, error, EditorCommandError::InvalidParameters);
				return Finish(context, transaction);
			} });

		registry.Register({ "component.remove", "Removes a component from an entity (undoable; while playing it changes the running copy only).",
			ObjectSchema({ { "entity", EntitySchema("Entity") }, { "component", StringSchema("Component type") } }, { "entity", "component" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				Entity entity = arguments.GetEntity(scene, "entity");
				const ComponentInfo* info = GetComponentInfo(arguments, "component", true);
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!info->Has(scene.GetRegistry(), entity.GetHandle()))
					return EditorCommandResult::Fail(fmt::format("The entity has no {} component", info->Name));

				SceneEditTransaction transaction(scene, fmt::format("Remove {}", info->DisplayName), { entity.GetUUID() });
				std::string error;
				if (!ComponentAccess::RemoveComponent(entity, *info, &error))
					return RollBack(transaction, error, EditorCommandError::Failed);
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

		registry.Register({ "component.set", "Sets property values of a component the entity has (undoable; while playing it changes the running copy only). Invalid values change nothing.",
			ObjectSchema({ { "entity", EntitySchema("Entity") }, { "component", StringSchema("Component type") }, { "values", AnyObjectSchema("Property values, e.g. {\"Intensity\": 2}") } },
				{ "entity", "component", "values" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				Entity entity = arguments.GetEntity(scene, "entity");
				const ComponentInfo* info = GetComponentInfo(arguments, "component", true);
				const nlohmann::json& values = arguments.GetObject("values");
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!info->Has(scene.GetRegistry(), entity.GetHandle()))
					return EditorCommandResult::Fail(fmt::format("The entity has no {} component (use component.add)", info->Name));

				SceneEditTransaction transaction(scene, fmt::format("Edit {}", info->DisplayName), { entity.GetUUID() });
				std::string error;
				if (!ApplyComponents(entity, { { info->Name, values } }, &error))
					return RollBack(transaction, error, EditorCommandError::InvalidParameters);
				return Finish(context, transaction, { { "values", ComponentAccess::Serialize(*info, info->Get(scene.GetRegistry(), entity.GetHandle())) } });
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
				const std::vector<Entity> roots = asset->Instantiate(scene, parent);
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
