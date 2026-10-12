#include <doctest/doctest.h>

#include "Audio/AudioTestUtils.h"
#include "Perf/SceneGenerators.h"
#include "Scene/SceneLoaderTestUtils.h"
#include "Scene/SceneTestUtils.h"
#include "Scripting/ScriptTestUtils.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Audio/AudioSystem.h"
#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/Scene/SubScene.h"
#include "TestHelpers.h"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr float c_FrameTime = 1.0f / 60.0f;

	// A prepared scene of `count` entities nested ten to a parent (Perf::CreateNestedHierarchy), its world transforms current.
	Ref<Scene> MakeNestedScene(size_t count, const std::string& name = "Sub")
	{
		Ref<Scene> scene = CreateRef<Scene>(name);
		Perf::CreateNestedHierarchy(*scene, count);
		scene->UpdateWorldTransforms();
		return scene;
	}

	// Updates the scene (the sub-scenes first) until the sub-scene is loaded or failed, recording the entities each update
	// created. REQUIREs that it finishes.
	std::vector<size_t> UpdateUntilSettled(Scene& scene, SubSceneID id, const std::function<void()>& beforeUpdate = {})
	{
		std::vector<size_t> created;
		REQUIRE(WaitUntil([&]()
		{
			if (beforeUpdate)
				beforeUpdate();
			const size_t before = scene.GetEntityCount();
			scene.OnUpdateRuntime(c_FrameTime);
			if (scene.GetEntityCount() > before)
				created.push_back(scene.GetEntityCount() - before);
			const SubSceneStatus* status = scene.GetSubSceneStatus(id);
			REQUIRE(status);
			return status->State == SubSceneState::Loaded || status->State == SubSceneState::Failed;
		}, std::chrono::seconds(60)));
		return created;
	}

	uint32_t CountProxies(const RenderScene& renderScene)
	{
		uint32_t count = 0;
		for (uint32_t proxy = 0; proxy < renderScene.GetProxyCapacity(); proxy++)
		{
			if (renderScene.GetEntity(proxy) != entt::null)
				count++;
		}
		return count;
	}

	size_t CountSubSceneTags(const Scene& scene)
	{
		const auto* tags = scene.GetRegistry().storage<SubSceneComponent>();
		return tags ? tags->size() : 0;
	}

	// Unloads a sub-scene the first time an entity of the scene is destroyed, as a script's OnDestroy might.
	struct UnloadOnDestroy
	{
		Scene& Target;
		SubSceneID SubScene = 0;
		bool Unloaded = false;
		entt::connection Connection;

		void OnDestroyed(entt::registry&, entt::entity)
		{
			if (!Unloaded)
				Unloaded = Target.UnloadSubScene(SubScene);
		}

		void Connect() { Connection = Target.GetRegistry().on_destroy<NameComponent>().connect<&UnloadOnDestroy::OnDestroyed>(*this); }
		void Disconnect() { Connection.release(); }
	};

	size_t CountOccurrences(const std::string& text, const std::string& part)
	{
		size_t count = 0;
		for (size_t position = text.find(part); position != std::string::npos; position = text.find(part, position + part.size()))
			count++;
		return count;
	}

}

