// Prefab and model instantiation test scripts.

#include "TestScripts.h"

using namespace Strata;
using namespace ScriptTests;

// The script of the test prefab's root.
class Spawned : public Script
{
public:
	int32_t Value = 0;
	int32_t ValueSeenInCreate = -1;

	void OnCreate() override
	{
		ValueSeenInCreate = Value;
		Record(*this, "Spawned", "Create");
	}
};

ST_SCRIPT_CLASS(Spawned)
{
	ST_SCRIPT_FIELD(Value);
	ST_SCRIPT_FIELD(ValueSeenInCreate);
}

// Instantiates the prefab at PrefabPath (also given by handle) and the model in OnCreate, and SpawnPerUpdate more
// prefab instances every update.
class Spawner : public CheckingScript
{
public:
	std::string PrefabPath;
	AssetHandle Prefab;
	AssetHandle Model;
	int32_t SpawnPerUpdate = 0;
	int32_t SpawnCount = 0;
	Entity FirstSpawned;
	Entity PathSpawned;
	Entity ModelSpawned;
	Entity LastSpawned;

	void OnCreate() override
	{
		const AssetHandle found = Assets::Find(PrefabPath);
		Expect(found == Prefab, "Assets::Find");
		Expect(!Assets::Find("Missing/Nothing.stprefab"), "Assets::Find of a missing path");
		Expect(Assets::RequestLoad(found), "Assets::RequestLoad");
		Expect(!Assets::RequestLoad(AssetHandle(0x999)), "Assets::RequestLoad of an unknown asset");

		Entity spawned = Scene::Instantiate(Prefab, glm::vec3(1.0f, 2.0f, 3.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f), GetEntity());
		Expect(spawned.IsValid() && spawned.GetParent() == GetEntity(), "Instantiate under a parent");
		Expect(Near(spawned.GetTransform().GetTranslation(), glm::vec3(1.0f, 2.0f, 3.0f)), "Instantiate with a transform");
		Expect(spawned.GetChildren().size() == 1, "the prefab hierarchy is instantiated");
		Spawned* script = spawned.GetScript<Spawned>();
		Expect(script != nullptr, "scripts of instantiated entities exist immediately");
		if (script)
		{
			Expect(script->ValueSeenInCreate == -1, "their OnCreate has not run yet");
			script->Value = 7;
		}
		Expect(Assets::IsLoaded(Prefab), "instantiated assets are loaded");

		Entity byPath = Scene::Instantiate(PrefabPath);
		Expect(byPath.IsValid() && !byPath.GetParent() && byPath != spawned, "Instantiate by path");

		Entity model = Scene::Instantiate(Model);
		Expect(model.IsValid(), "Instantiate a model");

		Expect(!Scene::Instantiate(AssetHandle(0x999)).IsValid(), "Instantiate of an unknown asset fails");
		Expect(!Scene::Instantiate(AssetHandle()).IsValid(), "Instantiate of a null asset fails");
		Expect(!Scene::Instantiate(Prefab, Entity(0x31337)).IsValid(), "Instantiate under a missing parent fails");

		FirstSpawned = spawned;
		PathSpawned = byPath;
		ModelSpawned = model;
	}

	void OnUpdate(float) override
	{
		for (int32_t index = 0; index < SpawnPerUpdate; index++)
		{
			LastSpawned = Scene::Instantiate(Prefab);
			if (Spawned* script = LastSpawned.GetScript<Spawned>())
				script->Value = 100 + SpawnCount;
			SpawnCount++;
		}
	}
};

ST_SCRIPT_CLASS(Spawner)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(PrefabPath);
	ST_SCRIPT_FIELD(Prefab);
	ST_SCRIPT_FIELD(Model);
	ST_SCRIPT_FIELD(SpawnPerUpdate);
	ST_SCRIPT_FIELD(SpawnCount);
	ST_SCRIPT_FIELD(FirstSpawned);
	ST_SCRIPT_FIELD(PathSpawned);
	ST_SCRIPT_FIELD(ModelSpawned);
	ST_SCRIPT_FIELD(LastSpawned);
}
