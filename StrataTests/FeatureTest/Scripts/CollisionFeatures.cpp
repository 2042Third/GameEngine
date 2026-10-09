// Contact callbacks: a probe that falls through the trigger zone onto the ground and is lifted out again, and a fragile
// probe that destroys itself when it lands.

#include "FeatureScript.h"

using namespace Strata;
using namespace FeatureTest;

// On "Collision Probe": a small dynamic cube above the trigger zone. It falls into the zone and onto the ground; resting
// there, it jumps back to its start above the zone (ending both contacts) and falls in again.
class CollisionFeatures : public FeatureScript
{
public:
	int32_t CollisionEnters = 0;
	int32_t CollisionExits = 0;
	int32_t TriggerEnters = 0;
	int32_t TriggerExits = 0;
	glm::vec3 Start = glm::vec3(0.0f);
	bool Lifted = false;

	void OnCreate() override
	{
		Journal(*this, "CollisionFeatures", "OnCreate");
		Start = GetTransform().GetWorldPosition();
	}

	void OnFixedUpdate(float) override
	{
		if (Lifted || CollisionEnters == 0 || TriggerEnters == 0)
			return;
		Lifted = true;
		RigidBody body = GetEntity().GetRigidBody();
		Expect(body.Teleport(Start) && body.SetLinearVelocity(glm::vec3(0.0f)), "lift the probe out of the zone");
	}

	void OnCollisionEnter(const Collision& collision) override
	{
		if (CollisionEnters++ > 0)
			return;
		Journal(*this, "CollisionFeatures", "OnCollisionEnter");
		Expect(collision.Other == Scene::FindEntityByName("Ground"), "the probe lands on the ground");
		Expect(collision.Normal.y < -0.9f, "the contact normal points from the probe to the ground");
		Expect(Near(collision.Point.y, 0.0f, 0.05f), "the contact point lies on the ground");
		Expect(TriggerEnters == 1, "the probe passed into the trigger zone before");
	}

	void OnCollisionExit(const Collision& collision) override
	{
		if (CollisionExits++ == 0)
		{
			Journal(*this, "CollisionFeatures", "OnCollisionExit");
			Expect(Lifted && collision.Other == Scene::FindEntityByName("Ground"), "lifting the probe ends its contact with the ground");
		}
		Completed = CollisionExits > 0 && TriggerExits > 0;
	}

	void OnTriggerEnter(const Collision& collision) override
	{
		if (TriggerEnters++ > 0)
			return;
		Journal(*this, "CollisionFeatures", "OnTriggerEnter");
		Expect(collision.Other == Scene::FindEntityByName("Trigger Zone"), "the probe enters the trigger zone");
	}

	void OnTriggerExit(const Collision& collision) override
	{
		if (TriggerExits++ == 0)
		{
			Journal(*this, "CollisionFeatures", "OnTriggerExit");
			Expect(Lifted && collision.Other == Scene::FindEntityByName("Trigger Zone"), "lifting the probe takes it out of the trigger zone");
		}
		Completed = CollisionExits > 0 && TriggerExits > 0;
	}
};

ST_SCRIPT_CLASS(CollisionFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(CollisionEnters);
	ST_SCRIPT_FIELD(CollisionExits);
	ST_SCRIPT_FIELD(TriggerEnters);
	ST_SCRIPT_FIELD(TriggerExits);
	ST_SCRIPT_FIELD(Start);
	ST_SCRIPT_FIELD(Lifted);
}

// On "Fragile Probe": a small dynamic cube that destroys its own entity when it lands (the runners check the journal).
class FragileProbe : public Script
{
public:
	void OnCreate() override
	{
		Journal(*this, "FragileProbe", "OnCreate");
	}

	void OnCollisionEnter(const Collision&) override
	{
		Journal(*this, "FragileProbe", "OnCollisionEnter");
		GetEntity().Destroy();
	}

	void OnDestroy() override
	{
		Journal(*this, "FragileProbe", "OnDestroy");
	}
};

ST_SCRIPT_CLASS(FragileProbe) {}
