#pragma once

#include <doctest/doctest.h>

#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSystem.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

// Helpers of the interpolation tests (Scene.Timing, Physics.Interpolation, Animation.Interpolation, the GPU cases): a
// scene system that moves entities in OnFixedUpdate as physics' write-back does, and hooks into a running scene's
// callbacks.

namespace Strata::Tests
{

	// An entity FixedMoverSystem moves every fixed step: Translation += Velocity * step, Rotation turned by TurnRate
	// (radians per second) about Axis.
	struct FixedMover
	{
		entt::entity Entity = entt::null;
		glm::vec3 Velocity = glm::vec3(0.0f);
		float TurnRate = 0.0f;
		glm::vec3 Axis = glm::vec3(0.0f, 1.0f, 0.0f);
	};

	// Moves each scene's listed movers in OnFixedUpdate (after the systems registered before it, physics among them), with one
	// Scene::MarkTransformsChanged for all and no on_update signal, as physics writes its poses back. The hooks, when set,
	// run in the scene's OnUpdate, after the moves of OnFixedUpdate, and in OnLateUpdate.
	class FixedMoverSystem : public SceneSystem
	{
	public:
		explicit FixedMoverSystem(Scene& scene)
			: m_Scene(scene)
		{
		}

		void OnUpdate(Timestep timestep) override
		{
			if (UpdateHook)
				UpdateHook(m_Scene, timestep);
		}

		void OnFixedUpdate(float step) override
		{
			auto it = Movers.find(&m_Scene);
			if (it != Movers.end())
			{
				entt::registry& registry = m_Scene.GetRegistry();
				m_Moved.clear();
				for (const FixedMover& mover : it->second)
				{
					if (!registry.valid(mover.Entity))
						continue;
					TransformComponent& transform = registry.get<TransformComponent>(mover.Entity);
					transform.Translation += mover.Velocity * step;
					if (mover.TurnRate != 0.0f)
						transform.Rotation = glm::normalize(glm::angleAxis(mover.TurnRate * step, mover.Axis) * transform.Rotation);
					m_Moved.push_back(mover.Entity);
				}
				m_Scene.MarkTransformsChanged(m_Moved);
			}
			if (FixedHook)
				FixedHook(m_Scene, step);
		}

		void OnLateUpdate(Timestep timestep) override
		{
			if (LateHook)
				LateHook(m_Scene, timestep);
		}

		static inline std::unordered_map<const Scene*, std::vector<FixedMover>> Movers;
		static inline std::function<void(Scene&, Timestep)> UpdateHook;
		static inline std::function<void(Scene&, float)> FixedHook;
		static inline std::function<void(Scene&, Timestep)> LateHook;
	private:
		Scene& m_Scene;
		std::vector<entt::entity> m_Moved;
	};

	// Registers FixedMoverSystem for the scope (start scenes inside it), with no movers and no hooks.
	struct ScopedFixedMover
	{
		ScopedFixedMover()
		{
			Reset();
			REQUIRE(SceneSystemRegistry::Register(MakeSceneSystemDescriptor<FixedMoverSystem>("TestFixedMover", true,
				[](Scene& scene) { return CreateScope<FixedMoverSystem>(scene); })));
		}

		~ScopedFixedMover()
		{
			Reset();
			CHECK(SceneSystemRegistry::Unregister("TestFixedMover"));
		}

		static void Reset()
		{
			FixedMoverSystem::Movers.clear();
			FixedMoverSystem::UpdateHook = nullptr;
			FixedMoverSystem::FixedHook = nullptr;
			FixedMoverSystem::LateHook = nullptr;
		}
	};

	inline void AddFixedMover(Scene& scene, Entity entity, const glm::vec3& velocity, float turnRate = 0.0f,
		const glm::vec3& axis = glm::vec3(0.0f, 1.0f, 0.0f))
	{
		FixedMoverSystem::Movers[&scene].push_back(FixedMover { entity.GetHandle(), velocity, turnRate, axis });
	}

	inline void RemoveFixedMover(Scene& scene, Entity entity)
	{
		std::erase_if(FixedMoverSystem::Movers[&scene], [&](const FixedMover& mover) { return mover.Entity == entity.GetHandle(); });
	}

	inline bool SameMatrix(const glm::mat4& a, const glm::mat4& b)
	{
		return std::memcmp(&a, &b, sizeof(glm::mat4)) == 0;
	}

	// What a renderer draws the entity at this frame: the scene's drawn matrices brought up to date, as RenderScene::Sync does.
	inline glm::mat4 GetRenderedTransform(Scene& scene, Entity entity)
	{
		scene.UpdateDrawnTransforms();
		if (const glm::mat4* drawn = scene.FindDrawnTransform(entity.GetHandle()))
			return *drawn;
		return scene.GetWorldTransform(entity);
	}

}
