#pragma once

#include "Strata/Renderer/SceneRenderer.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <deque>
#include <vector>

// The wave's one flicker meter (suite Renderer.FlickerMetrics): how much a sequence's light changes from frame to frame at
// the same world point, beyond a steady drift. Each pixel of the newest frame of a triple is reprojected by depth into the
// two before it, and only where all three frames saw the same surface (depth and normal agree) the second difference of its
// value, relative to its mean, is measured. Statistics are fractions; reports multiply them by 100. Docs/DeveloperGuide.md,
// "GPU tests".
namespace Strata::Tests
{

	// One frame of a measured sequence, read back from a renderer that rendered it with HDROutput and a forced pre-exposure.
	struct FlickerFrame
	{
		uint32_t Width = 0, Height = 0;
		std::vector<float> Value;      // Rec. 709 luminance of the measured image (the HDR target)
		std::vector<float> ViewDepth;  // Meters along the camera's forward axis; 0 where nothing was drawn
		std::vector<glm::vec3> Normal; // World space, unit (the prepass's)
		glm::mat4 ViewProjection;      // The frame's, as rendered (jittered with TAA), camera-relative
		glm::dvec3 CameraPosition;
		glm::vec3 CameraForward;
	};

	struct FlickerResult
	{
		double Mean = 0, P99 = 0;         // Per pixel |d|
		double Blocks = 0, BlocksP99 = 0; // Per 32x32 block |mean d|
		double MeasuredShare = 0;         // Pixels measured over all pixels
		uint32_t Frames = 0;              // Triples averaged
	};

	// HDR, depth, normals; Renderer::ReadTexture.
	FlickerFrame ReadFlickerFrame(SceneRenderer& renderer, const SceneCamera& camera);

	// A frame as the person sees it: Value is the Rec. 709 luminance of the 8-bit output (GetOutputTexture) decoded from sRGB to
	// linear, after tone mapping and FXAA or TAA; depth and normals are the prepass's, as ReadFlickerFrame reads them.
	FlickerFrame ReadDisplayFlickerFrame(SceneRenderer& renderer, const SceneCamera& camera);

	// The linear Rec. 709 luminance of an 8-bit sRGB-encoded color (ReadDisplayFlickerFrame's Value).
	float GetDisplayLuminance(uint8_t red, uint8_t green, uint8_t blue);

	// a oldest.
	FlickerResult MeasureFlicker(const FlickerFrame& a, const FlickerFrame& b, const FlickerFrame& c, uint32_t blockPixels = 32);

	// A strafe measured as it renders: holds the last three frames only (a 3840x2160 frame is 166 MB here; fourteen would be 2.3 GB)
	// and averages the triples ending at frames firstFrame and later (frames count from 0, the sequence's first Add).
	class FlickerSequence
	{
	public:
		explicit FlickerSequence(uint32_t firstFrame = 3, uint32_t blockPixels = 32);
		void Add(FlickerFrame frame);
		FlickerResult GetResult() const;

	private:
		uint32_t m_FirstFrame = 3;
		uint32_t m_BlockPixels = 32;
		uint32_t m_Added = 0;
		std::deque<FlickerFrame> m_Frames;
		FlickerResult m_Sum;
	};

}
