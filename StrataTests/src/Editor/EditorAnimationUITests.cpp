#include <doctest/doctest.h>

#include "Editor/HarnessEditor.h"
#include "Editor/ImGuiHarness.h"
#include "EditorLayer.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "TestHelpers.h"
#include "UI/ItemProbe.h"

#include <Strata/Animation/AnimationController.h>
#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Reflection/PropertyJson.h>

#include <imgui.h>

#include <string>

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
		REQUIRE(editor.FramesUntil([]() { return UI::ItemProbe::Find("AssetInspector.Controller.AddParameter").has_value(); }));
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
}

TEST_SUITE("Editor.UI.Inspector")
{
	TEST_CASE("While playing, the Inspector shows what an animator plays and its parameters")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorAnimationLiveState"));
		editor.Run("scene.open", { { "scene", "Scenes/Feature.stscene" } });
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
}
