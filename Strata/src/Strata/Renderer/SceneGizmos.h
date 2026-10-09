#pragma once

#include "Strata/Renderer/DebugDraw.h"

#include <glm/glm.hpp>

namespace Strata
{

	class Scene;

	struct SceneGizmoSettings
	{
		bool Lights = true;
		bool Cameras = true;
		bool Colliders = true;
		float CameraAspectRatio = 16.0f / 9.0f; // Shape of the camera frustums
		float MaxCameraFrustumLength = 10.0f;  // Camera frustums end at their far plane or this far, whichever is nearer
		glm::vec4 CameraColor = { 0.85f, 0.85f, 0.85f, 1.0f };
		glm::vec4 ColliderColor = { 0.3f, 1.0f, 0.45f, 1.0f };
		DebugDrawDepth Depth = DebugDrawDepth::Tested;
	};

	// Adds the editor gizmo shapes of a scene's active components to a debug draw list: directional lights (an arrow along
	// the light), point lights (their range), spot lights (their outer cone), cameras (their view frustum) and physics
	// colliders. Collider shapes follow the entity's scale like rigid bodies do: box extents per axis, sphere radius by the
	// largest axis, capsule radius by the larger of X and Z and its height by Y; mesh colliders show the mesh bounds. Light
	// shapes use the light's color. Reads the cached world transforms: call Scene::UpdateWorldTransforms first after
	// editing the scene.
	void DrawSceneGizmos(DebugDraw& debugDraw, const Scene& scene, const SceneGizmoSettings& settings = {});

}
