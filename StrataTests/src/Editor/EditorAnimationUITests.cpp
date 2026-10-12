#include <doctest/doctest.h>

#include "Editor/HarnessEditor.h"
#include "Editor/ImGuiHarness.h"
#include "EditorLayer.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "Panels/ContentBrowserPanel.h"
#include "TestHelpers.h"
#include "UI/ItemProbe.h"

#include <Strata/Animation/AnimationController.h>
#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>

#include <spdlog/fmt/fmt.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <optional>
#include <string>
#include <string_view>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr AssetHandle c_ArmController = UUID(0xF7A000000000000Cull);
	constexpr AssetHandle c_ArmSkeleton = UUID(0x03B1B955BE45291Aull);
	constexpr AssetHandle c_SwingClip = UUID(0xB24808E5B27B99CDull);

	Ref<AnimationController> LoadedController()
	{
		Ref<AnimationController> controller = AssetManager::GetAsset<AnimationController>(c_ArmController);
		REQUIRE(controller);
		return controller;
	}

	// Commits the text of the field that has the keyboard focus.
	void PressEnter(HarnessEditor& editor)
	{
		editor.Harness.SetKey(ImGuiKey_Enter, true);
		editor.Frames(1);
		editor.Harness.SetKey(ImGuiKey_Enter, false);
		editor.Frames(1);
	}

	// Whether the Asset tab shows the controller's tables. A tab brought forward is drawn hidden in its first frames (ImGui
	// submits its items, so their probes exist, but clicks and keys cannot reach them yet), and the controller may already be
	// loaded then: the project's start scene opened with everything it uses.
	bool ShowsControllerTables(HarnessEditor& editor)
	{
		const ImGuiWindow* window = editor.FindPanelWindow(EditorPanels::c_AssetInspector);
		return window && !window->Hidden && (!window->DockIsActive || window->DockTabIsVisible)
			&& UI::ItemProbe::Find("AssetInspector.Controller.AddParameter").has_value();
	}

	// Reveals an asset in the Content Browser (a sub-asset inside its expanded model) and drags its tile onto an item of
	// another panel, as a person would.
	void DragFromBrowser(HarnessEditor& editor, ContentBrowserPanel& browser, AssetHandle asset, std::string_view target)
	{
		REQUIRE(browser.RevealAsset(editor.Context(), asset));
		editor.Frames(3);
		const std::optional<ContentBrowserPanel::ItemRect> tile = browser.FindItemRect(asset);
		REQUIRE_MESSAGE(tile.has_value(), "The tile of ", asset.ToString(), " was not drawn");
		const std::optional<UI::ItemProbe::Item> item = UI::ItemProbe::Find(target);
		REQUIRE_MESSAGE(item.has_value(), "No item ", target);
		const ImVec2 from((tile->Min.x + tile->Max.x) * 0.5f, (tile->Min.y + tile->Max.y) * 0.5f);
		const ImVec2 to = item->GetCenter();
		editor.Harness.MoveMouse(from);
		editor.Frames(1);
		editor.Harness.SetMouseButton(ImGuiMouseButton_Left, true);
		editor.Frames(1);
		for (int step = 1; step <= 4; step++)
		{
			const float t = static_cast<float>(step) / 4.0f;
			editor.Harness.MoveMouse(ImVec2(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t));
			editor.Frames(1);
		}
		editor.Frames(1);
		editor.Harness.SetMouseButton(ImGuiMouseButton_Left, false);
		editor.Frames(2);
	}

	// Turns the mouse wheel's horizontal axis to the right over an item of a table, once per frame.
	void ScrollRight(HarnessEditor& editor, const UI::ItemProbe::Item& inTable, int frames)
	{
		for (int frame = 0; frame < frames; frame++)
		{
			editor.Harness.MoveMouse(inTable.GetCenter());
			ImGui::GetIO().AddMouseWheelEvent(-1.0f, 0.0f);
			editor.Frames(1);
		}
	}

}

