#include <doctest/doctest.h>

#include "Strata/Core/FileSystem.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/AreaLights.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace Strata;

// What the tests compare with comes from outside the code under test: the tables' digits as the authors published them,
// closed forms (Howell's catalog of configuration factors, GGX's integrals where they have one) and, for the specular
// lobe, a quadrature of the BRDF itself. The double-precision references below share no code with AreaLights.cpp: they
// use the exact arc instead of its fit and clip polygons point by point.

namespace
{

	constexpr double c_Pi = 3.14159265358979323846;

	// Uniform numbers that are the same with every standard library (the distributions of <random> are not).
	class Sequence
	{
	public:
		explicit Sequence(uint32_t seed)
			: m_Engine(seed)
		{
		}

		double Next()
		{
			return static_cast<double>(m_Engine() >> 8) * (1.0 / 16777216.0);
		}

		double Next(double from, double to)
		{
			return from + (to - from) * Next();
		}

	private:
		std::mt19937 m_Engine;
	};

	// A receiver's frame somewhere in the world: the receiver is at its origin and its normal is its +Z. Nothing the tests
	// place is aligned with the world's axes.
	struct Frame
	{
		glm::dmat3 Rotation = glm::dmat3(1.0);
		glm::dvec3 Origin = glm::dvec3(0.0);

		glm::vec3 GetPoint(const glm::dvec3& local) const
		{
			return glm::vec3(Origin + Rotation * local);
		}

		glm::vec3 GetDirection(const glm::dvec3& local) const
		{
			return glm::vec3(Rotation * local);
		}
	};

	Frame MakeFrame()
	{
		Frame frame;
		frame.Rotation = glm::dmat3(glm::rotate(glm::dmat4(1.0), 0.9, glm::normalize(glm::dvec3(0.3, -0.8, 0.52))));
		frame.Origin = glm::dvec3(0.7, -1.3, 2.1);
		return frame;
	}

	// A light given in a receiver's frame: `normal` and `axisX` are unit and perpendicular.
	AreaLights::AreaLight MakeLight(const Frame& frame, const glm::dvec3& center, const glm::dvec3& normal, const glm::dvec3& axisX, double halfX,
		double halfY, bool twoSided = false)
	{
		AreaLights::AreaLight light;
		light.Center = frame.GetPoint(center);
		light.Normal = frame.GetDirection(normal);
		light.AxisX = frame.GetDirection(axisX * halfX);
		light.AxisY = frame.GetDirection(glm::cross(axisX, normal) * halfY);
		light.Radiance = glm::vec3(1.0f);
		light.TwoSided = twoSided;
		return light;
	}

	// A light facing down from the plane z = height above a receiver: x0..x1 by y0..y1.
	AreaLights::AreaLight MakeParallelLight(const Frame& frame, double x0, double x1, double y0, double y1, double height)
	{
		return MakeLight(frame, glm::dvec3((x0 + x1) * 0.5, (y0 + y1) * 0.5, height), glm::dvec3(0.0, 0.0, -1.0), glm::dvec3(1.0, 0.0, 0.0),
			(x1 - x0) * 0.5, (y1 - y0) * 0.5);
	}

	// A light in the plane x = distance, facing the receiver's side: y0..y1 wide, from the height z0 to z1.
	AreaLights::AreaLight MakeStandingLight(const Frame& frame, double y0, double y1, double z0, double z1, double distance)
	{
		return MakeLight(frame, glm::dvec3(distance, (y0 + y1) * 0.5, (z0 + z1) * 0.5), glm::dvec3(-1.0, 0.0, 0.0), glm::dvec3(0.0, 1.0, 0.0),
			(y1 - y0) * 0.5, (z1 - z0) * 0.5);
	}

	// Howell's catalog of configuration factors, B-3: a plane element and a rectangle of sides a and b in a parallel plane
	// at the distance c, the element's normal passing through one of the rectangle's corners.
	double GetCornerFactor(double a, double b, double c)
	{
		const double u = std::sqrt(a * a + c * c);
		const double v = std::sqrt(b * b + c * c);
		return (a / u * std::atan(b / u) + b / v * std::atan(a / v)) / (2.0 * c_Pi);
	}

	// Any rectangle of a parallel plane, x0..x1 by y0..y1 around the foot of the element's normal: four corner rectangles,
	// added and taken away.
	double GetParallelFactor(double x0, double x1, double y0, double y1, double c)
	{
		const auto corner = [c](double x, double y)
		{
			const double sign = (x < 0.0 ? -1.0 : 1.0) * (y < 0.0 ? -1.0 : 1.0);
			return sign * GetCornerFactor(std::abs(x), std::abs(y), c);
		};
		return corner(x1, y1) - corner(x0, y1) - corner(x1, y0) + corner(x0, y0);
	}

	// Howell's B-4: a plane element and a rectangle in a plane perpendicular to the element's at the distance c, standing
	// on the element's plane with a corner opposite the element: a high (along the element's normal) and b wide.
	double GetStandingFactor(double a, double b, double c)
	{
		const double u = std::sqrt(a * a + c * c);
		return (std::atan(b / c) - c / u * std::atan(b / u)) / (2.0 * c_Pi);
	}

	// Any rectangle of that plane, y0..y1 wide and from the height z0 to z1 over the element's plane; what is under the
	// plane lights nothing.
	double GetPerpendicularFactor(double y0, double y1, double z0, double z1, double c)
	{
		const auto standing = [c](double y, double z)
		{
			if (z <= 0.0)
				return 0.0;
			return (y < 0.0 ? -1.0 : 1.0) * GetStandingFactor(z, std::abs(y), c);
		};
		return standing(y1, z1) - standing(y0, z1) - standing(y1, z0) + standing(y0, z0);
	}

	// The corners of the light the float code sees, from the position, in double.
	std::array<glm::dvec3, 4> GetCorners(const glm::vec3& position, const AreaLights::AreaLight& light)
	{
		const glm::dvec3 center = glm::dvec3(light.Center) - glm::dvec3(position);
		const glm::dvec3 x = glm::dvec3(light.AxisX);
		const glm::dvec3 y = glm::dvec3(light.AxisY);
		return { center - x - y, center + x - y, center + x + y, center - x + y };
	}

	// An edge's exact share of the vector form factor: the angle it is seen under over 2 pi, along the edge plane's normal.
	glm::dvec3 IntegrateReferenceEdge(const glm::dvec3& start, const glm::dvec3& end)
	{
		const glm::dvec3 normal = glm::cross(start, end);
		const double length = glm::length(normal);
		if (!(length > 0.0))
			return glm::dvec3(0.0);
		return normal * (std::atan2(length, glm::dot(start, end)) / (2.0 * c_Pi * length));
	}

	glm::dvec3 GetReferenceVectorFormFactor(const glm::vec3& position, const AreaLights::AreaLight& light)
	{
		const std::array<glm::dvec3, 4> corners = GetCorners(position, light);
		glm::dvec3 sum(0.0);
		for (size_t index = 0; index < corners.size(); index++)
			sum += IntegrateReferenceEdge(corners[index], corners[(index + 1) % corners.size()]);
		return sum;
	}

	// 1 in front of the light, -1 behind it, 0 on its plane.
	double GetReferenceSide(const glm::vec3& position, const AreaLights::AreaLight& light)
	{
		const double distance = glm::dot(glm::dvec3(light.Normal), glm::dvec3(position) - glm::dvec3(light.Center));
		return distance > 0.0 ? 1.0 : (distance < 0.0 ? -1.0 : 0.0);
	}

	// The exact form factor: the face clipped at the receiver's horizon point by point, then the edges' arcs. For a
	// one-sided light it is what the front receives; behind, what a two-sided light gives.
	double GetReferenceFormFactor(const glm::vec3& position, const glm::dvec3& normal, const AreaLights::AreaLight& light)
	{
		const std::array<glm::dvec3, 4> corners = GetCorners(position, light);
		std::vector<glm::dvec3> clipped;
		for (size_t index = 0; index < corners.size(); index++)
		{
			const glm::dvec3& start = corners[index];
			const glm::dvec3& end = corners[(index + 1) % corners.size()];
			const double startHeight = glm::dot(start, normal);
			const double endHeight = glm::dot(end, normal);
			if (startHeight > 0.0)
				clipped.push_back(start);
			if ((startHeight > 0.0) != (endHeight > 0.0))
				clipped.push_back(start + (end - start) * (startHeight / (startHeight - endHeight)));
		}
		double sum = 0.0;
		for (size_t index = 0; index < clipped.size(); index++)
			sum += glm::dot(IntegrateReferenceEdge(clipped[index], clipped[(index + 1) % clipped.size()]), normal);
		return std::max(sum * GetReferenceSide(position, light), 0.0);
	}

	double GetRelativeError(double value, double reference)
	{
		return std::abs(value - reference) / std::abs(reference);
	}

	std::string Describe(const AreaLights::AreaLight& light)
	{
		return "center " + std::to_string(light.Center.x) + " " + std::to_string(light.Center.y) + " " + std::to_string(light.Center.z) + ", half sizes " +
			std::to_string(glm::length(light.AxisX)) + " " + std::to_string(glm::length(light.AxisY));
	}

}

TEST_SUITE("Renderer.AreaLights")
{
	TEST_CASE("The tables hold two layers of finite values and transforms that can be inverted")
	{
		constexpr size_t c_LayerValues = static_cast<size_t>(AreaLights::c_TableSize) * AreaLights::c_TableSize * AreaLights::c_TableChannels;
		REQUIRE(c_LayerValues == 16384);
		for (uint32_t layer = 0; layer < AreaLights::c_TableLayers; layer++)
		{
			const std::span<const float> values = AreaLights::GetTableLayer(layer);
			REQUIRE(values.size() == c_LayerValues);
			CHECK(std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); }));
		}
		CHECK(AreaLights::GetTableLayer(AreaLights::c_TableLayers).empty());
		CHECK(AreaLights::LoadTable(AreaLights::c_TableLayers, 0, 0) == glm::vec4(0.0f));

		// Layer 0's last term scales the direction along the normal: zero or less would fold the lobe. The determinant of
		// the whole transform stays positive too, so a face keeps its winding in the lobe's space.
		const std::span<const float> transforms = AreaLights::GetTableLayer(AreaLights::c_TransformLayer);
		const std::span<const float> terms = AreaLights::GetTableLayer(AreaLights::c_TermsLayer);
		bool positive = true;
		bool inRange = true;
		for (size_t texel = 0; texel < c_LayerValues; texel += AreaLights::c_TableChannels)
		{
			const float determinant = transforms[texel] * transforms[texel + 3] - transforms[texel + 2] * transforms[texel + 1];
			positive = positive && transforms[texel + 3] > 0.0f && determinant > 0.0f;
			// A magnitude is an albedo, a Fresnel term a part of it, and the horizon term a share.
			inRange = inRange && terms[texel] > 0.0f && terms[texel] <= 1.0f && terms[texel + 1] >= 0.0f && terms[texel + 1] <= terms[texel] &&
				terms[texel + 2] == 0.0f && terms[texel + 3] >= 0.0f && terms[texel + 3] <= 1.0f;
		}
		CHECK(positive);
		CHECK(inRange);
	}

	TEST_CASE("Table lookups return the published values at the corners and in the middle")
	{
		// fit/results/ltc.js of the authors' repository, digit for digit: entry a + t * 64 of g_ltc_1 and g_ltc_2.
		struct Entry
		{
			uint32_t Roughness = 0;
			uint32_t View = 0;
			glm::dvec4 Transform = glm::dvec4(0.0);
			glm::dvec4 Terms = glm::dvec4(0.0);
		};
		const Entry entries[] = {
			{ 0, 0, { 1.0, 0.0, 0.0, 2e-05 }, { 1.0, 0.0, 0.0, 0.0 } },
			{ 63, 0, { 1.0, 0.0, 0.0, 1.12813 }, { 0.306974, 3.39596e-05, 0.0, 1.0 } },
			{ 0, 63, { 6.19747e-06, -1e-07, 0.00778326, 7.96756e-11 }, { 0.999804, 0.995833, 0.0, 0.0 } },
			{ 63, 63, { 0.996389, -0.0808124, 0.0489007, 1.6577 }, { 0.932164, 0.0471899, 0.0, 1.0 } },
			{ 32, 32, { 0.626092, -0.219644, 0.531595, 0.312202 }, { 0.88238, 0.00296637, 0.0, 0.19188 } },
			{ 31, 31, { 0.634192, -0.207816, 0.520422, 0.299377 }, { 0.897731, 0.00248374, 0.0, 0.171672 } },
			{ 20, 40, { 0.363847, -0.0935002, 0.484947, 0.0758086 }, { 0.977085, 0.0129694, 0.0, 0.0714114 } },
			{ 40, 20, { 0.900589, -0.208474, 0.371752, 0.63723 }, { 0.774847, 0.000406563, 0.0, 0.302468 } },
		};
		for (const Entry& entry : entries)
		{
			CAPTURE(entry.Roughness);
			CAPTURE(entry.View);
			const glm::vec4 transform = AreaLights::LoadTable(AreaLights::c_TransformLayer, entry.Roughness, entry.View);
			const glm::vec4 terms = AreaLights::LoadTable(AreaLights::c_TermsLayer, entry.Roughness, entry.View);
			for (int channel = 0; channel < 4; channel++)
			{
				CAPTURE(channel);
				// To a float's precision: the published decimals reach the table through one rounding and this list through two.
				CHECK(std::abs(transform[channel] - entry.Transform[channel]) <= 1e-6 * std::abs(entry.Transform[channel]));
				CHECK(std::abs(terms[channel] - entry.Terms[channel]) <= 1e-6 * std::abs(entry.Terms[channel]));
			}

			// A sample at a texel's center is the texel, and so is the layer's own storage at a + t * 64.
			const glm::vec2 uv = glm::vec2(static_cast<float>(entry.Roughness), static_cast<float>(entry.View)) / 63.0f;
			const glm::vec4 sampled = AreaLights::SampleTable(AreaLights::c_TransformLayer, uv);
			const size_t offset = (static_cast<size_t>(entry.Roughness) + static_cast<size_t>(entry.View) * 64) * 4;
			for (int channel = 0; channel < 4; channel++)
			{
				CAPTURE(channel);
				CHECK(std::abs(sampled[channel] - transform[channel]) <= 1e-5f);
				CHECK(AreaLights::GetTableLayer(AreaLights::c_TransformLayer)[offset + static_cast<size_t>(channel)] == transform[channel]);
			}
		}

		// Indexes past the last texel read the last one.
		CHECK(AreaLights::LoadTable(AreaLights::c_TransformLayer, 64, 900) == AreaLights::LoadTable(AreaLights::c_TransformLayer, 63, 63));
	}

	TEST_CASE("Samples between texels weigh the four around them, clamped to the table")
	{
		const auto load = [](uint32_t x, uint32_t y) { return glm::dvec4(AreaLights::LoadTable(AreaLights::c_TransformLayer, x, y)); };
		// A quarter of the way from texel (20, 40) to (21, 40) and three quarters of the way to row 41.
		const glm::vec2 uv = glm::vec2(20.25f, 40.75f) / 63.0f;
		const glm::dvec4 expected = (load(20, 40) * 0.75 + load(21, 40) * 0.25) * 0.25 + (load(20, 41) * 0.75 + load(21, 41) * 0.25) * 0.75;
		const glm::vec4 sampled = AreaLights::SampleTable(AreaLights::c_TransformLayer, uv);
		for (int channel = 0; channel < 4; channel++)
			CHECK(std::abs(sampled[channel] - expected[channel]) <= 1e-5);

		// Outside the table and for a value that is no number, the nearest edge.
		CHECK(AreaLights::SampleTable(AreaLights::c_TransformLayer, glm::vec2(-3.0f, 7.0f)) == AreaLights::LoadTable(AreaLights::c_TransformLayer, 0, 63));
		CHECK(AreaLights::SampleTable(AreaLights::c_TransformLayer, glm::vec2(2.0f, -1.0f)) == AreaLights::LoadTable(AreaLights::c_TransformLayer, 63, 0));
		const float nan = std::numeric_limits<float>::quiet_NaN();
		CHECK(AreaLights::SampleTable(AreaLights::c_TransformLayer, glm::vec2(nan, nan)) == AreaLights::LoadTable(AreaLights::c_TransformLayer, 0, 0));
	}

	TEST_CASE("The lobe seen along the normal is isotropic, and the tables agree with GGX where it has a closed form")
	{
		for (uint32_t roughness = 0; roughness < AreaLights::c_TableSize; roughness++)
		{
			CAPTURE(roughness);
			const glm::vec4 transform = AreaLights::LoadTable(AreaLights::c_TransformLayer, roughness, 0);
			CHECK(transform.x == doctest::Approx(1.0).epsilon(1e-6));
			CHECK(std::abs(transform.y) <= 1e-6f);
			CHECK(std::abs(transform.z) <= 1e-6f);

			// A sharp lobe seen along the normal: at the small angle theta between the light and the mirror's direction
			// GGX's D goes as 1 / (alpha^2 + (theta / 2)^2)^2, the half vector being theta / 2 from the normal, and a cosine
			// squeezed by the last term w as 1 / (w^2 + theta^2)^2: the same lobe for w = 2 alpha. The fit follows that to
			// a percent up to a roughness of a quarter.
			if (roughness >= 1 && roughness <= 16)
			{
				const double alpha = (roughness / 63.0) * (roughness / 63.0);
				CHECK(GetRelativeError(transform.w, 2.0 * alpha) <= 0.01);
			}
		}
		const glm::vec4 rough = AreaLights::LoadTable(AreaLights::c_TransformLayer, 63, 0);
		CHECK(rough.w == doctest::Approx(1.12813).epsilon(1e-5));

		// A mirror reflects everything. At alpha 1 and along the normal D is 1 / pi and the visibility 0.5 / (1 + NoL): the
		// albedo is the integral of NoL / (2 pi (1 + NoL)) over the hemisphere, 1 - ln 2.
		CHECK(AreaLights::LoadTable(AreaLights::c_TermsLayer, 0, 0).x == 1.0f);
		CHECK(AreaLights::LoadTable(AreaLights::c_TermsLayer, 63, 0).x == doctest::Approx(1.0 - std::log(2.0)).epsilon(1e-3));

		// The horizon term for a vanishing cap is the cosine of where the cap is, 0 under the horizon.
		for (uint32_t x = 0; x < AreaLights::c_TableSize; x++)
		{
			CAPTURE(x);
			const double cosine = x / 63.0 * 2.0 - 1.0;
			CHECK(AreaLights::LoadTable(AreaLights::c_TermsLayer, x, 0).w == doctest::Approx(std::max(cosine, 0.0)).epsilon(1e-4));
		}
	}

	TEST_CASE("The edge fit follows the arc over its sine")
	{
		double worst = 0.0;
		for (int step = 0; step <= 4000; step++)
		{
			const double cosine = step / 4000.0;
			const double angle = std::acos(cosine);
			// At a zero angle the arc over the sine is 1.
			const double exact = (step == 4000 ? 1.0 : angle / std::sin(angle)) / (2.0 * c_Pi);
			worst = std::max(worst, GetRelativeError(AreaLights::GetEdgeFit(static_cast<float>(cosine)), exact));
		}
		MESSAGE("edge fit: worst relative error " << worst);
		// A fixed tolerance against the closed form (measured 1.5e-06).
		CHECK(worst <= 3e-6);
	}
}

