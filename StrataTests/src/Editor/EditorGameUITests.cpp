#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "Editor/UIRectEdit.h"
#include "UI/ItemProbe.h"

#include <Strata/GameUI/GameUIComponents.h>
#include <Strata/GameUI/GameUIScene.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>

#include <string>

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
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.UIRect.Left").has_value()); // Not stretched

		REQUIRE(editor.Click("Inspector.UIRect.AnchorButton"));
		editor.Frames(2); // The popup appears, sized and placed, in the frames after the click
		REQUIRE(editor.Click("Inspector.UIRect.Anchor.topStretch"));
		editor.Frames(2);
		const UIRectComponent& rect = element.GetComponent<UIRectComponent>();
		CHECK(UIRectEdit::FindPreset(rect) == UIAnchorPreset::TopStretch);
		const std::optional<UIRectEdit::Placement> after = UIRectEdit::FindPlacement(*editor.Context().GetActiveScene(), element, screen);
		REQUIRE(after.has_value());
		CheckSameBox(after->Box, before->Box);
		CHECK(UI::ItemProbe::Find("Inspector.UIRect.Left").has_value());
		CHECK(UI::ItemProbe::Find("Inspector.UIRect.Right").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.UIRect.Top").has_value());
		editor.Run("edit.undo");
		CHECK(UIRectEdit::FindPreset(element.GetComponent<UIRectComponent>()) == UIAnchorPreset::MiddleCenter);

		// Outside a canvas the drawer says so.
		const std::string plain = editor.Run("entity.create", { { "name", "Loose" }, { "components", { { "UIRect", nlohmann::json::object() } } } })["id"].get<std::string>();
		editor.Context().Select(*UUIDFromJson(plain));
		editor.Frames(3);
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.UIRect.AnchorButton").has_value());
	}
}
