#include <doctest/doctest.h>

#include "Editor/CanvasEdit.h"
#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "Editor/UIRectEdit.h"
#include "Panels/CanvasModeView.h"
#include "UI/ItemProbe.h"

#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/GameUI/GameUIComponents.h>
#include <Strata/GameUI/GameUIScene.h>
#include <Strata/Math/Math.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <string>
#include <string_view>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr float c_Tolerance = 1e-3f;

	void CheckSameBox(const UIBox& a, const UIBox& b)
	{
		CHECK(a.Min.x == doctest::Approx(b.Min.x).epsilon(c_Tolerance));
		CHECK(a.Min.y == doctest::Approx(b.Min.y).epsilon(c_Tolerance));
		CHECK(a.Size.x == doctest::Approx(b.Size.x).epsilon(c_Tolerance));
		CHECK(a.Size.y == doctest::Approx(b.Size.y).epsilon(c_Tolerance));
	}

	// An editor without UI and a canvas with a panel and a button in it, made by commands.
	struct UIRectScene
	{
		EditorContext Context;
		EditorCommandRegistry Commands;
		Entity Canvas;
		Entity Panel;
		Entity Button;

		UIRectScene()
		{
			const nlohmann::json canvas = Run("ui.create", { { "type", "canvas" } });
			const nlohmann::json panel = Run("ui.create", { { "type", "panel" }, { "parent", canvas["entity"] }, { "anchor", "topLeft" },
				{ "position", { 100, 50 } }, { "size", { 600, 400 } } });
			const nlohmann::json button = Run("ui.create", { { "type", "button" }, { "parent", panel["entity"] }, { "position", { 30, -20 } } });
			Canvas = Find(canvas["entity"]);
			Panel = Find(panel["entity"]);
			Button = Find(button["entity"]);
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return RunCommandToCompletion(Context, Commands, name, parameters);
		}

		Entity Find(const nlohmann::json& id)
		{
			return Context.GetActiveScene()->GetEntityByUUID(*UUIDFromJson(id));
		}

		UIBox Box(Entity entity)
		{
			const std::optional<UIRectEdit::Placement> placement = UIRectEdit::FindPlacement(*Context.GetActiveScene(), entity, GameUIScene::c_DefaultScreen);
			REQUIRE(placement.has_value());
			return placement->Box;
		}
	};

	// The editor's window with its panels (the launcher dismissed).
	struct GameUIEditor : HarnessEditor
	{
		explicit GameUIEditor(const ImGuiHarness::Specification& specification = {})
			: HarnessEditor(specification)
		{
			if (!Context().HasProject())
				Layer->DismissLauncher();
			Frames(2);
		}

		Entity Find(const nlohmann::json& id)
		{
			return Context().GetActiveScene()->GetEntityByUUID(*UUIDFromJson(id));
		}

		// Types a number into a drag field (Ctrl+click turns it into text) in place of its value and presses Enter.
		void TypeNumber(std::string_view probeKey, std::string_view text)
		{
			const std::optional<UI::ItemProbe::Item> item = UI::ItemProbe::Find(probeKey);
			REQUIRE_MESSAGE(item.has_value(), "No ", std::string(probeKey), " in the last frame");
			// The modifier in a frame of its own: ImGui applies input queued after a key change a frame later.
			Harness.SetKey(ImGuiMod_Ctrl, true);
			Frames(1);
			ClickAt(item->GetCenter());
			Harness.SetKey(ImGuiMod_Ctrl, false);
			Frames(1);
			Harness.SetKey(ImGuiMod_Ctrl, true);
			Harness.SetKey(ImGuiKey_A, true);
			Frames(1);
			Harness.SetKey(ImGuiKey_A, false);
			Harness.SetKey(ImGuiMod_Ctrl, false);
			Frames(1);
			Harness.TypeText(text);
			Frames(1);
			Harness.SetKey(ImGuiKey_Enter, true);
			Frames(1);
			Harness.SetKey(ImGuiKey_Enter, false);
			Frames(2);
		}
	};

}