TEST_SUITE("Editor.UI.AssetInspector")
{
	TEST_CASE("The Asset tab shows a skeleton's joints and a clip's channels")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorAnimationAssetViews"));
		REQUIRE(editor.Context().SetInspectedAsset(c_ArmSkeleton));
		REQUIRE(editor.FramesUntil([]() { return UI::ItemProbe::Find("AssetInspector.Skeleton.Summary").has_value(); }));
		CHECK(UI::ItemProbe::Find("AssetInspector.Skeleton.Summary")->Text == "4");
		CHECK(UI::ItemProbe::Find("AssetInspector.Header")->Text.starts_with("Skeleton"));

		REQUIRE(editor.Context().SetInspectedAsset(c_SwingClip));
		REQUIRE(editor.FramesUntil([]() { return UI::ItemProbe::Find("AssetInspector.Clip.Summary").has_value(); }));
		CHECK(UI::ItemProbe::Find("AssetInspector.Clip.Summary")->Text == "3");
		CHECK(UI::ItemProbe::Find("AssetInspector.Header")->Text.starts_with("Animation Clip"));
	}

	TEST_CASE("The Asset tab edits a controller's tables, one undo step per edit, and refuses what would break it")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorAnimationControllerEdit"));
		REQUIRE(editor.Context().SetInspectedAsset(c_ArmController));
		REQUIRE(editor.FramesUntil([&]() { return ShowsControllerTables(editor); }));
		CHECK(UI::ItemProbe::Find("AssetInspector.Header")->Text.starts_with("Animation Controller"));
		CHECK(UI::ItemProbe::Find("AssetInspector.Controller.State0.Name")->Text == "Wave");
		CHECK(UI::ItemProbe::Find("AssetInspector.Controller.Transition0.Conditions")->Text == "Speed > 0.5");
		const size_t history = editor.Context().GetUndoStack().GetHistory().size();

		// A parameter added: one step, which undo takes back.
		REQUIRE(editor.Click("AssetInspector.Controller.AddParameter"));
		editor.Frames(2);
		CHECK(LoadedController()->GetParameters().size() == 4);
		CHECK(editor.Context().GetUndoStack().GetHistory().size() == history + 1);
		editor.Run("edit.undo");
		editor.Frames(2);
		CHECK(LoadedController()->GetParameters().size() == 3);

		// A state renamed; transitions follow it (they refer to states by position).
		REQUIRE(editor.ReplaceText("AssetInspector.Controller.State2.Name", "Idle"));
		PressEnter(editor);
		CHECK(LoadedController()->GetStates()[2].Name == "Idle");
		CHECK(LoadedController()->FindState("Rest") == ~0u);

		// Conditions as text.
		REQUIRE(editor.ReplaceText("AssetInspector.Controller.Transition0.Conditions", "Speed > 0.75, Raised"));
		PressEnter(editor);
		const AnimationTransition& transition = LoadedController()->GetTransitions()[0];
		REQUIRE(transition.Conditions.size() == 2);
		CHECK(transition.Conditions[0].Threshold == 0.75f);
		CHECK(transition.Conditions[1].Mode == AnimationConditionMode::True);
		CHECK(UI::ItemProbe::Find("AssetInspector.Controller.Transition0.Conditions")->Text == "Speed > 0.75, Raised");

		// Text that names nothing is refused, with the reason, and changes nothing.
		const size_t steps = editor.Context().GetUndoStack().GetHistory().size();
		REQUIRE(editor.ReplaceText("AssetInspector.Controller.Transition0.Conditions", "Nope > 1"));
		PressEnter(editor);
		CHECK(UI::ItemProbe::Find("AssetInspector.Controller.Error").has_value());
		CHECK(LoadedController()->GetTransitions()[0].Conditions.size() == 2);
		CHECK(editor.Context().GetUndoStack().GetHistory().size() == steps);
		// So is a duplicate state name (the controller validates it).
		REQUIRE(editor.ReplaceText("AssetInspector.Controller.State2.Name", "Wave"));
		PressEnter(editor);
		CHECK(LoadedController()->GetStates()[2].Name == "Idle");
		CHECK(editor.Context().GetUndoStack().GetHistory().size() == steps);
		CHECK_FALSE(editor.Context().IsSceneModified());
	}

	TEST_CASE("In a narrow Asset tab the controller's fields keep legible widths")
	{
		// A window as wide as a laptop's at 150% (and tall enough to show every table): the Asset tab is narrower than the fields
		// of the states and transitions together.
		HarnessEditor editor({ ImVec2(1600.0f, 1600.0f), 1.5f }, WithFeatureProject("EditorAnimationNarrowTables"));
		REQUIRE(editor.Context().SetInspectedAsset(c_ArmController));
		REQUIRE(editor.FramesUntil([&]() { return ShowsControllerTables(editor); }));
		editor.Frames(2);
		// Measured in frame heights (the default-state checkbox is a square): a state name, a number such as 0.25 and an
		// event list fit in their fields.
		const std::optional<UI::ItemProbe::Item> square = UI::ItemProbe::Find("AssetInspector.Controller.State0.Default");
		const std::optional<UI::ItemProbe::Item> name = UI::ItemProbe::Find("AssetInspector.Controller.State0.Name");
		const std::optional<UI::ItemProbe::Item> from = UI::ItemProbe::Find("AssetInspector.Controller.Transition0.From");
		REQUIRE(square.has_value());
		REQUIRE(name.has_value());
		REQUIRE(from.has_value());
		const float frameHeight = square->Max.y - square->Min.y;
		const std::optional<UI::ItemProbe::Item> speed = UI::ItemProbe::Find("AssetInspector.Controller.State0.Speed");
		const std::optional<UI::ItemProbe::Item> events = UI::ItemProbe::Find("AssetInspector.Controller.State1.Events");
		const std::optional<UI::ItemProbe::Item> fade = UI::ItemProbe::Find("AssetInspector.Controller.Transition0.Duration");
		const std::optional<UI::ItemProbe::Item> remove = UI::ItemProbe::Find("AssetInspector.Controller.Transition0.Remove");
		REQUIRE(speed.has_value());
		REQUIRE(events.has_value());
		REQUIRE(fade.has_value());
		REQUIRE(remove.has_value());
		CHECK(name->Max.x - name->Min.x >= 3.5f * frameHeight);
		CHECK(from->Max.x - from->Min.x >= 4.0f * frameHeight);
		CHECK(speed->Max.x - speed->Min.x >= 2.25f * frameHeight);
		CHECK(events->Max.x - events->Min.x >= 4.0f * frameHeight);
		CHECK(events->Text == "SwingEnd@0.9");
		CHECK(fade->Max.x - fade->Min.x >= 1.75f * frameHeight);

		// So a transition's row is wider than the tab, and the table scrolls sideways (the wheel's horizontal axis) to its end.
		CHECK(remove->Max.x - from->Min.x >= 20.0f * frameHeight);
		ScrollRight(editor, *from, 30);
		const std::optional<UI::ItemProbe::Item> scrolled = UI::ItemProbe::Find("AssetInspector.Controller.Transition0.Remove");
		REQUIRE(scrolled.has_value());
		CHECK(scrolled->Min.x < remove->Min.x - 4.0f * frameHeight);
	}

	TEST_CASE("The controller tables' checkbox columns are as wide as their headers")
	{
		// "Loop" over the states' checkboxes is wider than a checkbox at either scale; it ran into the Events header. In a
		// maximized window of a 4K display (the default layout's Asset tab) the states still fit without scrolling.
		for (const float scale : { 1.0f, 1.5f })
		{
			CAPTURE(scale);
			HarnessEditor editor({ ImVec2(3840.0f, 2054.0f), scale }, WithFeatureProject("EditorAnimationHeaders"));
			REQUIRE(editor.Context().SetInspectedAsset(c_ArmController));
			REQUIRE(editor.FramesUntil([&]() { return ShowsControllerTables(editor); }));
			editor.Frames(2);
			// The states' table: the one whose fifth column is Loop.
			const ImGuiTable* states = nullptr;
			ImGuiContext& context = *GImGui;
			for (int index = 0; index < context.Tables.GetMapSize(); index++)
			{
				ImGuiTable* table = context.Tables.TryGetMapData(index);
				if (table && table->ColumnsCount == 7 && std::string_view(ImGui::TableGetColumnName(table, 4)) == "Loop")
					states = table;
			}
			REQUIRE(states);
			float header = 0.0f;
			editor.Harness.Frame([&header]() { header = ImGui::CalcTextSize("Loop").x; });
			CHECK(states->Columns[4].WidthGiven >= header);
			CHECK((states->Flags & ImGuiTableFlags_ScrollX) == 0);
		}
	}

	TEST_CASE("An edit that ends in the middle of the tables leaves the rest of the frame drawn from intact data")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorAnimationControllerCommit"));
		REQUIRE(editor.Context().SetInspectedAsset(c_ArmController));
		REQUIRE(editor.FramesUntil([&]() { return ShowsControllerTables(editor); }));

		// Typing keeps the edit in progress (the tables draw it) until Enter commits it from the first row of the states.
		REQUIRE(editor.ReplaceText("AssetInspector.Controller.State0.Name", "Greet"));
		CHECK(LoadedController()->GetStates()[0].Name == "Wave");
		editor.Harness.SetKey(ImGuiKey_Enter, true);
		bool committed = false;
		for (int frame = 0; frame < 4 && !committed; frame++)
		{
			editor.Frames(1);
			if (frame == 0)
				editor.Harness.SetKey(ImGuiKey_Enter, false);
			committed = LoadedController()->GetStates()[0].Name == "Greet";
			if (!committed)
				continue;
			// The frame that committed it drew the later states and the transitions too, as they were.
			const std::optional<UI::ItemProbe::Item> events = UI::ItemProbe::Find("AssetInspector.Controller.State1.Events");
			REQUIRE(events.has_value());
			CHECK(events->Text == "SwingEnd@0.9");
			const std::optional<UI::ItemProbe::Item> conditions = UI::ItemProbe::Find("AssetInspector.Controller.Transition0.Conditions");
			REQUIRE(conditions.has_value());
			CHECK(conditions->Text == "Speed > 0.5");
		}
		CHECK(committed);
		editor.Frames(1);
		CHECK(UI::ItemProbe::Find("AssetInspector.Controller.State0.Name")->Text == "Greet");
	}
}

