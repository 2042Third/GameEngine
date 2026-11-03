#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>

namespace Strata
{

	// The types of a scene's interpolated set: what the fixed steps moved, drawn between its poses before and after them
	// (Scene::GetDrawnTransform; Docs/DeveloperGuide.md, "Interpolation").
	inline constexpr uint32_t c_NoInterpolationSlot = ~0u;

	// How an entity of the interpolated set is drawn.
	enum class InterpolationRole : uint8_t
	{
		Blend,   // Its own transform changed in the fixed steps: drawn between its world pose before them and after them
		Compose, // Moved only with an ancestor: its parent's drawn transform times its current local transform
		Exact    // Drawn where it is: a teleport, a per-frame move, a reparent, shear, or new without an interpolated parent
	};

	// A world pose in parts, as the blend reads them.
	struct InterpolationPose
	{
		glm::vec3 Translation = glm::vec3(0.0f);
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec3 Scale = glm::vec3(1.0f);
	};

	// An entry of the set besides its entity and drawn matrix (Scene keeps the three in parallel arrays, by slot).
	struct InterpolatedEntry
	{
		uint32_t ParentSlot = c_NoInterpolationSlot; // Its hierarchy parent's slot when the parent is in the set
		InterpolationRole Role = InterpolationRole::Exact;
		InterpolationPose From; // Blend: the world pose before the stepping frame's steps
		InterpolationPose To;   // Blend: after them; per-frame rotation and scale writes keep it current
	};

	// The set's size by role (Scene::GetInterpolationStats: for commands and tests, it walks the set).
	struct InterpolationStats
	{
		uint32_t Entities = 0;
		uint32_t Blended = 0;
		uint32_t Composed = 0;
		uint32_t Exact = 0;
	};

}
