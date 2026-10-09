// The physics API of scripts (RigidBody, Physics) on a weightless dynamic probe: queries against the bodies of the scene,
// velocities, impulses, forces and teleporting.

#include "FeatureScript.h"

#include <optional>
#include <vector>

using namespace Strata;
using namespace FeatureTest;

// On "Physics Probe": a dynamic unit cube of mass 1 without gravity and damping, high above the ground at a free spot.
// Forces act during the next simulation step, but the first steps can be held (the game runtime streams the mesh of the
// platform's mesh collider in, and physics waits for it), so the probe applies each force once and waits for the step
// that takes it.
class PhysicsApiFeatures : public FeatureScript
{
public:
	static constexpr float c_CubeInertia = 1.0f / 6.0f; // Around every axis through the center
	static constexpr int32_t c_MaxWaitSteps = 150;

	glm::vec3 Home = glm::vec3(0.0f);
	int32_t Phase = 0;
	int32_t WaitSteps = 0;

	void OnCreate() override
	{
		Journal(*this, "PhysicsApiFeatures", "OnCreate");
		Home = GetTransform().GetWorldPosition();
		Expect(GetEntity().GetProperty<float>("RigidBody", "GravityScale") == 0.0f, "the probe is weightless");
	}

	void OnFixedUpdate(float fixedDeltaTime) override
	{
		if (Completed)
			return;

		RigidBody body = GetEntity().GetRigidBody();
		Expect(body.GetEntity() == GetEntity(), "RigidBody::GetEntity");
		const glm::vec3 center = GetTransform().GetWorldPosition();
		if (Phase == 0)
		{
			CheckQueries();
			CheckImpulses(body, center);
			Stop(body);
			Expect(body.AddForce({ 0.0f, 0.0f, 30.0f }), "AddForce");
			Expect(body.AddTorque({ 0.0f, 0.5f, 0.0f }), "AddTorque");
			Phase = 1;
		}
		else if (Phase == 1 && Stepped(body))
		{
			Expect(Near(body.GetLinearVelocity(), glm::vec3(0.0f, 0.0f, 30.0f * fixedDeltaTime), 1e-3f), "a force accelerates the body for one step");
			Expect(Near(body.GetAngularVelocity(), glm::vec3(0.0f, 0.5f * fixedDeltaTime / c_CubeInertia, 0.0f), 1e-3f), "a torque turns the body for one step");
			Stop(body);
			Expect(body.AddForceAtPosition({ 0.0f, 0.0f, 30.0f }, center + glm::vec3(1.0f, 0.0f, 0.0f)), "AddForceAtPosition");
			Phase = 2;
		}
		else if (Phase == 2 && Stepped(body))
		{
			Expect(Near(body.GetLinearVelocity(), glm::vec3(0.0f, 0.0f, 30.0f * fixedDeltaTime), 1e-3f), "a force off the center pushes the body");
			Expect(Near(body.GetAngularVelocity().y, -30.0f * fixedDeltaTime / c_CubeInertia, 1e-2f), "and turns it");
			Stop(body);
			const glm::quat turned = glm::angleAxis(glm::radians(20.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			Expect(body.Teleport(Home, turned) && Near(GetTransform().GetWorldRotation(), turned), "Teleport to a position and rotation");
			Expect(body.Teleport(Home) && Near(GetTransform().GetWorldPosition(), Home) && Near(GetTransform().GetWorldRotation(), turned),
				"Teleport keeps the rotation");
			Expect(Physics::OverlapSphere(Home, 0.25f) == std::vector<Entity> { GetEntity() }, "queries see the teleported body at once");
			Completed = true;
		}
	}
private:
	void Stop(RigidBody& body)
	{
		Expect(body.SetLinearVelocity(glm::vec3(0.0f)) && body.SetAngularVelocity(glm::vec3(0.0f)), "stop the probe");
	}

	// Whether the step that takes the pending force ran (the probe moves); fails after waiting too long.
	bool Stepped(const RigidBody& body)
	{
		if (body.GetLinearVelocity() != glm::vec3(0.0f))
		{
			WaitSteps = 0;
			return true;
		}
		Expect(++WaitSteps < c_MaxWaitSteps, "the simulation steps");
		return false;
	}

	void CheckQueries()
	{
		const Entity self = GetEntity();
		const Entity ground = Scene::FindEntityByName("Ground");
		const Entity zone = Scene::FindEntityByName("Trigger Zone");
		const glm::vec3 above = Home + glm::vec3(0.0f, 3.0f, 0.0f);
		const glm::vec3 down(0.0f, -1.0f, 0.0f);

		const std::optional<RaycastHit> hit = Physics::Raycast(above, down);
		Expect(hit.has_value() && hit->HitEntity == self && Near(hit->Distance, 2.5f, 1e-3f) && Near(hit->Point, Home + glm::vec3(0.0f, 0.5f, 0.0f), 1e-3f)
			&& Near(hit->Normal, glm::vec3(0.0f, 1.0f, 0.0f), 1e-3f), "Physics::Raycast hits the probe's top face");
		const std::optional<RaycastHit> groundHit = Physics::Raycast(above, down, 100.0f, Physics::c_AllLayers, self);
		Expect(groundHit.has_value() && groundHit->HitEntity == ground && Near(groundHit->Distance, above.y, 1e-3f), "a ray ignoring the probe hits the ground");
		const std::vector<RaycastHit> hits = Physics::RaycastAll(above, down);
		Expect(hits.size() == 2 && hits[0].HitEntity == self && hits[1].HitEntity == ground, "Physics::RaycastAll, sorted by distance");

		// The trigger zone's box spans x = 7 to 9, y = 0 to 2 and z = 5 to 7 (the collision probe falls into its x+ z- quarter).
		const glm::vec3 aboveZone(7.5f, 5.0f, 6.5f);
		Expect(Physics::Raycast(aboveZone, down).value_or(RaycastHit()).HitEntity == ground, "rays pass through triggers");
		const std::optional<RaycastHit> zoneHit = Physics::Raycast(aboveZone, down, 10.0f, Physics::c_AllLayers, Entity(), true);
		Expect(zoneHit.has_value() && zoneHit->HitEntity == zone && Near(zoneHit->Distance, 3.0f, 1e-3f), "rays can hit triggers");
		Expect(Physics::Raycast(aboveZone, down, 10.0f, ~(1u << 3), Entity(), true).value_or(RaycastHit()).HitEntity == ground, "rays filter layers");

		const glm::vec3 inZone(7.5f, 1.0f, 6.5f);
		Expect(Physics::OverlapSphere(inZone, 0.4f).empty(), "Physics::OverlapSphere skips triggers");
		Expect(Physics::OverlapSphere(inZone, 0.4f, Physics::c_AllLayers, true) == std::vector<Entity> { zone }, "Physics::OverlapSphere can include triggers");
		const glm::quat turned = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		Expect(Physics::OverlapBox(Home, glm::vec3(0.25f), turned) == std::vector<Entity> { self }, "Physics::OverlapBox");
	}

	void CheckImpulses(RigidBody& body, const glm::vec3& center)
	{
		Expect(body.SetLinearVelocity({ 1.0f, 0.0f, 0.0f }) && Near(body.GetLinearVelocity(), glm::vec3(1.0f, 0.0f, 0.0f)), "SetLinearVelocity");
		Expect(body.SetAngularVelocity({ 0.0f, 2.0f, 0.0f }) && Near(body.GetAngularVelocity(), glm::vec3(0.0f, 2.0f, 0.0f)), "SetAngularVelocity");
		Expect(body.AddImpulse({ 0.0f, 0.0f, 2.0f }) && Near(body.GetLinearVelocity(), glm::vec3(1.0f, 0.0f, 2.0f), 1e-3f), "AddImpulse changes the velocity at once");
		Expect(body.AddAngularImpulse({ 0.0f, c_CubeInertia, 0.0f }) && Near(body.GetAngularVelocity(), glm::vec3(0.0f, 3.0f, 0.0f), 1e-3f),
			"AddAngularImpulse changes the angular velocity at once");
		Stop(body);
		Expect(body.AddImpulseAtPosition({ 0.0f, 0.0f, 1.0f }, center + glm::vec3(1.0f, 0.0f, 0.0f))
			&& Near(body.GetLinearVelocity(), glm::vec3(0.0f, 0.0f, 1.0f), 1e-3f) && Near(body.GetAngularVelocity().y, -1.0f / c_CubeInertia, 1e-2f),
			"AddImpulseAtPosition pushes and turns the body");
	}
};

ST_SCRIPT_CLASS(PhysicsApiFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(Home);
	ST_SCRIPT_FIELD(Phase);
	ST_SCRIPT_FIELD(WaitSteps);
}
