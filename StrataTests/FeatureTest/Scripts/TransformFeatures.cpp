// Transforms: local and world values, directions, whole-transform access and reparenting.

#include "FeatureScript.h"

using namespace Strata;
using namespace FeatureTest;

// Uses the authored "Transform Parent" (translation (1, 2, 3), a quarter turn around +Y, scale 2) and its child
// "Transform Child" (local translation (1, 0, 0)). Only the child and entities created here are changed.
class TransformFeatures : public FeatureScript
{
public:
	Entity Mover;

	void OnCreate() override
	{
		Journal(*this, "TransformFeatures", "OnCreate");
		CheckAuthoredTransforms();
		CheckWorldWrites();
		CheckLocalWrites();
		CheckReparenting();
	}

	void OnUpdate(float) override
	{
		// The script's own transform: Script::GetTransform is the transform of GetEntity().
		TransformComponent self = GetTransform();
		Expect(self.GetEntity() == GetEntity(), "Script::GetTransform");
		const glm::vec3 position = self.GetTranslation();
		Expect(self.SetTranslation(position + glm::vec3(0.0f, 0.0f, 0.5f)), "move the script's own entity");
		Expect(Near(self.GetWorldPosition(), position + glm::vec3(0.0f, 0.0f, 0.5f)), "a root's world position is its translation");
		if (GetFrame() == 2)
			Completed = true;
	}
private:
	void CheckAuthoredTransforms()
	{
		Entity parent = Scene::FindEntityByName("Transform Parent");
		Entity child = Scene::FindEntityByName("Transform Child");
		const glm::quat quarterTurn = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));

		TransformComponent parentTransform = parent.GetTransform();
		Expect(parentTransform.GetEntity() == parent, "TransformComponent::GetEntity");
		Expect(Near(parentTransform.GetTranslation(), glm::vec3(1.0f, 2.0f, 3.0f)), "GetTranslation");
		Expect(Near(parentTransform.GetRotation(), quarterTurn), "GetRotation");
		Expect(Near(parentTransform.GetScale(), glm::vec3(2.0f)), "GetScale");
		Expect(Near(parentTransform.GetEulerAngles(), glm::vec3(0.0f, glm::radians(90.0f), 0.0f), 1e-3f), "GetEulerAngles");

		glm::vec3 translation(0.0f), scale(0.0f);
		glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
		Expect(parentTransform.GetLocalTransform(translation, rotation, scale) && Near(translation, glm::vec3(1.0f, 2.0f, 3.0f)) && Near(rotation, quarterTurn)
			&& Near(scale, glm::vec3(2.0f)), "GetLocalTransform");

		// World = parent translation + parent rotation * (parent scale * local translation).
		TransformComponent childTransform = child.GetTransform();
		Expect(Near(childTransform.GetTranslation(), glm::vec3(1.0f, 0.0f, 0.0f)), "the child's local translation");
		Expect(Near(childTransform.GetWorldPosition(), glm::vec3(1.0f, 2.0f, 1.0f), 1e-3f), "GetWorldPosition");
		Expect(Near(childTransform.GetWorldRotation(), quarterTurn), "GetWorldRotation");
		Expect(Near(childTransform.GetWorldScale(), glm::vec3(2.0f), 1e-3f), "GetWorldScale");
		Expect(Near(childTransform.GetForward(), glm::vec3(-1.0f, 0.0f, 0.0f), 1e-3f), "GetForward (-Z)");
		Expect(Near(childTransform.GetRight(), glm::vec3(0.0f, 0.0f, -1.0f), 1e-3f), "GetRight (+X)");
		Expect(Near(childTransform.GetUp(), glm::vec3(0.0f, 1.0f, 0.0f), 1e-3f), "GetUp (+Y)");
	}

	void CheckWorldWrites()
	{
		Entity child = Scene::FindEntityByName("Transform Child");
		TransformComponent transform = child.GetTransform();
		Expect(transform.SetWorldPosition(glm::vec3(1.0f, 2.0f, 5.0f)), "SetWorldPosition");
		Expect(Near(transform.GetWorldPosition(), glm::vec3(1.0f, 2.0f, 5.0f), 1e-3f), "the world position was written");
		// The target is (0, 0, 2) away from the parent: turned back by the quarter turn and divided by the scale 2.
		Expect(Near(transform.GetTranslation(), glm::vec3(-1.0f, 0.0f, 0.0f), 1e-3f), "SetWorldPosition computes the local translation");

		const glm::quat target = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		Expect(transform.SetWorldRotation(target) && Near(transform.GetWorldRotation(), target, 1e-3f), "SetWorldRotation");
	}

	void CheckLocalWrites()
	{
		Mover = Scene::CreateEntity("Transform Mover");
		TransformComponent transform = Mover.GetTransform();
		Expect(transform.SetTranslation(glm::vec3(4.0f, 5.0f, 6.0f)) && Near(transform.GetTranslation(), glm::vec3(4.0f, 5.0f, 6.0f)), "SetTranslation");
		const glm::quat rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(1.0f, 0.0f, 0.0f));
		Expect(transform.SetRotation(rotation) && Near(transform.GetRotation(), rotation), "SetRotation");
		Expect(transform.SetScale(glm::vec3(1.0f, 2.0f, 3.0f)) && Near(transform.GetScale(), glm::vec3(1.0f, 2.0f, 3.0f)), "SetScale");
		Expect(Near(transform.GetTranslation(), glm::vec3(4.0f, 5.0f, 6.0f)), "setting one part keeps the others");

		const glm::vec3 euler(glm::radians(10.0f), glm::radians(20.0f), glm::radians(30.0f));
		Expect(transform.SetEulerAngles(euler) && Near(transform.GetEulerAngles(), euler, 1e-3f), "SetEulerAngles");

		const glm::quat turn = glm::angleAxis(glm::radians(-60.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		Expect(transform.SetLocalTransform(glm::vec3(-1.0f, 0.5f, 2.0f), turn, glm::vec3(0.5f)), "SetLocalTransform");
		glm::vec3 translation(0.0f), scale(0.0f);
		glm::quat read(1.0f, 0.0f, 0.0f, 0.0f);
		Expect(transform.GetLocalTransform(translation, read, scale), "GetLocalTransform after SetLocalTransform");
		Expect(Near(translation, glm::vec3(-1.0f, 0.5f, 2.0f)) && Near(read, turn) && Near(scale, glm::vec3(0.5f)), "SetLocalTransform writes every part");
	}

	void CheckReparenting()
	{
		Entity parent = Scene::FindEntityByName("Transform Parent");
		TransformComponent mover = Mover.GetTransform();
		const glm::vec3 world = mover.GetWorldPosition();
		Expect(Mover.SetParent(parent, true) && Near(mover.GetWorldPosition(), world, 1e-3f), "SetParent keeping the world transform");
		Expect(Mover.SetParent(Entity(), true) && Near(mover.GetWorldPosition(), world, 1e-3f), "back to a root keeping the world transform");

		const glm::vec3 local = mover.GetTranslation();
		Expect(Mover.SetParent(parent, false) && Near(mover.GetTranslation(), local), "SetParent keeping the local transform");
		Expect(!Near(mover.GetWorldPosition(), world, 1e-2f), "the world position follows the new parent");
	}
};

ST_SCRIPT_CLASS(TransformFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(Mover);
}
