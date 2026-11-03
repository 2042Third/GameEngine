#pragma once

// A flame's light: on an entity with a PointLight, it writes the light's intensity as Intensity times a slow, smooth
// flicker of +-Amount. Outpost sets Intensity per time of day (0 puts the light out: a light at 0 cd is neither drawn nor
// shadowed).

#include "StrataScript/StrataScript.h"

class Flicker : public Strata::Script
{
public:
	float Intensity = 0.0f; // Candela the light burns at on average
	float Amount = 0.15f;   // The flicker's depth, a fraction of Intensity
	float Speed = 9.0f;     // How fast it flickers (radians of its fastest wave per second)

	void OnCreate() override;
	void OnUpdate(float deltaTime) override;
private:
	float m_Time = 0.0f;
	float m_Written = -1.0f;
};
