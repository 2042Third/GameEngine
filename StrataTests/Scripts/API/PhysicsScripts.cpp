// Scripts of the Scripting.Physics tests (StrataTests/src/Scripting/ScriptPhysicsTests.cpp). They run on "Tester" in the
// physics test scene: a static "Ground" box (top face at y = 0), a weightless dynamic "Crate" (a unit cube of mass 1 without
// damping at (0, 5, 0), layer 2), a kinematic "Platform" (a unit cube at (5, 2, 0)), a static trigger "Zone" (half extents
// 1 at (-5, 1, 0), layer 3) and "Plain", an entity without physics.

#include "TestScripts.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

using namespace Strata;
using namespace ScriptTests;

namespace
{

	// A unit cube of mass 1: its inertia around every axis through its center.
	constexpr float c_CubeInertia = 1.0f / 6.0f;
	constexpr float c_NaN = std::numeric_limits<float>::quiet_NaN();
	constexpr float c_Infinity = std::numeric_limits<float>::infinity();

	struct PhysicsScene
	{
		Entity Ground = Scene::FindEntityByName("Ground");
		Entity Crate = Scene::FindEntityByName("Crate");
		Entity Platform = Scene::FindEntityByName("Platform");
		Entity Zone = Scene::FindEntityByName("Zone");
		Entity Plain = Scene::FindEntityByName("Plain");
	};

	// Points the SDK at another host API table while it lives (restoring the engine's on every path).
	class ScopedHost
	{
	public:
		explicit ScopedHost(const StrataScriptHostAPI* host)
			: m_Previous(Detail::s_Host)
		{
			Detail::s_Host = host;
		}

		~ScopedHost()
		{
			Detail::s_Host = m_Previous;
		}

		ScopedHost(const ScopedHost&) = delete;
		ScopedHost& operator=(const ScopedHost&) = delete;
	private:
		const StrataScriptHostAPI* m_Previous;
	};

}

// Body functions and queries through the SDK, over three fixed steps (the physics world exists from the first one on).
class PhysicsAPI : public CheckingScript
{
public:
	int32_t Steps = 0;
	bool Done = false;

