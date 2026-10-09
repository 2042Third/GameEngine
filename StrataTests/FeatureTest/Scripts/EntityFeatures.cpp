// Entities: lookup, names, tags, activation, hierarchy, creation and destruction.

#include "SharedScripts.h"

#include <algorithm>
#include <vector>

using namespace Strata;
using namespace FeatureTest;

namespace
{

	bool Contains(const std::vector<Entity>& entities, Entity entity)
	{
		return std::find(entities.begin(), entities.end(), entity) != entities.end();
	}

}

// Works on the authored entities "Hierarchy Root" > "Branch A" > "Leaf", "Branch B" (Leaf and Branch B are tagged
// "Feature"), "Dormant" (inactive) > "Dormant Child", "Toggled" and "Doomed".
class EntityFeatures : public FeatureScript
{
public:
	static constexpr int32_t c_ReactivateFrame = 3;

	Entity CreatedRoot;
	Entity CreatedChild;
	Entity DefaultNamed;
	Entity Doomed;

	void OnCreate() override
	{
		Journal(*this, "EntityFeatures", "OnCreate");

		Entity self = GetEntity();
		Expect(self.IsValid() && static_cast<bool>(self), "the script's entity is valid");
		Expect(self.GetName() == "Entity Features", "Entity::GetName");
		Expect(self.GetID() != 0 && Scene::GetEntity(self.GetID()) == self, "Scene::GetEntity finds an entity by id");
		Expect(!Scene::GetEntity(0x0BADF00D).IsValid() && !Entity().IsValid(), "unknown and null entities are invalid");

		CheckAuthoredHierarchy();
		CheckCreation();
		CheckActivation();

		// Destruction is deferred to the end of the frame (here: the end of the start).
		Doomed = Scene::FindEntityByName("Doomed");
		Expect(Doomed.IsValid() && Doomed.HasScript("DoomedProbe"), "the doomed entity exists");
		Doomed.Destroy();
		Expect(Doomed.IsValid(), "destroyed entities stay valid until the end of the frame");
		CreatedRoot.Destroy();
		DefaultNamed.Destroy();
	}

	void OnUpdate(float) override
	{
		const int32_t frame = GetFrame();
		if (frame == 0)
		{
			Expect(!Doomed.IsValid() && !Scene::FindEntityByName("Doomed"), "destroyed entities are gone after the frame");
			Expect(!CreatedRoot.IsValid() && !CreatedChild.IsValid(), "destroying an entity destroys its descendants");
			Expect(!DefaultNamed.IsValid(), "a created entity can be destroyed");
		}

		Entity toggled = Scene::FindEntityByName("Toggled");
		ActivationProbe* probe = toggled.GetScript<ActivationProbe>();
		Expect(probe != nullptr, "GetScript finds the activation probe");
		if (!probe)
			return;

		if (frame < c_ReactivateFrame)
		{
			Expect(probe->Updates == 0, "inactive entities receive no updates");
		}
		else if (frame == c_ReactivateFrame)
		{
			toggled.SetActive(true);
			Expect(toggled.IsActive() && toggled.IsActiveInHierarchy(), "SetActive(true) reactivates");
		}
		else if (frame == c_ReactivateFrame + 3)
		{
			Expect(probe->Updates >= 2, "reactivated entities update again");
			Completed = true;
		}
	}
private:
	void CheckAuthoredHierarchy()
	{
		Entity root = Scene::FindEntityByName("Hierarchy Root");
		Entity branchA = Scene::FindEntityByName("Branch A");
		Entity branchB = Scene::FindEntityByName("Branch B");
		Entity leaf = Scene::FindEntityByName("Leaf");
		Expect(root && branchA && branchB && leaf, "Scene::FindEntityByName");
		Expect(!Scene::FindEntityByName("No Such Entity").IsValid(), "FindEntityByName of a missing name");

		const std::vector<Entity> children = root.GetChildren();
		Expect(children.size() == 2 && children[0] == branchA && children[1] == branchB, "GetChildren in order");
		Expect(branchA.GetParent() == root && leaf.GetParent() == branchA, "GetParent");
		Expect(!root.GetParent().IsValid(), "roots have no parent");
		Expect(leaf.GetChildren().empty(), "leaves have no children");

		Expect(leaf.GetTag() == "Feature" && branchB.GetTag() == "Feature", "GetTag of authored tags");
		Expect(root.GetTag().empty(), "untagged entities have an empty tag");
		const std::vector<Entity> tagged = Scene::FindEntitiesByTag("Feature");
		Expect(tagged.size() == 2 && tagged[0] == leaf && tagged[1] == branchB, "FindEntitiesByTag in hierarchy order");
		Expect(Scene::FindEntitiesByTag("No Such Tag").empty(), "FindEntitiesByTag of an unused tag");

		const std::vector<Entity> roots = Scene::GetRootEntities();
		Expect(Contains(roots, root) && Contains(roots, GetEntity()) && !Contains(roots, leaf), "Scene::GetRootEntities");
	}

