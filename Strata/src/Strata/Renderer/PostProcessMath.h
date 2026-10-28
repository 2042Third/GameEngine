#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace Strata::PostProcessMath
{

	// The CPU twins of the post-processing shaders' math (shaders/Include/Common.glsl's sRGB encoding, Include/Dither.glsl), in
	// the same arithmetic, so tests can compute what the GPU must show.

	// The sRGB transfer functions (IEC 61966-2-1) as Include/Common.glsl computes them, in float.
	glm::vec3 LinearToSRGB(const glm::vec3& linear);
	float SRGBToLinear(float encoded);

	// Include/Dither.glsl's MixBits: a 32-bit integer finalizer (three xorshifts, two multiplications by fixed odd constants).
	uint32_t MixBits(uint32_t x);
	// A uniform value in [0, 1) for a pixel and a stream: the top 24 bits of MixBits(x ^ MixBits(y ^ MixBits(stream))).
	float PixelRandom(glm::uvec2 pixel, uint32_t stream);
	// The dither's triangular noise in (-1, 1) per channel (streams 0-5, two uniform values per channel).
	glm::vec3 TriangularNoise(glm::uvec2 pixel);
	// Include/Dither.glsl's DitherToCodes: the dithered code values of a display-encoded color for a target of `levels` + 1
	// codes per channel (255, 1023), as normalized values; `levels` 0 returns the color unchanged.
	glm::vec3 DitherToCodes(const glm::vec3& encoded, glm::uvec2 pixel, float levels);

}
