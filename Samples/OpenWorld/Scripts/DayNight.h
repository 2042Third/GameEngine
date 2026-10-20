// Turns the sun (the entity it is on, with a DirectionalLight) through a day of DayLengthSeconds, in steps of at most
// StepDegrees so the shadow cascades are drawn again only when it moved, and colors the procedural sky to match (its
// colors are the SkyLight's properties, constant unless a script drives them). Campfires (entities tagged "Campfire" with
// a PointLight) light up at dusk and go out after dawn.

#pragma once

#include "StrataScript/StrataScript.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace DayNightDetail
{

	inline glm::vec3 Mix(const glm::vec3& a, const glm::vec3& b, float t) { return a + (b - a) * std::clamp(t, 0.0f, 1.0f); }

}

class DayNight : public Strata::Script
{
public:
	float DayLengthSeconds = 600.0f; // A whole day
	float TimeOfDay = 9.0f;          // Hours, 0 to 24 (6 sunrise, 18 sunset)
	float StepDegrees = 0.25f;       // The sun turns in steps of at most this much
	float Azimuth = 35.0f;           // Degrees: where the sun crosses the sky
	float SunIntensity = 3.0f;       // At noon
	float CampfireIntensity = 6.0f;
	bool CampfiresLit = false;

	void OnCreate() override { Apply(true); }
	void OnReload() override { Apply(true); }

	void OnUpdate(float deltaTime) override
	{
		TimeOfDay = std::fmod(TimeOfDay + deltaTime * 24.0f / std::max(DayLengthSeconds, 1.0f), 24.0f);
		Apply(false);
	}

	// The sun's elevation in degrees: -90 at midnight, 90 at noon.
	float GetElevation() const { return (TimeOfDay - 6.0f) / 12.0f * 180.0f; }
private:
	void Apply(bool force)
	{
		const float angle = GetElevation();
		if (!force && std::abs(angle - m_AppliedAngle) < StepDegrees)
			return;
		m_AppliedAngle = angle;

		// The light shines along its -Z: turned toward the azimuth, then down by the elevation.
		const glm::quat rotation = glm::angleAxis(glm::radians(Azimuth), glm::vec3(0.0f, 1.0f, 0.0f))
			* glm::angleAxis(glm::radians(-std::clamp(angle, 2.0f, 178.0f)), glm::vec3(1.0f, 0.0f, 0.0f));
		GetTransform().SetRotation(rotation);
		const float height = std::sin(glm::radians(angle));   // 1 at noon, below 0 at night
		const float day = std::clamp(height * 4.0f, 0.0f, 1.0f); // Full light once the sun is 15 degrees up
		const float dusk = std::clamp(1.0f - std::abs(height) * 5.0f, 0.0f, 1.0f); // Near the horizon
		Strata::Entity entity = GetEntity();
		entity.SetProperty("DirectionalLight", "Intensity", SunIntensity * day);
		entity.SetProperty("DirectionalLight", "Color", DayNightDetail::Mix(glm::vec3(1.0f), glm::vec3(1.0f, 0.55f, 0.3f), dusk));

		if (Strata::Entity sky = Strata::Scene::FindEntityByName("Sky"); sky.IsValid())
		{
			const glm::vec3 nightZenith(0.01f, 0.015f, 0.04f);
			const glm::vec3 nightHorizon(0.02f, 0.03f, 0.06f);
			const glm::vec3 dayZenith(0.15f, 0.32f, 0.75f);
			const glm::vec3 dayHorizon(0.65f, 0.75f, 0.9f);
			const glm::vec3 duskHorizon(0.95f, 0.45f, 0.2f);
			sky.SetProperty("SkyLight", "ZenithColor", DayNightDetail::Mix(nightZenith, dayZenith, day));
			sky.SetProperty("SkyLight", "HorizonColor", DayNightDetail::Mix(DayNightDetail::Mix(nightHorizon, dayHorizon, day), duskHorizon, dusk * 0.8f));
			sky.SetProperty("SkyLight", "Intensity", 0.15f + 0.85f * day);
		}

		const bool lit = height < 0.1f; // From just before sunset to just after sunrise
		if (lit != CampfiresLit || force)
		{
			CampfiresLit = lit;
			for (Strata::Entity fire : Strata::Scene::FindEntitiesByTag("Campfire"))
				fire.SetProperty("PointLight", "Intensity", lit ? CampfireIntensity : 0.0f);
			if (!force)
				Strata::Log::Info("OpenWorld: campfires ", lit ? "lit" : "out", " at ", static_cast<int32_t>(TimeOfDay), ":00");
		}
	}
private:
	float m_AppliedAngle = -1000.0f;
};