TEST_SUITE("Scene.Streaming")
{
	TEST_CASE("An additive load of 20,000 entities is created in slices of at most the per-frame limit")
	{
		ScopedJobSystem jobs(2, 1);
		const Ref<Scene> running = CreateRef<Scene>("Running");
		Scene& scene = *running;
		const Entity existing = scene.CreateEntity("Existing");
		REQUIRE(scene.GetSettings().MaxEntitiesInstantiatedPerFrame == 5000);

		Ref<Scene> sub = MakeNestedScene(20'000);
		const UUID subRootID = sub->GetEntitiesInHierarchyOrder().front().GetUUID();
		const SubSceneID id = scene.BeginAdditiveLoad(std::move(sub));
		REQUIRE(id != 0);
		REQUIRE(scene.GetSubSceneStatus(id));
		CHECK(scene.GetSubSceneStatus(id)->State == SubSceneState::Loading);
		CHECK_FALSE(scene.IsSubSceneLoaded(id));
		CHECK(scene.HasPendingSubScenes());

		const std::vector<size_t> created = UpdateUntilSettled(scene, id);
		const SubSceneStatus& status = *scene.GetSubSceneStatus(id);
		REQUIRE(status.State == SubSceneState::Loaded);
		CHECK(scene.IsSubSceneLoaded(id));
		CHECK_FALSE(scene.HasPendingSubScenes());
		CHECK(created.size() >= 4);
		for (const size_t count : created)
			CHECK(count <= 5000);
		CHECK(status.Slices == created.size());
		CHECK(status.CreatedEntities == 20'000);
		CHECK(status.TotalEntities == 20'000);
		CHECK(status.Progress == 1.0f);
		CHECK(scene.GetEntityCount() == 20'001);

		// The entities keep their IDs; the top-level one names its sub-scene and follows the existing entity.
		const std::vector<Entity> roots = scene.GetSubSceneRoots(id);
		REQUIRE(roots.size() == 1);
		CHECK(roots[0].GetUUID() == subRootID);
		CHECK(scene.GetRegistry().get<SubSceneComponent>(roots[0].GetHandle()).SubScene == id);
		CHECK(scene.GetRootEntities() == std::vector<UUID> { existing.GetUUID(), subRootID });
		CheckSceneCaches(scene);

		// The scene's setting bounds every slice; it is saved with the scene.
		scene.GetSettings().MaxEntitiesInstantiatedPerFrame = 1500;
		const SubSceneID second = scene.BeginAdditiveLoad(MakeNestedScene(6000, "Second"));
		for (const size_t count : UpdateUntilSettled(scene, second))
			CHECK(count <= 1500);
		CHECK(scene.GetSubSceneStatus(second)->Slices >= 4);
		std::string error;
		const Ref<Scene> reloaded = CreateRef<Scene>();
		REQUIRE_MESSAGE(SceneSerializer::Deserialize(*reloaded, SceneSerializer::Serialize(scene), &error), error);
		CHECK(reloaded->GetSettings().MaxEntitiesInstantiatedPerFrame == 1500);
		// Sub-scene tags are runtime only: neither saved nor copied.
		CHECK(CountSubSceneTags(scene) == 2);
		CHECK(CountSubSceneTags(*reloaded) == 0);
		CHECK(CountSubSceneTags(*Scene::Copy(running)) == 0);

		// Unloading removes exactly what each brought.
		CHECK(scene.UnloadSubScene(id));
		CHECK_FALSE(scene.UnloadSubScene(id));
		CHECK(scene.GetSubSceneStatus(id) == nullptr);
		CHECK(scene.GetEntityCount() == 6001);
		CHECK(scene.UnloadSubScene(second));
		CHECK(scene.GetEntityCount() == 1);
		CHECK(scene.GetSubScenes().empty());
		CheckSceneCaches(scene);
	}

	TEST_CASE("Unloading a sub-scene leaves none of its entities, script instances, bodies, voices or render proxies")
	{
		ScopedJobSystem jobs(2, 1);
		ScopedAudioEngine audio;
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		constexpr size_t c_Streamed = 12;

		// Each entity of both scenes has a script (logging only its creation and destruction), a body, an audio source and a mesh.
		const auto equip = [](Entity entity)
		{
			AddFieldOverride(AddScriptEntry(entity, "Lifecycle"), "RecordUpdates", PropertyType::Bool, false);
			entity.AddComponent<RigidBodyComponent>().GravityScale = 0.0f;
			entity.AddComponent<BoxColliderComponent>();
			entity.AddComponent<AudioSourceComponent>();
			entity.AddComponent<MeshRendererComponent>().Mesh = BuiltinAssets::CubeMesh;
		};
		Ref<Scene> sub = CreateRef<Scene>("Room");
		for (size_t index = 0; index < c_Streamed; index++)
		{
			Entity entity = sub->CreateEntity("Streamed" + std::to_string(index));
			entity.GetComponent<TransformComponent>().Translation = glm::vec3(static_cast<float>(index) * 3.0f, 0.0f, 10.0f);
			equip(entity);
		}

		Scene scene("Level");
		CreateLogEntity(scene);
		Entity resident = scene.CreateEntity("Resident");
		equip(resident);
		scene.OnRuntimeStart();
		RenderScene& renderScene = RenderScene::Get(scene);
		const auto frame = [&]()
		{
			scene.OnUpdateRuntime(c_FrameTime);
			renderScene.Sync(nullptr);
		};
		frame();
		ScriptSystem& scripts = GetScriptSystem(scene);
		PhysicsSystem& physics = *scene.GetSystem<PhysicsSystem>();
		AudioSystem& sounds = *scene.GetSystem<AudioSystem>();
		const size_t entities = scene.GetEntityCount();
		const size_t instances = scripts.GetInstanceCount();
		const uint32_t bodies = physics.GetStats().BodyCount;
		const uint32_t voices = sounds.GetStats().SourceCount;
		const uint32_t proxies = CountProxies(renderScene);
		CHECK(instances == 1);
		CHECK(bodies == 1);
		CHECK(voices == 1);
		CHECK(proxies == 1);
		ClearLog(scene);

		// Streamed in, the entities behave like any other: OnCreate, a body, a voice and a proxy each.
		const SubSceneID id = scene.BeginAdditiveLoad(sub);
		UpdateUntilSettled(scene, id, [&]() { renderScene.Sync(nullptr); });
		REQUIRE(scene.IsSubSceneLoaded(id));
		frame();
		frame();
		CHECK(scene.GetEntityCount() == entities + c_Streamed);
		CHECK(scripts.GetInstanceCount() == instances + c_Streamed);
		CHECK(physics.GetStats().BodyCount == bodies + c_Streamed);
		CHECK(sounds.GetStats().SourceCount == voices + c_Streamed);
		CHECK(CountProxies(renderScene) == proxies + c_Streamed);
		CHECK(CountOccurrences(GetLog(scene), ".Lifecycle.Create;") == c_Streamed);
		CHECK(GetLog(scene).find("Streamed0.Lifecycle.Create;") != std::string::npos);

		// Unloaded, nothing of them remains, and their scripts got OnDestroy.
		ClearLog(scene);
		CHECK(scene.UnloadSubScene(id));
		frame();
		CHECK(scene.GetEntityCount() == entities);
		CHECK(scripts.GetInstanceCount() == instances);
		CHECK(physics.GetStats().BodyCount == bodies);
		CHECK(sounds.GetStats().SourceCount == voices);
		CHECK(CountProxies(renderScene) == proxies);
		CHECK(CountOccurrences(GetLog(scene), ".Lifecycle.Destroy;") == c_Streamed);
		CHECK(GetLog(scene).find("Resident.Lifecycle.Destroy") == std::string::npos);
		CHECK(scene.GetEntityByUUID(resident.GetUUID()));
		std::string error;
		CHECK_MESSAGE(renderScene.Validate(&error), error);
		CheckSceneCaches(scene);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A sub-scene whose IDs the scene has fails with an error and changes nothing")
	{
		ScopedJobSystem jobs(2, 1);
		Scene scene("Running");
		scene.CreateEntity("A");
		scene.CreateEntity("B");
		scene.UpdateWorldTransforms();
		const nlohmann::json before = SceneSerializer::Serialize(scene);

		// One of its 300 entities has the ID of the scene's second entity.
		Ref<Scene> sub = MakeNestedScene(300);
		const UUID taken = scene.GetEntitiesInHierarchyOrder()[1].GetUUID();
		sub->CreateEntityWithUUID(taken, "Twin");
		const SubSceneID id = scene.BeginAdditiveLoad(sub);
		UpdateUntilSettled(scene, id);
		const SubSceneStatus* status = scene.GetSubSceneStatus(id);
		REQUIRE(status);
		CHECK(status->State == SubSceneState::Failed);
		CHECK(status->Error.find(taken.ToString()) != std::string::npos);
		CHECK_FALSE(scene.IsSubSceneLoaded(id));
		CHECK(status->CreatedEntities == 0);
		CHECK(SceneSerializer::Serialize(scene) == before);
		CHECK(CountSubSceneTags(scene) == 0);
		CheckSceneCaches(scene);
		// A failed sub-scene stays, with its error, until it is unloaded.
		CHECK(scene.GetSubScenes().size() == 1);
		CHECK(scene.UnloadSubScene(id));
		CHECK(scene.GetSubScenes().empty());

		// An ID the scene gets while the sub-scene is being created fails it at that slice: what it created so far goes too.
		scene.GetSettings().MaxEntitiesInstantiatedPerFrame = 100;
		const Ref<Scene> late = MakeNestedScene(300, "Late");
		const UUID lastID = late->GetEntitiesInHierarchyOrder().back().GetUUID();
		const SubSceneID lateID = scene.BeginAdditiveLoad(late);
		bool interfered = false;
		UpdateUntilSettled(scene, lateID, [&]()
		{
			const SubSceneStatus* lateStatus = scene.GetSubSceneStatus(lateID);
			if (!interfered && lateStatus->CreatedEntities > 0)
			{
				scene.CreateEntityWithUUID(lastID, "Interloper");
				interfered = true;
			}
		});
		CHECK(interfered);
		CHECK(scene.GetSubSceneStatus(lateID)->State == SubSceneState::Failed);
		CHECK(scene.GetSubSceneStatus(lateID)->Error.find(lastID.ToString()) != std::string::npos);
		CHECK(scene.GetEntityCount() == 3);
		CHECK(scene.GetEntityByUUID(lastID).GetName() == "Interloper");
		CHECK(CountSubSceneTags(scene) == 0);
		CheckSceneCaches(scene);

		// What reacts to a failed sub-scene's entities going (scripts' OnDestroy) may unload sub-scenes: the unload waits until
		// the sub-scenes' update is done.
		const SubSceneID resident = scene.BeginAdditiveLoad(MakeNestedScene(10, "Resident"));
		UpdateUntilSettled(scene, resident);
		REQUIRE(scene.IsSubSceneLoaded(resident));
		const Ref<Scene> doomed = MakeNestedScene(300, "Doomed");
		const UUID doomedLast = doomed->GetEntitiesInHierarchyOrder().back().GetUUID();
		const SubSceneID doomedID = scene.BeginAdditiveLoad(doomed);
		UnloadOnDestroy listener { scene, resident };
		bool collided = false;
		UpdateUntilSettled(scene, doomedID, [&]()
		{
			if (!collided && scene.GetSubSceneStatus(doomedID)->CreatedEntities > 0)
			{
				scene.CreateEntityWithUUID(doomedLast, "Collider");
				listener.Connect();
				collided = true;
			}
		});
		listener.Disconnect();
		CHECK(scene.GetSubSceneStatus(doomedID)->State == SubSceneState::Failed);
		CHECK(listener.Unloaded);
		CHECK(scene.GetSubSceneStatus(resident) == nullptr);
		CHECK(scene.GetEntityCount() == 4);
		CHECK(CountSubSceneTags(scene) == 0);
		CheckSceneCaches(scene);

		// Nothing to add, or an unknown sub-scene.
		std::string error;
		CHECK(scene.BeginAdditiveLoad(nullptr, &error) == 0);
		CHECK_FALSE(error.empty());
		CHECK_FALSE(scene.UnloadSubScene(12345));
		CHECK(scene.GetSubSceneRoots(12345).empty());
	}

	TEST_CASE("Entities whose parent was destroyed after an earlier slice created it are not created")
	{
		ScopedJobSystem jobs(2, 1);
		Scene scene("Running");
		const Entity resident = scene.CreateEntity("Resident");
		scene.UpdateWorldTransforms();

		// A room (Room) with two groups of 50 entities each (GroupA, GroupB).
		Ref<Scene> sub = CreateRef<Scene>("Rooms");
		const Entity room = sub->CreateEntity("Room");
		const Entity groupA = sub->CreateChildEntity(room, "GroupA");
		const Entity groupB = sub->CreateChildEntity(room, "GroupB");
		for (int index = 0; index < 50; index++)
		{
			sub->CreateChildEntity(groupA, "A" + std::to_string(index));
			sub->CreateChildEntity(groupB, "B" + std::to_string(index));
		}
		sub->UpdateWorldTransforms();
		const UUID roomID = room.GetUUID();
		const UUID groupAID = groupA.GetUUID();
		const UUID groupBID = groupB.GetUUID();

		// Two entities per update: gameplay destroys GroupA once it exists, before its children's slices.
		scene.GetSettings().MaxEntitiesInstantiatedPerFrame = 2;
		const SubSceneID id = scene.BeginAdditiveLoad(sub);
		bool destroyed = false;
		UpdateUntilSettled(scene, id, [&]()
		{
			const Entity group = scene.GetEntityByUUID(groupAID);
			if (!destroyed && group)
			{
				scene.DestroyEntity(group);
				destroyed = true;
			}
		});
		REQUIRE(destroyed);
		const SubSceneStatus* status = scene.GetSubSceneStatus(id);
		REQUIRE(status);
		CHECK(status->State == SubSceneState::Loaded);
		CHECK(scene.IsSubSceneLoaded(id));
		CHECK(status->TotalEntities == 103);
		CHECK(status->SkippedEntities == 50);
		CHECK(status->CreatedEntities == 53);
		// The room, GroupB and its 50 entities are there; GroupA and everything below it are not.
		CHECK(scene.GetEntityCount() == 1 + 52);
		CHECK_FALSE(scene.GetEntityByUUID(groupAID));
		CHECK_FALSE(scene.FindEntityByName("A0"));
		CHECK_FALSE(scene.FindEntityByName("A49"));
		const Entity streamedGroupB = scene.GetEntityByUUID(groupBID);
		REQUIRE(streamedGroupB);
		CHECK(streamedGroupB.GetParent() == scene.GetEntityByUUID(roomID));
		CHECK(scene.FindEntityByName("B49").GetParent() == streamedGroupB);
		CheckSceneCaches(scene);
		CHECK(scene.UnloadSubScene(id));
		CHECK(scene.GetEntityCount() == 1);
		CheckSceneCaches(scene);

		// The top-level entity destroyed after the first slice: nothing else of the sub-scene is created, and it counts as
		// loaded (there is nothing left to create).
		scene.GetSettings().MaxEntitiesInstantiatedPerFrame = 1;
		Ref<Scene> nested = MakeNestedScene(300, "Nested");
		const UUID nestedRoot = nested->GetEntitiesInHierarchyOrder().front().GetUUID();
		const SubSceneID second = scene.BeginAdditiveLoad(std::move(nested));
		destroyed = false;
		UpdateUntilSettled(scene, second, [&]()
		{
			const Entity root = scene.GetEntityByUUID(nestedRoot);
			if (!destroyed && root)
			{
				scene.DestroyEntity(root);
				destroyed = true;
			}
		});
		REQUIRE(destroyed);
		const SubSceneStatus* secondStatus = scene.GetSubSceneStatus(second);
		REQUIRE(secondStatus);
		CHECK(secondStatus->State == SubSceneState::Loaded);
		CHECK(secondStatus->CreatedEntities == 1);
		CHECK(secondStatus->SkippedEntities == 299);
		CHECK(secondStatus->Progress == 1.0f);
		CHECK(scene.GetEntityCount() == 1);
		CHECK(scene.GetRootEntities() == std::vector<UUID> { resident.GetUUID() });
		CheckSceneCaches(scene);
	}

	TEST_CASE("Cancelling a sub-scene mid-load leaves the scene unchanged and releases every pin")
	{
		SceneLoaderFixture fixture;
		FakeAssetManager& manager = *fixture.Manager;
		Scene scene("Running");
		scene.CreateEntity("Resident");
		scene.UpdateWorldTransforms();
		// The scene's entities as saved (its settings change below).
		const auto saveEntities = [&]() { return SceneSerializer::Serialize(scene)["Scene"]["Entities"]; };
		const nlohmann::json before = saveEntities();
		const auto frame = [&]()
		{
			manager.Update();
			scene.OnUpdateRuntime(c_FrameTime);
		};
		const auto pinCounts = [&]()
		{
			uint32_t pins = manager.GetPinCount(fixture.SceneHandle);
			for (const AssetHandle dependency : fixture.Dependencies)
				pins += manager.GetPinCount(dependency);
			return pins;
		};

		// While loading: the scene asset and what it uses are pinned; cancelled, none is, and the scene never changed.
		manager.CloseGate();
		std::string error;
		const SubSceneID loading = scene.LoadSubScene(fixture.SceneHandle, {}, &error);
		REQUIRE_MESSAGE(loading != 0, error);
		CHECK(pinCounts() == 11);
		manager.AllowReads(3);
		REQUIRE(WaitUntil([&]()
		{
			frame();
			return scene.GetSubSceneStatus(loading)->Progress > 0.0f;
		}, std::chrono::seconds(10)));
		CHECK(scene.GetSubSceneStatus(loading)->State == SubSceneState::Loading);
		CHECK(scene.GetSubSceneStatus(loading)->Progress < 0.5f);
		CHECK(scene.UnloadSubScene(loading));
		CHECK(pinCounts() == 0);
		CHECK(scene.GetSubSceneStatus(loading) == nullptr);
		manager.OpenGate();
		REQUIRE(manager.WaitForPendingLoads());
		frame();
		CHECK(manager.GetStats().PinnedAssets == 0);
		CHECK(StateCountsMatch(manager));
		CHECK(saveEntities() == before);

		// While its entities are being created (two per update): the pins hold until all exist; cancelled, the entities
		// created so far go and so do the pins.
		scene.GetSettings().MaxEntitiesInstantiatedPerFrame = 2;
		const SubSceneID creating = scene.LoadSubScene(fixture.SceneHandle);
		REQUIRE(creating != 0);
		REQUIRE(WaitUntil([&]()
		{
			frame();
			return scene.GetSubSceneStatus(creating)->CreatedEntities > 0;
		}, std::chrono::seconds(10)));
		CHECK(scene.GetSubSceneStatus(creating)->State == SubSceneState::Instantiating);
		CHECK(scene.GetEntityCount() == 3);
		CHECK(pinCounts() == 11);
		CHECK(scene.UnloadSubScene(creating));
		CHECK(pinCounts() == 0);
		CHECK(manager.GetStats().PinnedAssets == 0);
		CHECK(StateCountsMatch(manager));
		frame();
		CHECK(saveEntities() == before);
		CheckSceneCaches(scene);

		// Loaded completely, its entities are the scene's; its pins went once the last one existed.
		const SubSceneID loaded = scene.LoadSubScene(fixture.SceneHandle);
		REQUIRE(WaitUntil([&]()
		{
			frame();
			return scene.IsSubSceneLoaded(loaded);
		}, std::chrono::seconds(10)));
		CHECK(scene.GetEntityCount() == 6);
		CHECK(scene.GetSubSceneStatus(loaded)->Scene == fixture.SceneHandle);
		CHECK(scene.GetSubSceneStatus(loaded)->Slices == 3);
		CHECK(pinCounts() == 0);
		const std::vector<Entity> roots = scene.GetSubSceneRoots(loaded);
		REQUIRE(roots.size() == 5);
		CHECK(scene.GetRegistry().get<SubSceneComponent>(roots[0].GetHandle()).Source == fixture.SceneHandle);
		CHECK(roots[4].GetComponent<TransformComponent>().Translation.x == 4.0f);
		CHECK(scene.UnloadSubScene(loaded));
		CHECK(saveEntities() == before);

		// What cannot load fails at once, adding nothing.
		CHECK(scene.LoadSubScene(fixture.Dependencies[0], {}, &error) == 0);
		CHECK(error.find("not a scene asset") != std::string::npos);
		CHECK(scene.GetSubScenes().empty());
	}
}
