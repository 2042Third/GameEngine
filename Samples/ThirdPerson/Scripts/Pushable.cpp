// The crate the player pushes off the ledge into the pit (a dynamic box): it reports how far it moved from where it
// started, says once when it dropped into the pit, where it is the step that gets the player across, and goes back to
// its start when it fell off the islands (the course stays solvable).

#include "StrataScript/StrataScript.h"

using namespace Strata;

class Pushable : public Script
{
public:
	float DropHeight = 1.0f;     // Meters below its start that count as having dropped
	float LostHeight = -15.0f;   // Below this height it fell off the islands
	float Displacement = 0.0f;   // Meters from where it started
	bool Dropped = false;
	glm::vec3 Start = glm::vec3(0.0f); // Where it started (a field, so that a hot reload keeps it)

	void OnCreate() override { Start = GetTransform().GetWorldPosition(); }

	void OnUpdate(float) override
	{
		const glm::vec3 position = GetTransform().GetWorldPosition();
		Displacement = glm::length(position - Start);
		if (!Dropped && position.y < Start.y - DropHeight)
		{
			Dropped = true;
			Log::Info("ThirdPerson: the crate dropped into the pit");
		}
		if (position.y < LostHeight)
		{
			RigidBody body = GetEntity().GetRigidBody();
			body.SetLinearVelocity(glm::vec3(0.0f));
			body.SetAngularVelocity(glm::vec3(0.0f));
			body.Teleport(Start, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			Dropped = false;
			Log::Info("ThirdPerson: the crate is back on its ledge");
		}
	}
};

ST_SCRIPT_CLASS(Pushable)
{
	ST_SCRIPT_FIELD(DropHeight);
	ST_SCRIPT_FIELD(LostHeight);
	ST_SCRIPT_FIELD(Displacement);
	ST_SCRIPT_FIELD(Dropped);
	ST_SCRIPT_FIELD(Start);
}
