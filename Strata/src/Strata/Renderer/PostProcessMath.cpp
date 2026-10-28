#include "stpch.h"
#include "Strata/Renderer/PostProcessMath.h"

#include <algorithm>
#include <cmath>

namespace Strata::PostProcessMath
{

	glm::vec3 LinearToSRGB(const glm::vec3& linear)
	{
		glm::vec3 encoded;
		for (uint32_t channel = 0; channel < 3; channel++)
		{
			const float value = std::max(linear[channel], 0.0f);
			encoded[channel] = value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
		}
		return encoded;
	}

	float SRGBToLinear(float encoded)
	{
		return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
	}

	uint32_t MixBits(uint32_t x)
	{
		x ^= x >> 16u;
		x *= 0xA3B195E5u;
		x ^= x >> 15u;
		x *= 0x6C8E9CF7u;
		x ^= x >> 16u;
		return x;
	}

	float PixelRandom(glm::uvec2 pixel, uint32_t stream)
	{
		const uint32_t bits = MixBits(pixel.x ^ MixBits(pixel.y ^ MixBits(stream)));
		return static_cast<float>(bits >> 8u) * (1.0f / 16777216.0f);
	}

	glm::vec3 TriangularNoise(glm::uvec2 pixel)
	{
		glm::vec3 noise;
		for (uint32_t channel = 0; channel < 3; channel++)
			noise[channel] = PixelRandom(pixel, 2 * channel) + PixelRandom(pixel, 2 * channel + 1) - 1.0f;
		return noise;
	}

	glm::vec3 DitherToCodes(const glm::vec3& encoded, glm::uvec2 pixel, float levels)
	{
		if (levels <= 0.0f)
			return encoded;
		glm::vec3 result;
		for (uint32_t channel = 0; channel < 3; channel++)
		{
			float v = std::clamp(encoded[channel] * levels, 0.0f, levels);
			// Values within c_DitherEndSnap codes of black or white are black or white (Include/Dither.glsl).
			constexpr float c_DitherEndSnap = 1.0f / 256.0f;
			v = v < c_DitherEndSnap ? 0.0f : (v > levels - c_DitherEndSnap ? levels : v);
			const float u0 = PixelRandom(pixel, 2 * channel);
			const float u1 = PixelRandom(pixel, 2 * channel + 1);
			const float q = (v >= 0.5f && v <= levels - 0.5f) ? std::floor(v + (u0 + u1 - 1.0f) + 0.5f) : std::floor(v + u0);
			result[channel] = q / levels;
		}
		return result;
	}

}
