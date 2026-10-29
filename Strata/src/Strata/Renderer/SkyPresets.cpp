#include "stpch.h"
#include "Strata/Renderer/SkyPresets.h"

#include <array>
#include <cmath>

namespace Strata
{

	namespace
	{

		struct SkyPresetEntry
		{
			SkyPreset Preset;
			const char* Name;
			SkyPresetValues Values;
		};

		SkyPresetValues Atmosphere(float density, float haze, float elevation)
		{
			SkyPresetValues values;
			values.Source = SkyLightSource::Atmosphere;
			values.AtmosphereDensity = density;
			values.AtmosphereHaze = haze;
			values.GroundAlbedo = 0.3f;
			values.SunElevation = elevation;
			// The sun above the air: the atmosphere dims and reddens it on its way down (about 95,000 lux 40 degrees up).
			values.SunIlluminance = 120000.0f;
			return values;
		}

		SkyPresetValues Overcast()
		{
			// The atmosphere has no clouds (its haze is Earth's clear aerosol): an overcast sky is a near-uniform procedural one
			// of about 7,700 lux on flat ground, over a faint sun 60 degrees up as soft as the light allows (its diameter's
			// maximum), whose disk is not drawn.
			SkyPresetValues values;
			values.Source = SkyLightSource::Procedural;
			values.ZenithColor = glm::vec3(2600.0f, 2650.0f, 2750.0f);
			values.HorizonColor = glm::vec3(2200.0f, 2250.0f, 2300.0f);
			values.GroundColor = glm::vec3(400.0f, 380.0f, 360.0f);
			values.SunDiskIntensity = 0.0f;
			values.SunElevation = 60.0f;
			values.SunIlluminance = 1500.0f;
			values.ShadowSoftness = 10.0f;
			return values;
		}

		const std::array<SkyPresetEntry, c_SkyPresetCount>& GetEntries()
		{
			static const std::array<SkyPresetEntry, c_SkyPresetCount> s_Entries = { {
				{ SkyPreset::Clear, "Clear", Atmosphere(1.0f, 1.0f, 40.0f) },
				{ SkyPreset::Hazy, "Hazy", Atmosphere(1.2f, 4.0f, 35.0f) },
				{ SkyPreset::Overcast, "Overcast", Overcast() },
				{ SkyPreset::GoldenHour, "GoldenHour", Atmosphere(1.0f, 1.5f, 6.0f) }
			} };
			return s_Entries;
		}

	}

	const char* SkyPresetToString(SkyPreset preset)
	{
		for (const SkyPresetEntry& entry : GetEntries())
		{
			if (entry.Preset == preset)
				return entry.Name;
		}
		return "Clear";
	}

	std::optional<SkyPreset> SkyPresetFromString(std::string_view name)
	{
		for (const SkyPresetEntry& entry : GetEntries())
		{
			if (name == entry.Name)
				return entry.Preset;
		}
		return std::nullopt;
	}

	const SkyPresetValues& GetSkyPresetValues(SkyPreset preset)
	{
		for (const SkyPresetEntry& entry : GetEntries())
		{
			if (entry.Preset == preset)
				return entry.Values;
		}
		return GetEntries()[0].Values;
	}

	glm::vec3 GetSkyPresetSunDirection(float elevationDegrees, float azimuthDegrees)
	{
		const float elevation = glm::radians(elevationDegrees);
		const float azimuth = glm::radians(azimuthDegrees);
		return glm::vec3(std::cos(elevation) * std::cos(azimuth), std::sin(elevation), std::cos(elevation) * std::sin(azimuth));
	}

}
