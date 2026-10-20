// The launch pad at the top of the ramp: a trigger that sends every ball touching it down the ramp toward the pins.

#include "StrataScript/StrataScript.h"

using namespace Strata;

class LaunchPad : public Script
{
public:
	glm::vec3 LaunchVelocity = glm::vec3(9.0f, 1.0f, 0.0f);

	void OnTriggerEnter(const Collision& collision) override
	{
		if (collision.Other.IsValid() && collision.Other.GetTag() == "Ball")
			collision.Other.GetRigidBody().SetLinearVelocity(LaunchVelocity);
	}
};

ST_SCRIPT_CLASS(LaunchPad)
{
	ST_SCRIPT_FIELD(LaunchVelocity);
}
