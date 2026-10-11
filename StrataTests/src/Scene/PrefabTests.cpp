#include <doctest/doctest.h>

#include "Scene/SceneTestUtils.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/EntityPrototype.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/Scene/UnknownComponents.h"

#include <glm/gtc/matrix_transform.hpp>

#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace Strata;

namespace
{

	std::vector<uint8_t> ToBytes(const nlohmann::json& json)
	{
		const std::string text = JsonUtils::Dump(json);
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	// Root "Turret" with a child "Barrel"; both carry components with non-default values.
	Ref<Scene> CreateTurretScene(Entity& outRoot)
	{
		Ref<Scene> scene = CreateRef<Scene>();
		outRoot = scene->CreateEntity("Turret");
		outRoot.GetComponent<TransformComponent>().Translation = { 1.0f, 2.0f, 3.0f };
		outRoot.AddComponent<TagComponent>().Tag = "Enemy";
		Entity barrel = scene->CreateEntity("Barrel");
		scene->SetParent(barrel, outRoot);
		barrel.GetComponent<TransformComponent>().Translation = { 0.0f, 0.5f, 1.0f };
		barrel.AddComponent<PointLightComponent>().Intensity = 7.0f;
		return scene;
	}

}

TEST_SUITE("Scene.Prefab")
{
	TEST_CASE("Prefabs round trip and instantiate with new ids linked to the asset")
	{
		Entity root;
		Ref<Scene> source = CreateTurretScene(root);
		Ref<Prefab> prefab = Prefab::CreateFromEntities(*source, { root });
		REQUIRE(prefab);
		CHECK(prefab->GetEntityCount() == 2);

		std::string error;
		Ref<Prefab> loaded = Prefab::Deserialize(std::span<const uint8_t>(ToBytes(prefab->Serialize())), &error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetEntityCount() == 2);
		loaded->Handle = UUID(0x4242);

		Ref<Scene> target = CreateRef<Scene>();
		Entity parent = target->CreateEntity("Spawner");
		for (int copy = 0; copy < 2; copy++)
		{
			std::vector<Entity> roots = loaded->Instantiate(*target, parent);
			REQUIRE(roots.size() == 1);
			Entity instance = roots[0];
			CHECK(instance.GetName() == "Turret");
			CHECK(instance.GetParent() == parent);
			CHECK(instance.GetUUID() != root.GetUUID());
			CHECK(instance.GetComponent<TransformComponent>().Translation == glm::vec3(1.0f, 2.0f, 3.0f));
			CHECK(instance.GetComponent<TagComponent>().Tag == "Enemy");
			REQUIRE(instance.HasComponent<PrefabInstanceComponent>());
			CHECK(instance.GetComponent<PrefabInstanceComponent>().Prefab == UUID(0x4242));
			CHECK(instance.GetComponent<PrefabInstanceComponent>().PrefabEntityID == root.GetUUID());

			const std::vector<Entity> children = instance.GetChildren();
			REQUIRE(children.size() == 1);
			CHECK(children[0].GetName() == "Barrel");
			CHECK(children[0].GetComponent<PointLightComponent>().Intensity == 7.0f);
			CHECK(children[0].GetComponent<PrefabInstanceComponent>().Prefab == UUID(0x4242));
		}
		CHECK(target->GetEntityCount() == 5);
	}

	TEST_CASE("Prefab and model documents are validated")
	{
		std::string error;
		CHECK_FALSE(Prefab::Deserialize(std::span<const uint8_t>(), &error));
		CHECK_FALSE(error.empty());

		const nlohmann::json wrongFormat = { { "Strata", { { "Format", "Model" }, { "Version", 1 } } }, { "Prefab", { { "Entities", nlohmann::json::array() } } } };
		CHECK_FALSE(Prefab::Deserialize(std::span<const uint8_t>(ToBytes(wrongFormat)), &error));
		const nlohmann::json futureVersion = { { "Strata", { { "Format", "Prefab" }, { "Version", 99 } } }, { "Prefab", { { "Entities", nlohmann::json::array() } } } };
		CHECK_FALSE(Prefab::Deserialize(std::span<const uint8_t>(ToBytes(futureVersion)), &error));
		const nlohmann::json noEntities = { { "Strata", { { "Format", "Prefab" }, { "Version", 1 } } }, { "Prefab", nlohmann::json::object() } };
		CHECK_FALSE(Prefab::Deserialize(std::span<const uint8_t>(ToBytes(noEntities)), &error));

		// Entity data is validated when the prefab loads, not when it is first instantiated.
		nlohmann::json duplicateIds = { { "Strata", { { "Format", "Prefab" }, { "Version", 1 } } },
			{ "Prefab", { { "Entities", nlohmann::json::array({ { { "ID", "0000000000000005" } }, { { "ID", "0000000000000005" } } }) } } } };
		error.clear();
		CHECK_FALSE(Prefab::Deserialize(std::span<const uint8_t>(ToBytes(duplicateIds)), &error));
		CHECK(error.find("prefab") != std::string::npos);

		Entity root;
		Ref<Scene> source = CreateTurretScene(root);
		Ref<Model> model = Model::CreateFromSnapshot(SceneSerializer::SerializeEntities(*source, { root }));
		REQUIRE(model);
		Ref<Model> loadedModel = Model::Deserialize(ToBytes(model->Serialize()), &error);
		REQUIRE_MESSAGE(loadedModel, error);
		CHECK(loadedModel->GetEntityCount() == 2);
		CHECK_FALSE(Model::Deserialize(std::span<const uint8_t>(ToBytes(Prefab::CreateFromEntities(*source, { root })->Serialize())), &error));
	}

	TEST_CASE("Scene assets validate documents and create independent scenes")
	{
		Entity root;
		Ref<Scene> source = CreateTurretScene(root);
		const std::vector<uint8_t> document = ToBytes(SceneSerializer::Serialize(*source));

		std::string error;
		Ref<SceneAsset> asset = SceneAsset::Deserialize(document, &error);
		REQUIRE_MESSAGE(asset, error);
		Ref<Scene> first = asset->CreateScene(&error);
		Ref<Scene> second = asset->CreateScene(&error);
		REQUIRE(first);
		REQUIRE(second);
		CHECK(first != second);
		CHECK(first->GetEntityCount() == 2);
		Entity loadedRoot = first->GetEntityByUUID(root.GetUUID());
		REQUIRE(loadedRoot);
		CHECK(loadedRoot.GetComponent<TransformComponent>().Translation == glm::vec3(1.0f, 2.0f, 3.0f));

		const std::string broken = "{ \"Strata\": { \"Format\": \"Scene\", \"Version\": 1 }, \"Scene\": { \"Entities\": [ { \"ID\": \"0000000000000005\" }, { \"ID\": \"0000000000000005\" } ] } }";
		CHECK_FALSE(SceneAsset::Deserialize(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(broken.data()), broken.size()), &error));
		CHECK_FALSE(SceneAsset::Deserialize(std::vector<uint8_t> { '{' }, &error));
	}

