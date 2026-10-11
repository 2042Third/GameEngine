#include <doctest/doctest.h>

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
#include <Strata/Scene/Scene.h>

#include <imgui.h>

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

	TEST_CASE("New content rests on the surface the view looks at, its bounds touching it")
	{
		PlacementHarness harness("PlacementView");
		EditorCamera camera;
		camera.LookAt(glm::vec3(4.0f, 6.0f, 8.0f), glm::vec3(1.0f, 0.0f, -2.0f));
		const EditorPlacement::Surface surface = EditorPlacement::FindViewSurface(harness.GetScene(), camera);
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

		// Looking at the sky: what lies under the camera's target.
		camera.LookAt(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, 50.0f, -1.0f));
		const EditorPlacement::Surface under = EditorPlacement::FindViewSurface(harness.GetScene(), camera);
		CHECK(under.Hit);
		CHECK(under.Point.y == doctest::Approx(0.0f).epsilon(1e-3));
		// Nothing anywhere: the target itself.
		Scene empty;
		const EditorPlacement::Surface nothing = EditorPlacement::FindViewSurface(empty, camera);
		CHECK_FALSE(nothing.Hit);
		CHECK(nothing.Point == camera.GetTarget());
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
	TEST_CASE("A Cube from the Create menu rests on the ground where the view looks, and the menu offers every built-in mesh")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f });
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(CreateTemporaryDirectory("PlacementUICube") / "Game") }, { "name", "Game" },
			{ "template", "basic3d" } });
		editor.Frames(3);
		REQUIRE(editor.Click("Hierarchy.Create"));
		for (const char* mesh : { "Create.Cube", "Create.Sphere", "Create.Plane", "Create.Cylinder", "Create.Capsule", "Create.Cone", "Create.Torus" })
			CHECK_MESSAGE(UI::ItemProbe::Find(mesh).has_value(), mesh);
		REQUIRE(editor.Click("Create.Cube"));
		const Entity cube = editor.Context().GetPrimarySelection();
		REQUIRE(cube);
		CHECK(cube.GetName() == "Cube");
		// Its bottom on the ground's top (y = 0), within 0.01; not half buried at the origin.
		const glm::vec3 position(cube.GetScene()->GetWorldTransform(cube)[3]);
		CHECK(position.y - 0.5f == doctest::Approx(0.0f).epsilon(0.01));
		CHECK(std::abs(position.y - 0.5f) <= 0.01f);
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
