// Physics observed through transforms: dynamic bodies fall onto colliders and come to rest; kinematic bodies follow
// their scripted transform.

#include "FeatureScript.h"

#include <cmath>

using namespace Strata;
using namespace FeatureTest;

// On a dynamic body: it must fall from its authored height and come to rest with its origin at RestHeight (the scene
// sets RestHeight from the collider shapes: a sphere on the mesh collider platform, a capsule and a compound body on the
// ground box).
class PhysicsFeatures : public FeatureScript
{
public:
	static constexpr float c_FallDistance = 0.5f;
	static constexpr float c_RestSpeed = 0.01f;  // World units per second
	static constexpr int32_t c_RestSteps = 15;   // Consecutive fixed steps below the rest speed

	float RestHeight = 0.0f;
	float Tolerance = 0.05f;
	float StartHeight = 0.0f;
	float LastHeight = 0.0f;
	bool Fell = false;
	int32_t RestingSteps = 0;
	int32_t FixedSteps = 0;

	void OnCreate() override
	{
		Journal(*this, "PhysicsFeatures", "OnCreate");
		Expect(GetEntity().HasComponent("RigidBody") && GetEntity().GetProperty<int32_t>("RigidBody", "Type") == 1, "physics probes sit on dynamic bodies");
		StartHeight = GetTransform().GetWorldPosition().y;
		LastHeight = StartHeight;
		Expect(StartHeight > RestHeight + c_FallDistance, "the body starts above its rest height");
	}

	void OnFixedUpdate(float fixedDeltaTime) override
	{
		FixedSteps++;
		const float height = GetTransform().GetWorldPosition().y;
		Expect(std::isfinite(height) && height <= StartHeight + 1e-3f, "bodies never rise above their start");
		Expect(height > RestHeight - 0.5f, "bodies do not fall through colliders");
		if (height < StartHeight - c_FallDistance)
			Fell = true;

		const float speed = std::abs(height - LastHeight) / fixedDeltaTime;
		LastHeight = height;
		RestingSteps = Fell && speed < c_RestSpeed ? RestingSteps + 1 : 0;
		if (RestingSteps == c_RestSteps && !Completed)
		{
			Expect(Near(height, RestHeight, Tolerance), "the body comes to rest on the collider below it");
			Completed = true;
		}
	}
};

ST_SCRIPT_CLASS(PhysicsFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(RestHeight);
	ST_SCRIPT_FIELD(Tolerance);
	ST_SCRIPT_FIELD(StartHeight);
	ST_SCRIPT_FIELD(LastHeight);
	ST_SCRIPT_FIELD(Fell);
	ST_SCRIPT_FIELD(RestingSteps);
	ST_SCRIPT_FIELD(FixedSteps);
}

// On a kinematic body: moves it up and down in fixed steps (bodies follow kinematic transforms).
class KinematicMover : public FeatureScript
{
public:
	float BaseHeight = 0.0f;
	float Amplitude = 0.0f;
	int32_t Steps = 0;

	void OnCreate() override
	{
		Journal(*this, "KinematicMover", "OnCreate");
		Expect(GetEntity().GetProperty<int32_t>("RigidBody", "Type") == 2, "the mover is a kinematic body");
		Expect(Amplitude > 0.0f, "the scene sets the amplitude");
	}

	void OnFixedUpdate(float) override
	{
		TransformComponent transform = GetTransform();
		const float expected = Steps == 0 ? transform.GetTranslation().y : Height(Steps);
		Expect(Near(transform.GetTranslation().y, expected, 1e-3f), "kinematic bodies keep their scripted position");

		Steps++;
		glm::vec3 position = transform.GetTranslation();
		position.y = Height(Steps);
		Expect(transform.SetTranslation(position), "move the kinematic body");
		if (Steps == 50)
			Completed = true;
	}
private:
	float Height(int32_t step) const
	{
		return BaseHeight + Amplitude * std::sin(static_cast<float>(step) * 0.1f);
	}
};

ST_SCRIPT_CLASS(KinematicMover)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(BaseHeight);
	ST_SCRIPT_FIELD(Amplitude);
	ST_SCRIPT_FIELD(Steps);
}
