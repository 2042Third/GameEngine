// A jump pad, on a static box trigger over a spring: the player walking onto it is thrown up (ThirdPersonPlayer::Launch)
// high enough to reach the next island, and the spring's model squashes and springs back.

#include "ThirdPersonPlayer.h"

#include <algorithm>
#include <cmath>

using namespace Strata;

class JumpPad : public Script
{
public:
	float LaunchSpeed = 12.0f; // m/s up: about 7 m high
	AssetHandle BounceSound;
	int32_t Launches = 0;

	void OnCreate() override
	{
		const std::vector<Entity> children = GetEntity().GetChildren();
		m_Visual = children.empty() ? Entity() : children.front();
		if (m_Visual.IsValid())
			m_VisualScale = m_Visual.GetTransform().GetScale();
		Assets::RequestLoad(BounceSound);
	}

	void OnReload() override { OnCreate(); }

	void OnUpdate(float deltaTime) override
	{
		if (!m_Visual.IsValid() || m_Squash <= 0.0f)
			return;
		m_Squash = std::max(0.0f, m_Squash - deltaTime * 3.0f);
		const float stretch = 1.0f + std::sin(m_Squash * 9.0f) * m_Squash * 0.6f;
		m_Visual.GetTransform().SetScale(glm::vec3(m_VisualScale.x, m_VisualScale.y * stretch, m_VisualScale.z));
	}

	void OnTriggerEnter(const Collision& collision) override
	{
		if (!collision.Other.IsValid() || collision.Other.GetTag() != "Player")
			return;
		ThirdPersonPlayer* player = collision.Other.GetScript<ThirdPersonPlayer>();
		if (!player)
			return;
		player->Launch(LaunchSpeed);
		Launches++;
		m_Squash = 1.0f;
		if (BounceSound.IsValid())
			Audio::PlayOneShotAt(BounceSound, GetTransform().GetWorldPosition(), 0.8f);
		Log::Info("ThirdPerson: jump pad ", GetEntity().GetName());
	}
private:
	Entity m_Visual;
	glm::vec3 m_VisualScale = glm::vec3(1.0f);
	float m_Squash = 0.0f;
};

ST_SCRIPT_CLASS(JumpPad)
{
	ST_SCRIPT_FIELD(LaunchSpeed);
	ST_SCRIPT_FIELD(BounceSound);
	ST_SCRIPT_FIELD(Launches);
}
