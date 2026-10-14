#include "stpch.h"
#include "Strata/Scene/PrefabOverrides.h"

#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Scene/ComponentAccess.h"
#include "Strata/Scene/PrefabInstances.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Strata
{

	namespace
	{

		constexpr const char* c_LinkComponent = "PrefabInstance";
		constexpr std::pair<PrefabOverrideKind, const char*> c_KindNames[] = {
			{ PrefabOverrideKind::Property, "Property" },
			{ PrefabOverrideKind::AddedComponent, "AddedComponent" },
			{ PrefabOverrideKind::RemovedComponent, "RemovedComponent" },
			{ PrefabOverrideKind::RemovedEntity, "RemovedEntity" } };

		UUID GetId(const nlohmann::json& entity)
		{
			const nlohmann::json* id = JsonUtils::Find(entity, "ID");
			return id ? UUIDFromJson(*id).value_or(UUID::Null()) : UUID::Null();
		}

		UUID GetParentId(const nlohmann::json& entity)
		{
			const nlohmann::json* parent = JsonUtils::Find(entity, "Parent");
			return parent ? UUIDFromJson(*parent).value_or(UUID::Null()) : UUID::Null();
		}

		const nlohmann::json& GetComponents(const nlohmann::json& entity)
		{
			static const nlohmann::json s_Empty = nlohmann::json::object();
			const nlohmann::json* components = JsonUtils::Find(entity, "Components");
			return components && components->is_object() ? *components : s_Empty;
		}

		// The entities of a part ({"Entities": [...]}) by id.
		std::unordered_map<UUID, const nlohmann::json*> MapEntities(const nlohmann::json& part)
		{
			std::unordered_map<UUID, const nlohmann::json*> entities;
			const nlohmann::json* list = JsonUtils::Find(part, "Entities");
			if (list && list->is_array())
			{
				for (const nlohmann::json& entity : *list)
					entities.emplace(GetId(entity), &entity);
			}
			return entities;
		}

		// A script component's scripts by class: their fields ({name: {Type, Value}}).
		std::vector<std::pair<std::string, nlohmann::json>> GetScripts(const nlohmann::json& component)
		{
			std::vector<std::pair<std::string, nlohmann::json>> scripts;
			const nlohmann::json* list = JsonUtils::Find(component, "Scripts");
			if (!list || !list->is_array())
				return scripts;
			for (const nlohmann::json& script : *list)
			{
				const nlohmann::json* name = JsonUtils::Find(script, "Class");
				if (!name || !name->is_string())
					continue;
				const nlohmann::json* fields = JsonUtils::Find(script, "Fields");
				scripts.emplace_back(name->get<std::string>(), fields && fields->is_object() ? *fields : nlohmann::json::object());
			}
			return scripts;
		}

		const nlohmann::json* FindScript(const std::vector<std::pair<std::string, nlohmann::json>>& scripts, std::string_view name)
		{
			for (const auto& [className, fields] : scripts)
			{
				if (className == name)
					return &fields;
			}
			return nullptr;
		}

		nlohmann::json MakeScriptComponent(const std::vector<std::pair<std::string, nlohmann::json>>& scripts)
		{
			nlohmann::json list = nlohmann::json::array();
			for (const auto& [className, fields] : scripts)
				list.push_back({ { "Class", className }, { "Fields", fields } });
			return { { "Scripts", std::move(list) } };
		}

		bool IsScript(std::string_view component)
		{
			return component == PrefabOverrides::c_ScriptComponent;
		}

		// The class of a script property ("<Class>.<Field>" or "<Class>") and its field (empty for a whole script).
		std::pair<std::string_view, std::string_view> SplitScriptProperty(std::string_view property)
		{
			const size_t dot = property.find('.');
			if (dot == std::string_view::npos)
				return { property, {} };
			return { property.substr(0, dot), property.substr(dot + 1) };
		}

		void CompareScripts(UUID entity, const nlohmann::json& templateComponent, const nlohmann::json& instanceComponent, std::vector<PrefabOverride>& result)
		{
			const std::string component(PrefabOverrides::c_ScriptComponent);
			const auto templateScripts = GetScripts(templateComponent);
			const auto instanceScripts = GetScripts(instanceComponent);
			for (const auto& [className, fields] : templateScripts)
			{
				const nlohmann::json* instanceFields = FindScript(instanceScripts, className);
				if (!instanceFields)
				{
					result.push_back({ entity, component, className, PrefabOverrideKind::RemovedComponent });
					continue;
				}
				std::vector<std::string> names;
				for (const auto& [field, value] : fields.items())
					names.push_back(field);
				for (const auto& [field, value] : instanceFields->items())
				{
					if (!fields.contains(field))
						names.push_back(field);
				}
				for (const std::string& field : names)
				{
					const nlohmann::json* a = JsonUtils::Find(fields, field);
					const nlohmann::json* b = JsonUtils::Find(*instanceFields, field);
					if (!a || !b || *a != *b)
						result.push_back({ entity, component, className + "." + field, PrefabOverrideKind::Property });
				}
			}
			for (const auto& [className, fields] : instanceScripts)
			{
				if (!FindScript(templateScripts, className))
					result.push_back({ entity, component, className, PrefabOverrideKind::AddedComponent });
			}
		}

		// The instance root's transform and name are its own, never overrides.
		bool IsRootOwn(bool root, std::string_view component)
		{
			return root && (component == "Transform" || component == "Name");
		}

		void CompareComponents(UUID entity, bool root, const nlohmann::json& templateComponents, const nlohmann::json& instanceComponents,
			std::vector<PrefabOverride>& result)
		{
			for (const auto& [name, value] : templateComponents.items())
			{
				if (IsRootOwn(root, name))
					continue;
				const nlohmann::json* instance = JsonUtils::Find(instanceComponents, name);
				if (IsScript(name))
				{
					CompareScripts(entity, value, instance ? *instance : nlohmann::json::object(), result);
					continue;
				}
				if (!instance)
				{
					result.push_back({ entity, name, {}, PrefabOverrideKind::RemovedComponent });
					continue;
				}
				if (!value.is_object() || !instance->is_object())
				{
					if (value != *instance)
						result.push_back({ entity, name, {}, PrefabOverrideKind::Property });
					continue;
				}
				for (const auto& [property, propertyValue] : value.items())
				{
					const nlohmann::json* other = JsonUtils::Find(*instance, property);
					if (!other || *other != propertyValue)
						result.push_back({ entity, name, property, PrefabOverrideKind::Property });
				}
				for (const auto& [property, propertyValue] : instance->items())
				{
					if (!value.contains(property))
						result.push_back({ entity, name, property, PrefabOverrideKind::Property });
				}
			}
			for (const auto& [name, value] : instanceComponents.items())
			{
				if (templateComponents.contains(name) || IsRootOwn(root, name))
					continue;
				if (IsScript(name))
					CompareScripts(entity, nlohmann::json::object(), value, result);
				else
					result.push_back({ entity, name, {}, PrefabOverrideKind::AddedComponent });
			}
		}

		// Replaces every string of the value that names an entity of the mapping (references are UUIDs as strings).
		void RemapStrings(nlohmann::json& value, const std::unordered_map<std::string, std::string>& mapping)
		{
			std::vector<nlohmann::json*> pending = { &value };
			while (!pending.empty())
			{
				nlohmann::json& current = *pending.back();
				pending.pop_back();
				if (current.is_string())
				{
					const auto found = mapping.find(current.get_ref<const std::string&>());
					if (found != mapping.end())
						current = found->second;
				}
				else if (current.is_object() || current.is_array())
				{
					for (nlohmann::json& child : current)
						pending.push_back(&child);
				}
			}
		}

		std::string ToText(UUID id)
		{
			return UUIDToJson(id).get<std::string>();
		}

		// The instance's linked entities by their prefab entity ids.
		std::unordered_map<UUID, Entity> MapLinked(Entity root)
		{
			std::unordered_map<UUID, Entity> linked;
			const AssetHandle prefab = PrefabInstances::GetPrefab(root);
			std::vector<Entity> stack = { root };
			while (!stack.empty())
			{
				const Entity entity = stack.back();
				stack.pop_back();
				const PrefabInstanceComponent* link = entity.TryGetComponent<PrefabInstanceComponent>();
				if (link && link->Prefab == prefab && link->PrefabEntityID.IsValid())
					linked.emplace(link->PrefabEntityID, entity);
				const std::vector<Entity> children = entity.GetChildren();
				stack.insert(stack.end(), children.begin(), children.end());
			}
			return linked;
		}

		// Prefab entity ids -> the instance's UUIDs, as text: what values from the prefab's part are remapped with.
		std::unordered_map<std::string, std::string> MakeRemap(const std::unordered_map<UUID, Entity>& linked)
		{
			std::unordered_map<std::string, std::string> remap;
			for (const auto& [templateId, entity] : linked)
				remap.emplace(ToText(templateId), ToText(entity.GetUUID()));
			return remap;
		}

		// Sets the properties of `values` (a component's JSON, references already remapped) on an entity's component, adding
		// it first when `add`, and signals the change.
		bool ApplyComponentValues(Entity entity, const ComponentInfo& info, const nlohmann::json& values, bool add, std::string* outError)
		{
			if (add && !ComponentAccess::AddComponent(entity, info, outError))
				return false;
			entt::registry& registry = entity.GetScene()->GetRegistry();
			void* component = info.Get(registry, entity.GetHandle());
			if (!component)
			{
				if (outError)
					*outError = fmt::format("'{}' has no {} component", entity.GetName(), info.Name);
				return false;
			}
			std::vector<std::string> warnings;
			if (!ComponentAccess::Deserialize(info, component, values, false, outError, &warnings))
				return false;
			for (const std::string& warning : warnings)
				ST_CORE_WARN("Prefab update of '{}': {}", entity.GetName(), warning);
			info.MarkModified(registry, entity.GetHandle());
			return true;
		}

		bool Fail(std::string* outError, std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		}

	}

	namespace PrefabOverrides
	{

		const char* KindToString(PrefabOverrideKind kind)
		{
			for (const auto& [value, name] : c_KindNames)
			{
				if (value == kind)
					return name;
			}
			return "Property";
		}

		std::optional<PrefabOverrideKind> KindFromString(std::string_view text)
		{
			for (const auto& [value, name] : c_KindNames)
			{
				if (text == name)
					return value;
			}
			return std::nullopt;
		}

		nlohmann::json ToJson(const std::vector<PrefabOverride>& overrides)
		{
			nlohmann::json list = nlohmann::json::array();
			for (const PrefabOverride& entry : overrides)
			{
				list.push_back({ { "Entity", entry.Entity.ToString() }, { "Component", entry.Component }, { "Property", entry.Property },
					{ "Kind", KindToString(entry.Kind) } });
			}
			return list;
		}

		std::vector<PrefabOverride> FromJson(const nlohmann::json& json, std::vector<std::string>* outWarnings)
		{
			std::vector<PrefabOverride> overrides;
			auto warn = [outWarnings](std::string message)
			{
				if (outWarnings)
					outWarnings->push_back(std::move(message));
			};
			if (!json.is_array())
			{
				warn("'Overrides' must be a list; ignored");
				return overrides;
			}
			for (size_t index = 0; index < json.size(); index++)
			{
				const nlohmann::json& entry = json[index];
				const nlohmann::json* entity = JsonUtils::Find(entry, "Entity");
				const nlohmann::json* component = JsonUtils::Find(entry, "Component");
				const nlohmann::json* property = JsonUtils::Find(entry, "Property");
				const nlohmann::json* kind = JsonUtils::Find(entry, "Kind");
				const std::optional<UUID> id = entity ? UUIDFromJson(*entity) : std::nullopt;
				const std::optional<PrefabOverrideKind> parsed = kind && kind->is_string() ? KindFromString(kind->get<std::string>()) : std::nullopt;
				if (!id || !id->IsValid() || !component || !component->is_string() || !property || !property->is_string() || !parsed)
				{
					warn(fmt::format("Override {} is malformed (it needs Entity, Component, Property and a known Kind); dropped", index));
					continue;
				}
				overrides.push_back({ *id, component->get<std::string>(), property->get<std::string>(), *parsed });
			}
			return overrides;
		}

		std::vector<PrefabOverride> ComputeOverrides(const Scene& scene, Entity root, const nlohmann::json& part)
		{
			const nlohmann::json* entities = JsonUtils::Find(part, "Entities");
			if (!root || !entities || !entities->is_array() || entities->empty())
				return {};
			std::unordered_map<UUID, UUID> ids;
			return ComputeOverrides(PrefabInstances::MakeTemplate(scene, root, nullptr, nullptr, ids), part);
		}

		std::vector<PrefabOverride> ComputeOverrides(const nlohmann::json& instancePart, const nlohmann::json& part)
		{
			std::vector<PrefabOverride> result;
			const nlohmann::json* entities = JsonUtils::Find(part, "Entities");
			if (!entities || !entities->is_array() || entities->empty())
				return result;
			const std::unordered_map<UUID, const nlohmann::json*> instanceEntities = MapEntities(instancePart);
			bool root = true;
			for (const nlohmann::json& entity : *entities)
			{
				const UUID id = GetId(entity);
				const auto found = instanceEntities.find(id);
				if (found == instanceEntities.end())
					result.push_back({ id, {}, {}, PrefabOverrideKind::RemovedEntity });
				else
					CompareComponents(id, root, GetComponents(entity), GetComponents(*found->second), result);
				root = false;
			}
			return result;
		}

		bool IsOverridden(const std::vector<PrefabOverride>& overrides, UUID entity, std::string_view component, std::string_view property)
		{
			const std::string_view scriptClass = IsScript(component) ? SplitScriptProperty(property).first : std::string_view();
			for (const PrefabOverride& entry : overrides)
			{
				if (entry.Entity != entity)
					continue;
				if (entry.Kind == PrefabOverrideKind::RemovedEntity)
					return true;
				if (entry.Component != component)
					continue;
				if (entry.Kind != PrefabOverrideKind::Property)
				{
					// A script added or removed overrides its fields; other components' additions and removals the whole component.
					if (!IsScript(component) || property.empty() || entry.Property == scriptClass)
						return true;
					continue;
				}
				if (property.empty() || entry.Property == property)
					return true;
			}
			return false;
		}

		PartChangeCache::PartChangeCache(const nlohmann::json* previous, const nlohmann::json& next)
			: m_Previous(previous), m_Next(next)
		{
		}

		const PartChangeCache::Parts& PartChangeCache::Get(UUID templateRoot)
		{
			const auto [it, added] = m_Parts.try_emplace(templateRoot);
			if (added)
			{
				if (m_Previous)
					it->second.Previous = PrefabInstances::ExtractTemplate(*m_Previous, templateRoot);
				it->second.Next = PrefabInstances::ExtractTemplate(m_Next, templateRoot);
				it->second.NextVersion = PrefabInstances::GetTemplateHash(it->second.Next);
			}
			return it->second;
		}

		bool Propagate(Scene& scene, Entity root, const nlohmann::json& previousPart, const nlohmann::json& nextPart, PropagateResult& outResult,
			const std::function<void(UUID)>& beforeChange, std::string* outError)
		{
			outResult = {};
			const nlohmann::json* nextEntities = JsonUtils::Find(nextPart, "Entities");
			const UUID templateRoot = root ? root.GetComponent<PrefabInstanceComponent>().PrefabEntityID : UUID::Null();
			if (!nextEntities || !nextEntities->is_array() || nextEntities->empty() || GetId((*nextEntities)[0]) != templateRoot)
				return Fail(outError, "The prefab no longer has the part this instance comes from");

			std::unordered_set<UUID> changed;
			auto change = [&](Entity entity)
			{
				const UUID id = entity.GetUUID();
				if (changed.insert(id).second && beforeChange)
					beforeChange(id);
			};

			// Without a recorded list, the instance's template as it is now: what it overrides, and (when nothing changes below)
			// what it still is afterwards.
			std::unordered_map<UUID, UUID> templateIds;
			const std::vector<PrefabOverride> recorded = root.GetComponent<PrefabInstanceComponent>().Overrides;
			std::optional<nlohmann::json> instanceBefore;
			if (recorded.empty())
				instanceBefore = PrefabInstances::MakeTemplate(scene, root, nullptr, nullptr, templateIds);
			const std::vector<PrefabOverride> overrides = !recorded.empty() ? recorded : ComputeOverrides(*instanceBefore, previousPart);
			const std::unordered_map<UUID, const nlohmann::json*> previous = MapEntities(previousPart);
			const std::unordered_map<UUID, const nlohmann::json*> next = MapEntities(nextPart);
			std::unordered_map<UUID, Entity> linked = MapLinked(root);
			std::unordered_map<std::string, std::string> remap = MakeRemap(linked);
			const AssetHandle prefab = PrefabInstances::GetPrefab(root);

			// Entities the prefab added: made below their parent's instance entity, with their own descendants that are new too.
			std::vector<const nlohmann::json*> added;
			std::unordered_set<UUID> addedIds;
			for (const nlohmann::json& entity : *nextEntities)
			{
				const UUID id = GetId(entity);
				if (!previous.contains(id) && !linked.contains(id) && !IsOverridden(overrides, id, {}, {}))
				{
					added.push_back(&entity);
					addedIds.insert(id);
				}
			}
			if (!added.empty())
			{
				if (scene.GetEntityCount() + added.size() > Scene::c_MaxEntities)
					return Fail(outError, fmt::format("The scene is full: it holds at most {} entities", Scene::c_MaxEntities));
				std::unordered_map<UUID, UUID> newIds;
				std::unordered_set<UUID> taken;
				for (const nlohmann::json* entity : added)
				{
					UUID target;
					while (scene.GetEntityByUUID(target) || !taken.insert(target).second)
						target = UUID();
					newIds.emplace(GetId(*entity), target);
					remap.emplace(ToText(GetId(*entity)), ToText(target));
				}
				// Top-level new entities (their parent is not new) go below their parent's instance entity; the rest follow them.
				for (const nlohmann::json* top : added)
				{
					const UUID parentId = GetParentId(*top);
					if (addedIds.contains(parentId))
						continue;
					const auto parent = linked.find(parentId);
					if (parent == linked.end())
						continue; // Its parent is gone from the instance: it goes with it
					nlohmann::json snapshot = { { "Entities", nlohmann::json::array() } };
					std::vector<UUID> subtree = { GetId(*top) };
					for (size_t index = 0; index < subtree.size(); index++)
					{
						for (const nlohmann::json* candidate : added)
						{
							if (GetParentId(*candidate) == subtree[index])
								subtree.push_back(GetId(*candidate));
						}
					}
					for (const UUID id : subtree)
					{
						nlohmann::json entity = *next.at(id);
						RemapStrings(entity, remap);
						entity["ID"] = ToText(newIds.at(id));
						entity["Components"][c_LinkComponent] = { { "Prefab", UUIDToJson(prefab) }, { "PrefabEntityID", id.ToString() } };
						if (id == GetId(*top))
							entity.erase("Parent");
						snapshot["Entities"].push_back(std::move(entity));
					}
					change(parent->second);
					EntityInstantiationOptions options;
					options.GenerateNewUUIDs = false;
					options.Parent = parent->second;
					options.ReportUnknownComponents = false;
					std::string error;
					const std::vector<Entity> made = SceneSerializer::DeserializeEntities(scene, snapshot, options, &error);
					if (made.empty())
						return Fail(outError, fmt::format("Adding the prefab's new entities failed: {}", error));
					for (const UUID id : subtree)
					{
						if (const Entity entity = scene.GetEntityByUUID(newIds.at(id)))
						{
							linked.emplace(id, entity);
							outResult.Created.push_back(entity.GetUUID());
						}
					}
				}
			}

			// Values of entities both versions of the part have.
			for (const nlohmann::json& nextEntity : *nextEntities)
			{
				const UUID id = GetId(nextEntity);
				const auto before = previous.find(id);
				const auto instance = linked.find(id);
				if (before == previous.end() || instance == linked.end() || IsOverridden(overrides, id, {}, {}))
					continue;
				Entity entity = instance->second;
				const nlohmann::json& previousComponents = GetComponents(*before->second);
				const nlohmann::json& nextComponents = GetComponents(nextEntity);
				for (const auto& [name, nextValue] : nextComponents.items())
				{
					// The root's place and name are the instance's own.
					if (id == templateRoot && (name == "Transform" || name == "Name"))
						continue;
					const nlohmann::json* previousValue = JsonUtils::Find(previousComponents, name);
					if (previousValue && *previousValue == nextValue)
						continue;
					const ComponentInfo* info = ComponentRegistry::Find(name);
					if (!info)
						continue;
					const bool has = info->Has(scene.GetRegistry(), entity.GetHandle());
					if (IsScript(name))
					{
						// Script by script and field: the instance's scripts with what changed in the prefab where it does not
						// override it.
						auto scripts = has ? GetScripts(ComponentAccess::Serialize(*info, info->Get(scene.GetRegistry(), entity.GetHandle())))
							: std::vector<std::pair<std::string, nlohmann::json>>();
						const auto previousScripts = previousValue ? GetScripts(*previousValue) : std::vector<std::pair<std::string, nlohmann::json>>();
						bool modified = false;
						for (const auto& [className, fields] : GetScripts(nextValue))
						{
							const nlohmann::json* previousFields = FindScript(previousScripts, className);
							auto current = std::find_if(scripts.begin(), scripts.end(), [&](const auto& script) { return script.first == className; });
							if (current == scripts.end())
							{
								if (!previousFields && !IsOverridden(overrides, id, name, className))
								{
									nlohmann::json remapped = fields;
									RemapStrings(remapped, remap);
									scripts.emplace_back(className, std::move(remapped));
									modified = true;
								}
								continue;
							}
							for (const auto& [field, value] : fields.items())
							{
								const nlohmann::json* previousField = previousFields ? JsonUtils::Find(*previousFields, field) : nullptr;
								if ((previousField && *previousField == value) || IsOverridden(overrides, id, name, className + "." + field))
									continue;
								nlohmann::json remapped = value;
								RemapStrings(remapped, remap);
								if (current->second.value(field, nlohmann::json()) != remapped)
								{
									current->second[field] = std::move(remapped);
									modified = true;
								}
							}
						}
						for (const auto& [className, fields] : previousScripts)
						{
							if (FindScript(GetScripts(nextValue), className) || IsOverridden(overrides, id, name, className))
								continue;
							const auto current = std::find_if(scripts.begin(), scripts.end(), [&](const auto& script) { return script.first == className; });
							if (current != scripts.end())
							{
								scripts.erase(current);
								modified = true;
							}
						}
						if (modified)
						{
							change(entity);
							if (!ApplyComponentValues(entity, *info, MakeScriptComponent(scripts), !has, outError))
								return false;
						}
						continue;
					}
					if (!has)
					{
						// Added to the prefab: made here too, unless the instance removed it (then the prefab had it before).
						if (previousValue || IsOverridden(overrides, id, name, {}))
							continue;
						nlohmann::json values = nextValue;
						RemapStrings(values, remap);
						change(entity);
						if (!ApplyComponentValues(entity, *info, values, true, outError))
							return false;
						continue;
					}
					if (IsOverridden(overrides, id, name, {}) && !nextValue.is_object())
						continue;
					nlohmann::json values = nlohmann::json::object();
					if (nextValue.is_object())
					{
						for (const auto& [property, value] : nextValue.items())
						{
							const nlohmann::json* previousProperty = previousValue ? JsonUtils::Find(*previousValue, property) : nullptr;
							if ((previousProperty && *previousProperty == value) || IsOverridden(overrides, id, name, property))
								continue;
							nlohmann::json remapped = value;
							RemapStrings(remapped, remap);
							values[property] = std::move(remapped);
						}
					}
					if (values.empty())
						continue;
					change(entity);
					if (!ApplyComponentValues(entity, *info, values, false, outError))
						return false;
				}
				// Removed from the prefab: gone here too, unless the instance added it.
				for (const auto& [name, previousValue] : previousComponents.items())
				{
					if (nextComponents.contains(name))
						continue;
					const ComponentInfo* info = ComponentRegistry::Find(name);
					if (!info || !info->Has(scene.GetRegistry(), entity.GetHandle()) || !info->IsRemovable())
						continue;
					if (IsScript(name))
					{
						// The prefab's scripts go; those the instance added stay.
						auto scripts = GetScripts(ComponentAccess::Serialize(*info, info->Get(scene.GetRegistry(), entity.GetHandle())));
						const size_t count = scripts.size();
						std::erase_if(scripts, [&](const auto& script)
						{
							return FindScript(GetScripts(previousValue), script.first) && !IsOverridden(overrides, id, name, script.first);
						});
						if (scripts.size() == count)
							continue;
						change(entity);
						if (!(scripts.empty() ? ComponentAccess::RemoveComponent(entity, *info, outError)
							: ApplyComponentValues(entity, *info, MakeScriptComponent(scripts), false, outError)))
							return false;
						continue;
					}
					if (IsOverridden(overrides, id, name, {}))
						continue;
					change(entity);
					if (!ComponentAccess::RemoveComponent(entity, *info, outError))
						return false;
				}
			}

			// Entities the prefab removed go, with what lies below them.
			std::vector<Entity> removed;
			for (const auto& [id, entity] : previous)
			{
				if (next.contains(id) || id == templateRoot)
					continue;
				const auto instance = linked.find(id);
				if (instance == linked.end())
					continue;
				std::vector<Entity> stack = { instance->second };
				while (!stack.empty())
				{
					const Entity current = stack.back();
					stack.pop_back();
					change(current);
					const std::vector<Entity> children = current.GetChildren();
					stack.insert(stack.end(), children.begin(), children.end());
				}
				removed.push_back(instance->second);
			}
			if (!removed.empty())
				scene.DestroyEntities(removed);

			// The list against the new part, from the instance's template: the one taken before when nothing changed.
			const bool unchanged = changed.empty() && outResult.Created.empty() && instanceBefore;
			templateIds.clear();
			const nlohmann::json instanceAfter = unchanged ? std::move(*instanceBefore) : PrefabInstances::MakeTemplate(scene, root, nullptr, nullptr, templateIds);
			outResult.ContentHash = PrefabInstances::GetTemplateHash(instanceAfter);
			std::vector<PrefabOverride> updated = ComputeOverrides(instanceAfter, nextPart);
			if (updated != root.GetComponent<PrefabInstanceComponent>().Overrides)
			{
				change(root);
				root.GetComponent<PrefabInstanceComponent>().Overrides = std::move(updated);
			}
			for (const UUID id : changed)
			{
				if (scene.GetEntityByUUID(id))
					outResult.Changed.push_back(id);
			}
			return true;
		}

		bool Revert(Scene& scene, Entity entity, const nlohmann::json& part, std::string_view component, std::string_view property, std::string* outError)
		{
			const PrefabInstanceComponent* link = entity ? entity.TryGetComponent<PrefabInstanceComponent>() : nullptr;
			const Entity root = PrefabInstances::FindInstanceRoot(entity);
			if (!link || !root)
				return Fail(outError, "The entity is not linked to a prefab");
			const std::unordered_map<UUID, const nlohmann::json*> entities = MapEntities(part);
			const auto found = entities.find(link->PrefabEntityID);
			if (found == entities.end())
				return Fail(outError, fmt::format("The prefab no longer has '{}'", entity.GetName()));
			const ComponentInfo* info = ComponentRegistry::Find(component);
			if (!info)
				return Fail(outError, fmt::format("Unknown component '{}'", component));
			const nlohmann::json* templateValue = JsonUtils::Find(GetComponents(*found->second), info->Name);
			const std::unordered_map<std::string, std::string> remap = MakeRemap(MapLinked(root));
			entt::registry& registry = scene.GetRegistry();
			const bool has = info->Has(registry, entity.GetHandle());

			if (IsScript(info->Name) && !property.empty())
			{
				const auto [className, field] = SplitScriptProperty(property);
				auto scripts = has ? GetScripts(ComponentAccess::Serialize(*info, info->Get(registry, entity.GetHandle())))
					: std::vector<std::pair<std::string, nlohmann::json>>();
				const auto templateScripts = templateValue ? GetScripts(*templateValue) : std::vector<std::pair<std::string, nlohmann::json>>();
				const nlohmann::json* templateFields = FindScript(templateScripts, className);
				auto current = std::find_if(scripts.begin(), scripts.end(), [&](const auto& script) { return script.first == className; });
				if (field.empty())
				{
					if (!templateFields)
					{
						if (current != scripts.end())
							scripts.erase(current);
					}
					else
					{
						nlohmann::json fields = *templateFields;
						RemapStrings(fields, remap);
						if (current != scripts.end())
							current->second = std::move(fields);
						else
							scripts.emplace_back(std::string(className), std::move(fields));
					}
				}
				else
				{
					const nlohmann::json* value = templateFields ? JsonUtils::Find(*templateFields, field) : nullptr;
					if (current == scripts.end() || !value)
						return Fail(outError, fmt::format("'{}' and its prefab do not both have the script field {}", entity.GetName(), property));
					nlohmann::json remapped = *value;
					RemapStrings(remapped, remap);
					current->second[std::string(field)] = std::move(remapped);
				}
				return ApplyComponentValues(entity, *info, MakeScriptComponent(scripts), !has, outError);
			}
			if (property.empty())
			{
				if (!templateValue)
				{
					if (has && !ComponentAccess::RemoveComponent(entity, *info, outError))
						return false;
					return true;
				}
				nlohmann::json values = *templateValue;
				RemapStrings(values, remap);
				return ApplyComponentValues(entity, *info, values, !has, outError);
			}
			const nlohmann::json* value = templateValue ? JsonUtils::Find(*templateValue, property) : nullptr;
			if (!has || !value)
				return Fail(outError, fmt::format("'{}' and its prefab do not both have {}.{}", entity.GetName(), info->Name, property));
			nlohmann::json values = { { std::string(property), *value } };
			RemapStrings(values, remap);
			return ApplyComponentValues(entity, *info, values, false, outError);
		}

		nlohmann::json ApplyToPart(const nlohmann::json& part, const nlohmann::json& instancePart, UUID entity, std::string_view component, std::string_view property)
		{
			nlohmann::json result = part;
			nlohmann::json* entities = result.contains("Entities") ? &result["Entities"] : nullptr;
			if (!entities || !entities->is_array())
				return result;
			const std::unordered_map<UUID, const nlohmann::json*> instance = MapEntities(instancePart);
			const auto source = instance.find(entity);
			auto target = std::find_if(entities->begin(), entities->end(), [entity](const nlohmann::json& candidate) { return GetId(candidate) == entity; });
			if (component.empty())
			{
				// The entity itself: removed from the instance, so from the prefab (with what lies below it).
				if (source != instance.end() || target == entities->end())
					return result;
				std::unordered_set<UUID> removed = { entity };
				bool grew = true;
				while (grew)
				{
					grew = false;
					for (const nlohmann::json& candidate : *entities)
					{
						if (removed.contains(GetParentId(candidate)) && removed.insert(GetId(candidate)).second)
							grew = true;
					}
				}
				nlohmann::json kept = nlohmann::json::array();
				for (const nlohmann::json& candidate : *entities)
				{
					if (!removed.contains(GetId(candidate)))
						kept.push_back(candidate);
				}
				*entities = std::move(kept);
				return result;
			}
			if (source == instance.end() || target == entities->end())
				return result;
			const std::string name(component);
			nlohmann::json& components = (*target)["Components"];
			const nlohmann::json* value = JsonUtils::Find(GetComponents(*source->second), name);
			if (IsScript(component) && !property.empty())
			{
				const auto [className, field] = SplitScriptProperty(property);
				auto scripts = components.contains(name) ? GetScripts(components[name]) : std::vector<std::pair<std::string, nlohmann::json>>();
				const auto instanceScripts = value ? GetScripts(*value) : std::vector<std::pair<std::string, nlohmann::json>>();
				const nlohmann::json* instanceFields = FindScript(instanceScripts, className);
				auto current = std::find_if(scripts.begin(), scripts.end(), [&](const auto& script) { return script.first == className; });
				if (field.empty())
				{
					if (!instanceFields && current != scripts.end())
						scripts.erase(current);
					else if (instanceFields && current != scripts.end())
						current->second = *instanceFields;
					else if (instanceFields)
						scripts.emplace_back(std::string(className), *instanceFields);
				}
				else if (instanceFields && current != scripts.end())
				{
					if (const nlohmann::json* fieldValue = JsonUtils::Find(*instanceFields, field))
						current->second[std::string(field)] = *fieldValue;
					else
						current->second.erase(std::string(field));
				}
				components[name] = MakeScriptComponent(scripts);
				return result;
			}
			if (property.empty())
			{
				if (value)
					components[name] = *value;
				else
					components.erase(name);
				return result;
			}
			if (!value || !components.contains(name) || !components[name].is_object())
				return result;
			if (const nlohmann::json* propertyValue = JsonUtils::Find(*value, std::string(property)))
				components[name][std::string(property)] = *propertyValue;
			else
				components[name].erase(std::string(property));
			return result;
		}

	}

}
