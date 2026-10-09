#include "stpch.h"
#include "Strata/Scene/Scene.h"

#include "Strata/Core/JobSystem.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneSerializer.h"

namespace Strata
{

	// Root count above which world transform propagation runs on the job system.
	static constexpr size_t c_ParallelTransformRootThreshold = 256;

	Scene::Scene(std::string name)
		: m_Name(std::move(name))
	{
	}

	Scene::~Scene()
	{
		OnRuntimeStop();
	}

	Ref<Scene> Scene::Copy(const Ref<Scene>& source)
	{
		Ref<Scene> destination = CreateRef<Scene>(source->m_Name);
		destination->m_Settings = source->m_Settings;

		const std::vector<const ComponentInfo*>& components = ComponentRegistry::GetAll();
		for (const Entity sourceEntity : source->GetEntitiesInHierarchyOrder())
		{
			const entt::entity handle = destination->m_Registry.create();
			for (const ComponentInfo* info : components)
			{
				if (info->IsCopyable())
					info->Copy(destination->m_Registry, handle, source->m_Registry, sourceEntity.GetHandle());
			}

			const WorldTransformComponent* worldTransform = source->m_Registry.try_get<WorldTransformComponent>(sourceEntity.GetHandle());
			destination->m_Registry.emplace<WorldTransformComponent>(handle, worldTransform ? *worldTransform : WorldTransformComponent());
			destination->m_EntityMap.emplace(sourceEntity.GetUUID(), handle);
		}

		destination->m_RootEntities = source->m_RootEntities;
		return destination;
	}

	Entity Scene::CreateEntity(const std::string& name)
	{
		return CreateEntityWithUUID(UUID(), name);
	}

	Entity Scene::CreateEntityWithUUID(UUID uuid, const std::string& name)
	{
		if (!uuid.IsValid() || m_EntityMap.find(uuid) != m_EntityMap.end())
		{
			if (uuid.IsValid())
				ST_CORE_ERROR("Scene '{}': entity UUID {} already exists; assigning a new UUID", m_Name, uuid.ToString());
			uuid = UUID();
		}

		Entity entity(m_Registry.create(), this);
		entity.AddComponent<IDComponent>(uuid);
		entity.AddComponent<NameComponent>(name.empty() ? std::string("Entity") : name);
		entity.AddComponent<TransformComponent>();
		entity.AddComponent<RelationshipComponent>();
		entity.AddComponent<WorldTransformComponent>();

		m_EntityMap.emplace(uuid, entity.GetHandle());
		m_RootEntities.push_back(uuid);
		return entity;
	}

	Entity Scene::CreateChildEntity(Entity parent, const std::string& name)
	{
		Entity entity = CreateEntity(name);
		if (parent.IsValid())
			SetParent(entity, parent, false);
		return entity;
	}

