#pragma once

#include "Strata/Core/JobSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Math/Math.h"
#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Physics/PhysicsWorld.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace Strata::Tests
{

	// Advances a running scene by `steps` fixed steps (each update covers exactly one fixed timestep).
	inline void StepScene(Scene& scene, uint32_t steps)
	{
		const float timestep = scene.GetSettings().FixedTimestep;
		for (uint32_t step = 0; step < steps; step++)
			scene.OnUpdateRuntime(timestep);
	}

	inline PhysicsSystem& GetPhysics(Scene& scene)
	{
		PhysicsSystem* physics = scene.GetSystem<PhysicsSystem>();
		REQUIRE(physics != nullptr);
		return *physics;
	}

	inline glm::vec3 GetWorldPosition(const Scene& scene, Entity entity)
	{
		return glm::vec3(scene.GetWorldTransform(entity)[3]);
	}

	inline glm::quat GetWorldRotation(const Scene& scene, Entity entity)
	{
		glm::vec3 translation;
		glm::quat rotation;
		glm::vec3 scale;
		REQUIRE(Math::DecomposeTransform(scene.GetWorldTransform(entity), translation, rotation, scale));
		return rotation;
	}

	inline bool IsFinite(const glm::vec3& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
	}

	inline Entity CreateStaticBox(Scene& scene, const std::string& name, const glm::vec3& position, const glm::vec3& halfExtents)
	{
		Entity entity = scene.CreateEntity(name);
		entity.GetTransform().Translation = position;
		entity.AddComponent<BoxColliderComponent>().HalfExtents = halfExtents;
		return entity;
	}

	// A large static box whose top face is the plane y = 0.
	inline Entity CreateGround(Scene& scene, const std::string& name = "Ground")
	{
		return CreateStaticBox(scene, name, glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(50.0f, 0.5f, 50.0f));
	}

	inline Entity CreateDynamicBox(Scene& scene, const std::string& name, const glm::vec3& position, const glm::vec3& halfExtents = glm::vec3(0.5f))
	{
		Entity entity = scene.CreateEntity(name);
		entity.GetTransform().Translation = position;
		entity.AddComponent<RigidBodyComponent>();
		entity.AddComponent<BoxColliderComponent>().HalfExtents = halfExtents;
		return entity;
	}

	inline Entity CreateDynamicSphere(Scene& scene, const std::string& name, const glm::vec3& position, float radius = 0.5f)
	{
		Entity entity = scene.CreateEntity(name);
		entity.GetTransform().Translation = position;
		entity.AddComponent<RigidBodyComponent>();
		entity.AddComponent<SphereColliderComponent>().Radius = radius;
		return entity;
	}

	// Collects every collision event of a physics system. The event list is shared with the listener, so it stays
	// valid however the recorder and the system are destroyed.
	class CollisionRecorder
	{
	public:
		explicit CollisionRecorder(PhysicsSystem& physics)
			: m_Events(CreateRef<std::vector<CollisionEvent>>())
		{
			Ref<std::vector<CollisionEvent>> events = m_Events;
			m_ListenerID = physics.AddCollisionListener([events](const CollisionEvent& event) { events->push_back(event); });
		}

		CollisionListenerID GetListenerID() const { return m_ListenerID; }
		const std::vector<CollisionEvent>& GetEvents() const { return *m_Events; }
		void Clear() { m_Events->clear(); }

		size_t Count(CollisionEventType type) const
		{
			return static_cast<size_t>(std::count_if(m_Events->begin(), m_Events->end(), [type](const CollisionEvent& event) { return event.Type == type; }));
		}

		// Events of the given type between two entities, in either order.
		size_t Count(CollisionEventType type, UUID a, UUID b) const
		{
			return static_cast<size_t>(std::count_if(m_Events->begin(), m_Events->end(), [&](const CollisionEvent& event)
			{
				return event.Type == type && ((event.AID == a && event.BID == b) || (event.AID == b && event.BID == a));
			}));
		}

		const CollisionEvent* Find(CollisionEventType type, UUID a, UUID b) const
		{
			for (const CollisionEvent& event : *m_Events)
			{
				if (event.Type == type && ((event.AID == a && event.BID == b) || (event.AID == b && event.BID == a)))
					return &event;
			}
			return nullptr;
		}
	private:
		Ref<std::vector<CollisionEvent>> m_Events;
		CollisionListenerID m_ListenerID = c_InvalidCollisionListener;
	};

	// Number of log messages containing `text` logged after `afterSequence` (see LogBuffer::GetLatestSequence).
	inline size_t CountLogMessages(uint64_t afterSequence, std::string_view text)
	{
		size_t count = 0;
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(afterSequence))
		{
			if (entry.Message.find(text) != std::string::npos)
				count++;
		}
		return count;
	}

	class ScopedMeshProvider
	{
	public:
		explicit ScopedMeshProvider(PhysicsMeshProvider provider)
		{
			PhysicsWorld::SetMeshProvider(std::move(provider));
		}

		~ScopedMeshProvider()
		{
			PhysicsWorld::SetMeshProvider({});
		}

		ScopedMeshProvider(const ScopedMeshProvider&) = delete;
		ScopedMeshProvider& operator=(const ScopedMeshProvider&) = delete;
	};

	class ScopedPhysicsSettings
	{
	public:
		explicit ScopedPhysicsSettings(const PhysicsSettings& settings)
			: m_Previous(PhysicsSystem::GetDefaultSettings())
		{
			PhysicsSystem::SetDefaultSettings(settings);
		}

		~ScopedPhysicsSettings()
		{
			PhysicsSystem::SetDefaultSettings(m_Previous);
		}

		ScopedPhysicsSettings(const ScopedPhysicsSettings&) = delete;
		ScopedPhysicsSettings& operator=(const ScopedPhysicsSettings&) = delete;
	private:
		PhysicsSettings m_Previous;
	};

	class ScopedJobSystem
	{
	public:
		explicit ScopedJobSystem(uint32_t workerThreads)
		{
			JobSystemSpecification specification;
			specification.WorkerThreadCount = workerThreads;
			specification.IOThreadCount = 0;
			JobSystem::Init(specification);
		}

		~ScopedJobSystem()
		{
			JobSystem::Shutdown();
		}

		ScopedJobSystem(const ScopedJobSystem&) = delete;
		ScopedJobSystem& operator=(const ScopedJobSystem&) = delete;
	};

}
