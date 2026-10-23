#include "Renderer/TAATestUtils.h"

#include <glm/gtc/constants.hpp>

#include <array>
#include <cmath>
#include <vector>

// The CPU prototype of the TAA's sharpening (SharpenLuminance), which chose its exponent: halos at edges as the TAA resolves
// them against the gain in texture detail.

using namespace Strata;
using namespace Strata::Tests;
using namespace Strata::Tests::TAA;

namespace
{

	constexpr uint32_t c_EdgeSize = 24;

	// A black-to-white edge (exposed luminance 0 to 1) through the image's middle at `angleDegrees` from vertical, moved
	// `offset` pixels along x, as the TAA resolves it at eight frames: each pixel the mean of its eight jittered samples.
	std::vector<double> ResolveEdge(float angleDegrees, float offset)
	{
		const float angle = glm::radians(angleDegrees);
		const glm::vec2 normal(std::cos(angle), std::sin(angle));
		std::vector<double> image(static_cast<size_t>(c_EdgeSize) * c_EdgeSize, 0.0);
		for (uint32_t y = 0; y < c_EdgeSize; y++)
		{
			for (uint32_t x = 0; x < c_EdgeSize; x++)
			{
				for (uint32_t phase = 0; phase < SceneRenderer::c_TemporalJitterPhases; phase++)
				{
					const glm::vec2 sample = glm::vec2(static_cast<float>(x) + 0.5f - offset, static_cast<float>(y) + 0.5f) + SceneRenderer::GetTemporalJitter(phase)
						- glm::vec2(static_cast<float>(c_EdgeSize) * 0.5f);
					if (glm::dot(sample, normal) < 0.0f)
						image[static_cast<size_t>(y) * c_EdgeSize + x] += 1.0 / SceneRenderer::c_TemporalJitterPhases;
				}
			}
		}
		return image;
	}

	// The largest overshoot of a sharpened edge beyond its pixels' unsharpened five-tap ranges, in compressed luminance, as a
	// fraction of the edge's range there (0.5), over sub-pixel positions of the edge.
	double EdgeOvershoot(float angleDegrees, double amount)
	{
		const auto compress = [](double luminance) { return luminance / (1.0 + luminance); };
		double worst = 0.0;
		for (uint32_t step = 0; step < 8; step++)
		{
			const std::vector<double> image = ResolveEdge(angleDegrees, static_cast<float>(step) / 8.0f);
			const auto at = [&](uint32_t x, uint32_t y) { return image[static_cast<size_t>(y) * c_EdgeSize + x]; };
			for (uint32_t y = 1; y + 1 < c_EdgeSize; y++)
			{
				for (uint32_t x = 1; x + 1 < c_EdgeSize; x++)
				{
					const std::array<double, 4> neighbors = { at(x - 1, y), at(x + 1, y), at(x, y - 1), at(x, y + 1) };
					double low = compress(at(x, y));
					double high = low;
					for (const double neighbor : neighbors)
					{
						low = std::min(low, compress(neighbor));
						high = std::max(high, compress(neighbor));
					}
					const double sharpened = compress(SharpenLuminance(at(x, y), neighbors, amount));
					worst = std::max({ worst, (sharpened - high) / 0.5, (low - sharpened) / 0.5 });
				}
			}
		}
		return worst;
	}

}

TEST_SUITE("Renderer.TAASharpening")
{
	TEST_CASE("At Sharp's amount a resolved black-to-white edge overshoots its five taps by at most 2% of its range, and at 1 by 5%")
	{
		// Every orientation, at eight sub-pixel positions each: diagonal edges, whose four neighbors see the least of the
		// contrast, overshoot the most (an exponent of 6 would let them reach 2.8% and 5.6%).
		const double sharp = c_TAASharpSettings.Sharpening;
		for (int degrees = 0; degrees <= 90; degrees += 5)
		{
			CAPTURE(degrees);
			const float angle = static_cast<float>(degrees);
			CHECK(EdgeOvershoot(angle, sharp) <= 0.02);
			CHECK(EdgeOvershoot(angle, 1.0) <= 0.05);
		}
	}

	TEST_CASE("At Sharp's amount texture detail of 3- to 6-pixel periods gains at least 8% of amplitude")
	{
		// The detail texture of the GPU cases (luminance 0.5 +- 0.2), as the TAA resolves it still: the mean of its eight jittered
		// samples, linearly filtered.
		double unsharpened = 0.0;
		double sharpened = 0.0;
		for (const float period : { 3.0f, 4.0f, 5.0f, 6.0f })
		{
			const auto texture = [period](float x) { return 0.5 + 0.2 * std::sin(glm::two_pi<double>() * x / period + 0.3); };
			std::vector<double> row(64, 0.0);
			for (uint32_t x = 0; x < row.size(); x++)
			{
				for (uint32_t phase = 0; phase < SceneRenderer::c_TemporalJitterPhases; phase++)
					row[x] += texture(static_cast<float>(x) + SceneRenderer::GetTemporalJitter(phase).x) / SceneRenderer::c_TemporalJitterPhases;
			}
			std::vector<double> result(row.size(), 0.0);
			for (uint32_t x = 1; x + 1 < row.size(); x++)
				result[x] = SharpenLuminance(row[x], { row[x - 1], row[x + 1], row[x], row[x] }, c_TAASharpSettings.Sharpening);
			const auto variance = [](const std::vector<double>& values)
			{
				double sum = 0.0;
				double squares = 0.0;
				for (size_t x = 4; x + 4 < values.size(); x++)
				{
					sum += values[x];
					squares += values[x] * values[x];
				}
				const double count = static_cast<double>(values.size() - 8);
				return squares / count - (sum / count) * (sum / count);
			};
			unsharpened += variance(row);
			sharpened += variance(result);
		}
		const double gain = std::sqrt(sharpened / unsharpened) - 1.0;
		INFO("gain " << gain);
		CHECK(gain >= 0.08);
	}

	TEST_CASE("The bounds keep bright low-contrast detail from running away, and black stays black")
	{
		// A bright pixel beside a darker one at full amount: unbounded, its compressed luminance would come close to 1 (and past
		// it, to negative luminance); bounded, it stays below the midpoint of the brightest tap's and 1.
		const std::array<double, 4> neighbors = { 19.0, 99.0, 99.0, 99.0 };
		const double bounded = SharpenLuminance(99.0, neighbors, 1.0);
		const double unbounded = SharpenLuminance(99.0, neighbors, 1.0, false);
		CHECK(bounded <= 1.0 + 2.0 * 99.0 + 1e-6);
		CHECK(unbounded > 1.0 + 2.0 * 99.0);
		CHECK(SharpenLuminance(0.0, { 1.0, 1.0, 1.0, 1.0 }, 1.0) == 0.0);
		// A dark pixel among bright ones never goes below 0.
		CHECK(SharpenLuminance(0.01, { 0.5, 0.5, 0.5, 0.5 }, 1.0) >= 0.0);
	}
}
