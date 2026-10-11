#include "stpch.h"
#include "Strata/Scene/EntityPrototype.h"

#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/Scene/UnknownComponents.h"

#include <algorithm>
#include <utility>

namespace Strata
{

	Ref<EntityPrototype> EntityPrototype::Compile(Ref<Scene> scene)
	{
		ST_PROFILE_FUNCTION();
		ST_CORE_ASSERT(scene, "EntityPrototype::Compile: no scene");

		Ref<EntityPrototype> prototype = CreateRef<EntityPrototype>();
		EntityPrototype& compiled = *prototype;
		entt::registry& registry = scene->GetRegistry();

		// The entities in hierarchy order, and where each one lies in it by its EnTT index (the private scene's identifiers
		// are dense, so a vector maps them).
		const std::vector<Entity> order = scene->GetEntitiesInHierarchyOrder();
		const size_t count = order.size();
		size_t indexSpan = 0;
		for (const Entity entity : order)
			indexSpan = std::max(indexSpan, static_cast<size_t>(entt::to_entity(entity.GetHandle())) + 1);
		std::vector<uint32_t> indexOf(indexSpan, c_NoParent);
		compiled.m_Entities.reserve(count);
		compiled.m_IDs.reserve(count);
		compiled.m_IndexByID.reserve(count);
		for (size_t index = 0; index < count; index++)
		{
			const entt::entity handle = order[index].GetHandle();
			indexOf[entt::to_entity(handle)] = static_cast<uint32_t>(index);
			compiled.m_Entities.push_back(handle);
			const UUID id = registry.get<IDComponent>(handle).ID;
			compiled.m_IDs.push_back(id);
			compiled.m_IndexByID.emplace(id, static_cast<uint32_t>(index));
		}
		const auto indexOfHandle = [&](entt::entity handle) { return indexOf[entt::to_entity(handle)]; };

		compiled.m_Parents.resize(count, c_NoParent);
		for (size_t index = 0; index < count; index++)
		{
			const entt::entity parent = registry.get<HierarchyComponent>(compiled.m_Entities[index]).Parent;
			if (parent == entt::null)
				compiled.m_TopLevel.push_back(static_cast<uint32_t>(index));
			else
				compiled.m_Parents[index] = indexOfHandle(parent);
		}

		// The registered components of every entity, in registration order (the outer loop), as offsets into one list. ID
		// and Relationship (the components the serializer keeps itself) are made anew by every instantiation.
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
					counts[indexOfHandle(handle)]++;
			}
		}
		compiled.m_ComponentOffsets.resize(count + 1, 0);
		for (size_t index = 0; index < count; index++)
			compiled.m_ComponentOffsets[index + 1] = compiled.m_ComponentOffsets[index] + counts[index];
		compiled.m_Components.resize(compiled.m_ComponentOffsets[count], nullptr);
		std::vector<uint32_t> cursor(compiled.m_ComponentOffsets.begin(), compiled.m_ComponentOffsets.end() - 1);
		for (const ComponentInfo* info : copied)
		{
			if (const entt::sparse_set* storage = registry.storage(info->TypeId))
			{
				for (const entt::entity handle : *storage)
					compiled.m_Components[cursor[indexOfHandle(handle)]++] = info;
			}
		}

		compiled.m_HasUnknownComponents.resize(count, 0);
		if (const auto* unknown = std::as_const(registry).storage<UnknownComponentsComponent>())
		{
			for (const entt::entity handle : static_cast<const entt::sparse_set&>(*unknown))
				compiled.m_HasUnknownComponents[indexOfHandle(handle)] = 1;
		}

		// References between the prototype's entities: entity-typed properties and script fields whose value names one.
		for (const ComponentInfo* info : copied)
		{
			std::vector<uint32_t> entityProperties;
			for (uint32_t property = 0; property < info->Properties.size(); property++)
			{
				if (info->Properties[property].Type == PropertyType::Entity)
					entityProperties.push_back(property);
			}
			const entt::sparse_set* storage = entityProperties.empty() ? nullptr : registry.storage(info->TypeId);
			if (!storage)
				continue;
			for (const entt::entity handle : *storage)
			{
				const void* component = info->Get(registry, handle);
				for (const uint32_t property : entityProperties)
				{
					const UUID target = std::get<UUID>(info->Properties[property].GetValue(component));
					if (const size_t targetIndex = prototype->FindEntity(target); targetIndex != c_NotFound)
						compiled.m_References.push_back({ indexOfHandle(handle), info, property, 0, static_cast<uint32_t>(targetIndex) });
				}
			}
		}
		if (const auto* scripts = std::as_const(registry).storage<ScriptComponent>())
		{
			for (const auto [handle, component] : scripts->each())
			{
				for (uint32_t script = 0; script < component.Scripts.size(); script++)
				{
					const std::vector<ScriptFieldValue>& fields = component.Scripts[script].Fields;
					for (uint32_t field = 0; field < fields.size(); field++)
					{
						if (fields[field].Type != PropertyType::Entity || !std::holds_alternative<UUID>(fields[field].Value))
							continue;
						if (const size_t targetIndex = prototype->FindEntity(std::get<UUID>(fields[field].Value)); targetIndex != c_NotFound)
							compiled.m_References.push_back({ indexOfHandle(handle), nullptr, script, field, static_cast<uint32_t>(targetIndex) });
					}
				}
			}
		}
		std::stable_sort(compiled.m_References.begin(), compiled.m_References.end(),
			[](const EntityReference& a, const EntityReference& b) { return a.Entity < b.Entity; });

		compiled.m_Scene = std::move(scene);
		compiled.m_MemoryUsage = compiled.m_Scene->EstimateMemoryUsage() + sizeof(EntityPrototype)
			+ compiled.m_Entities.capacity() * sizeof(entt::entity) + compiled.m_Parents.capacity() * sizeof(uint32_t)
			+ compiled.m_TopLevel.capacity() * sizeof(uint32_t) + compiled.m_IDs.capacity() * sizeof(UUID)
			+ compiled.m_IndexByID.size() * (sizeof(std::pair<const UUID, uint32_t>) + 2 * sizeof(void*)) + compiled.m_IndexByID.bucket_count() * 2 * sizeof(void*)
			+ compiled.m_ComponentOffsets.capacity() * sizeof(uint32_t) + compiled.m_Components.capacity() * sizeof(const ComponentInfo*)
			+ compiled.m_HasUnknownComponents.capacity() + compiled.m_References.capacity() * sizeof(EntityReference);
		return prototype;
	}

	Ref<EntityPrototype> EntityPrototype::FromSnapshot(const nlohmann::json& snapshot, std::string* outError, std::vector<std::string>* outWarnings)
	{
		// The snapshot's own IDs, which instances link to (PrefabInstanceComponent::PrefabEntityID).
		Ref<Scene> scene = CreateRef<Scene>("Prototype");
		EntityInstantiationOptions options;
		options.GenerateNewUUIDs = false;
		std::string error;
		SceneSerializer::DeserializeEntities(*scene, snapshot, options, &error, outWarnings);
		if (!error.empty())
		{
			if (outError)
				*outError = std::move(error);
			return nullptr;
		}
		return Compile(std::move(scene));
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

	nlohmann::json EntityPrototype::Serialize() const
	{
		std::vector<Entity> roots;
		roots.reserve(m_TopLevel.size());
		for (const uint32_t index : m_TopLevel)
			roots.emplace_back(m_Entities[index], m_Scene.get());
		return SceneSerializer::SerializeEntities(*m_Scene, roots);
	}

}
