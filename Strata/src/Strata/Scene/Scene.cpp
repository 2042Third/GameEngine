#include "stpch.h"
#include "Strata/Scene/Scene.h"

#include "Strata/Core/JobSystem.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneSerializer.h"

#include <limits>

namespace Strata
{

	namespace
	{

		// Width of the set of independent dirty subtrees above which world transforms are recomputed on the job system (each
		// job walks whole subtrees). Narrower sets are processed level by level until they are wide enough or done.
		constexpr size_t c_ParallelTransformWidth = 1024;
		// Relative tolerance of ValidateWorldTransforms.
		constexpr float c_TransformValidationTolerance = 1.0e-5f;

		uint64_t HashLookupValue(std::string_view value)
		{
			return static_cast<uint64_t>(std::hash<std::string_view>()(value));
		}

		// Equal values (matching infinities included) and two NaNs agree: transforms that overflow compute the same
		// non-finite elements in the cache and in a recomputation. Other finite values agree within the relative tolerance;
		// an infinity never agrees with a finite value (its relative tolerance would accept any).
		bool IsNearlyEqual(float x, float y)
		{
			if (x == y || (std::isnan(x) && std::isnan(y)))
				return true;
			if (!std::isfinite(x) || !std::isfinite(y))
				return false;
			return std::abs(x - y) <= c_TransformValidationTolerance * std::max(1.0f, std::abs(y));
		}

		bool IsNearlyEqualMatrix(const glm::mat4& a, const glm::mat4& b)
		{
			for (int column = 0; column < 4; column++)
			{
				for (int row = 0; row < 4; row++)
				{
					if (!IsNearlyEqual(a[column][row], b[column][row]))
						return false;
				}
			}
			return true;
		}

	}

	Scene::Scene(std::string name)
		: m_Name(std::move(name))
	{
		m_Connections.emplace_back(m_Registry.on_construct<TransformComponent>().connect<&Scene::OnTransformChanged>(*this));
		m_Connections.emplace_back(m_Registry.on_update<TransformComponent>().connect<&Scene::OnTransformChanged>(*this));
		m_Connections.emplace_back(m_Registry.on_construct<InactiveComponent>().connect<&Scene::OnInactiveAdded>(*this));
		m_Connections.emplace_back(m_Registry.on_destroy<InactiveComponent>().connect<&Scene::OnInactiveRemoved>(*this));
		m_Connections.emplace_back(m_Registry.on_construct<NameComponent>().connect<&Scene::OnNameChanged>(*this));
		m_Connections.emplace_back(m_Registry.on_update<NameComponent>().connect<&Scene::OnNameChanged>(*this));
		m_Connections.emplace_back(m_Registry.on_destroy<NameComponent>().connect<&Scene::OnNameRemoved>(*this));
		m_Connections.emplace_back(m_Registry.on_construct<TagComponent>().connect<&Scene::OnTagChanged>(*this));
		m_Connections.emplace_back(m_Registry.on_update<TagComponent>().connect<&Scene::OnTagChanged>(*this));
		m_Connections.emplace_back(m_Registry.on_destroy<TagComponent>().connect<&Scene::OnTagRemoved>(*this));
	}

	Scene::~Scene()
	{
		OnRuntimeStop();
		m_Connections.clear();
	}

	Ref<Scene> Scene::Copy(const Ref<Scene>& source)
	{
		Ref<Scene> destination = CreateRef<Scene>(source->m_Name);
		destination->m_Settings = source->m_Settings;

		// The hierarchy is rebuilt from the links instead of being copied with the components: entities are copied parents
		// first, so appending each one to its parent reproduces every sibling order.
		const entt::id_type relationshipType = entt::type_id<RelationshipComponent>().hash();
		const std::vector<const ComponentInfo*>& components = ComponentRegistry::GetAll();
		std::vector<entt::entity> order;
		source->CollectHierarchyOrder(order);
		for (const entt::entity sourceHandle : order)
		{
			const entt::entity handle = destination->m_Registry.create();
			for (const ComponentInfo* info : components)
			{
				if (info->IsCopyable() && info->TypeId != relationshipType)
					info->Copy(destination->m_Registry, handle, source->m_Registry, sourceHandle);
			}
			destination->m_Registry.emplace<RelationshipComponent>(handle);
			destination->m_Registry.emplace<HierarchyComponent>(handle);
			// The cached world transforms and activity are exact wherever the source's are (the same local transforms and
			// hierarchy); what is stale there is stale here.
			const WorldTransformComponent* worldTransform = source->m_Registry.try_get<WorldTransformComponent>(sourceHandle);
			destination->m_Registry.emplace<WorldTransformComponent>(handle, worldTransform ? *worldTransform : WorldTransformComponent());
			const UUID id = source->m_Registry.get<IDComponent>(sourceHandle).ID;
			destination->m_EntityMap.emplace(id, handle);

			const HierarchyComponent& sourceNode = source->GetHierarchy(sourceHandle);
			entt::entity parent = entt::null;
			if (sourceNode.Parent != entt::null)
			{
				auto parentIt = destination->m_EntityMap.find(source->m_Registry.get<IDComponent>(sourceNode.Parent).ID);
				ST_CORE_ASSERT(parentIt != destination->m_EntityMap.end(), "Scene::Copy: a parent was not copied before its child");
				if (parentIt != destination->m_EntityMap.end())
					parent = parentIt->second;
			}
			destination->LinkLast(handle, parent);
			destination->GetHierarchy(handle).Depth = parent == entt::null ? 0 : destination->GetHierarchy(parent).Depth + 1;
			if (sourceNode.TransformDirty || !worldTransform)
				destination->MarkTransformDirty(handle);
		}
		destination->m_HierarchyVersion++;
		return destination;
	}

	Entity Scene::CreateEntity(const std::string& name)
	{
		return CreateEntityWithUUID(UUID(), name);
	}

	Entity Scene::CreateEntityWithUUID(UUID uuid, const std::string& name)
	{
		if (GetRegistryEntityCount() >= c_MaxEntities)
		{
			ST_CORE_ERROR("Scene '{}': cannot create entity '{}': the scene already has the maximum of {} entities", m_Name, name, c_MaxEntities);
			return {};
		}

		if (!uuid.IsValid() || m_EntityMap.find(uuid) != m_EntityMap.end())
		{
			if (uuid.IsValid())
				ST_CORE_ERROR("Scene '{}': entity UUID {} already exists; assigning a new UUID", m_Name, uuid.ToString());
			uuid = UUID();
		}

		// The hierarchy node comes first: the signals of the other components rely on it.
		const entt::entity handle = m_Registry.create();
		m_Registry.emplace<HierarchyComponent>(handle);
		Entity entity(handle, this);
		entity.AddComponent<IDComponent>(uuid);
		entity.AddComponent<NameComponent>(name.empty() ? std::string("Entity") : name);
		entity.AddComponent<TransformComponent>();
		entity.AddComponent<RelationshipComponent>();
		entity.AddComponent<WorldTransformComponent>();

		m_EntityMap.emplace(uuid, handle);
		LinkLast(handle, entt::null);
		m_HierarchyVersion++;
		return entity;
	}

	size_t Scene::GetRegistryEntityCount() const
	{
		// EnTT keeps the live identifiers of its entity storage in front of the free list.
		return m_Registry.storage<entt::entity>()->free_list();
	}

	Entity Scene::CreateChildEntity(Entity parent, const std::string& name)
	{
		Entity entity = CreateEntity(name);
		if (entity && parent.IsValid())
			SetParent(entity, parent, false);
		return entity;
	}

	void Scene::DestroyEntity(Entity entity)
	{
		DestroyEntities(std::span<const Entity>(&entity, 1));
	}

	void Scene::DestroyEntities(std::span<const Entity> entities)
	{
		std::vector<entt::entity> roots;
		roots.reserve(entities.size());
		for (const Entity entity : entities)
		{
			if (!entity.IsValid() || entity.GetScene() != this)
				continue;

			if (m_IsUpdating)
			{
				const UUID uuid = entity.GetUUID();
				if (m_PendingDestroySet.insert(uuid).second)
					m_PendingDestroy.push_back(uuid);
				continue;
			}
			roots.push_back(entity.GetHandle());
		}
		if (roots.empty())
			return;
		RemoveNestedRoots(roots);

		// Every subtree is announced (once) while all of them still exist.
		if (m_IsRunning && !m_Systems.empty())
		{
			for (const entt::entity root : roots)
			{
				if (m_Registry.valid(root))
					NotifyEntitiesDestroying(root);
			}
		}
		DestroySubtrees(roots);
		// Systems reacting to the destruction may have requested more.
		FlushPendingDestroys();
	}

