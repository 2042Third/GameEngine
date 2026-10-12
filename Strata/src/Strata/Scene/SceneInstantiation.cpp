#include "stpch.h"
#include "Strata/Scene/Scene.h"

#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/EntityPrototype.h"
#include "Strata/Scene/UnknownComponents.h"

#include <algorithm>

// Copying compiled prototypes (EntityPrototype) into scenes: prefab and model instances, additive scene loads.

namespace Strata
{

	namespace
	{

		// The registered components every instantiation handles itself (not copied from the prototype).
		struct CoreComponentInfos
		{
			const ComponentInfo* ID = nullptr;
			const ComponentInfo* Relationship = nullptr;
			const ComponentInfo* Transform = nullptr;
			const ComponentInfo* PrefabInstance = nullptr;
		};

		const CoreComponentInfos& GetCoreComponentInfos()
		{
			// The registry is frozen before any scene exists, so the infos never change.
			static const CoreComponentInfos s_Infos = {
				ComponentRegistry::Find<IDComponent>(),
				ComponentRegistry::Find<RelationshipComponent>(),
				ComponentRegistry::Find<TransformComponent>(),
				ComponentRegistry::Find<PrefabInstanceComponent>() };
			return s_Infos;
		}

	}

	bool Scene::BeginInstantiation(const Ref<EntityPrototype>& prototype, const InstantiationOptions& options, PrototypeInstantiation& outInstantiation,
		std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		// A failed start leaves nothing to continue (the object may be reused, its tables keep their capacity).
		outInstantiation.m_Prototype.reset();
		outInstantiation.m_Scene = nullptr;
		if (!prototype)
			return fail("No prototype to instantiate");
		// An invalid parent means none (the top-level entities become roots), as for DeserializeEntities.
		if (options.Parent && options.Parent.GetScene() != this)
			return fail("The parent entity belongs to a different scene");

		const size_t count = prototype->GetEntityCount();
		const size_t existing = std::min(GetRegistryEntityCount(), c_MaxEntities);
		if (count > c_MaxEntities - existing)
			return fail(fmt::format("Cannot add {} entities to a scene with {}: a scene holds at most {} entities", count, existing, c_MaxEntities));

		outInstantiation.m_IDs.resize(count);
		if (options.KeepIDs)
		{
			for (size_t index = 0; index < count; index++)
			{
				const UUID id = prototype->GetEntityID(index);
				if (m_EntityMap.contains(id))
					return fail(fmt::format("The scene already has an entity with ID {} ('{}'); nothing was added", id.ToString(), GetEntityByUUID(id).GetName()));
				outInstantiation.m_IDs[index] = id;
			}
		}
		else
		{
			for (size_t index = 0; index < count; index++)
				outInstantiation.m_IDs[index] = UUID();
		}

		outInstantiation.m_Prototype = prototype;
		outInstantiation.m_Options = options;
		outInstantiation.m_HasParent = static_cast<bool>(options.Parent);
		if (!outInstantiation.m_HasParent)
			outInstantiation.m_Options.Parent = Entity();
		outInstantiation.m_Scene = this;
		outInstantiation.m_Handles.assign(count, entt::null);
		outInstantiation.m_Next = 0;
		outInstantiation.m_Skipped = 0;
		outInstantiation.m_Roots.clear();
		return true;
	}

