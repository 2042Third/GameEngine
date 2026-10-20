// The goal's basket: a trigger under the funnel that counts every ball that falls into it, once.

#include "Playground.h"

#include <cstdint>
#include <unordered_set>

using namespace Strata;

class GoalBasket : public Script
{
public:
	void OnTriggerEnter(const Collision& collision) override
	{
		if (!collision.Other.IsValid() || collision.Other.GetTag() != "Ball" || !m_Counted.insert(collision.Other.GetID()).second)
			return;
		if (Playground* playground = Scene::FindEntityByName("Game").GetScript<Playground>())
			playground->AddScore(collision.Point);
	}
private:
	std::unordered_set<uint64_t> m_Counted; // Balls already counted, so one that bounces out and back counts once
};

ST_SCRIPT_CLASS(GoalBasket)
{
}