namespace
{

	glm::dvec3 GetRandomDirection(Sequence& sequence)
	{
		const double z = sequence.Next(-1.0, 1.0);
		const double angle = sequence.Next(0.0, 2.0 * c_Pi);
		const double radius = std::sqrt(std::max(1.0 - z * z, 0.0));
		return glm::dvec3(radius * std::cos(angle), radius * std::sin(angle), z);
	}

	// A light anywhere around a receiver, facing any way: its center within the box given, its half sizes within the range.
	AreaLights::AreaLight MakeRandomLight(const Frame& frame, Sequence& sequence, const glm::dvec3& boxMin, const glm::dvec3& boxMax, double minHalfSize,
		double maxHalfSize, bool twoSided)
	{
		// Drawn one statement at a time: the order in which a call's arguments are evaluated is each compiler's choice, and
		// the lights have to be the same ones everywhere for the measured bounds to mean anything.
		const double centerZ = sequence.Next(boxMin.z, boxMax.z);
		const double centerY = sequence.Next(boxMin.y, boxMax.y);
		const double centerX = sequence.Next(boxMin.x, boxMax.x);
		const glm::dvec3 center(centerX, centerY, centerZ);
		const glm::dvec3 normal = GetRandomDirection(sequence);
		glm::dvec3 axisX = glm::cross(normal, GetRandomDirection(sequence));
		while (glm::length(axisX) < 0.1)
			axisX = glm::cross(normal, GetRandomDirection(sequence));
		const double halfX = sequence.Next(minHalfSize, maxHalfSize);
		const double halfY = sequence.Next(minHalfSize, maxHalfSize);
		return MakeLight(frame, center, normal, glm::normalize(axisX), halfX, halfY, twoSided);
	}

	bool IsLit(const glm::vec3& position, const AreaLights::AreaLight& light)
	{
		const double side = GetReferenceSide(position, light);
		return side > 0.0 || (light.TwoSided && side < 0.0);
	}

}

TEST_SUITE("Renderer.AreaLights")
{
	TEST_CASE("The vector form factor follows the exact arcs")
	{
		const Frame frame = MakeFrame();
		const glm::vec3 position = frame.GetPoint(glm::dvec3(0.0));
		Sequence sequence(11);
		double worst = 0.0;
		for (int index = 0; index < 1000; index++)
		{
			const AreaLights::AreaLight light = MakeRandomLight(frame, sequence, glm::dvec3(-4.0, -4.0, -4.0), glm::dvec3(4.0, 4.0, 4.0), 0.02, 2.0, false);
			const glm::dvec3 reference = GetReferenceVectorFormFactor(position, light);
			const glm::dvec3 value = glm::dvec3(AreaLights::GetVectorFormFactor(position, light));
			worst = std::max(worst, glm::length(value - reference));
		}
		MESSAGE("vector form factor: worst absolute error " << worst);
		CHECK(worst <= 1e-5);

		// A 1 cm light 20 m away: the edges' terms are a thousand times the sum they leave.
		const AreaLights::AreaLight distant = MakeLight(frame, glm::dvec3(9.0, -12.0, 13.0), glm::normalize(glm::dvec3(-0.5, 0.7, -0.6)),
			glm::normalize(glm::dvec3(0.7, 0.5, 0.0)), 0.005, 0.005);
		const glm::dvec3 reference = GetReferenceVectorFormFactor(position, distant);
		const glm::dvec3 value = glm::dvec3(AreaLights::GetVectorFormFactor(position, distant));
		const double relative = glm::length(value - reference) / glm::length(reference);
		MESSAGE("vector form factor: 1 cm at 20 m, relative error " << relative << " of " << glm::length(reference));
		CHECK(relative <= 1e-3);
	}

	TEST_CASE("The clipped form factor matches Howell's configuration factors at 60 configurations")
	{
		const Frame frame = MakeFrame();
		const glm::vec3 position = frame.GetPoint(glm::dvec3(0.0));
		const glm::vec3 normal = frame.GetDirection(glm::dvec3(0.0, 0.0, 1.0));
		Sequence sequence(2);
		double worst = 0.0;
		const auto check = [&](const AreaLights::AreaLight& light, double expected, double tolerance)
		{
			const std::string description = Describe(light);
			CAPTURE(description);
			CAPTURE(expected);
			const double error = GetRelativeError(AreaLights::FormFactorClipped(position, normal, light), expected);
			worst = std::max(worst, error);
			CHECK(error <= tolerance);
		};

		// Lights overhead, facing down.
		for (int index = 0; index < 36; index++)
		{
			const double height = sequence.Next(0.2, 5.0);
			const double x0 = sequence.Next(-3.0, 2.5);
			const double x1 = x0 + sequence.Next(0.1, 3.0);
			const double y0 = sequence.Next(-3.0, 2.5);
			const double y1 = y0 + sequence.Next(0.1, 3.0);
			check(MakeParallelLight(frame, x0, x1, y0, y1, height), GetParallelFactor(x0, x1, y0, y1, height), 1e-4);
		}
		// Wall panels; the receiver's plane cuts those that start under it.
		for (int index = 0; index < 21; index++)
		{
			const double distance = sequence.Next(0.3, 4.0);
			const double y0 = sequence.Next(-3.0, 2.0);
			const double y1 = y0 + sequence.Next(0.2, 3.0);
			const double z0 = index % 3 == 0 ? sequence.Next(-1.5, -0.1) : sequence.Next(0.0, 1.5);
			const double z1 = std::max(z0, 0.0) + sequence.Next(0.3, 2.5);
			check(MakeStandingLight(frame, y0, y1, z0, z1, distance), GetPerpendicularFactor(y0, y1, z0, z1, distance), 1e-4);
		}
		MESSAGE("clipped against Howell: worst relative error of the 57 drawn " << worst);

		// The default panel of 1 x 0.5 m seen from 1 m under its center.
		CHECK(std::abs(GetParallelFactor(-0.5, 0.5, -0.25, 0.25, 1.0) - 0.13237) <= 5e-6);
		check(MakeParallelLight(frame, -0.5, 0.5, -0.25, 0.25, 1.0), 0.13237, 1e-4);
		// A point 1 mm under the middle of a face: the face is almost the whole hemisphere.
		check(MakeParallelLight(frame, -0.5, 0.5, -0.5, 0.5, 0.001), GetParallelFactor(-0.5, 0.5, -0.5, 0.5, 0.001), 1e-4);
		// A 1 cm light 20 m away.
		check(MakeParallelLight(frame, 4.0, 4.01, -3.0, -2.99, 19.5), GetParallelFactor(4.0, 4.01, -3.0, -2.99, 19.5), 1e-3);
		MESSAGE("clipped against Howell: worst relative error of all 60 " << worst);
	}

	TEST_CASE("The horizon term against the exact clip over 80,000 lights")
	{
		// The bounds below are measured, and a draw of 2000 lights held them by its seed: 40 other seeds of 2000 put the
		// worst face above the horizon past the bound of 0.02 of the length 16 times. 80,000 lights (the first 2000 are the
		// same) measure what the draw reaches: 50 other draws of 40,000, nine of 80,000 and one of 4 million stay within 1.5
		// times it.
		const Frame frame = MakeFrame();
		const glm::vec3 position = frame.GetPoint(glm::dvec3(0.0));
		const glm::vec3 normal = frame.GetDirection(glm::dvec3(0.0, 0.0, 1.0));
		// The normal the float code is given, so that the reference clips at the same horizon.
		const glm::dvec3 exactNormal = glm::dvec3(normal);
		Sequence sequence(3);

		int exactCount = 0;
		int aboveCount = 0;
		int cutCount = 0;
		double worstClipped = 0.0;     // FormFactorClipped against the reference, absolute
		double worstIdentity = 0.0;    // clipped(N) - clipped(-N) against the vector form factor along N
		double worstExact = 0.0;       // The table where the cap is above the horizon, absolute
		// The table where the face is above the horizon but its cap is not: absolute, over the vector form factor's length
		// (the light's largest form factor) and over the exact value. Over the length only from a least length up: the
		// float vector form factor the term starts from has an error of its own, which grows against the length as the
		// edges' terms cancel (mostly the edge fit's 1.5e-6 of each term, then the floats' rounding), and the ratio is the
		// table's only where that error fits in the bound's margin over the worst the draws above reach. Over those draws
		// it is at most 0.0023 of the length from 1e-4 up, 0.026 from 1e-5, 0.07 from 1e-6, 0.22 from 1e-7 and 1.8 under
		// that. Hence c_AboveLeastLength, 1e-4 (the margin is 0.0357 - 0.0243), and c_CutLeastLength, 1e-5 (0.312 - 0.266).
		// The few lights left out (the messages count them) give too little for a bound on the table to be told from it.
		double worstAbove = 0.0;
		double worstAboveLength = 0.0;
		double worstAboveRelative = 0.0;
		int aboveShortCount = 0;
		// The table where the horizon cuts the face: the same three, the last where the exact value is at least a hundredth
		// of the length.
		double worstCut = 0.0;
		double worstCutLength = 0.0;
		double worstCutRelative = 0.0;
		int cutShortCount = 0;
		double worstOverLength = -1.0; // The reference minus the vector form factor's length (negative: under it)
		double worstUnderSum = -1.0;   // The signed sum minus the table's value (negative: the table is over it)
		constexpr double c_AboveLeastLength = 1e-4;
		constexpr double c_CutLeastLength = 1e-5;
		for (int index = 0; index < 80000; index++)
		{
			AreaLights::AreaLight light;
			if (index % 4 == 0)
			{
				// A wall panel whose lower edge is 1 to 10 cm above the receiver's plane.
				const double distance = sequence.Next(0.3, 4.0);
				const double middle = sequence.Next(-1.5, 1.5);
				const double width = sequence.Next(0.3, 4.0);
				const double lower = sequence.Next(0.01, 0.1);
				light = MakeStandingLight(frame, middle - width * 0.5, middle + width * 0.5, lower, lower + sequence.Next(0.2, 2.5), distance);
			}
			else
			{
				light = MakeRandomLight(frame, sequence, glm::dvec3(-4.0, -4.0, -1.0), glm::dvec3(4.0, 4.0, 4.0), 0.05, 2.0, index % 8 == 1);
			}
			const std::string description = Describe(light);
			CAPTURE(index);
			CAPTURE(description);

			const float clipless = AreaLights::FormFactor(position, normal, light);
			const float clipped = AreaLights::FormFactorClipped(position, normal, light);
			if (!IsLit(position, light))
			{
				CHECK(clipless == 0.0f);
				CHECK(clipped == 0.0f);
				continue;
			}

			const double side = GetReferenceSide(position, light);
			const double reference = GetReferenceFormFactor(position, exactNormal, light);
			const glm::dvec3 vector = GetReferenceVectorFormFactor(position, light) * side;
			const double length = glm::length(vector);
			const double alongNormal = glm::dot(vector, exactNormal);
			const double cosine = alongNormal / length;
			worstClipped = std::max(worstClipped, std::abs(clipped - reference));

			// What is above the horizon counts for the receiver and what is under it for a receiver turned over: their
			// difference is the whole face's signed sum, the vector form factor along the normal. So the clip never takes a
			// form factor under the unclipped sum, and neither the clip nor the table gives more than the unclipped face can
			// give any receiver, the vector's length.
			const double under = AreaLights::FormFactorClipped(position, -normal, light);
			worstIdentity = std::max(worstIdentity, std::abs(clipped - under - alongNormal));
			CHECK(reference >= alongNormal - 1e-12);
			CHECK(reference <= length + 1e-12);
			CHECK(clipped >= alongNormal - 1e-5);
			CHECK(clipped <= length + 1e-5);
			worstOverLength = std::max(worstOverLength, reference - length);
			CHECK(clipless >= 0.0f);
			CHECK(clipless >= alongNormal - 1e-5);
			CHECK(clipless <= length + 1e-5);
			worstUnderSum = std::max(worstUnderSum, alongNormal - clipless);

			const std::array<glm::dvec3, 4> corners = GetCorners(position, light);
			const bool above = std::all_of(corners.begin(), corners.end(), [&](const glm::dvec3& corner) { return glm::dot(corner, exactNormal) > 0.0; });
			// The term is the cosine itself where the cap of the same vector form factor is above the horizon, cosine >=
			// sqrt(length); the four texels a sample weighs are all there when the one of the least cosine and the greatest
			// length is.
			const double texelCosine = std::floor((cosine * 0.5 + 0.5) * 63.0) / 63.0 * 2.0 - 1.0;
			const double texelLength = std::min(std::floor(length * 63.0) + 1.0, 63.0) / 63.0;
			if (above && texelCosine >= std::sqrt(texelLength))
			{
				exactCount++;
				worstExact = std::max(worstExact, std::abs(clipless - reference));
				CHECK(std::abs(clipless - reference) <= 1e-5);
			}
			else if (above)
			{
				aboveCount++;
				worstAbove = std::max(worstAbove, std::abs(clipless - reference));
				if (length >= c_AboveLeastLength)
					worstAboveLength = std::max(worstAboveLength, std::abs(clipless - reference) / length);
				else
					aboveShortCount++;
				worstAboveRelative = std::max(worstAboveRelative, std::abs(clipless - reference) / reference);
			}
			else
			{
				cutCount++;
				worstCut = std::max(worstCut, std::abs(clipless - reference));
				if (length >= c_CutLeastLength)
					worstCutLength = std::max(worstCutLength, std::abs(clipless - reference) / length);
				else
					cutShortCount++;
				if (reference >= 0.01 * length)
					worstCutRelative = std::max(worstCutRelative, std::abs(clipless - reference) / reference);
			}
		}
		MESSAGE("horizon: cap above " << exactCount << " worst " << worstExact);
		MESSAGE("horizon: face above " << aboveCount << " worst " << worstAbove << ", over length " << worstAboveLength << " (" << aboveShortCount
			<< " shorter than " << c_AboveLeastLength << " left out), relative " << worstAboveRelative);
		MESSAGE("horizon: face cut " << cutCount << " worst " << worstCut << ", over length " << worstCutLength << " (" << cutShortCount << " shorter than "
			<< c_CutLeastLength << " left out), relative " << worstCutRelative);
		MESSAGE("horizon: clipped worst absolute " << worstClipped << ", identity " << worstIdentity << ", reference minus length " << worstOverLength
			<< ", signed sum minus table " << worstUnderSum);
		CHECK(exactCount >= 8000);
		CHECK(aboveCount >= 8000);
		CHECK(cutCount >= 8000);
		CHECK(worstClipped <= 1e-5);
		CHECK(worstIdentity <= 1e-5);
		// The table stands for a sphere of the same vector form factor, which the horizon cuts otherwise than it cuts the
		// face. With the whole face above the horizon the draw measures 0.0238 of the light's largest form factor (the
		// plan's 2% does not hold: a wide wall panel whose lower edge is a centimeter over the floor) and 0.0059 in absolute
		// terms; relative to the exact value it reaches 18% for low panels that give little. The bounds are 1.5 times that.
		CHECK(worstAboveLength <= 0.0357);
		CHECK(worstAbove <= 0.0088);
		// A face the horizon cuts is further from its sphere, most where it is seen as a strip standing across the horizon
		// (the next test): the strip's upper half gives up to 1 / pi of its length, the sphere's upper half next to nothing.
		// The bounds are 1.5 times the measured 0.208 of the length and 0.0559.
		CHECK(worstCutLength <= 0.312);
		CHECK(worstCut <= 0.0839);
	}

	TEST_CASE("A thin strip standing across the horizon gets 1 / pi of its vector form factor's length, which the table misses")
	{
		// A strip of the width w at the distance d, standing across the receiver's horizon, endlessly tall: its vector form
		// factor lies along the horizon and is w / (2 d) long, and its upper half gives the receiver w / (2 pi d), 1 / pi of
		// that length. The table's term stands for a sphere of the same vector form factor, a small cap centered on the
		// horizon, which gives next to nothing above it: the horizon term's largest miss the tests know of.
		const Frame frame = MakeFrame();
		const glm::vec3 position = frame.GetPoint(glm::dvec3(0.0));
		const glm::vec3 normal = frame.GetDirection(glm::dvec3(0.0, 0.0, 1.0));
		const AreaLights::AreaLight strip = MakeStandingLight(frame, -0.005, 0.005, -1000.0, 1000.0, 1.0);
		const double length = glm::length(GetReferenceVectorFormFactor(position, strip));
		const double upper = 0.01 / (2.0 * c_Pi);
		CHECK(GetRelativeError(length, 0.005) <= 1e-4);
		CHECK(GetRelativeError(GetReferenceFormFactor(position, glm::dvec3(normal), strip), upper) <= 1e-4);
		// The float clip, in absolute terms as over the random lights: its corners 1 km away are rounded to 6e-5 m.
		CHECK(std::abs(AreaLights::FormFactorClipped(position, normal, strip) - upper) <= 1e-5);
		const double table = AreaLights::FormFactor(position, normal, strip);
		MESSAGE("horizon: a strip 1 cm wide across the horizon, 1 m away: exact " << upper / length << " of the length, the table " << table / length);
		// 1.5 times the measured 0.0142.
		CHECK(table <= 0.0213 * length);
	}

	TEST_CASE("The panel just above a floor gets the horizon term's measured error, not more")
	{
		// A 2.4 x 0.95 m wall panel whose lower edge is 5 cm above the floor, seen from the floor 1 m in front of it: the
		// whole panel is above the horizon, the cap of its vector form factor is not.
		const Frame frame = MakeFrame();
		const glm::vec3 position = frame.GetPoint(glm::dvec3(0.0));
		const glm::vec3 normal = frame.GetDirection(glm::dvec3(0.0, 0.0, 1.0));
		const AreaLights::AreaLight panel = MakeStandingLight(frame, -1.2, 1.2, 0.05, 1.0, 1.0);
		const double exact = GetPerpendicularFactor(-1.2, 1.2, 0.05, 1.0, 1.0);
		const glm::dvec3 vector = GetReferenceVectorFormFactor(position, panel);
		CHECK(glm::length(vector) == doctest::Approx(0.301).epsilon(0.005));
		CHECK(glm::dot(vector, glm::dvec3(normal)) / glm::length(vector) == doctest::Approx(0.399).epsilon(0.005));
		CHECK(GetRelativeError(AreaLights::FormFactorClipped(position, normal, panel), exact) <= 1e-4);
		const double error = (AreaLights::FormFactor(position, normal, panel) - exact) / exact;
		MESSAGE("horizon: the panel 5 cm above the floor, table over exact " << error);
		CHECK(error > 0.0);
		// 1.5 times the measured 1.81%.
		CHECK(error <= 0.027);

		// The panel above the horizon for which a search over standing panels (widths of 1 m to 10 km, lower edges from
		// 1 micrometer, heights of 2 cm to 20 m, seen from 1 m) found the table furthest off: 10 km wide, 0.24 m tall, its
		// lower edge 1 micrometer above the floor.
		const AreaLights::AreaLight widest = MakeStandingLight(frame, -2000.0, 8000.0, 1e-6, 0.24, 1.0);
		const double widestExact = GetReferenceFormFactor(position, glm::dvec3(normal), widest);
		const double widestLength = glm::length(GetReferenceVectorFormFactor(position, widest));
		CHECK(std::abs(AreaLights::FormFactorClipped(position, normal, widest) - widestExact) <= 1e-5);
		const double widestError = (AreaLights::FormFactor(position, normal, widest) - widestExact) / widestLength;
		MESSAGE("horizon: a panel 10 km wide on the floor, table over exact " << widestError << " of the vector form factor's length " << widestLength);
		CHECK(widestError > 0.0);
		// 1.5 times the measured 2.9%.
		CHECK(widestError <= 0.044);
	}
}

