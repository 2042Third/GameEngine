#pragma once

#include <Strata/Renderer/SceneRenderer.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Strata
{

	class Scene;

	// What lighting.bakeProbes renders a reflection probe's capture with (EditorLightingCommands.cpp): six square views from the
	// probe's position, read back as HDR and put together into an equirectangular image (Include/IBL.glsl's mapping, which the
	// renderer's probes read), written as a Radiance .hdr file.
	namespace ProbeBake
	{

		constexpr uint32_t c_FaceSize = 256;       // Twice the renderer's probe cubes (RenderData::c_ReflectionProbeSize)
		constexpr uint32_t c_CaptureWidth = 1024;  // The equirectangular image keeps the faces' detail
		constexpr uint32_t c_CaptureHeight = 512;
		constexpr float c_Near = 0.05f;            // Meters
		constexpr float c_Far = 20000.0f;          // A world's extent (WorldData::c_SupportedExtent)

		// The view of face `face` (+X, -X, +Y, -Y, +Z, -Z) of a probe at `position`: 90 degrees, square, looking along the axis.
		SceneCamera GetFaceCamera(const glm::vec3& position, uint32_t face);

		// The equirectangular image (c_CaptureWidth x c_CaptureHeight linear RGB floats, rows top first) of six faces rendered with
		// GetFaceCamera (each c_FaceSize x c_FaceSize RGB floats, rows top first), each value times `scale` (the inverse of the
		// pre-exposure they were rendered with): every texel's direction is looked up, bilinearly, in the face it lies in.
		std::vector<float> AssembleEquirect(const std::array<std::vector<float>, 6>& faces, float scale);

		// The pre-exposure a bake renders with: a power of two that brings the scene's brightest directional light's illuminance
		// (lux) to about 1, so sunlit surfaces stay well within half floats (1 for scenes lit by less).
		float ChoosePreExposure(Scene& scene);

		// The renderer a bake renders with: no entity IDs, temporal anti-aliasing or texture streaming (it waits for every level),
		// every shadow view and probe conversion in its first frame, no fades.
		SceneRendererSpecification GetRendererSpecification();

	}

}
