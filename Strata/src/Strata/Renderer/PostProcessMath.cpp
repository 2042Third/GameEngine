#include "stpch.h"
#include "Strata/Renderer/PostProcessMath.h"

#include "Strata/Renderer/FilmicConstants.h"

#include <algorithm>
#include <cmath>

namespace Strata::PostProcessMath
{

	namespace
	{

		constexpr WhiteLocusEntry c_WhiteLocus[] = {
#include "Strata/Renderer/WhiteBalanceLocus.inl"
		};

		// sRGB's white, D65 (x, y), and its primaries: the matrices between linear sRGB and XYZ are built from them.
		constexpr glm::dvec2 c_D65 = { 0.3127, 0.3290 };
		constexpr glm::dvec2 c_SRGBPrimaries[3] = { { 0.64, 0.33 }, { 0.30, 0.60 }, { 0.15, 0.06 } };

		glm::dvec3 XYToXYZ(const glm::dvec2& xy)
		{
			return glm::dvec3(xy.x / xy.y, 1.0, (1.0 - xy.x - xy.y) / xy.y);
		}

		// Linear sRGB to XYZ: the primaries' XYZ scaled so that (1, 1, 1) is D65 at luminance 1.
		glm::dmat3 GetSRGBToXYZ()
		{
			const glm::dmat3 primaries(XYToXYZ(c_SRGBPrimaries[0]), XYToXYZ(c_SRGBPrimaries[1]), XYToXYZ(c_SRGBPrimaries[2]));
			const glm::dvec3 scale = glm::inverse(primaries) * XYToXYZ(c_D65);
			return primaries * glm::dmat3(scale.x, 0.0, 0.0, 0.0, scale.y, 0.0, 0.0, 0.0, scale.z);
		}

		// CIE 159:2004's CAT02 (rows as published; glm is column-major).
		const glm::dmat3 c_CAT02 = glm::transpose(glm::dmat3(0.7328, 0.4296, -0.1624, -0.7036, 1.6975, 0.0061, 0.0030, 0.0136, 0.9834));

		glm::dvec2 UVToXY(const glm::dvec2& uv)
		{
			const double denominator = 2.0 * uv.x - 8.0 * uv.y + 4.0;
			return glm::dvec2(3.0 * uv.x / denominator, 2.0 * uv.y / denominator);
		}

