#pragma once

#include "Strata/Renderer/SceneRenderData.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace Strata
{

	// A sun shadow cascade as a frame laid it out (SceneRendererStats::Cascades): its slice of view distances, from its near
	// end (including the blend band in which the previous cascade hands over) to its split, and the world size of its texels.
	struct ShadowCascadeStats
	{
		float Near = 0.0f;
		float Far = 0.0f;
		float WorldPerTexel = 0.0f;

		bool operator==(const ShadowCascadeStats&) const = default;
	};

	// Screen pixels per shadow texel over a set of receiver pixels (a cascade's, or every covered one): how many, the median,
	// 10th and 90th percentiles (each reported at the geometric center of the eighth of a stop it falls in), and how many of
	// them see a texel span 4 or more pixels.
	struct ShadowViewRatioStats
	{
		uint32_t Pixels = 0;
		float MedianPixelsPerTexel = 0.0f;
		float P10PixelsPerTexel = 0.0f;
		float P90PixelsPerTexel = 0.0f;
		uint32_t AboveFourPixels = 0;
	};

	// What the shadows debug view (SceneRenderOptions::ShowShadowView) counted in a frame (SceneRenderer::GetShadowViewStats):
	// the image's pixels as sky (nothing drawn), surfaces (anything drawn) and, of those, surfaces no cascade covers; per
	// cascade its layout, its receiver pixels' counts per bin and their ratio statistics; and the same over every covered pixel.
	struct ShadowViewStats
	{
		uint64_t Render = 0; // The renderer's frame (SceneRenderer::GetRenderCount after it) described; 0: none yet
		glm::uvec2 ViewportSize = glm::uvec2(0);
		uint32_t ShadowMapSize = 0; // The cascades' texels each way in that frame
		uint32_t SurfacePixels = 0;
		uint32_t SkyPixels = 0;
		uint32_t UncoveredPixels = 0;
		uint32_t CascadeCount = 0; // 0: no shadowed sun
		std::array<ShadowCascadeStats, RenderData::c_MaxCascades> Cascades = {};
		std::array<std::array<uint32_t, RenderData::c_ShadowViewBins>, RenderData::c_MaxCascades> Bins = {};
		std::array<ShadowViewRatioStats, RenderData::c_MaxCascades> PerCascade = {};
		ShadowViewRatioStats Covered;
	};

	namespace ShadowView
	{

		// The bin of a ratio of screen pixels per shadow texel, as Include/ShadowView.slang's GetShadowViewBin computes it.
		uint32_t GetBin(float pixelsPerTexel);
		// The geometric center of a bin: 2^((bin + 0.5) / 8 - 4).
		float GetBinCenter(uint32_t bin);
		// The statistics of a cascade's (or a sum of cascades') bins.
		ShadowViewRatioStats Summarize(std::span<const uint32_t, RenderData::c_ShadowViewBins> bins);
		// Fills `stats`' per-cascade and covered statistics from its bins and cascade count.
		void SummarizeAll(ShadowViewStats& stats);

	}

}
