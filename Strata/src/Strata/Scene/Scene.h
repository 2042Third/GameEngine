#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/Timestep.h"
#include "Strata/Core/UUID.h"
#include "Strata/Scene/SceneSystem.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Strata
{

	class Entity;

	struct SceneSettings
	{
		glm::vec3 Gravity = { 0.0f, -9.81f, 0.0f };
		float FixedTimestep = 1.0f / 60.0f;
		uint32_t MaxFixedStepsPerFrame = 8; // Prevents a spiral of death after long frames
	};

	// A world of entities. Owns the EnTT registry, the hierarchy, and (while playing) the runtime systems.
	// All Scene functions must be called on the main thread.
	class Scene
	{
	public:
		explicit Scene(std::string name = "Untitled");
		~Scene();

		Scene(const Scene&) = delete;
		Scene& operator=(const Scene&) = delete;

		// Deep copy preserving entity UUIDs; used to enter play mode without touching the edited scene.
		static Ref<Scene> Copy(const Ref<Scene>& source);

		//////////////////////////////////////////////////////////////////////////
		// Entities
		//////////////////////////////////////////////////////////////////////////

		Entity CreateEntity(const std::string& name = std::string());
		Entity CreateEntityWithUUID(UUID uuid, const std::string& name = std::string());
		Entity CreateChildEntity(Entity parent, const std::string& name = std::string());

		// Destroys the entity and all its descendants. While the scene is updating (scripts, physics callbacks)
		// destruction is deferred to the end of the frame, so handles stay valid for the rest of the frame.
		void DestroyEntity(Entity entity);
		bool IsPendingDestroy(Entity entity) const;
		// Entities whose destruction was deferred to the end of the current update (each with its descendants).
		const std::vector<UUID>& GetPendingDestroys() const { return m_PendingDestroy; }

		// Deep copy of an entity and its descendants with fresh UUIDs, inserted after the original.
		// References between entities inside the copied hierarchy are remapped to the copies.
		Entity DuplicateEntity(Entity entity);

		Entity GetEntityByUUID(UUID uuid) const; // Invalid Entity when not found
		Entity FindEntityByName(std::string_view name) const;
		std::vector<Entity> FindEntitiesByTag(std::string_view tag) const;
		size_t GetEntityCount() const { return m_EntityMap.size(); }

		// Root entities in hierarchy order.
		const std::vector<UUID>& GetRootEntities() const { return m_RootEntities; }
		// Every entity in depth-first hierarchy order (parents before children).
		std::vector<Entity> GetEntitiesInHierarchyOrder() const;

		//////////////////////////////////////////////////////////////////////////
		// Hierarchy and transforms
		//////////////////////////////////////////////////////////////////////////

		// Re-parents child under parent (an invalid parent makes it a root). Returns false if this would create a
		// cycle. With keepWorldTransform the child's world transform is preserved (through SetWorldTransform, which signals
		// the new local transform). A change of parent emits the child's RelationshipComponent on_update signal (the old and
		// new parents' Children lists change silently).
		bool SetParent(Entity child, Entity parent, bool keepWorldTransform = true);
		// Moves an entity to position `index` among its siblings (or among the roots). Only the order changes, so no
		// signal is emitted.
		bool SetSiblingIndex(Entity entity, size_t index);
		bool IsDescendantOf(Entity entity, Entity ancestor) const;

		// Recomputes the cached WorldTransformComponent of every entity (runs every frame).
		void UpdateWorldTransforms();
		// World transform computed from the hierarchy right now (always current, independent of the cache).
		glm::mat4 GetWorldTransform(Entity entity) const;
		// Sets the entity's local transform so that its world transform becomes `worldTransform`, and emits the
		// TransformComponent on_update signal like Entity::MarkModified. Returns false (leaving the entity unchanged, without
		// a signal) if the transform cannot be represented, e.g. under a parent with zero scale.
		bool SetWorldTransform(Entity entity, const glm::mat4& worldTransform);
		// A parent whose world transform's determinant is not above this (in magnitude) cannot be inverted, so the world
		// transforms of its children cannot be set.
		static constexpr float c_MinInvertibleDeterminant = 1.0e-12f;
		bool IsActiveInHierarchy(Entity entity) const;

		//////////////////////////////////////////////////////////////////////////
		// Simulation
		//////////////////////////////////////////////////////////////////////////

		void OnRuntimeStart(SceneRuntimeMode mode = SceneRuntimeMode::Play);
		void OnRuntimeStop();
		void OnUpdateRuntime(Timestep timestep);
		void OnUpdateEditor(Timestep timestep);

		bool IsRunning() const { return m_IsRunning; }
		SceneRuntimeMode GetRuntimeMode() const { return m_RuntimeMode; }
		bool IsUpdating() const { return m_IsUpdating; }
		// Pausing stops the updates of the running systems and tells them (SceneSystem::OnPausedChanged). Starting the scene
		// resets it to unpaused.
		void SetPaused(bool paused);
		bool IsPaused() const { return m_IsPaused; }
		// While paused, lets the next `frames` updates each advance the simulation by exactly one fixed step.
		void Step(uint32_t frames = 1) { m_StepFrames += frames; }

		// Simulation time since OnRuntimeStart (seconds) and number of runtime updates.
		double GetTime() const { return m_Time; }
		uint64_t GetFrameIndex() const { return m_FrameIndex; }
		float GetTimeScale() const { return m_TimeScale; }
		void SetTimeScale(float timeScale) { m_TimeScale = std::max(0.0f, timeScale); }

		template<typename T>
		T* GetSystem() const
		{
			for (const Scope<SceneSystem>& system : m_Systems)
			{
				if (T* typed = dynamic_cast<T*>(system.get()))
					return typed;
			}
			return nullptr;
		}

		Entity GetPrimaryCameraEntity();

		const std::string& GetName() const { return m_Name; }
		void SetName(const std::string& name) { m_Name = name; }
		SceneSettings& GetSettings() { return m_Settings; }
		const SceneSettings& GetSettings() const { return m_Settings; }

		entt::registry& GetRegistry() { return m_Registry; }
		const entt::registry& GetRegistry() const { return m_Registry; }

		template<typename... Components>
		auto GetAllEntitiesWith()
		{
			return m_Registry.view<Components...>();
		}
	private:
		void DestroyEntityImmediate(entt::entity handle);
		// Lets running systems react to the subtree's destruction (SceneSystem::OnEntityDestroying).
		void NotifyEntitiesDestroying(entt::entity root);
		// The entity and its descendants in depth-first hierarchy order (parents before children).
		std::vector<entt::entity> CollectSubtree(entt::entity root) const;
		void FlushPendingDestroys();
		void RemoveFromParent(entt::entity handle);
		struct HierarchyStackEntry
		{
			entt::entity Handle;
			entt::entity Parent; // entt::null for the subtree root
		};
		void UpdateSubtreeWorldTransforms(entt::entity root, std::vector<HierarchyStackEntry>& stack);
	private:
		std::string m_Name;
		entt::registry m_Registry;
		std::unordered_map<UUID, entt::entity> m_EntityMap;
		std::vector<UUID> m_RootEntities;
		SceneSettings m_Settings;

		std::vector<Scope<SceneSystem>> m_Systems;
		std::vector<UUID> m_PendingDestroy;
		std::unordered_set<UUID> m_PendingDestroySet;
		SceneRuntimeMode m_RuntimeMode = SceneRuntimeMode::Play;
		bool m_IsRunning = false;
		bool m_IsUpdating = false;
		bool m_IsPaused = false;
		uint32_t m_StepFrames = 0;
		float m_FixedTimeAccumulator = 0.0f;
		float m_TimeScale = 1.0f;
		double m_Time = 0.0;
		uint64_t m_FrameIndex = 0;

		friend class Entity;
		friend class SceneSerializer;
	};

}