	void OnFixedUpdate(float fixedDeltaTime) override
	{
		const PhysicsScene scene;
		RigidBody crate = scene.Crate.GetRigidBody();
		const glm::vec3 center = scene.Crate.GetTransform().GetWorldPosition();
		switch (Steps++)
		{
			case 0:
				// Queries first, while every body is where the scene placed it.
				CheckQueries(scene);
				CheckImpulses(crate, center);
				Expect(crate.GetEntity() == scene.Crate, "RigidBody::GetEntity");
				Expect(crate.SetLinearVelocity(glm::vec3(0.0f)) && crate.SetAngularVelocity(glm::vec3(0.0f)), "stop the crate");
				// Forces and torques act during this fixed step (the physics system steps after the scripts).
				Expect(crate.AddForce({ 0.0f, 0.0f, 60.0f }), "AddForce");
				Expect(crate.AddTorque({ 0.0f, 1.0f, 0.0f }), "AddTorque");
				break;
			case 1:
				Expect(Near(crate.GetLinearVelocity(), glm::vec3(0.0f, 0.0f, 60.0f * fixedDeltaTime), 1e-3f), "a force accelerates the body during one step");
				Expect(Near(crate.GetAngularVelocity(), glm::vec3(0.0f, fixedDeltaTime / c_CubeInertia, 0.0f), 1e-3f), "a torque turns the body during one step");
				Expect(crate.SetLinearVelocity(glm::vec3(0.0f)) && crate.SetAngularVelocity(glm::vec3(0.0f)), "stop the crate again");
				Expect(crate.AddForceAtPosition({ 0.0f, 0.0f, 60.0f }, center + glm::vec3(1.0f, 0.0f, 0.0f)), "AddForceAtPosition");
				break;
			case 2:
				Expect(Near(crate.GetLinearVelocity(), glm::vec3(0.0f, 0.0f, 60.0f * fixedDeltaTime), 1e-3f), "a force at a position pushes the body");
				Expect(Near(crate.GetAngularVelocity(), glm::vec3(0.0f, -60.0f * fixedDeltaTime / c_CubeInertia, 0.0f), 1e-2f), "and turns it");
				CheckTeleport(scene);
				Done = true;
				break;
			default:
				break;
		}
	}
private:
	void CheckQueries(const PhysicsScene& scene)
	{
		// The direction is not normalized: distances are in world units all the same.
		const glm::vec3 above(0.0f, 10.0f, 0.0f);
		const glm::vec3 down(0.0f, -2.0f, 0.0f);
		const std::optional<RaycastHit> hit = Physics::Raycast(above, down);
		Expect(hit.has_value() && hit->HitEntity == scene.Crate, "Raycast hits the closest body");
		Expect(hit.has_value() && Near(hit->Distance, 4.5f, 1e-3f) && Near(hit->Point, glm::vec3(0.0f, 5.5f, 0.0f), 1e-3f)
			&& Near(hit->Normal, glm::vec3(0.0f, 1.0f, 0.0f), 1e-3f), "the hit's distance, point and normal");
		Expect(Physics::Raycast(above, down, 100.0f, Physics::c_AllLayers, scene.Crate).value_or(RaycastHit()).HitEntity == scene.Ground,
			"Raycast skips the ignored entity");
		Expect(Physics::Raycast(above, down, 100.0f, ~(1u << 2)).value_or(RaycastHit()).HitEntity == scene.Ground, "Raycast filters layers");
		Expect(!Physics::Raycast(above, down, 4.0f).has_value(), "Raycast ends at the maximum distance");
		Expect(!Physics::Raycast({ 100.0f, 10.0f, 100.0f }, down).has_value(), "Raycast misses");

		const std::vector<RaycastHit> hits = Physics::RaycastAll(above, down);
		Expect(hits.size() == 2 && hits[0].HitEntity == scene.Crate && hits[1].HitEntity == scene.Ground && Near(hits[1].Distance, 10.0f, 1e-3f),
			"RaycastAll returns every body along the ray, the closest first");
		Expect(Physics::RaycastAll(above, down, 100.0f, Physics::c_AllLayers, scene.Ground).size() == 1, "RaycastAll skips the ignored entity");

		const glm::vec3 aboveZone(-5.0f, 10.0f, 0.0f);
		Expect(Physics::Raycast(aboveZone, down).value_or(RaycastHit()).HitEntity == scene.Ground, "rays pass through triggers");
		const std::optional<RaycastHit> zoneHit = Physics::Raycast(aboveZone, down, 100.0f, Physics::c_AllLayers, Entity(), true);
		Expect(zoneHit.has_value() && zoneHit->HitEntity == scene.Zone && Near(zoneHit->Distance, 8.0f, 1e-3f), "rays can hit triggers");
		Expect(Physics::RaycastAll(aboveZone, down, 100.0f, Physics::c_AllLayers, Entity(), true).size() == 2, "RaycastAll can include triggers");

		Expect(Physics::OverlapSphere({ 0.0f, 5.0f, 0.0f }, 1.0f) == std::vector<Entity> { scene.Crate }, "OverlapSphere");
		Expect(Physics::OverlapSphere({ -5.0f, 1.0f, 0.0f }, 0.5f).empty(), "overlaps skip triggers");
		Expect(Physics::OverlapSphere({ -5.0f, 1.0f, 0.0f }, 0.5f, Physics::c_AllLayers, true) == std::vector<Entity> { scene.Zone }, "overlaps can include triggers");
		Expect(Physics::OverlapSphere({ -5.0f, 1.0f, 0.0f }, 0.5f, ~(1u << 3), true).empty(), "overlaps filter layers");
		const glm::quat turned = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		Expect(Physics::OverlapBox({ 5.0f, 2.0f, 0.0f }, glm::vec3(0.5f), turned) == std::vector<Entity> { scene.Platform }, "OverlapBox");
		Expect(Physics::OverlapBox({ 5.0f, 4.0f, 0.0f }, { 1.0f, 0.25f, 1.0f }).empty(), "OverlapBox finds nothing in empty space");
	}

