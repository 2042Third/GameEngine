#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Core/UUID.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/Entity.h"

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class Scene;

	// Entity hierarchies compiled for copying into scenes: their entities in hierarchy order (parents before their children)
	// with what instantiation needs worked out once - each entity's parent in that order, its components (real objects of
	// their types, kept in one compact array per type, ComponentValueArray) and the entity references between its entities
	// that a copy with fresh IDs points at the copies. EntityTemplate (prefabs, models) compiles its snapshot into one on the
	// loading worker; additive scene loads compile the prepared scene they merge. Instances are made with
	// Scene::BeginInstantiation (see there), which copies the components (ComponentInfo::EmplaceCopy) rather than reading JSON.
	//
	// Immutable once compiled, so the loading worker that compiles it and the main thread that instantiates it share it
	// without locks.
	class EntityPrototype
	{
	public:
		static constexpr uint32_t c_NoParent = std::numeric_limits<uint32_t>::max();
		static constexpr size_t c_NotFound = std::numeric_limits<size_t>::max();

		EntityPrototype() = default;
		~EntityPrototype();

		EntityPrototype(const EntityPrototype&) = delete;
		EntityPrototype& operator=(const EntityPrototype&) = delete;

		// Compiles the entities of a scene (a copy: the scene can go afterwards). Any thread, while nothing changes the scene.
		static Ref<EntityPrototype> Compile(const Scene& scene);
		// Reads a snapshot ({"Entities": [...]}, SceneSerializer::SerializeEntities) and compiles it. Null, with the error,
		// when the entity data is invalid; what it could not read as written goes to outWarnings.
		static Ref<EntityPrototype> FromSnapshot(const nlohmann::json& snapshot, std::string* outError = nullptr,
			std::vector<std::string>* outWarnings = nullptr);

		size_t GetEntityCount() const { return m_IDs.size(); }
		// The index of each entity's parent in hierarchy order (c_NoParent for the top-level entities).
		std::span<const uint32_t> GetParentIndices() const { return m_Parents; }
		// The indices of the top-level entities, in order.
		std::span<const uint32_t> GetTopLevelIndices() const { return m_TopLevel; }
		// The ID an entity has in the prototype (in a prefab: its snapshot's ID, which instances link to through
		// PrefabInstanceComponent::PrefabEntityID).
		UUID GetEntityID(size_t index) const { return m_IDs[index]; }
		// The index of the entity with that prototype ID, or c_NotFound.
		size_t FindEntity(UUID id) const;
		// The registered components entity `index` has (registration order), apart from ID and Relationship, which every
		// entity has and instantiation makes anew; and their values (objects of the components' types).
		std::span<const ComponentInfo* const> GetComponents(size_t index) const;
		std::span<const void* const> GetComponentValues(size_t index) const;
		// The value of one component of an entity, or null when the entity lacks it.
		const void* FindComponent(size_t index, const ComponentInfo& info) const;
		template<typename T>
		const T* FindComponent(size_t index) const;
		// The components of entity `index` this build does not register, kept verbatim (UnknownComponentsComponent), or null.
		const nlohmann::json* GetUnknownComponents(size_t index) const;

		// The entities as a snapshot ({"Entities": [...]}, as SceneSerializer::SerializeEntities writes them).
		nlohmann::json Serialize() const;
		// About the system memory the prototype occupies (its value arrays and tables; not what components own on the heap).
		uint64_t GetMemoryUsage() const { return m_MemoryUsage; }
	private:
		friend class Scene;

		// An entity-typed property of a component (or a script field of entity type) whose value names an entity of the
		// prototype: a copy with fresh IDs points it at the copy of that entity.
		struct EntityReference
		{
			uint32_t Entity = 0;                     // Index of the entity holding the reference
			const ComponentInfo* Component = nullptr; // Null for script fields
			uint32_t Property = 0;                   // Property index, or for script fields: script index
			uint32_t Field = 0;                      // Script fields: field index in the script's overrides
			uint32_t Target = 0;                     // Index of the entity referenced
		};
		static constexpr uint32_t c_NoUnknownComponents = std::numeric_limits<uint32_t>::max();
	private:
		std::vector<uint32_t> m_Parents;
		std::vector<uint32_t> m_TopLevel;
		std::vector<UUID> m_IDs;
		std::unordered_map<UUID, uint32_t> m_IndexByID;
		// Per entity: its components and their values, from m_ComponentOffsets[index] to m_ComponentOffsets[index + 1].
		std::vector<uint32_t> m_ComponentOffsets;
		std::vector<const ComponentInfo*> m_Components;
		std::vector<const void*> m_Values;
		std::vector<Scope<ComponentValueArray>> m_Arrays; // Own the values
		// Per entity: the index of its unknown components in m_UnknownComponents, or c_NoUnknownComponents.
		std::vector<uint32_t> m_UnknownIndex;
		std::vector<nlohmann::json> m_UnknownComponents;
		std::vector<EntityReference> m_References; // By entity, in order
		uint64_t m_MemoryUsage = 0;
	};

	template<typename T>
	const T* EntityPrototype::FindComponent(size_t index) const
	{
		const ComponentInfo* info = ComponentRegistry::Find<T>();
		return info ? static_cast<const T*>(FindComponent(index, *info)) : nullptr;
	}

	// What Scene::BeginInstantiation copies a prototype as.
	struct InstantiationOptions
	{
		// Parent of the prototype's top-level entities (invalid: they become roots).
		Entity Parent;
		// When set, every created entity gets a PrefabInstanceComponent linking it to this asset and to its entity in the
		// prototype (GetEntityID).
		AssetHandle SourcePrefab = UUID::Null();
		// Create the entities with the prototype's IDs (an additive scene load: its entities keep their IDs) instead of fresh
		// ones; an ID the scene already has fails the instantiation.
		bool KeepIDs = false;
	};

	// An instantiation in progress (Scene::BeginInstantiation): the prototype's entities are created in hierarchy order, in
	// one call or in slices (Scene::ContinueInstantiation), each slice's entities complete and live once it returns.
	class PrototypeInstantiation
	{
	public:
		PrototypeInstantiation() = default;

		const Ref<EntityPrototype>& GetPrototype() const { return m_Prototype; }
		// Entities created so far, and whether every entity was.
		size_t GetCreatedCount() const { return m_Next; }
		bool IsComplete() const { return m_Prototype && m_Next == m_Prototype->GetEntityCount(); }
		// The created top-level entities, in order.
		const std::vector<Entity>& GetRoots() const { return m_Roots; }
		// The ID the prototype's entity `index` has (or gets) in the scene.
		UUID GetID(size_t index) const { return m_IDs[index]; }
	private:
		friend class Scene;

		Ref<EntityPrototype> m_Prototype;
		InstantiationOptions m_Options;
		bool m_HasParent = false; // A parent was given: its destruction stops the instantiation
		Scene* m_Scene = nullptr;
		std::vector<UUID> m_IDs;            // Per prototype entity: its ID in the scene
		std::vector<entt::entity> m_Handles; // Per prototype entity: its entity in the scene (entt::null until created)
		size_t m_Next = 0;
		std::vector<Entity> m_Roots;
	};

}
