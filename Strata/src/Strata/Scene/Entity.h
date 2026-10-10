#pragma once

#include "Strata/Core/Assert.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

#include <entt/entt.hpp>

namespace Strata
{

	// Lightweight handle to an entity in a scene (an EnTT entity plus its scene). Copy freely; it does not own
	// the entity. Handles of destroyed entities become invalid (IsValid() returns false).
	class Entity
	{
	public:
		Entity() = default;
		Entity(entt::entity handle, Scene* scene)
			: m_EntityHandle(handle), m_Scene(scene)
		{
		}

		// Returns the new component (nothing for empty tag components, which carry no data).
		template<typename T, typename... Args>
		decltype(auto) AddComponent(Args&&... args)
		{
			ST_CORE_ASSERT(!HasComponent<T>(), "Entity already has the component");
			return m_Scene->m_Registry.emplace<T>(m_EntityHandle, std::forward<Args>(args)...);
		}

		template<typename T, typename... Args>
		decltype(auto) AddOrReplaceComponent(Args&&... args)
		{
			return m_Scene->m_Registry.emplace_or_replace<T>(m_EntityHandle, std::forward<Args>(args)...);
		}

		template<typename T>
		T& GetComponent()
		{
			static_assert(!std::is_empty_v<T>, "Tag components carry no data; use HasComponent");
			ST_CORE_ASSERT(HasComponent<T>(), "Entity does not have the component");
			return m_Scene->m_Registry.get<T>(m_EntityHandle);
		}

		template<typename T>
		const T& GetComponent() const
		{
			static_assert(!std::is_empty_v<T>, "Tag components carry no data; use HasComponent");
			ST_CORE_ASSERT(HasComponent<T>(), "Entity does not have the component");
			return m_Scene->m_Registry.get<T>(m_EntityHandle);
		}

		template<typename T>
		T* TryGetComponent()
		{
			return IsValid() ? m_Scene->m_Registry.try_get<T>(m_EntityHandle) : nullptr;
		}

		template<typename T>
		const T* TryGetComponent() const
		{
			return IsValid() ? m_Scene->m_Registry.try_get<T>(m_EntityHandle) : nullptr;
		}

		template<typename T>
		bool HasComponent() const
		{
			return IsValid() && m_Scene->m_Registry.all_of<T>(m_EntityHandle);
		}

		template<typename... T>
		bool HasAnyComponent() const
		{
			return IsValid() && m_Scene->m_Registry.any_of<T...>(m_EntityHandle);
		}

		template<typename T>
		void RemoveComponent()
		{
			ST_CORE_ASSERT(HasComponent<T>(), "Entity does not have the component");
			m_Scene->m_Registry.remove<T>(m_EntityHandle);
		}

		// Notifies systems (through the registry's on_update signal) that a component was modified in place.
		template<typename T>
		void MarkModified()
		{
			if (HasComponent<T>())
				m_Scene->m_Registry.patch<T>(m_EntityHandle);
		}

		UUID GetUUID() const { return GetComponent<IDComponent>().ID; }
		const std::string& GetName() const { return GetComponent<NameComponent>().Name; }
		// Writing the returned component requires MarkModified<TransformComponent>() afterwards, so that the scene recomputes
		// the cached world transforms (the transform contract, see Scene).
		TransformComponent& GetTransform() { return GetComponent<TransformComponent>(); }

		Entity GetParent() const;
		std::vector<Entity> GetChildren() const;
		bool IsActive() const { return !HasComponent<InactiveComponent>(); }
		void SetActive(bool active);

		bool IsValid() const { return m_Scene && m_Scene->m_Registry.valid(m_EntityHandle); }
		explicit operator bool() const { return IsValid(); }
		operator entt::entity() const { return m_EntityHandle; }
		entt::entity GetHandle() const { return m_EntityHandle; }
		Scene* GetScene() const { return m_Scene; }

		bool operator==(const Entity& other) const { return m_EntityHandle == other.m_EntityHandle && m_Scene == other.m_Scene; }
		bool operator!=(const Entity& other) const { return !(*this == other); }
	private:
		entt::entity m_EntityHandle = entt::null;
		Scene* m_Scene = nullptr;
	};

}
