#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

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

	// White balance. The white locus (Renderer/WhiteBalanceLocus.inl, Tools/GenerateWhiteBalance.py): an entry every 5 mireds
	// from 60 to 510, its CIE 1960 (u, v) and unit normal toward green; the daylight locus at and above 5000 K (shifted so that
	// 6500 K is sRGB's D65 white), Planck's at and below 4000 K, blended between.
	struct WhiteLocusEntry
	{
		double Mireds;
		double U;
		double V;
		double NormalU;
		double NormalV;
	};
	std::span<const WhiteLocusEntry> GetWhiteLocusTable();
	// The locus at a temperature (clamped to the table), interpolated linearly in mireds: (u, v, normal u, normal v).
	glm::dvec4 GetWhiteLocus(double kelvin);
	// sRGB's linear color of a CIE 1960 (u, v) chromaticity at luminance 1.
	glm::dvec3 ChromaticityToLinearSRGB(const glm::dvec2& uv);
	// The white balance of scene-linear sRGB light: the light of the white locus at `kelvin` (2,000 to 15,000), moved along
	// the locus's normal by 0.02 * `tint` in (u, v) (positive assumes a greener light, so the image turns magenta), shows as
	// white. A von Kries scaling in the cone space of CAT02 (CIE 159:2004), relative to sRGB's D65 white:
	// M = XYZToSRGB * CAT02^-1 * diag(reference / light cones) * CAT02 * SRGBToXYZ, computed in double. 6500 K and tint 0
	// return the identity exactly; lower temperatures cool the image, higher warm it.
	glm::mat3 GetWhiteBalanceMatrix(float kelvin, float tint);

	// The grading before the tone curve (Include/... Tonemap.frag): c1 = Matrix * c (white balance times the slope), c2 =
	// max(c1 + Offset, 0) (the offset in units of light, PostProcessComponent::ColorOffset times middle gray), c3 = 0.18 *
	// (c2 / 0.18)^Power. Active is false when every part is neutral: the tone mapping then skips it (bit-identical to none).
	struct ColorGrading
	{
		glm::mat3 Matrix = glm::mat3(1.0f);
		glm::vec3 Offset = glm::vec3(0.0f);
		glm::vec3 Power = glm::vec3(1.0f);
		bool Active = false;
	};
	// From the Post Process's values (each clamped to its property's range; non-finite values are neutral).
	ColorGrading MakeColorGrading(float kelvin, float tint, const glm::vec3& slope, const glm::vec3& offset, const glm::vec3& power);
	glm::vec3 ApplyColorGrading(const glm::vec3& color, const ColorGrading& grading);

	// The motion blur's gather (PostProcess/MotionBlur.comp) on an ideal edge (black left of pixel 0, white from it) of one
	// surface moving uniformly with a blur of `lengthPixels` (after the shutter, clamp and threshold): per pixel from FirstPixel
	// on, the profile's mean over the rows' static offsets and its spread (the RMS of single rows about the mean: the streak
	// noise), in linear light.
	struct MotionBlurProfile
	{
		int FirstPixel = 0;
		std::vector<float> Mean;
		std::vector<float> Spread;
	};
	MotionBlurProfile GetMotionBlurProfile(float lengthPixels, uint32_t sampleCount);

}