namespace
{

	// A 1 x 1 m light at the origin along the world's axes, shining up: positions' offsets from it are exact, so "on its
	// plane" and "next to its edge" are what the code is given and only its own arithmetic is tested.
	AreaLights::AreaLight MakeAxisLight(bool twoSided = false)
	{
		AreaLights::AreaLight light;
		light.Center = glm::vec3(0.0f);
		light.Normal = glm::vec3(0.0f, 0.0f, 1.0f);
		light.AxisX = glm::vec3(0.5f, 0.0f, 0.0f);
		light.AxisY = glm::cross(light.AxisX, light.Normal);
		light.Radiance = glm::vec3(1.0f);
		light.TwoSided = twoSided;
		return light;
	}

	// The share of the fitted lobe for `roughness` and `viewCosine` that lies above the surface's horizon, from the
	// lobe's transform alone: what a light filling the sky covers. The lobe's share of a set of directions is the clamped
	// cosine's share of their images under the texel's inverse transform M^-1 (rows (x, 0, z), (0, 1, 0), (y, 0, w)). The
	// images of the directions above the surface are the directions M takes above it: the hemisphere around M's third row,
	// (-y, 0, x) / (x w - y z). A clamped cosine fills (1 + cos a) / 2 of a hemisphere whose pole is a away from its own (a
	// plane tilted by a sees that share of the sky), and the tables' determinants are positive (the first test checks
	// them), so the share is (1 + x / sqrt(x^2 + y^2)) / 2.
	double GetSkyShare(float roughness, double viewCosine)
	{
		const glm::vec2 uv(roughness, std::sqrt(std::max(1.0f - static_cast<float>(viewCosine), 0.0f)));
		const glm::dvec4 transform(AreaLights::SampleTable(AreaLights::c_TransformLayer, uv));
		return 0.5 * (1.0 + transform.x / std::sqrt(transform.x * transform.x + transform.y * transform.y));
	}

	// A light of 2000 x 2000 m 1 m over a receiver: it leaves out the directions within 1e-3 rad of the horizon, a millionth
	// of the cosine. So the lobe's share of it is GetSkyShare's within the 1e-3 the tests hold it to (the white furnace's
	// views, down to a view cosine of 0.05, measure 6.3e-5 at most).
	AreaLights::AreaLight MakeSkyLight(const Frame& frame)
	{
		return MakeParallelLight(frame, -1000.0, 1000.0, -1000.0, 1000.0, 1.0);
	}

	constexpr double c_SkyShareTolerance = 1e-3;

}

TEST_SUITE("Renderer.AreaLights")
{
	TEST_CASE("A point next to a face's edge gets the edge's exact share")
	{
		// A surface flush with a light sees the nearest edge under almost a straight angle, where a sine taken from the
		// cosine is all rounding.
		const AreaLights::AreaLight light = MakeAxisLight();
		double worstVector = 0.0;
		float worstFlush = 0.0f;
		for (const float distance : { 0.0001f, 0.00025f, 0.0005f, 0.00075f, 0.001f, 0.0025f })
		{
			for (const float along : { -0.4f, -0.15f, 0.0f, 0.22f, 0.45f })
			{
				for (const float lift : { 1e-6f, 1e-5f, 1e-4f })
				{
					for (int edge = 0; edge < 4; edge++)
					{
						const glm::vec3 outward = edge % 2 == 0 ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
						const glm::vec3 across = edge % 2 == 0 ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
						const glm::vec3 position = outward * ((0.5f + distance) * (edge < 2 ? 1.0f : -1.0f)) + across * along + light.Normal * lift;
						const glm::dvec3 reference = GetReferenceVectorFormFactor(position, light);
						worstVector = std::max(worstVector, glm::length(glm::dvec3(AreaLights::GetVectorFormFactor(position, light)) - reference));
						// The face is in the flush surface's horizon: it gives that surface nothing. The table gives the little
						// the horizon leaves of a sphere of the same vector form factor.
						worstFlush = std::max(worstFlush, AreaLights::FormFactor(position, light.Normal, light));
						CHECK(AreaLights::FormFactorClipped(position, light.Normal, light) == 0.0f);
					}
				}
			}
		}
		MESSAGE("flush edge: worst vector error " << worstVector << ", worst flush form factor " << worstFlush);
		CHECK(worstVector <= 1e-5);
		// 1.5 times the measured 0.0074, the sphere's share at the grid's steepest look down onto the face.
		CHECK(worstFlush <= 0.0111f);
	}

	TEST_CASE("A surface in the plane of a turned light gets nothing from it, however rounding places the corners")
	{
		// The flush surface again, next to a light that is not along the world's axes: the heights of the face's corners
		// over the surface are rounding, each on either side of zero, so the outline can cross the horizon four times where
		// a plane cuts a real rectangle twice at most. Each stretch under the horizon has to be closed by its own edge
		// along the horizon: an edge from one stretch's exit to another's entry adds the angle between two unrelated
		// crossings, a form factor of 0.05 to 0.4 where the face gives nothing.
		const Frame frame = MakeFrame();
		Sequence sequence(7);
		int fourCrossings = 0;
		double worstReference = 0.0;
		double worstError = 0.0;
		double worstShare = 0.0;
		for (const bool twoSided : { false, true })
		{
			// In the frame's plane z = 0, from x 0.9 to 2.1 and y 0 to 0.6.
			const glm::dvec3 lightCenter(1.5, 0.3, 0.0);
			const glm::dvec2 lightHalfSize(0.6, 0.3);
			const AreaLights::AreaLight light = MakeLight(frame, lightCenter, glm::dvec3(0.0, 0.0, 1.0), glm::dvec3(1.0, 0.0, 0.0), lightHalfSize.x, lightHalfSize.y,
				twoSided);
			const glm::dvec3 exactNormal = glm::dvec3(light.Normal);
			for (int index = 0; index < 12000; index++)
			{
				// Beside one of the four edges, 0.1 mm to 0.5 m out (as many in every decade), anywhere along it and up to a
				// fifth of its length past its ends; on the plane and a tenth of a micrometer to either side of it, which the
				// positions' own rounding exceeds.
				const int edge = index % 4;
				const double out = 0.0001 * std::pow(5000.0, sequence.Next());
				const double along = sequence.Next(-1.2, 1.2);
				const double lift = (index / 4) % 3 == 0 ? 0.0 : ((index / 4) % 3 == 1 ? 1e-7 : -1e-7);
				const double outward = edge < 2 ? 1.0 : -1.0;
				const glm::dvec2 inPlane = edge % 2 == 0 ? glm::dvec2((lightHalfSize.x + out) * outward, lightHalfSize.y * along)
					: glm::dvec2(lightHalfSize.x * along, (lightHalfSize.y + out) * outward);
				const glm::vec3 position = frame.GetPoint(lightCenter + glm::dvec3(inPlane, lift));
				CAPTURE(twoSided);
				CAPTURE(index);

				// How often the case is met: the corners' heights as the code computes them, in its order.
				const glm::vec3 center = light.Center - position;
				const glm::vec3 corners[4] = { center - light.AxisX - light.AxisY, center + light.AxisX - light.AxisY, center + light.AxisX + light.AxisY,
					center - light.AxisX + light.AxisY };
				int crossings = 0;
				for (int corner = 0; corner < 4; corner++)
					crossings += (glm::dot(corners[corner], light.Normal) > 0.0f) != (glm::dot(corners[(corner + 1) % 4], light.Normal) > 0.0f) ? 1 : 0;
				fourCrossings += crossings == 4 ? 1 : 0;

				// The face gives the surface nothing, or the sliver the positions' rounding lifts over its horizon: the
				// reference clips the face the code is given, in double. The code's own corners are differences of floats,
				// each off by up to half a unit in the last place (1.2e-7 m here), so a corner's direction is known to
				// 1.2e-7 radians over its distance, and an angle over 2 pi is what an edge adds: 1.9e-8 over the distance for
				// each of the two edges that meet there, more than the 1e-5 the form factor keeps elsewhere once a corner is
				// within 4 mm.
				const glm::dvec2 fromCorner = glm::abs(inPlane) - lightHalfSize;
				const double cornerDistance = glm::length(fromCorner);
				const double tolerance = 1e-5 + 4e-8 / cornerDistance;
				const float clipped = AreaLights::FormFactorClipped(position, light.Normal, light);
				const double reference = GetReferenceFormFactor(position, exactNormal, light);
				const double error = std::abs(clipped - reference);
				worstReference = std::max(worstReference, reference);
				worstError = std::max(worstError, error);
				worstShare = std::max(worstShare, error / tolerance);
				CHECK(error <= tolerance);
			}
		}
		MESSAGE("flush with a turned light: the sliver gives at most " << worstReference << ", worst error " << worstError << ", at most " << worstShare
			<< " of what the corners' rounding allows; outlines crossing four times " << fourCrossings);
		// The sliver: a face h over a horizon and d away fills (h / d)^2 / 4 of it at most, with h under 0.6 micrometers
		// (the lift and the rounding of the receiver's and the light's positions) and d from 0.1 mm.
		CHECK(worstReference <= 1e-5);
		CHECK(fourCrossings >= 20);
	}

	TEST_CASE("A one-sided light lights its front only, a two-sided light both sides alike, and neither its own plane")
	{
		const Frame frame = MakeFrame();
		const glm::dvec3 center(0.4, -0.3, 1.5);
		const glm::dvec3 lightNormal = glm::normalize(glm::dvec3(0.2, 0.1, -1.0));
		const glm::dvec3 axisX = glm::normalize(glm::cross(lightNormal, glm::dvec3(0.0, 1.0, 0.0)));
		const AreaLights::AreaLight oneSided = MakeLight(frame, center, lightNormal, axisX, 0.6, 0.4);
		const AreaLights::AreaLight twoSided = MakeLight(frame, center, lightNormal, axisX, 0.6, 0.4, true);

		// A receiver in front, and its image in the light's plane: behind the light, seeing the same face.
		const auto mirror = [&](const glm::dvec3& direction) { return direction - lightNormal * (2.0 * glm::dot(direction, lightNormal)); };
		const glm::dvec3 front(0.1, 0.2, 0.3);
		const glm::dvec3 frontNormal = glm::normalize(glm::dvec3(0.3, -0.2, 1.0));
		const glm::dvec3 frontView = glm::normalize(glm::dvec3(-0.6, 0.3, 0.5));
		const glm::vec3 frontPoint = frame.GetPoint(front);
		const glm::vec3 backPoint = frame.GetPoint(center + mirror(front - center));
		const glm::vec3 n = frame.GetDirection(frontNormal);
		const glm::vec3 v = frame.GetDirection(frontView);
		const glm::vec3 backN = frame.GetDirection(mirror(frontNormal));
		const glm::vec3 backV = frame.GetDirection(mirror(frontView));

		const float lit = AreaLights::FormFactor(frontPoint, n, oneSided);
		const float litClipped = AreaLights::FormFactorClipped(frontPoint, n, oneSided);
		const float litSpecular = AreaLights::SpecularFraction(frontPoint, n, v, 0.4f, oneSided);
		CHECK(lit > 0.05f);
		CHECK(litClipped > 0.05f);
		CHECK(litSpecular > 0.01f);
		CHECK(AreaLights::FormFactor(backPoint, backN, oneSided) == 0.0f);
		CHECK(AreaLights::FormFactorClipped(backPoint, backN, oneSided) == 0.0f);
		CHECK(AreaLights::SpecularFraction(backPoint, backN, backV, 0.4f, oneSided) == 0.0f);

		CHECK(AreaLights::FormFactor(frontPoint, n, twoSided) == lit);
		CHECK(std::abs(AreaLights::FormFactor(backPoint, backN, twoSided) - lit) <= 1e-5f);
		CHECK(std::abs(AreaLights::FormFactorClipped(backPoint, backN, twoSided) - litClipped) <= 1e-5f);
		CHECK(std::abs(AreaLights::SpecularFraction(backPoint, backN, backV, 0.4f, twoSided) - litSpecular) <= 1e-4f);

		// On the light's plane, inside the face and beside it, nothing.
		for (const bool both : { false, true })
		{
			const AreaLights::AreaLight flat = MakeAxisLight(both);
			for (const glm::vec3& point : { glm::vec3(0.2f, 0.1f, 0.0f), glm::vec3(0.5f, 0.0f, 0.0f), glm::vec3(0.9f, -0.3f, 0.0f), glm::vec3(0.5f, -0.5f, 0.0f) })
			{
				CHECK(AreaLights::FormFactor(point, glm::vec3(1.0f, 0.0f, 0.0f), flat) == 0.0f);
				CHECK(AreaLights::FormFactorClipped(point, glm::vec3(1.0f, 0.0f, 0.0f), flat) == 0.0f);
				CHECK(AreaLights::SpecularFraction(point, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.6f, 0.0f, 0.8f), 0.5f, flat) == 0.0f);
			}
		}
	}

	TEST_CASE("The identity transform gives the form factor, and a fully rough lobe tends to the diffuse result")
	{
		const Frame frame = MakeFrame();
		const glm::vec3 position = frame.GetPoint(glm::dvec3(0.0));
		const glm::vec3 normal = frame.GetDirection(glm::dvec3(0.0, 0.0, 1.0));
		const glm::vec3 view = frame.GetDirection(glm::normalize(glm::dvec3(0.5, 0.2, 0.7)));

		const AreaLights::AreaLight panel = MakeParallelLight(frame, -0.3, 1.1, -0.6, 0.4, 0.8);
		const float formFactor = AreaLights::FormFactor(position, normal, panel);
		CHECK(std::abs(AreaLights::GetLobeCoverage(glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), position, normal, view, panel) - formFactor) <= 1e-5f);

		// A light that fills the hemisphere fills the lobes that lie within it: the diffuse term and the fully rough lobe
		// both receive all of it, and so does a sharp lobe around a direction well above the horizon.
		const AreaLights::AreaLight sky = MakeSkyLight(frame);
		CHECK(std::abs(AreaLights::FormFactor(position, normal, sky) - 1.0f) <= 2e-3f);
		CHECK(std::abs(AreaLights::FormFactorClipped(position, normal, sky) - 1.0f) <= 2e-3f);
		const float roughSky = AreaLights::SpecularFraction(position, normal, view, 1.0f, sky);
		const float sharpSky = AreaLights::SpecularFraction(position, normal, view, 0.1f, sky);
		// Between the two the fitted lobe is a tilted cosine that reaches under the surface, and the horizon takes that
		// part: 2.2% of the lobe at roughness 0.5 for this view, 38 degrees off the normal.
		const float middleSky = AreaLights::SpecularFraction(position, normal, view, 0.5f, sky);
		MESSAGE("sky: the lobe's share at roughness 1 " << roughSky << ", 0.5 " << middleSky << ", 0.1 " << sharpSky);
		CHECK(std::abs(roughSky - 1.0f) <= 1e-3f);
		CHECK(std::abs(sharpSky - 1.0f) <= 1e-3f);
		CHECK(middleSky <= 1.0f);
		CHECK(std::abs(middleSky - GetSkyShare(0.5f, glm::dot(normal, view))) <= c_SkyShareTolerance);

		// A small light at the zenith seen along the normal: the fitted lobe at roughness 1 is the cosine widened by its
		// last term w, whose density at the zenith is 1 / (pi w^2), against the cosine's 1 / pi. GGX itself gives
		// D Vis / albedo = (1 / pi) (1 / 4) / (1 - ln 2) there: the fit keeps within 5% of it, and both within 25% of the
		// diffuse term, which a mirror-like lobe exceeds by orders of magnitude.
		const AreaLights::AreaLight zenith = MakeParallelLight(frame, -0.01, 0.01, -0.01, 0.01, 1.0);
		const glm::vec3 up = normal;
		const double diffuse = AreaLights::FormFactor(position, normal, zenith);
		const double rough = AreaLights::SpecularFraction(position, normal, up, 1.0f, zenith);
		CHECK(GetRelativeError(rough / diffuse, 1.0 / (1.12813 * 1.12813)) <= 1e-3);
		CHECK(GetRelativeError(rough / diffuse, 1.0 / (4.0 * (1.0 - std::log(2.0)))) <= 0.05);
		CHECK(AreaLights::SpecularFraction(position, normal, up, 0.1f, zenith) > 100.0 * diffuse);
	}
}