TEST_SUITE("Editor.UI.Inspector")
{
	TEST_CASE("While playing, the Inspector shows what an animator plays and its parameters")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorAnimationLiveState"));
		editor.Run("scene.open", { { "scene", "Scenes/Feature.stscene" } });
		// The scene opens in the background; the edited scene is read-only until it replaced it.
		REQUIRE(editor.FramesUntil([&]() { return !editor.Context().IsSceneLoading(); }));
		const std::string rig = editor.Run("entity.find", { { "name", "Arm Rig" } })["entities"][0].get<std::string>();
		editor.Run("component.set", { { "entity", rig }, { "component", "Animator" }, { "values", { { "PlayOnStart", true } } } });
		editor.Run("selection.set", { { "entities", { rig } } });
		editor.Frames(2);
		// Editing: no live state.
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.Animator.State").has_value());

		LoadAllAssets(*editor.Context().GetAssetManager());
		editor.Run("play.start");
		REQUIRE(editor.FramesUntil([]() { return UI::ItemProbe::Find("Inspector.Animator.State").has_value()
			&& UI::ItemProbe::Find("Inspector.Animator.State")->Text == "Wave"; }));
		CHECK(UI::ItemProbe::Find("Inspector.Animator.Time.Label").has_value());
		editor.Run("play.stop");
		editor.Frames(2);
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.Animator.State").has_value());
	}

	TEST_CASE("Skeletons, clips and controllers drag from the Content Browser onto an animator's fields, one undo step each")
	{
		// Tall enough that the docked browser shows a whole row of tiles.
		HarnessEditor editor({ ImVec2(1600.0f, 1400.0f), 1.0f }, WithFeatureProject("EditorAnimationDrops"));
		editor.Run("scene.open", { { "scene", "Scenes/Feature.stscene" } });
		REQUIRE(editor.FramesUntil([&]() { return !editor.Context().IsSceneLoading(); }));
		const std::string rig = editor.Run("entity.create", { { "name", "Dropped Rig" } })["id"].get<std::string>();
		editor.Run("component.add", { { "entity", rig }, { "component", "Animator" } });
		editor.Run("selection.set", { { "entities", { rig } } });
		editor.Layer->GetPanels().Focus(EditorPanels::c_ContentBrowser);
		editor.Layer->GetPanels().Focus(EditorPanels::c_Inspector);
		editor.Frames(3);
		ContentBrowserPanel* browser = editor.Layer->GetPanels().Get<ContentBrowserPanel>(EditorPanels::c_ContentBrowser);
		REQUIRE(browser);
		const Entity entity = editor.Context().GetEditScene()->GetEntityByUUID(UUID::FromString(rig).value());
		REQUIRE(entity);

		// Each dragged onto its field: one step.
		const auto drop = [&](AssetHandle asset, const char* field)
		{
			CAPTURE(field);
			const size_t steps = editor.Context().GetUndoStack().GetHistory().size();
			DragFromBrowser(editor, *browser, asset, fmt::format("Inspector.Animator.{}", field));
			CHECK(editor.Context().GetUndoStack().GetHistory().size() == steps + 1);
		};

		const AnimatorComponent& animator = entity.GetComponent<AnimatorComponent>();
		drop(c_ArmSkeleton, "Skeleton");
		CHECK(animator.Skeleton == c_ArmSkeleton);
		drop(c_SwingClip, "Clip");
		CHECK(animator.Clip == c_SwingClip);
		drop(c_ArmController, "Controller");
		CHECK(animator.Controller == c_ArmController);

		// A field takes its own type only: a clip dropped on the skeleton changes nothing.
		const size_t steps = editor.Context().GetUndoStack().GetHistory().size();
		DragFromBrowser(editor, *browser, c_SwingClip, "Inspector.Animator.Skeleton");
		CHECK(animator.Skeleton == c_ArmSkeleton);
		CHECK(editor.Context().GetUndoStack().GetHistory().size() == steps);
	}
}
