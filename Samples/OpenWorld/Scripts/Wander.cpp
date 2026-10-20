// A fox that wanders around where it was placed: it surveys, walks or runs to a random spot within Range of its home
// (inside its own world cell, so it never walks into one that is not loaded), stands on the terrain under it (a ray down
// to the Terrain layer: it waits where it is until the ground under it is there) and plays the matching state of its
// animator.

#include "StrataScript/StrataScript.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

using namespace Strata;

class Wander : public Script
{
public:
	float WalkSpeed = 1.6f;   // m/s
	float RunSpeed = 5.5f;
	float Range = 22.0f;      // Meters around its home
	int32_t Seed = 1;
	int32_t StateIndex = -1;  // 0 Survey, 1 Walk, 2 Run; -1 before its first state
	float Distance = 0.0f;    // Meters moved since it was last set to 0

	void OnCreate() override
	{
		m_Home = GetTransform().GetWorldPosition();
		m_Random.Seed(static_cast<uint64_t>(Seed) * 7919u + 17u);
		const int32_t layer = Physics::GetLayerIndex("Terrain");
		m_TerrainMask = layer >= 0 ? (1u << static_cast<uint32_t>(layer)) : Physics::c_AllLayers;
		Choose();
	}

	void OnReload() override { OnCreate(); }

	void OnUpdate(float deltaTime) override
	{
		TransformComponent transform = GetTransform();
		const glm::vec3 position = transform.GetWorldPosition();
		const std::optional<RaycastHit> ground = Physics::Raycast(position + glm::vec3(0.0f, 50.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 200.0f, m_TerrainMask);
		if (!ground)
			return; // No ground under it yet: it waits
		m_Timer -= deltaTime;
		if (m_Timer <= 0.0f)
			Choose();
		glm::vec3 next = position;
		if (StateIndex > 0)
		{
			const glm::vec3 toTarget(m_Target.x - position.x, 0.0f, m_Target.z - position.z);
			const float remaining = glm::length(toTarget);
			if (remaining < 0.5f)
			{
				SetState(0, 2.0f + m_Random.NextFloat() * 3.0f);
			}
			else
			{
				const float speed = StateIndex == 2 ? RunSpeed : WalkSpeed;
				next += toTarget / remaining * std::min(speed * deltaTime, remaining);
				transform.SetWorldRotation(glm::angleAxis(std::atan2(toTarget.x, toTarget.z), glm::vec3(0.0f, 1.0f, 0.0f)));
			}
		}
		if (const std::optional<RaycastHit> under = Physics::Raycast(next + glm::vec3(0.0f, 50.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 200.0f, m_TerrainMask))
			next.y = under->Point.y;
		Distance += glm::length(glm::vec2(next.x - position.x, next.z - position.z));
		transform.SetWorldPosition(next);
	}
private:
	void Choose()
	{
		const float roll = m_Random.NextFloat();
		const float angle = m_Random.NextFloat() * 6.2831853f;
		const float distance = Range * (0.3f + 0.7f * m_Random.NextFloat());
		m_Target = m_Home + glm::vec3(std::cos(angle) * distance, 0.0f, std::sin(angle) * distance);
		// A fox that surveyed walks or runs next: it never stands for two surveys in a row.
		const bool surveyed = StateIndex == 0;
		SetState(!surveyed && roll < 0.25f ? 0 : (roll < 0.85f ? 1 : 2), 4.0f + m_Random.NextFloat() * 6.0f);
	}

	void SetState(int32_t state, float seconds)
	{
		m_Timer = seconds;
		if (state == StateIndex)
			return;
		StateIndex = state;
		static const char* const c_States[] = { "Survey", "Walk", "Run" };
		GetEntity().GetAnimator().CrossFade(c_States[state], 0.3f);
	}
private:
	glm::vec3 m_Home = glm::vec3(0.0f);
	glm::vec3 m_Target = glm::vec3(0.0f);
	float m_Timer = 0.0f;
	uint32_t m_TerrainMask = Physics::c_AllLayers;
	Random m_Random;
};

ST_SCRIPT_CLASS(Wander)
{
	ST_SCRIPT_FIELD(WalkSpeed);
	ST_SCRIPT_FIELD(RunSpeed);
	ST_SCRIPT_FIELD(Range);
	ST_SCRIPT_FIELD(Seed);
	ST_SCRIPT_FIELD(StateIndex);
	ST_SCRIPT_FIELD(Distance);
}
