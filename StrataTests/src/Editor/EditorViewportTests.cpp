#include <doctest/doctest.h>

#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorViewport.h"
#include "Editor/SceneBounds.h"
#include "TestHelpers.h"

#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Math/Math.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/SceneSerializer.h>

#include <cmath>
#include <limits>

using namespace Strata;

namespace
{

	struct ViewportHarness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			REQUIRE_FALSE(result.IsPending());
			return result.Value;
		}

		std::string Error(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			CHECK_FALSE(result.Success);
			CHECK_FALSE(result.IsPending());
			return result.Error;
		}

		nlohmann::json Snapshot() const
		{
			return SceneSerializer::Serialize(*Context.GetEditScene())["Scene"]["Entities"];
		}
	};

	glm::vec3 ToVec3(const nlohmann::json& json)
	{
		return glm::vec3(json[0].get<float>(), json[1].get<float>(), json[2].get<float>());
	}

	bool Near(const glm::vec3& a, const glm::vec3& b, float epsilon = 1e-3f)
	{
		return Math::IsNearlyEqual(a, b, epsilon);
	}

	Entity CreateEntity(Scene& scene, const std::string& name, const glm::vec3& translation, const glm::vec3& scale = glm::vec3(1.0f), Entity parent = {})
	{
		Entity entity = parent ? scene.CreateChildEntity(parent, name) : scene.CreateEntity(name);
		entity.GetComponent<TransformComponent>().Translation = translation;
		entity.GetComponent<TransformComponent>().Scale = scale;
		return entity;
	}

}