	void Scene::RemoveNestedRoots(std::vector<entt::entity>& roots) const
	{
		// Whether an entity lies below one of the roots (or is one), remembered for every entity on a walked path, so that
		// listing every entity of a deep chain stays linear.
		const std::unordered_set<entt::entity> listed(roots.begin(), roots.end());
		std::unordered_map<entt::entity, bool> covered;
		std::vector<entt::entity> path;
		std::vector<entt::entity> kept;
		std::unordered_set<entt::entity> keptSet;
		for (const entt::entity root : roots)
		{
			path.clear();
			bool nested = false;
			for (entt::entity current = GetHierarchy(root).Parent; current != entt::null; current = GetHierarchy(current).Parent)
			{
				if (auto known = covered.find(current); known != covered.end())
				{
					nested = known->second;
					break;
				}
				if (listed.contains(current))
				{
					nested = true;
					break;
				}
				path.push_back(current);
			}
			for (const entt::entity entity : path)
				covered.emplace(entity, nested);
			// Listed twice: kept once.
			if (!nested && keptSet.insert(root).second)
				kept.push_back(root);
		}
		roots = std::move(kept);
	}

	bool Scene::IsPendingDestroy(Entity entity) const
	{
		if (!entity.IsValid())
			return false;
		return m_PendingDestroySet.find(entity.GetUUID()) != m_PendingDestroySet.end();
	}

	std::vector<entt::entity> Scene::CollectSubtree(entt::entity root) const
	{
		// Iteratively: hierarchies can be arbitrarily deep.
		std::vector<entt::entity> subtree;
		std::vector<entt::entity> stack = { root };
		while (!stack.empty())
		{
			const entt::entity current = stack.back();
			stack.pop_back();
			subtree.push_back(current);
			for (entt::entity child = GetHierarchy(current).LastChild; child != entt::null; child = GetHierarchy(child).PrevSibling)
				stack.push_back(child);
		}
		return subtree;
	}

	void Scene::CollectHierarchyOrder(std::vector<entt::entity>& outEntities) const
	{
		// Depth-first, parents before children, iteratively (hierarchies can be arbitrarily deep).
		outEntities.reserve(outEntities.size() + m_EntityMap.size());
		std::vector<entt::entity> stack;
		for (entt::entity root = m_LastRoot; root != entt::null; root = GetHierarchy(root).PrevSibling)
			stack.push_back(root);
		while (!stack.empty())
		{
			const entt::entity current = stack.back();
			stack.pop_back();
			outEntities.push_back(current);
			for (entt::entity child = GetHierarchy(current).LastChild; child != entt::null; child = GetHierarchy(child).PrevSibling)
				stack.push_back(child);
		}
	}

	void Scene::NotifyEntitiesDestroying(entt::entity root)
	{
		// Systems run arbitrary code here (scripts' OnDestroy), so destruction they request is deferred, and the subtree
		// is collected again afterwards: entities attached to it in the meantime are announced too.
		const bool wasUpdating = m_IsUpdating;
		m_IsUpdating = true;
		std::unordered_set<entt::entity> notified;
		bool notifiedAny = true;
		while (notifiedAny && m_Registry.valid(root))
		{
			notifiedAny = false;
			const std::vector<entt::entity> subtree = CollectSubtree(root);
			for (auto it = subtree.rbegin(); it != subtree.rend(); ++it)
			{
				if (!notified.insert(*it).second)
					continue;
				notifiedAny = true;
				const Entity entity(*it, this);
				for (const Scope<SceneSystem>& system : m_Systems)
					system->OnEntityDestroying(entity);
			}
		}
		m_IsUpdating = wasUpdating;
	}

	void Scene::DestroySubtrees(std::span<const entt::entity> roots)
	{
		// A single subtree leaves its sibling list directly (its position is usually known); a batch compacts each sibling
		// list it touches once, so destroying many entities of a long list stays linear.
		const bool batch = roots.size() > 1;
		std::unordered_map<entt::entity, std::vector<UUID>> removedIds; // By parent (entt::null: the roots)
		bool destroyedAny = false;
		for (const entt::entity root : roots)
		{
			// Destroyed with an earlier root of the batch.
			if (!m_Registry.valid(root))
				continue;

			const std::vector<entt::entity> subtree = CollectSubtree(root);
			if (batch)
				removedIds[GetHierarchy(root).Parent].push_back(m_Registry.get<IDComponent>(root).ID);
			// Only the subtree root leaves its parent; the links inside the subtree disappear with the entities.
			Unlink(root, !batch);

			m_DestroyingEntities = true;
			// Children before their parents.
			for (auto it = subtree.rbegin(); it != subtree.rend(); ++it)
			{
				m_EntityMap.erase(m_Registry.get<IDComponent>(*it).ID);
				m_Registry.destroy(*it);
			}
			m_DestroyingEntities = false;
			destroyedAny = true;
		}

		for (const auto& [parent, ids] : removedIds)
		{
			// The parent may have gone with a later root of the batch.
			if (parent != entt::null && !m_Registry.valid(parent))
				continue;
			std::vector<UUID>& siblings = GetChildIds(parent);
			const std::unordered_set<UUID> removed(ids.begin(), ids.end());
			std::erase_if(siblings, [&removed](UUID id) { return removed.contains(id); });
		}

		if (destroyedAny)
			m_HierarchyVersion++;
	}

	void Scene::FlushPendingDestroys()
	{
		// Destroying entities can request more destruction (systems reacting to it), so repeat until nothing is left.
		// Each round iterates over a moved-out copy: destroying entities must not observe a list being modified.
		while (!m_PendingDestroy.empty())
		{
			std::vector<UUID> pending = std::move(m_PendingDestroy);
			m_PendingDestroy.clear();
			m_PendingDestroySet.clear();
			for (UUID uuid : pending)
			{
				auto it = m_EntityMap.find(uuid);
				if (it == m_EntityMap.end())
					continue;

				const entt::entity handle = it->second;
				if (m_IsRunning && !m_Systems.empty())
					NotifyEntitiesDestroying(handle);
				if (m_Registry.valid(handle))
					DestroySubtrees(std::span<const entt::entity>(&handle, 1));
			}
		}
	}

	Entity Scene::DuplicateEntity(Entity entity)
	{
		if (!entity.IsValid() || entity.GetScene() != this)
			return {};

		const Entity parent = entity.GetParent();
		const nlohmann::json snapshot = SceneSerializer::SerializeEntities(*this, { entity });

		EntityInstantiationOptions options;
		options.GenerateNewUUIDs = true;
		options.Parent = parent;
		std::vector<Entity> roots = SceneSerializer::DeserializeEntities(*this, snapshot, options);
		if (roots.empty())
			return {};

		// Place the copy directly after the original among its siblings.
		SetSiblingIndex(roots.front(), GetSiblingIndex(entity) + 1);
		return roots.front();
	}

	Entity Scene::GetEntityByUUID(UUID uuid) const
	{
		auto it = m_EntityMap.find(uuid);
		if (it == m_EntityMap.end())
			return {};
		return Entity(it->second, const_cast<Scene*>(this));
	}

	////////////////////////////////////////////////////////////////////////////////
	// Lookups
	////////////////////////////////////////////////////////////////////////////////

	void Scene::BuildLookupIndex(LookupIndex& index, bool names) const
	{
		index.Buckets.clear();
		index.Slots.clear();
		index.Built = true;
		m_LookupIndexBuildCount++;
		if (names)
		{
			for (const auto [handle, name] : m_Registry.view<NameComponent>().each())
				IndexLookupValue(index, handle, name.Name);
		}
		else
		{
			for (const auto [handle, tag] : m_Registry.view<TagComponent>().each())
				IndexLookupValue(index, handle, tag.Tag);
		}
	}

	void Scene::IndexLookupValue(LookupIndex& index, entt::entity handle, std::string_view value) const
	{
		const size_t slotIndex = static_cast<size_t>(entt::to_entity(handle));
		if (slotIndex >= index.Slots.size())
			index.Slots.resize(slotIndex + 1);

		const uint64_t key = HashLookupValue(value);
		if (index.Slots[slotIndex].Indexed)
		{
			if (index.Slots[slotIndex].Key == key)
				return;
			RemoveLookupValue(index, handle);
		}

		std::vector<entt::entity>& bucket = index.Buckets[key];
		index.Slots[slotIndex] = LookupSlot { key, static_cast<uint32_t>(bucket.size()), true };
		bucket.push_back(handle);
	}

