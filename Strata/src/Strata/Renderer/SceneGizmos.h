#pragma once

#include "Strata/Renderer/DebugDraw.h"
#include "Strata/Scene/Entity.h"

#include <glm/glm.hpp>

#include <optional>
#include <span>

namespace Strata
{

	class Scene;

	// The view scene gizmos are drawn for (SceneGizmoSettings::View).
	struct SceneGizmoView
	{
		glm::mat4 ViewProjection = glm::mat4(1.0f); // Strata clip space (reversed-Z)
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Right = glm::vec3(1.0f, 0.0f, 0.0f); // World directions of the image's +X and +Y
		glm::vec3 Up = glm::vec3(0.0f, 1.0f, 0.0f);
		// Vertical field of view in radians, or the vertical extent in world units of an orthographic view.
		float VerticalFOV = glm::radians(60.0f);
		bool Orthographic = false;
		float ViewportHeight = 720.0f; // Pixels
	};

	struct SceneGizmoSettings
	{
		bool Lights = true;
		bool Cameras = true;
		bool Colliders = true;
		// Colliders of every entity, or (false) only of ColliderEntities, e.g. the selection, sorted by entity handle.
		bool AllColliders = true;
		std::span<const Entity> ColliderEntities;
		// The range spheres of point lights and the cones of spot lights for every light, or (false) only for
		// LightRangeEntities, e.g. the selection, sorted by entity handle: with a view the other lights are drawn as their
		// icons (a light's range would cover what is near it), without one they are left out.
		bool AllLightRanges = true;
		std::span<const Entity> LightRangeEntities;
		float CameraAspectRatio = 16.0f / 9.0f; // Shape of the camera frustums
		float MaxCameraFrustumLength = 10.0f;  // Camera frustums end at their far plane or this far, whichever is nearer
		glm::vec4 CameraColor = { 0.85f, 0.85f, 0.85f, 1.0f };
		glm::vec4 ColliderColor = { 0.3f, 1.0f, 0.45f, 1.0f };
		DebugDrawDepth Depth = DebugDrawDepth::Tested;

		// With a view, shapes outside its frustum or further than MaxDistance from it are left out, and lights and cameras
		// whose shape would be smaller on screen than MinShapePixels (too small to read) are drawn as icons of IconPixels
		// instead (a few lines that face the view), so a scene with thousands of lights costs what is near and in view.
		std::optional<SceneGizmoView> View;
		float MaxDistance = 500.0f;
		float MinShapePixels = 128.0f;
		float IconPixels = 24.0f;
	};

	// Adds the editor gizmo shapes of a scene's active components to a debug draw list: directional lights (an arrow along
	// the light), point lights (their range), spot lights (their outer cone), cameras (their view frustum) and physics
	// colliders. Collider shapes follow the entity's scale like rigid bodies do: box extents per axis, sphere radius by the
	// largest axis, capsule radius by the larger of X and Z and its height by Y; mesh colliders show the mesh bounds. Light
	// shapes use the light's color. Reads the cached world transforms: call Scene::UpdateWorldTransforms first after
	// editing the scene.
	void DrawSceneGizmos(DebugDraw& debugDraw, const Scene& scene, const SceneGizmoSettings& settings = {});

}
