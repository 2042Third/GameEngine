// Plays a sound when its body hits something: wood, metal or soft by Kind, louder the harder the two bodies met, heard
// where they met. Impacts share a budget (a pour starts hundreds of contacts at once): at most c_MaxSoundsPerFrame in a
// frame and c_SoundsPerSecond on average, so a few voices play at a time. The sounds are found by path
// (Sounds/Impacts/...), so a pruned export keeps them through the project's alwaysInclude.

#include "StrataScript/StrataScript.h"

#include <algorithm>
#include <cstdint>

using namespace Strata;

namespace
{

	constexpr int32_t c_MaxSoundsPerFrame = 8;
	constexpr float c_SoundsPerSecond = 16.0f;
	constexpr const char* c_SoundPaths[] = { "Sounds/Impacts/Wood.ogg", "Sounds/Impacts/Metal.ogg", "Sounds/Impacts/Soft.ogg" };

	// One budget for every body of the module: tokens that refill at c_SoundsPerSecond of real time, up to a frame's worth.
	float s_Tokens = static_cast<float>(c_MaxSoundsPerFrame);
	double s_LastRealTime = 0.0;

	bool TakeSoundBudget()
	{
		const double now = Time::GetRealTime();
		const float elapsed = static_cast<float>(std::max(0.0, now - s_LastRealTime));
		s_Tokens = std::min(static_cast<float>(c_MaxSoundsPerFrame), s_Tokens + elapsed * c_SoundsPerSecond);
		s_LastRealTime = now;
		if (s_Tokens < 1.0f)
			return false;
		s_Tokens -= 1.0f;
		return true;
	}

}

class Impact : public Script
{
public:
	int32_t Kind = 0;            // 0 wood, 1 metal, 2 soft
	float MinSpeed = 1.5f;       // m/s of the two bodies toward each other below which nothing sounds
	float FullVolumeSpeed = 10.0f;

	void OnCreate() override { FindSound(); }
	void OnReload() override { FindSound(); }

	void OnCollisionEnter(const Collision& collision) override
	{
		// The contact is reported after the step that resolved it, so the bodies already move apart: how fast they move
		// along the contact's normal now (a bounce keeps most of the impact's speed, a thud little), plus how fast either
		// still moves, tells a hard hit from a touch without tracking every body's velocity every step.
		const glm::vec3 own = GetEntity().GetRigidBody().GetLinearVelocity();
		const glm::vec3 other = collision.Other.IsValid() ? collision.Other.GetRigidBody().GetLinearVelocity() : glm::vec3(0.0f);
		const float separating = std::abs(glm::dot(own - other, collision.Normal));
		const float speed = separating + 0.5f * std::max(glm::length(own), glm::length(other));
		if (speed < MinSpeed || !m_Sound.IsValid() || !TakeSoundBudget())
			return;
		const float volume = std::clamp(speed / FullVolumeSpeed, 0.1f, 1.0f);
		Audio::PlayOneShotAt(m_Sound, collision.Point, volume, 0.9f + 0.2f * std::min(1.0f, speed / FullVolumeSpeed));
	}
private:
	void FindSound()
	{
		m_Sound = Assets::Find(c_SoundPaths[std::clamp(Kind, 0, 2)]);
		Assets::RequestLoad(m_Sound);
	}
private:
	AssetHandle m_Sound;
};

ST_SCRIPT_CLASS(Impact)
{
	ST_SCRIPT_FIELD(Kind);
	ST_SCRIPT_FIELD(MinSpeed);
	ST_SCRIPT_FIELD(FullVolumeSpeed);
}