	void Scene::RemoveLookupValue(LookupIndex& index, entt::entity handle) const
	{
		const size_t slotIndex = static_cast<size_t>(entt::to_entity(handle));
		if (slotIndex >= index.Slots.size() || !index.Slots[slotIndex].Indexed)
			return;

		LookupSlot& slot = index.Slots[slotIndex];
		auto bucket = index.Buckets.find(slot.Key);
		ST_CORE_ASSERT(bucket != index.Buckets.end(), "Scene lookup index: an indexed entity has no bucket");
		if (bucket != index.Buckets.end())
		{
			// Swap with the last entry, so removal does not depend on the bucket's size.
			std::vector<entt::entity>& entities = bucket->second;
			const entt::entity moved = entities.back();
			entities[slot.Position] = moved;
			index.Slots[static_cast<size_t>(entt::to_entity(moved))].Position = slot.Position;
			entities.pop_back();
			if (entities.empty())
				index.Buckets.erase(bucket);
		}
		slot.Indexed = false;
	}

	Entity Scene::FindEntityByName(std::string_view name) const
	{
		if (!m_NameIndex.Built)
			BuildLookupIndex(m_NameIndex, true);

		auto bucket = m_NameIndex.Buckets.find(HashLookupValue(name));
		if (bucket == m_NameIndex.Buckets.end())
			return {};

		Entity first;
		for (const entt::entity handle : bucket->second)
		{
			m_LookupVisitCount++;
			if (m_Registry.get<NameComponent>(handle).Name != name)
				continue;
			const Entity candidate(handle, const_cast<Scene*>(this));
			if (!first || CompareHierarchyOrder(candidate, first) < 0)
				first = candidate;
		}
		return first;
	}

	std::vector<Entity> Scene::FindEntitiesByTag(std::string_view tag) const
	{
		if (!m_TagIndex.Built)
			BuildLookupIndex(m_TagIndex, false);

		std::vector<Entity> result;
		auto bucket = m_TagIndex.Buckets.find(HashLookupValue(tag));
		if (bucket == m_TagIndex.Buckets.end())
			return result;

		for (const entt::entity handle : bucket->second)
		{
			m_LookupVisitCount++;
			if (m_Registry.get<TagComponent>(handle).Tag == tag)
				result.emplace_back(handle, const_cast<Scene*>(this));
		}
		std::sort(result.begin(), result.end(), [this](const Entity& a, const Entity& b) { return CompareHierarchyOrder(a, b) < 0; });
		return result;
	}

	Entity Scene::GetPrimaryCameraEntity()
	{
		// Few entities have cameras: examining them all is cheap, and reading Primary here keeps direct writes visible.
		Entity first;
		for (const auto [handle, camera] : m_Registry.view<CameraComponent>().each())
		{
			m_LookupVisitCount++;
			if (!camera.Primary || !m_Registry.get<WorldTransformComponent>(handle).ActiveInHierarchy)
				continue;
			const Entity candidate(handle, this);
			if (!first || CompareHierarchyOrder(candidate, first) < 0)
				first = candidate;
		}
		return first;
	}