namespace
{

	// The frame AreaLight states: unit axes at right angles, AxisY along cross(AxisX, Normal).
	void CheckFrame(const AreaLights::Record& record)
	{
		CHECK(glm::length(record.Normal) == doctest::Approx(1.0).epsilon(1e-5));
		CHECK(glm::length(record.AxisX) == doctest::Approx(1.0).epsilon(1e-5));
		CHECK(glm::length(record.AxisY) == doctest::Approx(1.0).epsilon(1e-5));
		CHECK(std::abs(glm::dot(record.Normal, record.AxisX)) <= 1e-5f);
		CHECK(std::abs(glm::dot(record.Normal, record.AxisY)) <= 1e-5f);
		CHECK(std::abs(glm::dot(record.AxisX, record.AxisY)) <= 1e-5f);
		// The corners' order the edge sums count on.
		CHECK(glm::length(record.AxisY - glm::cross(record.AxisX, record.Normal)) <= 1e-5f);
	}

	bool IsNotDrawn(const AreaLights::Record& record)
	{
		return !record.Drawn && record.Radiance == glm::vec3(0.0f) && record.HalfExtents == glm::vec2(0.0f) && record.Range == 0.0f &&
			record.BoundsRadius == 0.0f;
	}

	// The value a shader source gives the float constant `name`, which it declares once.
	std::optional<double> FindShaderConstant(const std::string& source, const std::string& name)
	{
		const std::string declaration = "static const float " + name + " = ";
		const size_t start = source.find(declaration);
		if (start == std::string::npos || source.find(declaration, start + 1) != std::string::npos)
			return std::nullopt;
		const char* digits = source.c_str() + start + declaration.size();
		char* end = nullptr;
		const double value = std::strtod(digits, &end);
		if (end == digits)
			return std::nullopt;
		return value;
	}

}

