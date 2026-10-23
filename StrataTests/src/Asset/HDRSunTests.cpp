#include <doctest/doctest.h>

#include "Asset/SyntheticSky.h"
#include "Strata/Asset/HDRSun.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	constexpr uint32_t c_Width = 2048;
	constexpr uint32_t c_Height = 1024;

	float Luminance(const float* texel)
	{
		return 0.2126f * texel[0] + 0.7152f * texel[1] + 0.0722f * texel[2];
	}

	double AngleDegrees(const glm::vec3& a, const glm::vec3& b)
	{
		const double chord = glm::length(glm::dvec3(glm::normalize(a)) - glm::dvec3(glm::normalize(b)));
		return 2.0 * std::asin(std::min(chord * 0.5, 1.0)) * 180.0 / std::numbers::pi;
	}

	// A direction `elevation` degrees above the horizon, `azimuth` degrees around +Y from +X toward +Z.
	glm::vec3 Toward(float elevation, float azimuth)
	{
		const float e = glm::radians(elevation);
		const float a = glm::radians(azimuth);
		return glm::vec3(std::cos(e) * std::cos(a), std::sin(e), std::cos(e) * std::sin(a));
	}

}

TEST_SUITE("Asset.HDRSun")
{
	TEST_CASE("Texel directions follow the equirectangular mapping the renderer samples, and solid angles add up to the sphere")
	{
		// u = atan(z, x) / 2 pi + 0.5 (Include/IBL.glsl): the middle column looks along +X, the first one toward -X; the top
		// row looks up.
		CHECK(AngleDegrees(HDRSun::TexelDirection(c_Width / 2, c_Height / 2, c_Width, c_Height), glm::vec3(1.0f, 0.0f, 0.0f)) < 0.2);
		CHECK(AngleDegrees(HDRSun::TexelDirection(0, c_Height / 2, c_Width, c_Height), glm::vec3(-1.0f, 0.0f, 0.0f)) < 0.2);
		CHECK(AngleDegrees(HDRSun::TexelDirection(3 * c_Width / 4, c_Height / 2, c_Width, c_Height), glm::vec3(0.0f, 0.0f, 1.0f)) < 0.2);
		CHECK(HDRSun::TexelDirection(5, 0, c_Width, c_Height).y > 0.9999f);
		double total = 0.0;
		for (uint32_t y = 0; y < 64; y++)
			total += HDRSun::TexelSolidAngle(y, 128, 64) * 128.0;
		CHECK(total == doctest::Approx(4.0 * std::numbers::pi).epsilon(1.0e-9));
	}

	TEST_CASE("A sun's direction comes within 0.1 degree and its illuminance within 1%, also across the seam and over a pole")
	{
		struct Case
		{
			const char* Name;
			glm::vec3 Direction;
		};
		const Case cases[] = {
			{ "afternoon", Toward(35.0f, -120.0f) },
			// Azimuth 180 degrees: the disk straddles the first and the last column.
			{ "across the seam", Toward(20.0f, 180.0f) },
			// Its disk reaches over the zenith into the rows on the far side.
			{ "at the zenith", Toward(89.8f, 30.0f) },
		};
		for (const Case& test : cases)
		{
			CAPTURE(test.Name);
			Tests::SyntheticSun sun;
			sun.Direction = test.Direction;
			const std::vector<float> sky = Tests::MakeSyntheticSky(c_Width, c_Height, sun);
			std::string reason;
			const std::optional<HDRSun::Sun> found = HDRSun::Find(sky, c_Width, c_Height, &reason);
			REQUIRE_MESSAGE(found, reason);
			const double expected = Tests::SyntheticSunIlluminance(sun);
			MESSAGE(std::string(test.Name), ": direction off by ", AngleDegrees(found->Direction, sun.Direction), " degrees, illuminance ", found->Illuminance.r / expected,
				" times the disk's, ", found->Region.size(), " texels");
			CHECK(AngleDegrees(found->Direction, sun.Direction) < 0.1);
			for (int channel = 0; channel < 3; channel++)
				CHECK(found->Illuminance[channel] == doctest::Approx(expected * sun.Color[channel]).epsilon(0.01));
			CHECK(found->Color.r == doctest::Approx(1.0f));
			CHECK(found->Color.b == doctest::Approx(sun.Color.b).epsilon(0.01));
			CHECK_FALSE(found->Clipped);
			CHECK(found->AngularDiameter >= 2.0f * sun.RadiusDegrees * 0.9f);
			CHECK(found->AngularDiameter <= HDRSun::c_MaxDiameter + 0.2f);
			CHECK(found->Background > 0.5f);
			CHECK(found->Background < 4.0f);
			CHECK(found->Peak > found->Threshold);
		}
	}

	TEST_CASE("A flat peak is reported as clipped, its illuminance a lower bound")
	{
		Tests::SyntheticSun sun;
		sun.Direction = Toward(40.0f, 60.0f);
		sun.RadiusDegrees = 1.0f;
		sun.ClipAt = 0.25f * sun.Radiance;
		const std::vector<float> sky = Tests::MakeSyntheticSky(c_Width, c_Height, sun);
		const std::optional<HDRSun::Sun> found = HDRSun::Find(sky, c_Width, c_Height);
		REQUIRE(found);
		CHECK(found->Clipped);
		CHECK(AngleDegrees(found->Direction, sun.Direction) < 0.1);
		CHECK(found->Illuminance.r == doctest::Approx(Tests::SyntheticSunIlluminance(sun)).epsilon(0.01));
	}

	TEST_CASE("A sky without a sun has none: nothing bright, or nothing much brighter than the sky around it")
	{
		std::string reason;
		const std::vector<float> black(static_cast<size_t>(64) * 32 * 4, 0.0f);
		CHECK_FALSE(HDRSun::Find(black, 64, 32, &reason));
		CHECK(reason.find("no bright texel") != std::string::npos);

		// A dim sun only twice as bright as its sky.
		Tests::SyntheticSun sun;
		sun.Radiance = 6.0f;
		const std::vector<float> overcast = Tests::MakeSyntheticSky(512, 256, sun);
		CHECK_FALSE(HDRSun::Find(overcast, 512, 256, &reason));
		CHECK(reason.find("times as bright as the sky around it") != std::string::npos);

		// Fewer floats than texels, and no texels.
		CHECK_FALSE(HDRSun::Find(std::span<const float>(black.data(), 10), 64, 32, &reason));
		CHECK_FALSE(HDRSun::Find({}, 0, 0, &reason));

		// Texels that are not finite or negative count as black around a real sun.
		std::vector<float> sky = Tests::MakeSyntheticSky(512, 256, Tests::SyntheticSun {});
		sky[0] = std::numeric_limits<float>::infinity();
		sky[5] = std::numeric_limits<float>::quiet_NaN();
		sky[10] = -1.0e9f;
		const std::optional<HDRSun::Sun> found = HDRSun::Find(sky, 512, 256);
		REQUIRE(found);
		CHECK(std::isfinite(found->Luminance));
		CHECK(AngleDegrees(found->Direction, Tests::SyntheticSun {}.Direction) < 0.2);
	}

	TEST_CASE("Removing the sun leaves no texel above the threshold and the sky around it as it was")
	{
		for (const glm::vec3 direction : { Toward(35.0f, -120.0f), Toward(20.0f, 180.0f), Toward(89.8f, 30.0f) })
		{
			CAPTURE(direction);
			Tests::SyntheticSun sun;
			sun.Direction = direction;
			constexpr uint32_t c_RemoveWidth = c_Width / 2;
			constexpr uint32_t c_RemoveHeight = c_Height / 2;
			std::vector<float> sky = Tests::MakeSyntheticSky(c_RemoveWidth, c_RemoveHeight, sun);
			const std::vector<float> before = sky;
			const std::optional<HDRSun::Sun> found = HDRSun::Find(sky, c_RemoveWidth, c_RemoveHeight);
			REQUIRE(found);
			HDRSun::Remove(sky, c_RemoveWidth, c_RemoveHeight, *found);
			float brightest = 0.0f;
			for (size_t texel = 0; texel < static_cast<size_t>(c_RemoveWidth) * c_RemoveHeight; texel++)
				brightest = std::max(brightest, Luminance(&sky[texel * 4]));
			CHECK(brightest <= found->Threshold);
			// The filled texels are no darker than the sky there (the texels around them, some of the disk's edge among them, may
			// be brighter) and keep a hundredth of the sun's light at most; the others did not change, alpha neither.
			double left = 0.0;
			for (const uint32_t texel : found->Region)
			{
				const uint32_t x = texel % c_RemoveWidth;
				const uint32_t y = texel / c_RemoveWidth;
				const glm::vec3 expected = Tests::SyntheticSkyRadiance(HDRSun::TexelDirection(x, y, c_RemoveWidth, c_RemoveHeight));
				const float luminance = Luminance(&sky[static_cast<size_t>(texel) * 4]);
				CHECK(luminance > 0.5f * (0.2126f * expected.r + 0.7152f * expected.g + 0.0722f * expected.b));
				CHECK(sky[static_cast<size_t>(texel) * 4 + 3] == 1.0f);
				left += luminance * HDRSun::TexelSolidAngle(y, c_RemoveWidth, c_RemoveHeight);
			}
			CHECK(left < 0.01 * found->Luminance);
			size_t changed = 0;
			for (size_t index = 0; index < sky.size(); index++)
				changed += sky[index] != before[index] ? 1 : 0;
			CHECK(changed <= found->Region.size() * 3);
			// Nothing left to find.
			CHECK_FALSE(HDRSun::Find(sky, c_RemoveWidth, c_RemoveHeight));
		}
	}

	TEST_CASE("Decoding reads Radiance HDR files as linear floats and rejects garbage")
	{
		const std::vector<float> sky = Tests::MakeSyntheticSky(256, 128, Tests::SyntheticSun {});
		const std::vector<uint8_t> encoded = Tests::EncodeSyntheticSky(256, 128, sky);
		REQUIRE_FALSE(encoded.empty());
		std::string error;
		const std::optional<HDRSun::Image> image = HDRSun::Decode(encoded, &error);
		REQUIRE_MESSAGE(image, error);
		CHECK(image->Width == 256);
		CHECK(image->Height == 128);
		const std::span<const float> pixels = image->GetPixels();
		REQUIRE(pixels.size() == sky.size());
		// RGBE keeps about 1% of each value.
		CHECK(pixels[4 * 1000] == doctest::Approx(sky[4 * 1000]).epsilon(0.01));
		CHECK(pixels[4 * 1000 + 3] == 1.0f);

		const std::vector<uint8_t> garbage = { 1, 2, 3, 4, 5, 6, 7, 8 };
		CHECK_FALSE(HDRSun::Decode(garbage, &error));
		CHECK_FALSE(error.empty());
		CHECK_FALSE(HDRSun::Decode({}, &error));
	}
}
