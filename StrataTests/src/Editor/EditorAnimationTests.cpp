#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "TestHelpers.h"

#include "Editor/AssetEdit.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"

#include <Strata/Animation/AnimationController.h>
#include <Strata/Animation/AnimationPreview.h>
#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/RenderScene.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/SceneServices.h>

#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// The feature project's arm (FeatureTest/Assets/Models/Arm.gltf) and its controller.
	constexpr const char* c_ArmModel = "Models/Arm.gltf";
	constexpr const char* c_ArmController = "F7A000000000000C";
	constexpr const char* c_WaveClip = "149C5DE55254DA1F";
	constexpr const char* c_SwingClip = "B24808E5B27B99CD";
	constexpr const char* c_ArmSkeleton = "03B1B955BE45291A";
	constexpr const char* c_ArmRig = "F7E0000000000060";

	// An editor with a copy of the feature project open and its scene loaded.
	struct AnimationHarness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;

		AnimationHarness()
		{
			const std::filesystem::path projectFile = CopyFeatureProject(CreateTemporaryDirectory("EditorAnimation") / "Project");
			Run("project.open", { { "path", FileSystem::ToUTF8(projectFile) } });
			Run("scene.open", { { "scene", "Scenes/Feature.stscene" } });
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return RunCommandToCompletion(Context, Commands, name, parameters);
		}

		EditorCommandResult Fail(std::string_view name, const nlohmann::json& parameters)
		{
			EditorCommandResult result = CompleteCommand(Context, Commands.Execute(Context, name, parameters));
			CHECK_FALSE(result.Success);
			return result;
		}
	};

	nlohmann::json LocomotionStates()
	{
		return nlohmann::json::array({
			{ { "Name", "Idle" }, { "Clip", c_WaveClip }, { "Speed", 1.0 }, { "Loop", true } },
			{ { "Name", "Run" }, { "Clip", c_SwingClip }, { "Speed", 1.5 }, { "Loop", true }, { "Events", { { { "Name", "Step" }, { "Time", 0.5 } } } } } });
	}

}