TEST_SUITE("Renderer.AreaLights")
{
	TEST_CASE("Lumens spread over the face's area and its sides, nits are the luminance itself")
	{
		// A Lambertian face of area A and luminance L emits pi L A lumens: 1000 lm from 1 m^2 are 1000 / pi nits.
		const glm::vec2 squareMeter(0.5f, 0.5f);
		CHECK(AreaLights::GetLuminance(1000.0f, AreaLightUnits::Lumens, squareMeter, false) == doctest::Approx(318.31).epsilon(1e-5));
		CHECK(AreaLights::GetLuminance(1000.0f, AreaLightUnits::Lumens, squareMeter, true) == doctest::Approx(159.155).epsilon(1e-5));

		// The area is the face's in the world: half sizes of 0.5 x 0.25 under a scale of 2 x 3 are 2 x 1.5 m.
		const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(4.0f, -2.0f, 1.0f)) *
			glm::rotate(glm::mat4(1.0f), 0.7f, glm::normalize(glm::vec3(0.2f, 1.0f, -0.4f))) * glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 3.0f, 5.0f));
		const glm::vec2 halfExtents = AreaLights::GetWorldHalfExtents(world, glm::vec2(0.5f, 0.25f));
		CHECK(halfExtents.x == doctest::Approx(1.0).epsilon(1e-5));
		CHECK(halfExtents.y == doctest::Approx(0.75).epsilon(1e-5));
		CHECK(AreaLights::GetLuminance(1000.0f, AreaLightUnits::Lumens, halfExtents, false) == doctest::Approx(1000.0 / (c_Pi * 3.0)).epsilon(1e-5));

		for (const glm::vec2& size : { squareMeter, halfExtents, glm::vec2(0.0f), glm::vec2(100.0f, 0.001f) })
		{
			CHECK(AreaLights::GetLuminance(250.0f, AreaLightUnits::Nits, size, false) == 250.0f);
			CHECK(AreaLights::GetLuminance(250.0f, AreaLightUnits::Nits, size, true) == 250.0f);
		}
		// Power from no area would be an infinite luminance: the light is dark instead.
		CHECK(AreaLights::GetLuminance(1000.0f, AreaLightUnits::Lumens, glm::vec2(0.0f, 0.5f), false) == 0.0f);
		CHECK(AreaLights::GetLuminance(1000.0f, AreaLightUnits::Lumens, glm::vec2(0.0f), true) == 0.0f);
	}

	TEST_CASE("A record follows the light's transform: rotated, scaled and under a mirroring parent")
	{
		const glm::vec3 translation(4.0f, -2.0f, 1.0f);
		const glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), 0.7f, glm::normalize(glm::vec3(0.2f, 1.0f, -0.4f)));
		const glm::mat4 local = glm::translate(glm::mat4(1.0f), translation) * rotation * glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 3.0f, 5.0f));
		const glm::vec3 color(1.0f, 0.5f, 0.25f);

		const AreaLights::Record record = AreaLights::MakeRecord(local, true, color, 200.0f, glm::vec2(0.5f, 0.25f), 8.0f, true);
		REQUIRE(record.Drawn);
		CheckFrame(record);
		CHECK(glm::length(record.Center - translation) <= 1e-6f);
		CHECK(glm::length(record.Normal + glm::vec3(rotation[2])) <= 1e-5f);
		CHECK(glm::length(record.AxisX - glm::vec3(rotation[0])) <= 1e-5f);
		CHECK(glm::length(record.AxisY - glm::vec3(rotation[1])) <= 1e-5f);
		CHECK(glm::length(record.HalfExtents - glm::vec2(1.0f, 0.75f)) <= 1e-5f);
		CHECK(glm::length(record.Radiance - glm::vec3(200.0f, 100.0f, 50.0f)) <= 1e-4f);
		CHECK(record.Range == 8.0f);
		CHECK(record.BoundsRadius == doctest::Approx(8.0 + 1.25).epsilon(1e-6));
		CHECK(record.TwoSided);

		const AreaLights::AreaLight light = AreaLights::LoadAreaLight(record);
		CHECK(light.Center == record.Center);
		CHECK(light.Normal == record.Normal);
		CHECK(glm::length(light.AxisX - glm::vec3(rotation[0])) <= 1e-5f);
		CHECK(glm::length(light.AxisY - glm::vec3(rotation[1]) * 0.75f) <= 1e-5f);
		CHECK(light.Radiance == record.Radiance);
		CHECK(light.InverseRangeSquared == doctest::Approx(1.0 / 64.0).epsilon(1e-6));
		CHECK(light.TwoSided);

		// Under a parent that mirrors, the light still shines along its transformed -Z, and its corners still run clockwise
		// seen from there: the transform's Y axis points the other way around the normal, so AxisY is not that axis.
		const glm::mat4 mirrored = glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f)) * local;
		const AreaLights::Record mirroredRecord = AreaLights::MakeRecord(mirrored, true, color, 200.0f, glm::vec2(0.5f, 0.25f), 8.0f, false);
		REQUIRE(mirroredRecord.Drawn);
		CheckFrame(mirroredRecord);
		const glm::vec3 shineDirection = -glm::normalize(glm::vec3(mirrored[2]));
		CHECK(glm::length(mirroredRecord.Normal - shineDirection) <= 1e-5f);
		CHECK(glm::length(mirroredRecord.AxisX - glm::normalize(glm::vec3(mirrored[0]))) <= 1e-5f);
		CHECK(glm::length(mirroredRecord.AxisY + glm::normalize(glm::vec3(mirrored[1]))) <= 1e-5f);
		CHECK(glm::length(mirroredRecord.HalfExtents - glm::vec2(1.0f, 0.75f)) <= 1e-5f);
		CHECK(!mirroredRecord.TwoSided);

		// It lights what is in front of it as the same face under a transform that does not mirror (the Y axis turned
		// back), by Howell's factor for a 2 x 1.5 m face seen from 1 m in front of its center, and nothing behind it.
		glm::mat4 twin = mirrored;
		twin[1] = -twin[1];
		const AreaLights::Record twinRecord = AreaLights::MakeRecord(twin, true, color, 200.0f, glm::vec2(0.5f, 0.25f), 8.0f, false);
		REQUIRE(twinRecord.Drawn);
		const AreaLights::AreaLight mirroredLight = AreaLights::LoadAreaLight(mirroredRecord);
		const AreaLights::AreaLight twinLight = AreaLights::LoadAreaLight(twinRecord);
		const glm::vec3 center = glm::vec3(mirrored[3]);
		const glm::vec3 front = center + shineDirection;
		const glm::vec3 back = center - shineDirection;
		const float lit = AreaLights::FormFactor(front, -shineDirection, mirroredLight);
		CHECK(GetRelativeError(lit, GetParallelFactor(-1.0, 1.0, -0.75, 0.75, 1.0)) <= 1e-4);
		CHECK(std::abs(lit - AreaLights::FormFactor(front, -shineDirection, twinLight)) <= 1e-4f);
		CHECK(std::abs(AreaLights::FormFactorClipped(front, -shineDirection, mirroredLight) - AreaLights::FormFactorClipped(front, -shineDirection, twinLight)) <= 1e-4f);
		const glm::vec3 view = glm::normalize(-shineDirection + mirroredRecord.AxisX * 0.5f);
		CHECK(AreaLights::SpecularFraction(front, -shineDirection, view, 0.3f, mirroredLight) > 0.1f);
		CHECK(std::abs(AreaLights::SpecularFraction(front, -shineDirection, view, 0.3f, mirroredLight) -
			AreaLights::SpecularFraction(front, -shineDirection, view, 0.3f, twinLight)) <= 1e-4f);
		CHECK(AreaLights::FormFactor(back, shineDirection, mirroredLight) == 0.0f);
		CHECK(AreaLights::FormFactorClipped(back, shineDirection, mirroredLight) == 0.0f);
		CHECK(AreaLights::SpecularFraction(back, shineDirection, shineDirection, 0.3f, mirroredLight) == 0.0f);
	}

	TEST_CASE("A sheared face becomes the rectangle with its sides' lengths, in the face's plane")
	{
		// A light turned under a parent with an uneven scale has axes that are not at right angles any more. Its record
		// keeps the face's plane, the X axis and both sides' lengths, and turns Y in the plane until it is perpendicular to X.
		const glm::vec3 color(1.0f, 0.5f, 0.25f);
		const glm::vec2 half(0.5f, 0.25f);
		const glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), 0.7f, glm::normalize(glm::vec3(0.2f, 1.0f, -0.4f)));
		const float turn = glm::radians(30.0f);
		// Sheared in the face's plane and out of it, in the plane only (Z stays the face's normal, X and Y lean toward each
		// other), and out of it only (X and Y stay at right angles, Z leans away from the normal).
		const glm::mat4 sheared[] = {
			glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 3.0f)) * rotation,
			glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 1.0f)) * glm::rotate(glm::mat4(1.0f), turn, glm::vec3(0.0f, 0.0f, 1.0f)),
			glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, 3.0f)) * glm::rotate(glm::mat4(1.0f), turn, glm::vec3(1.0f, 0.0f, 0.0f)),
		};
		for (size_t index = 0; index < std::size(sheared); index++)
		{
			CAPTURE(index);
			const glm::vec3 x = glm::vec3(sheared[index][0]);
			const glm::vec3 y = glm::vec3(sheared[index][1]);
			const glm::vec3 z = glm::vec3(sheared[index][2]);
			const AreaLights::Record record = AreaLights::MakeRecord(sheared[index], true, color, 200.0f, half, 8.0f, false);
			REQUIRE(record.Drawn);
			CheckFrame(record);
			// The face's plane is the transformed one, and the light shines to its transformed -Z's side.
			CHECK(std::abs(glm::dot(record.Normal, glm::normalize(x))) <= 1e-5f);
			CHECK(std::abs(glm::dot(record.Normal, glm::normalize(y))) <= 1e-5f);
			CHECK(glm::dot(record.Normal, z) < 0.0f);
			CHECK(glm::length(record.AxisX - glm::normalize(x)) <= 1e-5f);
			CHECK(glm::dot(record.AxisY, y) > 0.0f);
			CHECK(glm::length(record.HalfExtents - half * glm::vec2(glm::length(x), glm::length(y))) <= 1e-5f);
			CHECK(record.BoundsRadius == doctest::Approx(8.0 + glm::length(record.HalfExtents)).epsilon(1e-6));
		}

		// The rectangle is larger than the parallelogram the transform makes of the face, by 1 / sin(a) for sides that meet
		// at the angle a: here sqrt(3.25 * 1.75) / 2 = 1.1924 times its 1 m^2, and a light in nits emits that much more.
		const glm::mat4& inPlane = sheared[1];
		const AreaLights::Record record = AreaLights::MakeRecord(inPlane, true, color, 200.0f, half, 8.0f, false);
		REQUIRE(record.Drawn);
		const float parallelogram = 4.0f * half.x * half.y * glm::length(glm::cross(glm::vec3(inPlane[0]), glm::vec3(inPlane[1])));
		CHECK(parallelogram == doctest::Approx(1.0).epsilon(1e-5));
		CHECK(4.0f * record.HalfExtents.x * record.HalfExtents.y == doctest::Approx(1.19242).epsilon(1e-5));
		CHECK(glm::length(record.Radiance - glm::vec3(200.0f, 100.0f, 50.0f)) <= 1e-4f);
		// What it lights is that rectangle's share: Howell's factor for its half sizes, sqrt(3.25) / 2 and sqrt(1.75) / 4,
		// seen from 1 m in front of its center.
		const AreaLights::AreaLight light = AreaLights::LoadAreaLight(record);
		const double halfX = std::sqrt(3.25) * 0.5;
		const double halfY = std::sqrt(1.75) * 0.25;
		const float lit = AreaLights::FormFactor(record.Center + record.Normal, -record.Normal, light);
		CHECK(GetRelativeError(lit, GetParallelFactor(-halfX, halfX, -halfY, halfY, 1.0)) <= 1e-4);
	}

	TEST_CASE("A light without a face, light or a finite transform is not drawn")
	{
		const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f));
		const glm::vec3 white(1.0f);
		const glm::vec2 half(0.5f, 0.25f);
		const float infinity = std::numeric_limits<float>::infinity();
		const float nan = std::numeric_limits<float>::quiet_NaN();
		CHECK(AreaLights::MakeRecord(world, true, white, 100.0f, half, 5.0f, false).Drawn);
		CHECK(IsNotDrawn(AreaLights::Record()));

		CHECK(IsNotDrawn(AreaLights::MakeRecord(world, false, white, 100.0f, half, 5.0f, false)));
		for (const float luminance : { 0.0f, -1.0f, infinity, nan })
			CHECK(IsNotDrawn(AreaLights::MakeRecord(world, true, white, luminance, half, 5.0f, false)));
		for (const float range : { 0.0f, -2.0f, infinity, nan })
			CHECK(IsNotDrawn(AreaLights::MakeRecord(world, true, white, 100.0f, half, range, false)));
		for (const glm::vec2& size : { glm::vec2(0.0f, 0.25f), glm::vec2(0.5f, 0.0f), glm::vec2(0.0004f, 0.25f), glm::vec2(0.5f, 0.0004f), glm::vec2(-0.5f, 0.25f),
				 glm::vec2(nan, 0.25f), glm::vec2(0.5f, infinity) })
			CHECK(IsNotDrawn(AreaLights::MakeRecord(world, true, white, 100.0f, size, 5.0f, false)));
		// A face of 1.2 mm along either axis is drawn, and a scale takes one under the least size.
		CHECK(AreaLights::MakeRecord(world, true, white, 100.0f, glm::vec2(0.0006f, 0.25f), 5.0f, false).Drawn);
		CHECK(AreaLights::MakeRecord(world, true, white, 100.0f, glm::vec2(0.25f, 0.0006f), 5.0f, false).Drawn);
		CHECK(IsNotDrawn(AreaLights::MakeRecord(world * glm::scale(glm::mat4(1.0f), glm::vec3(0.5f, 1.0f, 1.0f)), true, white, 100.0f,
			glm::vec2(0.0006f, 0.25f), 5.0f, false)));
		CHECK(IsNotDrawn(AreaLights::MakeRecord(world * glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 0.5f, 1.0f)), true, white, 100.0f,
			glm::vec2(0.25f, 0.0006f), 5.0f, false)));
		// The least size is a size, twice the half size, and a face of exactly that size is drawn.
		CHECK(AreaLights::MakeRecord(world, true, white, 100.0f, glm::vec2(AreaLights::c_MinSize * 0.5f, 0.25f), 5.0f, false).Drawn);
		CHECK(AreaLights::MakeRecord(world, true, white, 100.0f, glm::vec2(0.25f, AreaLights::c_MinSize * 0.5f), 5.0f, false).Drawn);
		// Also turned, as an entity's transform turns it (a ceiling light: -90 degrees about X), although the axes of a
		// rotation's matrix are a float step or so off unit length; a face clearly under the least size still is not drawn.
		for (const glm::vec3& degrees : { glm::vec3(-90.0f, 0.0f, 0.0f), glm::vec3(90.0f, 0.0f, 0.0f), glm::vec3(0.0f, 90.0f, 0.0f), glm::vec3(0.0f, 0.0f, 90.0f),
				 glm::vec3(180.0f, 0.0f, 0.0f), glm::vec3(37.0f, -61.0f, 113.0f), glm::vec3(-45.0f, 135.0f, 10.0f) })
		{
			CAPTURE(degrees);
			const glm::mat4 turned = Math::ComposeTransform(glm::vec3(1.0f, 2.0f, 3.0f), Math::EulerDegreesToQuat(degrees), glm::vec3(1.0f));
			CHECK(AreaLights::MakeRecord(turned, true, white, 100.0f, glm::vec2(AreaLights::c_MinSize * 0.5f, 0.25f), 5.0f, false).Drawn);
			CHECK(AreaLights::MakeRecord(turned, true, white, 100.0f, glm::vec2(0.25f, AreaLights::c_MinSize * 0.5f), 5.0f, false).Drawn);
			CHECK(AreaLights::MakeRecord(turned, true, white, 100.0f, glm::vec2(AreaLights::c_MinSize * 0.5f), 5.0f, false).Drawn);
			CHECK(IsNotDrawn(AreaLights::MakeRecord(turned, true, white, 100.0f, glm::vec2(AreaLights::c_MinSize * 0.499f, 0.25f), 5.0f, false)));
			CHECK(IsNotDrawn(AreaLights::MakeRecord(turned, true, white, 100.0f, glm::vec2(0.25f, AreaLights::c_MinSize * 0.499f), 5.0f, false)));
		}
		// A face whose bounds radius is not finite in float (a half diagonal whose square a float cannot hold, from 1.8e19 m
		// on) is not drawn, whether its own size or the transform's scale makes it that large; one of 1e18 m is. (A scale
		// of 1e19 along both axes would leave the face's normal without a finite length: another refusal.)
		CHECK(IsNotDrawn(AreaLights::MakeRecord(world, true, white, 100.0f, glm::vec2(2.0e38f), 5.0f, false)));
		CHECK(IsNotDrawn(AreaLights::MakeRecord(world * glm::scale(glm::mat4(1.0f), glm::vec3(1.0e19f, 1.0f, 1.0f)), true, white, 100.0f,
			glm::vec2(1.0e19f, 0.25f), 5.0f, false)));
		CHECK(AreaLights::MakeRecord(world * glm::scale(glm::mat4(1.0f), glm::vec3(1.0e9f, 1.0f, 1.0f)), true, white, 100.0f,
			glm::vec2(1.0e9f, 0.25f), 5.0f, false).Drawn);
		for (const glm::vec3& color : { glm::vec3(nan, 1.0f, 1.0f), glm::vec3(1.0f, infinity, 1.0f), glm::vec3(3.0e38f, 1.0f, 1.0f) })
			CHECK(IsNotDrawn(AreaLights::MakeRecord(world, true, color, 100.0f, half, 5.0f, false)));

		// Transforms that leave no face (an axis of no length, two axes along each other) or no side for it to shine to.
		for (const glm::vec3& scale : { glm::vec3(0.0f, 1.0f, 1.0f), glm::vec3(1.0f, 0.0f, 1.0f), glm::vec3(1.0f, 1.0f, 0.0f), glm::vec3(0.0f) })
			CHECK(IsNotDrawn(AreaLights::MakeRecord(world * glm::scale(glm::mat4(1.0f), scale), true, white, 100.0f, half, 5.0f, false)));
		glm::mat4 collapsed = world;
		collapsed[1] = collapsed[0];
		CHECK(IsNotDrawn(AreaLights::MakeRecord(collapsed, true, white, 100.0f, half, 5.0f, false)));
		for (int column = 0; column < 4; column++)
		{
			glm::mat4 broken = world;
			broken[column].y = nan;
			CHECK(IsNotDrawn(AreaLights::MakeRecord(broken, true, white, 100.0f, half, 5.0f, false)));
			broken[column].y = infinity;
			CHECK(IsNotDrawn(AreaLights::MakeRecord(broken, true, white, 100.0f, half, 5.0f, false)));
		}
	}

	TEST_CASE("The nearest point of a face and the window that ends the light's reach")
	{
		AreaLights::AreaLight light = MakeAxisLight();
		light.InverseRangeSquared = 1.0f / (2.0f * 2.0f);
		// In front of the face, beside an edge, past a corner.
		CHECK(glm::length(AreaLights::ClosestPoint(glm::vec3(0.2f, -0.1f, 1.0f), light) - glm::vec3(0.2f, -0.1f, 0.0f)) <= 1e-6f);
		CHECK(glm::length(AreaLights::ClosestPoint(glm::vec3(1.1f, 0.0f, 0.8f), light) - glm::vec3(0.5f, 0.0f, 0.0f)) <= 1e-6f);
		CHECK(glm::length(AreaLights::ClosestPoint(glm::vec3(1.7f, -2.1f, 0.3f), light) - glm::vec3(0.5f, -0.5f, 0.0f)) <= 1e-6f);

		// (1 - (d^2 / range^2)^2)^2 at the distance d from the face, not from its center: 1 m of a range of 2 leave
		// (15 / 16)^2.
		CHECK(AreaLights::GetRangeWindow(glm::vec3(0.1f, 0.1f, 0.0f), light) == 1.0f);
		CHECK(AreaLights::GetRangeWindow(glm::vec3(0.2f, -0.1f, 1.0f), light) == doctest::Approx(0.87890625).epsilon(1e-5));
		CHECK(AreaLights::GetRangeWindow(glm::vec3(1.1f, 0.0f, 0.8f), light) == doctest::Approx(0.87890625).epsilon(1e-5));
		CHECK(AreaLights::GetRangeWindow(glm::vec3(0.5f, 0.5f, -2.0f), light) == 0.0f);
		CHECK(AreaLights::GetRangeWindow(glm::vec3(1.7f, -2.1f, 3.0f), light) == 0.0f);
		// Where d^2 is half of range^2, 9 / 16 are left.
		CHECK(AreaLights::GetRangeWindow(glm::vec3(0.0f, 0.0f, 1.4142135f), light) == doctest::Approx(0.5625).epsilon(1e-5));

		// A face of unequal sides that is along no axis.
		const Frame frame = MakeFrame();
		const glm::dvec3 center(0.4, -0.3, 1.5);
		const glm::dvec3 normal = glm::normalize(glm::dvec3(0.2, 0.1, -1.0));
		const glm::dvec3 axisX = glm::normalize(glm::cross(normal, glm::dvec3(0.0, 1.0, 0.0)));
		const glm::dvec3 axisY = glm::cross(axisX, normal);
		const AreaLights::AreaLight tilted = MakeLight(frame, center, normal, axisX, 0.6, 0.4);
		const glm::vec3 nearest = AreaLights::ClosestPoint(frame.GetPoint(center + axisX * 2.0 + axisY * 0.1 + normal * 0.7), tilted);
		CHECK(glm::length(nearest - frame.GetPoint(center + axisX * 0.6 + axisY * 0.1)) <= 1e-5f);
		const glm::vec3 corner = AreaLights::ClosestPoint(frame.GetPoint(center - axisX * 0.9 + axisY * 3.0 - normal * 0.2), tilted);
		CHECK(glm::length(corner - frame.GetPoint(center - axisX * 0.6 + axisY * 0.4)) <= 1e-5f);
	}

	TEST_CASE("Shading adds the diffuse and specular terms by the rules the image-based light follows")
	{
		const Frame frame = MakeFrame();
		AreaLights::AreaLight light = MakeParallelLight(frame, -0.3, 1.1, -0.6, 0.4, 0.8);
		light.Radiance = glm::vec3(2.0f, 1.0f, 0.5f);
		light.InverseRangeSquared = 1.0f / 25.0f;

		AreaLights::Surface surface;
		surface.Position = frame.GetPoint(glm::dvec3(0.0));
		surface.Normal = frame.GetDirection(glm::dvec3(0.0, 0.0, 1.0));
		surface.View = frame.GetDirection(glm::normalize(glm::dvec3(0.5, 0.2, 0.7)));
		surface.Albedo = glm::vec3(0.8f, 0.6f, 0.4f);
		surface.Metallic = 0.25f;
		surface.Roughness = 0.4f;
		surface.F0 = glm::vec3(0.3f, 0.2f, 0.1f);
		surface.SpecularEnergy = glm::vec3(1.1f, 1.05f, 1.02f);
		const glm::vec2 dfg(0.7f, 0.2f);
		const AreaLights::Coat noCoat;
		const glm::vec2 coatDFG(0.9f, 0.05f);

		// The receiver is 0.8 m under the face, within it: the window of a range of 5 m.
		const double window = std::pow(1.0 - std::pow(0.64 / 25.0, 2.0), 2.0);
		const double formFactor = AreaLights::FormFactor(surface.Position, surface.Normal, light);
		const double fraction = AreaLights::SpecularFraction(surface.Position, surface.Normal, surface.View, 0.4f, light);
		CHECK(GetRelativeError(formFactor, GetParallelFactor(-0.3, 1.1, -0.6, 0.4, 0.8)) <= 1e-4);
		CHECK(fraction > 0.01);
		const glm::dvec3 radiance(2.0, 1.0, 0.5);
		const glm::dvec3 reflectance = glm::dvec3(0.3, 0.2, 0.1) * 0.7 + 0.2;
		const glm::dvec3 diffuse = glm::dvec3(0.8, 0.6, 0.4) * (0.75 * formFactor) * (1.0 - reflectance);
		const glm::dvec3 specular = reflectance * glm::dvec3(1.1, 1.05, 1.02) * fraction;
		const auto checkColor = [](const glm::vec3& value, const glm::dvec3& expected)
		{
			CAPTURE(value.x);
			CAPTURE(value.y);
			CAPTURE(value.z);
			CHECK(glm::length(glm::dvec3(value) - expected) <= 1e-5 * glm::length(expected));
		};

		checkColor(AreaLights::Shade(surface, noCoat, coatDFG, dfg, 1.0f, light, 1.0f), radiance * (diffuse + specular) * window);
		// Each weight scales its own term: the diffuse one is the ambient occlusion's place, the specular one is 0 for the
		// captures of reflection probes, which then hold the diffuse light only.
		checkColor(AreaLights::Shade(surface, noCoat, coatDFG, dfg, 0.5f, light, 0.75f), radiance * (diffuse * 0.5 + specular * 0.75) * window);
		checkColor(AreaLights::Shade(surface, noCoat, coatDFG, dfg, 1.0f, light, 0.0f), radiance * diffuse * window);
		checkColor(AreaLights::Shade(surface, noCoat, coatDFG, dfg, 0.0f, light, 1.0f), radiance * specular * window);

		// A coat takes what it reflects from the layers under it and adds its own lobe, around its own normal: this one
		// mirrors the view into the face, 27 cm from its nearest edge, so nearly all of the coat's sharp lobe is lit.
		AreaLights::Coat coat;
		coat.Normal = frame.GetDirection(glm::normalize(glm::dvec3(0.3, 0.05, 1.0)));
		coat.Strength = 0.8f;
		coat.Roughness = 0.1f;
		coat.SpecularEnergy = 1.03f;
		const double coatReflectance = 0.8 * (0.04 * 0.9 + 0.05) * 1.03;
		const double coatFraction = AreaLights::SpecularFraction(surface.Position, coat.Normal, surface.View, 0.1f, light);
		CHECK(coatFraction > 0.5);
		checkColor(AreaLights::Shade(surface, coat, coatDFG, dfg, 1.0f, light, 1.0f),
			radiance * ((diffuse + specular) * (1.0 - coatReflectance) + coatFraction * coatReflectance) * window);
		checkColor(AreaLights::Shade(surface, coat, coatDFG, dfg, 1.0f, light, 0.0f), radiance * diffuse * (1.0 - coatReflectance) * window);
		// The specular weight scales the coat's lobe as it scales the surface's.
		checkColor(AreaLights::Shade(surface, coat, coatDFG, dfg, 0.5f, light, 0.75f),
			radiance * ((diffuse * 0.5 + specular * 0.75) * (1.0 - coatReflectance) + coatFraction * coatReflectance * 0.75) * window);

		// The diffuse term is FormFactor's, the one with the table's horizon term, also where the exact clip gives another
		// value: of a strip standing across the horizon, 1 m away, the exact clip keeps almost four times what the table does.
		AreaLights::AreaLight strip = MakeStandingLight(frame, -0.1, 0.1, -2.0, 2.0, 1.0);
		strip.Radiance = light.Radiance;
		strip.InverseRangeSquared = light.InverseRangeSquared;
		const double stripFactor = AreaLights::FormFactor(surface.Position, surface.Normal, strip);
		CHECK(stripFactor > 0.005);
		CHECK(AreaLights::FormFactorClipped(surface.Position, surface.Normal, strip) > 3.0 * stripFactor);
		const double stripWindow = std::pow(1.0 - std::pow(1.0 / 25.0, 2.0), 2.0);
		checkColor(AreaLights::Shade(surface, noCoat, coatDFG, dfg, 1.0f, strip, 0.0f),
			radiance * glm::dvec3(0.8, 0.6, 0.4) * (0.75 * stripFactor) * (1.0 - reflectance) * stripWindow);

		// Nothing out of the light's reach, behind it or on its plane.
		AreaLights::AreaLight shortLight = light;
		shortLight.InverseRangeSquared = 1.0f / (0.5f * 0.5f);
		CHECK(AreaLights::Shade(surface, coat, coatDFG, dfg, 1.0f, shortLight, 1.0f) == glm::vec3(0.0f));
		AreaLights::Surface behind = surface;
		behind.Position = frame.GetPoint(glm::dvec3(0.2, 0.0, 1.6));
		behind.Normal = frame.GetDirection(glm::dvec3(0.0, 0.0, -1.0));
		behind.View = frame.GetDirection(glm::normalize(glm::dvec3(0.5, 0.2, -0.7)));
		CHECK(AreaLights::Shade(behind, coat, coatDFG, dfg, 1.0f, light, 1.0f) == glm::vec3(0.0f));
		AreaLights::AreaLight flat = MakeAxisLight(true);
		flat.InverseRangeSquared = 1.0f / 25.0f;
		AreaLights::Surface beside;
		beside.Position = glm::vec3(0.9f, 0.1f, 0.0f);
		beside.Normal = glm::vec3(-1.0f, 0.0f, 0.0f);
		beside.View = glm::vec3(-0.6f, 0.0f, 0.8f);
		CHECK(AreaLights::Shade(beside, noCoat, coatDFG, dfg, 1.0f, flat, 1.0f) == glm::vec3(0.0f));
		// The two-sided light's back shades as its front does: the surface behind is the image, in the light's plane, of
		// one in front.
		AreaLights::AreaLight both = light;
		both.TwoSided = true;
		AreaLights::Surface image = surface;
		image.Position = frame.GetPoint(glm::dvec3(0.2, 0.0, 0.0));
		const glm::vec3 fromFront = AreaLights::Shade(image, noCoat, coatDFG, dfg, 1.0f, light, 1.0f);
		CHECK(fromFront.x > 0.01f);
		checkColor(AreaLights::Shade(behind, noCoat, coatDFG, dfg, 1.0f, both, 1.0f), glm::dvec3(fromFront));
		CHECK(AreaLights::Shade(image, noCoat, coatDFG, dfg, 1.0f, both, 1.0f) == fromFront);
	}

	TEST_CASE("A roughness under the shaders' least one is shaded as the least one")
	{
		// The shaders evaluate no lobe under c_MinRoughness, and a material's roughness may be 0, as a coat's is unless it
		// is set: the tables' first column, read as it is, is a mirror's lobe. A strip 4 mm wide, 2 m along the mirror
		// direction, is narrower than the least lobe and wider than the mirror's, so it tells the two apart.
		const Frame frame = MakeFrame();
		const glm::dvec3 viewDirection(std::sqrt(1.0 - 0.7 * 0.7), 0.0, 0.7);
		const glm::dvec3 mirror(-viewDirection.x, 0.0, viewDirection.z);
		AreaLights::AreaLight strip = MakeLight(frame, mirror * 2.0, -mirror, glm::dvec3(0.0, 1.0, 0.0), 0.5, 0.002);
		strip.InverseRangeSquared = 1.0f / 25.0f;
		const glm::vec3 position = frame.GetPoint(glm::dvec3(0.0));
		const glm::vec3 normal = frame.GetDirection(glm::dvec3(0.0, 0.0, 1.0));
		const glm::vec3 view = frame.GetDirection(viewDirection);

		const float least = AreaLights::SpecularFraction(position, normal, view, AreaLights::c_MinRoughness, strip);
		const glm::vec2 mirrorCoordinates(0.0f, std::sqrt(1.0f - glm::dot(normal, view)));
		const float unclamped = AreaLights::GetLobeCoverage(AreaLights::SampleTable(AreaLights::c_TransformLayer, mirrorCoordinates), position, normal, view, strip);
		MESSAGE("least roughness: the strip holds " << least << " of the least lobe and " << unclamped << " of the lobe of the tables' first column");
		CHECK(least > 0.05f);
		CHECK(least < 0.75f);
		CHECK(unclamped > least + 0.2f);
		for (const float roughness : { 0.0f, 0.02f, -1.0f })
			CHECK(AreaLights::SpecularFraction(position, normal, view, roughness, strip) == least);
		CHECK(AreaLights::SpecularFraction(position, normal, view, 0.06f, strip) < least);

		// Shading clamps the surface's roughness and the coat's the same way.
		AreaLights::Surface surface;
		surface.Position = position;
		surface.Normal = normal;
		surface.View = view;
		surface.Roughness = AreaLights::c_MinRoughness;
		const glm::vec2 dfg(0.7f, 0.2f);
		const glm::vec2 coatDFG(0.9f, 0.05f);
		AreaLights::Coat coat;
		coat.Normal = normal;
		coat.Strength = 0.8f;
		coat.Roughness = AreaLights::c_MinRoughness;
		const glm::vec3 shaded = AreaLights::Shade(surface, coat, coatDFG, dfg, 1.0f, strip, 1.0f);
		CHECK(shaded.x > 0.01f);
		AreaLights::Surface smooth = surface;
		smooth.Roughness = 0.0f;
		CHECK(AreaLights::Shade(smooth, coat, coatDFG, dfg, 1.0f, strip, 1.0f) == shaded);
		// A coat's roughness is 0 until it is set.
		AreaLights::Coat unset;
		unset.Normal = normal;
		unset.Strength = 0.8f;
		CHECK(unset.Roughness == 0.0f);
		CHECK(AreaLights::Shade(surface, unset, coatDFG, dfg, 1.0f, strip, 1.0f) == shaded);
		// The coat's lobe is in the sum as the least lobe: the surface's terms lose what the coat reflects, and the coat adds
		// that reflectance times the least lobe's share. The receiver is on the face's axis, 2 m from its center.
		const double window = std::pow(1.0 - std::pow(4.0 / 25.0, 2.0), 2.0);
		const double reflectance = 0.04 * 0.7 + 0.2;
		const double coatReflectance = 0.8 * (0.04 * 0.9 + 0.05);
		const double formFactor = AreaLights::FormFactor(position, normal, strip);
		const double expected = ((formFactor * (1.0 - reflectance) + reflectance * least) * (1.0 - coatReflectance) + least * coatReflectance) * window;
		CHECK(glm::length(glm::dvec3(shaded) - glm::dvec3(expected)) <= 1e-5 * glm::length(glm::dvec3(expected)));
	}

	TEST_CASE("The least roughness and the coat's F0 are the shaders'")
	{
		// AreaLights.h repeats two constants of Include/PBR.slang, which no C++ code can include.
		const std::optional<std::string> source = FileSystem::ReadText(FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "Strata" / "shaders" / "Include" / "PBR.slang");
		REQUIRE(source.has_value());
		const std::optional<double> minRoughness = FindShaderConstant(*source, "c_MinRoughness");
		const std::optional<double> clearcoatF0 = FindShaderConstant(*source, "c_ClearcoatF0");
		REQUIRE(minRoughness.has_value());
		REQUIRE(clearcoatF0.has_value());
		CHECK(static_cast<float>(*minRoughness) == AreaLights::c_MinRoughness);
		CHECK(static_cast<float>(*clearcoatF0) == AreaLights::c_ClearcoatF0);
	}

	TEST_CASE("A light that fills the sky returns its radiance from a white surface")
	{
		// The white furnace: under a uniform sky a surface that absorbs nothing sends back what it receives. A metal of
		// F0 1 reflects Es = dfg.x + dfg.y in one bounce and its energy term 1 / Es restores the rest; a dielectric of
		// F0 0 reflects dfg.y and its white diffuse layer the rest. What the fitted lobe keeps under the surface's horizon
		// is lost to both: the metal returns the lobe's share above it, GetSkyShare's, and the dielectric that share of its
		// specular part.
		const Frame frame = MakeFrame();
		AreaLights::AreaLight sky = MakeSkyLight(frame);
		sky.Radiance = glm::vec3(2.0f, 1.0f, 0.5f);
		const glm::dvec3 radiance(sky.Radiance);
		const double skyFactor = GetParallelFactor(-1000.0, 1000.0, -1000.0, 1000.0, 1.0);
		const glm::vec2 dfg(0.6f, 0.3f);
		const AreaLights::Coat noCoat;
		double worstWithin = 0.0;   // Roughness 0.1 and 1, view cosines from 0.5
		double worstMiddle = 0.0;   // Roughness 0.5, view cosines from 0.5
		double worstExpected = 0.0; // Against the lobe's share above the horizon, everywhere
		for (const float roughness : { 0.1f, 0.25f, 0.5f, 0.75f, 1.0f })
		{
			constexpr double c_ViewCosines[] = { 1.0, 0.8, 0.5, 0.2, 0.05 };
			double metalReturns[std::size(c_ViewCosines)] = {};
			for (size_t view = 0; view < std::size(c_ViewCosines); view++)
			{
				const double cosine = c_ViewCosines[view];
				CAPTURE(roughness);
				CAPTURE(cosine);
				AreaLights::Surface surface;
				surface.Position = frame.GetPoint(glm::dvec3(0.0));
				surface.Normal = frame.GetDirection(glm::dvec3(0.0, 0.0, 1.0));
				surface.View = frame.GetDirection(glm::dvec3(std::sqrt(1.0 - cosine * cosine), 0.0, cosine));
				surface.Roughness = roughness;
				surface.Albedo = glm::vec3(1.0f);

				surface.Metallic = 1.0f;
				surface.F0 = glm::vec3(1.0f);
				surface.SpecularEnergy = glm::vec3(1.0f / (dfg.x + dfg.y));
				const glm::vec3 metal = AreaLights::Shade(surface, noCoat, glm::vec2(0.0f), dfg, 1.0f, sky, 1.0f);
				surface.Metallic = 0.0f;
				surface.F0 = glm::vec3(0.0f);
				surface.SpecularEnergy = glm::vec3(1.0f);
				const glm::vec3 dielectric = AreaLights::Shade(surface, noCoat, glm::vec2(0.0f), dfg, 1.0f, sky, 1.0f);
				metalReturns[view] = metal.x / sky.Radiance.x;

				const double share = GetSkyShare(roughness, glm::dot(surface.Normal, surface.View));
				const glm::dvec3 expected[] = { radiance * share, radiance * ((1.0 - dfg.y) * skyFactor + dfg.y * share) };
				const glm::vec3 values[] = { metal, dielectric };
				for (size_t index = 0; index < std::size(values); index++)
				{
					const glm::vec3& value = values[index];
					const double expectedDifference = glm::length(glm::dvec3(value) - expected[index]) / glm::length(radiance);
					worstExpected = std::max(worstExpected, expectedDifference);
					CHECK(expectedDifference <= c_SkyShareTolerance);
					// The bounds measured before the share had its closed form, on the views and roughnesses they were measured on.
					if (cosine >= 0.5 && roughness != 0.25f && roughness != 0.75f)
					{
						double& worst = roughness == 0.5f ? worstMiddle : worstWithin;
						worst = std::max(worst, static_cast<double>(glm::length(value - sky.Radiance) / glm::length(sky.Radiance)));
					}
					// Never more than it receives.
					CHECK(value.x <= sky.Radiance.x * (1.0f + 1e-5f));
					CHECK(value.y <= sky.Radiance.y * (1.0f + 1e-5f));
					CHECK(value.z <= sky.Radiance.z * (1.0f + 1e-5f));
				}
			}
			MESSAGE("white furnace: at roughness " << roughness << " the metal returns " << metalReturns[0] << ", " << metalReturns[1] << ", " << metalReturns[2]
				<< ", " << metalReturns[3] << " and " << metalReturns[4] << " of the sky at view cosines of 1, 0.8, 0.5, 0.2 and 0.05");
		}
		MESSAGE("white furnace: worst relative difference at roughness 0.1 and 1 " << worstWithin << ", at roughness 0.5 " << worstMiddle
			<< " (view cosines from 0.5); from the lobe's share above the horizon " << worstExpected);
		// The sharp and the fully rough lobe lie within the hemisphere and return all of it from views of 60 degrees off the
		// normal or less (measured within 3.1e-4).
		CHECK(worstWithin <= 5e-4);
		// At roughness 0.5 the fitted lobe keeps part of its mass under the surface's horizon, more the lower the view:
		// the metal returns 0.979 at a view cosine of 0.8 and 0.940 at 0.5. The bound is 1.5 times the measured 6.05%. The
		// lower views and the other roughnesses are held to the share above the horizon alone: the metal returns 0.893 at
		// roughness 0.5 seen at a view cosine of 0.2, and 0.845 at roughness 0.25 seen at 0.05 (see SpecularFraction).
		CHECK(worstMiddle <= 0.091);
	}
}

