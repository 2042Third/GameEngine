#include "stpch.h"
#include "Strata/Scene/EntitySink.h"

#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Scene/ComponentAccess.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/EntityPrototype.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/UnknownComponents.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <utility>

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_NotLinked = std::numeric_limits<uint32_t>::max();
		// Entities between two progress reports of a sink's Finish: a few milliseconds of work.
		constexpr size_t c_FinishProgressInterval = 4096;

		void ReportFinish(const EntityReadProgress& progress, float start, float share, size_t index, size_t count)
		{
			if (progress && index > 0 && index % c_FinishProgressInterval == 0)
				progress(start + share * static_cast<float>(static_cast<double>(index) / static_cast<double>(count)));
		}

		// The ID of an entity object ("ID"; a new one when it has none), or nullopt when it is invalid.
		std::optional<UUID> ReadEntityID(const nlohmann::json& entity)
		{
			const nlohmann::json* id = JsonUtils::Find(entity, "ID");
			const std::optional<UUID> parsed = id ? UUIDFromJson(*id) : std::optional<UUID>(UUID());
			if (!parsed || !parsed->IsValid())
				return std::nullopt;
			return parsed;
		}

		// Reads the "Components" of an entity object as SceneSerializer::DeserializeEntities does: each registered component
		// into the object `add` returns for it (ComponentAccess::Deserialize, not strict: what it cannot read is reported),
		// components this build does not register into `outUnknown`, counted in `tally`.
		template<typename AddComponent>
		void ReadComponents(const nlohmann::json& entity, UUID id, AddComponent&& add, nlohmann::json& outUnknown, Detail::UnknownComponentTally& tally,
			std::vector<std::string>* outWarnings)
		{
			const nlohmann::json* components = JsonUtils::Find(entity, "Components");
			if (!components)
				return;
			if (!components->is_object())
			{
				if (outWarnings)
					outWarnings->push_back(fmt::format("Entity {}: 'Components' must be an object", id.ToString()));
				return;
			}
			for (const auto& [componentName, componentJson] : components->items())
			{
				const ComponentInfo* info = ComponentRegistry::Find(componentName);
				if (!info)
				{
					outUnknown[componentName] = componentJson;
					tally.Add(componentName);
					continue;
				}
				if (HasFlag(info->Flags, ComponentFlags::NoSerialize))
				{
					if (outWarnings)
						outWarnings->push_back(fmt::format("Entity {}: component '{}' is not read from entity data; skipped", id.ToString(), componentName));
					continue;
				}
				void* component = add(*info);
				std::string error;
				if (!ComponentAccess::Deserialize(*info, component, componentJson, false, &error, outWarnings) && outWarnings)
					outWarnings->push_back(fmt::format("Entity {}: {}", id.ToString(), error));
			}
		}

		// The parent each entity is linked to (its index in document order, or c_NotLinked: the top level), as
		// SceneSerializer::DeserializeEntities links them: in document order, a parent that is not an ID (`invalidParents`,
		// ascending), is not among the entities (`indexOf` gives c_NotLinked) or would close a cycle leaves the entity at the
		// top level, with a warning. Accepted links form a forest in which each child is still a root when its link is
		// considered (IDs are unique), so a link closes a cycle exactly when both ends are already connected: union-find
		// answers that in near-constant time, keeping deep hierarchies linear to load.
		template<typename IdOf, typename IndexOf>
		std::vector<uint32_t> LinkParents(std::span<const UUID> parents, std::span<const uint32_t> invalidParents, IdOf&& idOf, IndexOf&& indexOf,
			std::vector<std::string>* outWarnings, const EntityReadProgress& progress, float progressStart, float progressShare)
		{
			const size_t count = parents.size();
			std::vector<uint32_t> linked(count, c_NotLinked);
			std::vector<uint32_t> connected(count);
			std::iota(connected.begin(), connected.end(), 0u);
			auto findRepresentative = [&connected](uint32_t index)
			{
				uint32_t representative = index;
				while (connected[representative] != representative)
					representative = connected[representative];
				while (connected[index] != representative) // Path compression
					index = std::exchange(connected[index], representative);
				return representative;
			};

			size_t nextInvalid = 0;
			for (uint32_t index = 0; index < count; index++)
			{
				ReportFinish(progress, progressStart, progressShare, index, count);
				if (nextInvalid < invalidParents.size() && invalidParents[nextInvalid] == index)
				{
					nextInvalid++;
					if (outWarnings)
						outWarnings->push_back(fmt::format("Entity {}: invalid 'Parent'; attached at the top level", idOf(index).ToString()));
					continue;
				}
				const UUID parent = parents[index];
				if (!parent.IsValid())
					continue;
				const uint32_t parentIndex = indexOf(parent);
				if (parentIndex == c_NotLinked)
				{
					if (outWarnings)
						outWarnings->push_back(fmt::format("Entity {}: parent {} not found; attached at the top level", idOf(index).ToString(), parent.ToString()));
					continue;
				}
				const uint32_t childRepresentative = findRepresentative(index);
				const uint32_t parentRepresentative = findRepresentative(parentIndex);
				if (childRepresentative == parentRepresentative)
				{
					if (outWarnings)
						outWarnings->push_back(fmt::format("Entity {}: parent {} would create a cycle; attached at the top level", idOf(index).ToString(), parent.ToString()));
					continue;
				}
				connected[childRepresentative] = parentRepresentative;
				linked[index] = parentIndex;
			}
			return linked;
		}

		bool Fail(std::string* outError, std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		}

	}

	namespace Detail
	{

		void UnknownComponentTally::Add(const std::string& name)
		{
			auto [count, firstSeen] = Counts.try_emplace(name, 0);
			if (firstSeen)
				Names.push_back(name);
			count->second++;
		}

		void UnknownComponentTally::Merge(const UnknownComponentTally& other)
		{
			for (const std::string& name : other.Names)
			{
				auto [count, firstSeen] = Counts.try_emplace(name, 0);
				if (firstSeen)
					Names.push_back(name);
				count->second += other.Counts.at(name);
			}
		}

		void UnknownComponentTally::AppendWarnings(std::vector<std::string>& outWarnings) const
		{
			for (const std::string& name : Names)
			{
				const size_t count = Counts.at(name);
				outWarnings.push_back(fmt::format("Unknown component '{}' ({} {}) kept unchanged: no module of this build registers it", name, count,
					count == 1 ? "entity" : "entities"));
			}
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// EntityColumns
	////////////////////////////////////////////////////////////////////////////////

	EntityColumns::EntityColumns()
		: m_NameInfo(ComponentRegistry::Find<NameComponent>()), m_TransformInfo(ComponentRegistry::Find<TransformComponent>())
	{
	}

	void EntityColumns::Reserve(size_t entities)
	{
		m_Entities.reserve(entities);
	}

	std::span<const EntityColumns::ComponentRecord> EntityColumns::GetComponents(size_t index) const
	{
		const EntityRecord& entity = m_Entities[index];
		return std::span<const ComponentRecord>(m_Components.data() + entity.FirstComponent, entity.ComponentCount);
	}

	void* EntityColumns::GetValue(const ComponentRecord& component) const
	{
		return m_Arrays[component.Array]->At(component.Value);
	}

	uint32_t EntityColumns::FindArray(const ComponentInfo& info)
	{
		// A document's entities have a few component types: a search beats a map.
		for (uint32_t array = 0; array < m_ArrayInfos.size(); array++)
		{
			if (m_ArrayInfos[array] == &info)
				return array;
		}
		m_Arrays.push_back(info.CreateValueArray());
		m_ArrayInfos.push_back(&info);
		return static_cast<uint32_t>(m_Arrays.size() - 1);
	}

	void* EntityColumns::AddComponent(const ComponentInfo& info)
	{
		const uint32_t array = FindArray(info);
		const size_t value = m_Arrays[array]->AppendDefault();
		m_Components.push_back(ComponentRecord { &info, array, value });
		return m_Arrays[array]->At(value);
	}

	bool EntityColumns::Add(const nlohmann::json& entity, std::vector<std::string>* outWarnings, Detail::UnknownComponentTally& tally, std::string* outError)
	{
		if (!entity.is_object())
			return Fail(outError, "Entity entries must be objects");
		const std::optional<UUID> id = ReadEntityID(entity);
		if (!id)
			return Fail(outError, "Entity has an invalid 'ID'");

		EntityRecord record;
		record.ID = *id;
		if (const nlohmann::json* parentJson = JsonUtils::Find(entity, "Parent"))
		{
			if (const std::optional<UUID> parsed = UUIDFromJson(*parentJson))
				record.Parent = *parsed;
			else
				record.InvalidParent = true;
		}
		record.FirstComponent = static_cast<uint32_t>(m_Components.size());
		// Every entity has a Name and a Transform, as DeserializeEntities creates them; the JSON's are read into them.
		if (m_NameInfo)
			static_cast<NameComponent*>(AddComponent(*m_NameInfo))->Name = "Entity";
		if (m_TransformInfo)
			AddComponent(*m_TransformInfo);
		nlohmann::json unknown = nlohmann::json::object();
		ReadComponents(entity, *id, [&](const ComponentInfo& info) -> void*
		{
			for (size_t component = record.FirstComponent; component < m_Components.size(); component++)
			{
				if (m_Components[component].Info == &info)
					return GetValue(m_Components[component]);
			}
			return AddComponent(info);
		}, unknown, tally, outWarnings);
		record.ComponentCount = static_cast<uint32_t>(m_Components.size() - record.FirstComponent);
		if (!unknown.empty())
		{
			record.Unknown = static_cast<uint32_t>(m_Unknown.size());
			m_Unknown.push_back(std::move(unknown));
		}
		m_Entities.push_back(record);
		return true;
	}

	////////////////////////////////////////////////////////////////////////////////
	// SceneBuilder
	////////////////////////////////////////////////////////////////////////////////

	namespace
	{

		// The shares of SceneBuilder::Finish its steps take: the parents, the links, the notifications.
		constexpr float c_FinishParentsShare = 0.2f;
		constexpr float c_FinishLinksShare = 0.3f;

	}

	SceneBuilder::SceneBuilder(Scene& scene, std::vector<std::string>* outWarnings, const EntityReadProgress& linkProgress)
		: m_Scene(scene), m_Warnings(outWarnings), m_LinkProgress(linkProgress), m_WasEmpty(scene.GetEntityCount() == 0)
	{
	}

	void SceneBuilder::ReserveEntities(uint64_t count)
	{
		const size_t entities = static_cast<size_t>(std::min<uint64_t>(count, Scene::c_MaxEntities));
		m_Handles.reserve(entities);
		m_Parents.reserve(entities);
		m_Scene.m_EntityMap.reserve(entities);
	}

	void SceneBuilder::ReadField(std::string_view key, const nlohmann::json& value)
	{
		if (key == "Name")
		{
			m_Scene.SetName(value.is_string() ? value.get<std::string>() : std::string("Untitled"));
			m_HasName = true;
		}
		else if (key == "Settings" && value.is_object())
		{
			SceneSerializer::DeserializeSettings(value, m_Scene, m_Warnings ? &m_SettingsWarnings : nullptr);
		}
	}

	bool SceneBuilder::CreateEntity(UUID id, UUID parent, bool invalidParent, entt::entity& outHandle, std::string* outError)
	{
		if (m_Scene.m_EntityMap.contains(id))
			return Fail(outError, fmt::format("Duplicate entity ID {} in entity data", id.ToString()));
		// Created directly (not through CreateEntity), as DeserializeEntities does, so building large hierarchies stays linear:
		// they are linked once all exist.
		entt::registry& registry = m_Scene.m_Registry;
		outHandle = registry.create();
		registry.emplace<IDComponent>(outHandle, id);
		registry.emplace<RelationshipComponent>(outHandle);
		registry.emplace<WorldTransformComponent>(outHandle);
		m_Scene.m_EntityMap.emplace(id, outHandle);
		if (invalidParent)
			m_InvalidParents.push_back(static_cast<uint32_t>(m_Handles.size()));
		m_Handles.push_back(outHandle);
		m_Parents.push_back(invalidParent ? UUID::Null() : parent);
		return true;
	}

	bool SceneBuilder::ReadEntity(const nlohmann::json& entity, std::string* outError)
	{
		if (!m_WasEmpty)
			return Fail(outError, "Scenes can only be deserialized into an empty scene");
		if (!entity.is_object())
			return Fail(outError, "Entity entries must be objects");
		const std::optional<UUID> id = ReadEntityID(entity);
		if (!id)
			return Fail(outError, "Entity has an invalid 'ID'");

		UUID parent = UUID::Null();
		bool invalidParent = false;
		if (const nlohmann::json* parentJson = JsonUtils::Find(entity, "Parent"))
		{
			if (const std::optional<UUID> parsed = UUIDFromJson(*parentJson))
				parent = *parsed;
			else
				invalidParent = true;
		}
		entt::entity handle = entt::null;
		if (!CreateEntity(*id, parent, invalidParent, handle, outError))
			return false;
		entt::registry& registry = m_Scene.m_Registry;
		registry.emplace<NameComponent>(handle, "Entity");
		registry.emplace<TransformComponent>(handle);

		nlohmann::json unknown = nlohmann::json::object();
		ReadComponents(entity, *id, [&](const ComponentInfo& info) { return info.Add(registry, handle); }, unknown, m_Unknown,
			m_Warnings ? &m_EntityWarnings : nullptr);
		if (!unknown.empty())
			registry.emplace<UnknownComponentsComponent>(handle, UnknownComponentsComponent { std::move(unknown) });
		return true;
	}

	bool SceneBuilder::ReadPrepared(PreparedEntities& entities, std::string* outError)
	{
		if (!m_WasEmpty)
			return Fail(outError, "Scenes can only be deserialized into an empty scene");
		// What reading them reported comes first: an entity refused here (its ID taken) fails the whole read.
		if (m_Warnings)
		{
			for (std::string& warning : entities.Warnings)
				m_EntityWarnings.push_back(std::move(warning));
		}
		m_Unknown.Merge(entities.Unknown);

		entt::registry& registry = m_Scene.m_Registry;
		EntityColumns& columns = entities.Columns;
		for (size_t index = 0; index < columns.GetEntityCount(); index++)
		{
			const EntityColumns::EntityRecord& record = columns.GetEntity(index);
			entt::entity handle = entt::null;
			if (!CreateEntity(record.ID, record.Parent, record.InvalidParent, handle, outError))
				return false;
			// Name and Transform come first among the columns' components, as ReadEntity creates them first.
			for (const EntityColumns::ComponentRecord& component : columns.GetComponents(index))
				component.Info->EmplaceCopy(registry, handle, columns.GetValue(component));
			if (record.Unknown != EntityColumns::c_NoUnknownComponents)
				registry.emplace<UnknownComponentsComponent>(handle, UnknownComponentsComponent { std::move(columns.GetUnknown(record.Unknown)) });
		}
		return true;
	}

	bool SceneBuilder::Finish(std::string* outError)
	{
		if (!m_WasEmpty)
			return Fail(outError, "Scenes can only be deserialized into an empty scene");
		if (!m_HasName)
			m_Scene.SetName("Untitled");

		// Where each entity lies in the document by its EnTT index.
		entt::registry& registry = m_Scene.m_Registry;
		const size_t count = m_Handles.size();
		size_t indexSpan = 0;
		for (const entt::entity handle : m_Handles)
			indexSpan = std::max(indexSpan, static_cast<size_t>(entt::to_entity(handle)) + 1);
		std::vector<uint32_t> indexOfSlot(indexSpan, c_NotLinked);
		for (size_t index = 0; index < count; index++)
			indexOfSlot[entt::to_entity(m_Handles[index])] = static_cast<uint32_t>(index);

		std::vector<std::string> parentWarnings;
		const std::vector<uint32_t> linked = LinkParents(m_Parents, m_InvalidParents,
			[&](uint32_t index) { return registry.get<IDComponent>(m_Handles[index]).ID; },
			[&](UUID id)
			{
				const auto it = m_Scene.m_EntityMap.find(id);
				return it != m_Scene.m_EntityMap.end() ? indexOfSlot[entt::to_entity(it->second)] : c_NotLinked;
			},
			m_Warnings ? &parentWarnings : nullptr, m_LinkProgress, 0.0f, c_FinishParentsShare);
		m_Parents = {};
		m_InvalidParents = {};

		// The runtime links (HierarchyComponent) are built now that every entity exists: children may be listed before their
		// parents. Linking also fills the RelationshipComponents and the root list.
		for (const entt::entity handle : m_Handles)
			registry.emplace<HierarchyComponent>(handle);
		std::vector<entt::entity> rootHandles;
		for (size_t index = 0; index < count; index++)
		{
			ReportFinish(m_LinkProgress, c_FinishParentsShare, c_FinishLinksShare, index, count);
			const entt::entity parent = linked[index] != c_NotLinked ? m_Handles[linked[index]] : entt::null;
			if (parent == entt::null)
				rootHandles.push_back(m_Handles[index]);
			m_Scene.LinkLast(m_Handles[index], parent);
		}
		// Depths, activity and the (stale) world transforms of the new subtrees.
		m_Scene.FinishLinking(rootHandles);
		m_Scene.m_HierarchyVersion++;

		// Components were constructed with default values and filled in afterwards; now that data and hierarchy are complete,
		// systems hear of them (on_update) and see the final state.
		const std::vector<const ComponentInfo*>& infos = ComponentRegistry::GetAll();
		for (size_t index = 0; index < count; index++)
		{
			ReportFinish(m_LinkProgress, c_FinishParentsShare + c_FinishLinksShare, 1.0f - c_FinishParentsShare - c_FinishLinksShare, index, count);
			for (const ComponentInfo* info : infos)
			{
				if (info->Has(registry, m_Handles[index]))
					info->MarkModified(registry, m_Handles[index]);
			}
		}
		m_Handles = {};

		if (m_Warnings)
		{
			for (std::string& warning : m_SettingsWarnings)
				m_Warnings->push_back(std::move(warning));
			for (std::string& warning : m_EntityWarnings)
				m_Warnings->push_back(std::move(warning));
			m_Unknown.AppendWarnings(*m_Warnings);
			for (std::string& warning : parentWarnings)
				m_Warnings->push_back(std::move(warning));
		}
		if (m_LinkProgress)
			m_LinkProgress(1.0f);
		return true;
	}

	////////////////////////////////////////////////////////////////////////////////
	// EntityPrototypeBuilder
	////////////////////////////////////////////////////////////////////////////////

	EntityPrototypeBuilder::EntityPrototypeBuilder(std::vector<std::string>* outWarnings)
		: m_Warnings(outWarnings)
	{
	}

	void EntityPrototypeBuilder::ReserveEntities(uint64_t count)
	{
		const size_t entities = static_cast<size_t>(std::min<uint64_t>(count, Scene::c_MaxEntities));
		m_Columns.Reserve(entities);
		m_IndexByID.reserve(entities);
	}

	bool EntityPrototypeBuilder::ReadEntity(const nlohmann::json& entity, std::string* outError)
	{
		if (!m_Columns.Add(entity, m_Warnings ? &m_EntityWarnings : nullptr, m_Unknown, outError))
			return false;
		const uint32_t index = static_cast<uint32_t>(m_Columns.GetEntityCount() - 1);
		const UUID id = m_Columns.GetEntity(index).ID;
		if (!m_IndexByID.emplace(id, index).second)
			return Fail(outError, fmt::format("Duplicate entity ID {} in entity data", id.ToString()));
		return true;
	}

	bool EntityPrototypeBuilder::Finish([[maybe_unused]] std::string* outError)
	{
		ST_PROFILE_FUNCTION();
		const size_t count = m_Columns.GetEntityCount();
		std::vector<UUID> parents(count);
		std::vector<uint32_t> invalidParents;
		for (size_t index = 0; index < count; index++)
		{
			const EntityColumns::EntityRecord& entity = m_Columns.GetEntity(index);
			parents[index] = entity.Parent;
			if (entity.InvalidParent)
				invalidParents.push_back(static_cast<uint32_t>(index));
		}
		std::vector<std::string> parentWarnings;
		const std::vector<uint32_t> linked = LinkParents(parents, invalidParents,
			[&](uint32_t index) { return m_Columns.GetEntity(index).ID; },
			[&](UUID id)
			{
				const auto it = m_IndexByID.find(id);
				return it != m_IndexByID.end() ? it->second : c_NotLinked;
			},
			m_Warnings ? &parentWarnings : nullptr, {}, 0.0f, 0.0f);

		// Hierarchy order, as a scene that linked the entities in document order lists them: the top-level entities in document
		// order, each followed by its descendants, children in document order.
		std::vector<uint32_t> childOffsets(count + 1, 0);
		for (size_t index = 0; index < count; index++)
		{
			if (linked[index] != c_NotLinked)
				childOffsets[linked[index] + 1]++;
		}
		for (size_t index = 0; index < count; index++)
			childOffsets[index + 1] += childOffsets[index];
		std::vector<uint32_t> children(childOffsets[count]);
		std::vector<uint32_t> cursor(childOffsets.begin(), childOffsets.end() - 1);
		for (size_t index = 0; index < count; index++)
		{
			if (linked[index] != c_NotLinked)
				children[cursor[linked[index]]++] = static_cast<uint32_t>(index);
		}
		std::vector<uint32_t> order;
		order.reserve(count);
		std::vector<uint32_t> stack;
		for (size_t root = 0; root < count; root++)
		{
			if (linked[root] != c_NotLinked)
				continue;
			stack.push_back(static_cast<uint32_t>(root));
			while (!stack.empty())
			{
				const uint32_t entity = stack.back();
				stack.pop_back();
				order.push_back(entity);
				for (uint32_t child = childOffsets[entity + 1]; child > childOffsets[entity]; child--)
					stack.push_back(children[child - 1]);
			}
		}
		// Every entity is reached: the links form a forest (LinkParents refuses cycles).
		ST_CORE_ASSERT(order.size() == count, "The hierarchy order misses entities");
		std::vector<uint32_t> position(count, 0);
		for (size_t index = 0; index < count; index++)
			position[order[index]] = static_cast<uint32_t>(index);

		// Each entity's components in registration order, as EntityPrototype::Compile lists them.
		std::unordered_map<const ComponentInfo*, uint32_t> registrationIndex;
		const std::vector<const ComponentInfo*>& infos = ComponentRegistry::GetAll();
		for (uint32_t index = 0; index < infos.size(); index++)
			registrationIndex.emplace(infos[index], index);

		Ref<EntityPrototype> prototype = CreateRef<EntityPrototype>();
		EntityPrototype& built = *prototype;
		built.m_IDs.reserve(count);
		built.m_IndexByID.reserve(count);
		built.m_Parents.resize(count, EntityPrototype::c_NoParent);
		built.m_ComponentOffsets.resize(count + 1, 0);
		built.m_UnknownIndex.resize(count, EntityPrototype::c_NoUnknownComponents);
		std::vector<EntityColumns::ComponentRecord> components;
		for (size_t index = 0; index < count; index++)
		{
			const uint32_t source = order[index];
			const EntityColumns::EntityRecord& entity = m_Columns.GetEntity(source);
			built.m_IDs.push_back(entity.ID);
			built.m_IndexByID.emplace(entity.ID, static_cast<uint32_t>(index));
			if (linked[source] != c_NotLinked)
				built.m_Parents[index] = position[linked[source]];
			else
				built.m_TopLevel.push_back(static_cast<uint32_t>(index));

			const std::span<const EntityColumns::ComponentRecord> records = m_Columns.GetComponents(source);
			components.assign(records.begin(), records.end());
			std::sort(components.begin(), components.end(), [&registrationIndex](const EntityColumns::ComponentRecord& a, const EntityColumns::ComponentRecord& b)
			{
				return registrationIndex.at(a.Info) < registrationIndex.at(b.Info);
			});
			for (const EntityColumns::ComponentRecord& component : components)
			{
				built.m_Components.push_back(component.Info);
				built.m_Values.push_back(m_Columns.GetValue(component));
			}
			built.m_ComponentOffsets[index + 1] = static_cast<uint32_t>(built.m_Components.size());
			if (entity.Unknown != EntityColumns::c_NoUnknownComponents)
			{
				built.m_UnknownIndex[index] = static_cast<uint32_t>(built.m_UnknownComponents.size());
				built.m_UnknownComponents.push_back(std::move(m_Columns.GetUnknown(entity.Unknown)));
			}
		}
		// The values stay where they are: the arrays move over whole.
		for (Scope<ComponentValueArray>& values : m_Columns.GetArrays())
			built.m_Arrays.push_back(std::move(values));
		m_Columns.GetArrays().clear();
		built.FindReferences();
		built.ComputeMemoryUsage();

		if (m_Warnings)
		{
			for (std::string& warning : m_EntityWarnings)
				m_Warnings->push_back(std::move(warning));
			m_Unknown.AppendWarnings(*m_Warnings);
			for (std::string& warning : parentWarnings)
				m_Warnings->push_back(std::move(warning));
		}
		m_Prototype = std::move(prototype);
		return true;
	}

}
