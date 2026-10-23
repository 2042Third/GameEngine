#pragma once

#include <glm/glm.hpp>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

// Equirectangular HDR skies with a sun of known direction, size and radiance (HDRSun's mapping), for the tests of sun
// extraction: Asset.HDRSun, Asset.TextureImport and GPU.Editor.Lighting.
namespace Strata::Tests
{

	struct SyntheticSun
	{
		glm::vec3 Direction = glm::normalize(glm::vec3(0.4f, 0.6f, -0.7f)); // Toward the sun
		float RadiusDegrees = 0.6f;
		float Radiance = 50000.0f;                     // Nits at the disk's center
		glm::vec3 Color = glm::vec3(1.0f, 0.95f, 0.88f); // Times Radiance
		// The disk darkens toward its edge as the real sun does: Radiance * (1 - LimbDarkening * (1 - cos)), cos the
		// cosine of the angle on the sun's surface. 0 makes it uniform.
		float LimbDarkening = 0.6f;
		// Radiance above this is cut (a clipped capture: a flat peak); 0 cuts nothing.
		float ClipAt = 0.0f;
	};

	// The sky behind the sun: brighter toward the zenith, a dim ground below the horizon.
	inline glm::vec3 SyntheticSkyRadiance(const glm::vec3& direction)
	{
		return glm::vec3(0.6f, 0.8f, 1.0f) * (1.0f + 2.0f * std::max(direction.y, 0.0f)) * (direction.y < 0.0f ? 0.3f : 1.0f);
	}

	// The sun's radiance along a direction (0 off its disk).
	inline float SyntheticSunRadiance(const SyntheticSun& sun, const glm::dvec3& direction)
	{
		const double angle = 2.0 * std::asin(std::min(glm::length(direction - glm::dvec3(glm::normalize(sun.Direction))) * 0.5, 1.0));
		const double radius = sun.RadiusDegrees * std::numbers::pi / 180.0;
		if (angle >= radius)
			return 0.0f;
		const double ratio = angle / radius;
		const double cosine = std::sqrt(1.0 - ratio * ratio);
		const float radiance = static_cast<float>(sun.Radiance * (1.0 - sun.LimbDarkening * (1.0 - cosine)));
		return sun.ClipAt > 0.0f ? std::min(radiance, sun.ClipAt) : radiance;
	}

	// The disk's illuminance per unit of Color: its radiance integrated over its solid angle.
	inline double SyntheticSunIlluminance(const SyntheticSun& sun)
	{
		constexpr int c_Steps = 20000;
		const double radius = sun.RadiusDegrees * std::numbers::pi / 180.0;
		double sum = 0.0;
		for (int step = 0; step < c_Steps; step++)
		{
			const double angle = (static_cast<double>(step) + 0.5) / c_Steps * radius;
			const glm::dvec3 axis = glm::normalize(glm::dvec3(sun.Direction));
			// Any direction at that angle from the axis: the disk is round.
			const glm::dvec3 side = glm::normalize(glm::cross(axis, std::abs(axis.y) < 0.9 ? glm::dvec3(0.0, 1.0, 0.0) : glm::dvec3(1.0, 0.0, 0.0)));
			const glm::dvec3 direction = axis * std::cos(angle) + side * std::sin(angle);
			sum += SyntheticSunRadiance(sun, direction) * std::sin(angle);
		}
		return 2.0 * std::numbers::pi * sum * radius / c_Steps;
	}

	// RGBA floats, rows from the top. Texels near the sun average 8 x 8 samples weighted by their solid angle, so that each
	// texel holds its share of the disk's light.
	inline std::vector<float> MakeSyntheticSky(uint32_t width, uint32_t height, const SyntheticSun& sun)
	{
		constexpr uint32_t c_Samples = 8;
		const glm::dvec3 axis = glm::normalize(glm::dvec3(sun.Direction));
		const double sunReach = (sun.RadiusDegrees + 4.0 * 180.0 / height) * std::numbers::pi / 180.0;
		std::vector<float> pixels(static_cast<size_t>(width) * height * 4);
		const auto direction = [&](double x, double y)
		{
			const double azimuth = (x / width - 0.5) * 2.0 * std::numbers::pi;
			const double polar = y / height * std::numbers::pi;
			return glm::dvec3(std::sin(polar) * std::cos(azimuth), std::cos(polar), std::sin(polar) * std::sin(azimuth));
		};
		for (uint32_t y = 0; y < height; y++)
		{
			for (uint32_t x = 0; x < width; x++)
			{
				const glm::dvec3 center = direction(x + 0.5, y + 0.5);
				glm::dvec3 color(SyntheticSkyRadiance(glm::vec3(center)));
				if (2.0 * std::asin(std::min(glm::length(center - axis) * 0.5, 1.0)) < sunReach)
				{
					glm::dvec3 sum(0.0);
					double weights = 0.0;
					for (uint32_t j = 0; j < c_Samples; j++)
					{
						const double sampleY = y + (j + 0.5) / c_Samples;
						const double weight = std::sin(sampleY / height * std::numbers::pi);
						for (uint32_t i = 0; i < c_Samples; i++)
						{
							const glm::dvec3 sample = direction(x + (i + 0.5) / c_Samples, sampleY);
							sum += (glm::dvec3(SyntheticSkyRadiance(glm::vec3(sample))) + glm::dvec3(sun.Color) * static_cast<double>(SyntheticSunRadiance(sun, sample))) * weight;
							weights += weight;
						}
					}
					color = sum / weights;
				}
				float* texel = &pixels[(static_cast<size_t>(y) * width + x) * 4];
				texel[0] = static_cast<float>(color.r);
				texel[1] = static_cast<float>(color.g);
				texel[2] = static_cast<float>(color.b);
				texel[3] = 1.0f;
			}
		}
		return pixels;
	}

	// The sky as a Radiance HDR file (RGB; alpha dropped).
	inline std::vector<uint8_t> EncodeSyntheticSky(uint32_t width, uint32_t height, const std::vector<float>& rgba)
	{
		std::vector<float> rgb;
		rgb.reserve(static_cast<size_t>(width) * height * 3);
		for (size_t texel = 0; texel < static_cast<size_t>(width) * height; texel++)
			rgb.insert(rgb.end(), rgba.begin() + static_cast<std::ptrdiff_t>(texel * 4), rgba.begin() + static_cast<std::ptrdiff_t>(texel * 4 + 3));
		std::vector<uint8_t> encoded;
		auto write = [](void* context, void* data, int size)
		{
			auto* output = static_cast<std::vector<uint8_t>*>(context);
			output->insert(output->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
		};
		if (stbi_write_hdr_to_func(write, &encoded, static_cast<int>(width), static_cast<int>(height), 3, rgb.data()) == 0)
			encoded.clear();
		return encoded;
	}

}