TEST_SUITE("Editor.UIRectEdit")
{
	TEST_CASE("Every anchor preset keeps a rect where it is, or moves it onto its anchors when asked")
	{
		const UIBox parent { { 100.0f, 50.0f }, { 600.0f, 400.0f } };
		const UIBox box { { 230.0f, 140.0f }, { 120.0f, 40.0f } };
		UIRectComponent rect;
		rect.Size = box.Size;
		for (uint32_t index = 0; index < c_UIAnchorPresetCount; index++)
		{
			const UIAnchorPreset preset = static_cast<UIAnchorPreset>(index);
			CAPTURE(UIRectEdit::GetPresetName(preset));
			CHECK(UIRectEdit::FindPreset(UIRectEdit::GetPresetName(preset)) == preset);
			for (const bool setPivot : { false, true })
			{
				const UIRectComponent kept = UIRectEdit::ApplyPreset(rect, box, parent, preset, setPivot, false);
				CHECK(UIRectEdit::FindPreset(kept) == preset);
				// Where the rect math puts it now (per axis: the anchors' span plus Size, the pivot point at the anchors plus
				// Position).
				for (int axis = 0; axis < 2; axis++)
				{
					const float start = parent.Min[axis] + kept.AnchorMin[axis] * parent.Size[axis];
					const float end = parent.Min[axis] + kept.AnchorMax[axis] * parent.Size[axis];
					const float size = end - start + kept.Size[axis];
					const float min = start + (end - start) * kept.Pivot[axis] + kept.Position[axis] - size * kept.Pivot[axis];
					CHECK(size == doctest::Approx(box.Size[axis]));
					CHECK(min == doctest::Approx(box.Min[axis]));
				}
			}
			const UIRectComponent snapped = UIRectEdit::ApplyPreset(rect, box, parent, preset, true, true);
			CHECK(snapped.Position == glm::vec2(0.0f));
		}
		CHECK_FALSE(UIRectEdit::FindPreset("upperLeft"));
		UIRectComponent odd;
		odd.AnchorMin = { 0.2f, 0.2f };
		CHECK_FALSE(UIRectEdit::FindPreset(odd));

		// Margins along a stretched axis read back what they set.
		const UIRectComponent stretched = UIRectEdit::ApplyPreset(rect, box, parent, UIAnchorPreset::TopStretch, true, false);
		CHECK(UIRectEdit::IsStretched(stretched, 0));
		CHECK_FALSE(UIRectEdit::IsStretched(stretched, 1));
		const glm::vec2 margins = UIRectEdit::GetMargins(stretched, box, parent, 0);
		CHECK(margins == glm::vec2(130.0f, 350.0f));
		const UIRectComponent narrowed = UIRectEdit::SetMargins(stretched, parent, 0, { 20.0f, 30.0f });
		const float start = parent.Min.x + 20.0f;
		const float width = parent.Size.x - 50.0f;
		CHECK(UIRectEdit::GetMargins(narrowed, UIBox { { start, box.Min.y }, { width, box.Size.y } }, parent, 0) == glm::vec2(20.0f, 30.0f));
		CHECK(narrowed.Size.x == -50.0f);
	}

	TEST_CASE("Anchor presets and margins of selected elements are one undo step each, and margin drags merge")
	{
		UIRectScene scene;
		const UIBox before = scene.Box(scene.Button);
		const size_t position = scene.Context.GetUndoStack().GetPosition();
		const Entity entities[] = { scene.Button };
		std::string error;
		REQUIRE_MESSAGE(UIRectEdit::ApplyPresetWithUndo(scene.Context, entities, UIAnchorPreset::BottomRight, true, false, &error), error);
		CheckSameBox(scene.Box(scene.Button), before);
		CHECK(scene.Button.GetComponent<UIRectComponent>().Pivot == glm::vec2(1.0f));
		CHECK(scene.Context.GetUndoStack().GetPosition() == position + 1);

		REQUIRE(UIRectEdit::ApplyPresetWithUndo(scene.Context, entities, UIAnchorPreset::MiddleStretch, true, false, &error));
		for (const float left : { 10.0f, 20.0f, 30.0f })
			REQUIRE_MESSAGE(UIRectEdit::SetMarginWithUndo(scene.Context, entities, 0, false, left, &error), error);
		CHECK(scene.Context.GetUndoStack().GetPosition() == position + 3);
		CHECK(scene.Box(scene.Button).Min.x == doctest::Approx(scene.Box(scene.Panel).Min.x + 30.0f));
		CHECK_FALSE(UIRectEdit::SetMarginWithUndo(scene.Context, entities, 1, false, 5.0f, &error)); // Not stretched vertically
		CHECK(error.find("vertically") != std::string::npos);
		const Entity canvas[] = { scene.Canvas };
		CHECK_FALSE(UIRectEdit::ApplyPresetWithUndo(scene.Context, canvas, UIAnchorPreset::Stretch, true, false, &error)); // A root canvas

		scene.Context.Undo();
		scene.Context.Undo();
		scene.Context.Undo();
		CHECK(scene.Context.GetUndoStack().GetPosition() == position);
		CheckSameBox(scene.Box(scene.Button), before);
	}

	TEST_CASE("A click picks the topmost element that draws something, never a canvas or an empty rect")
	{
		UIRectScene scene;
		Scene& active = *scene.Context.GetActiveScene();
		Entity layout = active.CreateChildEntity(scene.Canvas, "Overlay Layout");
		UIRectComponent& cover = layout.AddComponent<UIRectComponent>(); // Covers everything, draws nothing
		cover.AnchorMin = glm::vec2(0.0f);
		cover.AnchorMax = glm::vec2(1.0f);
		// The button's label is drawn over it: a click picks it, as what is in front.
		const UIBox button = scene.Box(scene.Button);
		const Entity label = scene.Button.GetChildren().front();
		CHECK(UIRectEdit::PickElement(active, GameUIScene::c_DefaultScreen, button.GetCenter()) == label);
		CHECK(UIRectEdit::PickElement(active, GameUIScene::c_DefaultScreen, { 110.0f, 60.0f }) == scene.Panel);
		CHECK_FALSE(UIRectEdit::PickElement(active, GameUIScene::c_DefaultScreen, { 1500.0f, 900.0f }));
		scene.Button.SetActive(false);
		CHECK(UIRectEdit::PickElement(active, GameUIScene::c_DefaultScreen, button.GetCenter()) == scene.Panel);
	}

	TEST_CASE("A click picks elements of the canvases in the scene where the view's camera shows them, nearest first, behind the screen's")
	{
		UIRectScene scene;
		Scene& active = *scene.Context.GetActiveScene();
		// A 4 x 2 m sign at the origin with an image filling it, seen from 5 m: its center at the screen's (no screen UI there).
		auto addSign = [&](const std::string& name, const glm::vec3& position)
		{
			Entity sign = active.CreateEntity(name);
			sign.GetComponent<TransformComponent>().Translation = position;
			sign.AddComponent<UIRectComponent>();
			UICanvasComponent& canvas = sign.AddComponent<UICanvasComponent>();
			canvas.RenderMode = UICanvasRenderMode::WorldSpace;
			canvas.ReferenceResolution = { 400.0f, 200.0f };
			canvas.WorldUnitsPerUnit = 0.01f;
			Entity image = active.CreateChildEntity(sign, name + " Image");
			UIRectComponent& rect = image.AddComponent<UIRectComponent>();
			rect.AnchorMin = glm::vec2(0.0f);
			rect.AnchorMax = glm::vec2(1.0f);
			image.AddComponent<UIImageComponent>();
			return std::pair<Entity, Entity>(sign, image);
		};
		auto [sign, image] = addSign("Sign", glm::vec3(0.0f));
		const UIScreen screen = GameUIScene::c_DefaultScreen;
		const glm::mat4 viewProjection = Math::PerspectiveReverseZ(glm::radians(60.0f), screen.Size.x / screen.Size.y, 0.1f, 100.0f) *
			glm::lookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::vec2 center = screen.Size * 0.5f;
		CHECK(UIRectEdit::PickElement(active, screen, center, true, &viewProjection) == image);
		CHECK(UIRectEdit::PickElement(active, screen, center, false, &viewProjection) == image); // Game UI hidden: still in the scene
		CHECK_FALSE(UIRectEdit::PickElement(active, screen, center)); // Without the view's camera
		CHECK_FALSE(UIRectEdit::PickElement(active, screen, { 960.0f, 1000.0f }, true, &viewProjection)); // Below the sign

		// A nearer sign in front of it is picked first.
		auto [nearSign, nearImage] = addSign("Near Sign", { 0.0f, 0.0f, 2.0f });
		CHECK(UIRectEdit::PickElement(active, screen, center, true, &viewProjection) == nearImage);
		nearSign.SetActive(false);

		// A cube in front of the sign hides it there: the click is the cube's (the viewport's ID-buffer pick).
		Entity cube = active.CreateEntity("Cube");
		cube.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 2.5f };
		cube.AddComponent<MeshRendererComponent>().Mesh = BuiltinAssets::CubeMesh;
		CHECK_FALSE(UIRectEdit::PickElement(active, screen, center, true, &viewProjection));
		CHECK(UIRectEdit::PickElement(active, screen, { 640.0f, 540.0f }, true, &viewProjection) == image); // Left of the cube
		cube.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, -2.0f }; // Behind it
		cube.MarkModified<TransformComponent>();
		CHECK(UIRectEdit::PickElement(active, screen, center, true, &viewProjection) == image);
		active.DestroyEntity(cube);

		// Grown over the whole view, the sign is behind the screen's panel, and in front where the game UI is hidden.
		sign.GetComponent<UICanvasComponent>().WorldUnitsPerUnit = 0.1f;
		sign.MarkModified<UICanvasComponent>();
		CHECK(UIRectEdit::PickElement(active, screen, { 110.0f, 60.0f }, true, &viewProjection) == scene.Panel);
		CHECK(UIRectEdit::PickElement(active, screen, { 110.0f, 60.0f }, false, &viewProjection) == image);
	}
}

