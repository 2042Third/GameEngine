#include "stpch.h"
#include "Strata/Scene/EntityPrototype.h"

#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Scene/ComponentAccess.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/Scene/UnknownComponents.h"

#include <algorithm>
#include <utility>

namespace Strata
{

	EntityPrototype::~EntityPrototype() = default;

	Ref<EntityPrototype> EntityPrototype::Compile(const Scene& scene)
	{
		ST_PROFILE_FUNCTION();
		Ref<EntityPrototype> prototype = CreateRef<EntityPrototype>();
		EntityPrototype& compiled = *prototype;
		const entt::registry& registry = scene.GetRegistry();

		// The entities in hierarchy order, and where each one lies in it by its EnTT index (entities of the registry that are
		// not the scene's have none).
		const std::vector<Entity> order = scene.GetEntitiesInHierarchyOrder();
		const size_t count = order.size();
		size_t indexSpan = 0;
		for (const Entity entity : order)
			indexSpan = std::max(indexSpan, static_cast<size_t>(entt::to_entity(entity.GetHandle())) + 1);
		std::vector<uint32_t> indexOf(indexSpan, c_NoParent);
		compiled.m_IDs.reserve(count);
		compiled.m_IndexByID.reserve(count);
		for (size_t index = 0; index < count; index++)
		{
			const entt::entity handle = order[index].GetHandle();
			indexOf[entt::to_entity(handle)] = static_cast<uint32_t>(index);
			const UUID id = registry.get<IDComponent>(handle).ID;
			compiled.m_IDs.push_back(id);
			compiled.m_IndexByID.emplace(id, static_cast<uint32_t>(index));
		}
		const auto indexOfHandle = [&](entt::entity handle)
		{
			const size_t slot = static_cast<size_t>(entt::to_entity(handle));
			return slot < indexOf.size() ? indexOf[slot] : c_NoParent;
		};

		compiled.m_Parents.resize(count, c_NoParent);
		for (size_t index = 0; index < count; index++)
		{
			const entt::entity parent = registry.get<HierarchyComponent>(order[index].GetHandle()).Parent;
			if (parent == entt::null)
				compiled.m_TopLevel.push_back(static_cast<uint32_t>(index));
			else
				compiled.m_Parents[index] = indexOfHandle(parent);
		}

		// The registered components of every entity, in registration order (the outer loop), as offsets into one list, each
		// type's values copied into one array. ID and Relationship (the components the serializer keeps itself) are made
		// anew by every instantiation.
		std::vector<const ComponentInfo*> copied;
		for (const ComponentInfo* info : ComponentRegistry::GetAll())
		{
			if (!HasFlag(info->Flags, ComponentFlags::NoSerialize))
				copied.push_back(info);
		}
		std::vector<uint32_t> counts(count + 1, 0);
		for (const ComponentInfo* info : copied)
		{
			if (const entt::sparse_set* storage = registry.storage(info->TypeId))
			{
				for (const entt::entity handle : *storage)
				{
					if (const uint32_t index = indexOfHandle(handle); index != c_NoParent)
						counts[index]++;
				}
			}
		}
		compiled.m_ComponentOffsets.resize(count + 1, 0);
		for (size_t index = 0; index < count; index++)
			compiled.m_ComponentOffsets[index + 1] = compiled.m_ComponentOffsets[index] + counts[index];
		compiled.m_Components.resize(compiled.m_ComponentOffsets[count], nullptr);
		compiled.m_Values.resize(compiled.m_ComponentOffsets[count], nullptr);
		std::vector<uint32_t> cursor(compiled.m_ComponentOffsets.begin(), compiled.m_ComponentOffsets.end() - 1);
		for (const ComponentInfo* info : copied)
		{
			const entt::sparse_set* storage = registry.storage(info->TypeId);
			if (!storage || storage->empty())
				continue;
			// Room for every value first: their addresses never change.
			Scope<ComponentValueArray> values = info->CreateValueArray();
			values->Reserve(storage->size());
			for (const entt::entity handle : *storage)
			{
				const uint32_t index = indexOfHandle(handle);
				if (index == c_NoParent)
					continue;
				const uint32_t slot = cursor[index]++;
				compiled.m_Components[slot] = info;
				compiled.m_Values[slot] = values->Append(storage->value(handle));
			}
			compiled.m_Arrays.push_back(std::move(values));
		}

		compiled.m_UnknownIndex.resize(count, c_NoUnknownComponents);
		if (const auto* unknown = registry.storage<UnknownComponentsComponent>())
		{
			for (const auto [handle, component] : unknown->each())
			{
				const uint32_t index = indexOfHandle(handle);
				if (index == c_NoParent)
					continue;
				compiled.m_UnknownIndex[index] = static_cast<uint32_t>(compiled.m_UnknownComponents.size());
				compiled.m_UnknownComponents.push_back(component.Components);
			}
		}

		// References between the prototype's entities: entity-typed properties and script fields whose value names one.
		const entt::id_type scriptType = entt::type_id<ScriptComponent>().hash();
		for (size_t index = 0; index < count; index++)
		{
			const std::span<const ComponentInfo* const> components = prototype->GetComponents(index);
			const std::span<const void* const> values = prototype->GetComponentValues(index);
			for (size_t slot = 0; slot < components.size(); slot++)
			{
				const ComponentInfo& info = *components[slot];
				for (uint32_t property = 0; property < info.Properties.size(); property++)
				{
					if (info.Properties[property].Type != PropertyType::Entity)
						continue;
					const UUID target = std::get<UUID>(info.Properties[property].GetValue(values[slot]));
					if (const size_t targetIndex = prototype->FindEntity(target); targetIndex != c_NotFound)
						compiled.m_References.push_back({ static_cast<uint32_t>(index), &info, property, 0, static_cast<uint32_t>(targetIndex) });
				}
				if (info.TypeId != scriptType)
					continue;
				const ScriptComponent& scripts = *static_cast<const ScriptComponent*>(values[slot]);
				for (uint32_t script = 0; script < scripts.Scripts.size(); script++)
				{
					const std::vector<ScriptFieldValue>& fields = scripts.Scripts[script].Fields;
					for (uint32_t field = 0; field < fields.size(); field++)
					{
						if (fields[field].Type != PropertyType::Entity || !std::holds_alternative<UUID>(fields[field].Value))
							continue;
						if (const size_t targetIndex = prototype->FindEntity(std::get<UUID>(fields[field].Value)); targetIndex != c_NotFound)
							compiled.m_References.push_back({ static_cast<uint32_t>(index), nullptr, script, field, static_cast<uint32_t>(targetIndex) });
					}
				}
			}
		}

		uint64_t memory = sizeof(EntityPrototype) + compiled.m_Parents.capacity() * sizeof(uint32_t) + compiled.m_TopLevel.capacity() * sizeof(uint32_t)
			+ compiled.m_IDs.capacity() * sizeof(UUID)
			+ compiled.m_IndexByID.size() * (sizeof(std::pair<const UUID, uint32_t>) + 2 * sizeof(void*)) + compiled.m_IndexByID.bucket_count() * 2 * sizeof(void*)
			+ compiled.m_ComponentOffsets.capacity() * sizeof(uint32_t) + compiled.m_Components.capacity() * sizeof(const ComponentInfo*)
			+ compiled.m_Values.capacity() * sizeof(const void*) + compiled.m_UnknownIndex.capacity() * sizeof(uint32_t)
			+ compiled.m_References.capacity() * sizeof(EntityReference);
		for (const Scope<ComponentValueArray>& values : compiled.m_Arrays)
			memory += values->GetMemoryUsage();
		compiled.m_MemoryUsage = memory;
		return prototype;
	}

