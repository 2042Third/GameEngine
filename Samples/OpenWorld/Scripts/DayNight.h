// Turns the sun (the entity it is on, with a DirectionalLight) through a day of DayLengthSeconds, in steps of at most
// StepDegrees so the shadow cascades are drawn again only when it moved. The sky is the atmosphere (the "Sky" entity's
// SkyLight), which colors itself and the light from where it stands: blue by day, red at sunset, the light dimmed and
// reddened near the horizon. At night the same light is the moon, opposite the sun and far dimmer, so the atmosphere
// shows a dark moonlit sky instead of none. Campfires (entities tagged "Campfire" with a PointLight) light up at dusk and
// go out after dawn.

#pragma once

#include "StrataScript/StrataScript.h"

#include <algorithm>
#include <cmath>
#include <string>

class DayNight : public Strata::Script
{
public:
	float DayLengthSeconds = 600.0f; // A whole day
	float TimeOfDay = 9.0f;          // Hours, 0 to 24 (6 sunrise, 18 sunset)
	float StepDegrees = 0.25f;       // The sun turns in steps of at most this much
	float Azimuth = 35.0f;           // Degrees: where the sun crosses the sky
	float SunIntensity = 3.0f;       // Lux above the atmosphere, which dims it on its way down
	float MoonIntensity = 0.05f;     // Lux: the light at night
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

		const float height = std::sin(glm::radians(angle)); // The sun's: 1 at noon, below 0 at night
		// By day the sun, at night the moon across the sky from it (its elevation the sun's depth below the horizon).
		const bool night = height < 0.0f;
		const float lightAngle = night ? angle - 180.0f : angle;
		// The light shines along its -Z: turned toward the azimuth, then down by the elevation. It stays at least 2 degrees
		// up (its shadows stay short enough to draw) and fades out from there to the horizon instead, where the atmosphere
		// has already turned it a dim red, and the sky, lit by it, darkens with it.
		const glm::quat rotation = glm::angleAxis(glm::radians(Azimuth), glm::vec3(0.0f, 1.0f, 0.0f))
			* glm::angleAxis(glm::radians(-std::clamp(lightAngle, 2.0f, 178.0f)), glm::vec3(1.0f, 0.0f, 0.0f));
		GetTransform().SetRotation(rotation);
		const float fade = std::clamp(std::sin(glm::radians(lightAngle)) / 0.035f, 0.0f, 1.0f);
		GetEntity().SetProperty("DirectionalLight", "Intensity", (night ? MoonIntensity : SunIntensity) * fade);

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
