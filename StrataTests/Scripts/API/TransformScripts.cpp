// Transform API test scripts.

#include "TestScripts.h"

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