	size_t Scene::ContinueInstantiation(PrototypeInstantiation& instantiation, size_t maxEntities, std::string* outError, const TransformComponent* rootTransform)
	{
		ST_PROFILE_FUNCTION();
		auto fail = [outError](std::string message) -> size_t
		{
			if (outError)
				*outError = std::move(message);
			return 0;
		};

		if (!instantiation.m_Prototype || instantiation.m_Scene != this)
			return fail("The instantiation was not begun for this scene");
		const EntityPrototype& prototype = *instantiation.m_Prototype;
		const InstantiationOptions& options = instantiation.m_Options;
		const size_t begin = instantiation.m_Next;
		const size_t end = std::min(prototype.GetEntityCount(), begin + std::min(maxEntities, prototype.GetEntityCount()));
		if (begin >= end)
			return 0;

		// Everything that can stop this slice is checked before anything is created.
		if (instantiation.m_HasParent && !options.Parent.IsValid())
			return fail("The parent entity was destroyed");

		// Entities whose parent is gone are not created: one created in an earlier slice may have been destroyed since (by
		// gameplay, the editor, or an unload it was moved under), and destroying an entity destroys its descendants, so they
		// go with it, as do the descendants of those not created. Parents come before their children, so each entity's
		// parent was decided already: created in an earlier slice (and alive or not), or in this one (or skipped). The first
		// slice (every Instantiate) has no earlier one, so it creates everything it covers.
		const std::span<const uint32_t> parents = prototype.GetParentIndices();
		std::vector<uint8_t>& skipped = instantiation.m_SliceSkipped;
		skipped.clear();
		size_t count = end - begin;
		if (begin > 0)
		{
			skipped.assign(end - begin, 0);
			for (size_t index = begin; index < end; index++)
			{
				const uint32_t parentIndex = parents[index];
				if (parentIndex == EntityPrototype::c_NoParent)
					continue;
				if (parentIndex >= begin ? skipped[parentIndex - begin] != 0 : !IsInstantiatedEntityAlive(instantiation, parentIndex))
				{
					skipped[index - begin] = 1;
					count--;
				}
			}
		}
		const auto isSkipped = [&skipped, begin](size_t index) { return !skipped.empty() && skipped[index - begin] != 0; };

		const size_t existing = std::min(GetRegistryEntityCount(), c_MaxEntities);
		if (count > c_MaxEntities - existing)
			return fail(fmt::format("Cannot add {} entities to a scene with {}: a scene holds at most {} entities", count, existing, c_MaxEntities));
		if (options.KeepIDs)
		{
			for (size_t index = begin; index < end; index++)
			{
				if (!isSkipped(index) && m_EntityMap.contains(instantiation.m_IDs[index]))
					return fail(fmt::format("The scene already has an entity with ID {}", instantiation.m_IDs[index].ToString()));
			}
		}

		const CoreComponentInfos& core = GetCoreComponentInfos();
		const entt::entity parentHandle = options.Parent ? options.Parent.GetHandle() : entt::null;
		const size_t firstRoot = prototype.GetTopLevelIndices().empty() ? EntityPrototype::c_NotFound : prototype.GetTopLevelIndices().front();

		// Entities and their components, created as DeserializeEntities creates them: identity, relationship and world
		// transform first, then the prototype's components (each emits on_construct with its final value). The hierarchy
		// node comes once the batch exists, so that component signals do not see half-linked entities.
		for (size_t index = begin; index < end; index++)
		{
			if (isSkipped(index))
				continue;
			UUID id = instantiation.m_IDs[index];
			// A fresh ID that happens to exist already is drawn again (IDs kept were checked above).
			while (!options.KeepIDs && m_EntityMap.contains(id))
				id = UUID();
			instantiation.m_IDs[index] = id;

			const entt::entity handle = m_Registry.create();
			m_Registry.emplace<IDComponent>(handle, id);
			m_Registry.emplace<RelationshipComponent>(handle);
			m_Registry.emplace<WorldTransformComponent>(handle);
			const std::span<const ComponentInfo* const> components = prototype.GetComponents(index);
			const std::span<const void* const> values = prototype.GetComponentValues(index);
			for (size_t slot = 0; slot < components.size(); slot++)
			{
				const ComponentInfo* info = components[slot];
				if (rootTransform && info == core.Transform && index == firstRoot)
					m_Registry.emplace_or_replace<TransformComponent>(handle, *rootTransform);
				else
					info->EmplaceCopy(m_Registry, handle, values[slot]);
			}
			if (const nlohmann::json* unknown = prototype.GetUnknownComponents(index))
				m_Registry.emplace<UnknownComponentsComponent>(handle, UnknownComponentsComponent { *unknown });
			m_EntityMap.emplace(id, handle);
			instantiation.m_Handles[index] = handle;
		}

		// The hierarchy: each entity under its parent (created before it: the order is the hierarchy's), the top-level ones
		// under the options' parent. Subtrees whose parent existed before this slice get their depth, activity and world
		// transforms from it.
		std::vector<entt::entity> subtreeRoots;
		for (size_t index = begin; index < end; index++)
		{
			if (isSkipped(index))
				continue;
			const entt::entity handle = instantiation.m_Handles[index];
			m_Registry.emplace<HierarchyComponent>(handle);
			const uint32_t parentIndex = parents[index];
			if (parentIndex == EntityPrototype::c_NoParent)
			{
				LinkLast(handle, parentHandle);
				instantiation.m_Roots.emplace_back(handle, this);
				subtreeRoots.push_back(handle);
			}
			else
			{
				LinkLast(handle, instantiation.m_Handles[parentIndex]);
				if (parentIndex < begin)
					subtreeRoots.push_back(handle);
			}
		}
		FinishLinking(subtreeRoots);
		m_HierarchyVersion++;

		// References among the prototype's entities name the copies (the IDs the scene gives them); kept IDs need nothing.
		if (!options.KeepIDs)
		{
			const auto& references = prototype.m_References;
			auto reference = std::lower_bound(references.begin(), references.end(), static_cast<uint32_t>(begin),
				[](const EntityPrototype::EntityReference& entry, uint32_t entity) { return entry.Entity < entity; });
			for (; reference != references.end() && reference->Entity < end; ++reference)
			{
				if (isSkipped(reference->Entity))
					continue;
				const entt::entity handle = instantiation.m_Handles[reference->Entity];
				const UUID target = instantiation.m_IDs[reference->Target];
				if (reference->Component)
				{
					void* component = reference->Component->Get(m_Registry, handle);
					reference->Component->Properties[reference->Property].Setter(component, target);
				}
				else
				{
					m_Registry.get<ScriptComponent>(handle).Scripts[reference->Property].Fields[reference->Field].Value = target;
				}
			}
		}

		const bool linkToPrefab = options.SourcePrefab.IsValid();
		for (size_t index = begin; index < end; index++)
		{
			if (linkToPrefab && !isSkipped(index))
				m_Registry.emplace_or_replace<PrefabInstanceComponent>(instantiation.m_Handles[index], PrefabInstanceComponent { options.SourcePrefab, prototype.GetEntityID(index) });
		}

		// Components were constructed before their entities were linked and their references remapped: every component of
		// every created entity now emits on_update (in registration order), so systems see the final state, e.g. when spawning
		// during play. The components each entity has are known, so nothing is looked up.
		const std::vector<const ComponentInfo*>& registered = ComponentRegistry::GetAll();
		for (size_t index = begin; index < end; index++)
		{
			if (isSkipped(index))
				continue;
			const entt::entity handle = instantiation.m_Handles[index];
			const std::span<const ComponentInfo* const> components = prototype.GetComponents(index);
			size_t next = 0;
			for (const ComponentInfo* info : registered)
			{
				const bool copied = next < components.size() && components[next] == info;
				if (copied)
					next++;
				if (copied || info == core.ID || info == core.Relationship || (linkToPrefab && info == core.PrefabInstance))
					info->MarkModified(m_Registry, handle);
			}
		}

		instantiation.m_Next = end;
		instantiation.m_Skipped += (end - begin) - count;
		return end - begin;
	}

	bool Scene::IsInstantiatedEntityAlive(const PrototypeInstantiation& instantiation, size_t index) const
	{
		const entt::entity handle = instantiation.m_Handles[index];
		if (!m_Registry.valid(handle))
			return false;
		// A handle whose slot was reused as often as its version wraps around names another entity: its ID tells.
		const IDComponent* id = m_Registry.try_get<IDComponent>(handle);
		return id && id->ID == instantiation.m_IDs[index];
	}

	std::vector<Entity> Scene::Instantiate(const Ref<EntityPrototype>& prototype, const InstantiationOptions& options, std::string* outError)
	{
		PrototypeInstantiation instantiation;
		if (!BeginInstantiation(prototype, options, instantiation, outError))
			return {};
		if (prototype->GetEntityCount() > 0 && ContinueInstantiation(instantiation, prototype->GetEntityCount(), outError) == 0)
			return {};
		return instantiation.GetRoots();
	}

}
