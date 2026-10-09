#include <doctest/doctest.h>

#include "Strata/Core/JsonUtils.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"

#include <string>
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
}
