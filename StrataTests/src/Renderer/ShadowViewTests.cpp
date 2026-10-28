#include <doctest/doctest.h>

#include "Strata/Renderer/ShadowView.h"

#include <array>
#include <cmath>

using namespace Strata;

TEST_SUITE("Renderer.ShadowView")
{
	TEST_CASE("Ratios fall into eighths of a stop from 1/16 to 16 pixels per texel")
	{
		CHECK(ShadowView::GetBin(1.0f / 16.0f) == 0);
		CHECK(ShadowView::GetBin(1.0f) == 32);
		CHECK(ShadowView::GetBin(2.0f) == 40);
		CHECK(ShadowView::GetBin(4.0f) == 48);
		CHECK(ShadowView::GetBin(std::exp2(0.124f)) == 32);
		CHECK(ShadowView::GetBin(std::exp2(0.126f)) == 33);
		// Outside the range: the first and last bins.
		CHECK(ShadowView::GetBin(0.0f) == 0);
		CHECK(ShadowView::GetBin(-1.0f) == 0);
		CHECK(ShadowView::GetBin(1000.0f) == 63);
		CHECK(ShadowView::GetBinCenter(32) == doctest::Approx(std::exp2(0.0625f)));
		CHECK(ShadowView::GetBinCenter(0) == doctest::Approx(std::exp2(-4.0f + 0.0625f)));
	}

	TEST_CASE("A cascade's percentiles are the bins where the cumulative count reaches them")
	{
		std::array<uint32_t, RenderData::c_ShadowViewBins> bins = {};
		const ShadowViewRatioStats empty = ShadowView::Summarize(bins);
		CHECK(empty.Pixels == 0);
		CHECK(empty.MedianPixelsPerTexel == 0.0f);
		CHECK(empty.AboveFourPixels == 0);

		// 10 pixels at 1/2, 40 at 1, 40 at 2, 10 at 8 pixels per texel.
		bins[ShadowView::GetBin(0.5f)] = 10;
		bins[ShadowView::GetBin(1.0f)] = 40;
		bins[ShadowView::GetBin(2.0f)] = 40;
		bins[ShadowView::GetBin(8.0f)] = 10;
		const ShadowViewRatioStats stats = ShadowView::Summarize(bins);
		CHECK(stats.Pixels == 100);
		CHECK(stats.P10PixelsPerTexel == doctest::Approx(ShadowView::GetBinCenter(ShadowView::GetBin(0.5f))));
		CHECK(stats.MedianPixelsPerTexel == doctest::Approx(ShadowView::GetBinCenter(ShadowView::GetBin(1.0f))));
		CHECK(stats.P90PixelsPerTexel == doctest::Approx(ShadowView::GetBinCenter(ShadowView::GetBin(2.0f))));
		CHECK(stats.AboveFourPixels == 10);
		// The bin starting at 4 counts as above four, the one below it does not.
		bins = {};
		bins[47] = 3;
		bins[48] = 2;
		CHECK(ShadowView::Summarize(bins).AboveFourPixels == 2);
	}

	TEST_CASE("The covered statistics sum the cascades the frame had")
	{
		ShadowViewStats stats;
		stats.CascadeCount = 2;
		stats.Bins[0][ShadowView::GetBin(1.0f)] = 30;
		stats.Bins[1][ShadowView::GetBin(4.0f)] = 10;
		stats.Bins[2][ShadowView::GetBin(8.0f)] = 99; // Beyond the cascade count: not the frame's
		ShadowView::SummarizeAll(stats);
		CHECK(stats.PerCascade[0].Pixels == 30);
		CHECK(stats.PerCascade[1].Pixels == 10);
		CHECK(stats.PerCascade[2].Pixels == 0);
		CHECK(stats.Covered.Pixels == 40);
		CHECK(stats.Covered.AboveFourPixels == 10);
		CHECK(stats.Covered.MedianPixelsPerTexel == doctest::Approx(ShadowView::GetBinCenter(ShadowView::GetBin(1.0f))));
	}
}
