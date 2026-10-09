// Entity API test scripts: names, tags, hierarchy, activity, creation and destruction.

#include "TestScripts.h"

#include <algorithm>

using namespace Strata;
using namespace ScriptTests;

// Verifies the entity API in OnCreate. The scene provides an entity named "Tester" running this script.
class EntityAPI : public CheckingScript
{
public:
	Entity DoomedEntity;
	Entity CreatedRoot;
	Entity CreatedChild;

	void OnCreate() override
	{
		Entity self = GetEntity();
		Expect(self.IsValid(), "the script's entity is valid");
		Expect(self.GetName() == "Tester", "GetName");
		self.SetName("Renamed");
		Expect(self.GetName() == "Renamed", "SetName");
		Expect(Scene::FindEntityByName("Renamed") == self, "FindEntityByName");
		Expect(!Scene::FindEntityByName("Nobody"), "FindEntityByName of a missing name");
		Expect(Scene::GetEntity(self.GetID()) == self, "Scene::GetEntity by id");
		Expect(!Scene::GetEntity(0x7777).IsValid(), "Scene::GetEntity of an unknown id");

		Expect(self.GetTag().empty(), "no tag initially");
		self.SetTag("Player");
		Expect(self.GetTag() == "Player", "SetTag");
		const std::vector<Entity> tagged = Scene::FindEntitiesByTag("Player");
		Expect(tagged.size() == 1 && tagged[0] == self, "FindEntitiesByTag");
		self.SetTag("");
		Expect(!self.HasComponent("Tag") && self.GetTag().empty(), "an empty tag removes the Tag component");

		Entity child = Scene::CreateEntity("Child", self);
		Expect(child.IsValid() && child.GetParent() == self, "CreateEntity under a parent");
		Expect(child.GetName() == "Child", "created entity name");
		Expect(child.HasComponent("Transform"), "created entities have a Transform");
		const std::vector<Entity> children = self.GetChildren();
		Expect(children.size() == 1 && children[0] == child, "GetChildren");
		Expect(!Scene::CreateEntity("Orphan", Entity(0x31337)).IsValid(), "CreateEntity under a missing parent fails");

		Entity root = Scene::CreateEntity("CreatedRoot");
		Expect(!root.GetParent(), "roots have no parent");
		Expect(child.SetParent(root), "SetParent");
		Expect(child.GetParent() == root && self.GetChildren().empty(), "SetParent moves the entity");
		Expect(!root.SetParent(child), "SetParent rejects cycles");
		Expect(child.SetParent(Entity()), "SetParent to null");
		Expect(!child.GetParent(), "SetParent to null makes a root");
		Expect(child.SetParent(root, false), "SetParent without keeping the world transform");
		const std::vector<Entity> roots = Scene::GetRootEntities();
		Expect(std::find(roots.begin(), roots.end(), root) != roots.end() && std::find(roots.begin(), roots.end(), self) != roots.end(),
			"GetRootEntities");

		Expect(self.IsActive() && self.IsActiveInHierarchy(), "entities start active");
		root.SetActive(false);
		Expect(!root.IsActive() && !root.IsActiveInHierarchy(), "SetActive(false)");
		Expect(child.IsActive() && !child.IsActiveInHierarchy(), "children of inactive entities are inactive in the hierarchy");
		root.SetActive(true);
		Expect(child.IsActiveInHierarchy(), "SetActive(true)");

		// Destruction is deferred to the end of the frame; until then the entity stays valid.
		Entity doomed = Scene::CreateEntity("Doomed");
		doomed.Destroy();
		Expect(doomed.IsValid(), "destroyed entities stay valid until the end of the frame");

		// Operations on missing entities are harmless no-ops.
		const Entity missing(0x1234567);
		Expect(!missing.IsValid() && missing.GetName().empty() && missing.GetTag().empty(), "missing entity queries");
		Expect(!missing.GetParent() && missing.GetChildren().empty() && !missing.IsActive(), "missing entity hierarchy");
		Expect(!Entity(missing).SetParent(self) && !Entity(missing).HasComponent("Transform"), "missing entity changes");
		Expect(!Entity().IsValid() && Entity().GetName().empty(), "null entity");

		DoomedEntity = doomed;
		CreatedRoot = root;
		CreatedChild = child;
	}
};

ST_SCRIPT_CLASS(EntityAPI)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(DoomedEntity);
	ST_SCRIPT_FIELD(CreatedRoot);
	ST_SCRIPT_FIELD(CreatedChild);
}

// Creates and destroys entities while the scene updates: frame 1 destroys "Victim" and spawns a scripted entity,
// frame 2 destroys itself.
class Destroyer : public CheckingScript
{
public:
	int32_t Frame = 0;
	Entity Spawned;

	void OnUpdate(float) override
	{
		Frame++;
		if (Frame == 1)
		{
			Entity victim = Scene::FindEntityByName("Victim");
			Expect(victim.IsValid(), "the victim exists");
			victim.Destroy();
			Expect(victim.IsValid(), "the victim stays valid during the frame");

			Spawned = Scene::CreateEntity("Spawned");
			Expect(Spawned.AddScript("Lifecycle"), "AddScript on a new entity");
			Expect(Spawned.HasScript("Lifecycle"), "HasScript after AddScript");
		}
		else if (Frame == 2)
		{
			GetEntity().Destroy();
		}
	}
};

ST_SCRIPT_CLASS(Destroyer)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Frame);
	ST_SCRIPT_FIELD(Spawned);
}

// Adds a Lifecycle script to its own entity in frame 1 and removes it in frame 2.
class ScriptAdder : public CheckingScript
{
public:
	int32_t Frame = 0;

	void OnUpdate(float) override
	{
		Frame++;
		Entity self = GetEntity();
		if (Frame == 1)
		{
			Lifecycle* added = self.AddScript<Lifecycle>();
			Expect(added != nullptr, "AddScript<T> returns the new instance");
			Expect(added && added->Creates == 0, "OnCreate of an added script runs later");
			Expect(self.AddScript<Lifecycle>() == added, "adding a script twice returns the existing instance");
			Expect(!self.AddScript("NoSuchScript"), "AddScript of an unknown class fails");
			Expect(self.HasScript("ScriptAdder") && self.HasScript("Lifecycle"), "HasScript");
		}
		else if (Frame == 2)
		{
			Expect(self.RemoveScript("Lifecycle"), "RemoveScript");
			Expect(!self.HasScript("Lifecycle") && !self.GetScript<Lifecycle>(), "a removed script is gone at once");
			Expect(!self.RemoveScript("Lifecycle"), "removing twice fails");
		}
	}
};

ST_SCRIPT_CLASS(ScriptAdder)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Frame);
}