	void CheckImpulses(RigidBody& crate, const glm::vec3& center)
	{
		Expect(crate.SetLinearVelocity({ 1.0f, 0.0f, 0.0f }) && Near(crate.GetLinearVelocity(), glm::vec3(1.0f, 0.0f, 0.0f)), "SetLinearVelocity");
		Expect(crate.SetAngularVelocity({ 0.0f, 2.0f, 0.0f }) && Near(crate.GetAngularVelocity(), glm::vec3(0.0f, 2.0f, 0.0f)), "SetAngularVelocity");
		Expect(crate.AddImpulse({ 0.0f, 0.0f, 2.0f }) && Near(crate.GetLinearVelocity(), glm::vec3(1.0f, 0.0f, 2.0f), 1e-3f), "an impulse changes the velocity at once");
		Expect(crate.AddAngularImpulse({ 0.0f, c_CubeInertia, 0.0f }) && Near(crate.GetAngularVelocity(), glm::vec3(0.0f, 3.0f, 0.0f), 1e-3f),
			"an angular impulse changes the angular velocity at once");

		Expect(crate.SetLinearVelocity(glm::vec3(0.0f)) && crate.SetAngularVelocity(glm::vec3(0.0f)), "stop the crate");
		Expect(crate.AddImpulseAtPosition({ 0.0f, 0.0f, 1.0f }, center + glm::vec3(1.0f, 0.0f, 0.0f)), "AddImpulseAtPosition");
		Expect(Near(crate.GetLinearVelocity(), glm::vec3(0.0f, 0.0f, 1.0f), 1e-3f) && Near(crate.GetAngularVelocity(), glm::vec3(0.0f, -1.0f / c_CubeInertia, 0.0f), 1e-2f),
			"an impulse off the center pushes and turns the body");
	}

	void CheckTeleport(const PhysicsScene& scene)
	{
		RigidBody platform = scene.Platform.GetRigidBody();
		const glm::quat rotation = scene.Platform.GetTransform().GetWorldRotation();
		Expect(platform.Teleport({ 5.0f, 3.0f, 0.0f }), "Teleport a kinematic body");
		Expect(Near(scene.Platform.GetTransform().GetWorldPosition(), glm::vec3(5.0f, 3.0f, 0.0f)) && Near(scene.Platform.GetTransform().GetWorldRotation(), rotation),
			"the entity moves at once and keeps its rotation");
		Expect(Physics::OverlapSphere({ 5.0f, 3.0f, 0.0f }, 0.25f) == std::vector<Entity> { scene.Platform }, "queries see the teleported body at once");
		const glm::quat turned = glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		Expect(platform.Teleport({ 5.0f, 3.0f, 0.0f }, turned) && Near(scene.Platform.GetTransform().GetWorldRotation(), turned), "Teleport with a rotation");
		Expect(Near(platform.GetLinearVelocity(), glm::vec3(0.0f)), "kinematic bodies have velocities to read");
	}
};

ST_SCRIPT_CLASS(PhysicsAPI)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Steps);
	ST_SCRIPT_FIELD(Done);
}

// Calls the physics host functions directly with what the SDK never passes: every call fails (or degrades) harmlessly.
class PhysicsMisuse : public CheckingScript
{
public:
	bool Done = false;

