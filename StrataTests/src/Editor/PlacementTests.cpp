#include <doctest/doctest.h>

#include "Animation/SkinnedGltf.h"
#include "Editor/AssetBounds.h"
#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCamera.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorPlacement.h"
#include "Editor/HarnessEditor.h"
#include "Editor/SceneRaycast.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/ViewportPanel.h"
#include "TestHelpers.h"
#include "UI/ItemProbe.h"

#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Math/Math.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/Material.h>
#include <Strata/Renderer/SceneRenderer.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Prefab.h>
#include <Strata/Scene/PrefabInstances.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SubScene.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	struct PlacementHarness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		std::filesystem::path Directory;

		// A basic3d project: its Ground's top is at y = 0.
		explicit PlacementHarness(const std::string& name)
			: Directory(CreateTemporaryDirectory(name) / "Game")
		{
			Run("project.create", { { "directory", FileSystem::ToUTF8(Directory) }, { "name", "Game" }, { "template", "basic3d" } });
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			REQUIRE_FALSE(result.IsPending());
			return result.Value;
		}

		// A command that may wait (asset.place while its asset loads), run to its end.
		nlohmann::json Complete(std::string_view name, const nlohmann::json& parameters)
		{
			return RunCommandToCompletion(Context, Commands, name, parameters);
		}

		Scene& GetScene() { return *Context.GetActiveScene(); }

		// Editor frames until nothing loads or imports (by the clock: CI's machines are slow).
		void Settle()
		{
			EditorAssetManager& assets = *Context.GetAssetManager();
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
			do
			{
				Context.Update(Timestep(1.0f / 60.0f));
				REQUIRE(std::chrono::steady_clock::now() < deadline);
			}
			while (assets.HasPendingLoads() || assets.HasPendingImports());
		}

		std::string CreateCube(const glm::vec3& position, const nlohmann::json& parent = nullptr)
		{
			nlohmann::json parameters = { { "name", "Cube" }, { "components", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } },
				{ "Transform", { { "Translation", { position.x, position.y, position.z } } } } } } };
			if (!parent.is_null())
				parameters["parent"] = parent;
			return Run("entity.create", parameters)["id"].get<std::string>();
		}

		glm::vec3 WorldPosition(const std::string& id)
		{
			return glm::vec3(GetScene().GetWorldTransform(GetScene().GetEntityByUUID(*UUIDFromJson(id)))[3]);
		}
	};

}