// The specular lobe's reference: GGX itself (the distribution and the height-correlated visibility the engine shades with,
// F = 1) integrated over the light's face by an adaptive quadrature in double. Plain scalars instead of glm: the tests
// evaluate it millions of times, also in Debug.
namespace
{

	enum class Integrand
	{
		Cosine,   // cos / pi: the form factor
		Specular, // D Vis cos: the lobe the tables are fitted to
		Fresnel   // The same times (1 - VoH)^5: what the tables' second term sums
	};

	// A face in a receiver's frame: the receiver at the origin, its normal +Z, the view in the XZ plane, toward +X.
	struct Rectangle
	{
		glm::dvec3 Center = glm::dvec3(0.0);
		glm::dvec3 Normal = glm::dvec3(0.0, 0.0, -1.0);
		glm::dvec3 AxisX = glm::dvec3(1.0, 0.0, 0.0); // Unit
		double HalfX = 0.5;
		double HalfY = 0.5;
		bool TwoSided = false;
	};

	struct LobeQuadrature
	{
		Integrand Kind = Integrand::Specular;
		double Alpha = 1.0;      // GGX's: the perceptual roughness squared
		double ViewX = 0.0;      // The view is (ViewX, 0, ViewZ)
		double ViewZ = 1.0;
		double Resolution = 0.25; // The share of the lobe's width a cell may span
		double Center[3] = {};
		double AxisX[3] = {};    // Half axes
		double AxisY[3] = {};
		double Normal[3] = {};
		double LengthX = 0.0;
		double LengthY = 0.0;
		double Area = 0.0;       // The face's area per unit of u and v
		uint64_t Evaluations = 0;
	};

	LobeQuadrature MakeQuadrature(Integrand kind, double roughness, double viewCosine, const Rectangle& rectangle)
	{
		LobeQuadrature quadrature;
		quadrature.Kind = kind;
		quadrature.Alpha = kind == Integrand::Cosine ? 1.0 : roughness * roughness;
		quadrature.ViewX = std::sqrt(std::max(1.0 - viewCosine * viewCosine, 0.0));
		quadrature.ViewZ = viewCosine;
		const glm::dvec3 axisX = rectangle.AxisX * rectangle.HalfX;
		const glm::dvec3 axisY = glm::cross(rectangle.AxisX, rectangle.Normal) * rectangle.HalfY;
		for (int index = 0; index < 3; index++)
		{
			quadrature.Center[index] = rectangle.Center[index];
			quadrature.AxisX[index] = axisX[index];
			quadrature.AxisY[index] = axisY[index];
			quadrature.Normal[index] = rectangle.Normal[index];
		}
		quadrature.LengthX = rectangle.HalfX;
		quadrature.LengthY = rectangle.HalfY;
		quadrature.Area = rectangle.HalfX * rectangle.HalfY;
		return quadrature;
	}

	// What decides how finely a cell is cut, at one of its points.
	struct CellSample
	{
		double Cosine = 0.0;   // Of the direction to the point with the receiver's normal
		double Distance = 0.0;
		double HalfX = 0.0;    // The half vector's part along the surface: the distribution depends on its length
		double HalfY = 0.0;
	};

	CellSample SampleCell(const LobeQuadrature& quadrature, double u, double v)
	{
		const double x = quadrature.Center[0] + quadrature.AxisX[0] * u + quadrature.AxisY[0] * v;
		const double y = quadrature.Center[1] + quadrature.AxisX[1] * u + quadrature.AxisY[1] * v;
		const double z = quadrature.Center[2] + quadrature.AxisX[2] * u + quadrature.AxisY[2] * v;
		const double distance = std::max(std::sqrt(x * x + y * y + z * z), 1e-300);
		const double halfX = x / distance + quadrature.ViewX;
		const double halfY = y / distance;
		const double halfZ = z / distance + quadrature.ViewZ;
		const double length = std::max(std::sqrt(halfX * halfX + halfY * halfY + halfZ * halfZ), 1e-300);
		CellSample sample;
		sample.Cosine = z / distance;
		sample.Distance = distance;
		sample.HalfX = halfX / length;
		sample.HalfY = halfY / length;
		return sample;
	}

	// The integrand per unit of the face's area.
	double EvaluateIntegrand(LobeQuadrature& quadrature, double u, double v)
	{
		const double x = quadrature.Center[0] + quadrature.AxisX[0] * u + quadrature.AxisY[0] * v;
		const double y = quadrature.Center[1] + quadrature.AxisX[1] * u + quadrature.AxisY[1] * v;
		const double z = quadrature.Center[2] + quadrature.AxisX[2] * u + quadrature.AxisY[2] * v;
		if (!(z > 0.0))
			return 0.0; // Under the receiver's horizon
		quadrature.Evaluations++;
		const double distanceSquared = x * x + y * y + z * z;
		const double distance = std::sqrt(distanceSquared);
		const double lightX = x / distance;
		const double lightY = y / distance;
		const double lightZ = z / distance;
		// The solid angle of a unit of the face's area, whichever of its sides is seen.
		const double solidAngle = std::abs(lightX * quadrature.Normal[0] + lightY * quadrature.Normal[1] + lightZ * quadrature.Normal[2]) / distanceSquared;
		if (quadrature.Kind == Integrand::Cosine)
			return lightZ / c_Pi * solidAngle;

		const double halfX = lightX + quadrature.ViewX;
		const double halfY = lightY;
		const double halfZ = lightZ + quadrature.ViewZ;
		const double halfSquared = halfX * halfX + halfY * halfY + halfZ * halfZ;
		// GGX: alpha^2 / (pi (cos^2 (alpha^2 - 1) + 1)^2) for the half vector's cosine, the bracket written as
		// sin^2 + alpha^2 cos^2 so that a sharp lobe's peak keeps its digits.
		const double alphaSquared = quadrature.Alpha * quadrature.Alpha;
		const double bracket = (halfX * halfX + halfY * halfY + alphaSquared * halfZ * halfZ) / halfSquared;
		const double distribution = alphaSquared / (c_Pi * bracket * bracket);
		const double viewZ = quadrature.ViewZ;
		const double visibility = 0.5 / (lightZ * std::sqrt(viewZ * viewZ * (1.0 - alphaSquared) + alphaSquared) +
			viewZ * std::sqrt(lightZ * lightZ * (1.0 - alphaSquared) + alphaSquared));
		double value = distribution * visibility * lightZ * solidAngle;
		if (quadrature.Kind == Integrand::Fresnel)
		{
			// V.H = (V.L + 1) / |L + V|
			const double viewDotHalf = (lightX * quadrature.ViewX + lightZ * viewZ + 1.0) / std::sqrt(halfSquared);
			value *= std::pow(std::max(1.0 - viewDotHalf, 0.0), 5.0);
		}
		return value;
	}

	double GetHalfDistance(const CellSample& first, const CellSample& second)
	{
		return std::hypot(first.HalfX - second.HalfX, first.HalfY - second.HalfY);
	}

