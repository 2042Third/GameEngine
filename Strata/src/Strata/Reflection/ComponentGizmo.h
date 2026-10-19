#pragma once

#include "Strata/Asset/AssetTypes.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>

namespace Strata
{

	// The editor gizmos a component asks for: which kind of shape it is drawn as (DrawSceneGizmos, Renderer/SceneGizmos.h)
	// and when. The module that owns the component fills its shape (ComponentInfoBuilder::Gizmo), so the renderer draws
	// every module's components without knowing their types.
	enum class ComponentGizmoGroup : uint8_t
	{
		None = 0,
		Collider // Drawn with the colliders (SceneGizmoSettings::Colliders), in the collider color
	};

	// A gizmo shape in world space.
	struct ComponentGizmoShape
	{
		enum class Kind : uint8_t
		{
			Box,       // A box of HalfExtents around Transform's origin, along its axes
			Sphere,    // A sphere of Radius around Transform's origin
			Capsule,   // A capsule of Radius along Transform's Y axis, its cylinder 2 * HalfHeight long
			MeshBounds // The bounds of Mesh (when it is loaded: gizmos never load assets), placed by Transform
		};

		Kind Type = Kind::Box;
		glm::mat4 Transform = glm::mat4(1.0f);
		glm::vec3 HalfExtents = glm::vec3(0.0f);
		float Radius = 0.0f;
		float HalfHeight = 0.0f;
		AssetHandle Mesh = AssetHandle::Null();
	};

	// Fills the shape of `component` (of the type it was registered for) on `entity`, whose world transform is `world`;
	// false when there is nothing to draw (a degenerate transform, a component without a shape).
	using ComponentGizmoFunction = bool (*)(const entt::registry& registry, entt::entity entity, const void* component, const glm::mat4& world,
		ComponentGizmoShape& outShape);

}