	void Scene::DestroyEntity(Entity entity)
	{
		if (!entity.IsValid() || entity.GetScene() != this)
			return;

		if (m_IsUpdating)
		{
			const UUID uuid = entity.GetUUID();
			if (m_PendingDestroySet.insert(uuid).second)
				m_PendingDestroy.push_back(uuid);
			return;
		}

		DestroyEntityImmediate(entity.GetHandle());
		// Systems reacting to the destruction may have requested more.
		FlushPendingDestroys();
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
			const std::vector<UUID>& children = m_Registry.get<RelationshipComponent>(current).Children;
			for (auto it = children.rbegin(); it != children.rend(); ++it)
			{
				auto child = m_EntityMap.find(*it);
				if (child != m_EntityMap.end())
					stack.push_back(child->second);
			}
		}
		return subtree;
	}

	void Scene::NotifyEntitiesDestroying(entt::entity root)
	{
		// Systems run arbitrary code here (scripts' OnDestroy), so destruction they request is deferred, and the subtree
		// is collected again afterwards: entities attached to it in the meantime are announced too.
		const bool wasUpdating = m_IsUpdating;
		m_IsUpdating = true;
		std::unordered_set<entt::entity> notified;
		bool notifiedAny = true;
		while (notifiedAny)
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

	void Scene::DestroyEntityImmediate(entt::entity handle)
	{
		if (m_IsRunning && !m_Systems.empty())
			NotifyEntitiesDestroying(handle);

		// Only the subtree root leaves its parent; the links inside the subtree disappear with the entities.
		const std::vector<entt::entity> subtree = CollectSubtree(handle);
		RemoveFromParent(handle);
		// Children before their parents.
		for (auto it = subtree.rbegin(); it != subtree.rend(); ++it)
		{
			m_EntityMap.erase(m_Registry.get<IDComponent>(*it).ID);
			m_Registry.destroy(*it);
		}
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
				if (it != m_EntityMap.end())
					DestroyEntityImmediate(it->second);
			}
		}
	}

	void Scene::RemoveFromParent(entt::entity handle)
	{
		RelationshipComponent& relationship = m_Registry.get<RelationshipComponent>(handle);
		const UUID uuid = m_Registry.get<IDComponent>(handle).ID;
		if (relationship.Parent.IsValid())
		{
			auto parentIt = m_EntityMap.find(relationship.Parent);
			if (parentIt != m_EntityMap.end())
			{
				std::vector<UUID>& siblings = m_Registry.get<RelationshipComponent>(parentIt->second).Children;
				siblings.erase(std::remove(siblings.begin(), siblings.end(), uuid), siblings.end());
			}
			relationship.Parent = UUID::Null();
		}
		else
		{
			m_RootEntities.erase(std::remove(m_RootEntities.begin(), m_RootEntities.end(), uuid), m_RootEntities.end());
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
		const std::vector<UUID>& siblings = parent.IsValid() ? parent.GetComponent<RelationshipComponent>().Children : m_RootEntities;
		auto it = std::find(siblings.begin(), siblings.end(), entity.GetUUID());
		if (it != siblings.end())
			SetSiblingIndex(roots.front(), static_cast<size_t>(std::distance(siblings.begin(), it)) + 1);
		return roots.front();
	}

	Entity Scene::GetEntityByUUID(UUID uuid) const
	{
		auto it = m_EntityMap.find(uuid);
		if (it == m_EntityMap.end())
			return {};
		return Entity(it->second, const_cast<Scene*>(this));
	}

	Entity Scene::FindEntityByName(std::string_view name) const
	{
		for (const Entity entity : GetEntitiesInHierarchyOrder())
		{
			if (entity.GetName() == name)
				return entity;
		}
		return {};
	}

	std::vector<Entity> Scene::FindEntitiesByTag(std::string_view tag) const
	{
		std::vector<Entity> result;
		for (const Entity entity : GetEntitiesInHierarchyOrder())
		{
			const TagComponent* tagComponent = entity.TryGetComponent<TagComponent>();
			if (tagComponent && tagComponent->Tag == tag)
				result.push_back(entity);
		}
		return result;
	}

	std::vector<Entity> Scene::GetEntitiesInHierarchyOrder() const
	{
		// Depth-first, parents before children, iteratively (hierarchies can be arbitrarily deep).
		std::vector<Entity> entities;
		entities.reserve(m_EntityMap.size());
		std::vector<UUID> stack(m_RootEntities.rbegin(), m_RootEntities.rend());
		while (!stack.empty())
		{
			auto it = m_EntityMap.find(stack.back());
			stack.pop_back();
			if (it == m_EntityMap.end())
				continue;

			entities.emplace_back(it->second, const_cast<Scene*>(this));
			const std::vector<UUID>& children = m_Registry.get<RelationshipComponent>(it->second).Children;
			stack.insert(stack.end(), children.rbegin(), children.rend());
		}
		return entities;
	}

	bool Scene::SetParent(Entity child, Entity parent, bool keepWorldTransform)
	{
		if (!child.IsValid() || child.GetScene() != this)
			return false;
		RelationshipComponent& relationship = child.GetComponent<RelationshipComponent>();
		// A cycle needs the new parent below the child; without children nothing is below it (building long chains
		// stays linear).
		if (parent.IsValid() && (parent.GetScene() != this || parent == child || (!relationship.Children.empty() && IsDescendantOf(parent, child))))
			return false;

		const UUID newParent = parent.IsValid() ? parent.GetUUID() : UUID::Null();
		if (relationship.Parent == newParent)
			return true;

		const glm::mat4 worldTransform = keepWorldTransform ? GetWorldTransform(child) : glm::mat4(1.0f);
		RemoveFromParent(child.GetHandle());
		if (parent.IsValid())
		{
			relationship.Parent = newParent;
			parent.GetComponent<RelationshipComponent>().Children.push_back(child.GetUUID());
		}
		else
		{
			m_RootEntities.push_back(child.GetUUID());
		}

		if (keepWorldTransform)
			SetWorldTransform(child, worldTransform);

		// Systems that build state from the hierarchy (physics merges descendants' colliders into a body) learn about the
		// move through the child's on_update signal, emitted once the hierarchy and transform are final.
		m_Registry.patch<RelationshipComponent>(child.GetHandle());
		return true;
	}

	bool Scene::SetSiblingIndex(Entity entity, size_t index)
	{
		if (!entity.IsValid() || entity.GetScene() != this)
			return false;

		const UUID uuid = entity.GetUUID();
		Entity parent = entity.GetParent();
		std::vector<UUID>& siblings = parent.IsValid() ? parent.GetComponent<RelationshipComponent>().Children : m_RootEntities;
		auto it = std::find(siblings.begin(), siblings.end(), uuid);
		if (it == siblings.end())
			return false;

		siblings.erase(it);
		index = std::min(index, siblings.size());
		siblings.insert(siblings.begin() + static_cast<ptrdiff_t>(index), uuid);
		return true;
	}

	bool Scene::IsDescendantOf(Entity entity, Entity ancestor) const
	{
		if (!entity.IsValid() || !ancestor.IsValid())
			return false;

		const UUID ancestorUUID = ancestor.GetUUID();
		UUID current = entity.GetComponent<RelationshipComponent>().Parent;
		while (current.IsValid())
		{
			if (current == ancestorUUID)
				return true;
			auto it = m_EntityMap.find(current);
			if (it == m_EntityMap.end())
				return false;
			current = m_Registry.get<RelationshipComponent>(it->second).Parent;
		}
		return false;
	}

	void Scene::UpdateSubtreeWorldTransforms(entt::entity root, std::vector<HierarchyStackEntry>& stack)
	{
		// Parents before children, iteratively (hierarchies can be arbitrarily deep).
		stack.clear();
		stack.push_back({ root, entt::null });
		while (!stack.empty())
		{
			const HierarchyStackEntry entry = stack.back();
			stack.pop_back();

			glm::mat4 parentMatrix(1.0f);
			bool parentActive = true;
			if (entry.Parent != entt::null)
			{
				const WorldTransformComponent& parent = m_Registry.get<WorldTransformComponent>(entry.Parent);
				parentMatrix = parent.Matrix;
				parentActive = parent.ActiveInHierarchy;
			}
			WorldTransformComponent& worldTransform = m_Registry.get<WorldTransformComponent>(entry.Handle);
			worldTransform.Matrix = parentMatrix * m_Registry.get<TransformComponent>(entry.Handle).GetTransform();
			worldTransform.ActiveInHierarchy = parentActive && !m_Registry.all_of<InactiveComponent>(entry.Handle);

			const std::vector<UUID>& children = m_Registry.get<RelationshipComponent>(entry.Handle).Children;
			for (auto it = children.rbegin(); it != children.rend(); ++it)
			{
				auto child = m_EntityMap.find(*it);
				if (child != m_EntityMap.end())
					stack.push_back({ child->second, entry.Handle });
			}
		}
	}

	void Scene::UpdateWorldTransforms()
	{
		ST_PROFILE_FUNCTION();

		auto updateRoots = [this](size_t begin, size_t end)
		{
			std::vector<HierarchyStackEntry> stack;
			for (size_t index = begin; index < end; index++)
			{
				auto it = m_EntityMap.find(m_RootEntities[index]);
				if (it != m_EntityMap.end())
					UpdateSubtreeWorldTransforms(it->second, stack);
			}
		};

		// Each root subtree touches only its own entities, so subtrees can be processed in parallel.
		if (m_RootEntities.size() >= c_ParallelTransformRootThreshold && JobSystem::IsInitialized())
		{
			JobSystem::ParallelFor(static_cast<uint32_t>(m_RootEntities.size()), 64, [&](uint32_t begin, uint32_t end)
			{
				updateRoots(begin, end);
			});
		}
		else
		{
			updateRoots(0, m_RootEntities.size());
		}
	}

	glm::mat4 Scene::GetWorldTransform(Entity entity) const
	{
		if (!entity.IsValid())
			return glm::mat4(1.0f);

		glm::mat4 result = entity.GetComponent<TransformComponent>().GetTransform();
		UUID parent = entity.GetComponent<RelationshipComponent>().Parent;
		while (parent.IsValid())
		{
			auto it = m_EntityMap.find(parent);
			if (it == m_EntityMap.end())
				break;
			result = m_Registry.get<TransformComponent>(it->second).GetTransform() * result;
			parent = m_Registry.get<RelationshipComponent>(it->second).Parent;
		}
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
		Entity current = entity;
		while (current.IsValid())
		{
			if (current.HasComponent<InactiveComponent>())
				return false;
			current = current.GetParent();
		}
		return entity.IsValid();
	}

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
			return;
		}

		const bool stepping = m_IsPaused && m_StepFrames > 0;
		if (m_IsPaused && !stepping)
		{
			UpdateWorldTransforms();
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
	}

	void Scene::OnUpdateEditor(Timestep)
	{
		ST_PROFILE_FUNCTION();
		UpdateWorldTransforms();
	}

	Entity Scene::GetPrimaryCameraEntity()
	{
		for (const Entity entity : GetEntitiesInHierarchyOrder())
		{
			const CameraComponent* camera = entity.TryGetComponent<CameraComponent>();
			if (camera && camera->Primary && IsActiveInHierarchy(entity))
				return entity;
		}
		return {};
	}

	////////////////////////////////////////////////////////////////////////////////
	// Entity
	////////////////////////////////////////////////////////////////////////////////

	Entity Entity::GetParent() const
	{
		if (!IsValid())
			return {};
		return m_Scene->GetEntityByUUID(GetComponent<RelationshipComponent>().Parent);
	}

	std::vector<Entity> Entity::GetChildren() const
	{
		std::vector<Entity> children;
		if (!IsValid())
			return children;

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