	void CheckCreation()
	{
		CreatedRoot = Scene::CreateEntity("Created Root");
		Expect(CreatedRoot.IsValid() && CreatedRoot.GetName() == "Created Root" && !CreatedRoot.GetParent(), "Scene::CreateEntity as a root");
		Expect(CreatedRoot.HasComponent("Transform") && CreatedRoot.IsActive(), "created entities have a transform and are active");
		DefaultNamed = Scene::CreateEntity();
		Expect(DefaultNamed.GetName() == "Entity", "CreateEntity without arguments");

		CreatedChild = Scene::CreateEntity("Created Child", CreatedRoot);
		Expect(CreatedChild.GetParent() == CreatedRoot, "Scene::CreateEntity under a parent");
		Expect(CreatedChild.SetParent(Entity()) && !CreatedChild.GetParent(), "SetParent(null) makes a root");
		Expect(CreatedChild.SetParent(CreatedRoot, false) && CreatedChild.GetParent() == CreatedRoot, "SetParent");

		CreatedRoot.SetName("Renamed Root");
		Expect(CreatedRoot.GetName() == "Renamed Root" && Scene::FindEntityByName("Renamed Root") == CreatedRoot, "SetName");
		CreatedRoot.SetTag("Created");
		Expect(CreatedRoot.GetTag() == "Created" && CreatedRoot.HasComponent("Tag"), "SetTag adds a tag");
		Expect(Scene::FindEntitiesByTag("Created").size() == 1, "FindEntitiesByTag finds new tags");
		CreatedRoot.SetTag("");
		Expect(CreatedRoot.GetTag().empty() && !CreatedRoot.HasComponent("Tag"), "an empty tag removes the Tag component");
	}

	void CheckActivation()
	{
		Entity dormant = Scene::FindEntityByName("Dormant");
		Entity dormantChild = Scene::FindEntityByName("Dormant Child");
		Expect(!dormant.IsActive() && !dormant.IsActiveInHierarchy(), "authored inactive entities");
		Expect(dormantChild.IsActive() && !dormantChild.IsActiveInHierarchy(), "children of inactive entities are inactive in the hierarchy");

		Entity toggled = Scene::FindEntityByName("Toggled");
		Expect(toggled.IsActive(), "the toggled entity starts active");
		toggled.SetActive(false);
		Expect(!toggled.IsActive() && !toggled.IsActiveInHierarchy() && toggled.HasComponent("Inactive"), "SetActive(false) deactivates");
	}
};

ST_SCRIPT_CLASS(EntityFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(CreatedRoot);
	ST_SCRIPT_FIELD(CreatedChild);
	ST_SCRIPT_FIELD(DefaultNamed);
	ST_SCRIPT_FIELD(Doomed);
}

void FeatureTest::ActivationProbe::OnCreate()
{
	Journal(*this, "ActivationProbe", "OnCreate");
}

void FeatureTest::ActivationProbe::OnUpdate(float)
{
	Updates++;
}

ST_SCRIPT_CLASS(ActivationProbe)
{
	ST_SCRIPT_FIELD(Updates);
}

// On the "Doomed" entity, which EntityFeatures destroys: OnDestroy runs while the entity is still valid.
class DoomedProbe : public Script
{
public:
	void OnCreate() override
	{
		Journal(*this, "DoomedProbe", "OnCreate");
	}

	void OnDestroy() override
	{
		Journal(*this, "DoomedProbe", "OnDestroy");
	}
};

ST_SCRIPT_CLASS(DoomedProbe)
{
}

// On "Dormant Child", whose parent is inactive: it is created and destroyed but never updated.
class InactiveProbe : public FeatureScript
{
public:
	void OnCreate() override
	{
		Journal(*this, "InactiveProbe", "OnCreate");
		Expect(GetEntity().IsActive() && !GetEntity().IsActiveInHierarchy(), "the probe's entity is inactive through its parent");
		Completed = true;
	}

	void OnUpdate(float) override
	{
		Expect(false, "inactive entities receive no OnUpdate");
	}

	void OnFixedUpdate(float) override
	{
		Expect(false, "inactive entities receive no OnFixedUpdate");
	}

	void OnLateUpdate(float) override
	{
		Expect(false, "inactive entities receive no OnLateUpdate");
	}
};

ST_SCRIPT_CLASS(InactiveProbe)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
}