TEST_SUITE("Editor.Viewport")
{
	TEST_CASE("Bounds use loaded meshes, placeholders and descendants")
	{
		EditorContext context(EditorContextSpecification { false });
		Scene& scene = *context.GetEditScene();
		Entity cube = CreateEntity(scene, "Cube", glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(2.0f));
		cube.AddComponent<MeshRendererComponent>().Mesh = BuiltinAssets::CubeMesh;
		Entity light = CreateEntity(scene, "Light", glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f), cube); // At (5, 0, 0), scaled by 2
		light.AddComponent<PointLightComponent>();

		AABB bounds = SceneBounds::GetEntityBounds(scene, cube, false);
		CHECK(Near(bounds.Min, glm::vec3(4.0f, -1.0f, -1.0f)));
		CHECK(Near(bounds.Max, glm::vec3(6.0f, 1.0f, 1.0f)));
		// The light has no extent of its own: a placeholder box scaled like the entity.
		bounds = SceneBounds::GetEntityBounds(scene, light, false);
		const float extent = SceneBounds::c_PlaceholderExtent * 2.0f;
		CHECK(Near(bounds.Min, glm::vec3(5.0f - extent, -extent, -extent)));

		Entity distant = CreateEntity(scene, "Distant", glm::vec3(0.0f, 0.0f, -20.0f));
		Entity distantChild = CreateEntity(scene, "DistantChild", glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(1.0f), distant);
		bounds = SceneBounds::GetEntityBounds(scene, distant, true);
		CHECK(bounds.Max.y == doctest::Approx(10.0f + SceneBounds::c_PlaceholderExtent));
		const std::vector<Entity> both = { cube, distantChild };
		bounds = SceneBounds::GetEntitiesBounds(scene, both);
		CHECK(bounds.Min.z == doctest::Approx(-20.0f - SceneBounds::c_PlaceholderExtent));
		CHECK(bounds.Max.x == doctest::Approx(6.0f));

		// Inactive entities do not count for the whole scene.
		distant.SetActive(false);
		bounds = SceneBounds::GetSceneBounds(scene);
		CHECK(bounds.Min.z == doctest::Approx(-1.0f));
		CHECK_FALSE(SceneBounds::GetEntityBounds(scene, Entity(), true).IsValid());
		Scene empty;
		CHECK_FALSE(SceneBounds::GetSceneBounds(empty).IsValid());
	}

	TEST_CASE("camera.get and camera.set change the view without touching the scene or the history")
	{
		ViewportHarness harness;
		harness.Run("entity.create", { { "name", "Keep" } });
		const nlohmann::json scene = harness.Snapshot();
		const size_t history = harness.Context.GetUndoStack().GetHistory().size();

		nlohmann::json camera = harness.Run("camera.get");
		for (const char* key : { "position", "target", "forward", "yaw", "pitch", "distance", "fov", "near", "far", "flySpeed" })
			CHECK(camera.contains(key));

		camera = harness.Run("camera.set", { { "position", { 0, 5, 10 } }, { "target", { 0, 0, 0 } } });
		CHECK(Near(ToVec3(camera["position"]), glm::vec3(0.0f, 5.0f, 10.0f)));
		CHECK(Near(ToVec3(camera["target"]), glm::vec3(0.0f)));
		CHECK(camera["yaw"].get<float>() == doctest::Approx(0.0f));
		CHECK(camera["distance"].get<float>() == doctest::Approx(std::sqrt(125.0f)));

		// Angles orbit around the target; the distance moves along the view.
		camera = harness.Run("camera.set", { { "yaw", 90 }, { "pitch", 0 }, { "distance", 4 } });
		CHECK(Near(ToVec3(camera["position"]), glm::vec3(4.0f, 0.0f, 0.0f)));
		CHECK(Near(ToVec3(camera["forward"]), glm::vec3(-1.0f, 0.0f, 0.0f)));

		// A position or target alone moves the camera, keeping the orientation.
		camera = harness.Run("camera.set", { { "target", { 1, 2, 3 } } });
		CHECK(Near(ToVec3(camera["position"]), glm::vec3(5.0f, 2.0f, 3.0f)));
		camera = harness.Run("camera.set", { { "position", { 0, 0, 0 } }, { "fov", 45 }, { "near", 0.5 }, { "far", 100 }, { "flySpeed", 3 } });
		CHECK(Near(ToVec3(camera["target"]), glm::vec3(-4.0f, 0.0f, 0.0f)));
		CHECK(camera["fov"] == 45.0);
		CHECK(camera["near"] == 0.5);
		CHECK(camera["far"] == 100.0);
		CHECK(camera["flySpeed"] == 3.0);
		CHECK(harness.Run("camera.get") == camera);

		// Invalid requests change nothing.
		CHECK(harness.Error("camera.set", { { "position", { 1, 1, 1 } }, { "target", { 1, 1, 1 } } }).find("apart") != std::string::npos);
		CHECK(harness.Error("camera.set", { { "position", { 0, 0, 0 } }, { "target", { 3e6, 0, 0 } } }).find("apart") != std::string::npos);
		CHECK(harness.Error("camera.set", { { "position", { 1, 1, 1 } }, { "target", { 0, 0, 0 } }, { "yaw", 3 } }).find("either") != std::string::npos);
		CHECK(harness.Error("camera.set", { { "near", 200 } }).find("below far") != std::string::npos);
		CHECK(harness.Error("camera.set", { { "fov", 0 } }).find("'fov'") != std::string::npos);
		CHECK(harness.Error("camera.set", { { "pitch", 90 } }).find("'pitch'") != std::string::npos);
		CHECK(harness.Error("camera.set", { { "position", { 1, 2 } } }).find("'position'") != std::string::npos);
		CHECK(harness.Error("camera.set", { { "target", { 1, 2, "3" } } }).find("'target'") != std::string::npos);
		CHECK(harness.Error("camera.set", { { "distance", "far" } }).find("'distance'") != std::string::npos);
		CHECK(harness.Run("camera.get") == camera);

		CHECK(harness.Snapshot() == scene);
		CHECK(harness.Context.GetUndoStack().GetHistory().size() == history);
		CHECK_FALSE(harness.Context.GetUndoStack().CanRedo());
	}

	TEST_CASE("camera.focus frames entities or the whole scene")
	{
		ViewportHarness harness;
		CHECK(harness.Error("camera.focus").find("Nothing to frame") != std::string::npos);

		const std::string first = harness.Run("entity.create", { { "name", "First" },
			{ "components", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } } } } })["id"].get<std::string>();
		const std::string second = harness.Run("entity.create", { { "name", "Second" },
			{ "components", { { "Transform", { { "Translation", { 30, 0, 0 } } } }, { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } } } } })["id"].get<std::string>();
		const nlohmann::json before = harness.Run("camera.get");

		nlohmann::json camera = harness.Run("camera.focus", { { "entities", { second } } });
		CHECK(Near(ToVec3(camera["target"]), glm::vec3(30.0f, 0.0f, 0.0f)));
		CHECK(camera["yaw"] == before["yaw"]);
		CHECK(camera["pitch"] == before["pitch"]);
		const float single = camera["distance"].get<float>();

		camera = harness.Run("camera.focus");
		CHECK(Near(ToVec3(camera["target"]), glm::vec3(15.0f, 0.0f, 0.0f)));
		CHECK(camera["distance"].get<float>() > single);
		camera = harness.Run("camera.focus", { { "entities", { first, second } } });
		CHECK(Near(ToVec3(camera["target"]), glm::vec3(15.0f, 0.0f, 0.0f)));

		CHECK(harness.Error("camera.focus", { { "entities", nlohmann::json::array() } }).find("'entities'") != std::string::npos);
		CHECK(harness.Error("camera.focus", { { "entities", { "00000000000000AB" } } }).find("no entity") != std::string::npos);
		CHECK(harness.Context.GetUndoStack().GetHistory().size() == 2); // The two entities only
	}

	TEST_CASE("viewport.capture validates its parameters before needing a GPU")
	{
		ViewportHarness harness;
		CHECK(harness.Error("viewport.capture", { { "camera", "game" } }).find("'camera'") != std::string::npos);
		CHECK(harness.Error("viewport.capture", { { "width", 8 } }).find("'width'") != std::string::npos);
		CHECK(harness.Error("viewport.capture", { { "height", 100000 } }).find("'height'") != std::string::npos);
		CHECK(harness.Error("viewport.capture", { { "path", "Capture.jpg" } }).find(".png") != std::string::npos);
		CHECK(harness.Error("viewport.capture", { { "path", "" } }).find("empty") != std::string::npos);
		CHECK(harness.Error("viewport.capture", { { "path", "Capture.png" } }).find("no project is open") != std::string::npos);
		CHECK(harness.Error("viewport.capture", { { "overwrite", 1 } }).find("'overwrite'") != std::string::npos);
		CHECK(harness.Error("viewport.capture", { { "overlays", "yes" } }).find("'overlays'") != std::string::npos);
	}

	TEST_CASE("Output paths resolve against the project and never replace files by accident")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("OutputPaths");
		ViewportHarness harness;
		std::string error;
		auto resolve = [&](std::string_view path, bool overwrite = false)
		{
			error.clear();
			return CommandUtils::ResolveOutputPath(harness.Context, path, ".png", overwrite, &error);
		};

		// Without a project only absolute paths work: the editor's working directory means nothing to a client.
		CHECK_FALSE(resolve("Shot.png"));
		CHECK(error.find("no project") != std::string::npos);
		const std::filesystem::path absolute = directory / "Elsewhere" / "Shot.png";
		CHECK(resolve(FileSystem::ToUTF8(absolute)) == absolute.lexically_normal());

		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory / "Game") }, { "name", "Game" } });
		const std::filesystem::path projectDirectory = harness.Context.GetProject()->GetProjectDirectory();
		CHECK(resolve("Captures/Shot.png") == (projectDirectory / "Captures" / "Shot.png").lexically_normal());
		CHECK(resolve("Captures/../Shot.PNG") == (projectDirectory / "Shot.PNG").lexically_normal());

		// Network and device paths, paths that are neither relative nor fully absolute, reserved device names and other
		// extensions are refused.
		for (const char* refused : { "\\\\server\\share\\Shot.png", "//server/share/Shot.png", "\\\\?\\C:\\Shot.png", "\\\\.\\NUL.png", "NUL.png",
			"Captures/con.png", "com1.png", "Captures/LPT9.dump.png", "Shot.jpg", "Shot" })
		{
			CAPTURE(refused);
			CHECK_FALSE(resolve(refused));
			CHECK_FALSE(error.empty());
		}