TEST_SUITE("Editor.Placement")
{
	TEST_CASE("Rays hit the triangles of what the scene draws, nearest first, and leave out what is being placed")
	{
		PlacementHarness harness("PlacementRaycast");
		Scene& scene = harness.GetScene();
		const std::string cube = harness.CreateCube(glm::vec3(3.0f, 0.5f, 0.0f));

		// Straight down onto the ground.
		const Ray down(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
		std::optional<SceneRayHit> hit = SceneRaycast::Cast(scene, down);
		REQUIRE(hit);
		CHECK(hit->HitEntity.GetName() == "Ground");
		CHECK(hit->OnTriangle);
		CHECK(hit->Point.y == doctest::Approx(0.0f).epsilon(1e-4));
		CHECK(hit->Normal.y == doctest::Approx(1.0f));
		CHECK(hit->Distance == doctest::Approx(10.0f));

		// Onto the cube's top, then through it when the cube is left out.
		const Ray onCube(glm::vec3(3.0f, 10.0f, 0.2f), glm::vec3(0.0f, -1.0f, 0.0f));
		hit = SceneRaycast::Cast(scene, onCube);
		REQUIRE(hit);
		CHECK(hit->HitEntity.GetUUID() == *UUIDFromJson(cube));
		CHECK(hit->Point.y == doctest::Approx(1.0f));
		const Entity ignore[] = { hit->HitEntity };
		hit = SceneRaycast::Cast(scene, onCube, std::numeric_limits<float>::max(), ignore);
		REQUIRE(hit);
		CHECK(hit->HitEntity.GetName() == "Ground");

		// From the side: the cube's face, its normal against the ray.
		hit = SceneRaycast::Cast(scene, Ray(glm::vec3(-5.0f, 0.5f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
		REQUIRE(hit);
		CHECK(hit->Point.x == doctest::Approx(2.5f));
		CHECK(hit->Normal.x == doctest::Approx(-1.0f));
		// Not that far, or pointing away: nothing.
		CHECK_FALSE(SceneRaycast::Cast(scene, Ray(glm::vec3(-5.0f, 0.5f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f)), 5.0f));
		CHECK_FALSE(SceneRaycast::Cast(scene, Ray(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f))));
	}

	TEST_CASE("A placement rests on the surface the ray hits, else on the grid within 500 m, else in front of the camera")
	{
		PlacementHarness harness("PlacementPlace");
		Scene& scene = harness.GetScene();
		const AABB cube(glm::vec3(-0.5f), glm::vec3(0.5f));
		const float unlimited = std::numeric_limits<float>::max();

		// Straight down onto the ground: the cube's bottom on it.
		std::optional<EditorPlacement::Placement> placed = EditorPlacement::Place(scene,
			EditorPlacement::MakeRayRequest(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), unlimited, 10.0f, std::nullopt), cube);
		REQUIRE(placed);
		CHECK(placed->Source == EditorPlacement::PlacementSource::Surface);
		CHECK(placed->HitEntity.GetName() == "Ground");
		CHECK(placed->Translation.y == doctest::Approx(0.5f).epsilon(1e-4));
		CHECK_FALSE(placed->Snapped);

		// Past the 20 m ground: where the ray meets y = 0, 50 m away.
		placed = EditorPlacement::Place(scene, EditorPlacement::MakeRayRequest(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(50.0f, -10.0f, 0.0f), unlimited, 10.0f,
			std::nullopt), cube);
		REQUIRE(placed);
		CHECK(placed->Source == EditorPlacement::PlacementSource::Grid);
		CHECK_FALSE(placed->HitEntity);
		CHECK(placed->RestingOn.Point.x == doctest::Approx(50.0f).epsilon(1e-4));
		CHECK(placed->RestingOn.Point.y == doctest::Approx(0.0f));
		CHECK(placed->Translation.y == doctest::Approx(0.5f).epsilon(1e-4));

		// The grid 1,000 m away: in front of the camera at the fallback distance, the cube's center on the ray.
		const glm::vec3 distant(1000.0f, -10.0f, 0.0f);
		placed = EditorPlacement::Place(scene, EditorPlacement::MakeRayRequest(glm::vec3(0.0f, 10.0f, 0.0f), distant, unlimited, 10.0f, std::nullopt), cube);
		REQUIRE(placed);
		CHECK(placed->Source == EditorPlacement::PlacementSource::InFront);
		const glm::vec3 expected = glm::vec3(0.0f, 10.0f, 0.0f) + glm::normalize(distant) * 10.0f;
		CHECK(glm::length(placed->Translation - expected) < 1e-4f);
		// Upward: in front too.
		placed = EditorPlacement::Place(scene, EditorPlacement::MakeRayRequest(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), unlimited, 3.0f,
			std::nullopt), cube);
		REQUIRE(placed);
		CHECK(placed->Source == EditorPlacement::PlacementSource::InFront);
		CHECK(placed->Translation.y == doctest::Approx(13.0f));
		// A ray limited under the minimum in-front distance stops at its limit.
		placed = EditorPlacement::Place(scene, EditorPlacement::MakeRayRequest(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), 0.25f, 3.0f,
			std::nullopt), cube);
		REQUIRE(placed);
		CHECK(placed->Translation.y == doctest::Approx(10.25f));

		// Requiring a surface, a miss places nothing.
		EditorPlacement::PlacementRequest required = EditorPlacement::MakeRayRequest(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), unlimited,
			10.0f, std::nullopt);
		required.RequireSurface = true;
		CHECK_FALSE(EditorPlacement::Place(scene, required, cube));

		// A 2 x 1 x 1 box turned 90 degrees about Z stands 2 high, its bottom on the ground.
		EditorPlacement::PlacementRequest turned = EditorPlacement::MakeRayRequest(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), unlimited,
			10.0f, std::nullopt);
		turned.Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		placed = EditorPlacement::Place(scene, turned, AABB(glm::vec3(-1.0f, -0.5f, -0.5f), glm::vec3(1.0f, 0.5f, 0.5f)));
		REQUIRE(placed);
		const glm::vec3 size = placed->PlacedBounds.Max - placed->PlacedBounds.Min;
		CHECK(size.x == doctest::Approx(1.0f).epsilon(1e-4));
		CHECK(size.y == doctest::Approx(2.0f).epsilon(1e-4));
		CHECK(size.z == doctest::Approx(1.0f).epsilon(1e-4));
		CHECK(placed->Translation.y + placed->PlacedBounds.Min.y == doctest::Approx(0.0f).epsilon(1e-4));

		// The given point: resting against it along its normal.
		EditorPlacement::PlacementRequest point;
		point.At = EditorPlacement::Surface { glm::vec3(3.0f, 1.0f, 0.0f), glm::vec3(-2.0f, 0.0f, 0.0f), true };
		placed = EditorPlacement::Place(scene, point, cube);
		REQUIRE(placed);
		CHECK(placed->Source == EditorPlacement::PlacementSource::Point);
		CHECK(placed->RestingOn.Normal.x == doctest::Approx(-1.0f));
		CHECK(placed->Translation.x == doctest::Approx(2.5f));
		CHECK(std::string(EditorPlacement::PlacementSourceToString(EditorPlacement::PlacementSource::InFront)) == "inFront");
	}

	TEST_CASE("Snapping puts the placement on the grid step and finds the height there")
	{
		PlacementHarness harness("PlacementSnap");
		Scene& scene = harness.GetScene();
		const AABB cube(glm::vec3(-0.5f), glm::vec3(0.5f));
		const float unlimited = std::numeric_limits<float>::max();
		const auto down = [&](const glm::vec3& at, float step)
		{
			return EditorPlacement::Place(scene, EditorPlacement::MakeRayRequest(at + glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), unlimited, 10.0f,
				step), cube);
		};

		// The empty ground: x and z on the step, y on the ground.
		std::optional<EditorPlacement::Placement> placed = down(glm::vec3(0.8f, 0.0f, 0.3f), 1.0f);
		REQUIRE(placed);
		CHECK(placed->Snapped);
		CHECK(placed->RestingOn.Point.x == doctest::Approx(1.0f));
		CHECK(placed->RestingOn.Point.y == doctest::Approx(0.0f).epsilon(1e-4));
		CHECK(placed->RestingOn.Point.z == doctest::Approx(0.0f));

		// A small cube standing where the snapped point lands, beside the hit: the snapped point finds its top.
		const std::string stool = harness.Run("entity.create", { { "name", "Stool" }, { "components", {
			{ "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } },
			{ "Transform", { { "Translation", { 1.0, 0.25, 0.0 } }, { "Scale", { 0.5, 0.5, 0.5 } } } } } } })["id"].get<std::string>();
		placed = down(glm::vec3(0.8f, 0.0f, 0.3f), 1.0f);
		REQUIRE(placed);
		CHECK(placed->Snapped);
		CHECK(placed->HitEntity.GetUUID() == *UUIDFromJson(stool));
		CHECK(placed->RestingOn.Point.y == doctest::Approx(0.5f).epsilon(1e-4));
		CHECK(placed->Translation.y == doctest::Approx(1.0f).epsilon(1e-4));

		// A wall facing -X: x stays on its plane, y and z snap.
		harness.CreateCube(glm::vec3(5.0f, 0.5f, 5.0f));
		placed = EditorPlacement::Place(scene, EditorPlacement::MakeRayRequest(glm::vec3(0.0f, 0.7f, 5.4f), glm::vec3(1.0f, 0.0f, 0.0f), unlimited, 10.0f, 1.0f),
			cube);
		REQUIRE(placed);
		CHECK(placed->Source == EditorPlacement::PlacementSource::Surface);
		CHECK(placed->Snapped);
		CHECK(placed->RestingOn.Point.x == doctest::Approx(4.5f));
		CHECK(placed->RestingOn.Point.y == doctest::Approx(1.0f));
		CHECK(placed->RestingOn.Point.z == doctest::Approx(5.0f));

		// Snapped beyond the ground's edge (x = 10), over nothing: the hit's height stays.
		placed = down(glm::vec3(9.5f, 0.0f, 0.0f), 13.0f);
		REQUIRE(placed);
		CHECK(placed->Snapped);
		CHECK(placed->RestingOn.Point.x == doctest::Approx(13.0f));
		CHECK(placed->RestingOn.Point.y == doctest::Approx(0.0f).epsilon(1e-4));
	}

	TEST_CASE("The view's ray goes through the pointer from the camera, also far from the origin")
	{
		const glm::vec3 position(10000.0f, 50.0f, -10000.0f);
		const glm::vec3 target = position + glm::normalize(glm::vec3(1.0f, -0.4f, 0.7f)) * 30.0f;
		const glm::vec3 pointed = target + glm::vec3(2.0f, 1.5f, -1.0f); // Off the view's center
		for (const bool orthographic : { false, true })
		{
			CAPTURE(orthographic);
			SceneCamera camera;
			camera.Position = position;
			camera.View = glm::lookAt(position, target, glm::vec3(0.0f, 1.0f, 0.0f));
			camera.Projection = orthographic ? Math::OrthographicReverseZ(-20.0f, 20.0f, -12.0f, 12.0f, 0.1f, 1000.0f)
				: Math::PerspectiveReverseZ(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
			camera.Orthographic = orthographic;
			// The point projected in double, camera-relative.
			const glm::dvec3 relative = glm::dvec3(pointed) - glm::dvec3(position);
			const glm::dvec4 clip = glm::dmat4(camera.Projection) * glm::dmat4(glm::dmat3(glm::dmat4(camera.View))) * glm::dvec4(relative, 1.0);
			const glm::vec2 ndc(static_cast<float>(clip.x / clip.w), static_cast<float>(clip.y / clip.w));
			REQUIRE(std::abs(ndc.x) < 1.0f);
			REQUIRE(std::abs(ndc.y) < 1.0f);
			const Ray ray = EditorPlacement::MakeViewRay(camera, ndc);
			const glm::dvec3 toPoint = glm::dvec3(pointed) - glm::dvec3(ray.Origin);
			const double distance = glm::length(glm::cross(toPoint, glm::normalize(glm::dvec3(ray.Direction))));
			CHECK(distance < 5e-3);
		}
	}

	TEST_CASE("AssetBounds measures a pending template again only once something it waits for was published")
	{
		PlacementHarness harness("PlacementPendingBounds");
		const std::filesystem::path models = harness.Directory / "Assets" / "Models";
		REQUIRE(FileSystem::CreateDirectories(models));
		REQUIRE(FileSystem::WriteBytes(models / "Arm.gltf", CreateSkinnedArmGltf()));
		EditorAssetManager& assets = *harness.Context.GetAssetManager();
		assets.Scan();
		const AssetHandle model = assets.FindAssetByPath("Models/Arm.gltf");
		REQUIRE(model.IsValid());
		harness.Settle();
		// The model's template resident, its mesh not.
		const Ref<EntityTemplate> entityTemplate = std::static_pointer_cast<EntityTemplate>(assets.LoadAssetSync(model));
		REQUIRE(entityTemplate);
		Scene scratch;
		REQUIRE_FALSE(entityTemplate->Instantiate(scratch).empty());
		AssetHandle mesh = UUID::Null();
		for (const auto [handle, renderer] : scratch.GetRegistry().view<MeshRendererComponent>().each())
			mesh = renderer.Mesh;
		REQUIRE(mesh.IsValid());
		harness.Settle();
		assets.UnloadAsset(mesh);
		REQUIRE(assets.GetAssetState(mesh) != AssetState::Ready);

		AssetBounds& bounds = harness.Context.GetAssetBounds();
		const uint64_t before = bounds.GetMeasureCount();
		CHECK(bounds.Measure(assets, model).Pending);
		CHECK(bounds.Measure(assets, model).Pending);
		CHECK(bounds.GetMeasureCount() == before + 1);
		// Another asset published (the content version moves): still answered from the cache.
		const uint64_t version = assets.GetContentVersion();
		AssetMetadata metadata;
		metadata.Type = AssetType::Material;
		metadata.Name = "Materials/Unrelated.stmat";
		assets.AddMemoryAsset(Material::Create(), metadata);
		REQUIRE(assets.GetContentVersion() != version);
		CHECK(bounds.Measure(assets, model).Pending);
		CHECK(bounds.GetMeasureCount() == before + 1);
		// Once the mesh is in, the next call measures and completes.
		harness.Settle();
		REQUIRE(assets.GetAssetState(mesh) == AssetState::Ready);
		const AssetBounds::Result complete = bounds.Measure(assets, model);
		CHECK_FALSE(complete.Pending);
		CHECK(complete.Bounds.IsValid());
		CHECK(bounds.GetMeasureCount() == before + 2);
	}

	TEST_CASE("scene.raycast hits what the scene draws from a ray or a Scene view pixel, and misses cleanly")
	{
		PlacementHarness harness("PlacementRaycastCommand");
		const std::string cube = harness.CreateCube(glm::vec3(0.0f, 0.5f, 0.0f));
		const size_t history = harness.Run("edit.history")["history"].size();
		const EditorCommand* command = harness.Commands.Find("scene.raycast");
		REQUIRE(command);
		CHECK(command->Kind == EditorCommandKind::Query);

		// Origin and direction onto the cube's top.
		nlohmann::json hit = harness.Run("scene.raycast", { { "origin", { 0.0, 10.0, 0.2 } }, { "direction", { 0.0, -2.0, 0.0 } } });
		CHECK(hit["hit"] == true);
		CHECK(hit["entity"] == cube);
		CHECK(hit["name"] == "Cube");
		CHECK(hit["point"][1].get<double>() == doctest::Approx(1.0));
		CHECK(hit["normal"][1].get<double>() == doctest::Approx(1.0));
		CHECK(hit["distance"].get<double>() == doctest::Approx(9.0));
		CHECK(hit["onTriangle"] == true);
		CHECK(hit["ray"]["direction"] == nlohmann::json::array({ 0.0, -1.0, 0.0 }));
		// Leaving the cube out: the ground under it. Too short: nothing, with the ray.
		hit = harness.Run("scene.raycast", { { "origin", { 0.0, 10.0, 0.2 } }, { "direction", { 0.0, -1.0, 0.0 } }, { "ignore", { cube } } });
		CHECK(hit["name"] == "Ground");
		hit = harness.Run("scene.raycast", { { "origin", { 0.0, 10.0, 0.2 } }, { "direction", { 0.0, -1.0, 0.0 } }, { "maxDistance", 5.0 } });
		CHECK(hit["hit"] == false);
		CHECK(hit.contains("ray"));

		// The middle of the Scene view (headless: 1280 x 720) through the editor camera looking at the cube.
		harness.Run("camera.set", { { "position", { 0.0, 6.0, 8.0 } }, { "target", { 0.0, 0.5, 0.0 } } });
		hit = harness.Run("scene.raycast", { { "viewport", { { "x", 640 }, { "y", 360 } } } });
		CHECK(hit["entity"] == cube);
		// With Game camera, through the scene's primary camera: one at (5, 3, 0) looking along -Z sees nothing.
		harness.Run("entity.create", { { "name", "Game Camera" }, { "components", { { "Camera", { { "Primary", true } } },
			{ "Transform", { { "Translation", { 5.0, 3.0, 0.0 } } } } } } });
		const Entity mainCamera = harness.GetScene().FindEntityByName("Main Camera");
		REQUIRE(mainCamera);
		harness.Run("component.set", { { "entity", UUIDToJson(mainCamera.GetUUID()) }, { "component", "Camera" }, { "values", { { "Primary", false } } } });
		harness.Run("viewport.setSettings", { { "pilotGameCamera", true } });
		hit = harness.Run("scene.raycast", { { "viewport", { { "x", 640 }, { "y", 360 }, { "width", 1280 }, { "height", 720 } } } });
		CHECK(hit["hit"] == false);
		CHECK(hit["ray"]["origin"][0].get<double>() == doctest::Approx(5.0).epsilon(1e-3));
		CHECK(hit["ray"]["direction"][2].get<double>() == doctest::Approx(-1.0).epsilon(1e-4));
		harness.Run("viewport.setSettings", { { "pilotGameCamera", false } });

		// Refused, changing nothing.
		const size_t steps = harness.Run("edit.history")["history"].size();
		for (const nlohmann::json& refused : {
			nlohmann::json { { "origin", { 0, 1, 0 } }, { "direction", { 0, -1, 0 } }, { "viewport", { { "x", 1 }, { "y", 1 } } } },
			nlohmann::json::object(),
			nlohmann::json { { "origin", { 0, 1, 0 } } },
			nlohmann::json { { "origin", { 0, 1, 0 } }, { "direction", { 0, 0, 0 } } },
			nlohmann::json { { "viewport", { { "x", 2000 }, { "y", 10 }, { "width", 1280 } } } },
			nlohmann::json { { "viewport", { { "x", 10 }, { "y", 10 }, { "depth", 3 } } } },
			nlohmann::json { { "origin", { 0, 1, 0 } }, { "direction", { 0, -1, 0 } }, { "ignore", { "00000000000000ab" } } } })
		{
			CAPTURE(refused.dump());
			const EditorCommandResult result = harness.Commands.Execute(harness.Context, "scene.raycast", refused);
			CHECK_FALSE(result.Success);
			CHECK(result.ErrorKind == EditorCommandError::InvalidParameters);
		}
		CHECK(harness.Run("edit.history")["history"].size() == steps);
		CHECK(steps >= history);
	}

	TEST_CASE("asset.place puts a prefab, a model or a mesh where the ray lands, in one undo step")
	{
		PlacementHarness harness("PlacementPlaceCommand");
		Scene& scene = harness.GetScene();
		const std::string source = harness.Run("entity.create", { { "name", "Crate" }, { "components", {
			{ "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } } } } })["id"].get<std::string>();
		const std::string prefab = harness.Run("prefab.create", { { "entities", { source } }, { "path", "Prefabs/Crate.stprefab" }, { "link", false } })["asset"]
			.get<std::string>();
		harness.Run("entity.delete", { { "entities", { source } } });
		const nlohmann::json ground = harness.Run("entity.find", { { "name", "Ground" } })["entities"];
		REQUIRE(ground.size() == 1);
		const auto down = [](float x, float z)
		{
			return nlohmann::json { { "origin", { x, 10.0f, z } }, { "direction", { 0.0f, -1.0f, 0.0f } } };
		};
		const auto entityOf = [&scene](const nlohmann::json& id)
		{
			return scene.GetEntityByUUID(*UUIDFromJson(id));
		};

		// By a ray onto the ground: one undo step, linked to its prefab.
		const size_t history = harness.Run("edit.history")["history"].size();
		nlohmann::json placed = harness.Complete("asset.place", { { "asset", prefab }, { "ray", down(2.0f, 3.0f) } });
		CHECK(placed["placedOn"] == "surface");
		CHECK(placed["hitEntity"] == ground[0]);
		CHECK(placed["snapped"] == false);
		CHECK(placed["entities"].size() == 1);
		const Entity crate = entityOf(placed["id"]);
		REQUIRE(crate);
		CHECK(PrefabInstances::GetPrefab(crate) == *UUIDFromJson(prefab));
		CHECK(harness.WorldPosition(placed["id"].get<std::string>()).x == doctest::Approx(2.0f));
		CHECK(harness.WorldPosition(placed["id"].get<std::string>()).y == doctest::Approx(0.5f).epsilon(1e-4));
		CHECK(placed["bounds"]["min"][1].get<double>() == doctest::Approx(0.0).epsilon(1e-4));
		const nlohmann::json steps = harness.Run("edit.history")["history"];
		REQUIRE(steps.size() == history + 1);
		CHECK(steps.back() == "Place 'Crate'");
		harness.Run("edit.undo");
		CHECK_FALSE(entityOf(placed["id"]));

		// Resting against a point of a wall facing -X.
		placed = harness.Complete("asset.place", { { "asset", prefab }, { "point", { 3.0, 1.0, 0.0 } }, { "normal", { -1.0, 0.0, 0.0 } } });
		CHECK(placed["placedOn"] == "point");
		CHECK(placed["hitEntity"].is_null());
		CHECK(harness.WorldPosition(placed["id"].get<std::string>()).x == doctest::Approx(2.5f));

		// A mesh: an entity named after it, drawing it.
		placed = harness.Complete("asset.place", { { "asset", "Builtin/Cube" }, { "ray", down(-3.0f, -3.0f) } });
		const Entity mesh = entityOf(placed["id"]);
		REQUIRE(mesh);
		CHECK(mesh.GetName() == "Cube");
		REQUIRE(mesh.HasComponent<MeshRendererComponent>());
		CHECK(mesh.GetComponent<MeshRendererComponent>().Mesh == BuiltinAssets::CubeMesh);
		CHECK(harness.WorldPosition(placed["id"].get<std::string>()).y == doctest::Approx(0.5f).epsilon(1e-4));

		// Turned and scaled over its own, still resting on the ground.
		placed = harness.Complete("asset.place", { { "asset", prefab }, { "ray", down(-5.0f, 4.0f) }, { "rotation", { 0.0, 90.0, 0.0 } }, { "scale", 2.0 } });
		const TransformComponent& turned = entityOf(placed["id"]).GetComponent<TransformComponent>();
		const glm::quat quarter = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		CHECK(std::abs(glm::dot(turned.Rotation, quarter)) == doctest::Approx(1.0f).epsilon(1e-5));
		CHECK(turned.Scale.x == doctest::Approx(2.0f));
		CHECK(turned.Translation.y == doctest::Approx(1.0f).epsilon(1e-4));
		CHECK(placed["bounds"]["min"][1].get<double>() == doctest::Approx(0.0).epsilon(1e-3));

		// Under a parent: the same world position as without one.
		const std::string parent = harness.Run("entity.create", { { "name", "Parent" }, { "components", { { "Transform", { { "Translation", { 5.0, 0.0, 0.0 } } } } } } })["id"]
			.get<std::string>();
		placed = harness.Complete("asset.place", { { "asset", prefab }, { "ray", down(1.0f, 1.0f) }, { "parent", parent } });
		const Entity child = entityOf(placed["id"]);
		REQUIRE(child);
		CHECK(child.GetParent().GetUUID() == *UUIDFromJson(parent));
		const glm::vec3 childPosition = harness.WorldPosition(placed["id"].get<std::string>());
		CHECK(childPosition.x == doctest::Approx(1.0f).epsilon(1e-4));
		CHECK(childPosition.y == doctest::Approx(0.5f).epsilon(1e-4));
		CHECK(childPosition.z == doctest::Approx(1.0f).epsilon(1e-4));

		// Snapped to a 1 m grid.
		placed = harness.Complete("asset.place", { { "asset", prefab }, { "ray", down(0.8f, -6.3f) }, { "snap", 1.0 } });
		CHECK(placed["snapped"] == true);
		CHECK(placed["translation"][0].get<double>() == doctest::Approx(1.0));
		CHECK(placed["translation"][2].get<double>() == doctest::Approx(-6.0));

		// A template of two roots moves rigidly: the second keeps its offset from the first, turned with it.
		const std::string left = harness.CreateCube(glm::vec3(0.0f, 0.5f, -8.0f));
		const std::string right = harness.CreateCube(glm::vec3(2.0f, 0.5f, -8.0f));
		const std::string pair = harness.Run("prefab.create", { { "entities", { left, right } }, { "path", "Prefabs/Pair.stprefab" }, { "link", false } })["asset"]
			.get<std::string>();
		placed = harness.Complete("asset.place", { { "asset", pair }, { "ray", down(-6.0f, 6.0f) }, { "rotation", { 0.0, 90.0, 0.0 } } });
		REQUIRE(placed["entities"].size() == 2);
		const glm::vec3 offset = harness.WorldPosition(placed["entities"][1].get<std::string>()) - harness.WorldPosition(placed["entities"][0].get<std::string>());
		CHECK(glm::length(offset - quarter * glm::vec3(2.0f, 0.0f, 0.0f)) < 1e-4f);

		// In a world, the new root joins the edit layer's cell where it stands, as entity.create's entities do.
		harness.Complete("world.create", { { "path", "Worlds/Island" }, { "cellSize", 32 }, { "force", true } });
		harness.Run("world.edit", { { "editLayer", "Terrain" } });
		Scene& world = *harness.Context.GetActiveScene();
		const nlohmann::json made = harness.Run("entity.create", { { "name", "Marker" }, { "components", {
			{ "Transform", { { "Translation", { 40.0, 0.0, 8.0 } } } } } } });
		placed = harness.Complete("asset.place", { { "asset", "Builtin/Cube" }, { "point", { 40.0, 0.0, 8.0 } } });
		const SubSceneID cell = world.GetSubSceneOf(world.GetEntityByUUID(*UUIDFromJson(made["id"])));
		CHECK(cell != 0);
		CHECK(world.GetSubSceneOf(world.GetEntityByUUID(*UUIDFromJson(placed["id"]))) == cell);
	}

	TEST_CASE("asset.place refuses what it cannot place and changes nothing")
	{
		PlacementHarness harness("PlacementPlaceRefused");
		const std::string material = harness.Run("material.create", { { "path", "Materials/Red.stmat" } })["asset"].get<std::string>();
		const nlohmann::json ray = { { "origin", { 0.0, 10.0, 0.0 } }, { "direction", { 0.0, -1.0, 0.0 } } };
		const size_t history = harness.Run("edit.history")["history"].size();
		const size_t entities = harness.GetScene().GetEntityCount();
		for (const nlohmann::json& refused : {
			nlohmann::json { { "asset", material }, { "ray", ray } },
			nlohmann::json { { "asset", "Builtin/DefaultMaterial" }, { "ray", ray } },
			nlohmann::json { { "asset", "Builtin/Cube" }, { "ray", ray }, { "point", { 0, 0, 0 } } },
			nlohmann::json { { "asset", "Builtin/Cube" } },
			nlohmann::json { { "asset", "Builtin/Cube" }, { "ray", ray }, { "normal", { 0, 1, 0 } } },
			nlohmann::json { { "asset", "Builtin/Cube" }, { "viewport", { { "x", 10 }, { "y", 10 } } }, { "fallbackDistance", 4.0 } },
			nlohmann::json { { "asset", "Builtin/Cube" }, { "point", { 0, 0, 0 } }, { "maxDistance", 4.0 } },
			nlohmann::json { { "asset", "Builtin/Cube" }, { "ray", ray }, { "scale", 0.0 } },
			nlohmann::json { { "asset", "Builtin/Cube" }, { "ray", ray }, { "snap", -1.0 } },
			nlohmann::json { { "asset", "Builtin/Cube" }, { "ray", ray }, { "parent", "00000000000000ab" } },
			nlohmann::json { { "asset", "Builtin/Cube" }, { "ray", { { "origin", { 0, 1, 0 } }, { "direction", { 0, 0, 0 } } } } },
			nlohmann::json { { "asset", "Builtin/Cube" }, { "ray", { { "origin", { 0, 1, 0 } }, { "direction", { 0, -1, 0 } }, { "length", 3 } } } } })
		{
			CAPTURE(refused.dump());
			const EditorCommandResult result = harness.Commands.Execute(harness.Context, "asset.place", refused);
			CHECK_FALSE(result.Success);
			CHECK_FALSE(result.IsPending());
			CHECK(result.ErrorKind == EditorCommandError::InvalidParameters);
		}
		// A ray at the sky with requireSurface: a valid request that places nothing.
		const EditorCommandResult sky = harness.Commands.Execute(harness.Context, "asset.place", { { "asset", "Builtin/Cube" },
			{ "ray", { { "origin", { 0, 1, 0 } }, { "direction", { 0, 1, 0 } } } }, { "requireSurface", true } });
		CHECK_FALSE(sky.Success);
		CHECK(sky.ErrorKind == EditorCommandError::Failed);
		CHECK(harness.Run("edit.history")["history"].size() == history);
		CHECK(harness.GetScene().GetEntityCount() == entities);
	}

	TEST_CASE("asset.place waits for a template that is not resident, and gives up on one that fails")
	{
		PlacementHarness harness("PlacementPlaceWaits");
		const std::string source = harness.Run("entity.create", { { "name", "Crate" }, { "components", {
			{ "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } } } } })["id"].get<std::string>();
		const std::string prefab = harness.Run("prefab.create", { { "entities", { source } }, { "path", "Prefabs/Crate.stprefab" }, { "link", false } })["asset"]
			.get<std::string>();
		const AssetHandle handle = *UUIDFromJson(prefab);
		EditorAssetManager& assets = *harness.Context.GetAssetManager();
		const nlohmann::json parameters = { { "asset", prefab }, { "ray", { { "origin", { 2.0, 10.0, 0.0 } }, { "direction", { 0.0, -1.0, 0.0 } } } } };

		// Not resident: it waits (0.7.0 loaded it at once, holding the frame), then places.
		harness.Settle();
		assets.UnloadAsset(handle);
		EditorCommandResult result = harness.Commands.Execute(harness.Context, "asset.place", parameters);
		CHECK(result.IsPending());
		result = Tests::CompleteCommand(harness.Context, std::move(result));
		REQUIRE(result.Success);
		CHECK(harness.WorldPosition(result.Value["id"].get<std::string>()).y == doctest::Approx(0.5f).epsilon(1e-4));

		// Playing starts while it waits: nothing is placed.
		assets.UnloadAsset(handle);
		result = harness.Commands.Execute(harness.Context, "asset.place", parameters);
		REQUIRE(result.IsPending());
		Tests::RunCommandToCompletion(harness.Context, harness.Commands, "play.start");
		result = Tests::CompleteCommand(harness.Context, std::move(result));
		CHECK_FALSE(result.Success);
		CHECK(result.Error.find("active scene changed") != std::string::npos);
		Tests::RunCommandToCompletion(harness.Context, harness.Commands, "play.stop");

		// A prefab whose file no longer loads: the load's error, nothing placed.
		const size_t entities = harness.GetScene().GetEntityCount();
		REQUIRE(FileSystem::WriteText(harness.Directory / "Assets" / "Prefabs" / "Crate.stprefab", "not a prefab"));
		assets.UnloadAsset(handle);
		result = Tests::CompleteCommand(harness.Context, harness.Commands.Execute(harness.Context, "asset.place", parameters));
		CHECK_FALSE(result.Success);
		CHECK(result.Error.find("could not be loaded") != std::string::npos);
		CHECK(harness.GetScene().GetEntityCount() == entities);

		// The project closes while it waits: cancelled.
		const std::string other = harness.Run("prefab.create", { { "entities", { source } }, { "path", "Prefabs/Other.stprefab" }, { "link", false } })["asset"]
			.get<std::string>();
		harness.Settle();
		assets.UnloadAsset(*UUIDFromJson(other));
		result = harness.Commands.Execute(harness.Context, "asset.place", { { "asset", other }, { "point", { 0.0, 0.0, 0.0 } } });
		REQUIRE(result.IsPending());
		Tests::RunCommandToCompletion(harness.Context, harness.Commands, "project.close", { { "force", true } });
		result = Tests::CompleteCommand(harness.Context, std::move(result));
		CHECK_FALSE(result.Success);
		CHECK(result.ErrorKind == EditorCommandError::Cancelled);
	}

	TEST_CASE("New content rests on the surface under the view's pivot, its bounds touching it")
	{
		PlacementHarness harness("PlacementView");
		EditorCamera camera;
		camera.LookAt(glm::vec3(4.0f, 6.0f, 8.0f), glm::vec3(1.0f, 0.0f, -2.0f));
		const EditorPlacement::Surface surface = EditorPlacement::FindPivotSurface(harness.GetScene(), camera);
		REQUIRE(surface.Hit);
		CHECK(surface.Point.x == doctest::Approx(1.0f).epsilon(1e-3));
		CHECK(surface.Point.y == doctest::Approx(0.0f).epsilon(1e-3));
		CHECK(surface.Point.z == doctest::Approx(-2.0f).epsilon(1e-3));
		const glm::vec3 translation = EditorPlacement::RestOn(surface, EditorPlacement::GetMeshBounds(BuiltinAssets::CubeMesh));
		CHECK(translation.y == doctest::Approx(0.5f).epsilon(1e-3));

		// A box whose origin is its bottom stands on the point; without bounds the origin goes there.
		CHECK(EditorPlacement::RestOn(surface, AABB(glm::vec3(-1.0f, 0.0f, -1.0f), glm::vec3(1.0f, 2.0f, 1.0f))).y == doctest::Approx(0.0f).epsilon(1e-3));
		CHECK(EditorPlacement::RestOn(surface, AABB()) == surface.Point);
		// Against a wall: out along its normal.
		const EditorPlacement::Surface wall { glm::vec3(5.0f, 1.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), true };
		CHECK(EditorPlacement::RestOn(wall, EditorPlacement::GetMeshBounds(BuiltinAssets::CubeMesh)).x == doctest::Approx(4.5f));

		// A pivot above the ground, looked at from a little above it: the ground under the pivot, not where the view's center
		// ray meets the ground far behind it.
		camera.LookAt(glm::vec3(0.0f, 2.0f, 10.0f), glm::vec3(0.0f, 1.5f, 0.0f));
		const EditorPlacement::Surface pivot = EditorPlacement::FindPivotSurface(harness.GetScene(), camera);
		CHECK(pivot.Hit);
		CHECK(pivot.Point.x == doctest::Approx(0.0f).epsilon(1e-3));
		CHECK(pivot.Point.y == doctest::Approx(0.0f).epsilon(1e-3));
		CHECK(pivot.Point.z == doctest::Approx(0.0f).epsilon(1e-3));
		// A roof over the pivot is no surface under it.
		harness.CreateCube(glm::vec3(0.0f, 4.0f, 0.0f));
		CHECK(EditorPlacement::FindPivotSurface(harness.GetScene(), camera).Point.y == doctest::Approx(0.0f).epsilon(1e-3));

		// Looking at the sky: what lies under the camera's target.
		camera.LookAt(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(3.0f, 50.0f, -1.0f));
		const EditorPlacement::Surface under = EditorPlacement::FindPivotSurface(harness.GetScene(), camera);
		CHECK(under.Hit);
		CHECK(under.Point.y == doctest::Approx(0.0f).epsilon(1e-3));
		// Nothing anywhere: the target itself.
		Scene empty;
		const EditorPlacement::Surface nothing = EditorPlacement::FindPivotSurface(empty, camera);
		CHECK_FALSE(nothing.Hit);
		CHECK(nothing.Point == camera.GetTarget());
	}

	TEST_CASE("A prefab's bounds are its meshes' around its root's origin, with the root's scale and its children's places")
	{
		PlacementHarness harness("PlacementTemplateBounds");
		// A crate (a cube scaled to 2 high) with a lid 1.5 above it, the root at (5, 0, 0) in the scene it was made from.
		const nlohmann::json created = harness.Run("entity.create", { { "name", "Crate" }, { "components", {
			{ "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } },
			{ "Transform", { { "Translation", { 5.0, 0.0, 0.0 } }, { "Scale", { 1.0, 2.0, 1.0 } } } } } } });
		const std::string crate = created["id"].get<std::string>();
		harness.Run("entity.create", { { "name", "Lid" }, { "parent", crate }, { "components", {
			{ "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } }, { "Transform", { { "Translation", { 0.0, 1.5, 0.0 } } } } } } });
		const AssetHandle prefab = *UUIDFromJson(harness.Run("prefab.create", { { "entities", { crate } }, { "path", "Prefabs/Crate.stprefab" } })["asset"]);

		const AABB bounds = EditorPlacement::GetTemplateBounds(prefab);
		REQUIRE(bounds.IsValid());
		CHECK(bounds.Min.x == doctest::Approx(-0.5f));
		CHECK(bounds.Max.x == doctest::Approx(0.5f));
		CHECK(bounds.Min.y == doctest::Approx(-1.0f)); // The crate's bottom, scaled
		CHECK(bounds.Max.y == doctest::Approx(4.0f));  // The lid's top: (1.5 + 0.5) * 2
		// Resting on the ground: the crate's bottom on it.
		CHECK(EditorPlacement::RestOn(EditorPlacement::Surface { glm::vec3(2.0f, 0.0f, 3.0f), glm::vec3(0.0f, 1.0f, 0.0f), true }, bounds).y == doctest::Approx(1.0f));
		// What is no prefab has no bounds.
		CHECK_FALSE(EditorPlacement::GetTemplateBounds(BuiltinAssets::CubeMesh).IsValid());
	}

	TEST_CASE("entity.dropToGround rests entities on what is below them in one undo step")
	{
		PlacementHarness harness("PlacementDrop");
		const std::string high = harness.CreateCube(glm::vec3(0.0f, 5.0f, 0.0f));
		const std::string child = harness.CreateCube(glm::vec3(2.0f, 2.0f, 0.0f), high); // Moves with its parent
		const std::string stacked = harness.CreateCube(glm::vec3(0.0f, 9.0f, 0.2f));
		const std::string beyond = harness.CreateCube(glm::vec3(5000.0f, 3.0f, 0.0f)); // Nothing below it
		const size_t history = harness.Run("edit.history")["history"].size();

		const nlohmann::json result = harness.Run("entity.dropToGround", { { "entities", { high, child, beyond } } });
		CHECK(result["moved"] == nlohmann::json::array({ high }));
		CHECK(result["unchanged"] == nlohmann::json::array({ beyond }));
		CHECK(harness.Run("edit.history")["history"].size() == history + 1);
		// The lowest point of the cube and its child (the parent's bottom) touches the ground; the child keeps its place on it.
		CHECK(harness.WorldPosition(high).y == doctest::Approx(0.5f).epsilon(1e-3));
		CHECK(harness.WorldPosition(child).y == doctest::Approx(2.5f).epsilon(1e-3));
		CHECK(harness.WorldPosition(beyond).y == doctest::Approx(3.0f));

		// The cube above lands on the one below.
		harness.Run("entity.dropToGround", { { "entities", { stacked } } });
		CHECK(harness.WorldPosition(stacked).y == doctest::Approx(1.5f).epsilon(1e-3));

		harness.Run("edit.undo");
		harness.Run("edit.undo");
		CHECK(harness.WorldPosition(high).y == doctest::Approx(5.0f));
		CHECK(harness.WorldPosition(stacked).y == doctest::Approx(9.0f));
	}
}

TEST_SUITE("Editor.UI.Placement")
{
	TEST_CASE("A Cube from the Create menu rests on the ground under the view's pivot, and the menu offers every built-in mesh")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f });
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(CreateTemporaryDirectory("PlacementUICube") / "Game") }, { "name", "Game" },
			{ "template", "basic3d" } });
		// The pivot 1.5 above the ground, looked at from above it: the view's center meets the ground (20 units wide) at z = -9,
		// 6 units behind the pivot.
		editor.Run("camera.set", { { "position", { 2.0, 4.0, 7.0 } }, { "target", { 2.0, 1.5, -3.0 } } });
		editor.Frames(3);
		REQUIRE(editor.Click("Hierarchy.Create"));
		for (const char* mesh : { "Create.Cube", "Create.Sphere", "Create.Plane", "Create.Cylinder", "Create.Capsule", "Create.Cone", "Create.Torus" })
			CHECK_MESSAGE(UI::ItemProbe::Find(mesh).has_value(), mesh);
		REQUIRE(editor.Click("Create.Cube"));
		const Entity cube = editor.Context().GetPrimarySelection();
		REQUIRE(cube);
		CHECK(cube.GetName() == "Cube");
		// Its bottom on the ground's top (y = 0), within 0.01, under the pivot; not half buried at the origin.
		const glm::vec3 position(cube.GetScene()->GetWorldTransform(cube)[3]);
		CHECK(std::abs(position.y - 0.5f) <= 0.01f);
		CHECK(position.x == doctest::Approx(2.0f).epsilon(1e-3));
		CHECK(position.z == doctest::Approx(-3.0f).epsilon(1e-3));
	}

	TEST_CASE("A prefab dropped into the viewport rests on the surface under the cursor; Add to Scene puts one under the pivot")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 1000.0f), 1.0f });
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(CreateTemporaryDirectory("PlacementUIPrefab") / "Game") }, { "name", "Game" },
			{ "template", "basic3d" } });
		// A crate prefab whose origin is the cube's center (as a cube from the Create menu has it).
		const std::string source = editor.Run("entity.create", { { "name", "Crate" }, { "components", {
			{ "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } } } } })["id"].get<std::string>();
		const AssetHandle prefab = *UUIDFromJson(editor.Run("prefab.create", { { "entities", { source } }, { "path", "Prefabs/Crate.stprefab" },
			{ "link", false } })["asset"]);
		editor.Run("entity.delete", { { "entities", { source } } });
		editor.Run("camera.set", { { "position", { 0.0, 8.0, 6.0 } }, { "target", { 0.0, 0.0, 0.0 } } });
		ContentBrowserPanel* browser = editor.Layer->GetPanels().Get<ContentBrowserPanel>(EditorPanels::c_ContentBrowser);
		REQUIRE(browser);
		editor.Layer->GetPanels().Focus(EditorPanels::c_ContentBrowser);
		browser->SetCurrentDirectory("Prefabs");
		editor.Frames(4);
		const std::optional<ContentBrowserPanel::ItemRect> tile = browser->FindItemRect(prefab);
		REQUIRE(tile);
		ViewportPanel* viewport = editor.Layer->GetPanels().Get<ViewportPanel>(EditorPanels::c_Viewport);
		REQUIRE(viewport);
		const ViewportImageArea image = viewport->GetImageArea();
		REQUIRE(image.Size.x > 0.0f);
		// Off the view's center, over the ground: where the cursor's ray meets it. On a whole pixel, as ImGui rounds the mouse
		// position down.
		const ImVec2 point(std::floor(image.Min.x + image.Size.x * 0.3f), std::floor(image.Min.y + image.Size.y * 0.65f));
		const glm::vec2 ndc((point.x - image.Min.x) / image.Size.x * 2.0f - 1.0f, 1.0f - (point.y - image.Min.y) / image.Size.y * 2.0f);
		const EditorCamera& camera = editor.Context().GetViewport().GetCamera();
		const std::optional<SceneRayHit> hit = SceneRaycast::Cast(*editor.Context().GetActiveScene(),
			EditorPlacement::MakeViewRay(camera.GetSceneCamera(image.Size.x / image.Size.y), ndc), camera.GetFar());
		REQUIRE(hit);
		CHECK(hit->HitEntity.GetName() == "Ground");
		const size_t history = editor.Run("edit.history")["history"].size();
		const uint64_t measures = editor.Context().GetAssetBounds().GetMeasureCount();

		const ImVec2 from((tile->Min.x + tile->Max.x) * 0.5f, (tile->Min.y + tile->Max.y) * 0.5f);
		editor.Harness.MoveMouse(from);
		editor.Frames(1);
		editor.Harness.SetMouseButton(ImGuiMouseButton_Left, true);
		editor.Frames(1);
		for (int step = 1; step <= 6; step++)
		{
			const float t = static_cast<float>(step) / 6.0f;
			editor.Harness.MoveMouse(ImVec2(from.x + (point.x - from.x) * t, from.y + (point.y - from.y) * t));
			editor.Frames(1);
		}
		// Hovering the view measured it (requesting what it needs), before the drop.
		CHECK(editor.Context().GetAssetBounds().GetMeasureCount() > measures);
		editor.Harness.SetMouseButton(ImGuiMouseButton_Left, false);
		editor.Frames(2);

		// The instance's root is selected, where the cursor's ray meets the ground, its bottom on it.
		const Entity root = editor.Context().GetPrimarySelection();
		REQUIRE(root);
		CHECK(PrefabInstances::IsInstanceRoot(root));
		CHECK(PrefabInstances::GetPrefab(root) == prefab);
		CHECK(editor.Run("edit.history")["history"].size() == history + 1);
		const glm::vec3 position(root.GetScene()->GetWorldTransform(root)[3]);
		CHECK(position.x == doctest::Approx(hit->Point.x).epsilon(1e-3));
		CHECK(position.z == doctest::Approx(hit->Point.z).epsilon(1e-3));
		CHECK(std::abs(position.y - 0.5f) <= 0.01f);

		// The Asset tab's Add to Scene: under the pivot, resting on the ground.
		editor.Run("camera.set", { { "position", { 4.0, 3.0, 9.0 } }, { "target", { 4.0, 1.0, 1.0 } } });
		editor.Run("selection.set", { { "asset", "Prefabs/Crate.stprefab" } });
		editor.Frames(3);
		REQUIRE(editor.Click("AssetInspector.AddToScene"));
		const Entity added = editor.Context().GetPrimarySelection();
		REQUIRE(added);
		CHECK(added != root);
		CHECK(PrefabInstances::GetPrefab(added) == prefab);
		const glm::vec3 addedPosition(added.GetScene()->GetWorldTransform(added)[3]);
		CHECK(addedPosition.x == doctest::Approx(4.0f).epsilon(1e-3));
		CHECK(addedPosition.z == doctest::Approx(1.0f).epsilon(1e-3));
		CHECK(std::abs(addedPosition.y - 0.5f) <= 0.01f);
	}

	TEST_CASE("Dropping a material onto a mesh in the viewport gives it the material in one undo step")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 1000.0f), 1.0f });
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(CreateTemporaryDirectory("PlacementUIMaterial") / "Game") }, { "name", "Game" },
			{ "template", "basic3d" } });
		const std::string red = editor.Run("material.create", { { "path", "Materials/Red.stmat" }, { "properties", { { "BaseColor", { 1.0, 0.0, 0.0, 1.0 } } } } })["asset"]
			.get<std::string>();
		// The ground fills the middle of the view.
		editor.Run("camera.set", { { "position", { 0.0, 8.0, 6.0 } }, { "target", { 0.0, 0.0, 0.0 } } });
		ContentBrowserPanel* browser = editor.Layer->GetPanels().Get<ContentBrowserPanel>(EditorPanels::c_ContentBrowser);
		REQUIRE(browser);
		editor.Layer->GetPanels().Focus(EditorPanels::c_ContentBrowser);
		browser->SetCurrentDirectory("Materials");
		editor.Frames(4);
		const std::optional<ContentBrowserPanel::ItemRect> tile = browser->FindItemRect(*UUIDFromJson(red));
		REQUIRE(tile);
		ViewportPanel* viewport = editor.Layer->GetPanels().Get<ViewportPanel>(EditorPanels::c_Viewport);
		REQUIRE(viewport);
		const ViewportImageArea& image = viewport->GetImageArea();
		REQUIRE(image.Size.x > 0.0f);
		const ImVec2 center(image.Min.x + image.Size.x * 0.5f, image.Min.y + image.Size.y * 0.5f);
		const size_t history = editor.Run("edit.history")["history"].size();

		// The drag, from the tile to the view's center.
		const ImVec2 from((tile->Min.x + tile->Max.x) * 0.5f, (tile->Min.y + tile->Max.y) * 0.5f);
		editor.Harness.MoveMouse(from);
		editor.Frames(1);
		editor.Harness.SetMouseButton(ImGuiMouseButton_Left, true);
		editor.Frames(1);
		for (int step = 1; step <= 6; step++)
		{
			const float t = static_cast<float>(step) / 6.0f;
			editor.Harness.MoveMouse(ImVec2(from.x + (center.x - from.x) * t, from.y + (center.y - from.y) * t));
			editor.Frames(1);
		}
		editor.Harness.SetMouseButton(ImGuiMouseButton_Left, false);
		editor.Frames(2);

		const nlohmann::json ground = editor.Run("entity.find", { { "name", "Ground" } })["entities"];
		REQUIRE(ground.size() == 1);
		CHECK(editor.Run("component.get", { { "entity", ground[0] }, { "component", "MeshRenderer" } })["values"]["Material"] == red);
		CHECK(editor.Run("edit.history")["history"].size() == history + 1);
		editor.Run("edit.undo");
		CHECK(editor.Run("component.get", { { "entity", ground[0] }, { "component", "MeshRenderer" } })["values"]["Material"] != red);
	}
}