		float FiniteOr(float value, float fallback)
		{
			return std::isfinite(value) ? value : fallback;
		}

	}

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
			// Values within c_DitherEndSnap codes of black or white are black or white (Include/Dither.slang).
			constexpr float c_DitherEndSnap = 1.0f / 256.0f;
			v = v < c_DitherEndSnap ? 0.0f : (v > levels - c_DitherEndSnap ? levels : v);
			const float u0 = PixelRandom(pixel, 2 * channel);
			const float u1 = PixelRandom(pixel, 2 * channel + 1);
			const float q = (v >= 0.5f && v <= levels - 0.5f) ? std::floor(v + (u0 + u1 - 1.0f) + 0.5f) : std::floor(v + u0);
			result[channel] = q / levels;
		}
		return result;
	}

	FilmicCurve MakeFilmicCurve(float toe, float shoulder)
	{
		toe = std::clamp(std::isfinite(toe) ? toe : 0.5f, 0.0f, 1.0f);
		shoulder = std::clamp(std::isfinite(shoulder) ? shoulder : 0.5f, 0.0f, 1.0f);
		FilmicCurve curve;
		curve.ToeSlope = FilmicConstants::c_Contrast * (0.85f + 0.5f * toe);
		curve.WhiteStops = 6.5f + 3.0f * (shoulder - 0.5f);
		const float middleGray = FilmicConstants::c_MiddleGray;
		curve.ShoulderPower = FilmicConstants::c_Contrast * middleGray * std::log(2.0f) * curve.WhiteStops / (1.0f - middleGray);
		return curve;
	}

	float EvaluateFilmicCurve(float x, const FilmicCurve& curve)
	{
		const float middleGray = FilmicConstants::c_MiddleGray;
		if (!(x > 0.0f))
			return 0.0f;
		const float t = std::log2(x / middleGray);
		if (t < 0.0f)
		{
			const float toeWidth = FilmicConstants::c_ToeWidth;
			return middleGray * std::exp2(curve.ToeSlope * t + (FilmicConstants::c_Contrast - curve.ToeSlope) * toeWidth * (std::exp(t / toeWidth) - 1.0f));
		}
		if (t >= curve.WhiteStops)
			return 1.0f;
		return middleGray + (1.0f - middleGray) * (1.0f - std::pow(1.0f - t / curve.WhiteStops, curve.ShoulderPower));
	}

	std::span<const WhiteLocusEntry> GetWhiteLocusTable()
	{
		return c_WhiteLocus;
	}

	glm::dvec4 GetWhiteLocus(double kelvin)
	{
		const std::span<const WhiteLocusEntry> table = GetWhiteLocusTable();
		const double mireds = std::clamp(1e6 / kelvin, table.front().Mireds, table.back().Mireds);
		size_t index = 0;
		while (index + 2 < table.size() && table[index + 1].Mireds < mireds)
			index++;
		const WhiteLocusEntry& first = table[index];
		const WhiteLocusEntry& second = table[index + 1];
		const double weight = (mireds - first.Mireds) / (second.Mireds - first.Mireds);
		const glm::dvec2 normal = glm::normalize(glm::mix(glm::dvec2(first.NormalU, first.NormalV), glm::dvec2(second.NormalU, second.NormalV), weight));
		return glm::dvec4(glm::mix(first.U, second.U, weight), glm::mix(first.V, second.V, weight), normal);
	}

	glm::dvec3 ChromaticityToLinearSRGB(const glm::dvec2& uv)
	{
		return glm::inverse(GetSRGBToXYZ()) * XYToXYZ(UVToXY(uv));
	}

	glm::mat3 GetWhiteBalanceMatrix(float kelvin, float tint)
	{
		if (kelvin == 6500.0f && tint == 0.0f)
			return glm::mat3(1.0f);
		const glm::dvec4 locus = GetWhiteLocus(std::clamp(static_cast<double>(FiniteOr(kelvin, 6500.0f)), 2000.0, 15000.0));
		const double shift = 0.02 * std::clamp(static_cast<double>(FiniteOr(tint, 0.0f)), -1.0, 1.0);
		const glm::dvec2 light = glm::dvec2(locus.x, locus.y) + shift * glm::dvec2(locus.z, locus.w);
		const glm::dvec3 lightCones = c_CAT02 * XYToXYZ(UVToXY(light));
		const glm::dvec3 referenceCones = c_CAT02 * XYToXYZ(c_D65);
		const glm::dvec3 gains = referenceCones / lightCones;
		const glm::dmat3 toXYZ = GetSRGBToXYZ();
		const glm::dmat3 scale(gains.x, 0.0, 0.0, 0.0, gains.y, 0.0, 0.0, 0.0, gains.z);
		return glm::mat3(glm::inverse(toXYZ) * glm::inverse(c_CAT02) * scale * c_CAT02 * toXYZ);
	}

	ColorGrading MakeColorGrading(float kelvin, float tint, const glm::vec3& slope, const glm::vec3& offset, const glm::vec3& power)
	{
		ColorGrading grading;
		glm::vec3 clampedSlope;
		glm::vec3 clampedOffset;
		glm::vec3 clampedPower;
		for (int channel = 0; channel < 3; channel++)
		{
			clampedSlope[channel] = std::clamp(FiniteOr(slope[channel], 1.0f), 0.0f, 4.0f);
			clampedOffset[channel] = std::clamp(FiniteOr(offset[channel], 0.0f), -0.1f, 0.1f);
			clampedPower[channel] = std::clamp(FiniteOr(power[channel], 1.0f), 0.2f, 5.0f);
		}
		const float temperature = std::clamp(FiniteOr(kelvin, 6500.0f), 2000.0f, 15000.0f);
		const float clampedTint = std::clamp(FiniteOr(tint, 0.0f), -1.0f, 1.0f);
		// Slope scales the white-balanced channels: diag(slope) * M.
		const glm::mat3 whiteBalance = GetWhiteBalanceMatrix(temperature, clampedTint);
		const glm::mat3 slopeMatrix(clampedSlope.x, 0.0f, 0.0f, 0.0f, clampedSlope.y, 0.0f, 0.0f, 0.0f, clampedSlope.z);
		grading.Matrix = slopeMatrix * whiteBalance;
		grading.Offset = clampedOffset * FilmicConstants::c_MiddleGray;
		grading.Power = clampedPower;
		grading.Active = temperature != 6500.0f || clampedTint != 0.0f || clampedSlope != glm::vec3(1.0f) || clampedOffset != glm::vec3(0.0f)
			|| clampedPower != glm::vec3(1.0f);
		return grading;
	}

	glm::vec3 ApplyColorGrading(const glm::vec3& color, const ColorGrading& grading)
	{
		if (!grading.Active)
			return color;
		const glm::vec3 balanced = glm::max(grading.Matrix * color + grading.Offset, glm::vec3(0.0f));
		const float middleGray = FilmicConstants::c_MiddleGray;
		return middleGray * glm::pow(balanced / middleGray, grading.Power);
	}

	MotionBlurProfile GetMotionBlurProfile(float lengthPixels, uint32_t sampleCount)
	{
		// One surface moving uniformly: every pixel's blur, its tile's and its neighborhood's are the same, so every sample
		// counts as a sample of the center's own surface (PostProcess/MotionBlur.comp's weights with equal depths).
		MotionBlurProfile profile;
		const float radius = 0.5f * lengthPixels;
		const int reach = static_cast<int>(std::ceil(radius)) + 1;
		profile.FirstPixel = -reach;
		const size_t pixels = static_cast<size_t>(2 * reach);
		profile.Mean.assign(pixels, 0.0f);
		profile.Spread.assign(pixels, 0.0f);
		auto edge = [](int pixel) { return pixel >= 0 ? 1.0f : 0.0f; };
		if (radius < 0.5f || sampleCount == 0)
		{
			for (size_t index = 0; index < pixels; index++)
				profile.Mean[index] = edge(profile.FirstPixel + static_cast<int>(index));
			return profile;
		}
		const float sampleRadius = std::max(radius, 0.5f);
		auto cover = [](float distance, float r) { return std::clamp(1.0f - distance / r, 0.0f, 1.0f); };
		auto span = [](float distance, float r)
		{
			const float t = std::clamp((distance - 0.95f * r) / (0.1f * r), 0.0f, 1.0f);
			return 1.0f - t * t * (3.0f - 2.0f * t);
		};
		auto weight = [&](float distance) { return 2.0f * cover(distance, sampleRadius) + 2.0f * span(distance, sampleRadius) * span(distance, sampleRadius); };
		// The rows' static offsets are uniform in [0, 1): the profile averages them, finely sampled.
		constexpr int c_Offsets = 512;
		for (size_t index = 0; index < pixels; index++)
		{
			const int pixel = profile.FirstPixel + static_cast<int>(index);
			double sum = 0.0;
			double sumSquares = 0.0;
			for (int step = 0; step < c_Offsets; step++)
			{
				const float jitter = (static_cast<float>(step) + 0.5f) / static_cast<float>(c_Offsets) - 0.5f;
				float total = weight(0.0f);
				float value = total * edge(pixel);
				for (uint32_t sample = 0; sample < sampleCount; sample++)
				{
					const float position = -1.0f + (2.0f * static_cast<float>(sample) + 1.0f + jitter) / static_cast<float>(sampleCount);
					const int samplePixel = static_cast<int>(std::floor(static_cast<float>(pixel) + 0.5f + position * radius));
					const float w = weight(std::abs(position) * radius);
					value += w * edge(samplePixel);
					total += w;
				}
				const double shown = value / total;
				sum += shown;
				sumSquares += shown * shown;
			}
			const double mean = sum / c_Offsets;
			profile.Mean[index] = static_cast<float>(mean);
			profile.Spread[index] = static_cast<float>(std::sqrt(std::max(sumSquares / c_Offsets - mean * mean, 0.0)));
		}
		return profile;
	}

	glm::vec3 ApplyFilmic(const glm::vec3& exposed, const FilmicCurve& curve)
	{
		const glm::vec3 inset = FilmicConstants::c_Inset * glm::max(exposed, glm::vec3(0.0f));
		const float largest = std::max({ inset.r, inset.g, inset.b });
		if (!(largest > 0.0f))
			return glm::vec3(0.0f);
		// Below middle gray the color scales as a whole (its largest channel on the curve), above it each channel takes the
		// curve; the shader's smoothstep between them over the largest channel's stops.
		const glm::vec3 perChannel(EvaluateFilmicCurve(inset.r, curve), EvaluateFilmicCurve(inset.g, curve), EvaluateFilmicCurve(inset.b, curve));
		const glm::vec3 whole = inset * (EvaluateFilmicCurve(largest, curve) / largest);
		const float t = std::clamp((std::log2(largest / FilmicConstants::c_MiddleGray) - FilmicConstants::c_ChannelStart)
			/ (FilmicConstants::c_ChannelEnd - FilmicConstants::c_ChannelStart), 0.0f, 1.0f);
		const float channel = t * t * (3.0f - 2.0f * t);
		return glm::clamp(FilmicConstants::c_Outset * glm::mix(whole, perChannel, channel), glm::vec3(0.0f), glm::vec3(1.0f));
	}

}
