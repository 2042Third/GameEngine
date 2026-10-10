#include <doctest/doctest.h>

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/ComponentAccess.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/Scene/UnknownComponents.h"
#include "TestHelpers.h"

#include <algorithm>

using namespace Strata;

namespace
{
	// Builds a scene that uses every serializable component with non-default values.
	Ref<Scene> CreateFullScene()
	{
		Ref<Scene> scene = CreateRef<Scene>("Full");
		scene->GetSettings().Gravity = { 0.0f, -20.0f, 1.0f };
		scene->GetSettings().FixedTimestep = 1.0f / 120.0f;

		Entity root = scene->CreateEntity("Root");
		root.GetTransform().Translation = { 1.5f, -2.0f, 3.25f };
		root.GetTransform().Rotation = Math::EulerDegreesToQuat({ 10.0f, 20.0f, 30.0f });
		root.GetTransform().Scale = { 2.0f, 2.0f, 2.0f };
		root.AddComponent<TagComponent>("Player");

		Entity camera = scene->CreateChildEntity(root, "Camera");
		CameraComponent& cameraComponent = camera.AddComponent<CameraComponent>();
		cameraComponent.Projection = ProjectionType::Orthographic;
		cameraComponent.OrthographicSize = 25.0f;
		cameraComponent.ClearColor = { 0.1f, 0.2f, 0.3f, 1.0f };

		Entity mesh = scene->CreateChildEntity(root, "Mesh");
		mesh.AddComponent<MeshRendererComponent>(UUID(11), UUID(12), false);
		mesh.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		mesh.AddComponent<BoxColliderComponent>().HalfExtents = { 1.0f, 2.0f, 3.0f };
		mesh.AddComponent<SphereColliderComponent>().Radius = 2.0f;
		mesh.AddComponent<CapsuleColliderComponent>().HalfHeight = 1.5f;
		mesh.AddComponent<MeshColliderComponent>().Convex = false;
		mesh.SetActive(false);

		Entity lights = scene->CreateEntity("Lights");
		lights.AddComponent<DirectionalLightComponent>().Intensity = 5.0f;
		lights.AddComponent<PointLightComponent>().Range = 42.0f;
		lights.AddComponent<SpotLightComponent>().OuterConeAngle = 45.0f;
		lights.AddComponent<SkyLightComponent>().EnvironmentMap = UUID(99);
		PostProcessComponent& postProcess = lights.AddComponent<PostProcessComponent>();
		postProcess.Tonemapper = TonemapOperator::AgX;
		postProcess.Exposure = 1.5f;

		Entity ui = scene->CreateEntity("UI");
		TextComponent& text = ui.AddComponent<TextComponent>();
		text.Text = "Score: 0\nLine two \xE2\x9C\x93";
		text.Alignment = TextAlignment::Right;
		ui.AddComponent<AudioSourceComponent>().Loop = true;
		ui.AddComponent<AudioListenerComponent>();
		ui.AddComponent<PrefabInstanceComponent>(UUID(7), UUID(8));

		ScriptComponent& scripts = ui.AddComponent<ScriptComponent>();
		ScriptEntry& script = scripts.Scripts.emplace_back();
		script.ClassName = "ScoreCounter";
		script.Fields.push_back({ "Points", PropertyType::Int, int32_t(42) });
		script.Fields.push_back({ "Speed", PropertyType::Float, 2.5f });
		script.Fields.push_back({ "Name", PropertyType::String, std::string("hello") });
		script.Fields.push_back({ "Target", PropertyType::Entity, root.GetUUID() });
		script.Fields.push_back({ "Tint", PropertyType::Color4, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f) });
		return scene;
	}

	// Components of a module this build lacks: nested objects, arrays, numbers, strings and null must survive verbatim.
	const nlohmann::json c_Vehicle = { { "Wheels", 4 }, { "Engine", { { "Power", 310.5 }, { "Kind", "V8" } } }, { "Gears", { 1, 2, 3 } }, { "Owner", nullptr } };
	const nlohmann::json c_Trailer = { { "Wheels", 2 }, { "Hitch", { { "Height", 0.45 } } } };
	const nlohmann::json c_Turret = { { "Yaw", -12.5 }, { "Ammo", { "Shell", "Flare" } } };
	const nlohmann::json c_Tire = { { "Pressure", 2.2 } };

	// Truck (Vehicle, Turret) with a child Wheel (Tire), Car (Vehicle) and Plain (only registered components).
	nlohmann::json CreateGarageDocument()
	{
		return {
			{ "Strata", { { "Format", "Scene" }, { "Version", 1 } } },
			{ "Scene", {
				{ "Name", "Garage" },
				{ "Entities", {
					{ { "ID", "00000000000000AB" }, { "Components", {
						{ "Name", { { "Name", "Truck" } } },
						{ "Transform", { { "Translation", { 1, 2, 3 } } } },
						{ "Vehicle", c_Vehicle },
						{ "Turret", c_Turret }
					} } },
					{ { "ID", "00000000000000AC" }, { "Parent", "00000000000000AB" }, { "Components", {
						{ "Name", { { "Name", "Wheel" } } },
						{ "Tire", c_Tire }
					} } },
					{ { "ID", "00000000000000AD" }, { "Components", { { "Name", { { "Name", "Car" } } }, { "Vehicle", c_Trailer } } } },
					{ { "ID", "00000000000000AE" }, { "Components", { { "Name", { { "Name", "Plain" } } } } } }
				} }
			} }
		};
	}

	// The serialized components of the entity named `name` in a scene or snapshot document's entity list.
	nlohmann::json FindComponents(const nlohmann::json& entities, std::string_view name)
	{
		for (const nlohmann::json& entity : entities)
		{
			if (entity["Components"].contains("Name") && entity["Components"]["Name"]["Name"] == name)
				return entity["Components"];
		}
		return nullptr;
	}

	size_t CountWarnings(const std::vector<std::string>& warnings, std::string_view text)
	{
		return static_cast<size_t>(std::count_if(warnings.begin(), warnings.end(), [&](const std::string& warning) { return warning.find(text) != std::string::npos; }));
	}
}

