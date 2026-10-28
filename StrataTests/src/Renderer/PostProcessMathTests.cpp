#include <doctest/doctest.h>

#include "Strata/Renderer/FilmicConstants.h"
#include "Strata/Renderer/PostProcessMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
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

	// ICtCp (ITU-R BT.2100) of a display-linear BT.709 color whose white is 100 nits: BT.709 to BT.2020 (BT.2087), the LMS
	// matrix, PQ. Tools/FitFilmic.py measures the Filmic curve's hues the same way.
	glm::dvec3 ToICtCp(const glm::dvec3& color)
	{
		const glm::dmat3 toBT2020 = glm::transpose(glm::dmat3(0.627403896, 0.329283039, 0.043313065, 0.069097289, 0.919540395, 0.011362316, 0.016391439,
			0.088013308, 0.895595253));
		const glm::dmat3 toLMS = glm::transpose(glm::dmat3(1688.0, 2146.0, 262.0, 683.0, 2951.0, 462.0, 99.0, 309.0, 3688.0)) / 4096.0;
		const glm::dvec3 lms = toLMS * (toBT2020 * color);
		auto pq = [](double value)
		{
			const double m1 = 2610.0 / 16384.0;
			const double m2 = 2523.0 / 4096.0 * 128.0;
			const double c1 = 3424.0 / 4096.0;
			const double c2 = 2413.0 / 4096.0 * 32.0;
			const double c3 = 2392.0 / 4096.0 * 32.0;
			const double p = std::pow(std::max(value, 0.0) * 100.0 / 10000.0, m1);
			return std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
		};
		const glm::dvec3 encoded(pq(lms.x), pq(lms.y), pq(lms.z));
		return glm::dvec3((2048.0 * encoded.x + 2048.0 * encoded.y) / 4096.0, (6610.0 * encoded.x - 13613.0 * encoded.y + 7003.0 * encoded.z) / 4096.0,
			(17933.0 * encoded.x - 17390.0 * encoded.y - 543.0 * encoded.z) / 4096.0);
	}

	double Luminance(const glm::dvec3& color)
	{
		return 0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b;
	}

	double HueDegrees(const glm::dvec3& ictcp)
	{
		return glm::degrees(std::atan2(ictcp.z, ictcp.y));
	}

	double Chroma(const glm::dvec3& ictcp)
	{
		return std::hypot(ictcp.y, ictcp.z);
	}

	double HueDifference(double a, double b)
	{
		const double difference = std::fmod(a - b + 540.0, 360.0) - 180.0;
		return std::abs(difference);
	}

	struct FilmicTestColor
	{
		const char* Name;
		glm::dvec3 Color;
		double HueLimit; // Degrees (Tools/FitFilmic.py's T5)
	};

	constexpr FilmicTestColor c_FilmicTestColors[] = {
		{ "red", { 1.0, 0.05, 0.05 }, 12.5 },
		{ "green", { 0.05, 1.0, 0.05 }, 12.5 },
		{ "blue", { 0.05, 0.05, 1.0 }, 12.5 },
		{ "yellow", { 1.0, 1.0, 0.05 }, 12.5 },
		{ "cyan", { 0.05, 1.0, 1.0 }, 12.5 },
		{ "magenta", { 1.0, 0.05, 1.0 }, 12.5 },
		{ "orange", { 1.0, 0.4, 0.05 }, 12.5 },
		{ "skin", { 0.8, 0.5, 0.35 }, 7.5 },
		{ "foliage", { 0.2, 0.4, 0.08 }, 7.5 },
		{ "sky", { 0.25, 0.45, 0.9 }, 7.5 },
	};

	glm::dvec3 ScaledToMiddleGray(const glm::dvec3& color)
	{
		return color * (0.18 / Luminance(color));
	}

	glm::dvec3 Filmic(const glm::dvec3& scene, const PostProcessMath::FilmicCurve& look)
	{
		return glm::dvec3(PostProcessMath::ApplyFilmic(glm::vec3(scene), look));
	}

	// T5 and T7 of Tools/FitFilmic.py for one test color: the largest hue deviation (degrees) over the ramp from -4 to +7 stops
	// where the displayed chroma is at least 15% of the ramp's largest, and the saturation ratio at 0 stops.
	struct RampMeasure
	{
		double WorstHue = 0.0;
		double Saturation = 0.0;
	};

	RampMeasure MeasureRamp(const glm::dvec3& color, const PostProcessMath::FilmicCurve& look)
	{
		struct Step
		{
			double Stops;
			double Hue;
			double Chroma;
			double ReferenceHue;
			double ReferenceChroma;
		};
		std::vector<Step> steps;
		const glm::dvec3 base = ScaledToMiddleGray(color);
		for (int index = 0; index < 45; index++)
		{
			const double stops = -4.0 + 0.25 * index;
			const glm::dvec3 scene = base * std::exp2(stops);
			const glm::dvec3 shown = Filmic(scene, look);
			const glm::dvec3 shownICtCp = ToICtCp(shown);
			// The scene color scaled linearly to the displayed luminance.
			const glm::dvec3 reference = ToICtCp(scene * (Luminance(shown) / Luminance(scene)));
			steps.push_back({ stops, HueDegrees(shownICtCp), Chroma(shownICtCp), HueDegrees(reference), Chroma(reference) });
		}
		double largest = 0.0;
		for (const Step& step : steps)
			largest = std::max(largest, step.Chroma);
		RampMeasure measure;
		for (const Step& step : steps)
		{
			if (step.Chroma >= 0.15 * largest)
				measure.WorstHue = std::max(measure.WorstHue, HueDifference(step.Hue, step.ReferenceHue));
			if (step.Stops == 0.0)
				measure.Saturation = step.Chroma / step.ReferenceChroma;
		}
		return measure;
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

	TEST_CASE("Filmic meets its targets")
	{
		const PostProcessMath::FilmicCurve look = PostProcessMath::MakeFilmicCurve(0.5f, 0.5f);
		// T1: middle gray shows as middle gray.
		CHECK(PostProcessMath::EvaluateFilmicCurve(0.18f, look) == doctest::Approx(0.18f).epsilon(1e-5));
		// T2: the log-log slope at middle gray is C0, within the fit's range.
		constexpr float c_Stops = 1e-3f;
		const float slope = (std::log2(PostProcessMath::EvaluateFilmicCurve(0.18f * std::exp2(c_Stops), look))
			- std::log2(PostProcessMath::EvaluateFilmicCurve(0.18f * std::exp2(-c_Stops), look))) / (2.0f * c_Stops);
		CHECK(std::abs(slope - FilmicConstants::Contrast) < 0.01f);
		CHECK(FilmicConstants::Contrast >= 1.35f);
		CHECK(FilmicConstants::Contrast <= 1.45f);
		// T3: white Tw stops above middle gray; +6 stops at least code 250.
		CHECK(PostProcessMath::EvaluateFilmicCurve(0.18f * std::exp2(look.WhiteStops), look) == 1.0f);
		CHECK(PostProcessMath::EvaluateFilmicCurve(0.18f * 64.0f, look) >= 0.955f);
		// T4: 0.005 shows at code 2 to 4.
		const float toeCode = PostProcessMath::LinearToSRGB(glm::vec3(PostProcessMath::EvaluateFilmicCurve(0.005f, look))).r * 255.0f;
		CHECK(toeCode >= 2.0f);
		CHECK(toeCode <= 4.0f);
		for (const FilmicTestColor& color : c_FilmicTestColors)
		{
			CAPTURE(std::string(color.Name));
			// T5 and T7.
			const RampMeasure measure = MeasureRamp(color.Color, look);
			CHECK(measure.WorstHue <= color.HueLimit);
			CHECK(measure.Saturation >= 0.90);
			CHECK(measure.Saturation <= 1.20);
			// T6: one stop past white every channel is at least 0.95.
			const glm::dvec3 white = Filmic(ScaledToMiddleGray(color.Color) * std::exp2(static_cast<double>(look.WhiteStops) + 1.0), look);
			CHECK(std::min({ white.r, white.g, white.b }) >= 0.95);
		}
	}

	TEST_CASE("Filmic's look keeps the curve monotonic and soft")
	{
		for (float toe = 0.0f; toe <= 1.0f; toe += 0.25f)
		{
			for (float shoulder = 0.0f; shoulder <= 1.0f; shoulder += 0.25f)
			{
				CAPTURE(toe);
				CAPTURE(shoulder);
				const PostProcessMath::FilmicCurve look = PostProcessMath::MakeFilmicCurve(toe, shoulder);
				CHECK(look.ShoulderPower >= 1.0f);
				CHECK(look.WhiteStops == doctest::Approx(6.5f + 3.0f * (shoulder - 0.5f)));
				// White exactly at Tw stops above middle gray, and just below it not yet.
				CHECK(PostProcessMath::EvaluateFilmicCurve(0.18f * std::exp2(look.WhiteStops), look) == 1.0f);
				CHECK(PostProcessMath::EvaluateFilmicCurve(0.18f * std::exp2(look.WhiteStops - 0.05f), look) < 1.0f);
				// T8: monotonic per channel and in luminance from -12 to +10 stops, for gray and every test color.
				std::vector<glm::dvec3> colors = { glm::dvec3(1.0) };
				for (const FilmicTestColor& color : c_FilmicTestColors)
					colors.push_back(color.Color);
				for (const glm::dvec3& color : colors)
				{
					glm::dvec3 previous(0.0);
					for (int step = 0; step <= 88; step++)
					{
						const glm::dvec3 shown = Filmic(ScaledToMiddleGray(color) * std::exp2(-12.0 + 0.25 * step), look);
						CHECK(glm::all(glm::greaterThanEqual(shown, previous - 1e-6)));
						CHECK(Luminance(shown) >= Luminance(previous) - 1e-6);
						previous = shown;
					}
				}
			}
		}
		// The toe deepens with Toe; the shoulder reaches white later with Shoulder.
		CHECK(PostProcessMath::EvaluateFilmicCurve(0.005f, PostProcessMath::MakeFilmicCurve(0.0f, 0.5f))
			> PostProcessMath::EvaluateFilmicCurve(0.005f, PostProcessMath::MakeFilmicCurve(1.0f, 0.5f)));
		CHECK(PostProcessMath::MakeFilmicCurve(0.5f, 0.0f).WhiteStops == doctest::Approx(5.0f));
		CHECK(PostProcessMath::MakeFilmicCurve(0.5f, 1.0f).WhiteStops == doctest::Approx(8.0f));
		// Out of range and non-finite looks clamp.
		CHECK(PostProcessMath::MakeFilmicCurve(-1.0f, 2.0f).WhiteStops == doctest::Approx(8.0f));
		CHECK(std::isfinite(PostProcessMath::MakeFilmicCurve(std::numeric_limits<float>::quiet_NaN(), 0.5f).ToeSlope));
		// Neutral colors pass the gamut unchanged: gray is exactly the curve.
		const PostProcessMath::FilmicCurve look = PostProcessMath::MakeFilmicCurve(0.5f, 0.5f);
		for (float value : { 0.001f, 0.05f, 0.18f, 1.0f, 4.0f })
			CHECK(PostProcessMath::ApplyFilmic(glm::vec3(value), look).g == doctest::Approx(PostProcessMath::EvaluateFilmicCurve(value, look)).epsilon(1e-4));
		CHECK(PostProcessMath::ApplyFilmic(glm::vec3(0.0f), look) == glm::vec3(0.0f));
		CHECK(PostProcessMath::ApplyFilmic(glm::vec3(-1.0f), look) == glm::vec3(0.0f));
	}
}
