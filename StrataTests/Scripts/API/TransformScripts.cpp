// Transform API test scripts.

#include "TestScripts.h"

#include <cstddef>
#include <limits>

using namespace Strata;
using namespace ScriptTests;

// Verifies the typed Transform access in OnCreate. Leaves the entity at local translation (-10, 0, 0) under a parent
// "TransformParent" at (10, 0, 0) with scale 2.
class TransformAPI : public CheckingScript
{
public:
	void OnCreate() override
	{
		TransformComponent transform = GetTransform();
		Expect(transform.SetTranslation({ 1.0f, 2.0f, 3.0f }) && Near(transform.GetTranslation(), glm::vec3(1.0f, 2.0f, 3.0f)), "translation");
		Expect(transform.SetScale(glm::vec3(2.0f)) && Near(transform.GetScale(), glm::vec3(2.0f)), "scale");
		const glm::quat yaw = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		Expect(transform.SetRotation(yaw) && Near(transform.GetRotation(), yaw), "rotation");
		Expect(Near(transform.GetForward(), glm::vec3(-1.0f, 0.0f, 0.0f)), "forward follows the rotation");
		Expect(transform.SetEulerAngles(glm::vec3(0.0f, glm::radians(45.0f), 0.0f)) && Near(transform.GetEulerAngles().y, glm::radians(45.0f)), "Euler angles");

		glm::vec3 translation, scale;
		glm::quat rotation;
		Expect(transform.SetLocalTransform({ 1.0f, 2.0f, 3.0f }, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)), "SetLocalTransform");
		Expect(transform.GetLocalTransform(translation, rotation, scale) && Near(translation, glm::vec3(1.0f, 2.0f, 3.0f)) && Near(scale, glm::vec3(1.0f)),
			"GetLocalTransform");

		Entity parent = Scene::CreateEntity("TransformParent");
		parent.GetTransform().SetTranslation({ 10.0f, 0.0f, 0.0f });
		parent.GetTransform().SetScale(glm::vec3(2.0f));
		Expect(GetEntity().SetParent(parent, false), "parenting");
		Expect(Near(transform.GetWorldPosition(), glm::vec3(12.0f, 4.0f, 6.0f)), "world position under a parent");
		Expect(Near(transform.GetWorldScale(), glm::vec3(2.0f)), "world scale under a parent");

		Expect(transform.SetWorldPosition({ -10.0f, 0.0f, 0.0f }), "SetWorldPosition");
		Expect(Near(transform.GetWorldPosition(), glm::vec3(-10.0f, 0.0f, 0.0f)) && Near(transform.GetTranslation(), glm::vec3(-10.0f, 0.0f, 0.0f)),
			"SetWorldPosition converts to local space");
		Expect(transform.SetWorldRotation(yaw) && Near(transform.GetWorldRotation(), yaw), "SetWorldRotation");
		Expect(transform.SetRotation(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)), "reset rotation");
		Expect(Near(transform.GetForward(), glm::vec3(0.0f, 0.0f, -1.0f)) && Near(transform.GetUp(), glm::vec3(0.0f, 1.0f, 0.0f))
			&& Near(transform.GetRight(), glm::vec3(1.0f, 0.0f, 0.0f)), "axes");

		const float nan = std::numeric_limits<float>::quiet_NaN();
		Expect(!transform.SetTranslation({ nan, 0.0f, 0.0f }), "non-finite translations are rejected");
		Expect(!transform.SetRotation(glm::quat(0.0f, 0.0f, 0.0f, 0.0f)), "zero rotations are rejected");
		Expect(Near(transform.GetTranslation(), glm::vec3(-10.0f, 0.0f, 0.0f)), "rejected values leave the transform unchanged");
		Expect(!TransformComponent(Entity(0x5151)).SetTranslation(glm::vec3(1.0f)), "missing entity");
	}
};

ST_SCRIPT_CLASS(TransformAPI)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
}

// Interpolation (Time::GetStepAlpha, TransformComponent::GetDrawnPosition, GetDrawnRotation, ResetInterpolation): moves itself
// in OnFixedUpdate, records in OnLateUpdate what it sees of its drawn pose (the test compares it with the scene's), resets its
// interpolation when the test asks, and checks once what an older engine gives.
class InterpolationProbe : public CheckingScript
{
public:
	float Alpha = -1.0f;                        // Time::GetStepAlpha in the latest OnLateUpdate
	glm::vec3 DrawnPosition = glm::vec3(0.0f);   // GetDrawnPosition there
	glm::quat DrawnRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	bool ResetRequested = false;                // The next OnLateUpdate resets the interpolation
	bool ResetDone = false;

	void OnFixedUpdate(float step) override
	{
		TransformComponent transform = GetTransform();
		transform.SetTranslation(transform.GetTranslation() + glm::vec3(2.0f * step, 0.0f, 0.0f));
		transform.SetRotation(glm::angleAxis(step, glm::vec3(0.0f, 1.0f, 0.0f)) * transform.GetRotation());
	}

	void OnLateUpdate(float) override
	{
		TransformComponent transform = GetTransform();
		Alpha = Time::GetStepAlpha();
		DrawnPosition = transform.GetDrawnPosition();
		DrawnRotation = transform.GetDrawnRotation();
		if (ResetRequested)
		{
			ResetRequested = false;
			Expect(transform.ResetInterpolation(), "ResetInterpolation");
			Expect(Near(transform.GetDrawnPosition(), transform.GetWorldPosition()), "a reset draws the world position");
			ResetDone = true;
		}
		if (!m_OlderChecked)
		{
			m_OlderChecked = true;
			StrataScriptHostAPI older = *Detail::GetHost();
			older.StructSize = static_cast<uint32_t>(offsetof(StrataScriptHostAPI, GetStepAlpha));
			CheckUnavailable(older, "an older engine");
			StrataScriptHostAPI missing = *Detail::GetHost();
			missing.GetStepAlpha = nullptr;
			missing.GetDrawnTransform = nullptr;
			missing.ResetInterpolation = nullptr;
			CheckUnavailable(missing, "null entries");
			Expect(!TransformComponent(Entity(0x5151)).ResetInterpolation(), "a missing entity cannot be reset");
		}
	}
private:
	// Without the functions: nothing interpolates, the drawn pose is the world pose, and nothing resets.
	void CheckUnavailable(const StrataScriptHostAPI& host, const char* what)
	{
		const ScopedHost scoped(&host);
		TransformComponent transform = GetTransform();
		Expect(Time::GetStepAlpha() == 1.0f, what);
		Expect(Near(transform.GetDrawnPosition(), transform.GetWorldPosition()) && Near(transform.GetDrawnRotation(), transform.GetWorldRotation()), what);
		Expect(!transform.ResetInterpolation(), what);
	}

	bool m_OlderChecked = false;
};

ST_SCRIPT_CLASS(InterpolationProbe)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Alpha);
	ST_SCRIPT_FIELD(DrawnPosition);
	ST_SCRIPT_FIELD(DrawnRotation);
	ST_SCRIPT_FIELD(ResetRequested);
	ST_SCRIPT_FIELD(ResetDone);
}