TEST_SUITE("Scene.Serialization")
{
	TEST_CASE("Scenes round trip through JSON without changes")
	{
		Ref<Scene> original = CreateFullScene();
		const nlohmann::json first = SceneSerializer::Serialize(*original);

		Scene loaded;
		std::string error;
		std::vector<std::string> warnings;
		REQUIRE_MESSAGE(SceneSerializer::Deserialize(loaded, first, &error, &warnings), error);
		CHECK(warnings.empty());

		const nlohmann::json second = SceneSerializer::Serialize(loaded);
		CHECK(first == second);

		CHECK(loaded.GetName() == "Full");
		CHECK(loaded.GetSettings().Gravity == glm::vec3(0.0f, -20.0f, 1.0f));
		CHECK(loaded.GetEntityCount() == original->GetEntityCount());

		Entity root = loaded.FindEntityByName("Root");
		REQUIRE(root.IsValid());
		CHECK(root.GetChildren().size() == 2);
		CHECK(root.GetChildren()[0].GetName() == "Camera");
		CHECK(Math::IsNearlyEqual(root.GetTransform().Rotation, Math::EulerDegreesToQuat({ 10.0f, 20.0f, 30.0f })));

		Entity mesh = loaded.FindEntityByName("Mesh");
		CHECK_FALSE(mesh.IsActive());
		CHECK(mesh.GetComponent<MeshRendererComponent>().Material == UUID(12));
		CHECK(mesh.GetComponent<RigidBodyComponent>().Type == RigidBodyType::Kinematic);

		Entity ui = loaded.FindEntityByName("UI");
		CHECK(ui.GetComponent<TextComponent>().Text == "Score: 0\nLine two \xE2\x9C\x93");
		const ScriptEntry* script = ui.GetComponent<ScriptComponent>().FindScript("ScoreCounter");
		REQUIRE(script);
		CHECK(std::get<int32_t>(script->FindField("Points")->Value) == 42);
		CHECK(std::get<UUID>(script->FindField("Target")->Value) == root.GetUUID());
		CHECK(ui.GetComponent<PrefabInstanceComponent>().PrefabEntityID == UUID(8));
	}

	TEST_CASE("Scene files round trip on disk")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("SceneFiles");
		Ref<Scene> original = CreateFullScene();
		std::string error;
		REQUIRE(SceneSerializer::SaveToFile(*original, directory / "Full.stscene", &error));

		Ref<Scene> loaded = SceneSerializer::LoadFromFile(directory / "Full.stscene", &error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(SceneSerializer::Serialize(*loaded) == SceneSerializer::Serialize(*original));

		// Floats are written in their shortest form, keeping files readable.
		const std::string text = FileSystem::ReadText(directory / "Full.stscene").value();
		CHECK(text.find("1.5") != std::string::npos);

		CHECK_FALSE(SceneSerializer::LoadFromFile(directory / "Missing.stscene", &error));
		REQUIRE(FileSystem::WriteText(directory / "Broken.stscene", "{ not json"));
		CHECK_FALSE(SceneSerializer::LoadFromFile(directory / "Broken.stscene", &error));
		CHECK(error.find("not valid JSON") != std::string::npos);
	}

	TEST_CASE("Invalid documents are rejected with errors")
	{
		std::string error;
		Scene scene;
		CHECK_FALSE(SceneSerializer::Deserialize(scene, nlohmann::json::object(), &error));
		CHECK_FALSE(error.empty());

		nlohmann::json wrongFormat = { { "Strata", { { "Format", "Prefab" }, { "Version", 1 } } }, { "Scene", nlohmann::json::object() } };
		CHECK_FALSE(SceneSerializer::Deserialize(scene, wrongFormat, &error));

		nlohmann::json futureVersion = { { "Strata", { { "Format", "Scene" }, { "Version", 999 } } }, { "Scene", nlohmann::json::object() } };
		CHECK_FALSE(SceneSerializer::Deserialize(scene, futureVersion, &error));
		CHECK(error.find("999") != std::string::npos);

		nlohmann::json duplicateIds = { { "Entities", { { { "ID", "0000000000000001" } }, { { "ID", "0000000000000001" } } } } };
		CHECK(SceneSerializer::DeserializeEntities(scene, duplicateIds, {}, &error).empty());
		CHECK(error.find("Duplicate") != std::string::npos);
		CHECK(scene.GetEntityCount() == 0);
	}

	TEST_CASE("Unknown components and properties produce warnings, not failures")
	{
		nlohmann::json document = {
			{ "Strata", { { "Format", "Scene" }, { "Version", 1 } } },
			{ "Scene", {
				{ "Name", "Forward" },
				{ "Entities", {
					{ { "ID", "00000000000000AA" }, { "Components", {
						{ "Name", { { "Name", "Thing" } } },
						{ "FutureComponent", { { "X", 1 } } },
						{ "Transform", { { "Translation", { 1, 2, 3 } }, { "Wobble", 4 }, { "Scale", "big" } } }
					} } }
				} }
			} }
		};

		Scene scene;
		std::string error;
		std::vector<std::string> warnings;
		REQUIRE(SceneSerializer::Deserialize(scene, document, &error, &warnings));
		CHECK(warnings.size() == 3);
		Entity thing = scene.GetEntityByUUID(UUID(0xAA));
		REQUIRE(thing.IsValid());
		CHECK(thing.GetName() == "Thing");
		CHECK(thing.GetTransform().Translation == glm::vec3(1.0f, 2.0f, 3.0f));
		CHECK(thing.GetTransform().Scale == glm::vec3(1.0f)); // Invalid value ignored
	}

	TEST_CASE("Hand-written entity data without ids is accepted")
	{
		nlohmann::json snapshot = { { "Entities", {
			{ { "Components", { { "Name", { { "Name", "Block" } } }, { "Transform", { { "Rotation", { 0, 45, 0 } } } } } } }
		} } };

		Scene scene;
		std::vector<Entity> created = SceneSerializer::DeserializeEntities(scene, snapshot, {});
		REQUIRE(created.size() == 1);
		CHECK(created[0].GetName() == "Block");
		CHECK(Math::IsNearlyEqual(created[0].GetTransform().Rotation, Math::EulerDegreesToQuat({ 0, 45, 0 })));
	}

	TEST_CASE("Entity snapshots instantiate under a parent with new ids and prefab links")
	{
		Ref<Scene> source = CreateFullScene();
		Entity root = source->FindEntityByName("Root");
		const nlohmann::json snapshot = SceneSerializer::SerializeEntities(*source, { root, root.GetChildren()[0] });
		CHECK(snapshot["Entities"].size() == 3); // The nested root is not duplicated

		Scene target;
		Entity anchor = target.CreateEntity("Anchor");
		EntityInstantiationOptions options;
		options.Parent = anchor;
		options.SourcePrefab = UUID(555);
		std::vector<Entity> created = SceneSerializer::DeserializeEntities(target, snapshot, options);
		REQUIRE(created.size() == 1);
		CHECK(created[0].GetParent() == anchor);
		CHECK(created[0].GetUUID() != root.GetUUID());
		CHECK(created[0].GetComponent<PrefabInstanceComponent>().Prefab == UUID(555));
		CHECK(created[0].GetComponent<PrefabInstanceComponent>().PrefabEntityID == root.GetUUID());
		CHECK(created[0].GetChildren().size() == 2);

		// Instantiating twice yields independent copies.
		std::vector<Entity> second = SceneSerializer::DeserializeEntities(target, snapshot, options);
		REQUIRE(second.size() == 1);
		CHECK(second[0].GetUUID() != created[0].GetUUID());
		CHECK(anchor.GetChildren().size() == 2);
	}

	TEST_CASE("ComponentAccess validates edits")
	{
		Scene scene;
		Entity entity = scene.CreateEntity("Edited");
		const ComponentInfo* transformInfo = ComponentRegistry::Find<TransformComponent>();
		const ComponentInfo* rigidBodyInfo = ComponentRegistry::Find<RigidBodyComponent>();

		std::string error;
		CHECK(ComponentAccess::SetProperty(entity, *transformInfo, *transformInfo->FindProperty("Translation"), glm::vec3(4.0f), &error));
		CHECK(entity.GetTransform().Translation == glm::vec3(4.0f));
		CHECK_FALSE(ComponentAccess::SetProperty(entity, *rigidBodyInfo, *rigidBodyInfo->FindProperty("Mass"), 2.0f, &error));
		CHECK(error.find("no RigidBody") != std::string::npos);

		CHECK(ComponentAccess::AddComponent(entity, *rigidBodyInfo, &error));
		CHECK(entity.HasComponent<RigidBodyComponent>());
		CHECK(ComponentAccess::GetProperty(entity, *rigidBodyInfo, *rigidBodyInfo->FindProperty("Mass")).has_value());
		CHECK(ComponentAccess::RemoveComponent(entity, *rigidBodyInfo, &error));
		CHECK_FALSE(ComponentAccess::RemoveComponent(entity, *transformInfo, &error));
		CHECK_FALSE(ComponentAccess::AddComponent(entity, *ComponentRegistry::Find<IDComponent>(), &error));

		const ComponentInfo* idInfo = ComponentRegistry::Find<IDComponent>();
		CHECK_FALSE(ComponentAccess::SetProperty(entity, *idInfo, *idInfo->FindProperty("ID"), UUID(5), &error));

		// Strict deserialization reports the first problem and stops.
		TransformComponent transform;
		CHECK_FALSE(ComponentAccess::Deserialize(*transformInfo, &transform, { { "Translation", "bad" } }, true, &error));
		CHECK_FALSE(ComponentAccess::Deserialize(*transformInfo, &transform, { { "Unknown", 1 } }, true, &error));
		CHECK(ComponentAccess::Deserialize(*transformInfo, &transform, { { "Translation", { 1, 1, 1 } } }, true, &error));

		const nlohmann::json components = ComponentAccess::SerializeEntityComponents(entity);
		CHECK(components.contains("Transform"));
		CHECK(components.contains("Name"));
		CHECK_FALSE(components.contains("ID"));
		CHECK_FALSE(components.contains("Relationship"));
	}

	TEST_CASE("Components no module of this build registers load with one warning each and save unchanged")
	{
		REQUIRE(ComponentRegistry::Find("Vehicle") == nullptr);
		const nlohmann::json document = CreateGarageDocument();

		Scene scene;
		std::string error;
		std::vector<std::string> warnings;
		REQUIRE(SceneSerializer::Deserialize(scene, document, &error, &warnings));
		// Once per name and load, however many entities have it.
		CHECK(warnings.size() == 3);
		CHECK(CountWarnings(warnings, "Unknown component 'Vehicle' (2 entities) kept unchanged") == 1);
		CHECK(CountWarnings(warnings, "Unknown component 'Turret' (1 entity) kept unchanged") == 1);
		CHECK(CountWarnings(warnings, "Unknown component 'Tire' (1 entity) kept unchanged") == 1);

		// Registered components load as usual; the others are kept in the runtime-only blob.
		Entity truck = scene.GetEntityByUUID(UUID(0xAB));
		REQUIRE(truck);
		CHECK(truck.GetTransform().Translation == glm::vec3(1.0f, 2.0f, 3.0f));
		REQUIRE(truck.HasComponent<UnknownComponentsComponent>());
		CHECK(truck.GetComponent<UnknownComponentsComponent>().Components == nlohmann::json { { "Vehicle", c_Vehicle }, { "Turret", c_Turret } });
		CHECK_FALSE(scene.GetEntityByUUID(UUID(0xAE)).HasComponent<UnknownComponentsComponent>());
		CHECK(ComponentRegistry::Find<UnknownComponentsComponent>() == nullptr);

		// Saving writes them back unchanged, next to the registered components.
		const nlohmann::json entities = SceneSerializer::Serialize(scene)["Scene"]["Entities"];
		const nlohmann::json truckComponents = FindComponents(entities, "Truck");
		CHECK(truckComponents["Vehicle"] == c_Vehicle);
		CHECK(truckComponents["Turret"] == c_Turret);
		CHECK(truckComponents.contains("Transform"));
		CHECK(FindComponents(entities, "Wheel")["Tire"] == c_Tire);
		CHECK(FindComponents(entities, "Car")["Vehicle"] == c_Trailer);
		CHECK_FALSE(FindComponents(entities, "Plain").contains("Vehicle"));

		// Through a file, and loaded again: the same document, and the same warnings (once per load).
		const std::filesystem::path path = Tests::CreateTemporaryDirectory("UnknownComponents") / "Garage.stscene";
		REQUIRE(SceneSerializer::SaveToFile(scene, path, &error));
		const std::optional<std::string> text = FileSystem::ReadText(path);
		REQUIRE(text);
		const std::optional<nlohmann::json> saved = JsonUtils::Parse(*text);
		REQUIRE(saved);
		CHECK((*saved)["Scene"]["Entities"] == entities);
		Scene reloaded;
		warnings.clear();
		REQUIRE(SceneSerializer::Deserialize(reloaded, *saved, &error, &warnings));
		CHECK(warnings.size() == 3);
		CHECK(SceneSerializer::Serialize(reloaded)["Scene"]["Entities"] == entities);
	}

	TEST_CASE("Kept components travel with scene copies, prefab snapshots and duplicates")
	{
		Scene scene;
		REQUIRE(SceneSerializer::Deserialize(scene, CreateGarageDocument()));
		Entity truck = scene.GetEntityByUUID(UUID(0xAB));
		REQUIRE(truck);

		// Scene::Copy (play mode).
		Ref<Scene> source = CreateRef<Scene>();
		REQUIRE(SceneSerializer::Deserialize(*source, CreateGarageDocument()));
		const Ref<Scene> copy = Scene::Copy(source);
		const nlohmann::json copied = SceneSerializer::Serialize(*copy)["Scene"]["Entities"];
		CHECK(FindComponents(copied, "Truck")["Vehicle"] == c_Vehicle);
		CHECK(FindComponents(copied, "Wheel")["Tire"] == c_Tire);
		CHECK(copied == SceneSerializer::Serialize(*source)["Scene"]["Entities"]);

		// A prefab of the truck, through its document, instantiated into another scene.
		const Ref<Prefab> prefab = Prefab::CreateFromEntities(scene, { truck });
		const nlohmann::json prefabDocument = prefab->Serialize();
		CHECK(FindComponents(prefabDocument["Prefab"]["Entities"], "Truck")["Vehicle"] == c_Vehicle);
		std::string error;
		const Ref<Prefab> reloadedPrefab = Prefab::FromJson(prefabDocument, &error);
		REQUIRE(reloadedPrefab);
		Scene target;
		const std::vector<Entity> instances = reloadedPrefab->Instantiate(target);
		REQUIRE(instances.size() == 1);
		REQUIRE(instances[0].HasComponent<UnknownComponentsComponent>());
		CHECK(instances[0].GetComponent<UnknownComponentsComponent>().Components["Vehicle"] == c_Vehicle);
		REQUIRE(instances[0].GetChildren().size() == 1);
		CHECK(instances[0].GetChildren()[0].GetComponent<UnknownComponentsComponent>().Components["Tire"] == c_Tire);

		// DuplicateEntity.
		const Entity duplicate = scene.DuplicateEntity(truck);
		REQUIRE(duplicate);
		CHECK(duplicate.GetUUID() != truck.GetUUID());
		REQUIRE(duplicate.HasComponent<UnknownComponentsComponent>());
		CHECK(duplicate.GetComponent<UnknownComponentsComponent>().Components == truck.GetComponent<UnknownComponentsComponent>().Components);
		REQUIRE(duplicate.GetChildren().size() == 1);
		CHECK(duplicate.GetChildren()[0].GetComponent<UnknownComponentsComponent>().Components["Tire"] == c_Tire);

		// Data the scene wrote itself can be restored without reporting what it keeps.
		Scene restored;
		EntityInstantiationOptions options;
		options.ReportUnknownComponents = false;
		std::vector<std::string> warnings;
		SceneSerializer::DeserializeEntities(restored, SceneSerializer::SerializeEntities(scene, { truck }), options, &error, &warnings);
		CHECK(warnings.empty());
		CHECK(restored.GetEntityCount() == 2);
	}
}
