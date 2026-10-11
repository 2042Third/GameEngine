// Scripts of the Scripting.Streaming tests (StrataTests/src/Scripting/ScriptStreamingTests.cpp): sub-scenes, the progress of
// scene switches and of asset closures, and instantiating many instances at once.

#include "TestScripts.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace Strata;
using namespace ScriptTests;

// Loads Room (a scene asset) as a sub-scene, waits until it and the closure of Crate (a prefab) are in, instantiates three
// crates in one call, then unloads the room. Every update also records the progress of the scene switch the engine side
// fakes (SwitchProgress).
class SubSceneStreamer : public CheckingScript
{
public:
	AssetHandle Room;
	std::string RoomPath;
	AssetHandle Crate;
	int32_t Phase = 0;
	int32_t LoadingFrames = 0;
	float CrateProgress = 0.0f;
	float SwitchProgress = 0.0f;
	Entity FirstCrate;
	Entity RoomRoot;
	int32_t CratesSpawned = 0;
	bool Done = false;

	void OnUpdate(float) override
	{
		SwitchProgress = Game::GetSceneLoadProgress();
		switch (Phase)
		{
			case 0:
			{
				m_Room = Scene::LoadAdditive(Room);
				Expect(m_Room.IsValid() && m_Room.GetID() != 0, "Scene::LoadAdditive(handle)");
				Expect(!m_Room.IsLoaded(), "a sub-scene is not loaded at once");
				// The same room by path while the first is loading: it starts, but its IDs collide once the first is in.
				const SubScene twin = Scene::LoadAdditive(RoomPath);
				Expect(twin.IsValid() && twin.GetID() != m_Room.GetID(), "Scene::LoadAdditive(path)");
				Expect(twin.Unload(), "SubScene::Unload of a load in progress");
				Expect(!twin.Unload(), "SubScene::Unload of a sub-scene that is gone");
				CrateProgress = Assets::GetLoadProgress(Crate);
				Expect(CrateProgress >= 0.0f && CrateProgress <= 1.0f, "Assets::GetLoadProgress starts loading the closure");
				Phase = 1;
				break;
			}
			case 1:
			{
				CrateProgress = Assets::GetLoadProgress(Crate);
				if (!m_Room.IsLoaded() || CrateProgress < 1.0f)
				{
					LoadingFrames++;
					break;
				}
				RoomRoot = Scene::FindEntityByName("RoomFloor");
				Expect(RoomRoot.IsValid(), "the sub-scene's entities exist once it is loaded");
				std::vector<glm::mat4> transforms;
				for (int index = 0; index < 3; index++)
					transforms.push_back(glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<float>(index) * 2.0f, 1.0f, 0.0f)));
				const std::vector<Entity> crates = Scene::InstantiateMany(Crate, transforms, GetEntity());
				CratesSpawned = static_cast<int32_t>(crates.size());
				Expect(crates.size() == 3, "Scene::InstantiateMany(handle)");
				if (!crates.empty())
				{
					FirstCrate = crates[0];
					Expect(crates[2].GetTransform().GetTranslation() == glm::vec3(4.0f, 1.0f, 0.0f), "each instance gets its transform");
					Expect(crates[1].GetParent() == GetEntity(), "the instances are under the parent");
				}
				Expect(Scene::InstantiateMany("Prefabs/Crate.stprefab", { glm::mat4(1.0f) }).size() == 1, "Scene::InstantiateMany(path)");
				Phase = 2;
				break;
			}
			case 2:
			{
				Expect(m_Room.Unload(), "SubScene::Unload");
				Expect(!m_Room.IsLoaded(), "an unloaded sub-scene is not loaded");
				Expect(RoomRoot.IsValid(), "its entities go at the end of the frame");
				Phase = 3;
				break;
			}
			case 3:
			{
				Expect(!RoomRoot.IsValid(), "an unloaded sub-scene's entities are gone");
				Expect(Assets::Release(Crate), "Assets::Release ends the closure's request");
				Done = true;
				Phase = 4;
				break;
			}
			default:
				break;
		}
	}
private:
	SubScene m_Room;
};

ST_SCRIPT_CLASS(SubSceneStreamer)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Room);
	ST_SCRIPT_FIELD(RoomPath);
	ST_SCRIPT_FIELD(Crate);
	ST_SCRIPT_FIELD(Phase);
	ST_SCRIPT_FIELD(LoadingFrames);
	ST_SCRIPT_FIELD(CrateProgress);
	ST_SCRIPT_FIELD(SwitchProgress);
	ST_SCRIPT_FIELD(FirstCrate);
	ST_SCRIPT_FIELD(RoomRoot);
	ST_SCRIPT_FIELD(CratesSpawned);
	ST_SCRIPT_FIELD(Done);
}

// What the streaming functions refuse, and the functions on an engine that predates them (a host table that ends before
// them: nothing happens).
class StreamingMisuse : public CheckingScript
{
public:
	AssetHandle NotAScene; // An asset of another type
	AssetHandle Crate;     // A prefab, not loaded
	bool Done = false;

	void OnUpdate(float) override
	{
		if (Done)
			return;
		Done = true;

		Expect(!Scene::LoadAdditive(AssetHandle()).IsValid(), "the null asset is no scene");
		Expect(!Scene::LoadAdditive(NotAScene).IsValid(), "an asset that is not a scene");
		Expect(!Scene::LoadAdditive("Levels/Missing.stscene").IsValid(), "a path without an asset");
		Expect(!SubScene(0x5B5CE).IsLoaded(), "an unknown sub-scene is not loaded");
		Expect(!SubScene(0x5B5CE).Unload(), "an unknown sub-scene cannot be unloaded");
		Expect(Assets::GetLoadProgress(AssetHandle(0x51DE5)) < 0.0f, "the progress of an asset that does not exist");
		Expect(Scene::InstantiateMany(NotAScene, { glm::mat4(1.0f) }).empty(), "InstantiateMany of an asset that is no prefab");
		Expect(Scene::InstantiateMany(Crate, {}).empty(), "InstantiateMany without transforms");
		const glm::mat4 flat = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 1.0f));
		Expect(Scene::InstantiateMany(Crate, { glm::mat4(1.0f), flat }).empty(), "InstantiateMany with a degenerate transform creates none");

		const StrataScriptHostAPI* host = Detail::GetHost();
		StrataScriptHostAPI older = *host;
		older.StructSize = static_cast<uint32_t>(offsetof(StrataScriptHostAPI, LoadSceneAdditive));
		Detail::s_Host = &older;
		const bool loaded = Scene::LoadAdditive(NotAScene).IsValid();
		const float progress = Assets::GetLoadProgress(Crate);
		const float switchProgress = Game::GetSceneLoadProgress();
		const bool spawned = !Scene::InstantiateMany(Crate, { glm::mat4(1.0f) }).empty();
		Detail::s_Host = host;
		Expect(!loaded && progress < 0.0f && switchProgress < 0.0f && !spawned, "an engine without the functions does nothing");
	}
};

ST_SCRIPT_CLASS(StreamingMisuse)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(NotAScene);
	ST_SCRIPT_FIELD(Crate);
	ST_SCRIPT_FIELD(Done);
}
