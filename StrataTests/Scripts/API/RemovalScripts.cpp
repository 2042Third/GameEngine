// Scripts that remove scripts whose instances still exist: while entities are destroyed, together with a failing
// AddScript, and while scripts keep creating scripted entities.

#include "TestScripts.h"

#include <stdexcept>

using namespace Strata;
using namespace ScriptTests;

ST_SCRIPT_EXTERN_C ST_SCRIPT_EXPORT int64_t StrataTestScripts_GetLiveInstanceCount(void)
{
	return CountedScript::s_LiveInstances;
}

// Removes the script ClassName from the entity named Target when it is destroyed. With DestroySelf it destroys its own
// entity in its first update (so OnDestroy runs while the scene flushes destruction at the end of the frame).
class RemoveOnDestroy : public Script
{
public:
	std::string Target;
	std::string ClassName = "Lifecycle";
	bool DestroySelf = false;

	void OnUpdate(float) override
	{
		if (DestroySelf)
			GetEntity().Destroy();
	}

	void OnDestroy() override
	{
		Scene::FindEntityByName(Target).RemoveScript(ClassName);
	}
};

ST_SCRIPT_CLASS(RemoveOnDestroy)
{
	ST_SCRIPT_FIELD(Target);
	ST_SCRIPT_FIELD(ClassName);
	ST_SCRIPT_FIELD(DestroySelf);
}

// Its constructor throws while FailConstruction is set (except for the instance the module builds to read defaults).
class Fragile : public Lifecycle
{
public:
	static inline bool s_FailConstruction = false;

	Fragile()
	{
		if (s_FailConstruction && GetEntity().GetID() != 0)
			throw std::runtime_error("Fragile failed to construct on purpose");
	}
protected:
	const char* GetClassName() const override { return "Fragile"; }
};

ST_SCRIPT_CLASS(Fragile)
{
	ST_SCRIPT_FIELD(Creates);
	ST_SCRIPT_FIELD(RecordUpdates);
}

// In its first update: removes Fragile from its entity, then adds it again while Fragile cannot be constructed.
class Readder : public CheckingScript
{
public:
	int32_t Frame = 0;

	void OnUpdate(float) override
	{
		if (++Frame != 1)
			return;
		Entity self = GetEntity();
		Expect(self.RemoveScript("Fragile"), "RemoveScript");
		Fragile::s_FailConstruction = true;
		Expect(!self.AddScript("Fragile"), "AddScript fails while the script cannot be constructed");
		Fragile::s_FailConstruction = false;
	}
};

ST_SCRIPT_CLASS(Readder)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Frame);
}

// Each generation creates the next one in OnCreate and removes the previous one, up to MaxGenerations: far more work
// than one sync point does, so removals are pending whenever a sync point stops early.
class Replicator : public CountedScript
{
public:
	int32_t Generation = 0;
	int32_t MaxGenerations = 0;
	Entity Previous;

	void OnCreate() override
	{
		if (Previous)
			Previous.RemoveScript("Replicator");
		if (Generation >= MaxGenerations)
			return;

		Entity next = Scene::CreateEntity("Replica");
		if (Replicator* replica = next.AddScript<Replicator>())
		{
			replica->Generation = Generation + 1;
			replica->MaxGenerations = MaxGenerations;
			replica->Previous = GetEntity();
		}
	}
};

ST_SCRIPT_CLASS(Replicator)
{
	ST_SCRIPT_FIELD(Generation);
	ST_SCRIPT_FIELD(MaxGenerations);
}