	TEST_CASE("A scene asset reads its document once and hands the scene it prepared over once")
	{
		Entity root;
		Ref<Scene> source = CreateTurretScene(root);
		source->GetSettings().Gravity = glm::vec3(0.0f, -3.0f, 0.0f);
		nlohmann::json json = SceneSerializer::Serialize(*source);
		// Something this build cannot read as written, which the asset reports.
		json["Scene"]["Entities"][0]["Components"]["Transform"]["Wobble"] = 1.0f;
		const std::vector<uint8_t> document = ToBytes(json);

		const uint64_t reads = SceneSerializer::GetDeserializeCount();
		std::string error;
		Ref<SceneAsset> asset = SceneAsset::Deserialize(document, &error);
		REQUIRE_MESSAGE(asset, error);
		asset->Handle = UUID(0x5150);
		CHECK(SceneSerializer::GetDeserializeCount() == reads + 1);
		CHECK_FALSE(asset->IsTaken());
		CHECK(asset->GetMemoryUsage().Cpu > 0);
		REQUIRE(asset->GetWarnings().size() == 1);
		CHECK(asset->GetWarnings()[0].find("Wobble") != std::string::npos);

		// Tools make copies, which leave the prepared scene with the asset.
		const Ref<Scene> copy = asset->CreateScene(&error);
		REQUIRE_MESSAGE(copy, error);
		CHECK(copy->GetSourceAsset() == asset->Handle);
		CHECK_FALSE(asset->IsTaken());

		// The loader's hand-over: the prepared scene itself, read once, with its source asset and settings.
		const Ref<Scene> taken = asset->TakePreparedScene(&error);
		REQUIRE_MESSAGE(taken, error);
		CHECK(taken != copy);
		CHECK(SceneSerializer::GetDeserializeCount() == reads + 1);
		CHECK(taken->GetSourceAsset() == asset->Handle);
		CHECK(taken->GetLineageID() == asset->Handle);
		CHECK(taken->GetSettings().Gravity == glm::vec3(0.0f, -3.0f, 0.0f));
		CHECK(taken->GetEntityCount() == 2);
		Entity takenRoot = taken->GetEntityByUUID(root.GetUUID());
		REQUIRE(takenRoot);
		CHECK(takenRoot.GetComponent<TagComponent>().Tag == "Enemy");
		CHECK(takenRoot.GetChildren().size() == 1);
		std::string cachesError;
		CHECK_MESSAGE(taken->ValidateHierarchy(&cachesError), cachesError);

		// Spent: the asset holds nothing and refuses a second hand-over (whoever took it loads the asset again).
		CHECK(asset->IsTaken());
		CHECK(asset->GetMemoryUsage().Cpu == 0);
		error.clear();
		CHECK_FALSE(asset->TakePreparedScene(&error));
		CHECK(error.find("taken") != std::string::npos);
		error.clear();
		CHECK_FALSE(asset->CreateScene(&error));
		CHECK_FALSE(error.empty());
	}

