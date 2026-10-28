#pragma once

#include "Strata/Renderer/RendererComponents.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace Strata
{

	// The sun and sky presets (the editor's lighting.applySkyPreset and the Sky section's buttons): a sky light and a
	// directional light of real-world values. The values are command API: they never change meaning.
	enum class SkyPreset : uint8_t
	{
		Clear = 0,     // The physical atmosphere as a new sky has it, a 120,000 lux sun 40 degrees up: a clear day
		Hazy = 1,      // Thicker air and four times the aerosols, the sun 35 degrees up: a paler sky, softer contrast
		Overcast = 2,  // A near-uniform procedural sky of about 7,700 lux on the ground and a faint, very soft sun
		GoldenHour = 3 // The atmosphere with a little haze, the sun 6 degrees up: warm light and long shadows
	};

	constexpr uint32_t c_SkyPresetCount = 4;

	// "Clear", "Hazy", "Overcast", "GoldenHour".
	const char* SkyPresetToString(SkyPreset preset);
	std::optional<SkyPreset> SkyPresetFromString(std::string_view name);

	struct SkyPresetValues
	{
		SkyLightSource Source = SkyLightSource::Atmosphere;
		float SkyIntensity = 1.0f;
		// Atmosphere presets: SkyLightComponent's AtmosphereDensity, AtmosphereHaze and GroundAlbedo.
		float AtmosphereDensity = 1.0f;
		float AtmosphereHaze = 1.0f;
		float GroundAlbedo = 0.3f;
		// Procedural presets: the colors in nits and the sun disk's SunIntensity.
		glm::vec3 ZenithColor = glm::vec3(0.0f);
		glm::vec3 HorizonColor = glm::vec3(0.0f);
		glm::vec3 GroundColor = glm::vec3(0.0f);
		float SunDiskIntensity = 0.0f;
		float SunSize = 0.53f;          // Degrees across
		float SunElevation = 40.0f;     // Degrees above the horizon
		float SunIlluminance = 120000.0f; // DirectionalLightComponent::Intensity: lux, above the air for the atmosphere
		glm::vec3 SunColor = glm::vec3(1.0f);
		float ShadowSoftness = 0.53f;   // Degrees
	};

	const SkyPresetValues& GetSkyPresetValues(SkyPreset preset);

	// The unit direction toward the sun at `elevationDegrees` above the horizon and `azimuthDegrees` from +X toward +Z.
	glm::vec3 GetSkyPresetSunDirection(float elevationDegrees, float azimuthDegrees);

}