	void OnFixedUpdate(float) override
	{
		if (Done)
			return;
		Done = true;

		const StrataScriptHostAPI* host = Detail::GetHost();
		StrataScriptContext* context = Detail::GetContext();
		const PhysicsScene scene;
		const uint64_t crate = scene.Crate.GetID();
		const glm::vec3 crateStart = scene.Crate.GetTransform().GetWorldPosition();
		const float zero[3] = { 0.0f, 0.0f, 0.0f };
		const float up[3] = { 0.0f, 1.0f, 0.0f };
		const float notFinite[3] = { c_NaN, 0.0f, 0.0f };
		const float identity[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		const float zeroRotation[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

		// Entities without a (suitable) body.
		float velocity[3] = { 7.0f, 7.0f, 7.0f };
		Expect(!host->GetLinearVelocity(context, scene.Plain.GetID(), velocity) && velocity[0] == 7.0f, "an entity without a body has no velocity");
		Expect(!host->AddForce(context, scene.Plain.GetID(), up), "an entity without a body takes no force");
		Expect(!host->AddImpulse(context, 0, up), "the null entity");
		Expect(!host->AddImpulse(context, 0x51DE5u, up), "an entity that does not exist");
		Expect(!host->SetLinearVelocity(context, scene.Platform.GetID(), up), "kinematic bodies take no velocity");
		Expect(!host->AddTorque(context, scene.Ground.GetID(), up), "static bodies take no torque");
		Expect(!host->AddImpulseAtPosition(context, scene.Zone.GetID(), up, zero), "static triggers take no impulse");
		Expect(host->GetAngularVelocity(context, scene.Ground.GetID(), velocity) && velocity[0] == 0.0f && velocity[1] == 0.0f, "static bodies do not move");

		// Missing and invalid vectors.
		Expect(!host->GetLinearVelocity(context, crate, nullptr), "GetLinearVelocity without an output");
		Expect(!host->GetAngularVelocity(context, crate, nullptr), "GetAngularVelocity without an output");
		Expect(!host->SetAngularVelocity(context, crate, nullptr), "SetAngularVelocity without a velocity");
		Expect(!host->AddImpulse(context, crate, notFinite), "a non-finite impulse");
		Expect(!host->AddAngularImpulse(context, crate, notFinite), "a non-finite angular impulse");
		Expect(!host->AddForceAtPosition(context, crate, up, nullptr), "a force without a position");
		Expect(!host->AddImpulseAtPosition(context, crate, up, notFinite), "an impulse at a non-finite position");
		Expect(!host->Teleport(context, crate, up, nullptr), "Teleport without a rotation");
		Expect(!host->Teleport(context, crate, nullptr, identity), "Teleport without a position");
		Expect(!host->Teleport(context, crate, up, zeroRotation), "Teleport with a zero quaternion");
		Expect(!host->Teleport(context, crate, notFinite, identity), "Teleport to a non-finite position");
		Expect(Near(scene.Crate.GetTransform().GetWorldPosition(), crateStart), "refused teleports move nothing");
		host->GetLinearVelocity(context, crate, velocity);
		Expect(velocity[0] == 0.0f && velocity[1] == 0.0f && velocity[2] == 0.0f, "refused changes leave the body at rest");

		// Rays.
		const float origin[3] = { 0.0f, 10.0f, 0.0f };
		const float down[3] = { 0.0f, -1.0f, 0.0f };
		StrataScriptRaycastHit hit = {};
		Expect(!host->Raycast(context, nullptr, down, 100.0f, Physics::c_AllLayers, 0, false, &hit), "a ray without an origin");
		Expect(!host->Raycast(context, origin, nullptr, 100.0f, Physics::c_AllLayers, 0, false, &hit), "a ray without a direction");
		Expect(!host->Raycast(context, notFinite, down, 100.0f, Physics::c_AllLayers, 0, false, &hit), "a ray from a non-finite origin");
		Expect(!host->Raycast(context, origin, zero, 100.0f, Physics::c_AllLayers, 0, false, &hit), "a ray without a direction length");
		Expect(!host->Raycast(context, origin, down, 0.0f, Physics::c_AllLayers, 0, false, &hit), "a ray of length zero");
		Expect(!host->Raycast(context, origin, down, -1.0f, Physics::c_AllLayers, 0, false, &hit), "a ray of negative length");
		Expect(!host->Raycast(context, origin, down, c_NaN, Physics::c_AllLayers, 0, false, &hit), "a ray of NaN length");
		Expect(hit.Entity == 0, "refused rays write no hit");
		Expect(host->Raycast(context, origin, down, c_Infinity, Physics::c_AllLayers, 0, false, nullptr), "an endless ray without an output");
		Expect(host->Raycast(context, origin, down, 100.0f, Physics::c_AllLayers, 0x51DE5u, false, &hit) && hit.Entity == crate,
			"ignoring an entity that does not exist ignores nothing");
		Expect(!host->Raycast(context, origin, down, 100.0f, 0u, 0, false, &hit), "an empty layer mask hits nothing");

		// Buffers: without one (or without capacity) only the count is reported; at most the capacity is written.
		Expect(host->RaycastAll(context, origin, down, 100.0f, Physics::c_AllLayers, 0, false, nullptr, 8) == 2, "RaycastAll without a buffer");
		StrataScriptRaycastHit hits[2] = {};
		hits[1].Entity = 99;
		Expect(host->RaycastAll(context, origin, down, 100.0f, Physics::c_AllLayers, 0, false, hits, 1) == 2 && hits[0].Entity == crate && hits[1].Entity == 99,
			"RaycastAll writes at most the capacity");
		Expect(host->RaycastAll(context, origin, down, -1.0f, Physics::c_AllLayers, 0, false, hits, 2) == 0, "RaycastAll refuses invalid rays");
		uint64_t ids[1] = { 99 };
		const float crateCenter[3] = { crateStart.x, crateStart.y, crateStart.z };
		Expect(host->OverlapSphere(context, crateCenter, 1.0f, Physics::c_AllLayers, false, ids, 0) == 1 && ids[0] == 99, "OverlapSphere without capacity");
		Expect(host->OverlapSphere(context, crateCenter, 1.0f, Physics::c_AllLayers, false, nullptr, 4) == 1, "OverlapSphere without a buffer");
		Expect(host->OverlapSphere(context, nullptr, 1.0f, Physics::c_AllLayers, false, ids, 1) == 0, "OverlapSphere without a center");
		Expect(host->OverlapSphere(context, crateCenter, -1.0f, Physics::c_AllLayers, false, ids, 1) == 0, "OverlapSphere with a negative radius");
		Expect(host->OverlapSphere(context, crateCenter, c_Infinity, Physics::c_AllLayers, false, ids, 1) == 0, "OverlapSphere with an infinite radius");
		const float halfExtents[3] = { 1.0f, 1.0f, 1.0f };
		Expect(host->OverlapBox(context, crateCenter, halfExtents, identity, Physics::c_AllLayers, false, ids, 1) == 1 && ids[0] == crate, "OverlapBox");
		Expect(host->OverlapBox(context, crateCenter, zero, identity, Physics::c_AllLayers, false, ids, 1) == 0, "OverlapBox without extents");
		Expect(host->OverlapBox(context, crateCenter, nullptr, identity, Physics::c_AllLayers, false, ids, 1) == 0, "OverlapBox without half extents");
		Expect(host->OverlapBox(context, crateCenter, halfExtents, zeroRotation, Physics::c_AllLayers, false, ids, 1) == 0, "OverlapBox with a zero quaternion");
		Expect(host->OverlapBox(context, crateCenter, halfExtents, nullptr, Physics::c_AllLayers, false, ids, 1) == 0, "OverlapBox without a rotation");
		Expect(host->OverlapBox(context, nullptr, halfExtents, identity, Physics::c_AllLayers, false, ids, 1) == 0, "OverlapBox without a center");

		// The SDK on entities without a body: zero velocities and failures.
		RigidBody plain = scene.Plain.GetRigidBody();
		Expect(plain.GetLinearVelocity() == glm::vec3(0.0f) && plain.GetAngularVelocity() == glm::vec3(0.0f), "no body, no velocity");
		Expect(!plain.Teleport({ 1.0f, 2.0f, 3.0f }) && !plain.AddForce({ 0.0f, 1.0f, 0.0f }), "no body, no changes");
		Expect(!Entity().GetRigidBody().AddImpulse({ 0.0f, 1.0f, 0.0f }), "the null entity has no body");
		Expect(scene.Crate.IsValid() && scene.Plain.IsValid(), "the entities survive");
	}
};

ST_SCRIPT_CLASS(PhysicsMisuse)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Done);
}

