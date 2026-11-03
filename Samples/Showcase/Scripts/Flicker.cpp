#include "Flicker.h"

#include <algorithm>
#include <cmath>

void Flicker::OnCreate()
{
	// Each flame starts at its own phase (entities' IDs differ), so two lanterns never flicker in step.
	m_Time = static_cast<float>(GetEntity().GetID() % 1000u) * 0.37f;
}

void Flicker::OnUpdate(float deltaTime)
{
	m_Time += std::max(deltaTime, 0.0f);
	float value = 0.0f;
	if (Intensity > 0.0f)
	{
		// Three waves at unrelated rates: smooth, never repeating within a shot, in [-1, 1].
		const float t = m_Time * Speed;
		const float noise = 0.5f * std::sin(t) + 0.3f * std::sin(t * 0.613f + 1.3f) + 0.2f * std::sin(t * 1.731f + 2.1f);
		value = Intensity * (1.0f + Amount * noise);
	}
	if (value == m_Written)
		return;
	m_Written = value;
	GetEntity().SetProperty("PointLight", "Intensity", value);
}

ST_SCRIPT_CLASS(Flicker)
{
	ST_SCRIPT_FIELD(Intensity);
	ST_SCRIPT_FIELD(Amount);
	ST_SCRIPT_FIELD(Speed);
}
