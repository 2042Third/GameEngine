#pragma once

#include "Strata/Renderer/SceneRenderData.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace Strata
{

	// What a frame lays the sun's shadow cascades out for (SceneRenderer::FindShadowCasters): the camera's lens and turn, and
	// the cascades' settings. The camera's position and the light's direction place the layout's squares (ShadowCascades::
	// GetSquare), never its sizes.
	struct ShadowCascadeView
	{
		// From view space to world space (the inverse view's rotation): where the slices' spheres lie around the camera.
		glm::mat3 CameraRotation = glm::mat3(1.0f);
		float Near = 0.1f;               // The camera's near distance (at least 0.01)
		float ShadowDistance = 80.0f;    // D: where the last cascade ends (beyond Near)
		bool Orthographic = false;
		float TanHalfFOV = 0.0f;         // Perspective cameras: tan(vertical field of view / 2)
		float OrthographicHeight = 0.0f; // Orthographic cameras: the view's height
		float Aspect = 1.0f;
		uint32_t CascadeCount = 4;
		uint32_t MapSize = 2048;         // The cascades' texels each way
		float CascadeBlend = 0.1f;       // DirectionalLightComponent::CascadeBlend
		float Distribution = 0.9f;       // DirectionalLightComponent::CascadeDistribution: the splits' logarithmic share
	};

	// A cascade of a layout: its slice of view distances (the near end includes the band in which the previous cascade hands
	// over), the smallest sphere around that slice of the view, and the texels of the square that covers the sphere.
	struct ShadowCascadeSlice
	{
		float SliceNear = 0.0f;
		float SliceFar = 0.0f;                    // Its split
		glm::vec3 SphereCenter = glm::vec3(0.0f); // Relative to the camera, along the world's axes
		float Radius = 0.0f;                      // Rounded up to 1/16
		double WorldPerTexel = 0.0;               // The square's side over the map's texels

		bool operator==(const ShadowCascadeSlice&) const = default;
	};

	struct ShadowCascadeLayout
	{
		uint32_t CascadeCount = 0;
		std::array<ShadowCascadeSlice, RenderData::c_MaxCascades> Cascades = {};

		bool operator==(const ShadowCascadeLayout&) const = default;
	};

	// A cascade's square in absolute light view space (ShadowCascades::GetSquare).
	struct ShadowCascadeSquare
	{
		glm::dvec2 Center = glm::dvec2(0.0); // On a corner of the square's texels
		double HalfSide = 0.0;
		double SphereZ = 0.0;                // The sphere's center along the light (its depth range is the sphere's)
	};

	// Stable cascaded shadow maps (Docs/DeveloperGuide.md, "Sun shadows"): the slices split the view distances by a fixed rule
	// (the light's shadow distance and cascade distribution, the quality's cascade count), each cascade covers the smallest
	// sphere around its slice of the view, whose size the camera's turn and position never change, and its square snaps to
	// whole texels of light space as the camera moves. So the cascades depend on the frame's view alone: a turned camera's
	// first frame is final, and a moving one's shadows stay where they lie. Pure C++: SceneRenderer feeds it.
	namespace ShadowCascades
	{
		// Where the splits' logarithmic share starts, in meters from the camera (the first cascade still starts at the near
		// clip): at most half the shadow distance. 1.5 m ends the first cascade where 0.7.0's fit to a person's view did (5.8 m
		// at a shadow distance of 80 m), so thin casters a few meters away keep its finest texels.
		constexpr float c_SplitStart = 1.5f;
		// The cascades the logarithmic share splits; a layout of more (five: High and Ultra) puts the others before them, near the
		// camera (c_MaxNearRadius).
		constexpr uint32_t c_SharedCascades = 4;
		// The share's first cascade ends at most this far: a long shadow distance's share would otherwise push it out (9.7 m at
		// 200 m) and coarsen the shadows nearest the camera, where curved surfaces at a grazing sun show the texels as stripes.
		// (Capping it by its sphere's radius as well, 7 m, was measured and left out: at 70 degrees and 200 m it ended it at
		// 4.9 m and drew 4.9-7.5 m with the next cascade's 3.4 times coarser texels, which changed the penumbrae there; the
		// near cascade sharpens what lies within arm's reach instead.)
		constexpr float c_MaxFirstSplit = 7.5f;
		// A near cascade (the fifth) ends at c_SplitStart, or nearer where its sphere would exceed this radius (meters): at 8192
		// texels at most 0.49 mm for what lies within arm's reach (1.5 m and 0.45 mm at 60 degrees, 1.4 m at 70). The share's
		// first cascade keeps its sphere (wide views center it at its far end), so nothing beyond the near one changes.
		constexpr float c_MaxNearRadius = 2.0f;
		// How far inside its square's edges a cascade holds a receiver, in its texels (Include/Shadows.slang,
		// c_ShadowCascadeMarginTexels): the shadow filter's reach, so a receiver it holds is filtered from its own square. Each
		// square covers its sphere by this and the texel its snapping may move it.
		constexpr uint32_t c_FilterReachTexels = 33;

		// The layout of a view: `count` slices from the near clip to the shadow distance, each blending into the next over the
		// last CascadeBlend of its slice (the next slice starts at that band).
		ShadowCascadeLayout GetLayout(const ShadowCascadeView& view);
		// Split `index` (its far end) of `count` slices over [rangeNear, rangeFar] with a logarithmic share `distribution`.
		float GetSplit(float rangeNear, float rangeFar, float distribution, uint32_t index, uint32_t count);
		// Split `index` of a view's layout. The last c_SharedCascades slices (all of fewer) share [c_SplitStart, ShadowDistance] by
		// GetSplit with the view's Distribution, the first of them ending at most at c_MaxFirstSplit; a cascade before them ends
		// at c_SplitStart, at most at GetRadiusLimit(c_MaxNearRadius); the last one ends at the shadow distance.
		float GetSplit(const ShadowCascadeView& view, uint32_t index);
		// The farthest view distance (at most the shadow distance) whose slice from the near clip has a sphere of at most `radius`:
		// the lens's alone, so the cap never changes as the camera turns. Infinity where even the thinnest slice's is larger (a
		// wide orthographic view), which no cap can hold.
		float GetRadiusLimit(const ShadowCascadeView& view, float radius);
		// The smallest sphere around the view's slice [sliceNear, sliceFar], relative to the camera (radius rounded up to 1/16).
		void GetSliceSphere(const ShadowCascadeView& view, float sliceNear, float sliceFar, glm::vec3& center, float& radius);
		// A cascade's square for a camera at `cameraLight` (its position in light view space, in double): the sphere's center on
		// whole texels, so the square moves texel by texel with the camera.
		ShadowCascadeSquare GetSquare(const ShadowCascadeSlice& slice, const glm::dvec3& cameraLight, const glm::dmat3& lightRotation, uint32_t mapSize);
	}

}