// The SDK's physics wrappers on an engine that predates the physics functions (a host table that ends before them) and
// on a table whose physics entries are null: everything fails without calling the engine.
class OlderEnginePhysics : public CheckingScript
{
public:
	bool Done = false;

	void OnFixedUpdate(float) override
	{
		if (Done)
			return;
		Done = true;

		const PhysicsScene scene;
		const glm::vec3 above(0.0f, 10.0f, 0.0f);
		const glm::vec3 down(0.0f, -1.0f, 0.0f);
		Expect(Physics::Raycast(above, down).has_value(), "this engine has the physics functions");

		StrataScriptHostAPI older = *Detail::GetHost();
		older.StructSize = static_cast<uint32_t>(offsetof(StrataScriptHostAPI, GetLinearVelocity));
		CheckUnavailable(scene, older, "an older engine");

		StrataScriptHostAPI missing = *Detail::GetHost();
		missing.GetLinearVelocity = nullptr;
		missing.SetLinearVelocity = nullptr;
		missing.GetAngularVelocity = nullptr;
		missing.SetAngularVelocity = nullptr;
		missing.AddForce = nullptr;
		missing.AddForceAtPosition = nullptr;
		missing.AddImpulse = nullptr;
		missing.AddImpulseAtPosition = nullptr;
		missing.AddTorque = nullptr;
		missing.AddAngularImpulse = nullptr;
		missing.Teleport = nullptr;
		missing.Raycast = nullptr;
		missing.RaycastAll = nullptr;
		missing.OverlapSphere = nullptr;
		missing.OverlapBox = nullptr;
		CheckUnavailable(scene, missing, "null entries");

		Expect(Physics::Raycast(above, down).has_value(), "the engine's table is back");
	}
private:
	void CheckUnavailable(const PhysicsScene& scene, const StrataScriptHostAPI& host, const char* description)
	{
		const glm::vec3 crateStart = scene.Crate.GetTransform().GetWorldPosition();
		{
			const ScopedHost scope(&host);
			RigidBody crate = scene.Crate.GetRigidBody();
			const bool changed = crate.SetLinearVelocity({ 1.0f, 0.0f, 0.0f }) || crate.SetAngularVelocity({ 1.0f, 0.0f, 0.0f })
				|| crate.AddForce({ 1.0f, 0.0f, 0.0f }) || crate.AddForceAtPosition({ 1.0f, 0.0f, 0.0f }, crateStart)
				|| crate.AddImpulse({ 1.0f, 0.0f, 0.0f }) || crate.AddImpulseAtPosition({ 1.0f, 0.0f, 0.0f }, crateStart)
				|| crate.AddTorque({ 1.0f, 0.0f, 0.0f }) || crate.AddAngularImpulse({ 1.0f, 0.0f, 0.0f }) || crate.Teleport({ 0.0f, 9.0f, 0.0f });
			const bool read = crate.GetLinearVelocity() != glm::vec3(0.0f) || crate.GetAngularVelocity() != glm::vec3(0.0f);
			const bool found = Physics::Raycast({ 0.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }).has_value()
				|| !Physics::RaycastAll({ 0.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }).empty()
				|| !Physics::OverlapSphere(crateStart, 1.0f).empty() || !Physics::OverlapBox(crateStart, glm::vec3(1.0f)).empty();
			Expect(!changed && !read && !found, description);
		}
		RigidBody crate = scene.Crate.GetRigidBody();
		Expect(Near(scene.Crate.GetTransform().GetWorldPosition(), crateStart) && crate.GetLinearVelocity() == glm::vec3(0.0f)
			&& crate.GetAngularVelocity() == glm::vec3(0.0f), "nothing reached the engine");
	}
};

ST_SCRIPT_CLASS(OlderEnginePhysics)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Done);
}

// In a scene without physics components: there is no simulation, so queries find nothing and body functions fail.
class PhysicsWithoutWorld : public CheckingScript
{
public:
	void OnCreate() override
	{
		const glm::vec3 origin(0.0f, 10.0f, 0.0f);
		const glm::vec3 down(0.0f, -1.0f, 0.0f);
		Expect(!Physics::Raycast(origin, down).has_value(), "Raycast finds nothing");
		Expect(Physics::RaycastAll(origin, down).empty(), "RaycastAll finds nothing");
		Expect(Physics::OverlapSphere(origin, 100.0f).empty() && Physics::OverlapBox(origin, glm::vec3(100.0f)).empty(), "overlaps find nothing");
		RigidBody body = GetEntity().GetRigidBody();
		Expect(!body.AddForce({ 0.0f, 1.0f, 0.0f }) && !body.SetLinearVelocity({ 0.0f, 1.0f, 0.0f }) && !body.Teleport(glm::vec3(1.0f)), "body functions fail");
		Expect(body.GetLinearVelocity() == glm::vec3(0.0f), "no velocity");
	}
};

ST_SCRIPT_CLASS(PhysicsWithoutWorld)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
}
