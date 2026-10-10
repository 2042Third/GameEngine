#pragma once

#include <doctest/doctest.h>

#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>

#include <random>
#include <string>
#include <vector>

namespace Strata::Tests
{

	// The scene's caches agree with a full recomputation: hierarchy links, sibling positions, lookup indices, activity and
	// every world transform that is not known to be stale.
	inline void CheckSceneCaches(const Scene& scene)
	{
		std::string error;
		CHECK_MESSAGE(scene.ValidateHierarchy(&error), error);
		error.clear();
		CHECK_MESSAGE(scene.ValidateWorldTransforms(&error), error);
	}

	// A transform with a recognizable, non-trivial pose (rotation and non-uniform scale), chosen by `random`.
	inline void RandomizeTransform(Entity entity, std::mt19937& random)
	{
		std::uniform_real_distribution<float> offset(-5.0f, 5.0f);
		std::uniform_real_distribution<float> angle(-3.0f, 3.0f);
		std::uniform_real_distribution<float> scale(0.5f, 1.5f);
		TransformComponent& transform = entity.GetTransform();
		transform.Translation = glm::vec3(offset(random), offset(random), offset(random));
		transform.Rotation = glm::angleAxis(angle(random), glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f)));
		transform.Scale = glm::vec3(scale(random), scale(random), scale(random));
		entity.MarkModified<TransformComponent>();
	}

	// `count` entities with random transforms, each attached under a random earlier entity or left at the top level.
	inline std::vector<Entity> CreateRandomHierarchy(Scene& scene, size_t count, std::mt19937& random)
	{
		std::vector<Entity> entities;
		entities.reserve(count);
		for (size_t index = 0; index < count; index++)
		{
			Entity entity = scene.CreateEntity("Node" + std::to_string(index));
			RandomizeTransform(entity, random);
			if (!entities.empty() && std::uniform_int_distribution<int>(0, 3)(random) != 0)
			{
				const size_t parent = std::uniform_int_distribution<size_t>(0, entities.size() - 1)(random);
				REQUIRE(scene.SetParent(entity, entities[parent], false));
			}
			entities.push_back(entity);
		}
		return entities;
	}

}
