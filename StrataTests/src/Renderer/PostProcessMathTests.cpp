#include <doctest/doctest.h>

#include "Strata/Renderer/FilmicConstants.h"
#include "Strata/Renderer/PostProcessMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
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

	glm::dvec2 XYToUV(const glm::dvec2& xy)
	{
		const double denominator = -2.0 * xy.x + 12.0 * xy.y + 3.0;
		return glm::dvec2(4.0 * xy.x / denominator, 6.0 * xy.y / denominator);
	}

	// The CIE daylight locus's chromaticity polynomial (CIE 15) at a nominal temperature (corrected by 1.4388 / 1.4380).
	glm::dvec2 DaylightUV(double kelvin)
	{
		const double t = kelvin * 1.4388 / 1.4380;
		const double x = t <= 7000.0 ? -4.6070e9 / (t * t * t) + 2.9678e6 / (t * t) + 0.09911e3 / t + 0.244063
									 : -2.0064e9 / (t * t * t) + 1.9018e6 / (t * t) + 0.24748e3 / t + 0.237040;
		return XYToUV(glm::dvec2(x, -3.000 * x * x + 2.870 * x - 0.275));
	}

	// Planck's law against the CIE 1931 2-degree observer at 10 nm (every second row of its 5 nm table).
	glm::dvec2 PlanckianUV(double kelvin)
	{
		struct Row
		{
			double Wavelength;
			double X;
			double Y;
			double Z;
		};
		constexpr Row c_Observer[] = {
			{ 380, 0.001368, 3.9e-05, 0.006450001 },
			{ 390, 0.004243, 0.00012, 0.02005001 },
			{ 400, 0.01431, 0.000396, 0.06785001 },
			{ 410, 0.04351, 0.00121, 0.2074 },
			{ 420, 0.13438, 0.004, 0.6456 },
			{ 430, 0.2839, 0.0116, 1.3856 },
			{ 440, 0.34828, 0.023, 1.74706 },
			{ 450, 0.3362, 0.038, 1.77211 },
			{ 460, 0.2908, 0.06, 1.6692 },
			{ 470, 0.19536, 0.09098, 1.28764 },
			{ 480, 0.09564, 0.13902, 0.8129501 },
			{ 490, 0.03201, 0.20802, 0.46518 },
			{ 500, 0.0049, 0.323, 0.272 },
			{ 510, 0.0093, 0.503, 0.1582 },
			{ 520, 0.06327, 0.71, 0.07824999 },
			{ 530, 0.1655, 0.862, 0.04216 },
			{ 540, 0.2904, 0.954, 0.0203 },
			{ 550, 0.4334499, 0.9949501, 0.008749999 },
			{ 560, 0.5945, 0.995, 0.0039 },
			{ 570, 0.7621, 0.952, 0.0021 },
			{ 580, 0.9163, 0.87, 0.001650001 },
			{ 590, 1.0263, 0.757, 0.0011 },
			{ 600, 1.0622, 0.631, 0.0008 },
			{ 610, 1.0026, 0.503, 0.00034 },
			{ 620, 0.8544499, 0.381, 0.00019 },
			{ 630, 0.6424, 0.265, 4.999999e-05 },
			{ 640, 0.4479, 0.175, 2e-05 },
			{ 650, 0.2835, 0.107, 0.0 },
			{ 660, 0.1649, 0.061, 0.0 },
			{ 670, 0.0874, 0.032, 0.0 },
			{ 680, 0.04677, 0.017, 0.0 },
			{ 690, 0.0227, 0.00821, 0.0 },
			{ 700, 0.01135916, 0.004102, 0.0 },
			{ 710, 0.005790346, 0.002091, 0.0 },
			{ 720, 0.002899327, 0.001047, 0.0 },
			{ 730, 0.001439971, 0.00052, 0.0 },
			{ 740, 0.0006900786, 0.0002492, 0.0 },
			{ 750, 0.0003323011, 0.00012, 0.0 },
			{ 760, 0.0001661505, 6e-05, 0.0 },
			{ 770, 8.307527e-05, 3e-05, 0.0 },
			{ 780, 4.150994e-05, 1.499e-05, 0.0 },
		};
		glm::dvec3 xyz(0.0);
		for (const Row& row : c_Observer)
		{
			const double meters = row.Wavelength * 1e-9;
			const double radiance = std::pow(meters, -5.0) / std::expm1(1.4388e-2 / (meters * kelvin));
			xyz += radiance * glm::dvec3(row.X, row.Y, row.Z);
		}
		const double denominator = xyz.x + 15.0 * xyz.y + 3.0 * xyz.z;
		return glm::dvec2(4.0 * xyz.x / denominator, 6.0 * xyz.y / denominator);
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

	TEST_CASE("White balance at 6500 K and tint 0 is the identity")
	{
		CHECK(PostProcessMath::GetWhiteBalanceMatrix(6500.0f, 0.0f) == glm::mat3(1.0f));
		const PostProcessMath::ColorGrading neutral = PostProcessMath::MakeColorGrading(6500.0f, 0.0f, glm::vec3(1.0f), glm::vec3(0.0f), glm::vec3(1.0f));
		CHECK_FALSE(neutral.Active);
		CHECK(PostProcessMath::ApplyColorGrading(glm::vec3(0.3f, 0.2f, 0.1f), neutral) == glm::vec3(0.3f, 0.2f, 0.1f));
		// Near 6500 K the matrix is near the identity: the default lies on the locus (no jump).
		const glm::mat3 near = PostProcessMath::GetWhiteBalanceMatrix(6550.0f, 0.0f);
		for (int column = 0; column < 3; column++)
		{
			for (int row = 0; row < 3; row++)
				CHECK(std::abs(near[column][row] - (column == row ? 1.0f : 0.0f)) < 0.01f);
		}
	}

	TEST_CASE("White balance neutralizes the light it names")
	{
		for (double kelvin : { 3200.0, 4500.0, 9000.0 })
		{
			for (float tint : { 0.0f, 0.5f, -1.0f })
			{
				const glm::dvec4 locus = PostProcessMath::GetWhiteLocus(kelvin);
				const glm::dvec2 light = glm::dvec2(locus.x, locus.y) + 0.02 * static_cast<double>(tint) * glm::dvec2(locus.z, locus.w);
				const glm::vec3 shown = PostProcessMath::GetWhiteBalanceMatrix(static_cast<float>(kelvin), tint) * glm::vec3(PostProcessMath::ChromaticityToLinearSRGB(light));
				CAPTURE(kelvin);
				CAPTURE(tint);
				CHECK(std::abs(shown.r - shown.g) < 1e-3f * shown.g);
				CHECK(std::abs(shown.b - shown.g) < 1e-3f * shown.g);
			}
		}
	}

	TEST_CASE("Lower temperatures cool the image and tint moves between green and magenta")
	{
		const glm::vec3 gray(0.18f);
		const glm::vec3 tungsten = PostProcessMath::GetWhiteBalanceMatrix(3200.0f, 0.0f) * gray;
		CHECK(tungsten.b > tungsten.r); // A light named warmer than it is: the white-lit image turns blue
		const glm::vec3 shade = PostProcessMath::GetWhiteBalanceMatrix(9000.0f, 0.0f) * gray;
		CHECK(shade.r > shade.b); // Named cooler: it warms
		const glm::vec3 magenta = PostProcessMath::GetWhiteBalanceMatrix(6500.0f, 1.0f) * gray;
		CHECK(magenta.g < magenta.r);
		CHECK(magenta.g < magenta.b);
		const glm::vec3 green = PostProcessMath::GetWhiteBalanceMatrix(6500.0f, -1.0f) * gray;
		CHECK(green.g > green.r);
		CHECK(green.g > green.b);
	}

	TEST_CASE("The white locus matches Planck's law below 4000 K and the daylight formula above 5000 K")
	{
		const std::span<const PostProcessMath::WhiteLocusEntry> table = PostProcessMath::GetWhiteLocusTable();
		REQUIRE(table.size() == 91);
		CHECK(table.front().Mireds == 60.0);
		CHECK(table.back().Mireds == 510.0);
		auto entry = [&](double mireds) -> const PostProcessMath::WhiteLocusEntry&
		{
			const size_t index = static_cast<size_t>((mireds - 60.0) / 5.0);
			REQUIRE(table[index].Mireds == mireds);
			return table[index];
		};
		// Planck's law against the CIE 1931 2-degree observer's 10 nm table.
		for (double mireds : { 250.0, 300.0, 350.0, 420.0, 500.0 })
		{
			const glm::dvec2 expected = PlanckianUV(1e6 / mireds);
			CAPTURE(mireds);
			CHECK(std::abs(entry(mireds).U - expected.x) < 5e-4);
			CHECK(std::abs(entry(mireds).V - expected.y) < 5e-4);
		}
		// The CIE daylight formula (shifted onto D65 at 6500 K, by less than 1e-4).
		for (double mireds : { 60.0, 100.0, 150.0, 175.0, 200.0 })
		{
			const glm::dvec2 expected = DaylightUV(1e6 / mireds);
			CAPTURE(mireds);
			CHECK(std::abs(entry(mireds).U - expected.x) < 5e-4);
			CHECK(std::abs(entry(mireds).V - expected.y) < 5e-4);
		}
		// 6500 K is sRGB's white, D65.
		const glm::dvec4 at6500 = PostProcessMath::GetWhiteLocus(6500.0);
		const glm::dvec2 d65 = XYToUV(glm::dvec2(0.3127, 0.3290));
		CHECK(std::abs(at6500.x - d65.x) < 1e-5);
		CHECK(std::abs(at6500.y - d65.y) < 1e-5);
		// No jump: consecutive entries lie close, and the normals are unit length, toward green.
		for (size_t index = 0; index + 1 < table.size(); index++)
		{
			CHECK(std::hypot(table[index + 1].U - table[index].U, table[index + 1].V - table[index].V) < 0.004);
			CHECK(std::abs(std::hypot(table[index].NormalU, table[index].NormalV) - 1.0) < 1e-6);
			CHECK(table[index].NormalV > 0.0);
		}
	}

	TEST_CASE("Grading applies slope, offset and power around middle gray")
	{
		const PostProcessMath::ColorGrading grading = PostProcessMath::MakeColorGrading(6500.0f, 0.0f, glm::vec3(2.0f, 1.0f, 0.5f), glm::vec3(0.1f, 0.0f, -0.1f),
			glm::vec3(1.0f, 2.0f, 1.0f));
		CHECK(grading.Active);
		const glm::vec3 graded = PostProcessMath::ApplyColorGrading(glm::vec3(0.18f), grading);
		CHECK(graded.r == doctest::Approx(0.18f * 2.0f + 0.018f));
		CHECK(graded.g == doctest::Approx(0.18f)); // A power keeps middle gray
		CHECK(graded.b == doctest::Approx(0.18f * 0.5f - 0.018f));
		CHECK(PostProcessMath::ApplyColorGrading(glm::vec3(0.36f), grading).g == doctest::Approx(0.72f)); // (2)^2 times middle gray
		// Out-of-range and non-finite values clamp to the properties' ranges or are neutral.
		const PostProcessMath::ColorGrading clamped = PostProcessMath::MakeColorGrading(std::numeric_limits<float>::quiet_NaN(), 5.0f, glm::vec3(10.0f),
			glm::vec3(-1.0f), glm::vec3(0.0f));
		CHECK(clamped.Power == glm::vec3(0.2f));
		CHECK(clamped.Offset == glm::vec3(-0.1f * 0.18f));
		CHECK(clamped.Matrix == PostProcessMath::GetWhiteBalanceMatrix(6500.0f, 1.0f) * 4.0f);
	}

	TEST_CASE("The motion blur's model blurs an edge over its length and keeps still edges")
	{
		const PostProcessMath::MotionBlurProfile still = PostProcessMath::GetMotionBlurProfile(0.5f, 12);
		for (size_t index = 0; index < still.Mean.size(); index++)
			CHECK(still.Mean[index] == (still.FirstPixel + static_cast<int>(index) >= 0 ? 1.0f : 0.0f));
		for (const float length : { 6.0f, 12.0f, 24.0f })
		{
			const PostProcessMath::MotionBlurProfile profile = PostProcessMath::GetMotionBlurProfile(length, 12);
			uint32_t width = 0;
			float previous = 0.0f;
			for (size_t index = 0; index < profile.Mean.size(); index++)
			{
				width += profile.Mean[index] > 1e-3f && profile.Mean[index] < 1.0f - 1e-3f ? 1u : 0u;
				CHECK(profile.Mean[index] >= previous - 1e-6f); // Monotonic: no doubled edges
				previous = profile.Mean[index];
			}
			// The samples lie length / N apart: the outermost reach within that of the span's ends.
			CAPTURE(length);
			CHECK(std::abs(static_cast<float>(width) - length) <= std::max(1.0f, length / 12.0f));
		}
		// More samples, less streak noise.
		float spread8 = 0.0f;
		float spread16 = 0.0f;
		for (float value : PostProcessMath::GetMotionBlurProfile(12.0f, 8).Spread)
			spread8 = std::max(spread8, value);
		for (float value : PostProcessMath::GetMotionBlurProfile(12.0f, 16).Spread)
			spread16 = std::max(spread16, value);
		CHECK(spread16 < spread8);
	}
}