	TEST_CASE("A scene asset accounts for the scene it holds")
	{
		const auto makeAsset = [](int entities)
		{
			Scene scene("Counted");
			for (int index = 0; index < entities; index++)
				scene.CreateEntity("A longer entity name that does not fit in a short string " + std::to_string(index));
			const std::vector<uint8_t> document = ToBytes(SceneSerializer::Serialize(scene));
			std::string error;
			Ref<SceneAsset> asset = SceneAsset::Deserialize(document, &error);
			REQUIRE_MESSAGE(asset, error);
			return asset;
		};
		const Ref<SceneAsset> small = makeAsset(10);
		const Ref<SceneAsset> large = makeAsset(10'000);
		// At least the components every entity has, and the names, which do not fit in the string objects.
		const uint64_t perEntity = sizeof(IDComponent) + sizeof(NameComponent) + sizeof(TransformComponent) + sizeof(RelationshipComponent)
			+ sizeof(WorldTransformComponent) + 56;
		CHECK(large->GetMemoryUsage().Cpu > 10'000 * perEntity);
		CHECK(small->GetMemoryUsage().Cpu > 10 * perEntity);
		CHECK(large->GetMemoryUsage().Cpu > small->GetMemoryUsage().Cpu + 9'990 * perEntity);
		CHECK(large->GetMemoryUsage().GetGpu() == 0);
	}

	TEST_CASE("Prefabs compile their snapshot once and write it back unchanged")
	{
		Entity root;
		Ref<Scene> source = CreateTurretScene(root);
		const nlohmann::json snapshot = SceneSerializer::SerializeEntities(*source, { root });
		const Ref<Prefab> prefab = Prefab::CreateFromSnapshot(snapshot);
		REQUIRE(prefab);
		const Ref<EntityPrototype>& prototype = prefab->GetPrototype();
		REQUIRE(prototype);
		REQUIRE(prototype->GetEntityCount() == 2);
		// Hierarchy order, the parent table and the snapshot's IDs (which instances link to).
		CHECK(prototype->GetEntityID(0) == root.GetUUID());
		CHECK(prototype->GetParentIndices()[0] == EntityPrototype::c_NoParent);
		CHECK(prototype->GetParentIndices()[1] == 0);
		CHECK(std::vector<uint32_t>(prototype->GetTopLevelIndices().begin(), prototype->GetTopLevelIndices().end()) == std::vector<uint32_t> { 0 });
		CHECK(prototype->FindEntity(root.GetUUID()) == 0);
		CHECK(prototype->FindEntity(UUID(0x1234)) == EntityPrototype::c_NotFound);
		// The components each entity has, without the ID and relationship that every instance makes anew.
		std::vector<std::string> rootComponents;
		for (const ComponentInfo* info : prototype->GetComponents(0))
			rootComponents.push_back(info->Name);
		CHECK(rootComponents == std::vector<std::string> { "Name", "Transform", "Tag" });
		// The JSON is not kept: what the prefab writes comes from the prototype and equals what it was made from.
		CHECK(prefab->GetSnapshot() == snapshot);
		CHECK(prefab->Serialize()["Prefab"] == snapshot);

		// Invalid entity data fails at creation, with the reason.
		std::string error;
		const nlohmann::json duplicate = { { "Entities", nlohmann::json::array({ { { "ID", "0000000000000005" } }, { { "ID", "0000000000000005" } } }) } };
		CHECK_FALSE(Prefab::CreateFromSnapshot(duplicate, &error));
		CHECK(error.find("Duplicate entity ID") != std::string::npos);
	}

	TEST_CASE("Prefab instances copy the prototype: references, links, nested links and unknown components")
	{
		Ref<Scene> authoring = CreateRef<Scene>();
		Entity outside = authoring->CreateEntity("Outside");
		Entity ship = authoring->CreateEntity("Ship");
		Entity turret = authoring->CreateChildEntity(ship, "Turret");
		Entity engine = authoring->CreateChildEntity(ship, "Engine");
		// Script fields of entity type naming an entity of the prefab, one outside it, and none.
		ScriptEntry entry;
		entry.ClassName = "Ship";
		entry.Fields.push_back({ "Gun", PropertyType::Entity, PropertyValue(turret.GetUUID()) });
		entry.Fields.push_back({ "Home", PropertyType::Entity, PropertyValue(outside.GetUUID()) });
		entry.Fields.push_back({ "Nothing", PropertyType::Entity, PropertyValue(UUID::Null()) });
		ship.AddComponent<ScriptComponent>().Scripts.push_back(entry);
		// The engine came from another prefab, and has a component of a module this build lacks.
		engine.AddComponent<PrefabInstanceComponent>(PrefabInstanceComponent { UUID(0x777), UUID(0x888) });
		engine.AddComponent<UnknownComponentsComponent>(UnknownComponentsComponent { { { "Thruster", { { "Power", 12 } } } } });
		engine.AddComponent<InactiveComponent>();

		const Ref<Prefab> prefab = Prefab::CreateFromEntities(*authoring, { ship });
		REQUIRE(prefab);
		prefab->Handle = UUID(0x4243);

		Scene target("Target");
		Entity parent = target.CreateEntity("Parent");
		parent.GetComponent<TransformComponent>().Translation = glm::vec3(10.0f, 0.0f, 0.0f);
		parent.MarkModified<TransformComponent>();
		std::string error;
		const std::vector<Entity> roots = prefab->Instantiate(target, parent, &error);
		REQUIRE_MESSAGE(roots.size() == 1, error);
		Entity copy = roots[0];
		CHECK(copy.GetParent() == parent);
		CHECK(copy.GetUUID() != ship.GetUUID());
		const std::vector<Entity> children = copy.GetChildren();
		REQUIRE(children.size() == 2);
		CHECK(children[0].GetName() == "Turret");
		CHECK(children[1].GetName() == "Engine");

		const ScriptEntry& script = copy.GetComponent<ScriptComponent>().Scripts.at(0);
		CHECK(std::get<UUID>(script.Fields[0].Value) == children[0].GetUUID());
		CHECK(std::get<UUID>(script.Fields[1].Value) == outside.GetUUID());
		CHECK(std::get<UUID>(script.Fields[2].Value) == UUID::Null());

		// Every entity links to this prefab and to its prototype entity; the nested prefab's link is replaced, as before.
		CHECK(copy.GetComponent<PrefabInstanceComponent>().Prefab == prefab->Handle);
		CHECK(copy.GetComponent<PrefabInstanceComponent>().PrefabEntityID == ship.GetUUID());
		CHECK(children[1].GetComponent<PrefabInstanceComponent>().Prefab == prefab->Handle);
		CHECK(children[1].GetComponent<PrefabInstanceComponent>().PrefabEntityID == engine.GetUUID());
		REQUIRE(children[1].HasComponent<UnknownComponentsComponent>());
		CHECK(children[1].GetComponent<UnknownComponentsComponent>().Components["Thruster"]["Power"] == 12);
		CHECK(children[1].HasComponent<InactiveComponent>());
		CHECK_FALSE(target.IsActiveInHierarchy(children[1]));
		CHECK(target.IsActiveInHierarchy(children[0]));
		target.UpdateWorldTransforms();
		CHECK(target.GetWorldTransform(copy)[3].x == doctest::Approx(10.0f));
		Tests::CheckSceneCaches(target);

		// Removing the instance leaves the scene as it was.
		target.DestroyEntity(copy);
		CHECK(target.GetEntityCount() == 1);
		Tests::CheckSceneCaches(target);
	}

	TEST_CASE("Prefab instances emit the signals instantiating from JSON emitted")
	{
		Entity root;
		Ref<Scene> source = CreateTurretScene(root);
		const nlohmann::json snapshot = SceneSerializer::SerializeEntities(*source, { root });
		const Ref<Prefab> prefab = Prefab::CreateFromSnapshot(snapshot);
		REQUIRE(prefab);
		prefab->Handle = UUID(0x4244);

		// Per component type: how often it was constructed and updated in the scene.
		struct SignalCounts : ComponentObserver
		{
			uint32_t Calls = 0;
			void OnComponentChanged(entt::registry&, entt::entity) override { Calls++; }
		};
		const auto countSignals = [&](const std::function<void(Scene&)>& instantiate)
		{
			Scene scene;
			std::map<std::string, std::pair<uint32_t, uint32_t>> counts;
			std::vector<std::unique_ptr<SignalCounts>> constructed;
			std::vector<std::unique_ptr<SignalCounts>> updated;
			std::vector<entt::connection> connections;
			for (const ComponentInfo* info : ComponentRegistry::GetAll())
			{
				// Construction and destruction count together in Observe; updates are observed separately.
				SignalCounts& added = *constructed.emplace_back(std::make_unique<SignalCounts>());
				SignalCounts& changed = *updated.emplace_back(std::make_unique<SignalCounts>());
				for (entt::connection& connection : info->Observe(scene.GetRegistry(), added, false))
					connections.push_back(connection);
				std::vector<entt::connection> withUpdates = info->Observe(scene.GetRegistry(), changed, true);
				withUpdates[0].release();
				withUpdates[1].release();
				connections.push_back(withUpdates[2]);
			}
			instantiate(scene);
			for (size_t index = 0; index < ComponentRegistry::GetAll().size(); index++)
				counts[ComponentRegistry::GetAll()[index]->Name] = { constructed[index]->Calls, updated[index]->Calls };
			for (entt::connection& connection : connections)
				connection.release();
			return counts;
		};

		const auto fromJson = countSignals([&](Scene& scene)
		{
			EntityInstantiationOptions options;
			options.SourcePrefab = prefab->Handle;
			REQUIRE(SceneSerializer::DeserializeEntities(scene, snapshot, options).size() == 1);
		});
		const auto fromPrototype = countSignals([&](Scene& scene) { REQUIRE(prefab->Instantiate(scene).size() == 1); });
		CHECK(fromPrototype == fromJson);
		CHECK(fromPrototype.at("Transform") == std::pair<uint32_t, uint32_t>(2, 2));
		CHECK(fromPrototype.at("PointLight") == std::pair<uint32_t, uint32_t>(1, 1));
	}

	TEST_CASE("Many instances of a prefab are made in one call, each placed by its transform")
	{
		Entity root;
		Ref<Scene> source = CreateTurretScene(root);
		const Ref<Prefab> prefab = Prefab::CreateFromEntities(*source, { root });
		REQUIRE(prefab);
		prefab->Handle = UUID(0x4245);

		Scene target("Target");
		Entity parent = target.CreateEntity("Group");
		const std::vector<glm::mat4> transforms = {
			glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f)),
			glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 5.0f, 0.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(2.0f)),
			glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -3.0f)) };
		std::string error;
		const std::vector<Entity> instances = prefab->InstantiateMany(target, transforms, parent, &error);
		REQUIRE_MESSAGE(instances.size() == 3, error);
		CHECK(target.GetEntityCount() == 7);
		for (size_t index = 0; index < instances.size(); index++)
		{
			CAPTURE(index);
			const Entity instance = instances[index];
			CHECK(instance.GetName() == "Turret");
			CHECK(instance.GetParent() == parent);
			CHECK(instance.GetComponent<PrefabInstanceComponent>().Prefab == prefab->Handle);
			CHECK(instance.GetChildren().size() == 1);
			const glm::mat4 local = instance.GetComponent<TransformComponent>().GetTransform();
			for (int column = 0; column < 4; column++)
			{
				for (int row = 0; row < 4; row++)
					CHECK(local[column][row] == doctest::Approx(transforms[index][column][row]).epsilon(1e-5));
			}
		}
		CHECK(instances[1].GetComponent<TransformComponent>().Scale == glm::vec3(2.0f));
		// The instances are independent: distinct IDs, each child under its own root.
		CHECK(instances[0].GetChildren()[0] != instances[1].GetChildren()[0]);
		target.UpdateWorldTransforms();
		Tests::CheckSceneCaches(target);

		// A transform that is no translation, rotation and scale fails the call before anything is made.
		const std::vector<glm::mat4> broken = { glm::mat4(1.0f), glm::mat4(0.0f) };
		error.clear();
		CHECK(prefab->InstantiateMany(target, broken, parent, &error).empty());
		CHECK(error.find("Transform 1") != std::string::npos);
		CHECK(target.GetEntityCount() == 7);
		CHECK(prefab->InstantiateMany(target, {}, parent, &error).empty());
		CHECK(target.GetEntityCount() == 7);

		// The scene's capacity is checked for all instances at once.
		Scene full;
		std::vector<entt::entity> filler(Scene::c_MaxEntities - 5);
		full.GetRegistry().create(filler.begin(), filler.end());
		const std::vector<glm::mat4> three(3, glm::mat4(1.0f));
		error.clear();
		CHECK(prefab->InstantiateMany(full, three, {}, &error).empty());
		CHECK(error.find("at most 1048575 entities") != std::string::npos);
		CHECK(full.GetEntityCount() == 0);
		CHECK(prefab->InstantiateMany(full, std::span<const glm::mat4>(three.data(), 2), {}, &error).size() == 2);
		CHECK(full.GetEntityCount() == 4);
	}

	TEST_CASE("An instance is compared with its prefab entity by entity")
	{
		Entity root;
		Ref<Scene> source = CreateTurretScene(root);
		Entity barrel = root.GetChildren()[0];
		ScriptEntry entry;
		entry.ClassName = "Aim";
		entry.Fields.push_back({ "Target", PropertyType::Entity, PropertyValue(barrel.GetUUID()) });
		root.AddComponent<ScriptComponent>().Scripts.push_back(entry);
		const Ref<Prefab> prefab = Prefab::CreateFromEntities(*source, { root });
		REQUIRE(prefab);
		prefab->Handle = UUID(0x4246);

		Scene target("Target");
		const std::vector<Entity> roots = prefab->Instantiate(target);
		REQUIRE(roots.size() == 1);
		Entity instance = roots[0];
		Entity instanceBarrel = instance.GetChildren()[0];

		// Fresh from the prefab: every entity matched, nothing differs (its reference to the barrel counts as the prefab's).
		std::vector<PrefabInstanceEntity> entities = prefab->CompareInstance(target, instance);
		REQUIRE(entities.size() == 2);
		CHECK(entities[0].Instance == instance);
		CHECK(entities[0].PrototypeIndex == 0);
		CHECK(entities[1].Instance == instanceBarrel);
		CHECK(entities[1].PrototypeIndex == 1);
		CHECK(entities[0].Differences.empty());
		CHECK(entities[1].Differences.empty());

		// Changed values, an added and a removed component.
		instanceBarrel.GetComponent<PointLightComponent>().Intensity = 2.0f;
		instance.RemoveComponent<TagComponent>();
		instance.AddComponent<CameraComponent>();
		entities = prefab->CompareInstance(target, instance);
		REQUIRE(entities.size() == 2);
		std::vector<std::string> rootDifferences;
		for (const ComponentInfo* info : entities[0].Differences)
			rootDifferences.push_back(info->Name);
		CHECK(rootDifferences == std::vector<std::string> { "Tag", "Camera" });
		REQUIRE(entities[1].Differences.size() == 1);
		CHECK(entities[1].Differences[0]->Name == "PointLight");

		// A reference to an entity outside the instance differs.
		Entity other = target.CreateEntity("Other");
		instance.GetComponent<ScriptComponent>().Scripts[0].Fields[0].Value = other.GetUUID();
		entities = prefab->CompareInstance(target, instance);
		REQUIRE_FALSE(entities[0].Differences.empty());
		CHECK(entities[0].Differences.back()->Name == "Script");

		// Entities of other prefabs are not listed.
		CHECK(prefab->CompareInstance(target, other).empty());
	}
}
