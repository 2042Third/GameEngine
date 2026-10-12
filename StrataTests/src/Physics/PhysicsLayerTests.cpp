#include <doctest/doctest.h>

#include "Physics/PhysicsTestUtils.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/PhysicsLayerSettings.h"
#include "Strata/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A scene document with the given settings, as files store it.
	nlohmann::json MakeSceneDocument(const nlohmann::json& settings)
	{
		return {
			{ "Strata", { { "Format", "Scene" }, { "Version", 1 } } },
			{ "Scene", { { "Name", "Layers" }, { "Settings", settings } } } };
	}

	// A box on `layer` dropped onto ground on layer 0.
	Entity CreateBoxAbove(Scene& scene, const std::string& name, float x, uint32_t layer)
	{
		Entity box = CreateDynamicBox(scene, name, glm::vec3(x, 1.0f, 0.0f));
		box.GetComponent<RigidBodyComponent>().Layer = layer;
		return box;
	}

}

TEST_SUITE("Physics.Layers")
{
	TEST_CASE("Layer settings keep the matrix symmetric and find layers by name")
	{
		PhysicsLayerSettings layers;
		CHECK(layers.IsDefaultMatrix());
		CHECK(layers.DoCollide(0, 31));
		CHECK_FALSE(layers.DoCollide(0, 32));

		layers.SetCollide(3, 5, false);
		CHECK_FALSE(layers.DoCollide(3, 5));
		CHECK_FALSE(layers.DoCollide(5, 3));
		CHECK(layers.DoCollide(3, 3));
		CHECK_FALSE(layers.IsDefaultMatrix());
		CHECK_FALSE(layers.CollidesWithAll(3));
		CHECK_FALSE(layers.CollidesWithAll(5));
		CHECK(layers.CollidesWithAll(4));
		CHECK_FALSE(layers.CollidesWithAll(32));
		layers.SetCollide(4, 4, false);
		CHECK_FALSE(layers.DoCollide(4, 4));
		layers.SetCollide(40, 1, false); // Ignored
		CHECK(layers.DoCollide(1, 1));
		layers.SetCollide(3, 5, true);
		layers.SetCollide(4, 4, true);
		CHECK(layers.IsDefaultMatrix());

		layers.Names[1] = "Player";
		CHECK(layers.FindLayer("Player") == 1u);
		CHECK(layers.FindLayer("PLAYER") == 1u);
		CHECK(layers.FindLayer("Layer 7") == 7u);
		CHECK(layers.FindLayer("layer 7") == 7u);
		CHECK_FALSE(layers.FindLayer("Layer 1")); // Layer 1 has a name of its own
		CHECK_FALSE(layers.FindLayer("Layer 32"));
		CHECK_FALSE(layers.FindLayer("Layer 07x"));
		CHECK_FALSE(layers.FindLayer(""));
		CHECK_FALSE(layers.FindLayer("Enemy"));
		CHECK(layers.GetDisplayName(1) == "Player");
		CHECK(layers.GetDisplayName(2) == "Layer 2");

		CHECK_FALSE(layers.ValidateName(2, "Enemy"));
		CHECK_FALSE(layers.ValidateName(2, "")); // Clears the name
		CHECK_FALSE(layers.ValidateName(1, "Player"));
		CHECK(layers.ValidateName(2, "player")); // Another layer's name, ignoring case
		CHECK(layers.ValidateName(2, "Layer 4")); // Another layer's default name
		CHECK_FALSE(layers.ValidateName(4, "Layer 4"));
		CHECK(layers.ValidateName(2, " Enemy"));
		CHECK(layers.ValidateName(2, "Enemy\n"));
		CHECK(layers.ValidateName(2, std::string(PhysicsLayerSettings::c_MaxNameLength + 1, 'x')));
		CHECK_FALSE(layers.ValidateName(2, std::string(PhysicsLayerSettings::c_MaxNameLength, 'x')));
		CHECK(layers.ValidateName(32, "Enemy"));

		// A reset lets every pair collide and keeps the names.
		layers.SetCollide(6, 9, false);
		layers.ResetMatrix();
		CHECK(layers.IsDefaultMatrix());
		CHECK(layers.Names[1] == "Player");
	}

	TEST_CASE("Layer names and the matrix round trip through scene files")
	{
		Scene scene("Layers");
		PhysicsLayerSettings& layers = scene.GetSettings().PhysicsLayers;
		layers.Names[1] = "Player";
		layers.Names[30] = "Debris \xE2\x9C\x93";
		layers.SetCollide(1, 30, false);
		layers.SetCollide(30, 30, false);
		layers.SetCollide(0, 5, false);

		const nlohmann::json json = SceneSerializer::Serialize(scene);
		const nlohmann::json& stored = json["Scene"]["Settings"]["PhysicsLayers"];
		CHECK(stored["Names"] == nlohmann::json { { "1", "Player" }, { "30", "Debris \xE2\x9C\x93" } });
		CHECK(stored["NoCollisions"] == nlohmann::json::array({ { 0, 5 }, { 1, 30 }, { 30, 30 } }));

		Scene loaded;
		std::string error;
		std::vector<std::string> warnings;
		REQUIRE_MESSAGE(SceneSerializer::Deserialize(loaded, json, &error, &warnings), error);
		CHECK(warnings.empty());
		CHECK(loaded.GetSettings().PhysicsLayers == layers);
		CHECK(SceneSerializer::Serialize(loaded) == json);

		// A copy (what play mode runs) keeps them.
		Ref<Scene> source = CreateRef<Scene>();
		source->GetSettings().PhysicsLayers = layers;
		CHECK(Scene::Copy(source)->GetSettings().PhysicsLayers == layers);
	}

	TEST_CASE("Scenes saved before layer settings load with every layer colliding, and invalid entries are reported")
	{
		std::vector<std::string> warnings;
		Scene old;
		std::string error;
		REQUIRE_MESSAGE(SceneSerializer::Deserialize(old, MakeSceneDocument({ { "FixedTimestep", 0.02 } }), &error, &warnings), error);
		CHECK(warnings.empty());
		CHECK(old.GetSettings().PhysicsLayers == PhysicsLayerSettings());

		// Each invalid entry is skipped with a warning; the valid ones load.
		const nlohmann::json invalid = { { "PhysicsLayers", {
			{ "Names", { { "1", "Player" }, { "2", "player" }, { "x", "Bad" }, { "40", "Far" }, { "3", 7 }, { "4", "Layer 5" }, { "6", "Tab\tName" } } },
			{ "NoCollisions", nlohmann::json::array({ { 1, 2 }, { 1 }, { 1, 32 }, { -1, 2 }, "pair", { 2, 2 } }) } } } };
		Scene scene;
		REQUIRE_MESSAGE(SceneSerializer::Deserialize(scene, MakeSceneDocument(invalid), &error, &warnings), error);
		std::string reported;
		for (const std::string& warning : warnings)
			reported += warning + '\n';
		INFO(invalid.dump(), '\n', reported);
		const PhysicsLayerSettings& layers = scene.GetSettings().PhysicsLayers;
		CHECK(layers.Names[1] == "Player");
		CHECK(layers.Names[2].empty());
		CHECK(layers.Names[3].empty());
		CHECK(layers.Names[4].empty());
		CHECK(layers.Names[6].empty());
		CHECK_FALSE(layers.DoCollide(1, 2));
		CHECK_FALSE(layers.DoCollide(2, 2));
		CHECK(layers.DoCollide(1, 1));
		CHECK(warnings.size() == 10);

		// Values of the wrong type are ignored as a whole.
		for (const nlohmann::json& value : { nlohmann::json(5), nlohmann::json { { "Names", 3 } }, nlohmann::json { { "NoCollisions", "none" } } })
		{
			warnings.clear();
			Scene other;
			REQUIRE_MESSAGE(SceneSerializer::Deserialize(other, MakeSceneDocument({ { "PhysicsLayers", value } }), &error, &warnings), error);
			CHECK(other.GetSettings().PhysicsLayers == PhysicsLayerSettings());
			CHECK(warnings.size() == 1);
		}
	}

	TEST_CASE("Bodies on layers the scene's matrix keeps apart pass through each other")
	{
		Scene scene;
		CreateGround(scene); // Layer 0
		Entity ghost = CreateBoxAbove(scene, "Ghost", 0.0f, 4);
		Entity solid = CreateBoxAbove(scene, "Solid", 3.0f, 5);
		scene.GetSettings().PhysicsLayers.SetCollide(0, 4, false);

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK_FALSE(physics.DoLayersCollide(0, 4));
		CHECK(physics.DoLayersCollide(0, 5));
		REQUIRE(physics.GetWorld() != nullptr);
		CHECK_FALSE(physics.GetWorld()->DoLayersCollide(4, 0));
		StepScene(scene, 90);
		CHECK(GetWorldPosition(scene, ghost).y < -2.0f);
		CHECK(GetWorldPosition(scene, solid).y == doctest::Approx(0.5f).epsilon(0.03));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Changes of the scene's matrix while it plays apply at the next step, also back")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		Entity box = CreateBoxAbove(scene, "Box", 0.0f, 2);
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		StepScene(scene, 60);
		REQUIRE(GetWorldPosition(scene, box).y == doctest::Approx(0.5f).epsilon(0.03));

		// As scene.setSettings, undo or the inspector change it: the settings only.
		scene.GetSettings().PhysicsLayers.SetCollide(2, 0, false);
		CHECK_FALSE(physics.DoLayersCollide(0, 2));
		StepScene(scene, 20);
		const float sinking = GetWorldPosition(scene, box).y;
		CHECK(sinking < 0.0f);

		// Colliding again: the box, now inside the ground, is pushed out or keeps falling, but the world uses the matrix.
		scene.GetSettings().PhysicsLayers.SetCollide(2, 0, true);
		StepScene(scene, 1);
		REQUIRE(physics.GetWorld() != nullptr);
		CHECK(physics.GetWorld()->DoLayersCollide(0, 2));

		// PhysicsSystem::SetLayersCollide changes the scene's matrix, and the world at once.
		physics.SetLayersCollide(1, 2, false);
		CHECK_FALSE(scene.GetSettings().PhysicsLayers.DoCollide(2, 1));
		CHECK_FALSE(physics.GetWorld()->DoLayersCollide(1, 2));
		physics.SetLayersCollide(1, 2, true);
		CHECK(physics.GetWorld()->DoLayersCollide(2, 1));
		physics.SetLayersCollide(1, 40, false); // Refused with a warning
		CHECK(scene.GetSettings().PhysicsLayers.DoCollide(1, 31));
		CHECK(ground.IsValid());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Letting two layers collide while the scene plays makes their bodies collide")
	{
		Scene scene;
		CreateGround(scene); // Layer 0
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 6.0f, 0.0f));
		box.GetComponent<RigidBodyComponent>().Layer = 4;
		scene.GetSettings().PhysicsLayers.SetCollide(0, 4, false);
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);

		// Falling, still well above the ground, the box would pass through it; the change lets it land instead.
		StepScene(scene, 10);
		REQUIRE(GetWorldPosition(scene, box).y > 3.0f);
		physics.SetLayersCollide(0, 4, true);
		CHECK(scene.GetSettings().PhysicsLayers.DoCollide(4, 0));
		StepScene(scene, 120);
		CHECK(GetWorldPosition(scene, box).y == doctest::Approx(0.5f).epsilon(0.03));
		scene.OnRuntimeStop();
	}

	TEST_CASE("The default settings' matrix still restricts every scene's")
	{
		PhysicsSettings settings;
		settings.SetLayersCollide(6, 7, false);
		ScopedPhysicsSettings scopedSettings(settings);
		Scene scene;
		CreateGround(scene);
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK_FALSE(physics.DoLayersCollide(6, 7));
		// The scene's matrix allows the pair, but the default settings do not.
		CHECK(scene.GetSettings().PhysicsLayers.DoCollide(6, 7));
		REQUIRE(physics.GetWorld() != nullptr);
		CHECK_FALSE(physics.GetWorld()->DoLayersCollide(6, 7));
		scene.OnRuntimeStop();
	}
}
