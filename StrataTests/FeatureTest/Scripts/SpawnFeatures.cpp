// Assets and instantiation: finding and loading assets, spawning prefabs and models in every way the SDK offers.

#include "SharedScripts.h"

#include <string>
#include <vector>

using namespace Strata;
using namespace FeatureTest;

// Requests its assets in OnCreate and spawns once they are loaded (games never block on assets). Spawned crates fall
// onto the ground next to the spawner.
class SpawnFeatures : public FeatureScript
{
public:
	static constexpr int32_t c_MaxLoadFrames = 120;

	AssetHandle Prefab;
	AssetHandle Model;
	std::string PrefabPath;
	int32_t SpawnFrame = -1;
	Entity First;
	Entity Placed;
	Entity ByPath;
	Entity ByPathPlaced;
	Entity ModelInstance;

	void OnCreate() override
	{
		Journal(*this, "SpawnFeatures", "OnCreate");
		Expect(Prefab.IsValid() && Assets::Find(PrefabPath) == Prefab, "Assets::Find of the prefab (by path field)");
		Expect(Assets::Find("Models/Platform.gltf") == Model, "Assets::Find of the model");

		// Every asset type of the project can be requested.
		for (const char* path : { "Prefabs/Crate.stprefab", "Models/Platform.gltf", "Materials/Feature.stmat", "Textures/Checker.png",
				 "Textures/Sky.hdr", "Audio/Blip.wav", "Fonts/FeatureBlocks.ttf", "Scenes/Feature.stscene" })
		{
			const AssetHandle asset = Assets::Find(path);
			Expect(asset.IsValid() && static_cast<bool>(asset), "every project asset is found");
			Expect(Assets::RequestLoad(asset), "Assets::RequestLoad");
		}
	}

	void OnUpdate(float) override
	{
		const int32_t frame = GetFrame();
		if (SpawnFrame < 0)
		{
			if (Assets::IsLoaded(Prefab) && Assets::IsLoaded(Model))
			{
				Spawn();
				SpawnFrame = frame;
			}
			else
			{
				Expect(frame < c_MaxLoadFrames, "the prefab and the model load in time");
			}
			return;
		}

		if (frame == SpawnFrame + 1)
		{
			// OnCreate of the spawned scripts ran after Spawn configured them.
			CrateScript* crate = First.GetScript<CrateScript>();
			Expect(crate != nullptr && crate->ValueSeenInCreate == 5, "spawned scripts are configured before their OnCreate");
			CrateScript* byPath = ByPath.GetScript<CrateScript>();
			Expect(byPath != nullptr && byPath->ValueSeenInCreate == 1, "spawned scripts start with the prefab's field values");

			// The authored crate plus the four spawned ones.
			Expect(Scene::FindEntitiesByTag("Crate").size() == 5, "spawned prefab roots keep the prefab's tag");
			ByPathPlaced.Destroy();
		}
		else if (frame == SpawnFrame + 2)
		{
			Expect(!ByPathPlaced.IsValid() && Scene::FindEntitiesByTag("Crate").size() == 4, "spawned entities can be destroyed");
			Completed = true;
		}
	}
private:
	void Spawn()
	{
		const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);

		// By handle, as a root, at the prefab's own transform.
		First = Scene::Instantiate(Prefab);
		Expect(First.IsValid() && First.GetName() == "Crate" && !First.GetParent(), "Scene::Instantiate(handle)");
		Expect(First.GetChildren().size() == 1 && First.GetChildren()[0].GetName() == "Crate Lid", "the prefab hierarchy is instantiated");
		Expect(First.GetProperty<AssetHandle>("PrefabInstance", "Prefab") == Prefab, "spawned entities link to their prefab");
		CrateScript* crate = First.GetScript<CrateScript>();
		Expect(crate != nullptr && crate->ValueSeenInCreate == -1, "scripts of spawned entities exist before their OnCreate");
		if (crate)
			crate->Value = 5;
		First.GetTransform().SetTranslation(glm::vec3(9.0f, 2.0f, -2.0f));

		// By handle, under a parent, with a local transform.
		Placed = Scene::Instantiate(Prefab, glm::vec3(0.0f, 2.0f, 1.0f), identity, glm::vec3(0.5f), GetEntity());
		Expect(Placed.GetParent() == GetEntity(), "Scene::Instantiate(handle, transform, parent)");
		Expect(Near(Placed.GetTransform().GetTranslation(), glm::vec3(0.0f, 2.0f, 1.0f)) && Near(Placed.GetTransform().GetScale(), glm::vec3(0.5f)),
			"the spawned root gets the given local transform");

		// By path, under a parent.
		ByPath = Scene::Instantiate(PrefabPath, GetEntity());
		Expect(ByPath.IsValid() && ByPath.GetParent() == GetEntity(), "Scene::Instantiate(path, parent)");
		ByPath.GetTransform().SetTranslation(glm::vec3(0.0f, 2.0f, -1.0f));

		// By path, as a root, with a transform.
		const glm::quat turned = glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		ByPathPlaced = Scene::Instantiate(PrefabPath, glm::vec3(8.0f, 3.0f, 0.0f), turned);
		Expect(ByPathPlaced.IsValid() && !ByPathPlaced.GetParent() && Near(ByPathPlaced.GetTransform().GetRotation(), turned),
			"Scene::Instantiate(path, transform)");

		// A model: a root named after the file holding the glTF node hierarchy ("Platform" > "Beacon").
		ModelInstance = Scene::Instantiate(Model, glm::vec3(0.0f, 0.0f, -8.0f));
		Expect(ModelInstance.IsValid() && ModelInstance.GetName() == "Platform", "Scene::Instantiate of a model");
		Expect(ModelInstance.GetProperty<AssetHandle>("PrefabInstance", "Prefab") == Model, "model instances link to the model");
		const std::vector<Entity> nodes = ModelInstance.GetChildren();
		Expect(nodes.size() == 1 && nodes[0].GetName() == "Platform", "glTF nodes become child entities");
		if (nodes.empty())
			return;
		Expect(nodes[0].GetProperty<AssetHandle>("MeshRenderer", "Mesh").value_or(AssetHandle()).IsValid(), "model nodes render imported meshes");
		const std::vector<Entity> beacon = nodes[0].GetChildren();
		Expect(beacon.size() == 1 && beacon[0].GetName() == "Beacon" && beacon[0].HasComponent("MeshRenderer"), "child nodes keep their hierarchy");
	}
};

ST_SCRIPT_CLASS(SpawnFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(Prefab);
	ST_SCRIPT_FIELD(Model);
	ST_SCRIPT_FIELD(PrefabPath);
	ST_SCRIPT_FIELD(SpawnFrame);
	ST_SCRIPT_FIELD(First);
	ST_SCRIPT_FIELD(Placed);
	ST_SCRIPT_FIELD(ByPath);
	ST_SCRIPT_FIELD(ByPathPlaced);
	ST_SCRIPT_FIELD(ModelInstance);
}

void FeatureTest::CrateScript::OnCreate()
{
	Journal(*this, "CrateScript", "OnCreate");
	ValueSeenInCreate = Value;
	Expect(GetEntity().HasComponent("PrefabInstance") && GetEntity().GetTag() == "Crate", "crates are prefab instances");
	Completed = true;
}

ST_SCRIPT_CLASS(CrateScript)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(Value);
	ST_SCRIPT_FIELD(ValueSeenInCreate);
}
