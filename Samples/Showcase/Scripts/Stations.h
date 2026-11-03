#pragma once

// The High Desert Outpost's times of day. A station is the sun's direction and the values that go with it: the tour shows
// Dawn, Golden and Dusk, each with its own two baked reflection probes, and the photo mode's time slider samples the day
// between them (SampleDay). No engine calls: Tools/GenerateShowcaseTerrain.py computes the shots from the same numbers, and
// its selftest (CTest StrataTools.ShowcaseTerrain) reads c_Stations' rows below, one line per station, and fails when they
// differ from its own table.

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace Showcase
{

	enum class StationId
	{
		Dawn,
		Golden,
		Dusk
	};

	struct StationValues
	{
		float Hour;           // The photo mode's clock (hours) at the station
		float Elevation;      // The sun's elevation (degrees above the horizon)
		float DrawnElevation; // Where the light is drawn: at least 2 degrees up, so its shadows stay drawable
		float Azimuth;        // Degrees from +X toward +Z of the direction toward the sun
		float LightScale;     // Times the sun's illuminance (the sky, lit by the sun, darkens with it)
		float FogDensity;     // The HeightFog's density
		float Exposure;       // The Post Process's exposure compensation (EV)
		float ShadowDistance; // The sun's shadow distance (m)
		bool LocalLights;     // The fire and the lanterns burn
	};

	constexpr std::array<StationValues, 3> c_Stations = { {
		{ 6.5f, 5.0f, 5.0f, 140.0f, 1.0f, 0.006f, 0.0f, 600.0f, false },
		{ 17.75f, 6.0f, 6.0f, 45.0f, 1.0f, 0.0006f, -0.7f, 200.0f, false },
		{ 19.0f, 1.0f, 2.0f, 32.0f, 0.002f, 0.0015f, -2.5f, 150.0f, true },
	} };

	constexpr const char* c_StationNames[] = { "Dawn", "Golden", "Dusk" };

	// The sun's illuminance (lux) at a light scale of 1: the GoldenHour preset's.
	constexpr float c_SunIlluminance = 120000.0f;
	// The earliest and latest hours of the photo mode's day.
	constexpr float c_FirstHour = 6.5f;
	constexpr float c_LastHour = 19.0f;

	inline const StationValues& GetStation(StationId station)
	{
		return c_Stations[static_cast<size_t>(station)];
	}

	inline const char* GetStationName(StationId station)
	{
		return c_StationNames[static_cast<size_t>(station)];
	}

	// The direction toward the sun: (cos e cos a, sin e, cos e sin a), as lighting.applySkyPreset computes it.
	inline glm::vec3 TowardSun(float elevationDegrees, float azimuthDegrees)
	{
		const float e = glm::radians(elevationDegrees);
		const float a = glm::radians(azimuthDegrees);
		return glm::vec3(std::cos(e) * std::cos(a), std::sin(e), std::cos(e) * std::sin(a));
	}

	// The station whose probes light an hour of the photo mode's day: the nearest knot of the tour's three.
	inline StationId NearestStation(float hour)
	{
		const float dawnToGolden = 0.5f * (c_Stations[0].Hour + c_Stations[1].Hour); // 12.125 h
		const float goldenToDusk = 0.5f * (c_Stations[1].Hour + c_Stations[2].Hour); // 18.375 h
		if (hour < dawnToGolden)
			return StationId::Dawn;
		return hour < goldenToDusk ? StationId::Golden : StationId::Dusk;
	}

	// The light of an hour of the day: piecewise linear between the knots Dawn, Noon, Golden and Dusk, the fog and the light's
	// scale in log (Dusk's light is a five-hundredth of Golden's: the sun on the horizon and the sky dark enough for the fire
	// and the lanterns to read), the light drawn at least 2 degrees up. The local lights burn from the hour the probes switch
	// to Dusk's.
	inline StationValues SampleDay(float hour)
	{
		struct Knot
		{
			float Hour, Elevation, Azimuth, LightScale, FogDensity, Exposure, ShadowDistance;
		};
		const std::array<Knot, 4> knots = { {
			{ c_Stations[0].Hour, c_Stations[0].Elevation, c_Stations[0].Azimuth, c_Stations[0].LightScale, c_Stations[0].FogDensity, c_Stations[0].Exposure,
				c_Stations[0].ShadowDistance },
			{ 12.0f, 55.0f, 92.0f, 1.0f, 0.0006f, 0.0f, 200.0f },
			{ c_Stations[1].Hour, c_Stations[1].Elevation, c_Stations[1].Azimuth, c_Stations[1].LightScale, c_Stations[1].FogDensity, c_Stations[1].Exposure,
				c_Stations[1].ShadowDistance },
			{ c_Stations[2].Hour, c_Stations[2].Elevation, c_Stations[2].Azimuth, c_Stations[2].LightScale, c_Stations[2].FogDensity, c_Stations[2].Exposure,
				c_Stations[2].ShadowDistance },
		} };
		const float clamped = std::clamp(hour, knots.front().Hour, knots.back().Hour);
		size_t segment = 0;
		while (segment + 2 < knots.size() && clamped > knots[segment + 1].Hour)
			segment++;
		const Knot& a = knots[segment];
		const Knot& b = knots[segment + 1];
		const float t = (clamped - a.Hour) / (b.Hour - a.Hour);
		StationValues values {};
		values.Hour = clamped;
		values.Elevation = a.Elevation + (b.Elevation - a.Elevation) * t;
		values.DrawnElevation = std::max(values.Elevation, 2.0f);
		values.Azimuth = a.Azimuth + (b.Azimuth - a.Azimuth) * t;
		const auto logLerp = [t](float from, float to) { return std::exp(std::log(from) + (std::log(to) - std::log(from)) * t); };
		values.LightScale = logLerp(a.LightScale, b.LightScale);
		values.FogDensity = logLerp(a.FogDensity, b.FogDensity);
		values.Exposure = a.Exposure + (b.Exposure - a.Exposure) * t;
		values.ShadowDistance = a.ShadowDistance + (b.ShadowDistance - a.ShadowDistance) * t;
		values.LocalLights = NearestStation(clamped) == StationId::Dusk;
		return values;
	}

}