TEST_SUITE("Editor.Animation")
{
	TEST_CASE("animation.createController makes a controller asset that animators can use, refusing invalid content")
	{
		AnimationHarness harness;
		const nlohmann::json created = harness.Run("animation.createController", { { "path", "Animations/Hero.stanimator" },
			{ "parameters", { { { "Name", "Speed" }, { "Type", "Float" }, { "Default", 0.0 } } } }, { "states", LocomotionStates() },
			{ "transitions", { { { "From", "Idle" }, { "To", "Run" }, { "Conditions", { { { "Parameter", "Speed" }, { "Mode", "Greater" }, { "Threshold", 0.5 } } } },
				{ "Duration", 0.2 } } } } });
		const std::optional<UUID> handle = UUIDFromJson(created["asset"]);
		REQUIRE(handle);
		CHECK(created["type"] == "AnimationController");
		CHECK(created["controller"]["DefaultState"] == "Idle");
		CHECK(created["controller"]["States"].size() == 2);
		CHECK(created["clips"][1]["duration"].get<double>() == doctest::Approx(1.0));
		CHECK(std::filesystem::exists(harness.Context.GetAssetManager()->GetAbsolutePath(*handle)));
		CHECK(harness.Context.GetAssetManager()->GetMetadata(*handle)->Type == AssetType::AnimationController);

		// An animator takes it like any asset.
		harness.Run("component.set", { { "entity", c_ArmRig }, { "component", "Animator" }, { "values", { { "Controller", UUIDToJson(*handle) } } } });

		// Invalid content changes nothing.
		const size_t assets = harness.Context.GetAssetManager()->GetAllMetadata(AssetType::AnimationController).size();
		CHECK(harness.Fail("animation.createController", { { "path", "Animations/Bad.stanimator" }, { "states", nlohmann::json::array() } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("animation.createController", { { "path", "Animations/Bad.stanimator" },
			{ "states", { { { "Name", "A" }, { "Clip", c_ArmSkeleton } } } } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("animation.createController", { { "path", "Animations/Bad.stanimator" }, { "states", LocomotionStates() },
			{ "transitions", { { { "From", "Idle" }, { "To", "Swim" } } } } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("animation.createController", { { "path", "Animations/Bad.stanimator" }, { "states", LocomotionStates() },
			{ "unknown", 1 } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Context.GetAssetManager()->GetAllMetadata(AssetType::AnimationController).size() == assets);
	}

	TEST_CASE("animation.setController replaces sections in one undo step that undo restores")
	{
		AnimationHarness harness;
		const nlohmann::json before = harness.Run("animation.info", { { "asset", c_ArmController } });
		const size_t history = harness.Context.GetUndoStack().GetHistory().size();

		nlohmann::json states = before["controller"]["States"];
		states[0]["Speed"] = 2.5;
		const nlohmann::json changed = harness.Run("animation.setController", { { "controller", c_ArmController }, { "states", states } });
		CHECK(changed["controller"]["States"][0]["Speed"].get<double>() == doctest::Approx(2.5));
		CHECK(changed["controller"]["Transitions"] == before["controller"]["Transitions"]);
		CHECK(harness.Context.GetUndoStack().GetHistory().size() == history + 1);
		// The loaded controller is the new one at once.
		Ref<AnimationController> loaded = AssetManager::GetAsset<AnimationController>(*UUIDFromJson(nlohmann::json(c_ArmController)));
		REQUIRE(loaded);
		CHECK(loaded->GetStates()[0].Speed == 2.5f);
		CHECK_FALSE(harness.Context.IsSceneModified());

		harness.Run("edit.undo");
		CHECK(harness.Run("animation.info", { { "asset", c_ArmController } })["controller"] == before["controller"]);

		// Invalid sections are refused without a step.
		nlohmann::json invalid = before["controller"]["States"];
		invalid[1]["Name"] = invalid[0]["Name"];
		CHECK(harness.Fail("animation.setController", { { "controller", c_ArmController }, { "states", invalid } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("animation.setController", { { "controller", c_WaveClip } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Context.GetUndoStack().GetHistory().size() == history + 1);
	}

	TEST_CASE("animation.info describes models, skeletons, clips, controllers and playing animators")
	{
		AnimationHarness harness;
		const nlohmann::json model = harness.Run("animation.info", { { "asset", c_ArmModel } });
		CHECK(model["type"] == "Model");
		REQUIRE(model["skeletons"].size() == 1);
		CHECK(model["skeletons"][0]["jointCount"] == 4);
		CHECK(model["skeletons"][0]["rootMotionJoint"] == "Shoulder");
		REQUIRE(model["clips"].size() == 2);
		std::vector<std::string> names;
		for (const nlohmann::json& clip : model["clips"])
			names.push_back(clip["name"].get<std::string>());
		std::sort(names.begin(), names.end());
		CHECK(names == std::vector<std::string> { "Swing", "Wave" });

		const nlohmann::json skeleton = harness.Run("animation.info", { { "asset", c_ArmSkeleton } });
		REQUIRE(skeleton["joints"].size() == 4);
		CHECK(skeleton["joints"][0]["parent"].is_null());
		CHECK(skeleton["joints"][2]["parent"] == skeleton["joints"][1]["name"]);

		const nlohmann::json clip = harness.Run("animation.info", { { "asset", c_SwingClip } });
		CHECK(clip["duration"].get<double>() == doctest::Approx(1.0));
		CHECK(clip["channels"].size() == 3);

		const nlohmann::json controller = harness.Run("animation.info", { { "asset", c_ArmController } });
		CHECK(controller["controller"]["DefaultState"] == "Wave");
		CHECK(controller["clips"][0]["name"] == "Wave");
		CHECK(harness.Fail("animation.info", { { "asset", "Materials/Feature.stmat" } }).ErrorKind == EditorCommandError::InvalidParameters);

		// An animator: its assets, and while playing what it plays.
		const nlohmann::json editing = harness.Run("animation.info", { { "entity", c_ArmRig } });
		CHECK(editing["playing"] == false);
		CHECK(editing["controller"] == c_ArmController);
		LoadAllAssets(*harness.Context.GetAssetManager());
		harness.Run("play.start");
		harness.Run("editor.wait", { { "frames", 3 } });
		const nlohmann::json playing = harness.Run("animation.info", { { "entity", c_ArmRig } });
		CHECK(playing["playing"] == true);
		CHECK(playing["parameters"]["Raised"] == false);
		CHECK(playing["parameters"].contains("Speed"));
		harness.Run("play.stop");
	}

	TEST_CASE("animation.preview poses an animator of the edited scene without changing it")
	{
		AnimationHarness harness;
		LoadAllAssets(*harness.Context.GetAssetManager());
		const Ref<Scene> scene = harness.Context.GetEditScene();
		const Entity rig = scene->GetEntityByUUID(*UUIDFromJson(nlohmann::json(c_ArmRig)));
		REQUIRE(rig);
		CHECK(scene->GetService<ISkinningPoses>() == nullptr);

		const nlohmann::json posed = harness.Run("animation.preview", { { "entity", c_ArmRig }, { "state", "Wave" }, { "time", 0.5 } });
		CHECK(posed["posed"] == true);
		const ISkinningPoses* poses = scene->GetService<ISkinningPoses>();
		REQUIRE(poses);
		SkeletonPose pose;
		REQUIRE(poses->GetPose(rig.GetHandle(), pose));
		REQUIRE(pose.Joints.size() == 4);
		// Half a second into the wave the elbow is bent 90 degrees: the wrist is beside it.
		const glm::vec3 wrist = glm::vec3(pose.Joints[3][3]);
		CHECK(wrist.x == doctest::Approx(-0.5f).epsilon(1e-3));
		CHECK(wrist.y == doctest::Approx(0.5f).epsilon(1e-3));
		// An attachment is drawn at its joint, and what is under it with it (the render scene's proxies); the scene keeps
		// their transforms.
		const Entity armTip = scene->FindEntityByName("Arm Tip");
		const Entity tipMarker = scene->FindEntityByName("Tip Marker");
		REQUIRE(armTip);
		REQUIRE(tipMarker);
		RenderScene& renderScene = RenderScene::Get(*scene);
		renderScene.Sync(nullptr);
		const uint32_t markerProxy = renderScene.FindProxy(tipMarker.GetHandle());
		REQUIRE(markerProxy != RenderScene::c_InvalidIndex);
		const glm::mat4 drawnAtWrist = scene->GetWorldTransform(rig) * pose.Joints[3] * glm::inverse(scene->GetWorldTransform(armTip)) * scene->GetWorldTransform(tipMarker);
		const glm::mat4 drawn = renderScene.GetWorldTransform(markerProxy);
		for (int column = 0; column < 4; column++)
		{
			for (int row = 0; row < 3; row++)
				CHECK(drawn[column][row] == doctest::Approx(drawnAtWrist[column][row]).epsilon(1e-4));
		}
		CHECK(armTip.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f, 1.0f, 0.0f));
		std::string validation;
		CHECK_MESSAGE(renderScene.Validate(&validation), validation);
		CHECK_FALSE(harness.Context.IsSceneModified());
		CHECK(harness.Context.GetUndoStack().GetHistory().empty());

		// A clip, then failures: an unknown state (the request's fault, refused before the preview changes, naming the states
		// there are), an entity without an animator, a negative time.
		harness.Run("animation.preview", { { "entity", c_ArmRig }, { "clip", c_SwingClip }, { "time", 1.0 } });
		REQUIRE(poses->GetPose(rig.GetHandle(), pose));
		const uint64_t clipPose = pose.Version;
		const EditorCommandResult unknownState = harness.Fail("animation.preview", { { "entity", c_ArmRig }, { "state", "Swim" } });
		CHECK(unknownState.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(unknownState.Error.find("no state 'Swim' (its states: ") != std::string::npos);
		CHECK(unknownState.Error.find("Wave") != std::string::npos);
		REQUIRE(poses->GetPose(rig.GetHandle(), pose));
		CHECK(pose.Version == clipPose);
		// With the controller not loaded, the preview finds out once it is, and refuses the request the same way.
		harness.Context.GetAssetManager()->UnloadAsset(*UUIDFromJson(nlohmann::json(c_ArmController)));
		const EditorCommandResult unknownLater = harness.Fail("animation.preview", { { "entity", c_ArmRig }, { "state", "Fly" } });
		CHECK(unknownLater.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(unknownLater.Error.find("no state 'Fly' (its states: ") != std::string::npos);
		const std::string ground = harness.Run("entity.find", { { "name", "Ground" } })["entities"][0].get<std::string>();
		CHECK(harness.Fail("animation.preview", { { "entity", ground } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("animation.preview", { { "entity", c_ArmRig }, { "time", -1.0 } }).ErrorKind == EditorCommandError::InvalidParameters);

		// Playing poses the running copy, not previews; stopping a preview takes its pose away.
		harness.Run("play.start");
		CHECK_FALSE(harness.Fail("animation.preview", { { "entity", c_ArmRig } }).Success);
		harness.Run("play.stop");
		harness.Run("animation.preview", { { "entity", c_ArmRig }, { "state", "Wave" }, { "time", 0.25 } });
		CHECK(poses->GetPose(rig.GetHandle(), pose));
		harness.Run("animation.preview", { { "stop", true } });
		CHECK_FALSE(poses->GetPose(rig.GetHandle(), pose));
		renderScene.Sync(nullptr);
		CHECK(glm::vec3(renderScene.GetWorldTransform(markerProxy)[3]) == glm::vec3(scene->GetWorldTransform(tipMarker)[3]));
		CHECK_MESSAGE(renderScene.Validate(&validation), validation);

		// Another scene starts without previews.
		harness.Run("scene.new");
		CHECK(harness.Context.FindAnimationPreview() == nullptr);
	}
}