#if defined(ST_PLATFORM_WINDOWS)
		CHECK_FALSE(resolve("C:Shot.png"));   // Relative to the drive's current folder
		CHECK_FALSE(resolve("\\Shot.png")); // On the current drive
#endif

		// An existing file is replaced only on request; a folder never.
		REQUIRE(FileSystem::WriteText(projectDirectory / "Existing.png", "old"));
		CHECK_FALSE(resolve("Existing.png"));
		CHECK(error.find("overwrite") != std::string::npos);
		CHECK(resolve("Existing.png", true) == (projectDirectory / "Existing.png").lexically_normal());
		REQUIRE(FileSystem::CreateDirectories(projectDirectory / "Folder.png"));
		CHECK_FALSE(resolve("Folder.png", true));
		CHECK(error.find("not a regular file") != std::string::npos);
		CHECK(harness.Error("viewport.capture", { { "path", "Existing.png" } }).find("overwrite") != std::string::npos);
	}

	TEST_CASE("The view follows the play state")
	{
		ViewportHarness harness;
		EditorContext& context = harness.Context;
		std::string error;

		std::optional<ViewportView> view = ResolveViewportView(context, ViewportCameraSource::Automatic, 1.5f);
		REQUIRE(view);
		CHECK_FALSE(view->FromScene);
		CHECK(view->EditorOverlays);
		CHECK(view->Notice.empty());
		CHECK(view->Camera.View == context.GetViewport().GetCamera().GetViewMatrix());
		CHECK_FALSE(ResolveViewportView(context, ViewportCameraSource::Scene, 1.5f, &error));
		CHECK(error.find("Primary") != std::string::npos);

		// Playing without a camera falls back to the editor camera, with a notice.
		REQUIRE(context.Play());
		view = ResolveViewportView(context, ViewportCameraSource::Automatic, 1.5f);
		REQUIRE(view);
		CHECK_FALSE(view->FromScene);
		CHECK_FALSE(view->Notice.empty());
		context.Stop();

		// With a primary camera, playing looks through it; simulating and editing keep the editor camera.
		harness.Run("entity.create", { { "name", "Camera" }, { "components", { { "Camera", nlohmann::json::object() }, { "Transform", { { "Translation", { 0, 1, 7 } } } } } } });
		view = ResolveViewportView(context, ViewportCameraSource::Scene, 1.5f);
		REQUIRE(view);
		CHECK(view->FromScene);
		CHECK_FALSE(view->EditorOverlays);
		CHECK(Near(view->Camera.Position, glm::vec3(0.0f, 1.0f, 7.0f)));
		REQUIRE(context.Play());
		view = ResolveViewportView(context, ViewportCameraSource::Automatic, 1.5f);
		REQUIRE(view);
		CHECK(view->FromScene);
		view = ResolveViewportView(context, ViewportCameraSource::Editor, 1.5f);
		REQUIRE(view);
		CHECK_FALSE(view->FromScene);
		context.Stop();
		REQUIRE(context.Simulate());
		view = ResolveViewportView(context, ViewportCameraSource::Automatic, 1.5f);
		REQUIRE(view);
		CHECK_FALSE(view->FromScene);
		context.Stop();
	}

	TEST_CASE("The viewport image maps UI positions to framebuffer pixels")
	{
		ViewportImageArea area;
		area.Min = glm::vec2(100.0f, 40.0f);
		area.Size = glm::vec2(320.5f, 180.0f);
		CHECK(area.GetPixelSize() == glm::uvec2(320, 180));
		CHECK(area.ToPixel(glm::vec2(100.0f, 40.0f)) == glm::uvec2(0, 0));
		CHECK(area.ToPixel(glm::vec2(420.0f, 219.5f)) == glm::uvec2(319, 179)); // The remainder of the last pixel
		CHECK_FALSE(area.ToPixel(glm::vec2(99.9f, 50.0f)));
		CHECK_FALSE(area.ToPixel(glm::vec2(420.5f, 50.0f)));
		CHECK_FALSE(area.ToPixel(glm::vec2(200.0f, 220.0f)));

		// A Retina display has two pixels per unit: the image is rendered at twice the size and positions scale with it.
		area.PixelScale = glm::vec2(2.0f);
		CHECK(area.GetPixelSize() == glm::uvec2(641, 360));
		CHECK(area.ToPixel(glm::vec2(110.25f, 45.75f)) == glm::uvec2(20, 11));
		CHECK(area.ToPixel(glm::vec2(420.4f, 219.9f)) == glm::uvec2(640, 359));

		// Viewports have no scale of their own without multi-viewport support: the display's applies (Retina: 2).
		CHECK(ViewportImageArea::ChoosePixelScale(glm::vec2(0.0f), glm::vec2(2.0f)) == glm::vec2(2.0f));
		CHECK(ViewportImageArea::ChoosePixelScale(glm::vec2(1.5f), glm::vec2(2.0f)) == glm::vec2(1.5f));
		CHECK(ViewportImageArea::ChoosePixelScale(glm::vec2(0.0f), glm::vec2(0.0f)) == glm::vec2(1.0f));
		CHECK(ViewportImageArea::ChoosePixelScale(glm::vec2(-1.0f), glm::vec2(std::nanf(""))) == glm::vec2(1.0f));

		// Empty or degenerate areas have no pixels.
		area.Size = glm::vec2(0.0f, 10.0f);
		CHECK(area.GetPixelSize().x == 0);
		CHECK_FALSE(area.ToPixel(area.Min));
		area.Size = glm::vec2(10.0f);
		area.PixelScale = glm::vec2(std::numeric_limits<float>::infinity());
		CHECK(area.GetPixelSize() == glm::uvec2(0));
	}

	TEST_CASE("Only a playing game takes the input, and edit shortcuts stay off meanwhile")
	{
		EditorContext context(EditorContextSpecification { false });
		CHECK(context.AcceptsEditShortcuts());
		context.SetGameInputActive(true); // Editing: there is no game to receive input
		CHECK_FALSE(context.IsGameInputActive());
		CHECK(context.AcceptsEditShortcuts());

		REQUIRE(context.Simulate());
		context.SetGameInputActive(true); // Simulating runs no game logic
		CHECK_FALSE(context.IsGameInputActive());
		context.Stop();

		REQUIRE(context.Play());
		context.SetGameInputActive(true);
		CHECK(context.IsGameInputActive());
		CHECK_FALSE(context.AcceptsEditShortcuts());
		context.SetGameInputActive(false); // The game view lost focus
		CHECK(context.AcceptsEditShortcuts());
		context.SetGameInputActive(true);
		context.Stop(); // Stopping ends the game's input
		CHECK_FALSE(context.IsGameInputActive());
		CHECK(context.AcceptsEditShortcuts());
	}

	TEST_CASE("Clicks select, toggle, add and clear")
	{
		EditorContext context(EditorContextSpecification { false });
		Scene& scene = *context.GetEditScene();
		Entity a = scene.CreateEntity("A");
		Entity b = scene.CreateEntity("B");

		EditorViewport::ApplyPick(context, a, ViewportPickMode::Replace);
		CHECK(context.GetSelection() == std::vector<UUID> { a.GetUUID() });
		EditorViewport::ApplyPick(context, b, ViewportPickMode::Add);
		CHECK(context.GetSelection() == std::vector<UUID> { a.GetUUID(), b.GetUUID() });
		EditorViewport::ApplyPick(context, a, ViewportPickMode::Add); // Becomes the primary selection
		CHECK(context.GetPrimarySelection() == a);
		EditorViewport::ApplyPick(context, a, ViewportPickMode::Toggle);
		CHECK(context.GetSelection() == std::vector<UUID> { b.GetUUID() });
		EditorViewport::ApplyPick(context, a, ViewportPickMode::Toggle);
		CHECK(context.GetSelection().size() == 2);

		// Empty space: only a plain click clears.
		EditorViewport::ApplyPick(context, Entity(), ViewportPickMode::Toggle);
		EditorViewport::ApplyPick(context, Entity(), ViewportPickMode::Add);
		CHECK(context.GetSelection().size() == 2);
		EditorViewport::ApplyPick(context, Entity(), ViewportPickMode::Replace);
		CHECK(context.GetSelection().empty());

		// Entities of another scene are treated as empty space.
		Scene other;
		Entity stranger = other.CreateEntity("Stranger");
		context.Select(a.GetUUID());
		EditorViewport::ApplyPick(context, stranger, ViewportPickMode::Replace);
		CHECK(context.GetSelection().empty());

		// Without a renderer nothing can be picked.
		CHECK_FALSE(context.GetViewport().RequestPick(glm::uvec2(1, 1), glm::uvec2(64, 64), ViewportPickMode::Replace));
		CHECK_FALSE(context.GetViewport().IsPickPending());
	}

	TEST_CASE("Viewport settings round trip and reject invalid values")
	{
		ViewportSettings settings;
		settings.ShowGrid = false;
		settings.ShowStats = true;
		settings.Gizmo = GizmoOperation::Rotate;
		settings.Space = GizmoSpace::Local;
		settings.Snap = true;
		settings.TranslateSnap = 0.25f;
		settings.RotateSnap = 45.0f;
		settings.ScaleSnap = 0.5f;
		ViewportSettings restored;
		std::string error;
		REQUIRE_MESSAGE(restored.FromJson(settings.ToJson(), &error), error);
		CHECK(restored.ToJson() == settings.ToJson());
		CHECK(restored.Snap);
		// The selection color is the editor theme's, not saved with the project.
		CHECK_FALSE(settings.ToJson().contains("SelectionColor"));

		const nlohmann::json before = restored.ToJson();
		for (const nlohmann::json& invalid : { nlohmann::json("x"), nlohmann::json { { "ShowGrid", 1 } }, nlohmann::json { { "Snap", "yes" } },
			nlohmann::json { { "Gizmo", "Move" } },
			nlohmann::json { { "Space", "Global" } }, nlohmann::json { { "RotateSnap", 0 } }, nlohmann::json { { "TranslateSnap", 1e9 } },
			nlohmann::json { { "ScaleSnap", -1 } }, nlohmann::json { { "ScaleSnap", 1e300 } }, nlohmann::json { { "ShowStats", false }, { "ScaleSnap", "big" } } })
		{
			CAPTURE(invalid.dump());
			CHECK_FALSE(restored.FromJson(invalid, &error));
			CHECK(restored.ToJson() == before);
		}
	}

	TEST_CASE("The editor camera and viewport settings persist per project")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ViewportProject");
		const std::filesystem::path other = Tests::CreateTemporaryDirectory("ViewportOtherProject");
		nlohmann::json savedCamera;
		{
			ViewportHarness harness;
			harness.Run("project.create", { { "directory", FileSystem::ToUTF8(other / "Other") }, { "name", "Other" } });
			harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory / "Game") }, { "name", "Game" } });
			savedCamera = harness.Run("camera.set", { { "position", { 3, 4, 5 } }, { "target", { 0, 1, 0 } }, { "fov", 50 } });
			harness.Context.GetViewport().GetSettings().ShowGrid = false;
			harness.Context.GetViewport().GetSettings().RotateSnap = 30.0f;
			harness.Context.GetViewport().GetSettings().Snap = true;
			const glm::vec4 themeColor(0.25f, 0.5f, 0.75f, 1.0f);
			harness.Context.GetViewport().GetSettings().SelectionColor = themeColor;

			// Opening another project saves this one's view and starts from that project's (default) view; the selection
			// color (the editor's theme) stays.
			harness.Run("project.open", { { "path", FileSystem::ToUTF8(other / "Other") } });
			CHECK_FALSE(harness.Context.GetViewport().GetSettings().Snap);
			CHECK(harness.Context.GetViewport().GetSettings().SelectionColor == themeColor);
			const nlohmann::json camera = harness.Run("camera.get");
			CHECK(camera["fov"] == 60.0);
			CHECK(camera["yaw"] == 45.0);
			CHECK(camera["pitch"] == -30.0);
			CHECK(camera["distance"] == 10.0);
			CHECK(harness.Context.GetViewport().GetSettings().ShowGrid);
		}
		const std::filesystem::path stateFile = directory / "Game" / ".strata" / "EditorViewport.json";
		REQUIRE(FileSystem::Exists(stateFile));

		{
			ViewportHarness harness;
			harness.Run("project.open", { { "path", FileSystem::ToUTF8(directory / "Game") } });
			CHECK(harness.Run("camera.get") == savedCamera);
			CHECK_FALSE(harness.Context.GetViewport().GetSettings().ShowGrid);
			CHECK(harness.Context.GetViewport().GetSettings().RotateSnap == 30.0f);
			CHECK(harness.Context.GetViewport().GetSettings().Snap);
		}

		// A damaged state file is ignored: the project opens with the default view.
		REQUIRE(FileSystem::WriteText(stateFile, "{ \"Strata\": { \"Format\": \"EditorViewport\", \"Version\": 1 }, \"Camera\": { \"FOV\": -5 } }"));
		{
			ViewportHarness harness;
			harness.Run("project.open", { { "path", FileSystem::ToUTF8(directory / "Game") } });
			CHECK(harness.Run("camera.get")["fov"] == 60.0);
		}
		REQUIRE(FileSystem::WriteText(stateFile, "not json"));
		{
			ViewportHarness harness;
			harness.Run("project.open", { { "path", FileSystem::ToUTF8(directory / "Game") } });
			CHECK(harness.Run("camera.get")["fov"] == 60.0);
		}
		EditorViewport viewport;
		std::string error;
		CHECK_FALSE(viewport.FromJson({ { "Strata", { { "Format", "Scene" }, { "Version", 1 } } } }, &error));
		CHECK_FALSE(viewport.FromJson({ { "Strata", { { "Format", "EditorViewport" }, { "Version", 99 } } } }, &error));
		CHECK(error.find("version") != std::string::npos);
	}
}
