// The goal, on a static box trigger around the flag at the end of the course: the player reaching it completes the course
// (CourseState shows the time and keeps the best one).

#include "CourseState.h"

using namespace Strata;

class Goal : public Script
{
public:
	void OnTriggerEnter(const Collision& collision) override
	{
		if (!collision.Other.IsValid() || collision.Other.GetTag() != "Player")
			return;
		if (CourseState* course = Scene::FindEntityByName("Game").GetScript<CourseState>())
			course->Complete();
	}
};

ST_SCRIPT_CLASS(Goal)
{
}
