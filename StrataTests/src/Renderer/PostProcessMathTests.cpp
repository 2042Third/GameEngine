#include <doctest/doctest.h>

#include "Strata/Renderer/PostProcessMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using namespace Strata;

namespace
{

	constexpr uint32_t c_NoiseSize = 512;

	// Pearson's correlation of two equally long series.
	double Correlation(const std::vector<double>& a, const std::vector<double>& b)
	{
		REQUIRE(a.size() == b.size());
		double meanA = 0.0;
		double meanB = 0.0;
		for (size_t index = 0; index < a.size(); index++)
		{
			meanA += a[index];
			meanB += b[index];
		}
		meanA /= static_cast<double>(a.size());
		meanB /= static_cast<double>(b.size());
		double covariance = 0.0;
		double varianceA = 0.0;
		double varianceB = 0.0;
		for (size_t index = 0; index < a.size(); index++)
		{
			covariance += (a[index] - meanA) * (b[index] - meanB);
			varianceA += (a[index] - meanA) * (a[index] - meanA);
			varianceB += (b[index] - meanB) * (b[index] - meanB);
		}
		return covariance / std::sqrt(varianceA * varianceB);
	}

	// The triangular density's integral from -1 to t.
	double TriangularCDF(double t)
	{
		return t < 0.0 ? 0.5 * (t + 1.0) * (t + 1.0) : 1.0 - 0.5 * (1.0 - t) * (1.0 - t);
	}

}

TEST_SUITE("Renderer.PostProcessMath")
{
	TEST_CASE("The dither noise is triangular and uncorrelated")
	{
		// noise[channel][y * size + x]
		std::array<std::vector<double>, 3> noise;
		for (std::vector<double>& channel : noise)
			channel.resize(static_cast<size_t>(c_NoiseSize) * c_NoiseSize);
		for (uint32_t y = 0; y < c_NoiseSize; y++)
		{
			for (uint32_t x = 0; x < c_NoiseSize; x++)
			{
				const glm::vec3 value = PostProcessMath::TriangularNoise(glm::uvec2(x, y));
				for (uint32_t channel = 0; channel < 3; channel++)
				{
					REQUIRE(value[channel] > -1.0f);
					REQUIRE(value[channel] < 1.0f);
					noise[channel][static_cast<size_t>(y) * c_NoiseSize + x] = value[channel];
				}
			}
		}

		// Each channel's histogram follows the triangular density (peak 1) in bins 0.1 wide.
		constexpr uint32_t c_Bins = 20;
		for (uint32_t channel = 0; channel < 3; channel++)
		{
			std::array<uint32_t, c_Bins> bins = {};
			for (double value : noise[channel])
				bins[std::min(c_Bins - 1, static_cast<uint32_t>((value + 1.0) * 10.0))]++;
			for (uint32_t bin = 0; bin < c_Bins; bin++)
			{
				const double low = -1.0 + 0.1 * bin;
				const double expected = (TriangularCDF(low + 0.1) - TriangularCDF(low)) / 0.1;
				const double observed = static_cast<double>(bins[bin]) / static_cast<double>(noise[channel].size()) / 0.1;
				CAPTURE(channel);
				CAPTURE(bin);
				CHECK(std::abs(observed - expected) < 0.03);
			}
		}

		// The channels are independent of each other, and each pixel of its neighbors.
		CHECK(std::abs(Correlation(noise[0], noise[1])) < 0.03);
		CHECK(std::abs(Correlation(noise[1], noise[2])) < 0.03);
		CHECK(std::abs(Correlation(noise[0], noise[2])) < 0.03);
		for (uint32_t channel = 0; channel < 3; channel++)
		{
			std::vector<double> left, right, top, bottom;
			for (uint32_t y = 0; y < c_NoiseSize; y++)
			{
				for (uint32_t x = 0; x < c_NoiseSize; x++)
				{
					const double value = noise[channel][static_cast<size_t>(y) * c_NoiseSize + x];
					if (x + 1 < c_NoiseSize)
					{
						left.push_back(value);
						right.push_back(noise[channel][static_cast<size_t>(y) * c_NoiseSize + x + 1]);
					}
					if (y + 1 < c_NoiseSize)
					{
						top.push_back(value);
						bottom.push_back(noise[channel][static_cast<size_t>(y + 1) * c_NoiseSize + x]);
					}
				}
			}
			CAPTURE(channel);
			CHECK(std::abs(Correlation(left, right)) < 0.03);
			CHECK(std::abs(Correlation(top, bottom)) < 0.03);
		}
	}

	TEST_CASE("Dithering keeps black, white and the mean")
	{
		for (float levels : { 255.0f, 1023.0f })
		{
			for (uint32_t y = 0; y < 64; y++)
			{
				for (uint32_t x = 0; x < 64; x++)
				{
					CHECK(PostProcessMath::DitherToCodes(glm::vec3(0.0f), glm::uvec2(x, y), levels) == glm::vec3(0.0f));
					CHECK(PostProcessMath::DitherToCodes(glm::vec3(1.0f), glm::uvec2(x, y), levels) == glm::vec3(1.0f));
				}
			}
		}
		// Values within a 256th of a code of black or white are black or white (white encodes a few ulps below 1 in float).
		CHECK(PostProcessMath::DitherToCodes(glm::vec3(0.99999994f), glm::uvec2(5, 6), 255.0f) == glm::vec3(1.0f));
		CHECK(PostProcessMath::DitherToCodes(glm::vec3(1e-6f), glm::uvec2(5, 6), 255.0f) == glm::vec3(0.0f));
		// Levels 0: no quantization.
		CHECK(PostProcessMath::DitherToCodes(glm::vec3(0.3f, 0.5f, 0.7f), glm::uvec2(3, 4), 0.0f) == glm::vec3(0.3f, 0.5f, 0.7f));

		struct Case
		{
			float Levels;
			float Base;
		};
		const Case cases[] = { { 255.0f, 0.0f }, { 255.0f, 1.0f }, { 255.0f, 100.0f }, { 255.0f, 254.0f }, { 1023.0f, 0.0f }, { 1023.0f, 1022.0f } };
		for (const Case& testCase : cases)
		{
			for (float fraction : { 0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f })
			{
				const float codes = testCase.Base + fraction;
				const glm::vec3 encoded(codes / testCase.Levels);
				// The value the dither sees: the encoded value in codes, as the shader computes it.
				const float seen = encoded.x * testCase.Levels;
				glm::dvec3 sum(0.0);
				bool integral = true;
				for (uint32_t y = 0; y < c_NoiseSize; y++)
				{
					for (uint32_t x = 0; x < c_NoiseSize; x++)
					{
						const glm::vec3 code = PostProcessMath::DitherToCodes(encoded, glm::uvec2(x, y), testCase.Levels) * testCase.Levels;
						integral = integral && glm::all(glm::lessThan(glm::abs(code - glm::round(code)), glm::vec3(1e-3f)));
						sum += glm::dvec3(glm::round(code));
					}
				}
				const glm::dvec3 mean = sum / static_cast<double>(c_NoiseSize * c_NoiseSize);
				CAPTURE(testCase.Levels);
				CAPTURE(codes);
				CHECK(integral);
				for (uint32_t channel = 0; channel < 3; channel++)
					CHECK(std::abs(mean[channel] - seen) < 0.01);
			}
		}
	}
}
