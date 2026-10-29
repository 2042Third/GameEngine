#include "stpch.h"
#include "Strata/Renderer/ShadowView.h"

#include <algorithm>
#include <cmath>

namespace Strata::ShadowView
{

	uint32_t GetBin(float pixelsPerTexel)
	{
		const float stops = std::log2(std::max(pixelsPerTexel, 1e-30f));
		const float bin = std::floor((stops - static_cast<float>(RenderData::c_ShadowViewMinStops)) * static_cast<float>(RenderData::c_ShadowViewBinsPerStop));
		return static_cast<uint32_t>(std::clamp(bin, 0.0f, static_cast<float>(RenderData::c_ShadowViewBins - 1)));
	}

	float GetBinCenter(uint32_t bin)
	{
		const float stops = (static_cast<float>(bin) + 0.5f) / static_cast<float>(RenderData::c_ShadowViewBinsPerStop)
			+ static_cast<float>(RenderData::c_ShadowViewMinStops);
		return std::exp2(stops);
	}

	ShadowViewRatioStats Summarize(std::span<const uint32_t, RenderData::c_ShadowViewBins> bins)
	{
		ShadowViewRatioStats stats;
		uint64_t total = 0;
		for (uint32_t count : bins)
			total += count;
		stats.Pixels = static_cast<uint32_t>(total);
		if (total == 0)
			return stats;

		// The first bin where the cumulative count reaches the fraction of the pixels.
		const auto percentile = [&](double fraction)
		{
			const double wanted = fraction * static_cast<double>(total);
			uint64_t cumulative = 0;
			for (uint32_t bin = 0; bin < RenderData::c_ShadowViewBins; bin++)
			{
				cumulative += bins[bin];
				if (static_cast<double>(cumulative) >= wanted && cumulative > 0)
					return GetBinCenter(bin);
			}
			return GetBinCenter(RenderData::c_ShadowViewBins - 1);
		};
		stats.P10PixelsPerTexel = percentile(0.1);
		stats.MedianPixelsPerTexel = percentile(0.5);
		stats.P90PixelsPerTexel = percentile(0.9);
		// Bins from the one starting at 4 pixels per texel: 2 stops above 1, from 1/16.
		const uint32_t firstAboveFour = static_cast<uint32_t>((2 - RenderData::c_ShadowViewMinStops) * static_cast<int32_t>(RenderData::c_ShadowViewBinsPerStop));
		uint64_t aboveFour = 0;
		for (uint32_t bin = firstAboveFour; bin < RenderData::c_ShadowViewBins; bin++)
			aboveFour += bins[bin];
		stats.AboveFourPixels = static_cast<uint32_t>(aboveFour);
		return stats;
	}

	void SummarizeAll(ShadowViewStats& stats)
	{
		std::array<uint32_t, RenderData::c_ShadowViewBins> covered = {};
		for (uint32_t cascade = 0; cascade < RenderData::c_MaxCascades; cascade++)
		{
			if (cascade >= stats.CascadeCount)
			{
				stats.PerCascade[cascade] = {};
				continue;
			}
			stats.PerCascade[cascade] = Summarize(stats.Bins[cascade]);
			for (uint32_t bin = 0; bin < RenderData::c_ShadowViewBins; bin++)
				covered[bin] += stats.Bins[cascade][bin];
		}
		stats.Covered = Summarize(covered);
	}

}
