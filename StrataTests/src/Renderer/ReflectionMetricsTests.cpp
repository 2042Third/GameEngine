#include "Renderer/ReflectionMetrics.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

// The look-dev harness's noise and pattern measures (Renderer/ReflectionMetrics.h, Tools/LookDev.py "Metrics") on synthetic
// images, as LookDev.py's selftest checks its own.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A gray 64x64 RGBA8 image of 128 plus `amplitude` codes times a k x k ordered pattern of mean zero.
	ReadbackImage PatternImage(uint32_t period, int amplitude)
	{
		constexpr uint32_t c_Order4[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
		constexpr uint32_t c_Order3[9] = { 0, 7, 3, 6, 5, 2, 4, 1, 8 };
		ReadbackImage image;
		image.Width = 64;
		image.Height = 64;
		image.Format = nvrhi::Format::RGBA8_UNORM;
		image.BytesPerPixel = 4;
		image.Pixels.resize(64 * 64 * 4);
		const uint32_t cells = period * period;
		for (uint32_t y = 0; y < 64; y++)
		{
			for (uint32_t x = 0; x < 64; x++)
			{
				int value = 128;
				if (period > 0)
				{
					const uint32_t order = period == 4 ? c_Order4[(y % 4) * 4 + x % 4] : c_Order3[(y % 3) * 3 + x % 3];
					// Ranks 0 .. cells - 1 spread over -amplitude .. amplitude.
					value += static_cast<int>(std::lround(amplitude * (2.0 * order / (cells - 1) - 1.0)));
				}
				uint8_t* pixel = image.Pixels.data() + (static_cast<size_t>(y) * 64 + x) * 4;
				pixel[0] = pixel[1] = pixel[2] = static_cast<uint8_t>(value);
				pixel[3] = 255;
			}
		}
		return image;
	}

}

TEST_SUITE("Renderer.ReflectionMetrics")
{
	TEST_CASE("A 4x4 screen-locked pattern is all Pattern4, a 3x3 one all Pattern3, a flat image nothing")
	{
		const glm::uvec4 whole(0, 0, 64, 64);
		const NoiseMeasure four = MeasureNoise(PatternImage(4, 8), whole);
		CAPTURE(four.Noise);
		CAPTURE(four.Pattern3);
		CAPTURE(four.Pattern4);
		CHECK(four.Pixels == 56u * 56u);
		CHECK(four.Noise > 1.0f);
		CHECK(std::abs(four.Pattern4 - four.Noise) <= 0.05f * four.Noise);
		CHECK(four.Pattern3 < 0.2f * four.Noise);

		const NoiseMeasure three = MeasureNoise(PatternImage(3, 8), whole);
		CAPTURE(three.Noise);
		CAPTURE(three.Pattern3);
		CAPTURE(three.Pattern4);
		CHECK(three.Noise > 1.0f);
		CHECK(std::abs(three.Pattern3 - three.Noise) <= 0.05f * three.Noise);
		CHECK(three.Pattern4 < 0.2f * three.Noise);

		const NoiseMeasure flat = MeasureNoise(PatternImage(0, 0), whole);
		CHECK(flat.Noise < 1e-6f); // The summed-area table's rounding
		CHECK(flat.Pattern3 < 1e-6f);
		CHECK(flat.Pattern4 < 1e-6f);
	}

	TEST_CASE("The noise is measured inside its rectangle only")
	{
		// A pattern in the left half, flat gray in the right: a rectangle within the right half measures nothing.
		ReadbackImage image = PatternImage(4, 8);
		for (uint32_t y = 0; y < 64; y++)
		{
			for (uint32_t x = 32; x < 64; x++)
			{
				uint8_t* pixel = image.Pixels.data() + (static_cast<size_t>(y) * 64 + x) * 4;
				pixel[0] = pixel[1] = pixel[2] = 128;
			}
		}
		const NoiseMeasure right = MeasureNoise(image, glm::uvec4(32, 0, 64, 64));
		CHECK(right.Pixels == 24u * 56u);
		CHECK(right.Noise < 1e-6f);
		CHECK(MeasureNoise(image, glm::uvec4(0, 0, 32, 64)).Noise > 1.0f);
		// Too small for a 9x9 window: nothing measured.
		CHECK(MeasureNoise(image, glm::uvec4(0, 0, 8, 64)).Pixels == 0u);
	}
}
