#pragma once

#include "Renderer/FlickerMetrics.h"
#include "Renderer/SceneRendererTestUtils.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>

// What the reflections' tests measure (suite Renderer.ReflectionMetrics): the look-dev harness's noise and screen-locked
// pattern over a rectangle of an 8-bit image, and the specular lab the strafe test (GPU.SceneRenderer.SSR) moves through.
namespace Strata::Tests
{

	// The look-dev harness's noise (Tools/LookDev.py "Noise") over a rectangle of an 8-bit image, in percent of full scale.
	struct NoiseMeasure
	{
		float Noise = 0.0f;    // RMS of the encoded luma minus its 9x9 mean
		float Pattern3 = 0.0f; // RMS over the 9 phases of (x mod 3, y mod 3) of the residual's mean
		float Pattern4 = 0.0f; // ...over the 16 phases of (x mod 4, y mod 4)
		uint32_t Pixels = 0;
	};

	// rect: x0, y0, x1, y1 (pixels, exclusive end). Only pixels whose 9x9 window lies inside the rectangle are measured.
	NoiseMeasure MeasureNoise(const ReadbackImage& image, const glm::uvec4& rect);

	// A measured surface: a rectangle of a center and two half axes; its normal is their cross product.
	struct MeasuredRectangle
	{
		glm::vec3 Center = glm::vec3(0.0f);
		glm::vec3 AxisU = glm::vec3(1.0f, 0.0f, 0.0f);
		glm::vec3 AxisV = glm::vec3(0.0f, 1.0f, 0.0f);
	};

	// The specular lab of the look-dev scene Scenes/Specular.stscene (Tools/LookDev/LookDevScene.json: change both), without its
	// tanks, probe and bulbs: a rough metal sheet beside a white box, three walls, six thin posts, four crates, the roughness
	// ladder of spheres and a wet (smooth, dark dielectric) plane on the ground, under a two-tone sky and a shadowed sun at the
	// tests' scale, with neutral post-processing and screen-space reflections on. Its two lanterns (the look-dev scene's point
	// lights, 60 cd, range 18, without shadows here) are inactive until a test turns them on.
	struct SpecularLab
	{
		Scene Contents;
		Entity Sun;
		Entity Sky;
		Entity Lanterns[2];

		explicit SpecularLab(SceneTestAssets& assets);

		// The sheet's face toward the camera (+Z) and the wet plane's top (+Y).
		static MeasuredRectangle GetSheetFace() { return { glm::vec3(8.0f, 1.7f, -4.75f), glm::vec3(6.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.5f, 0.0f) }; }
		static MeasuredRectangle GetWetPlane() { return { glm::vec3(-14.0f, 0.01f, 14.0f), glm::vec3(12.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -8.0f) }; }
	};

	// Leaves only a rectangle's pixels measured (FlickerFrame::ViewDepth 0 elsewhere): where the frame's ray meets the rectangle
	// at least `inset` pixels inside its outline, and nothing drawn in front of it. Returns the pixels kept.
	size_t MaskFlickerFrame(FlickerFrame& frame, const MeasuredRectangle& rectangle, uint32_t inset = 8);

}