	// The integral over the cell (u +- halfU, v +- halfV) of the face's parameters. A cell is cut along an axis while,
	// along it, it is long for its distance, spans more of the lobe than Resolution allows (the lobe's width where the cell
	// is: alpha at the peak, the distance from the peak away from it), or spans much of the layer above the horizon in which
	// the visibility term rises; a cell the horizon crosses is cut to Resolution / 32 of the face. Then 3 x 3 Gauss-Legendre
	// points.
	double IntegrateCell(LobeQuadrature& quadrature, double u, double v, double halfU, double halfV, int depth)
	{
		constexpr int c_MaxDepth = 64;
		const double horizonCell = quadrature.Resolution / 32.0;

		const CellSample center = SampleCell(quadrature, u, v);
		const CellSample corners[4] = { SampleCell(quadrature, u - halfU, v - halfV), SampleCell(quadrature, u + halfU, v - halfV),
			SampleCell(quadrature, u - halfU, v + halfV), SampleCell(quadrature, u + halfU, v + halfV) };
		double nearest = center.Distance;
		double leastCosine = center.Cosine;
		double greatestCosine = center.Cosine;
		double leastHalf = std::hypot(center.HalfX, center.HalfY);
		for (const CellSample& corner : corners)
		{
			nearest = std::min(nearest, corner.Distance);
			leastCosine = std::min(leastCosine, corner.Cosine);
			greatestCosine = std::max(greatestCosine, corner.Cosine);
			leastHalf = std::min(leastHalf, std::hypot(corner.HalfX, corner.HalfY));
		}
		if (!(greatestCosine > 0.0))
			return 0.0; // A flat cell whose corners are under the horizon is under it

		bool cutU = 2.0 * halfU * quadrature.LengthX > 0.25 * nearest;
		bool cutV = 2.0 * halfV * quadrature.LengthY > 0.25 * nearest;
		if (quadrature.Kind != Integrand::Cosine)
		{
			const double spreadU = std::max(GetHalfDistance(corners[0], corners[1]), GetHalfDistance(corners[2], corners[3]));
			const double spreadV = std::max(GetHalfDistance(corners[0], corners[2]), GetHalfDistance(corners[1], corners[3]));
			const double fromPeak = std::max(leastHalf - 0.5 * (spreadU + spreadV), 0.0);
			const double width = quadrature.Resolution * std::sqrt(quadrature.Alpha * quadrature.Alpha + fromPeak * fromPeak);
			cutU = cutU || spreadU > width;
			cutV = cutV || spreadV > width;
			const double layer = quadrature.Resolution * (std::max(leastCosine, 0.0) + quadrature.Alpha);
			cutU = cutU || std::max(std::abs(corners[0].Cosine - corners[1].Cosine), std::abs(corners[2].Cosine - corners[3].Cosine)) > layer;
			cutV = cutV || std::max(std::abs(corners[0].Cosine - corners[2].Cosine), std::abs(corners[1].Cosine - corners[3].Cosine)) > layer;
		}
		if (!(leastCosine > 0.0))
		{
			cutU = cutU || halfU > horizonCell;
			cutV = cutV || halfV > horizonCell;
		}
		if (depth < c_MaxDepth && (cutU || cutV))
		{
			const double childU = cutU ? halfU * 0.5 : halfU;
			const double childV = cutV ? halfV * 0.5 : halfV;
			double sum = 0.0;
			for (int indexV = 0; indexV < (cutV ? 2 : 1); indexV++)
			{
				for (int indexU = 0; indexU < (cutU ? 2 : 1); indexU++)
				{
					const double childCenterU = cutU ? u + (indexU == 0 ? -childU : childU) : u;
					const double childCenterV = cutV ? v + (indexV == 0 ? -childV : childV) : v;
					sum += IntegrateCell(quadrature, childCenterU, childCenterV, childU, childV, depth + 1);
				}
			}
			return sum;
		}

		const double node = std::sqrt(0.6);
		const double nodes[3] = { -node, 0.0, node };
		const double weights[3] = { 5.0 / 9.0, 8.0 / 9.0, 5.0 / 9.0 };
		double sum = 0.0;
		for (int indexV = 0; indexV < 3; indexV++)
		{
			for (int indexU = 0; indexU < 3; indexU++)
				sum += weights[indexU] * weights[indexV] * EvaluateIntegrand(quadrature, u + halfU * nodes[indexU], v + halfV * nodes[indexV]);
		}
		return sum * halfU * halfV * quadrature.Area;
	}

	double Integrate(LobeQuadrature& quadrature)
	{
		return IntegrateCell(quadrature, 0.0, 0.0, 1.0, 1.0, 0);
	}

	double Integrate(Integrand kind, double roughness, double viewCosine, const Rectangle& rectangle)
	{
		LobeQuadrature quadrature = MakeQuadrature(kind, roughness, viewCosine, rectangle);
		return Integrate(quadrature);
	}

	AreaLights::AreaLight MakeLight(const Frame& frame, const Rectangle& rectangle)
	{
		return MakeLight(frame, rectangle.Center, rectangle.Normal, rectangle.AxisX, rectangle.HalfX, rectangle.HalfY, rectangle.TwoSided);
	}

	// A rectangle that faces the receiver from `center`, turned by `roll` about the line between them.
	Rectangle MakeFacingRectangle(const glm::dvec3& center, double roll, double halfX, double halfY)
	{
		Rectangle rectangle;
		rectangle.Center = center;
		rectangle.Normal = -glm::normalize(center);
		const glm::dvec3 tangent = glm::normalize(glm::cross(rectangle.Normal, std::abs(rectangle.Normal.y) < 0.9 ? glm::dvec3(0.0, 1.0, 0.0) : glm::dvec3(1.0, 0.0, 0.0)));
		const glm::dvec3 bitangent = glm::cross(rectangle.Normal, tangent);
		rectangle.AxisX = tangent * std::cos(roll) + bitangent * std::sin(roll);
		rectangle.HalfX = halfX;
		rectangle.HalfY = halfY;
		return rectangle;
	}

	Rectangle MakeRectangle(const glm::dvec3& center, const glm::dvec3& normal, const glm::dvec3& axisX, double halfX, double halfY, bool twoSided = false)
	{
		Rectangle rectangle;
		rectangle.Center = center;
		rectangle.Normal = glm::normalize(normal);
		// The axis as given, made perpendicular to the normal.
		rectangle.AxisX = glm::normalize(axisX - rectangle.Normal * glm::dot(axisX, rectangle.Normal));
		rectangle.HalfX = halfX;
		rectangle.HalfY = halfY;
		rectangle.TwoSided = twoSided;
		return rectangle;
	}

	constexpr float c_SpecularRoughness[] = { 0.05f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f };
	constexpr int c_SpecularViewSteps = 8;

	double GetSpecularViewCosine(int step)
	{
		return 0.05 + 0.95 * step / (c_SpecularViewSteps - 1);
	}

	// The 24 faces the specular lobe is compared on, for a view whose cosine with the normal is `viewCosine`: eight placed
	// around the mirror direction, where the lobe is, and sixteen that stay where they are while the view turns (rooms'
	// panels, lights the horizon cuts, lights on the viewer's side, small, large and thin ones).
	std::vector<Rectangle> GetSpecularRectangles(double viewCosine)
	{
		const glm::dvec3 mirror(-std::sqrt(std::max(1.0 - viewCosine * viewCosine, 0.0)), 0.0, viewCosine);
		const glm::dvec3 down(0.0, 0.0, -1.0);
		const glm::dvec3 alongX(1.0, 0.0, 0.0);
		const glm::dvec3 alongY(0.0, 1.0, 0.0);
		std::vector<Rectangle> rectangles;
		rectangles.push_back(MakeFacingRectangle(mirror * 2.0, 0.0, 0.5, 0.5));
		rectangles.push_back(MakeFacingRectangle(mirror * 1.0, 0.6, 0.5, 0.25));
		rectangles.push_back(MakeFacingRectangle(mirror * 4.0, 1.2, 1.0, 0.3));
		rectangles.push_back(MakeFacingRectangle(mirror * 0.5, 0.0, 0.1, 0.1));
		rectangles.push_back(MakeFacingRectangle(mirror * 8.0, 0.3, 2.0, 1.5));
		rectangles.push_back(MakeFacingRectangle(mirror * 2.0, 0.9, 0.05, 1.5));
		rectangles.push_back(MakeFacingRectangle(mirror * 3.0 + glm::dvec3(0.0, 0.8, 0.3), 0.0, 0.5, 0.5));
		rectangles.push_back(MakeFacingRectangle(mirror * 1.5 + glm::dvec3(0.0, -1.0, 0.5), 0.4, 0.4, 0.6));

		// Overhead and ceiling panels, facing down.
		rectangles.push_back(MakeRectangle(glm::dvec3(0.0, 0.0, 2.0), down, alongX, 0.5, 0.5));
		rectangles.push_back(MakeRectangle(glm::dvec3(0.5, -0.3, 1.0), down, alongX, 1.5, 1.0));
		rectangles.push_back(MakeRectangle(glm::dvec3(-2.0, 0.0, 2.5), down, alongX, 0.6, 0.3));
		rectangles.push_back(MakeRectangle(glm::dvec3(-4.0, 1.0, 2.5), down, alongX, 0.6, 0.6));
		// A light on the viewer's side.
		rectangles.push_back(MakeFacingRectangle(glm::dvec3(2.0, 0.5, 1.5), 0.0, 0.5, 0.5));
		// Wall panels ahead: one whose lower edge is 5 cm above the receiver's plane, one the plane cuts, one mostly under it.
		rectangles.push_back(MakeRectangle(glm::dvec3(-1.0, 0.0, 0.525), alongX, alongY, 1.2, 0.475));
		rectangles.push_back(MakeRectangle(glm::dvec3(-2.0, 0.0, 0.5), alongX, alongY, 1.0, 1.0));
		rectangles.push_back(MakeRectangle(glm::dvec3(-1.5, 0.0, -0.65), alongX, alongY, 0.5, 0.85));
		// A panel on a side wall.
		rectangles.push_back(MakeRectangle(glm::dvec3(-1.0, 1.5, 1.05), -alongY, alongX, 1.0, 0.95));
		// A low ceiling panel far ahead, seen at a grazing angle.
		rectangles.push_back(MakeRectangle(glm::dvec3(-3.0, 0.0, 0.5), down, alongX, 1.0, 1.0));
		// A two-sided light seen from behind.
		rectangles.push_back(MakeRectangle(glm::dvec3(-1.5, 0.5, 1.5), glm::dvec3(-1.5, 0.5, 1.5), alongY, 0.6, 0.4, true));
		// A small light close by and a large one far away.
		rectangles.push_back(MakeFacingRectangle(glm::dvec3(-0.2, 0.1, 0.3), 0.0, 0.05, 0.05));
		rectangles.push_back(MakeFacingRectangle(glm::dvec3(-10.0, 3.0, 6.0), 0.2, 3.0, 2.0));
		// A strip light overhead, across the view.
		rectangles.push_back(MakeRectangle(glm::dvec3(-1.0, 0.0, 1.5), down, alongY, 2.0, 0.03));
		// A panel on the wall behind the viewer.
		rectangles.push_back(MakeRectangle(glm::dvec3(2.0, 0.0, 0.5), -alongX, alongY, 1.0, 0.5));
		// A panel that faces the receiver askew.
		rectangles.push_back(MakeRectangle(glm::dvec3(-1.0, -1.0, 1.0), glm::dvec3(0.5, 0.7, -0.6), alongX, 0.7, 0.2));
		return rectangles;
	}

	struct SpecularCase
	{
		float Roughness = 1.0f;
		double ViewCosine = 1.0;
		size_t Face = 0;
		AreaLights::AreaLight Light;
		glm::vec3 View = glm::vec3(0.0f);
		double Reference = 0.0; // GGX's integral over the face
		double Magnitude = 0.0; // The tables' whole lobe
	};

	struct SpecularCases
	{
		Frame ReceiverFrame;
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Normal = glm::vec3(0.0f);
		std::vector<SpecularCase> Cases;
		uint64_t Evaluations = 0;
	};

	// The references of every roughness, view and face, integrated once for the tests that read them.
	const SpecularCases& GetSpecularCases()
	{
		static const SpecularCases s_Cases = []
		{
			SpecularCases cases;
			cases.ReceiverFrame = MakeFrame();
			cases.Position = cases.ReceiverFrame.GetPoint(glm::dvec3(0.0));
			cases.Normal = cases.ReceiverFrame.GetDirection(glm::dvec3(0.0, 0.0, 1.0));
			for (const float roughness : c_SpecularRoughness)
			{
				for (int step = 0; step < c_SpecularViewSteps; step++)
				{
					const double viewCosine = GetSpecularViewCosine(step);
					const std::vector<Rectangle> rectangles = GetSpecularRectangles(viewCosine);
					const float magnitude = AreaLights::SampleTable(AreaLights::c_TermsLayer, glm::vec2(roughness, std::sqrt(1.0f - static_cast<float>(viewCosine)))).x;
					for (size_t index = 0; index < rectangles.size(); index++)
					{
						SpecularCase entry;
						entry.Roughness = roughness;
						entry.ViewCosine = viewCosine;
						entry.Face = index;
						entry.Light = MakeLight(cases.ReceiverFrame, rectangles[index]);
						entry.View = cases.ReceiverFrame.GetDirection(glm::dvec3(std::sqrt(std::max(1.0 - viewCosine * viewCosine, 0.0)), 0.0, viewCosine));
						LobeQuadrature quadrature = MakeQuadrature(Integrand::Specular, roughness, viewCosine, rectangles[index]);
						entry.Reference = Integrate(quadrature);
						entry.Magnitude = magnitude;
						cases.Evaluations += quadrature.Evaluations;
						cases.Cases.push_back(entry);
					}
				}
			}
			return cases;
		}();
		return s_Cases;
	}

	// The value at `share` (0..1) of the sorted values. NaN for no values, which fails every bound it is checked against:
	// some lists hold what the code under test makes of the cases, which a broken table can leave empty.
	double GetPercentile(std::span<const double> unsorted, double share)
	{
		if (unsorted.empty())
			return std::numeric_limits<double>::quiet_NaN();
		std::vector<double> values(unsorted.begin(), unsorted.end());
		std::sort(values.begin(), values.end());
		return values[std::min(static_cast<size_t>(share * static_cast<double>(values.size())), values.size() - 1)];
	}

	constexpr size_t c_SpecularFaces = 24;
	// The first faces of every view are the ones around its mirror direction, and the first of those are centered on it.
	constexpr size_t c_MirrorFaces = 8;
	constexpr size_t c_CenteredMirrorFaces = 6;
	constexpr size_t c_CasesPerRoughness = c_SpecularViewSteps * c_SpecularFaces;
	// The first of the steep views, the upper half of the eight: a cosine of 0.59 and more.
	constexpr int c_SteepViewStep = 4;

	std::span<const SpecularCase> GetRoughnessCases(const SpecularCases& cases, size_t roughnessIndex)
	{
		return std::span<const SpecularCase>(cases.Cases).subspan(roughnessIndex * c_CasesPerRoughness, c_CasesPerRoughness);
	}

	// What the shading returns for each case as a share of the light's radiance, at F0 = 1: the fitted lobe's part on the
	// face times the tables' magnitude. `transposed` swaps the transform's two off-diagonal terms, the slip a port of the
	// tables makes most easily.
	std::vector<double> GetSpecularShares(const SpecularCases& cases, std::span<const SpecularCase> entries, bool transposed)
	{
		std::vector<double> shares;
		for (const SpecularCase& entry : entries)
		{
			double fraction = AreaLights::SpecularFraction(cases.Position, cases.Normal, entry.View, entry.Roughness, entry.Light);
			if (transposed)
			{
				const glm::vec2 uv(entry.Roughness, std::sqrt(1.0f - static_cast<float>(entry.ViewCosine)));
				glm::vec4 transform = AreaLights::SampleTable(AreaLights::c_TransformLayer, uv);
				std::swap(transform.y, transform.z);
				fraction = AreaLights::GetLobeCoverage(transform, cases.Position, cases.Normal, entry.View, entry.Light);
			}
			shares.push_back(fraction * entry.Magnitude);
		}
		return shares;
	}

	// The shares' errors relative to GGX's integral over each face or, for a face that gets less than a hundredth of the
	// lobe, to that hundredth: under it a face holds a tail of the lobe, which the fit does not follow (on the wall behind
	// the viewer GGX keeps about 1% of a lobe of middling roughness where the fit has next to nothing). Negative where the
	// share comes out under GGX's.
	std::vector<double> GetSignedSpecularErrors(std::span<const SpecularCase> entries, std::span<const double> shares)
	{
		std::vector<double> errors;
		for (size_t index = 0; index < entries.size(); index++)
			errors.push_back((shares[index] - entries[index].Reference) / std::max(entries[index].Reference, 0.01 * entries[index].Magnitude));
		return errors;
	}

	// The errors' sizes.
	std::vector<double> GetSpecularErrors(std::span<const SpecularCase> entries, std::span<const double> shares)
	{
		std::vector<double> errors = GetSignedSpecularErrors(entries, shares);
		for (double& error : errors)
			error = std::abs(error);
		return errors;
	}

	// The errors on the faces that hold at least c_HoldingShare of the lobe: relative to GGX's share there.
	constexpr double c_HoldingShare = 0.05;

	std::vector<double> GetHoldingErrors(std::span<const SpecularCase> entries, std::span<const double> errors)
	{
		std::vector<double> holding;
		for (size_t index = 0; index < entries.size(); index++)
		{
			if (entries[index].Reference >= c_HoldingShare * entries[index].Magnitude)
				holding.push_back(errors[index]);
		}
		return holding;
	}

