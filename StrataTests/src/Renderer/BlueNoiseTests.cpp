#include <doctest/doctest.h>

#include "Strata/Renderer/BlueNoise.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

using namespace Strata;

TEST_SUITE("Renderer.BlueNoise")
{
	TEST_CASE("The blue-noise tile holds every byte value equally often")
	{
		static_assert(RenderData::c_BlueNoiseSize == 64);
		std::array<uint32_t, 256> counts = {};
		for (const uint8_t value : RenderData::c_BlueNoise)
			counts[value]++;
		for (uint32_t value = 0; value < 256; value++)
		{
			CAPTURE(value);
			CHECK(counts[value] == 16);
		}
	}

	TEST_CASE("The blue-noise tile has no low frequencies and its neighbors differ")
	{
		constexpr uint32_t size = RenderData::c_BlueNoiseSize;
		const auto at = [](uint32_t x, uint32_t y) { return static_cast<int>(RenderData::c_BlueNoise[(y % size) * size + (x % size)]); };

		// Every 8x8 block's mean lies near the middle (white noise: a block's mean has a standard deviation of about 9, its
		// worst of 64 about 20; this tile's are at most about 4.6, 1.9 on average).
		double worst = 0.0;
		double sum = 0.0;
		for (uint32_t blockY = 0; blockY < size; blockY += 8)
		{
			for (uint32_t blockX = 0; blockX < size; blockX += 8)
			{
				double mean = 0.0;
				for (uint32_t y = 0; y < 8; y++)
				{
					for (uint32_t x = 0; x < 8; x++)
						mean += at(blockX + x, blockY + y);
				}
				const double deviation = std::abs(mean / 64.0 - 127.5);
				worst = std::max(worst, deviation);
				sum += deviation;
			}
		}
		CAPTURE(worst);
		CHECK(worst <= 6.0);
		CHECK(sum / 64.0 <= 2.5);

		// Toroidal neighbors differ by more than white noise's 85 on average.
		double difference = 0.0;
		for (uint32_t y = 0; y < size; y++)
		{
			for (uint32_t x = 0; x < size; x++)
				difference += std::abs(at(x, y) - at(x + 1, y)) + std::abs(at(x, y) - at(x, y + 1));
		}
		CHECK(difference / (2.0 * size * size) > 90.0);
	}
}