	std::vector<Entity> Scene::GetEntitiesInHierarchyOrder() const
	{
		if (!m_HierarchyOrderValid || m_HierarchyOrderVersion != m_HierarchyVersion)
		{
			std::vector<entt::entity> handles;
			CollectHierarchyOrder(handles);
			m_HierarchyOrder.clear();
			m_HierarchyOrder.reserve(handles.size());
			for (const entt::entity handle : handles)
				m_HierarchyOrder.emplace_back(handle, const_cast<Scene*>(this));
			m_HierarchyOrderVersion = m_HierarchyVersion;
			m_HierarchyOrderValid = true;
			m_HierarchyOrderBuildCount++;
		}
		return m_HierarchyOrder;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Hierarchy links
	////////////////////////////////////////////////////////////////////////////////

	std::vector<UUID>& Scene::GetChildIds(entt::entity parent)
	{
		return parent == entt::null ? m_RootEntities : m_Registry.get<RelationshipComponent>(parent).Children;
	}

	entt::entity Scene::GetFirstChild(entt::entity parent) const
	{
		return parent == entt::null ? m_FirstRoot : GetHierarchy(parent).FirstChild;
	}

	void Scene::LinkLast(entt::entity child, entt::entity parent)
	{
		HierarchyComponent& node = GetHierarchy(child);
		const UUID id = m_Registry.get<IDComponent>(child).ID;
		node.Parent = parent;
		node.NextSibling = entt::null;
		if (parent == entt::null)
		{
			node.PrevSibling = m_LastRoot;
			if (m_LastRoot != entt::null)
				GetHierarchy(m_LastRoot).NextSibling = child;
			else
				m_FirstRoot = child;
			m_LastRoot = child;
			// Appending keeps the other positions current.
			node.SiblingIndex = static_cast<uint32_t>(m_RootEntities.size());
			m_RootEntities.push_back(id);
			m_Registry.get<RelationshipComponent>(child).Parent = UUID::Null();
			return;
		}

		HierarchyComponent& parentNode = GetHierarchy(parent);
		node.PrevSibling = parentNode.LastChild;
		if (parentNode.LastChild != entt::null)
			GetHierarchy(parentNode.LastChild).NextSibling = child;
		else
			parentNode.FirstChild = child;
		parentNode.LastChild = child;
		node.SiblingIndex = parentNode.ChildCount;
		parentNode.ChildCount++;
		m_Registry.get<RelationshipComponent>(parent).Children.push_back(id);
		m_Registry.get<RelationshipComponent>(child).Parent = m_Registry.get<IDComponent>(parent).ID;
	}

	void Scene::LinkAt(entt::entity child, entt::entity parent, size_t position)
	{
		std::vector<UUID>& siblings = GetChildIds(parent);
		if (position >= siblings.size())
		{
			LinkLast(child, parent);
			return;
		}

		// The entity now at `position` becomes the next sibling.
		auto nextIt = m_EntityMap.find(siblings[position]);
		ST_CORE_ASSERT(nextIt != m_EntityMap.end(), "Scene: a sibling list names an entity that does not exist");
		if (nextIt == m_EntityMap.end())
		{
			LinkLast(child, parent);
			return;
		}
		const entt::entity next = nextIt->second;
		HierarchyComponent& node = GetHierarchy(child);
		HierarchyComponent& nextNode = GetHierarchy(next);
		node.Parent = parent;
		node.NextSibling = next;
		node.PrevSibling = nextNode.PrevSibling;
		if (nextNode.PrevSibling != entt::null)
			GetHierarchy(nextNode.PrevSibling).NextSibling = child;
		else if (parent != entt::null)
			GetHierarchy(parent).FirstChild = child;
		else
			m_FirstRoot = child;
		nextNode.PrevSibling = child;

		siblings.insert(siblings.begin() + static_cast<ptrdiff_t>(position), m_Registry.get<IDComponent>(child).ID);
		if (parent != entt::null)
		{
			HierarchyComponent& parentNode = GetHierarchy(parent);
			parentNode.ChildCount++;
			parentNode.ChildIndicesValid = false;
			m_Registry.get<RelationshipComponent>(child).Parent = m_Registry.get<IDComponent>(parent).ID;
		}
		else
		{
			m_RootIndicesValid = false;
			m_Registry.get<RelationshipComponent>(child).Parent = UUID::Null();
		}
	}

	void Scene::Unlink(entt::entity child, bool removeId)
	{
		HierarchyComponent& node = GetHierarchy(child);
		const entt::entity parent = node.Parent;
		const bool wasLast = node.NextSibling == entt::null;

		if (node.PrevSibling != entt::null)
			GetHierarchy(node.PrevSibling).NextSibling = node.NextSibling;
		else if (parent != entt::null)
			GetHierarchy(parent).FirstChild = node.NextSibling;
		else
			m_FirstRoot = node.NextSibling;

		if (node.NextSibling != entt::null)
			GetHierarchy(node.NextSibling).PrevSibling = node.PrevSibling;
		else if (parent != entt::null)
			GetHierarchy(parent).LastChild = node.PrevSibling;
		else
			m_LastRoot = node.PrevSibling;

		bool& indicesValid = parent != entt::null ? GetHierarchy(parent).ChildIndicesValid : m_RootIndicesValid;
		if (removeId)
		{
			std::vector<UUID>& siblings = GetChildIds(parent);
			const UUID id = m_Registry.get<IDComponent>(child).ID;
			size_t position = indicesValid ? node.SiblingIndex : siblings.size();
			if (position >= siblings.size() || siblings[position] != id)
				position = static_cast<size_t>(std::find(siblings.begin(), siblings.end(), id) - siblings.begin());
			if (position < siblings.size())
				siblings.erase(siblings.begin() + static_cast<ptrdiff_t>(position));
		}
		// Removing the last entry keeps the other positions; anything else shifts them (and a deferred removal keeps the id
		// in the list for now).
		if (!wasLast || !removeId)
			indicesValid = false;
		if (parent != entt::null)
			GetHierarchy(parent).ChildCount--;

		m_Registry.get<RelationshipComponent>(child).Parent = UUID::Null();
		node.Parent = entt::null;
		node.NextSibling = entt::null;
		node.PrevSibling = entt::null;
	}

	void Scene::RebuildChildList(entt::entity parent, std::span<const entt::entity> children)
	{
		std::vector<UUID>& ids = GetChildIds(parent);
		ids.clear();
		ids.reserve(children.size());
		const UUID parentId = parent != entt::null ? m_Registry.get<IDComponent>(parent).ID : UUID::Null();
		entt::entity previous = entt::null;
		uint32_t index = 0;
		for (const entt::entity child : children)
		{
			HierarchyComponent& node = GetHierarchy(child);
			node.Parent = parent;
			node.PrevSibling = previous;
			node.NextSibling = entt::null;
			node.SiblingIndex = index++;
			if (previous != entt::null)
				GetHierarchy(previous).NextSibling = child;
			ids.push_back(m_Registry.get<IDComponent>(child).ID);
			m_Registry.get<RelationshipComponent>(child).Parent = parentId;
			previous = child;
		}

		const entt::entity first = children.empty() ? entt::null : children.front();
		if (parent != entt::null)
		{
			HierarchyComponent& parentNode = GetHierarchy(parent);
			parentNode.FirstChild = first;
			parentNode.LastChild = previous;
			parentNode.ChildCount = static_cast<uint32_t>(children.size());
			parentNode.ChildIndicesValid = true;
		}
		else
		{
			m_FirstRoot = first;
			m_LastRoot = previous;
			m_RootIndicesValid = true;
		}
	}

	void Scene::RefreshSiblingIndices(entt::entity parent) const
	{
		// A cache of positions: recomputing it changes nothing observable, hence const.
		bool& valid = parent != entt::null ? const_cast<HierarchyComponent&>(GetHierarchy(parent)).ChildIndicesValid : m_RootIndicesValid;
		if (valid)
			return;
		valid = true;
		entt::entity child = GetFirstChild(parent);
		if (child == entt::null)
			return;
		// Through the storage, fetched once: a registry lookup per sibling would cost more than the walk itself, and long
		// sibling lists (every single deletion among 100,000 roots) are walked here.
		const auto& hierarchy = *m_Registry.storage<HierarchyComponent>();
		uint32_t index = 0;
		while (child != entt::null)
		{
			HierarchyComponent& node = const_cast<HierarchyComponent&>(hierarchy.get(child));
			node.SiblingIndex = index++;
			child = node.NextSibling;
		}
	}

	void Scene::RefreshSubtreeDepth(entt::entity root, bool prune)
	{
		HierarchyComponent& rootNode = GetHierarchy(root);
		const uint32_t depth = rootNode.Parent == entt::null ? 0 : GetHierarchy(rootNode.Parent).Depth + 1;
		// Depths are relative: a subtree whose root keeps its depth keeps all of them.
		if (prune && rootNode.Depth == depth)
			return;

		rootNode.Depth = depth;
		std::vector<entt::entity> stack = { root };
		while (!stack.empty())
		{
			const entt::entity current = stack.back();
			stack.pop_back();
			const uint32_t childDepth = GetHierarchy(current).Depth + 1;
			for (entt::entity child = GetHierarchy(current).FirstChild; child != entt::null; child = GetHierarchy(child).NextSibling)
			{
				GetHierarchy(child).Depth = childDepth;
				stack.push_back(child);
			}
		}
	}

	void Scene::RefreshSubtreeActivity(entt::entity root, bool prune, bool rootLosesInactive)
	{
		const entt::entity parent = GetHierarchy(root).Parent;
		const bool parentActive = parent == entt::null || m_Registry.get<WorldTransformComponent>(parent).ActiveInHierarchy;
		const bool active = parentActive && (rootLosesInactive || !m_Registry.all_of<InactiveComponent>(root));
		WorldTransformComponent& rootWorld = m_Registry.get<WorldTransformComponent>(root);
		// A subtree whose root keeps its activity keeps all of it (it was consistent before).
		if (prune && rootWorld.ActiveInHierarchy == active)
			return;

		std::vector<entt::entity> changed;
		if (rootWorld.ActiveInHierarchy != active)
			changed.push_back(root);
		rootWorld.ActiveInHierarchy = active;
		std::vector<entt::entity> stack = { root };
		while (!stack.empty())
		{
			const entt::entity current = stack.back();
			stack.pop_back();
			const bool currentActive = m_Registry.get<WorldTransformComponent>(current).ActiveInHierarchy;
			for (entt::entity child = GetHierarchy(current).FirstChild; child != entt::null; child = GetHierarchy(child).NextSibling)
			{
				WorldTransformComponent& childWorld = m_Registry.get<WorldTransformComponent>(child);
				const bool childActive = currentActive && !m_Registry.all_of<InactiveComponent>(child);
				if (prune && childWorld.ActiveInHierarchy == childActive)
					continue;
				if (childWorld.ActiveInHierarchy != childActive)
					changed.push_back(child);
				childWorld.ActiveInHierarchy = childActive;
				stack.push_back(child);
			}
		}

		if (!changed.empty())
			RecordTransformChanges(++m_TransformsVersion, changed, changed.size() > c_MaxTransformChanges);
	}

	void Scene::FinishLinking(std::span<const entt::entity> roots)
	{
		// New subtrees have no consistent state to prune against.
		for (const entt::entity root : roots)
		{
			RefreshSubtreeDepth(root, false);
			RefreshSubtreeActivity(root, false);
			MarkTransformDirty(root);
		}
	}

	size_t Scene::GetSiblingIndex(Entity entity) const
	{
		if (!entity.IsValid() || entity.GetScene() != this)
			return 0;
		const HierarchyComponent& node = GetHierarchy(entity.GetHandle());
		RefreshSiblingIndices(node.Parent);
		return node.SiblingIndex;
	}

	uint32_t Scene::GetDepth(Entity entity) const
	{
		if (!entity.IsValid() || entity.GetScene() != this)
			return 0;
		return GetHierarchy(entity.GetHandle()).Depth;
	}

	bool Scene::SetParent(Entity child, Entity parent, bool keepWorldTransform)
	{
		if (!child.IsValid() || child.GetScene() != this)
			return false;
		const entt::entity childHandle = child.GetHandle();
		// A cycle needs the new parent below the child; without children nothing is below it (building long chains
		// stays linear).
		if (parent.IsValid() && (parent.GetScene() != this || parent == child || (GetHierarchy(childHandle).ChildCount > 0 && IsDescendantOf(parent, child))))
			return false;

		const entt::entity parentHandle = parent.IsValid() ? parent.GetHandle() : entt::null;
		if (GetHierarchy(childHandle).Parent == parentHandle)
			return true;

		const glm::mat4 worldTransform = keepWorldTransform ? GetWorldTransform(child) : glm::mat4(1.0f);
		Unlink(childHandle);
		LinkLast(childHandle, parentHandle);
		m_HierarchyVersion++;
		RefreshSubtreeDepth(childHandle, true);
		RefreshSubtreeActivity(childHandle, true);
		// The new parent's world transform applies from now on.
		MarkTransformDirty(childHandle);

		if (keepWorldTransform)
			SetWorldTransform(child, worldTransform);

		// Systems that build state from the hierarchy (physics merges descendants' colliders into a body) learn about the
		// move through the child's on_update signal, emitted once the hierarchy and transform are final.
		m_Registry.patch<RelationshipComponent>(childHandle);
		return true;
	}

	bool Scene::SetSiblingIndex(Entity entity, size_t index)
	{
		if (!entity.IsValid() || entity.GetScene() != this)
			return false;

		const entt::entity handle = entity.GetHandle();
		const entt::entity parent = GetHierarchy(handle).Parent;
		Unlink(handle);
		LinkAt(handle, parent, index);
		m_HierarchyVersion++;
		return true;
	}

	bool Scene::PlaceEntities(std::span<const EntityPlacement> placements)
	{
		struct Move
		{
			entt::entity Target = entt::null;
			entt::entity Parent = entt::null;
			entt::entity OldParent = entt::null;
			size_t SiblingIndex = 0;
		};

		bool success = true;
		std::vector<Move> moves;
		moves.reserve(placements.size());
		std::unordered_set<entt::entity> targets;
		for (const EntityPlacement& placement : placements)
		{
			auto target = m_EntityMap.find(placement.Target);
			if (target == m_EntityMap.end() || !targets.insert(target->second).second)
			{
				success = false;
				continue;
			}
			entt::entity parent = entt::null;
			if (placement.Parent.IsValid())
			{
				auto parentIt = m_EntityMap.find(placement.Parent);
				if (parentIt != m_EntityMap.end())
					parent = parentIt->second;
				else
					success = false;
			}
			moves.push_back({ target->second, parent, GetHierarchy(target->second).Parent, placement.SiblingIndex });
		}
		if (moves.empty())
			return success;

		// Everything is detached first, so that no intermediate link can close a cycle.
		std::unordered_map<entt::entity, std::vector<UUID>> detachedIds; // By old parent
		for (const Move& move : moves)
		{
			detachedIds[move.OldParent].push_back(m_Registry.get<IDComponent>(move.Target).ID);
			Unlink(move.Target, false);
		}
		for (const auto& [parent, ids] : detachedIds)
		{
			const std::unordered_set<UUID> removed(ids.begin(), ids.end());
			std::erase_if(GetChildIds(parent), [&removed](UUID id) { return removed.contains(id); });
		}

		// Attached in ascending position order. The parent links are set first, one entity at a time, so that each cycle
		// check sees the entities attached before it.
		std::stable_sort(moves.begin(), moves.end(), [](const Move& a, const Move& b) { return a.SiblingIndex < b.SiblingIndex; });
		for (Move& move : moves)
		{
			bool cycle = move.Parent == move.Target;
			for (entt::entity ancestor = move.Parent; !cycle && ancestor != entt::null; ancestor = GetHierarchy(ancestor).Parent)
				cycle = ancestor == move.Target;
			if (cycle)
			{
				move.Parent = entt::null;
				success = false;
			}
			GetHierarchy(move.Target).Parent = move.Parent;
		}

		// Each sibling list is rebuilt once: the entities that stayed keep their order and the placed ones are merged in.
		std::vector<entt::entity> parents;
		std::unordered_map<entt::entity, std::vector<const Move*>> movesByParent;
		for (const Move& move : moves)
		{
			std::vector<const Move*>& list = movesByParent[move.Parent];
			if (list.empty())
				parents.push_back(move.Parent);
			list.push_back(&move);
		}
		std::vector<entt::entity> merged;
		for (const entt::entity parent : parents)
		{
			merged.clear();
			entt::entity stayed = GetFirstChild(parent);
			for (const Move* move : movesByParent[parent])
			{
				while (merged.size() < move->SiblingIndex && stayed != entt::null)
				{
					merged.push_back(stayed);
					stayed = GetHierarchy(stayed).NextSibling;
				}
				merged.push_back(move->Target);
			}
			for (; stayed != entt::null; stayed = GetHierarchy(stayed).NextSibling)
				merged.push_back(stayed);
			RebuildChildList(parent, merged);
		}

		m_HierarchyVersion++;
		for (const Move& move : moves)
		{
			RefreshSubtreeDepth(move.Target, true);
			RefreshSubtreeActivity(move.Target, true);
			MarkTransformDirty(move.Target);
		}
		for (const Move& move : moves)
		{
			if (move.Parent != move.OldParent && m_Registry.valid(move.Target))
				m_Registry.patch<RelationshipComponent>(move.Target);
		}
		return success;
	}

	bool Scene::IsDescendantOf(Entity entity, Entity ancestor) const
	{
		if (!entity.IsValid() || !ancestor.IsValid() || entity.GetScene() != this || ancestor.GetScene() != this)
			return false;

		const entt::entity ancestorHandle = ancestor.GetHandle();
		for (entt::entity current = GetHierarchy(entity.GetHandle()).Parent; current != entt::null; current = GetHierarchy(current).Parent)
		{
			if (current == ancestorHandle)
				return true;
		}
		return false;
	}

	int Scene::CompareHierarchyOrder(Entity a, Entity b) const
	{
		ST_CORE_ASSERT(a.IsValid() && b.IsValid() && a.GetScene() == this && b.GetScene() == this, "CompareHierarchyOrder needs entities of this scene");
		if (a == b)
			return 0;

		entt::entity x = a.GetHandle();
		entt::entity y = b.GetHandle();
		uint32_t depthX = GetHierarchy(x).Depth;
		uint32_t depthY = GetHierarchy(y).Depth;
		for (; depthX > depthY; depthX--)
			x = GetHierarchy(x).Parent;
		// An ancestor comes before its descendants.
		if (x == y)
			return 1;
		for (; depthY > depthX; depthY--)
			y = GetHierarchy(y).Parent;
		if (x == y)
			return -1;

		// Up to the children of the closest common ancestor (or to the roots).
		while (GetHierarchy(x).Parent != GetHierarchy(y).Parent)
		{
			x = GetHierarchy(x).Parent;
			y = GetHierarchy(y).Parent;
		}
		RefreshSiblingIndices(GetHierarchy(x).Parent);
		return GetHierarchy(x).SiblingIndex < GetHierarchy(y).SiblingIndex ? -1 : 1;
	}

	////////////////////////////////////////////////////////////////////////////////
	// World transforms
	////////////////////////////////////////////////////////////////////////////////

	void Scene::MarkTransformDirty(entt::entity handle)
	{
		HierarchyComponent* node = m_Registry.try_get<HierarchyComponent>(handle);
		if (!node || node->TransformDirty)
			return;
		node->TransformDirty = true;
		m_DirtyTransforms.push_back(handle);
	}

	void Scene::MarkTransformChanged(Entity entity)
	{
		if (entity.IsValid() && entity.GetScene() == this)
			MarkTransformDirty(entity.GetHandle());
	}

	void Scene::InvalidateAllTransforms()
	{
		for (entt::entity root = m_FirstRoot; root != entt::null; root = GetHierarchy(root).NextSibling)
			MarkTransformDirty(root);
	}

	bool Scene::HasDirtyAncestor(entt::entity parent)
	{
		// Memoized for the current update: every entity on a walked path remembers the answer for itself (whether it or an
		// ancestor is dirty), so all the queries of one update together cost at most the number of entities.
		std::vector<entt::entity>& path = m_PathScratch;
		path.clear();
		bool result = false;
		for (entt::entity current = parent; current != entt::null;)
		{
			HierarchyComponent& node = GetHierarchy(current);
			if (node.TransformPass == m_TransformPass)
			{
				result = node.DirtyAbove;
				break;
			}
			if (node.TransformDirty)
			{
				node.TransformPass = m_TransformPass;
				node.DirtyAbove = true;
				result = true;
				break;
			}
			path.push_back(current);
			current = node.Parent;
		}
		for (const entt::entity entity : path)
		{
			HierarchyComponent& node = GetHierarchy(entity);
			node.TransformPass = m_TransformPass;
			node.DirtyAbove = result;
		}
		return result;
	}

	void Scene::RecordTransformChanges(uint64_t version, std::span<const entt::entity> entities, bool overflowed)
	{
		if (overflowed)
		{
			m_TransformChanges.clear();
			m_TransformChangesFloor = version;
			return;
		}
		for (const entt::entity entity : entities)
			m_TransformChanges.push_back(TransformChange { version, entity });
		while (m_TransformChanges.size() > c_MaxTransformChanges)
		{
			m_TransformChangesFloor = std::max(m_TransformChangesFloor, m_TransformChanges.front().Version);
			m_TransformChanges.pop_front();
		}
	}

	bool Scene::GetWorldTransformChanges(uint64_t sinceVersion, std::vector<entt::entity>& outEntities) const
	{
		if (sinceVersion >= m_TransformsVersion)
			return true;
		if (sinceVersion < m_TransformChangesFloor)
			return false;
		for (auto it = m_TransformChanges.rbegin(); it != m_TransformChanges.rend() && it->Version > sinceVersion; ++it)
			outEntities.push_back(it->Entity);
		return true;
	}

	void Scene::UpdateWorldTransforms()
	{
		ST_PROFILE_FUNCTION();

		if (m_DirtyTransforms.empty())
			return;

		// The memo of HasDirtyAncestor is keyed by the update; when the key wraps around, old marks could match again.
		if (++m_TransformPass == 0)
		{
			for (HierarchyComponent& node : m_Registry.storage<HierarchyComponent>())
				node.TransformPass = 0;
			m_TransformPass = 1;
		}

		// Subtrees to recompute: the dirty entities without a dirty ancestor (the others are recomputed with that ancestor).
		// Each starts from its parent's cached matrix, which is current.
		std::vector<entt::entity> frontier;
		for (const entt::entity handle : m_DirtyTransforms)
		{
			if (!m_Registry.valid(handle))
				continue;
			const HierarchyComponent& node = GetHierarchy(handle);
			if (node.TransformDirty && !HasDirtyAncestor(node.Parent))
				frontier.push_back(handle);
		}
		m_DirtyTransforms.clear();
		if (frontier.empty())
			return;

		auto& transforms = m_Registry.storage<TransformComponent>();
		auto& worlds = m_Registry.storage<WorldTransformComponent>();
		auto& hierarchy = m_Registry.storage<HierarchyComponent>();
		const auto compute = [&](entt::entity handle)
		{
			HierarchyComponent& node = hierarchy.get(handle);
			node.TransformDirty = false;
			const glm::mat4 local = transforms.get(handle).GetTransform();
			// The same association as GetWorldTransform, so both produce identical matrices.
			worlds.get(handle).Matrix = node.Parent == entt::null ? local : worlds.get(node.Parent).Matrix * local;
		};

		std::vector<entt::entity>& changes = m_ChangeScratch;
		changes.clear();
		size_t changeCount = 0;
		uint64_t updated = 0;

		// Level by level while the set of independent subtrees is narrow (a single moved root fans out), then in parallel.
		std::vector<entt::entity> next;
		while (!frontier.empty())
		{
			if (frontier.size() >= c_ParallelTransformWidth && JobSystem::IsInitialized())
			{
				std::atomic<uint64_t> parallelUpdated = 0;
				std::atomic<size_t> parallelChanges = changeCount;
				std::mutex changesMutex;
				const uint32_t count = static_cast<uint32_t>(frontier.size());
				const uint32_t batchSize = std::max<uint32_t>(1, count / (std::max<uint32_t>(1, JobSystem::GetWorkerThreadCount() + 1) * 4));
				JobSystem::ParallelFor(count, batchSize, [&](uint32_t begin, uint32_t end)
				{
					std::vector<entt::entity> stack;
					std::vector<entt::entity> localChanges;
					uint64_t localUpdated = 0;
					for (uint32_t index = begin; index < end; index++)
					{
						stack.push_back(frontier[index]);
						while (!stack.empty())
						{
							const entt::entity current = stack.back();
							stack.pop_back();
							compute(current);
							localUpdated++;
							if (localChanges.size() <= c_MaxTransformChanges)
								localChanges.push_back(current);
							for (entt::entity child = hierarchy.get(current).FirstChild; child != entt::null; child = hierarchy.get(child).NextSibling)
								stack.push_back(child);
						}
					}
					parallelUpdated.fetch_add(localUpdated, std::memory_order_relaxed);
					const size_t first = parallelChanges.fetch_add(static_cast<size_t>(localUpdated), std::memory_order_relaxed);
					if (first < c_MaxTransformChanges)
					{
						std::scoped_lock<std::mutex> lock(changesMutex);
						const size_t take = std::min(localChanges.size(), c_MaxTransformChanges - first);
						changes.insert(changes.end(), localChanges.begin(), localChanges.begin() + static_cast<ptrdiff_t>(take));
					}
				});
				updated += parallelUpdated.load(std::memory_order_relaxed);
				changeCount = parallelChanges.load(std::memory_order_relaxed);
				m_ParallelTransformUpdateCount++;
				break;
			}

			next.clear();
			for (const entt::entity handle : frontier)
			{
				compute(handle);
				updated++;
				if (changeCount++ < c_MaxTransformChanges)
					changes.push_back(handle);
				for (entt::entity child = hierarchy.get(handle).FirstChild; child != entt::null; child = hierarchy.get(child).NextSibling)
					next.push_back(child);
			}
			frontier.swap(next);
		}

		m_TransformUpdateCount += updated;
		RecordTransformChanges(++m_TransformsVersion, changes, changeCount > c_MaxTransformChanges);
	}

	glm::mat4 Scene::GetWorldTransform(Entity entity) const
	{
		if (!entity.IsValid())
			return glm::mat4(1.0f);
		if (entity.GetScene() != this)
			return entity.GetScene()->GetWorldTransform(entity);

		const entt::entity handle = entity.GetHandle();
		const glm::mat4& cached = m_Registry.get<WorldTransformComponent>(handle).Matrix;
		if (m_DirtyTransforms.empty())
			return cached;

		// The path up to the topmost entity that changed since the last update; above it the cache is current.
		std::vector<entt::entity>& path = m_PathScratch;
		path.clear();
		size_t topmost = SIZE_MAX;
		for (entt::entity current = handle; current != entt::null; current = GetHierarchy(current).Parent)
		{
			path.push_back(current);
			if (GetHierarchy(current).TransformDirty)
				topmost = path.size() - 1;
		}
		if (topmost == SIZE_MAX)
			return cached;

		// Top-down like UpdateWorldTransforms, so that the result equals what the cache will hold.
		const entt::entity above = GetHierarchy(path[topmost]).Parent;
		const glm::mat4 topLocal = m_Registry.get<TransformComponent>(path[topmost]).GetTransform();
		glm::mat4 result = above == entt::null ? topLocal : m_Registry.get<WorldTransformComponent>(above).Matrix * topLocal;
		for (size_t index = topmost; index-- > 0;)
			result = result * m_Registry.get<TransformComponent>(path[index]).GetTransform();
		return result;
	}

	bool Scene::SetWorldTransform(Entity entity, const glm::mat4& worldTransform)
	{
		if (!entity.IsValid())
			return false;

		glm::mat4 localTransform = worldTransform;
		if (const Entity parent = entity.GetParent())
		{
			// A parent with zero scale has no inverse; the child's local transform cannot express the request.
			const glm::mat4 parentWorld = GetWorldTransform(parent);
			if (!(glm::abs(glm::determinant(parentWorld)) > c_MinInvertibleDeterminant))
			{
				ST_CORE_WARN("Cannot set the world transform of '{}': its parent's transform is singular", entity.GetName());
				return false;
			}
			localTransform = glm::inverse(parentWorld) * worldTransform;
		}

		if (!entity.GetComponent<TransformComponent>().SetTransform(localTransform))
		{
			ST_CORE_WARN("Cannot set the world transform of '{}': the transform is degenerate", entity.GetName());
			return false;
		}

		// Systems that mirror transforms (e.g. physics bodies, which only follow signaled edits while they sleep) learn about
		// the change like about any other component edit.
		m_Registry.patch<TransformComponent>(entity.GetHandle());
		return true;
	}

	bool Scene::IsActiveInHierarchy(Entity entity) const
	{
		const WorldTransformComponent* worldTransform = entity.TryGetComponent<WorldTransformComponent>();
		return worldTransform && worldTransform->ActiveInHierarchy;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Validation
	////////////////////////////////////////////////////////////////////////////////

	bool Scene::ValidateWorldTransforms(std::string* outError) const
	{
		auto fail = [&](entt::entity handle, std::string_view what)
		{
			if (outError)
			{
				const IDComponent* id = m_Registry.try_get<IDComponent>(handle);
				const NameComponent* name = m_Registry.try_get<NameComponent>(handle);
				*outError = fmt::format("entity '{}' ({}): {}", name ? name->Name : std::string(), id ? id->ID.ToString() : std::string(), what);
			}
			return false;
		};

		struct Pending
		{
			entt::entity Handle;
			glm::mat4 ParentMatrix;
			bool ParentActive;
			bool ParentStale; // A cached matrix at or above the parent is known to be stale
			bool HasParent;
		};
		std::vector<Pending> stack;
		for (entt::entity root = m_LastRoot; root != entt::null; root = GetHierarchy(root).PrevSibling)
			stack.push_back({ root, glm::mat4(1.0f), true, false, false });
		while (!stack.empty())
		{
			const Pending current = stack.back();
			stack.pop_back();

			const HierarchyComponent& node = GetHierarchy(current.Handle);
			const WorldTransformComponent& world = m_Registry.get<WorldTransformComponent>(current.Handle);
			const bool active = current.ParentActive && !m_Registry.all_of<InactiveComponent>(current.Handle);
			if (world.ActiveInHierarchy != active)
				return fail(current.Handle, fmt::format("cached ActiveInHierarchy is {}, the hierarchy says {}", world.ActiveInHierarchy, active));

			const glm::mat4 local = m_Registry.get<TransformComponent>(current.Handle).GetTransform();
			const glm::mat4 expected = current.HasParent ? current.ParentMatrix * local : local;
			const bool stale = current.ParentStale || node.TransformDirty;
			if (!stale && !IsNearlyEqualMatrix(world.Matrix, expected))
			{
				// The element that differs most among those that disagree (a non-finite one counts as the most).
				int column = 0;
				int row = 0;
				float largest = -1.0f;
				for (int candidateColumn = 0; candidateColumn < 4; candidateColumn++)
				{
					for (int candidateRow = 0; candidateRow < 4; candidateRow++)
					{
						const float x = world.Matrix[candidateColumn][candidateRow];
						const float y = expected[candidateColumn][candidateRow];
						if (IsNearlyEqual(x, y))
							continue;
						const float difference = std::isfinite(x) && std::isfinite(y) ? std::abs(x - y) : std::numeric_limits<float>::infinity();
						if (difference > largest)
						{
							column = candidateColumn;
							row = candidateRow;
							largest = difference;
						}
					}
				}
				return fail(current.Handle, fmt::format("cached world matrix differs from the recomputed one (element [{}][{}]: {} instead of {}; translation ({}, {}, {}) "
					"instead of ({}, {}, {})); was a TransformComponent written without Entity::MarkModified?", column, row, world.Matrix[column][row],
					expected[column][row], world.Matrix[3].x, world.Matrix[3].y, world.Matrix[3].z, expected[3].x, expected[3].y, expected[3].z));
			}

			for (entt::entity child = node.LastChild; child != entt::null; child = GetHierarchy(child).PrevSibling)
				stack.push_back({ child, expected, active, stale, true });
		}
		return true;
	}

	bool Scene::ValidateHierarchy(std::string* outError) const
	{
		auto fail = [&](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};
		auto describe = [&](entt::entity handle)
		{
			const IDComponent* id = m_Registry.try_get<IDComponent>(handle);
			const NameComponent* name = m_Registry.try_get<NameComponent>(handle);
			return fmt::format("'{}' ({})", name ? name->Name : std::string(), id ? id->ID.ToString() : std::string());
		};

		// Walks a sibling list, checks it against its UUID list and appends its entities to outChildren.
		auto checkList =[&](entt::entity parent, const std::vector<UUID>& ids, uint32_t depth, bool indicesValid, std::vector<entt::entity>& outChildren) -> bool
		{
			entt::entity previous = entt::null;
			size_t index = 0;
			for (entt::entity child = GetFirstChild(parent); child != entt::null; child = GetHierarchy(child).NextSibling)
			{
				if (!m_Registry.valid(child) || !m_Registry.all_of<HierarchyComponent, IDComponent, RelationshipComponent>(child))
					return fail(fmt::format("a child list of {} links an entity that does not exist", parent == entt::null ? std::string("the roots") : describe(parent)));
				const HierarchyComponent& node = GetHierarchy(child);
				const UUID id = m_Registry.get<IDComponent>(child).ID;
				if (index >= ids.size() || ids[index] != id)
					return fail(fmt::format("{} is linked at position {} but the UUID list differs there", describe(child), index));
				if (node.Parent != parent || node.PrevSibling != previous)
					return fail(fmt::format("{} has inconsistent parent or sibling links", describe(child)));
				const UUID parentId = parent == entt::null ? UUID::Null() : m_Registry.get<IDComponent>(parent).ID;
				if (m_Registry.get<RelationshipComponent>(child).Parent != parentId)
					return fail(fmt::format("{}: RelationshipComponent::Parent disagrees with the links", describe(child)));
				if (node.Depth != depth)
					return fail(fmt::format("{} has depth {}, expected {}", describe(child), node.Depth, depth));
				if (indicesValid && node.SiblingIndex != index)
					return fail(fmt::format("{} has cached sibling index {}, expected {}", describe(child), node.SiblingIndex, index));
				auto mapped = m_EntityMap.find(id);
				if (mapped == m_EntityMap.end() || mapped->second != child)
					return fail(fmt::format("{} is missing from the UUID map", describe(child)));
				outChildren.push_back(child);
				previous = child;
				index++;
			}
			const entt::entity last = parent == entt::null ? m_LastRoot : GetHierarchy(parent).LastChild;
			if (last != previous)
				return fail(fmt::format("the last child link of {} is wrong", parent == entt::null ? std::string("the roots") : describe(parent)));
			if (index != ids.size())
				return fail(fmt::format("{} has {} linked children but {} in its UUID list", parent == entt::null ? std::string("The root list") : describe(parent), index, ids.size()));
			if (parent != entt::null && GetHierarchy(parent).ChildCount != index)
				return fail(fmt::format("{} has child count {}, expected {}", describe(parent), GetHierarchy(parent).ChildCount, index));
			return true;
		};

		size_t visited = 0;
		std::vector<entt::entity> children;
		std::vector<entt::entity> stack;
		if (!checkList(entt::null, m_RootEntities, 0, m_RootIndicesValid, children))
			return false;
		stack.assign(children.rbegin(), children.rend());
		while (!stack.empty())
		{
			const entt::entity current = stack.back();
			stack.pop_back();
			visited++;
			children.clear();
			const HierarchyComponent& node = GetHierarchy(current);
			if (!checkList(current, m_Registry.get<RelationshipComponent>(current).Children, node.Depth + 1, node.ChildIndicesValid, children))
				return false;
			stack.insert(stack.end(), children.rbegin(), children.rend());
		}
		if (visited != m_EntityMap.size())
			return fail(fmt::format("{} entities are reachable from the roots, but the scene has {}", visited, m_EntityMap.size()));

		auto checkIndex = [&](const LookupIndex& index, auto view, auto value, std::string_view kind) -> bool
		{
			if (!index.Built)
				return true;
			size_t indexed = 0;
			for (const auto& [key, bucket] : index.Buckets)
				indexed += bucket.size();
			size_t expected = 0;
			for (const entt::entity handle : view)
			{
				expected++;
				const size_t slotIndex = static_cast<size_t>(entt::to_entity(handle));
				const uint64_t key = HashLookupValue(value(handle));
				if (slotIndex >= index.Slots.size() || !index.Slots[slotIndex].Indexed || index.Slots[slotIndex].Key != key)
					return fail(fmt::format("the {} index does not list {} under its current value; was the component written without Entity::MarkModified?", kind, describe(handle)));
				auto bucket = index.Buckets.find(key);
				if (bucket == index.Buckets.end() || index.Slots[slotIndex].Position >= bucket->second.size() || bucket->second[index.Slots[slotIndex].Position] != handle)
					return fail(fmt::format("the {} index has a wrong position for {}", kind, describe(handle)));
			}
			if (indexed != expected)
				return fail(fmt::format("the {} index lists {} entities, but {} have the component", kind, indexed, expected));
			return true;
		};
		if (!checkIndex(m_NameIndex, m_Registry.view<NameComponent>(), [this](entt::entity handle) -> std::string_view { return m_Registry.get<NameComponent>(handle).Name; }, "name"))
			return false;
		if (!checkIndex(m_TagIndex, m_Registry.view<TagComponent>(), [this](entt::entity handle) -> std::string_view { return m_Registry.get<TagComponent>(handle).Tag; }, "tag"))
			return false;
		return true;
	}

	void Scene::AssertCachesValid() const
	{
		std::string error;
		if (!ValidateWorldTransforms(&error) || !ValidateHierarchy(&error))
			ST_CORE_ASSERT(false, "Scene '{}' has inconsistent caches: {}", m_Name, error);
	}

	////////////////////////////////////////////////////////////////////////////////
	// Signals
	////////////////////////////////////////////////////////////////////////////////

	void Scene::OnTransformChanged(entt::registry&, entt::entity handle)
	{
		MarkTransformDirty(handle);
	}

	void Scene::OnInactiveAdded(entt::registry& registry, entt::entity handle)
	{
		// Components being copied or deserialized before the entity is linked: the linking step computes activity.
		if (m_DestroyingEntities || !registry.all_of<HierarchyComponent, WorldTransformComponent>(handle))
			return;
		RefreshSubtreeActivity(handle, true);
	}

	void Scene::OnInactiveRemoved(entt::registry& registry, entt::entity handle)
	{
		// Emitted before the component is gone, so the entity counts as active already.
		if (m_DestroyingEntities || !registry.all_of<HierarchyComponent, WorldTransformComponent>(handle))
			return;
		RefreshSubtreeActivity(handle, true, true);
	}

	void Scene::OnNameChanged(entt::registry& registry, entt::entity handle)
	{
		if (m_NameIndex.Built)
			IndexLookupValue(m_NameIndex, handle, registry.get<NameComponent>(handle).Name);
	}

	void Scene::OnNameRemoved(entt::registry&, entt::entity handle)
	{
		if (m_NameIndex.Built)
			RemoveLookupValue(m_NameIndex, handle);
	}

	void Scene::OnTagChanged(entt::registry& registry, entt::entity handle)
	{
		if (m_TagIndex.Built)
			IndexLookupValue(m_TagIndex, handle, registry.get<TagComponent>(handle).Tag);
	}

	void Scene::OnTagRemoved(entt::registry&, entt::entity handle)
	{
		if (m_TagIndex.Built)
			RemoveLookupValue(m_TagIndex, handle);
	}

	////////////////////////////////////////////////////////////////////////////////
	// Runtime
	////////////////////////////////////////////////////////////////////////////////

	void Scene::OnRuntimeStart(SceneRuntimeMode mode)
	{
		if (m_IsRunning)
			return;

		m_IsRunning = true;
		m_RuntimeMode = mode;
		m_IsPaused = false;
		m_StepFrames = 0;
		m_FixedTimeAccumulator = 0.0f;
		m_Time = 0.0;
		m_FrameIndex = 0;
		m_QuitRequest.reset();
		m_SceneLoadRequest.reset();
		UpdateWorldTransforms();

		for (const SceneSystemDescriptor& descriptor : SceneSystemRegistry::GetAll())
		{
			if (mode == SceneRuntimeMode::Play || descriptor.RunsInSimulateMode)
				m_Systems.push_back(descriptor.Create(*this));
		}

		// Systems may create or destroy entities while starting (e.g. scripts in OnCreate).
		m_IsUpdating = true;
		for (const Scope<SceneSystem>& system : m_Systems)
			system->OnRuntimeStart();
		for (const Scope<SceneSystem>& system : m_Systems)
			system->OnRuntimeStarted();
		m_IsUpdating = false;
		FlushPendingDestroys();
		UpdateWorldTransforms();
	}

	void Scene::OnRuntimeStop()
	{
		if (!m_IsRunning)
			return;

		m_IsUpdating = true;
		for (auto it = m_Systems.rbegin(); it != m_Systems.rend(); ++it)
			(*it)->OnRuntimeStop();
		m_IsUpdating = false;

		while (!m_Systems.empty())
			m_Systems.pop_back();

		FlushPendingDestroys();
		m_IsRunning = false;
	}

	void Scene::OnUpdateRuntime(Timestep timestep)
	{
		ST_PROFILE_FUNCTION();

		if (!m_IsRunning)
		{
			UpdateWorldTransforms();
#ifdef ST_DEBUG
			AssertCachesValid();
#endif
			return;
		}

		const bool stepping = m_IsPaused && m_StepFrames > 0;
		if (m_IsPaused && !stepping)
		{
			UpdateWorldTransforms();
#ifdef ST_DEBUG
			AssertCachesValid();
#endif
			return;
		}

		const float fixedTimestep = m_Settings.FixedTimestep > 0.0f ? m_Settings.FixedTimestep : 1.0f / 60.0f;
		const float deltaTime = stepping ? fixedTimestep : static_cast<float>(timestep) * m_TimeScale;

		m_IsUpdating = true;
		UpdateWorldTransforms();

		for (const Scope<SceneSystem>& system : m_Systems)
			system->OnUpdate(Timestep(deltaTime));

		if (stepping)
		{
			for (const Scope<SceneSystem>& system : m_Systems)
				system->OnFixedUpdate(fixedTimestep);
			m_StepFrames--;
		}
		else
		{
			m_FixedTimeAccumulator += deltaTime;
			const uint32_t maxSteps = std::max(1u, m_Settings.MaxFixedStepsPerFrame);
			uint32_t steps = 0;
			while (m_FixedTimeAccumulator >= fixedTimestep && steps < maxSteps)
			{
				for (const Scope<SceneSystem>& system : m_Systems)
					system->OnFixedUpdate(fixedTimestep);
				m_FixedTimeAccumulator -= fixedTimestep;
				steps++;
			}

			// Too far behind (e.g. after a hitch): drop the backlog instead of simulating it all next frame.
			if (steps == maxSteps)
				m_FixedTimeAccumulator = std::min(m_FixedTimeAccumulator, fixedTimestep);
		}

		for (const Scope<SceneSystem>& system : m_Systems)
			system->OnLateUpdate(Timestep(deltaTime));

		m_Time += deltaTime;
		m_FrameIndex++;
		m_IsUpdating = false;

		FlushPendingDestroys();
		UpdateWorldTransforms();
#ifdef ST_DEBUG
		AssertCachesValid();
#endif
	}

	void Scene::SetPaused(bool paused)
	{
		if (paused == m_IsPaused)
			return;

		m_IsPaused = paused;
		if (!m_IsRunning)
			return;
		for (const Scope<SceneSystem>& system : m_Systems)
			system->OnPausedChanged(paused);
	}

	void Scene::OnUpdateEditor(Timestep)
	{
		ST_PROFILE_FUNCTION();
		UpdateWorldTransforms();
#ifdef ST_DEBUG
		AssertCachesValid();
#endif
	}

	std::optional<UUID> Scene::TakeSceneLoadRequest()
	{
		std::optional<UUID> request = m_SceneLoadRequest;
		m_SceneLoadRequest.reset();
		return request;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Entity
	////////////////////////////////////////////////////////////////////////////////

	Entity Entity::GetParent() const
	{
		if (!IsValid())
			return {};
		// Entities being deserialized are linked last; until then their relationship names the parent.
		if (const HierarchyComponent* node = m_Scene->m_Registry.try_get<HierarchyComponent>(m_EntityHandle))
			return node->Parent != entt::null ? Entity(node->Parent, m_Scene) : Entity();
		return m_Scene->GetEntityByUUID(GetComponent<RelationshipComponent>().Parent);
	}

	std::vector<Entity> Entity::GetChildren() const
	{
		std::vector<Entity> children;
		if (!IsValid())
			return children;

		const entt::registry& registry = m_Scene->m_Registry;
		if (const HierarchyComponent* node = registry.try_get<HierarchyComponent>(m_EntityHandle))
		{
			children.reserve(node->ChildCount);
			for (entt::entity child = node->FirstChild; child != entt::null; child = registry.get<HierarchyComponent>(child).NextSibling)
				children.emplace_back(child, m_Scene);
			return children;
		}
		for (UUID child : GetComponent<RelationshipComponent>().Children)
		{
			if (Entity entity = m_Scene->GetEntityByUUID(child))
				children.push_back(entity);
		}
		return children;
	}

	void Entity::SetActive(bool active)
	{
		if (!IsValid())
			return;

		if (active && HasComponent<InactiveComponent>())
			RemoveComponent<InactiveComponent>();
		else if (!active && !HasComponent<InactiveComponent>())
			AddComponent<InactiveComponent>();
	}

	////////////////////////////////////////////////////////////////////////////////
	// SceneSystemRegistry
	////////////////////////////////////////////////////////////////////////////////

	// Defined in SceneSystemRegistration.cpp.
	void CreateBuiltinSceneSystems(std::vector<SceneSystemDescriptor>& descriptors);

	// The built-in systems are added directly to the storage (never through Register, which would re-enter the
	// call_once), before any other registration.
	static std::vector<SceneSystemDescriptor>& GetSceneSystemDescriptors()
	{
		static std::vector<SceneSystemDescriptor> s_Descriptors;
		static std::once_flag s_BuiltinsRegistered;
		std::call_once(s_BuiltinsRegistered, []() { CreateBuiltinSceneSystems(s_Descriptors); });
		return s_Descriptors;
	}

	void SceneSystemRegistry::Register(SceneSystemDescriptor descriptor)
	{
		Unregister(descriptor.Name);
		GetSceneSystemDescriptors().push_back(std::move(descriptor));
	}

	void SceneSystemRegistry::Unregister(const std::string& name)
	{
		std::vector<SceneSystemDescriptor>& descriptors = GetSceneSystemDescriptors();
		descriptors.erase(std::remove_if(descriptors.begin(), descriptors.end(), [&](const SceneSystemDescriptor& descriptor) { return descriptor.Name == name; }), descriptors.end());
	}

	const std::vector<SceneSystemDescriptor>& SceneSystemRegistry::GetAll()
	{
		return GetSceneSystemDescriptors();
	}

}
