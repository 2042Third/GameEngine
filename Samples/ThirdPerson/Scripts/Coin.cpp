// A coin, on a static sphere trigger tagged "Coin" whose child is the coin's model: the model spins and bobs, and the
// player walking through the trigger collects the coin once (CourseState counts it).

#include "CourseState.h"

#include <cmath>

using namespace Strata;

class Coin : public Script
{
public:
	float SpinSpeed = 180.0f; // Degrees per second
	float BobHeight = 0.12f;  // Meters
	bool Collected = false;

	void OnCreate() override
	{
		const std::vector<Entity> children = GetEntity().GetChildren();
		m_Visual = children.empty() ? Entity() : children.front();
		if (m_Visual.IsValid())
			m_VisualTranslation = m_Visual.GetTransform().GetTranslation();
		m_Phase = static_cast<float>(GetEntity().GetID() % 628) * 0.01f; // Coins bob out of step
	}

	void OnReload() override { OnCreate(); }

	void OnUpdate(float deltaTime) override
	{
		if (!m_Visual.IsValid())
			return;
		m_Time += deltaTime;
		TransformComponent transform = m_Visual.GetTransform();
		transform.SetRotation(glm::angleAxis(glm::radians(m_Time * SpinSpeed), glm::vec3(0.0f, 1.0f, 0.0f)));
		transform.SetTranslation(m_VisualTranslation + glm::vec3(0.0f, std::sin(m_Time * 2.5f + m_Phase) * BobHeight, 0.0f));
	}

	void OnTriggerEnter(const Collision& collision) override
	{
		if (Collected || !collision.Other.IsValid() || collision.Other.GetTag() != "Player")
			return;
		Collected = true;
		if (CourseState* course = Scene::FindEntityByName("Game").GetScript<CourseState>())
			course->AddCoin(GetTransform().GetWorldPosition());
		GetEntity().SetActive(false);
	}
private:
	Entity m_Visual;
	glm::vec3 m_VisualTranslation = glm::vec3(0.0f); // Where the model sits on the trigger
	float m_Time = 0.0f;
	float m_Phase = 0.0f;
};

ST_SCRIPT_CLASS(Coin)
{
	ST_SCRIPT_FIELD(SpinSpeed);
	ST_SCRIPT_FIELD(BobHeight);
	ST_SCRIPT_FIELD(Collected);
}