	// The errors on the faces around the mirror direction, for the views off the normal.
	std::vector<double> GetMirrorErrors(std::span<const double> errors)
	{
		std::vector<double> mirror;
		for (int step = 0; step < c_SpecularViewSteps - 1; step++)
		{
			const std::span<const double> faces = errors.subspan(static_cast<size_t>(step) * c_SpecularFaces, c_MirrorFaces);
			mirror.insert(mirror.end(), faces.begin(), faces.end());
		}
		return mirror;
	}

	// What the comparison with GGX is held to at one roughness.
	struct SpecularBounds
	{
		double Median = 0.0;          // Of the relative errors over the eight views
		double Percentile = 0.0;      // Their 95th percentile
		double SteepMedian = 0.0;     // The same two over the steep views
		double SteepPercentile = 0.0;
		double MirrorMedian = 0.0;    // The median on the faces around the mirror direction, for the views off the normal
		double Share = 0.0;           // The greatest difference of a share, of the light's radiance
		// A share that comes out too small is off by 1 at most, and at middling roughness the 95th percentile of all the
		// errors is near or over 1 with the fit's own excess toward grazing views: it guards against too large a share only.
		// These two guard against too small a one, with bounds under 1.
		double Under = 0.0;           // How far under GGX's the 5th percentile of the errors with their sign reaches
		double HoldingPercentile = 0.0; // The 95th percentile on the faces that hold c_HoldingShare of the lobe or more
	};

	// The aim is a median within 3% and a 95th percentile within 10% (15% under roughness 0.25). The published fit meets it
	// for sharp lobes and, of the rougher ones, for the steep views' medians only: toward grazing views its lobe departs
	// from GGX's, and a part of it lies under the horizon, where no light reaches it. So each bound is 1.5 times the value
	// measured when the comparison was written (the test prints the values), or the aim where the measurement meets the aim
	// and 1.5 times it would not. The largest differences of a share belong to the lowest view (a cosine of 0.05): at
	// roughness 0.05 its mirror direction meets the lower edge of the wall panel that ends 5 cm above the receiver's plane,
	// which puts half the lobe at stake, and for the rougher lobes they are on the wall panels at the plane. Each row's
	// measurement is the comment above it, in the order of the fields.
	constexpr SpecularBounds c_SpecularBounds[] = {
		// 0.00047, 0.023, 0.00051, 0.021, 5.2e-05, 0.32, -0.00164, 0.0018
		{ 0.00071, 0.0346, 0.00077, 0.0316, 0.000078, 0.48, 0.00246, 0.00264 },
		// 0.0022, 0.087, 0.0015, 0.064, 0.00044, 0.0144, -0.018, 0.0121
		{ 0.00323, 0.13, 0.0022, 0.096, 0.00067, 0.0217, 0.027, 0.0182 },
		// 0.026, 0.70, 0.0089, 0.157, 0.0058, 0.233, -0.486, 0.287
		{ 0.03, 1.05, 0.0135, 0.236, 0.0087, 0.35, 0.729, 0.431 },
		// 0.086, 0.96, 0.023, 0.356, 0.0554, 0.194, -0.518, 0.418
		{ 0.129, 1.45, 0.03, 0.535, 0.0832, 0.291, 0.776, 0.627 },
		// 0.063, 0.86, 0.028, 0.661, 0.0544, 0.0486, -0.411, 0.190
		{ 0.0942, 1.29, 0.03, 0.992, 0.0817, 0.0729, 0.616, 0.285 },
		// 0.035, 0.25, 0.029, 0.093, 0.0375, 0.0086, -0.247, 0.0758
		{ 0.0526, 0.371, 0.03, 0.10, 0.0563, 0.013, 0.370, 0.114 },
	};
}

TEST_SUITE("Renderer.AreaLights")
{
	TEST_CASE("The reference quadrature reproduces closed forms")
	{
		const glm::dvec3 down(0.0, 0.0, -1.0);
		const glm::dvec3 alongX(1.0, 0.0, 0.0);
		const glm::dvec3 alongY(0.0, 1.0, 0.0);

		// The cosine alone gives Howell's configuration factors: a panel overhead, a wall panel the receiver's plane cuts.
		const Rectangle overhead = MakeRectangle(glm::dvec3(0.4, -0.1, 0.8), down, alongX, 0.7, 0.5);
		const double overheadError = GetRelativeError(Integrate(Integrand::Cosine, 1.0, 1.0, overhead), GetParallelFactor(-0.3, 1.1, -0.6, 0.4, 0.8));
		const Rectangle wall = MakeRectangle(glm::dvec3(1.5, 0.2, 0.3), -alongX, alongY, 1.0, 0.9);
		const double wallError = GetRelativeError(Integrate(Integrand::Cosine, 1.0, 1.0, wall), GetPerpendicularFactor(-0.8, 1.2, -0.6, 1.2, 1.5));
		MESSAGE("quadrature against Howell: overhead " << overheadError << ", wall " << wallError);
		CHECK(overheadError <= 1e-6);
		CHECK(wallError <= 1e-5);

		// GGX at roughness 1 is D = 1 / pi and Vis = 1 / (2 (NoL + NoV)): under a sky that fills the hemisphere the lobe
		// sums to 1 - c ln(1 + 1 / c) for the view's cosine c.
		const Rectangle sky = MakeRectangle(glm::dvec3(0.0, 0.0, 1.0), down, alongX, 10000.0, 10000.0);
		double worstSky = 0.0;
		for (const double cosine : { 1.0, 0.5, 0.05 })
			worstSky = std::max(worstSky, GetRelativeError(Integrate(Integrand::Specular, 1.0, cosine, sky), 1.0 - cosine * std::log(1.0 + 1.0 / cosine)));
		MESSAGE("quadrature against GGX's sum at roughness 1: " << worstSky);
		CHECK(worstSky <= 1e-5);

		// A nearly smooth surface sends back nearly all it receives, and never more.
		const double sharp = Integrate(Integrand::Specular, 0.05, 1.0, sky);
		MESSAGE("quadrature of a sharp lobe under the sky: " << sharp);
		CHECK(sharp <= 1.0 + 1e-6);
		CHECK(sharp >= 0.999);

		// Cells half as wide change nothing that matters: the cutting rule has converged.
		double worstChange = 0.0;
		const size_t faces[] = { 0, 3, 9, 13, 14, 17, 21 };
		for (const double cosine : { 1.0, 0.3, 0.05 })
		{
			const std::vector<Rectangle> rectangles = GetSpecularRectangles(cosine);
			REQUIRE(rectangles.size() == c_SpecularFaces);
			for (const double roughness : { 0.05, 0.25, 1.0 })
			{
				for (const size_t face : faces)
				{
					LobeQuadrature coarse = MakeQuadrature(Integrand::Specular, roughness, cosine, rectangles[face]);
					LobeQuadrature fine = coarse;
					fine.Resolution = coarse.Resolution * 0.5;
					worstChange = std::max(worstChange, GetRelativeError(Integrate(coarse), Integrate(fine)));
				}
			}
		}
		MESSAGE("quadrature at half the cell width: worst relative change " << worstChange);
		// 1.5 times the measured 2.3e-5.
		CHECK(worstChange <= 3.5e-5);
	}

	TEST_CASE("The fitted lobe times the tables' magnitude against GGX over every roughness, view and face")
	{
		const SpecularCases& cases = GetSpecularCases();
		REQUIRE(cases.Cases.size() == std::size(c_SpecularRoughness) * c_CasesPerRoughness);
		REQUIRE(std::size(c_SpecularBounds) == std::size(c_SpecularRoughness));
		// A list the code under test leaves empty fails its bounds instead of being read past its end.
		CHECK(std::isnan(GetPercentile(std::span<const double>(), 0.95)));
		MESSAGE("reference quadrature: " << cases.Evaluations << " evaluations");
		for (size_t index = 0; index < std::size(c_SpecularRoughness); index++)
		{
			const float roughness = c_SpecularRoughness[index];
			CAPTURE(roughness);
			const std::span<const SpecularCase> entries = GetRoughnessCases(cases, index);
			const std::vector<double> shares = GetSpecularShares(cases, entries, false);
			const std::vector<double> errors = GetSpecularErrors(entries, shares);
			const std::span<const double> steep = std::span<const double>(errors).subspan(static_cast<size_t>(c_SteepViewStep) * c_SpecularFaces);
			const std::vector<double> mirror = GetMirrorErrors(errors);
			const std::vector<double> signedErrors = GetSignedSpecularErrors(entries, shares);
			const std::vector<double> holding = GetHoldingErrors(entries, errors);
			double worstShare = 0.0;
			size_t worstCase = 0;
			for (size_t entry = 0; entry < entries.size(); entry++)
			{
				if (std::abs(shares[entry] - entries[entry].Reference) > worstShare)
				{
					worstShare = std::abs(shares[entry] - entries[entry].Reference);
					worstCase = entry;
				}
			}
			MESSAGE("roughness " << roughness << ": median " << GetPercentile(errors, 0.5) << ", 95th " << GetPercentile(errors, 0.95) << ", worst " << GetPercentile(errors, 1.0)
				<< "; views from " << GetSpecularViewCosine(c_SteepViewStep) << ": median " << GetPercentile(steep, 0.5) << ", 95th " << GetPercentile(steep, 0.95)
				<< ", worst " << GetPercentile(steep, 1.0) << "; around the mirror direction: median " << GetPercentile(mirror, 0.5) << "; worst share difference "
				<< worstShare << " (face " << entries[worstCase].Face << " at view " << entries[worstCase].ViewCosine << ", GGX " << entries[worstCase].Reference << ")"
				<< "; with their sign: 5th " << GetPercentile(signedErrors, 0.05) << "; on the " << holding.size() << " faces that hold " << c_HoldingShare
				<< " of the lobe or more: 95th " << GetPercentile(holding, 0.95));
			// Reported: what the faces centered on the mirror direction get, against GGX, at the three lowest views (the
			// fitted lobe gives them more than GGX does at middling roughness; SpecularFraction's comment quotes it).
			constexpr int c_LowViews = 3;
			double lowest[c_LowViews];
			double highest[c_LowViews];
			for (int step = 0; step < c_LowViews; step++)
			{
				lowest[step] = std::numeric_limits<double>::max();
				highest[step] = 0.0;
				for (size_t face = 0; face < c_CenteredMirrorFaces; face++)
				{
					const size_t entry = static_cast<size_t>(step) * c_SpecularFaces + face;
					REQUIRE(entries[entry].Reference > 0.0);
					lowest[step] = std::min(lowest[step], shares[entry] / entries[entry].Reference);
					highest[step] = std::max(highest[step], shares[entry] / entries[entry].Reference);
				}
			}
			MESSAGE("roughness " << roughness << ": on the faces centered on the mirror direction the share is " << lowest[0] << " to " << highest[0]
				<< " times GGX's at view " << GetSpecularViewCosine(0) << ", " << lowest[1] << " to " << highest[1] << " at " << GetSpecularViewCosine(1) << ", "
				<< lowest[2] << " to " << highest[2] << " at " << GetSpecularViewCosine(2));

			const SpecularBounds& bounds = c_SpecularBounds[index];
			CHECK(GetPercentile(errors, 0.5) <= bounds.Median);
			CHECK(GetPercentile(errors, 0.95) <= bounds.Percentile);
			CHECK(GetPercentile(steep, 0.5) <= bounds.SteepMedian);
			CHECK(GetPercentile(steep, 0.95) <= bounds.SteepPercentile);
			CHECK(GetPercentile(mirror, 0.5) <= bounds.MirrorMedian);
			CHECK(worstShare <= bounds.Share);
			CHECK(GetPercentile(signedErrors, 0.05) >= -bounds.Under);
			CHECK(holding.size() >= 40);
			CHECK(GetPercentile(holding, 0.95) <= bounds.HoldingPercentile);
			CHECK(bounds.Under < 1.0);
			CHECK(bounds.HoldingPercentile < 1.0);
		}
	}

	TEST_CASE("Transposing the transform's terms takes the lobe away from GGX")
	{
		// The comparison's own check: read with its two off-diagonal terms swapped, the transform sends the lobe elsewhere
		// than the mirror direction, and the faces around that direction lose it. Roughness 1 is left out: its lobe is
		// nearly the cosine whichever way the two small terms are read.
		const SpecularCases& cases = GetSpecularCases();
		REQUIRE(cases.Cases.size() == std::size(c_SpecularRoughness) * c_CasesPerRoughness);
		for (size_t index = 0; index < std::size(c_SpecularRoughness); index++)
		{
			const float roughness = c_SpecularRoughness[index];
			CAPTURE(roughness);
			const std::span<const SpecularCase> entries = GetRoughnessCases(cases, index);
			const std::vector<double> errors = GetSpecularErrors(entries, GetSpecularShares(cases, entries, true));
			const std::vector<double> mirror = GetMirrorErrors(errors);
			MESSAGE("roughness " << roughness << " transposed: median " << GetPercentile(errors, 0.5) << ", around the mirror direction " << GetPercentile(mirror, 0.5));
			if (roughness < 1.0f)
				CHECK(GetPercentile(mirror, 0.5) > 0.3);
			// Over all the faces a sharp lobe misses most of them either way, so only the wider lobes show it there.
			if (roughness >= 0.25f && roughness < 1.0f)
				CHECK(GetPercentile(errors, 0.5) > 0.3);
		}
	}

	TEST_CASE("A view next to the normal turns the lobe's frame with it: half a sharp lobe against GGX")
	{
		// The tables' frame follows the view's direction along the surface, and only a view within 1e-4 rad of the normal,
		// which has none, gets a fixed one. A face with an edge through the mirror direction holds half of a sharp lobe, and
		// gains or loses much of it when the lobe leans another way than the view does. Each receiver is turned three ways
		// about its normal, so that the view cannot be along the fixed tangent every time.
		double worst = 0.0;
		for (const double angle : { 0.003, 0.05 })
		{
			const double viewCosine = std::cos(angle);
			const glm::dvec3 viewDirection(std::sin(angle), 0.0, viewCosine);
			const glm::dvec3 mirror(-viewDirection.x, 0.0, viewDirection.z);
			// The face's edge through the mirror direction lies across the view's plane, then in it.
			for (const glm::dvec3& toFace : { glm::dvec3(0.0, 1.0, 0.0), glm::dvec3(viewCosine, 0.0, std::sin(angle)) })
			{
				const Rectangle rectangle = MakeRectangle(mirror * 2.0 + toFace * 0.3, -mirror, toFace, 0.3, 0.5);
				for (const float roughness : { 0.05f, 0.1f })
				{
					const double reference = Integrate(Integrand::Specular, roughness, viewCosine, rectangle);
					const float magnitude = AreaLights::SampleTable(AreaLights::c_TermsLayer, glm::vec2(roughness, std::sqrt(1.0f - static_cast<float>(viewCosine)))).x;
					for (const double turn : { 0.4, 2.5, 4.6 })
					{
						CAPTURE(angle);
						CAPTURE(toFace.y);
						CAPTURE(roughness);
						CAPTURE(turn);
						Frame frame = MakeFrame();
						frame.Rotation = frame.Rotation * glm::dmat3(glm::rotate(glm::dmat4(1.0), turn, glm::dvec3(0.0, 0.0, 1.0)));
						const AreaLights::AreaLight light = MakeLight(frame, rectangle);
						const double share = AreaLights::SpecularFraction(frame.GetPoint(glm::dvec3(0.0)), frame.GetDirection(glm::dvec3(0.0, 0.0, 1.0)),
							frame.GetDirection(viewDirection), roughness, light) * magnitude;
						worst = std::max(worst, std::abs(share - reference));
						CHECK(reference > 0.4);
						CHECK(reference < 0.6);
						// 1.5 times the measured 0.0048, which is the tables' magnitude over GGX's sum: a lobe that leans the
						// fixed tangent's way is off by 0.01 to 0.35 at these views.
						CHECK(std::abs(share - reference) <= 0.0073);
					}
				}
			}
		}
		MESSAGE("near the normal: worst difference of a share from GGX's " << worst);
	}

	TEST_CASE("The tables' magnitude and Fresnel term beside GGX's sums under a sky")
	{
		// Reported, not asserted: the tables' second layer comes from the fit's own sum over 32 x 32 samples of the lobe
		// per entry, and the engine takes the highlight's energy from its own table, not from these.
		const Rectangle sky = MakeRectangle(glm::dvec3(0.0, 0.0, 1.0), glm::dvec3(0.0, 0.0, -1.0), glm::dvec3(1.0, 0.0, 0.0), 10000.0, 10000.0);
		for (const float roughness : { 0.1f, 0.25f, 0.5f, 1.0f })
		{
			for (const double cosine : { 1.0, 0.5, 0.05 })
			{
				const glm::vec4 terms = AreaLights::SampleTable(AreaLights::c_TermsLayer, glm::vec2(roughness, std::sqrt(1.0f - static_cast<float>(cosine))));
				const double magnitude = Integrate(Integrand::Specular, roughness, cosine, sky);
				const double fresnel = Integrate(Integrand::Fresnel, roughness, cosine, sky);
				MESSAGE("roughness " << roughness << " cosine " << cosine << ": magnitude " << terms.x << " against " << magnitude << ", Fresnel term " << terms.y << " against " << fresnel);
				CHECK(std::isfinite(magnitude));
			}
		}
	}
}
