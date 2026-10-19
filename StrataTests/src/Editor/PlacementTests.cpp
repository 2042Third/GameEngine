#include <doctest/doctest.h>

#include "Editor/AssetBounds.h"
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
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/PrefabInstances.h>
#include <Strata/Scene/Scene.h>

#include <imgui.h>

#include <cmath>
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

		// A basic3d project: its Ground's top is at y = 0.
		explicit PlacementHarness(const std::string& name)
		{
			Run("project.create", { { "directory", FileSystem::ToUTF8(CreateTemporaryDirectory(name) / "Game") }, { "name", "Game" }, { "template", "basic3d" } });
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			REQUIRE_FALSE(result.IsPending());
			return result.Value;
		}

		Scene& GetScene() { return *Context.GetActiveScene(); }

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
		const std::optional<SceneRayHit> hit = EditorPlacement::CastAt(*editor.Context().GetActiveScene(), editor.Context().GetViewport().GetCamera(), ndc,
			image.Size.x / image.Size.y);
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
