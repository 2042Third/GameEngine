#pragma once

#include <Strata/Math/Ray.h>
#include <Strata/Scene/Entity.h>

#include <glm/glm.hpp>

#include <limits>
#include <optional>
#include <span>

namespace Strata
{

	class Scene;

	// Where a ray meets what a scene draws.
	struct SceneRayHit
	{
		Entity HitEntity;                          // The mesh renderer's entity
		glm::vec3 Point = glm::vec3(0.0f);         // World position of the hit
		glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f); // World normal of the surface there (facing the ray's origin)
		float Distance = 0.0f;                      // Along the ray
		bool OnTriangle = false;                    // The mesh's triangles were hit (false: its bounds, the mesh not loaded)
	};

	// Rays against the surfaces the editor places content on: the mesh renderers of a scene, found through its render
	// scene's spatial index (the boxes around the ray) and hit on their loaded meshes' triangles (positions and indices stay
	// on the CPU), or on their bounds while a mesh is not loaded. Main thread; syncs the scene's render scene.
	namespace SceneRaycast
	{

		// The nearest hit within maxDistance of the ray's origin, leaving out the listed entities and their descendants (what
		// is being placed). nullopt when the ray hits nothing.
		std::optional<SceneRayHit> Cast(Scene& scene, const Ray& ray, float maxDistance = std::numeric_limits<float>::max(),
			std::span<const Entity> ignore = {});

	}

}
