#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace Strata::PostProcessMath
{

	// The CPU twins of the post-processing shaders' math (shaders/Include/Common.glsl's sRGB encoding, Include/Dither.glsl,
	// Include/Filmic.glsl), in the same arithmetic, so tests can compute what the GPU must show.

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

	// The Filmic curve's look (TonemapOperator::Filmic; its form and fit: Tools/FitFilmic.py): the toe's log-log slope Ct, the
	// stops above middle gray where white is reached (Tw) and the shoulder's power q, which matches the slope at middle gray.
	struct FilmicCurve
	{
		float ToeSlope = 0.0f;
		float WhiteStops = 0.0f;
		float ShoulderPower = 0.0f;
	};
	// From PostProcessComponent::Toe and Shoulder (each clamped to [0, 1]): Ct = C0 (0.85 + 0.5 Toe), Tw = 6.5 + 3 (Shoulder -
	// 0.5) stops, q = C0 0.18 ln2 Tw / 0.82 (at least 1 on the whole range: the shoulder never clips).
	FilmicCurve MakeFilmicCurve(float toe, float shoulder);
	// The curve of one channel (Include/Filmic.glsl's FilmicCurve): 0 for x <= 0, middle gray to middle gray, 1 from Tw on.
	float EvaluateFilmicCurve(float x, const FilmicCurve& curve);
	// The whole operator (Include/Filmic.glsl's TonemapFilmic): the inset, the curve per channel, the outset, clamped to [0, 1].
	glm::vec3 ApplyFilmic(const glm::vec3& exposed, const FilmicCurve& curve);

}