TEST_SUITE("Editor.UI.Hierarchy")
{
	TEST_CASE("The create menu's UI submenu makes a button under a new canvas in one undo step")
	{
		GameUIEditor editor;
		const nlohmann::json before = SceneSerializer::Serialize(*editor.Context().GetEditScene())["Scene"]["Entities"];
		REQUIRE(editor.Click("Hierarchy.Create"));
		REQUIRE(editor.Click("Create.UI"));
		REQUIRE(editor.Click("Create.UI.Button"));
		const Entity button = editor.Context().GetPrimarySelection();
		REQUIRE(button);
		CHECK(button.HasComponent<UIButtonComponent>());
		CHECK(button.GetParent().HasComponent<UICanvasComponent>());
		editor.Run("edit.undo");
		CHECK(SceneSerializer::Serialize(*editor.Context().GetEditScene())["Scene"]["Entities"] == before);
		// The world-space text preset is "3D Text" now: "Text" is the UI's.
		REQUIRE(editor.Click("Hierarchy.Create"));
		CHECK(UI::ItemProbe::Find("Create.3D Text").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Create.Text").has_value());
	}
}

TEST_SUITE("Editor.UI.Inspector")
{
	TEST_CASE("The UI Rect drawer shows an element of a canvas in the scene where it is on its canvas, in units")
	{
		GameUIEditor editor;
		const nlohmann::json canvas = editor.Run("ui.create", { { "type", "canvas" } });
		const nlohmann::json panel = editor.Run("ui.create", { { "type", "panel" }, { "parent", canvas["entity"] }, { "size", { 200, 100 } } });
		editor.Context().Select(editor.Find(panel["entity"]).GetUUID());
		editor.Frames(3);
		REQUIRE(UI::ItemProbe::Find("Inspector.UIRect.Pixels").has_value());
		CHECK(UI::ItemProbe::Find("Inspector.UIRect.Pixels")->Text.find("units") == std::string::npos);
		editor.Run("component.set", { { "entity", canvas["entity"] }, { "component", "UICanvas" }, { "values", { { "RenderMode", "WorldSpace" } } } });
		editor.Frames(3);
		const std::optional<UI::ItemProbe::Item> units = UI::ItemProbe::Find("Inspector.UIRect.Pixels");
		REQUIRE(units.has_value());
		CHECK(units->Text == "860, 490   200 x 100 units"); // Centered on the 1920 x 1080 canvas
	}

	TEST_CASE("A UI component without its UI Rect says it has no effect, and its button adds the rect back")
	{
		GameUIEditor editor;
		const nlohmann::json canvas = editor.Run("ui.create", { { "type", "canvas" } });
		const nlohmann::json button = editor.Run("ui.create", { { "type", "button" }, { "parent", canvas["entity"] } });
		const Entity element = editor.Find(button["entity"]);
		editor.Context().Select(element.GetUUID());
		editor.Frames(3);
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.UIImage.NeedsUIRect").has_value());

		// Removing a required component is allowed: every section that needs it says so.
		editor.Run("component.remove", { { "entity", button["entity"] }, { "component", "UIRect" } });
		editor.Frames(3);
		const std::optional<UI::ItemProbe::Item> notice = UI::ItemProbe::Find("Inspector.UIImage.NeedsUIRect");
		REQUIRE(notice.has_value());
		CHECK(notice->Text == "Needs a UI Rect: without it this component has no effect.");
		CHECK(UI::ItemProbe::Find("Inspector.UISelectable.NeedsUIRect").has_value());
		CHECK(UI::ItemProbe::Find("Inspector.UIButton.NeedsUIRect").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.Transform.NeedsUIRect").has_value());

		// The button adds it back, one undo step.
		REQUIRE(editor.Click("Inspector.UIImage.NeedsUIRect.Add"));
		editor.Frames(2);
		CHECK(element.HasComponent<UIRectComponent>());
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.UIImage.NeedsUIRect").has_value());
		editor.Run("edit.undo");
		editor.Frames(2);
		CHECK_FALSE(element.HasComponent<UIRectComponent>());
		CHECK(UI::ItemProbe::Find("Inspector.UIImage.NeedsUIRect").has_value());
	}

	TEST_CASE("The UI Rect drawer's anchor grid keeps the rect in place, and stretched axes show margins")
	{
		GameUIEditor editor;
		const nlohmann::json canvas = editor.Run("ui.create", { { "type", "canvas" } });
		const nlohmann::json panel = editor.Run("ui.create", { { "type", "panel" }, { "parent", canvas["entity"] }, { "position", { 40, 30 } } });
		const Entity element = editor.Find(panel["entity"]);
		editor.Context().Select(element.GetUUID());
		editor.Frames(3);
		const std::optional<UI::ItemProbe::Item> pixels = UI::ItemProbe::Find("Inspector.UIRect.Pixels");
		REQUIRE(pixels.has_value());
		const UIScreen screen = UIRectEdit::GetGameScreen(editor.Context());
		const std::optional<UIRectEdit::Placement> before = UIRectEdit::FindPlacement(*editor.Context().GetActiveScene(), element, screen);
		REQUIRE(before.has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.UIRect.Left").has_value()); // Not stretched: the generic rows place it
		CHECK(UI::ItemProbe::Find("Inspector.UIRect.Position").has_value());
		CHECK(UI::ItemProbe::Find("Inspector.UIRect.Size").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.UIRect.Height").has_value());
		CHECK(UI::ItemProbe::Find("Inspector.UIRect.AnchorButton")->Text.find("Middle Center") != std::string::npos);

		REQUIRE(editor.Click("Inspector.UIRect.AnchorButton"));
		editor.Frames(2); // The popup appears, sized and placed, in the frames after the click
		REQUIRE(editor.Click("Inspector.UIRect.Anchor.topStretch"));
		editor.Frames(2);
		const UIRectComponent& rect = element.GetComponent<UIRectComponent>();
		CHECK(UIRectEdit::FindPreset(rect) == UIAnchorPreset::TopStretch);
		const std::optional<UIRectEdit::Placement> after = UIRectEdit::FindPlacement(*editor.Context().GetActiveScene(), element, screen);
		REQUIRE(after.has_value());
		CheckSameBox(after->Box, before->Box);
		// Stretched across: margins place it horizontally, its position and height vertically, in place of the generic
		// Position and Size rows.
		CHECK(UI::ItemProbe::Find("Inspector.UIRect.Left").has_value());
		CHECK(UI::ItemProbe::Find("Inspector.UIRect.Right").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.UIRect.Top").has_value());
		CHECK(UI::ItemProbe::Find("Inspector.UIRect.PosY").has_value());
		CHECK(UI::ItemProbe::Find("Inspector.UIRect.Height").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.UIRect.PosX").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.UIRect.Width").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.UIRect.Position").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.UIRect.Size").has_value());
		CHECK(UI::ItemProbe::Find("Inspector.UIRect.Pivot").has_value()); // Other generic rows stay
		CHECK(UI::ItemProbe::Find("Inspector.UIRect.AnchorButton")->Text.find("Top Stretch") != std::string::npos);

		// The height row sets the height alone, in one undo step.
		const glm::vec2 stretchedSize = element.GetComponent<UIRectComponent>().Size;
		const size_t beforeHeight = editor.Context().GetUndoStack().GetPosition();
		editor.TypeNumber("Inspector.UIRect.Height", "120");
		CHECK(element.GetComponent<UIRectComponent>().Size.y == doctest::Approx(120.0f));
		CHECK(element.GetComponent<UIRectComponent>().Size.x == doctest::Approx(stretchedSize.x));
		CHECK(editor.Context().GetUndoStack().GetPosition() == beforeHeight + 1);
		editor.Run("edit.undo");
		CHECK(element.GetComponent<UIRectComponent>().Size.y == doctest::Approx(stretchedSize.y));

		editor.Run("edit.undo");
		CHECK(UIRectEdit::FindPreset(element.GetComponent<UIRectComponent>()) == UIAnchorPreset::MiddleCenter);
		editor.Frames(2);
		CHECK(UI::ItemProbe::Find("Inspector.UIRect.Position").has_value());

		// Outside a canvas the drawer says so.
		const std::string plain = editor.Run("entity.create", { { "name", "Loose" }, { "components", { { "UIRect", nlohmann::json::object() } } } })["id"].get<std::string>();
		editor.Context().Select(*UUIDFromJson(plain));
		editor.Frames(3);
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.UIRect.AnchorButton").has_value());
	}
}

TEST_SUITE("Editor.CanvasEdit")
{
	TEST_CASE("The canvas mode shows the selection's screen-space canvas, else the first one, never a canvas in the scene")
	{
		UIRectScene ui;
		const nlohmann::json sign = ui.Run("ui.create", { { "type", "canvas" }, { "name", "Sign" } });
		ui.Run("component.set", { { "entity", sign["entity"] }, { "component", "UICanvas" }, { "values", { { "RenderMode", "WorldSpace" } } } });
		const nlohmann::json signPanel = ui.Run("ui.create", { { "type", "panel" }, { "parent", sign["entity"] } });
		// The selection's own canvas when it is on the screen.
		ui.Context.Select(ui.Button.GetUUID());
		CHECK(CanvasEdit::FindShownCanvas(ui.Context) == ui.Canvas);
		// An element of the canvas in the scene: the screen's canvas instead.
		ui.Context.Select(ui.Find(signPanel["entity"]).GetUUID());
		CHECK(CanvasEdit::FindShownCanvas(ui.Context) == ui.Canvas);
		ui.Context.Select(ui.Find(sign["entity"]).GetUUID());
		CHECK(CanvasEdit::FindShownCanvas(ui.Context) == ui.Canvas);
		// No canvas on the screen: none.
		ui.Run("component.set", { { "entity", UUIDToJson(ui.Canvas.GetUUID()) }, { "component", "UICanvas" }, { "values", { { "RenderMode", "WorldSpace" } } } });
		CHECK_FALSE(CanvasEdit::FindShownCanvas(ui.Context));
	}

	TEST_CASE("Dragging a handle moves those sides, never past the others, and snaps to whole units")
	{
		const UIBox box { { 10.0f, 20.0f }, { 100.0f, 50.0f } };
		auto check = [](const UIBox& actual, const glm::vec2& min, const glm::vec2& size)
		{
			CHECK(actual.Min == min);
			CHECK(actual.Size == size);
		};
		check(CanvasEdit::DragBox(box, CanvasHandle::Move, { 5.5f, -3.0f }, false), { 15.5f, 17.0f }, { 100.0f, 50.0f });
		check(CanvasEdit::DragBox(box, CanvasHandle::Move, { 5.5f, -3.25f }, true), { 16.0f, 17.0f }, { 100.0f, 50.0f });
		check(CanvasEdit::DragBox(box, CanvasHandle::Right, { 20.0f, 99.0f }, false), { 10.0f, 20.0f }, { 120.0f, 50.0f });
		check(CanvasEdit::DragBox(box, CanvasHandle::Left, { 20.0f, 0.0f }, false), { 30.0f, 20.0f }, { 80.0f, 50.0f });
		check(CanvasEdit::DragBox(box, CanvasHandle::Left, { 500.0f, 0.0f }, false), { 110.0f, 20.0f }, { 0.0f, 50.0f });
		check(CanvasEdit::DragBox(box, CanvasHandle::TopLeft, { -10.0f, -10.0f }, false), { 0.0f, 10.0f }, { 110.0f, 60.0f });
		check(CanvasEdit::DragBox(box, CanvasHandle::BottomRight, { 0.4f, 0.6f }, true), { 10.0f, 20.0f }, { 100.0f, 51.0f });
		CHECK(CanvasEdit::GetHandles(box).size() == 8);

		// The rect values that put a rect there keep its anchors and pivot.
		const UIBox parent { { 0.0f, 0.0f }, { 400.0f, 300.0f } };
		UIRectComponent stretched;
		stretched.AnchorMin = { 0.0f, 0.5f };
		stretched.AnchorMax = { 1.0f, 0.5f };
		const UIRectComponent placed = CanvasEdit::PlaceRect(stretched, box, parent);
		CHECK(placed.AnchorMin == stretched.AnchorMin);
		CHECK(placed.Size == glm::vec2(-300.0f, 50.0f));
		CHECK(placed.Position == glm::vec2(-140.0f, -105.0f)); // Pivot (60, 45) from the anchors' (200, 150)
	}

	TEST_CASE("Canvas framing zooms about a point, pans within the panel, frames a box and goes back to the fit")
	{
		auto checkVec = [](const glm::vec2& actual, const glm::vec2& expected)
		{
			CHECK(actual.x == doctest::Approx(expected.x).epsilon(1e-4));
			CHECK(actual.y == doctest::Approx(expected.y).epsilon(1e-4));
		};
		CanvasEdit::CanvasFraming framing;
		const UIBox panel { { 10.0f, 20.0f }, { 800.0f, 600.0f } };
		const glm::vec2 pixelScale(2.0f);
		CanvasEdit::CanvasFraming::Frame frame = framing.Compute(panel, 16.0f / 9.0f, pixelScale);

		// The fit: the canvas's aspect ratio, letterboxed, rendered whole at the screen's pixels.
		REQUIRE_FALSE(frame.IsEmpty());
		checkVec(frame.Fit.Min, { 10.0f, 95.0f });
		checkVec(frame.Fit.Size, { 800.0f, 450.0f });
		CHECK(frame.ScreenPixels == glm::uvec2(1600, 900));
		checkVec(frame.Display.Min, frame.Fit.Min);
		checkVec(frame.Visible.Min, frame.Fit.Min);
		checkVec(frame.Visible.Size, frame.Fit.Size);
		const SceneScreenView fitView = CanvasEdit::GetScreenView(frame);
		CHECK(fitView.Zoom == 1.0f);
		checkVec(fitView.Offset, { 0.0f, 0.0f });
		checkVec(fitView.ScreenSize, { 1600.0f, 900.0f });

		// Zooming keeps the canvas point under the pointer where it is; the visible part is what lies in the panel.
		const glm::vec2 point(210.0f, 145.0f);
		const glm::vec2 under = frame.ToScreenPixels(point);
		framing.ZoomAbout(frame, point, 2.0f);
		frame = framing.Compute(panel, 16.0f / 9.0f, pixelScale);
		CHECK(frame.Zoom == 2.0f);
		checkVec(frame.Display.Size, { 1600.0f, 900.0f });
		checkVec(frame.ToScreenPixels(point), under);
		checkVec(frame.ToPanel(under), point);
		checkVec(frame.Visible.Min, glm::max(frame.Display.Min, panel.Min));
		CHECK(frame.Visible.GetMax().x <= panel.GetMax().x);
		CHECK(frame.Visible.GetMax().y <= panel.GetMax().y);
		const SceneScreenView zoomed = CanvasEdit::GetScreenView(frame);
		CHECK(zoomed.Zoom == 2.0f);
		checkVec(zoomed.Offset, (frame.Display.Min - frame.Visible.Min) * pixelScale);

		// Panning moves the canvas; however far, a quarter of the panel keeps showing it.
		const glm::vec2 before = frame.Display.Min;
		framing.Pan(frame, { 30.0f, -10.0f });
		frame = framing.Compute(panel, 16.0f / 9.0f, pixelScale);
		checkVec(frame.Display.Min, before + glm::vec2(30.0f, -10.0f));
		framing.Pan(frame, { 100000.0f, -100000.0f });
		frame = framing.Compute(panel, 16.0f / 9.0f, pixelScale);
		CHECK(frame.Display.Min.x == doctest::Approx(panel.GetMax().x - panel.Size.x * CanvasEdit::CanvasFraming::c_KeepVisible));
		CHECK(frame.Display.GetMax().y == doctest::Approx(panel.Min.y + panel.Size.y * CanvasEdit::CanvasFraming::c_KeepVisible));
		CHECK_FALSE(frame.IsEmpty());

		// The zoom stays within its limits.
		framing.ZoomAbout(frame, point, 1e6f);
		CHECK(framing.GetZoom() == CanvasEdit::CanvasFraming::c_MaxZoom);
		framing.ZoomAbout(frame, point, 1e-6f);
		CHECK(framing.GetZoom() == CanvasEdit::CanvasFraming::c_MinZoom);

		// Framing a tenth of the canvas fills 80% of the panel along its tighter axis, centered.
		framing.FrameBox(frame, UIBox { { 0.6f, 0.2f }, { 0.1f, 0.1f } });
		frame = framing.Compute(panel, 16.0f / 9.0f, pixelScale);
		CHECK(framing.GetZoom() == doctest::Approx(8.0f));
		const glm::vec2 boxMin = frame.Display.Min + glm::vec2(0.6f, 0.2f) * frame.Display.Size;
		const glm::vec2 boxSize = glm::vec2(0.1f) * frame.Display.Size;
		checkVec(boxMin + boxSize * 0.5f, panel.GetCenter());
		CHECK(boxSize.x == doctest::Approx(panel.Size.x * CanvasEdit::CanvasFraming::c_FrameFill));

		framing.Reset();
		CHECK(framing.IsFit());
		frame = framing.Compute(panel, 16.0f / 9.0f, pixelScale);
		checkVec(frame.Visible.Min, frame.Fit.Min);

		// A panel too small to show anything frames nothing.
		CHECK(framing.Compute(UIBox { { 0.0f, 0.0f }, { 0.0f, 300.0f } }, 1.0f, pixelScale).IsEmpty());
	}

	TEST_CASE("A cropped camera shows the visible part of the display, magnified, and its symmetric frustum holds it")
	{
		const UIBox display { { 0.0f, 0.0f }, { 1000.0f, 500.0f } };
		for (const bool orthographic : { false, true })
		{
			CAPTURE(orthographic);
			SceneCamera camera;
			camera.View = glm::lookAt(glm::vec3(1.0f, 2.0f, 8.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			camera.Orthographic = orthographic;
			camera.VerticalFOV = orthographic ? 6.0f : glm::radians(60.0f);
			camera.Projection = orthographic ? Math::OrthographicReverseZ(-6.0f, 6.0f, -3.0f, 3.0f, 0.1f, 100.0f)
				: Math::PerspectiveReverseZ(camera.VerticalFOV, 2.0f, 0.1f, 100.0f);
			for (const UIBox& visible : { UIBox { { 250.0f, 0.0f }, { 250.0f, 250.0f } }, UIBox { { 375.0f, 187.5f }, { 250.0f, 125.0f } } })
			{
				CAPTURE(visible.Min);
				const SceneCamera cropped = CanvasEdit::CropCamera(camera, display, visible);
				// A world point lands at the same place of the panel through both.
				for (const glm::vec3 world : { glm::vec3(0.0f), glm::vec3(-1.0f, 1.5f, 0.5f), glm::vec3(0.7f, -0.3f, 2.0f) })
				{
					const glm::vec4 full = camera.Projection * camera.View * glm::vec4(world, 1.0f);
					const glm::vec4 part = cropped.Projection * cropped.View * glm::vec4(world, 1.0f);
					const glm::vec2 fullNDC = glm::vec2(full) / full.w;
					const glm::vec2 partNDC = glm::vec2(part) / part.w;
					const glm::vec2 fullPanel = display.Min + glm::vec2(fullNDC.x * 0.5f + 0.5f, 0.5f - fullNDC.y * 0.5f) * display.Size;
					const glm::vec2 partPanel = visible.Min + glm::vec2(partNDC.x * 0.5f + 0.5f, 0.5f - partNDC.y * 0.5f) * visible.Size;
					CHECK(partPanel.x == doctest::Approx(fullPanel.x).epsilon(1e-4));
					CHECK(partPanel.y == doctest::Approx(fullPanel.y).epsilon(1e-4));
					CHECK(part.z / part.w == doctest::Approx(full.z / full.w).epsilon(1e-5)); // Depth unchanged
				}
			}
			// The center quarter of a 4x zoom: a symmetric frustum a quarter as high. An off-center corner keeps the full
			// height (it reaches the view's edge).
			const SceneCamera center = CanvasEdit::CropCamera(camera, display, UIBox { { 375.0f, 187.5f }, { 250.0f, 125.0f } });
			const SceneCamera corner = CanvasEdit::CropCamera(camera, display, UIBox { { 750.0f, 0.0f }, { 250.0f, 125.0f } });
			if (orthographic)
			{
				CHECK(center.VerticalFOV == doctest::Approx(camera.VerticalFOV * 0.25f));
				CHECK(corner.VerticalFOV == doctest::Approx(camera.VerticalFOV));
			}
			else
			{
				CHECK(std::tan(center.VerticalFOV * 0.5f) == doctest::Approx(std::tan(camera.VerticalFOV * 0.5f) * 0.25f));
				CHECK(corner.VerticalFOV == doctest::Approx(camera.VerticalFOV));
			}
		}
	}
}

TEST_SUITE("Editor.UI.CanvasMode")
{
	TEST_CASE("Canvas mode shows handles; dragging one resizes by the canvas units dragged in one undo step; a click selects")
	{
		for (const float scale : { 1.0f, 1.5f })
		{
			CAPTURE(scale);
			ImGuiHarness::Specification specification;
			specification.ContentScale = scale;
			GameUIEditor editor(specification);
			const nlohmann::json canvas = editor.Run("ui.create", { { "type", "canvas" } });
			const nlohmann::json panel = editor.Run("ui.create", { { "type", "panel" }, { "parent", canvas["entity"] } });
			const nlohmann::json button = editor.Run("ui.create", { { "type", "button" }, { "parent", panel["entity"] } });
			const Entity element = editor.Find(panel["entity"]);
			editor.Context().Select(element.GetUUID());
			editor.Run("viewport.setSettings", { { "canvasMode", true } });
			CHECK(editor.Run("viewport.getSettings")["canvasMode"] == true);
			editor.Frames(3);

			const std::optional<UI::ItemProbe::Item> selection = UI::ItemProbe::Find("Canvas.Selection");
			const std::optional<UI::ItemProbe::Item> right = UI::ItemProbe::Find("Canvas.Handle.Right");
			REQUIRE(selection.has_value());
			REQUIRE(right.has_value());
			CHECK(UI::ItemProbe::Find("Canvas.Handle.TopLeft").has_value());
			// The panel is 400 units wide: canvas units per ImGui unit on screen.
			const float unitsPerScreen = 400.0f / (selection->Max.x - selection->Min.x);
			const size_t position = editor.Context().GetUndoStack().GetPosition();

			const ImVec2 start = right->GetCenter();
			const float dragged = 40.0f;
			editor.Harness.MoveMouse(start);
			editor.Frames(1);
			editor.Harness.SetMouseButton(ImGuiMouseButton_Left, true);
			editor.Frames(1);
			editor.Harness.MoveMouse(ImVec2(start.x + dragged * 0.5f, start.y));
			editor.Frames(1);
			editor.Harness.MoveMouse(ImVec2(start.x + dragged, start.y));
			editor.Frames(1);
			editor.Harness.SetMouseButton(ImGuiMouseButton_Left, false);
			editor.Frames(2);
			const UIRectComponent& rect = element.GetComponent<UIRectComponent>();
			CHECK(rect.Size.x == doctest::Approx(400.0f + dragged * unitsPerScreen).epsilon(0.01));
			CHECK(rect.Size.y == doctest::Approx(300.0f));
			CHECK(editor.Context().GetUndoStack().GetPosition() == position + 1);
			editor.Run("edit.undo");
			CHECK(element.GetComponent<UIRectComponent>().Size.x == doctest::Approx(400.0f));
			editor.Frames(2);

			// A click on the panel's center (where the button and its label are) selects what is on top: the label.
			const std::optional<UI::ItemProbe::Item> moved = UI::ItemProbe::Find("Canvas.Selection");
			REQUIRE(moved.has_value());
			editor.ClickAt(moved->GetCenter());
			editor.Frames(1);
			const Entity picked = editor.Context().GetPrimarySelection();
			REQUIRE(picked);
			CHECK(picked == editor.Find(button["parts"]["label"]));
			CHECK(editor.Context().GetUndoStack().GetPosition() == position); // A click moves nothing
		}
	}

	TEST_CASE("Canvas mode zooms about the pointer, pans with the middle button, frames the selection with F and fits with Home")
	{
		for (const float scale : { 1.0f, 1.5f })
		{
			CAPTURE(scale);
			ImGuiHarness::Specification specification;
			specification.ContentScale = scale;
			GameUIEditor editor(specification);
			const nlohmann::json canvas = editor.Run("ui.create", { { "type", "canvas" } });
			const nlohmann::json panel = editor.Run("ui.create", { { "type", "panel" }, { "parent", canvas["entity"] } });
			const Entity element = editor.Find(panel["entity"]);
			editor.Context().Select(element.GetUUID());
			editor.Run("viewport.setSettings", { { "canvasMode", true } });
			editor.Frames(3);
			const glm::uvec2 screen = editor.Context().GetViewport().GetSize();
			const std::optional<UI::ItemProbe::Item> fit = UI::ItemProbe::Find("Canvas.Selection");
			const std::optional<UI::ItemProbe::Item> area = UI::ItemProbe::Find("Canvas.Panel");
			REQUIRE(fit.has_value());
			REQUIRE(area.has_value());
			// The canvas lies below the viewport's chips, so they cover none of it.
			const std::optional<UI::ItemProbe::Item> chip = UI::ItemProbe::Find("Viewport.CanvasMode");
			REQUIRE(chip.has_value());
			CHECK(area->Min.y >= chip->Max.y);
			const size_t position = editor.Context().GetUndoStack().GetPosition();
			auto width = [](const UI::ItemProbe::Item& item) { return item.Max.x - item.Min.x; };

			// Two notches of the wheel over the panel's corner: the canvas grows about the pointer.
			const ImVec2 pointer(fit->Min.x + width(*fit) * 0.25f, fit->Min.y + (fit->Max.y - fit->Min.y) * 0.25f);
			editor.Harness.MoveMouse(pointer);
			editor.Frames(1);
			editor.Harness.ScrollMouse(2.0f);
			editor.Frames(2);
			const std::optional<UI::ItemProbe::Item> zoomed = UI::ItemProbe::Find("Canvas.Selection");
			REQUIRE(zoomed.has_value());
			const float factor = CanvasModeView::c_WheelZoomStep * CanvasModeView::c_WheelZoomStep;
			CHECK(width(*zoomed) == doctest::Approx(width(*fit) * factor).epsilon(0.01));
			CHECK(pointer.x - zoomed->Min.x == doctest::Approx((pointer.x - fit->Min.x) * factor).epsilon(0.01));
			CHECK(pointer.y - zoomed->Min.y == doctest::Approx((pointer.y - fit->Min.y) * factor).epsilon(0.01));
			CHECK(editor.Context().GetViewport().GetSize() == screen); // The game's screen stays the fit
			CHECK(editor.Context().GetActiveScene() != nullptr);

			// A handle drag while zoomed resizes by the canvas units dragged.
			const std::optional<UI::ItemProbe::Item> right = UI::ItemProbe::Find("Canvas.Handle.Right");
			REQUIRE(right.has_value());
			const float unitsPerScreen = 400.0f / width(*zoomed);
			const ImVec2 start = right->GetCenter();
			editor.Harness.MoveMouse(start);
			editor.Frames(1);
			editor.Harness.SetMouseButton(ImGuiMouseButton_Left, true);
			editor.Frames(1);
			editor.Harness.MoveMouse(ImVec2(start.x + 30.0f, start.y));
			editor.Frames(1);
			editor.Harness.SetMouseButton(ImGuiMouseButton_Left, false);
			editor.Frames(2);
			CHECK(element.GetComponent<UIRectComponent>().Size.x == doctest::Approx(400.0f + 30.0f * unitsPerScreen).epsilon(0.01));
			CHECK(editor.Context().GetUndoStack().GetPosition() == position + 1);
			editor.Run("edit.undo");
			editor.Frames(2);

			// A middle drag pans by what it dragged.
			const std::optional<UI::ItemProbe::Item> before = UI::ItemProbe::Find("Canvas.Selection");
			REQUIRE(before.has_value());
			editor.Harness.MoveMouse(pointer);
			editor.Frames(1);
			editor.Harness.SetMouseButton(ImGuiMouseButton_Middle, true);
			editor.Frames(1);
			editor.Harness.MoveMouse(ImVec2(pointer.x + 60.0f, pointer.y + 30.0f));
			editor.Frames(1);
			editor.Harness.SetMouseButton(ImGuiMouseButton_Middle, false);
			editor.Frames(2);
			const std::optional<UI::ItemProbe::Item> panned = UI::ItemProbe::Find("Canvas.Selection");
			REQUIRE(panned.has_value());
			CHECK(panned->Min.x == doctest::Approx(before->Min.x + 60.0f).epsilon(0.01));
			CHECK(panned->Min.y == doctest::Approx(before->Min.y + 30.0f).epsilon(0.01));

			// F frames the selection: centered, filling most of the panel along its tighter axis.
			editor.Harness.SetKey(ImGuiKey_F, true);
			editor.Frames(1);
			editor.Harness.SetKey(ImGuiKey_F, false);
			editor.Frames(2);
			const std::optional<UI::ItemProbe::Item> framed = UI::ItemProbe::Find("Canvas.Selection");
			REQUIRE(framed.has_value());
			CHECK(framed->GetCenter().x == doctest::Approx(area->GetCenter().x).epsilon(0.01));
			CHECK(framed->GetCenter().y == doctest::Approx(area->GetCenter().y).epsilon(0.01));
			const float fill = std::max(width(*framed) / width(*area), (framed->Max.y - framed->Min.y) / (area->Max.y - area->Min.y));
			CHECK(fill == doctest::Approx(CanvasEdit::CanvasFraming::c_FrameFill).epsilon(0.01));

			// Home shows the whole canvas again.
			editor.Harness.SetKey(ImGuiKey_Home, true);
			editor.Frames(1);
			editor.Harness.SetKey(ImGuiKey_Home, false);
			editor.Frames(2);
			const std::optional<UI::ItemProbe::Item> home = UI::ItemProbe::Find("Canvas.Selection");
			REQUIRE(home.has_value());
			CHECK(home->Min.x == doctest::Approx(fit->Min.x));
			CHECK(home->Min.y == doctest::Approx(fit->Min.y));
			CHECK(width(*home) == doctest::Approx(width(*fit)));
			CHECK(editor.Context().GetUndoStack().GetPosition() == position); // Looking changes nothing
		}
	}
}