	Ref<EntityPrototype> EntityPrototype::FromSnapshot(const nlohmann::json& snapshot, std::string* outError, std::vector<std::string>* outWarnings)
	{
		// Read into a scene of its own (which validates the data) and compiled from it, with the snapshot's own IDs, which
		// instances link to (PrefabInstanceComponent::PrefabEntityID).
		Scene scene("Prototype");
		EntityInstantiationOptions options;
		options.GenerateNewUUIDs = false;
		std::string error;
		SceneSerializer::DeserializeEntities(scene, snapshot, options, &error, outWarnings);
		if (!error.empty())
		{
			if (outError)
				*outError = std::move(error);
			return nullptr;
		}
		return Compile(scene);
	}

	size_t EntityPrototype::FindEntity(UUID id) const
	{
		auto it = m_IndexByID.find(id);
		return it != m_IndexByID.end() ? it->second : c_NotFound;
	}

	std::span<const ComponentInfo* const> EntityPrototype::GetComponents(size_t index) const
	{
		return std::span<const ComponentInfo* const>(m_Components.data() + m_ComponentOffsets[index], m_ComponentOffsets[index + 1] - m_ComponentOffsets[index]);
	}

	std::span<const void* const> EntityPrototype::GetComponentValues(size_t index) const
	{
		return std::span<const void* const>(m_Values.data() + m_ComponentOffsets[index], m_ComponentOffsets[index + 1] - m_ComponentOffsets[index]);
	}

	const void* EntityPrototype::FindComponent(size_t index, const ComponentInfo& info) const
	{
		for (uint32_t slot = m_ComponentOffsets[index]; slot < m_ComponentOffsets[index + 1]; slot++)
		{
			if (m_Components[slot] == &info)
				return m_Values[slot];
		}
		return nullptr;
	}

	const nlohmann::json* EntityPrototype::GetUnknownComponents(size_t index) const
	{
		return m_UnknownIndex[index] == c_NoUnknownComponents ? nullptr : &m_UnknownComponents[m_UnknownIndex[index]];
	}

	nlohmann::json EntityPrototype::Serialize() const
	{
		// As SceneSerializer::SerializeEntities writes the top-level entities with their descendants: in hierarchy order,
		// parents within the snapshot only.
		nlohmann::json entities = nlohmann::json::array();
		for (size_t index = 0; index < m_IDs.size(); index++)
		{
			nlohmann::json entity;
			entity["ID"] = UUIDToJson(m_IDs[index]);
			if (m_Parents[index] != c_NoParent)
				entity["Parent"] = UUIDToJson(m_IDs[m_Parents[index]]);
			nlohmann::json components = nlohmann::json::object();
			const std::span<const ComponentInfo* const> infos = GetComponents(index);
			const std::span<const void* const> values = GetComponentValues(index);
			for (size_t slot = 0; slot < infos.size(); slot++)
				components[infos[slot]->Name] = ComponentAccess::Serialize(*infos[slot], values[slot]);
			if (const nlohmann::json* unknown = GetUnknownComponents(index))
				UnknownComponentsComponent { *unknown }.AppendTo(components);
			entity["Components"] = std::move(components);
			entities.push_back(std::move(entity));
		}
		return nlohmann::json { { "Entities", std::move(entities) } };
	}

}
