#include <doctest/doctest.h>

#include "Animation/AnimationTestUtils.h"
#include "Animation/SkinnedMeshTestUtils.h"
#include "Asset/AssetTestUtils.h"
#include "Scene/SceneTestUtils.h"
#include "Strata/Animation/AnimationPreview.h"
#include "Strata/Animation/AnimationSystem.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/Log.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneServices.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint64_t c_SkeletonHandle = AnimationTestAssets::Skeleton;
	constexpr uint64_t c_WaveHandle = AnimationTestAssets::Wave;
	constexpr uint64_t c_WalkHandle = AnimationTestAssets::Walk;
	constexpr uint64_t c_TurnHandle = AnimationTestAssets::Turn;
	constexpr uint64_t c_IdleHandle = AnimationTestAssets::Idle;
	constexpr uint64_t c_JumpHandle = AnimationTestAssets::Jump;
	constexpr uint64_t c_ControllerHandle = AnimationTestAssets::Controller;
	constexpr uint64_t c_MissingHandle = AnimationTestAssets::Missing;

	// A playing scene over in-memory animation assets, the active asset manager while it exists.
	class AnimationScene
	{
	public:
		explicit AnimationScene(bool preload = true)
		{
			AssetManager::SetActive(Manager);
			AddAnimationTestAssets(*Manager, preload);
		}

		~AnimationScene()
		{
			if (SceneRef->IsRunning())
				SceneRef->OnRuntimeStop();
			AssetManager::SetActive(nullptr);
		}

		AnimationScene(const AnimationScene&) = delete;
		AnimationScene& operator=(const AnimationScene&) = delete;

		Entity CreateAnimator(const std::string& name, uint64_t clip, uint64_t controller = 0)
		{
			Entity entity = SceneRef->CreateEntity(name);
			AnimatorComponent& animator = entity.AddComponent<AnimatorComponent>();
			animator.Skeleton = UUID(c_SkeletonHandle);
			animator.Clip = UUID(clip);
			animator.Controller = UUID(controller);
			return entity;
		}

		void Start()
		{
			SceneRef->OnRuntimeStart(SceneRuntimeMode::Play);
			REQUIRE(System());
		}

		void Update(float seconds)
		{
			SceneRef->OnUpdateRuntime(seconds);
		}

		AnimationSystem* System() const { return SceneRef->GetSystem<AnimationSystem>(); }

		// The model-space transform of a joint in the animator's pose.
		glm::mat4 GetJoint(Entity animator, uint32_t joint) const
		{
			const ISkinningPoses* poses = SceneRef->GetService<ISkinningPoses>();
			REQUIRE(poses);
			SkeletonPose pose;
			REQUIRE(poses->GetPose(animator.GetHandle(), pose));
			REQUIRE(joint < pose.Joints.size());
			return pose.Joints[joint];
		}

		Ref<FakeAssetManager> Manager = CreateRef<FakeAssetManager>();
		Ref<Scene> SceneRef = CreateRef<Scene>("Animation");
	};

	float ElbowDegrees(const glm::mat4& elbow)
	{
		return glm::degrees(glm::angle(glm::quat_cast(glm::mat3(elbow))));
	}

	bool SameMatrices(const glm::mat4& a, const glm::mat4& b, float tolerance)
	{
		for (int column = 0; column < 4; column++)
		{
			for (int row = 0; row < 4; row++)
			{
				if (std::abs(a[column][row] - b[column][row]) > tolerance)
					return false;
			}
		}
		return true;
	}

	size_t CountWarnings(uint64_t after, std::string_view text)
	{
		size_t count = 0;
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(after))
		{
			if (entry.Level == LogLevel::Warn && entry.Message.find(text) != std::string::npos)
				count++;
		}
		return count;
	}

}

TEST_SUITE("Animation.System")
{
	TEST_CASE("Animators play in Play mode and publish their poses through ISkinningPoses")
	{
		AnimationScene scene;
		Entity animator = scene.CreateAnimator("Arm", c_WaveHandle);
		CHECK(scene.SceneRef->GetService<ISkinningPoses>() == nullptr);
		scene.Start();
		ISkinningPoses* poses = scene.SceneRef->GetService<ISkinningPoses>();
		REQUIRE(poses);
		CHECK(scene.SceneRef->GetService<IAnimatorControl>() != nullptr);
		CHECK(scene.SceneRef->GetService<IAnimationEventSource>() != nullptr);
		SkeletonPose pose;
		CHECK_FALSE(poses->GetPose(animator.GetHandle(), pose));
		const uint64_t before = poses->GetPosesVersion();

		// A quarter into the wave the elbow is bent 45 degrees; the wrist rides on it.
		scene.Update(0.25f);
		REQUIRE(poses->GetPose(animator.GetHandle(), pose));
		CHECK(pose.Joints.size() == 3);
		CHECK(pose.Version != 0);
		CHECK(poses->GetPosesVersion() != before);
		CHECK(ElbowDegrees(pose.Joints[1]) == doctest::Approx(45.0f).epsilon(1e-3));
		CHECK(glm::vec3(pose.Joints[2][3]).x == doctest::Approx(-std::sin(glm::radians(45.0f))).epsilon(1e-4));
		const AnimationSystemStats stats = scene.System()->GetStats();
		CHECK(stats.Animators == 1);
		CHECK(stats.ActiveAnimators == 1);
		CHECK(stats.Evaluated == 1);
		CHECK(stats.AssetSets == 1);
		CheckSceneCaches(*scene.SceneRef);

		// Each new pose has a new version.
		const uint64_t version = pose.Version;
		scene.Update(0.25f);
		REQUIRE(poses->GetPose(animator.GetHandle(), pose));
		CHECK(pose.Version != version);
		CHECK(ElbowDegrees(pose.Joints[1]) == doctest::Approx(90.0f).epsilon(1e-3));

		// Stopping removes the services with the system.
		scene.SceneRef->OnRuntimeStop();
		CHECK(scene.SceneRef->GetService<ISkinningPoses>() == nullptr);
	}

	TEST_CASE("Inactive animators have no pose and start over when they are active again")
	{
		AnimationScene scene;
		Entity animator = scene.CreateAnimator("Arm", c_WaveHandle);
		Entity late = scene.CreateAnimator("Late", c_WaveHandle);
		late.GetComponent<AnimatorComponent>().PlayOnStart = false;
		scene.Start();
		scene.Update(0.25f);
		const ISkinningPoses* poses = scene.SceneRef->GetService<ISkinningPoses>();
		SkeletonPose pose;
		CHECK(poses->GetPose(animator.GetHandle(), pose));
		// Without PlayOnStart nothing plays: the rest pose.
		REQUIRE(poses->GetPose(late.GetHandle(), pose));
		CHECK(ElbowDegrees(pose.Joints[1]) == doctest::Approx(0.0f).epsilon(1e-3));
		CHECK_FALSE(scene.System()->FindInstance(late)->IsPlaying());

		animator.SetActive(false);
		const uint64_t before = poses->GetPosesVersion();
		scene.Update(0.25f);
		CHECK_FALSE(poses->GetPose(animator.GetHandle(), pose));
		CHECK(poses->GetPosesVersion() != before);
		CHECK(scene.System()->FindInstance(animator) == nullptr);

		animator.SetActive(true);
		scene.Update(0.25f);
		REQUIRE(poses->GetPose(animator.GetHandle(), pose));
		CHECK(ElbowDegrees(pose.Joints[1]) == doctest::Approx(45.0f).epsilon(1e-3));
		CHECK(scene.System()->FindInstance(animator)->GetNormalizedTime() == doctest::Approx(0.25));

		// Animators added and removed while the scene plays.
		Entity added = scene.CreateAnimator("Added", c_WaveHandle);
		scene.Update(0.5f);
		CHECK(poses->GetPose(added.GetHandle(), pose));
		CHECK(scene.System()->GetStats().Animators == 3);
		added.RemoveComponent<AnimatorComponent>();
		CHECK_FALSE(poses->GetPose(added.GetHandle(), pose));
		scene.SceneRef->DestroyEntity(late);
		scene.Update(0.1f);
		CHECK(scene.System()->GetStats().Animators == 1);
		CHECK(poses->GetPose(animator.GetHandle(), pose));
		CheckSceneCaches(*scene.SceneRef);

		// Without animators an update has nothing to do: the stats of the update before do not linger.
		CHECK(scene.System()->GetStats().Advanced == 1);
		scene.SceneRef->DestroyEntity(animator);
		scene.Update(0.1f);
		const AnimationSystemStats idle = scene.System()->GetStats();
		CHECK(idle.Animators == 0);
		CHECK(idle.ActiveAnimators == 0);
		CHECK(idle.Advanced == 0);
		CHECK(idle.Evaluated == 0);
		CHECK(scene.System()->FindInstance(animator) == nullptr);
	}

	TEST_CASE("Animators wait for their assets and report assets that do not load once")
	{
		AnimationScene scene(false);
		Entity animator = scene.CreateAnimator("Arm", c_WaveHandle, c_ControllerHandle);
		Entity broken = scene.CreateAnimator("Broken", c_WaveHandle);
		broken.GetComponent<AnimatorComponent>().Skeleton = UUID(c_MissingHandle);
		const uint64_t before = Log::GetBuffer().GetLatestSequence();
		scene.Start();
		// Commands given while the controller loads apply once it has.
		IAnimatorControl* control = scene.SceneRef->GetService<IAnimatorControl>();
		REQUIRE(control);
		CHECK(control->SetFloat(animator, "Speed", 1.0f));
		scene.Update(0.1f);
		const ISkinningPoses* poses = scene.SceneRef->GetService<ISkinningPoses>();
		SkeletonPose pose;
		CHECK_FALSE(poses->GetPose(animator.GetHandle(), pose));
		CHECK(scene.System()->GetStats().Waiting == 2);

		// The controller names the clips of its states, which load next.
		REQUIRE(scene.Manager->WaitForPendingLoads());
		scene.Update(0.1f);
		CHECK_FALSE(poses->GetPose(animator.GetHandle(), pose));
		REQUIRE(scene.Manager->WaitForPendingLoads());
		scene.Update(0.1f);
		REQUIRE(poses->GetPose(animator.GetHandle(), pose));
		CHECK_FALSE(poses->GetPose(broken.GetHandle(), pose));
		// Idle with Speed above 0.5 went on to Walk.
		CHECK(control->GetCurrentState(animator) == std::string("Walk"));
		CHECK(scene.System()->GetStats().Waiting == 1);
		for (int update = 0; update < 5; update++)
			scene.Update(0.1f);
		CHECK(CountWarnings(before, "Animator: the skeleton") == 1);
	}

	TEST_CASE("Scripts control animators through IAnimatorControl")
	{
		AnimationScene scene;
		Entity animator = scene.CreateAnimator("Arm", c_WaveHandle, c_ControllerHandle);
		animator.GetComponent<TransformComponent>().Translation = glm::vec3(3.0f, 0.0f, 0.0f);
		Entity plain = scene.SceneRef->CreateEntity("No animator");
		scene.Start();
		IAnimatorControl* control = scene.SceneRef->GetService<IAnimatorControl>();
		REQUIRE(control);
		scene.Update(0.1f);
		CHECK(control->GetCurrentState(animator) == std::string("Idle"));
		CHECK(control->GetNormalizedTime(animator) == doctest::Approx(0.05f)); // Idle's clip lasts 2 seconds

		CHECK(control->Play(animator, "Walk"));
		CHECK(control->GetCurrentState(animator) == std::string("Walk"));
		CHECK_FALSE(control->IsInTransition(animator));
		CHECK(control->SetFloat(animator, "Speed", 1.0f));
		scene.Update(0.25f);
		CHECK(control->GetNormalizedTime(animator) == doctest::Approx(0.25f));
		CHECK(control->CrossFade(animator, "Idle", 0.5f));
		CHECK(control->IsInTransition(animator));
		CHECK(control->GetCurrentState(animator) == std::string("Idle"));
		CHECK(control->SetFloat(animator, "Speed", 0.0f)); // Idle stays
		CHECK(control->SetBool(animator, "Grounded", true));
		CHECK(control->SetTrigger(animator, "Jump"));
		CHECK(control->ResetTrigger(animator, "Jump"));
		scene.Update(0.1f);
		CHECK(control->GetCurrentState(animator) == std::string("Idle"));
		CHECK(control->SetTrigger(animator, "Jump"));
		scene.Update(0.1f);
		CHECK(control->GetCurrentState(animator) == std::string("Jump"));

		// The speed is the component's, within its range.
		CHECK(control->SetSpeed(animator, 2.0f));
		CHECK(animator.GetComponent<AnimatorComponent>().Speed == 2.0f);
		CHECK_FALSE(control->SetSpeed(animator, 1000.0f));
		CHECK_FALSE(control->SetSpeed(animator, std::numeric_limits<float>::quiet_NaN()));

		// A clip played directly runs without the controller's transitions.
		CHECK(control->PlayClip(animator, UUID(c_WalkHandle)));
		CHECK(control->GetCurrentState(animator) == std::string());
		CHECK_FALSE(control->PlayClip(animator, UUID(c_ControllerHandle)));
		CHECK_FALSE(control->PlayClip(animator, UUID(c_MissingHandle)));
		scene.Update(0.125f);
		CHECK(control->GetNormalizedTime(animator) == doctest::Approx(0.25f)); // At speed 2

		// Joints in the world: the animator's transform times the pose.
		const std::optional<glm::mat4> wrist = control->GetJointTransform(animator, "Wrist");
		REQUIRE(wrist);
		CHECK(SameMatrices(*wrist, scene.SceneRef->GetWorldTransform(animator) * scene.GetJoint(animator, 2), 1e-6f));
		CHECK((*wrist)[3].x > 2.0f);
		CHECK_FALSE(control->GetJointTransform(animator, "Tail"));

		// Unknown states and parameters, and entities without an animator, fail.
		CHECK_FALSE(control->Play(animator, "Swim"));
		CHECK_FALSE(control->SetFloat(animator, "Grounded", 1.0f));
		CHECK_FALSE(control->Play(plain, "Walk"));
		CHECK_FALSE(control->SetBool(plain, "Grounded", true));
		CHECK_FALSE(control->GetCurrentState(plain));
		CHECK_FALSE(control->GetJointTransform(plain, "Wrist"));
		CHECK(control->GetNormalizedTime(plain) == 0.0f);
		CHECK_FALSE(control->IsInTransition(plain));

		// The editor reads the parameters by name.
		const std::vector<AnimatorParameterValue> parameters = scene.System()->GetParameters(animator);
		REQUIRE(parameters.size() == 3);
		CHECK(parameters[0].Name == "Speed");
		CHECK(parameters[0].Type == AnimationParameterType::Float);
		CHECK(parameters[0].Value == 0.0f);
		CHECK(parameters[1].Name == "Grounded");
		CHECK(parameters[1].Type == AnimationParameterType::Bool);
		CHECK(parameters[1].Value == 1.0f);
		CHECK(parameters[2].Type == AnimationParameterType::Trigger);
		CHECK(parameters[2].Value == 0.0f); // The jump consumed its trigger
	}

	TEST_CASE("Events reach the listeners after the poses, in order, once each")
	{
		AnimationScene scene;
		Entity first = scene.CreateAnimator("First", c_WaveHandle, c_ControllerHandle);
		Entity second = scene.CreateAnimator("Second", c_WaveHandle, c_ControllerHandle);
		scene.Start();
		IAnimatorControl* control = scene.SceneRef->GetService<IAnimatorControl>();
		IAnimationEventSource* source = scene.SceneRef->GetService<IAnimationEventSource>();
		REQUIRE(source);
		for (Entity entity : { first, second })
		{
			REQUIRE(control->Play(entity, "Walk"));
			REQUIRE(control->SetFloat(entity, "Speed", 1.0f));
		}

		struct Received
		{
			std::string Animator;
			std::string Name;
			std::string State;
			float Time;
			bool PoseReady;
		};
		std::vector<Received> received;
		const AnimationEventListenerID id = source->AddAnimationEventListener([&](const AnimationEventInfo& event)
		{
			// The poses are published before the events are dispatched.
			SkeletonPose pose;
			const bool ready = scene.SceneRef->GetService<ISkinningPoses>()->GetPose(event.Animator.GetHandle(), pose);
			received.push_back(Received { event.Animator.GetName(), std::string(event.Name), std::string(event.State), event.Time, ready });
			CHECK(event.AnimatorID == event.Animator.GetUUID());
		});
		CHECK(id != 0);
		CHECK(source->AddAnimationEventListener(nullptr) == 0);

		scene.Update(1.25f); // 0 -> 1.25: StepLeft, StepRight, then the next loop's StepLeft
		REQUIRE(received.size() == 6);
		const char* names[] = { "StepLeft", "StepRight", "StepLeft" };
		for (size_t index = 0; index < received.size(); index++)
		{
			CAPTURE(index);
			CHECK(received[index].Animator == (index < 3 ? "First" : "Second"));
			CHECK(received[index].Name == names[index % 3]);
			CHECK(received[index].State == "Walk");
			CHECK(received[index].PoseReady);
		}
		CHECK(scene.System()->GetStats().EventsDispatched == 6);

		// A listener that removes an animator does not disturb the dispatch of the others' events.
		received.clear();
		const AnimationEventListenerID remover = source->AddAnimationEventListener([&](const AnimationEventInfo& event)
		{
			if (event.Animator == first && first.HasComponent<AnimatorComponent>())
				first.RemoveComponent<AnimatorComponent>();
		});
		scene.Update(0.5f); // 1.25 -> 1.75: StepRight
		CHECK(received.size() == 2);
		CHECK(scene.System()->GetStats().Animators == 1);

		// Removed listeners are not called again.
		received.clear();
		CHECK(source->RemoveAnimationEventListener(id));
		CHECK(source->RemoveAnimationEventListener(remover));
		CHECK_FALSE(source->RemoveAnimationEventListener(id));
		scene.Update(1.0f);
		CHECK(received.empty());
		CheckSceneCaches(*scene.SceneRef);
	}

	TEST_CASE("Removing an animator keeps the order the others' events are dispatched in")
	{
		AnimationScene scene;
		std::vector<Entity> animators;
		for (const char* name : { "A", "B", "C" })
			animators.push_back(scene.CreateAnimator(name, c_WaveHandle, c_ControllerHandle));
		scene.Start();
		IAnimatorControl* control = scene.SceneRef->GetService<IAnimatorControl>();
		std::vector<std::string> order;
		scene.SceneRef->GetService<IAnimationEventSource>()->AddAnimationEventListener([&](const AnimationEventInfo& event)
		{
			// One entry per animator and update (its events arrive together).
			if (order.empty() || order.back() != event.Animator.GetName())
				order.push_back(event.Animator.GetName());
		});
		auto walk = [&](Entity entity)
		{
			REQUIRE(control->Play(entity, "Walk"));
			REQUIRE(control->SetFloat(entity, "Speed", 1.0f));
		};
		for (Entity entity : animators)
			walk(entity);
		scene.Update(0.5f);
		CHECK(order == std::vector<std::string> { "A", "B", "C" });

		// The first one goes between updates, one is added: the others keep their order, the new one comes last.
		animators[0].RemoveComponent<AnimatorComponent>();
		Entity added = scene.CreateAnimator("D", c_WaveHandle, c_ControllerHandle);
		scene.Update(0.0f); // Takes in the assets the new component was given after it was added
		walk(added);
		order.clear();
		scene.Update(0.5f);
		CHECK(order == std::vector<std::string> { "B", "C", "D" });

		// One removed while the update dispatches (by a listener) leaves at its end; the order of the rest stays.
		const AnimationEventListenerID remover = scene.SceneRef->GetService<IAnimationEventSource>()->AddAnimationEventListener([&](const AnimationEventInfo& event)
		{
			if (event.Animator == animators[1] && animators[1].HasComponent<AnimatorComponent>())
				animators[1].RemoveComponent<AnimatorComponent>();
		});
		order.clear();
		scene.Update(0.5f);
		CHECK(order == std::vector<std::string> { "B", "C", "D" });
		CHECK(scene.SceneRef->GetService<IAnimationEventSource>()->RemoveAnimationEventListener(remover));
		order.clear();
		scene.Update(0.5f);
		CHECK(order == std::vector<std::string> { "C", "D" });
		CHECK(scene.System()->GetStats().Animators == 2);
	}

	TEST_CASE("WhenVisible animators compute their pose only while a renderer saw them")
	{
		AnimationScene scene;
		Entity always = scene.CreateAnimator("Always", c_WaveHandle);
		Entity seen = scene.CreateAnimator("Seen", c_WaveHandle);
		seen.GetComponent<AnimatorComponent>().CullingMode = AnimatorCullingMode::WhenVisible;
		scene.Start();
		ISkinningPoses* poses = scene.SceneRef->GetService<ISkinningPoses>();
		// A first pose for both, so renderers have something to draw.
		scene.Update(0.25f);
		SkeletonPose pose;
		REQUIRE(poses->GetPose(seen.GetHandle(), pose));
		const uint64_t firstVersion = pose.Version;
		CHECK(scene.System()->GetStats().Evaluated == 2);

		// Unseen: time goes on, the pose stays.
		scene.Update(0.25f);
		CHECK(scene.System()->GetStats().Evaluated == 1);
		CHECK(scene.System()->GetStats().Culled == 1);
		REQUIRE(poses->GetPose(seen.GetHandle(), pose));
		CHECK(pose.Version == firstVersion);
		CHECK(scene.System()->FindInstance(seen)->GetNormalizedTime() == doctest::Approx(0.5));

		// Reported by a renderer: the next update poses it, at its current time.
		poses->ReportVisible(seen.GetHandle());
		scene.Update(0.25f);
		CHECK(scene.System()->GetStats().Evaluated == 2);
		REQUIRE(poses->GetPose(seen.GetHandle(), pose));
		CHECK(pose.Version != firstVersion);
		CHECK(ElbowDegrees(pose.Joints[1]) == doctest::Approx(45.0f).epsilon(1e-3)); // 0.75 into the wave
		scene.Update(0.25f);
		CHECK(scene.System()->GetStats().Culled == 1);
		poses->ReportVisible(always.GetHandle()); // Reports of other animators change nothing for it
		poses->ReportVisible(entt::null);
		scene.Update(0.25f);
		CHECK(scene.System()->GetStats().Culled == 1);
	}

	TEST_CASE("Attachments follow their joint through the transform contract")
	{
		AnimationScene scene;
		Entity animator = scene.CreateAnimator("Arm", c_WaveHandle);
		animator.GetComponent<TransformComponent>().Translation = glm::vec3(1.0f, 0.0f, 0.0f);
		animator.GetComponent<TransformComponent>().Rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		Entity sword = scene.SceneRef->CreateChildEntity(animator, "Sword");
		sword.AddComponent<AttachToJointComponent>() = AttachToJointComponent { animator.GetUUID(), "Wrist" };
		Entity elsewhere = scene.SceneRef->CreateEntity("Shield");
		elsewhere.AddComponent<AttachToJointComponent>() = AttachToJointComponent { animator.GetUUID(), "Elbow" };
		Entity lost = scene.SceneRef->CreateEntity("Lost");
		lost.AddComponent<AttachToJointComponent>() = AttachToJointComponent { animator.GetUUID(), "Tail" };
		lost.GetComponent<TransformComponent>().Translation = glm::vec3(5.0f);
		scene.Start();

		scene.Update(0.25f);
		CHECK(scene.System()->GetStats().Attachments == 2);
		const glm::mat4 animatorWorld = scene.SceneRef->GetWorldTransform(animator);
		CHECK(SameMatrices(scene.SceneRef->GetWorldTransform(sword), animatorWorld * scene.GetJoint(animator, 2), 1e-5f));
		CHECK(SameMatrices(scene.SceneRef->GetWorldTransform(elsewhere), animatorWorld * scene.GetJoint(animator, 1), 1e-5f));
		CHECK(lost.GetComponent<TransformComponent>().Translation == glm::vec3(5.0f));
		CheckSceneCaches(*scene.SceneRef);

		// The same pose writes nothing again.
		scene.Update(0.0f);
		CHECK(scene.System()->GetStats().Attachments == 0);
		// The animator moving moves the attachment elsewhere in the hierarchy too.
		animator.GetComponent<TransformComponent>().Translation = glm::vec3(-2.0f, 0.0f, 0.0f);
		animator.MarkModified<TransformComponent>();
		scene.Update(0.0f);
		CHECK(scene.System()->GetStats().Attachments == 1);
		CHECK(SameMatrices(scene.SceneRef->GetWorldTransform(elsewhere), scene.SceneRef->GetWorldTransform(animator) * scene.GetJoint(animator, 1), 1e-5f));
		CheckSceneCaches(*scene.SceneRef);
	}

	TEST_CASE("New assets on the component start the animator over, and a reloaded controller keeps parameter values")
	{
		AnimationScene scene;
		Entity animator = scene.CreateAnimator("Arm", c_WaveHandle, c_ControllerHandle);
		scene.Start();
		IAnimatorControl* control = scene.SceneRef->GetService<IAnimatorControl>();
		scene.Update(0.1f);
		REQUIRE(control->SetFloat(animator, "Speed", 0.25f));
		scene.Update(0.1f);

		// A controller document with one more parameter: the values of the others stay.
		AnimationControllerData data = CreateLocomotionControllerData(UUID(c_IdleHandle), UUID(c_WalkHandle), UUID(c_JumpHandle));
		data.Parameters.push_back(AnimationParameter { "Crouch", AnimationParameterType::Bool, 0.0f, true });
		scene.Manager->SetStoredData(UUID(c_ControllerHandle), StoreController(*CreateController(data)));
		scene.Manager->ReloadAsset(UUID(c_ControllerHandle));
		REQUIRE(scene.Manager->WaitForPendingLoads());
		scene.Update(0.1f);
		const std::vector<AnimatorParameterValue> parameters = scene.System()->GetParameters(animator);
		REQUIRE(parameters.size() == 4);
		CHECK(parameters[0].Value == 0.25f);
		CHECK(parameters[3].Name == "Crouch");
		CHECK(parameters[3].Value == 1.0f);

		// Without the controller the clip plays, from its start.
		animator.GetComponent<AnimatorComponent>().Controller = UUID::Null();
		scene.Update(0.25f);
		CHECK(control->GetCurrentState(animator) == std::string());
		CHECK(control->GetNormalizedTime(animator) == doctest::Approx(0.25f));
		CHECK(ElbowDegrees(scene.GetJoint(animator, 1)) == doctest::Approx(45.0f).epsilon(1e-3));
	}

	TEST_CASE("Components serialize every animation property")
	{
		AnimationScene scene;
		Entity animator = scene.CreateAnimator("Arm", c_WaveHandle, c_ControllerHandle);
		AnimatorComponent& component = animator.GetComponent<AnimatorComponent>();
		component.Speed = -0.5f;
		component.Loop = false;
		component.PlayOnStart = false;
		component.ApplyRootMotion = true;
		component.CullingMode = AnimatorCullingMode::WhenVisible;
		Entity sword = scene.SceneRef->CreateChildEntity(animator, "Sword");
		sword.AddComponent<AttachToJointComponent>() = AttachToJointComponent { animator.GetUUID(), "Wrist" };

		const Ref<Scene> copy = Scene::Copy(scene.SceneRef);
		const Entity copied = copy->GetEntityByUUID(animator.GetUUID());
		REQUIRE(copied);
		const AnimatorComponent& read = copied.GetComponent<AnimatorComponent>();
		CHECK(read.Skeleton == UUID(c_SkeletonHandle));
		CHECK(read.Controller == UUID(c_ControllerHandle));
		CHECK(read.Clip == UUID(c_WaveHandle));
		CHECK(read.Speed == -0.5f);
		CHECK_FALSE(read.Loop);
		CHECK_FALSE(read.PlayOnStart);
		CHECK(read.ApplyRootMotion);
		CHECK(read.CullingMode == AnimatorCullingMode::WhenVisible);
		const AttachToJointComponent& attachment = copy->GetEntityByUUID(sword.GetUUID()).GetComponent<AttachToJointComponent>();
		CHECK(attachment.Animator == animator.GetUUID());
		CHECK(attachment.Joint == "Wrist");
	}
}

TEST_SUITE("Animation.RootMotion")
{
	TEST_CASE("Root motion moves the entity by the clip's root travel and keeps the root in place")
	{
		AnimationScene scene;
		Entity walker = scene.CreateAnimator("Walker", c_WalkHandle);
		walker.GetComponent<AnimatorComponent>().ApplyRootMotion = true;
		// Turned a quarter about +Y, its +Z (the clip's travel) points along world +X.
		Entity turned = scene.CreateAnimator("Turned", c_WalkHandle);
		turned.GetComponent<AnimatorComponent>().ApplyRootMotion = true;
		turned.GetComponent<TransformComponent>().Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		// Under a parent scaled 2: its feet travel twice as far in the world.
		Entity parent = scene.SceneRef->CreateEntity("Giant");
		parent.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f);
		Entity giant = scene.CreateAnimator("Scaled", c_WalkHandle);
		scene.SceneRef->SetParent(giant, parent, false);
		giant.GetComponent<AnimatorComponent>().ApplyRootMotion = true;
		// Without root motion the clip's travel stays in the pose.
		Entity inPlace = scene.CreateAnimator("InPlace", c_WalkHandle);
		scene.Start();

		// Steps that do not divide the loop: the travel is exact across each loop boundary.
		const float step = 0.07f;
		int updates = 0;
		for (int loop = 1; loop <= 3; loop++)
		{
			while (static_cast<float>(updates + 1) * step <= static_cast<float>(loop) + 1e-4f)
			{
				scene.Update(step);
				updates++;
			}
			const float seconds = static_cast<float>(updates) * step;
			CAPTURE(loop);
			const float tolerance = 1e-4f * static_cast<float>(loop);
			CHECK(std::abs(walker.GetComponent<TransformComponent>().Translation.z - 2.0f * seconds) <= tolerance);
			CHECK(std::abs(walker.GetComponent<TransformComponent>().Translation.x) <= tolerance);
			CHECK(std::abs(scene.SceneRef->GetWorldTransform(turned)[3].x - 2.0f * seconds) <= tolerance);
			CHECK(std::abs(scene.SceneRef->GetWorldTransform(turned)[3].z) <= tolerance);
			CHECK(std::abs(scene.SceneRef->GetWorldTransform(giant)[3].z - 4.0f * seconds) <= 2.0f * tolerance);
			CHECK(inPlace.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
			// The root stays where the clip started in the pose of the moving ones, and travels in the other's.
			CHECK(std::abs(glm::vec3(scene.GetJoint(walker, 0)[3]).z) <= 1e-4f);
			const float inClip = seconds - std::floor(seconds + 1e-4f);
			CHECK(std::abs(glm::vec3(scene.GetJoint(inPlace, 0)[3]).z - 2.0f * inClip) <= 1e-3f);
		}
		CheckSceneCaches(*scene.SceneRef);
	}

	TEST_CASE("Root motion turns the entity by the root joint's turn about +Y")
	{
		AnimationScene scene;
		Entity turner = scene.CreateAnimator("Turner", c_TurnHandle);
		turner.GetComponent<AnimatorComponent>().ApplyRootMotion = true;
		scene.Start();
		// A quarter turn per loop: a loop and a half turns 135 degrees.
		for (int update = 0; update < 90; update++)
			scene.Update(1.0f / 60.0f);
		const glm::quat rotation = turner.GetComponent<TransformComponent>().Rotation;
		const glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, 1.0f);
		CHECK(glm::degrees(std::atan2(forward.x, forward.z)) == doctest::Approx(135.0f).epsilon(1e-4));
		CHECK(std::abs(forward.y) < 1e-5f);
		// The pose keeps the root facing as it did when the clip started.
		const glm::vec3 rootForward = glm::mat3(scene.GetJoint(turner, 0)) * glm::vec3(0.0f, 0.0f, 1.0f);
		CHECK(rootForward.z == doctest::Approx(1.0f).epsilon(1e-4));
	}

	TEST_CASE("Root motion moves and turns the entity by the same amount every update, also across loop boundaries")
	{
		// Frames of a sixtieth of a second over clips of a second: the playhead's float steps reach a loop boundary a little
		// before or after the loop count does, which must not move the entity by a loop (forward, then back) for a frame.
		AnimationScene scene;
		Entity walker = scene.CreateAnimator("Walker", c_WalkHandle);
		walker.GetComponent<AnimatorComponent>().ApplyRootMotion = true;
		Entity turner = scene.CreateAnimator("Turner", c_TurnHandle);
		turner.GetComponent<AnimatorComponent>().ApplyRootMotion = true;
		scene.Start();
		IAnimatorControl* control = scene.SceneRef->GetService<IAnimatorControl>();
		REQUIRE(control);

		const float step = 1.0f / 60.0f;
		auto heading = [](const glm::quat& rotation)
		{
			const glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, 1.0f);
			return glm::degrees(std::atan2(forward.x, forward.z));
		};
		float previousZ = walker.GetComponent<TransformComponent>().Translation.z;
		float previousHeading = heading(turner.GetComponent<TransformComponent>().Rotation);
		double previousNormalized = 0.0;
		for (int update = 1; update <= 250; update++)
		{
			scene.Update(step);
			CAPTURE(update);
			// The walk travels 2 per one-second loop, the turn 90 degrees.
			const float z = walker.GetComponent<TransformComponent>().Translation.z;
			CHECK(std::abs((z - previousZ) - 2.0f * step) <= 1e-4f);
			previousZ = z;
			const float currentHeading = heading(turner.GetComponent<TransformComponent>().Rotation);
			float turned = currentHeading - previousHeading;
			if (turned < -180.0f)
				turned += 360.0f;
			else if (turned > 180.0f)
				turned -= 360.0f;
			CHECK(std::abs(turned - 90.0f * step) <= 1e-2f);
			previousHeading = currentHeading;
			// The normalized time counts the loops the playhead made, a step at a time.
			const double normalized = scene.System()->FindInstance(walker)->GetNormalizedTime();
			CHECK(std::abs((normalized - previousNormalized) - static_cast<double>(step)) <= 1e-5);
			previousNormalized = normalized;
			CHECK(control->GetNormalizedTime(walker) == doctest::Approx(static_cast<float>(update) * step).epsilon(1e-4));
		}
		CheckSceneCaches(*scene.SceneRef);
	}
}

TEST_SUITE("Animation.StateMachine")
{
	TEST_CASE("Animators evaluate the same with any number of threads")
	{
		// One run: 200 animators with different speeds driven through their state machines; every pose and event recorded.
		auto run = []()
		{
			AnimationScene scene;
			std::vector<Entity> animators;
			for (int index = 0; index < 200; index++)
			{
				Entity entity = scene.CreateAnimator(fmt::format("Animator{}", index), c_WaveHandle, index % 3 == 0 ? 0 : c_ControllerHandle);
				AnimatorComponent& component = entity.GetComponent<AnimatorComponent>();
				component.Speed = 0.5f + 0.01f * static_cast<float>(index);
				component.ApplyRootMotion = index % 5 == 0;
				animators.push_back(entity);
			}
			scene.Start();
			IAnimatorControl* control = scene.SceneRef->GetService<IAnimatorControl>();
			std::vector<std::string> events;
			scene.SceneRef->GetService<IAnimationEventSource>()->AddAnimationEventListener([&](const AnimationEventInfo& event)
			{
				events.push_back(fmt::format("{}:{}:{}", event.Animator.GetName(), event.Name, event.Time));
			});
			std::vector<uint8_t> poses;
			for (int update = 0; update < 40; update++)
			{
				for (size_t index = 0; index < animators.size(); index++)
				{
					if ((index + update) % 7 == 0)
						control->SetFloat(animators[index], "Speed", update % 2 == 0 ? 1.0f : 0.0f);
					if ((index + update) % 11 == 0)
						control->SetTrigger(animators[index], "Jump");
				}
				scene.Update(1.0f / 30.0f);
				for (Entity entity : animators)
				{
					SkeletonPose pose;
					if (!scene.SceneRef->GetService<ISkinningPoses>()->GetPose(entity.GetHandle(), pose))
						continue;
					const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pose.Joints.data());
					poses.insert(poses.end(), bytes, bytes + pose.Joints.size_bytes());
					const glm::vec3 translation = entity.GetComponent<TransformComponent>().Translation;
					const uint8_t* moved = reinterpret_cast<const uint8_t*>(&translation);
					poses.insert(poses.end(), moved, moved + sizeof(translation));
				}
			}
			return std::make_pair(poses, events);
		};

		// Without the job system everything runs on this thread.
		const auto reference = run();
		REQUIRE_FALSE(reference.first.empty());
		REQUIRE_FALSE(reference.second.empty());
		for (const uint32_t threads : { 1u, 7u })
		{
			CAPTURE(threads);
			ScopedJobSystem jobs(threads, 1);
			const auto threaded = run();
			CHECK(threaded.first == reference.first);
			CHECK(threaded.second == reference.second);
		}
	}
}

TEST_SUITE("Animation.System")
{
	TEST_CASE("Every instance of an animated prefab plays its own animator, which its skins and attachments follow")
	{
		AnimationScene scene;
		// A character as a prefab: the animator at its root, a skinned body and a hand attached to the wrist, both naming it.
		const AssetHandle meshHandle = UUID(0x5C1B);
		const Ref<Mesh> tube = CreateSkinnedTube({ .Joints = 3, .RingsPerJoint = 2, .Sides = 6, .JointLength = 1.0f, .Radius = 0.25f });
		Ref<Prefab> prefab;
		{
			Scene source("Character");
			Entity root = source.CreateEntity("Character");
			AnimatorComponent& animator = root.AddComponent<AnimatorComponent>();
			animator.Skeleton = UUID(c_SkeletonHandle);
			animator.Clip = UUID(c_WaveHandle);
			Entity body = source.CreateChildEntity(root, "Body");
			body.AddComponent<MeshRendererComponent>().Mesh = meshHandle;
			body.AddComponent<SkinComponent>().Animator = root.GetUUID();
			Entity hand = source.CreateChildEntity(root, "Hand");
			hand.AddComponent<AttachToJointComponent>() = AttachToJointComponent { root.GetUUID(), "Wrist" };
			prefab = Prefab::CreateFromEntities(source, { root });
		}
		REQUIRE(prefab);

		// Two instances, the second at half speed and elsewhere: fresh IDs, and each one's references name its own root.
		const std::vector<Entity> first = prefab->Instantiate(*scene.SceneRef);
		const std::vector<Entity> second = prefab->Instantiate(*scene.SceneRef);
		REQUIRE(first.size() == 1);
		REQUIRE(second.size() == 1);
		Entity characters[] = { first[0], second[0] };
		CHECK(characters[0].GetUUID() != characters[1].GetUUID());
		for (const Entity character : characters)
		{
			const std::vector<Entity> children = character.GetChildren();
			REQUIRE(children.size() == 2);
			CHECK(children[0].GetComponent<SkinComponent>().Animator == character.GetUUID());
			CHECK(children[1].GetComponent<AttachToJointComponent>().Animator == character.GetUUID());
		}
		characters[1].GetComponent<AnimatorComponent>().Speed = 0.5f;
		characters[1].GetComponent<TransformComponent>().Translation = glm::vec3(4.0f, 0.0f, 0.0f);
		characters[1].MarkModified<TransformComponent>();

		scene.Start();
		scene.Update(0.25f);
		CHECK(scene.System()->GetStats().Animators == 2);
		CHECK(scene.System()->GetStats().AssetSets == 1); // Same skeleton and clip: shared
		// A quarter into the wave the first elbow is bent 45 degrees; the second is an eighth in, as the clip has it then.
		CHECK(ElbowDegrees(scene.GetJoint(characters[0], 1)) == doctest::Approx(45.0f).epsilon(1e-3));
		const Ref<Skeleton> skeleton = CreateArmSkeleton();
		const Ref<AnimationClip> wave = CreateWaveClip(1.0f);
		const AnimationClipBinding binding = wave->Bind(*skeleton);
		std::vector<JointPose> eighth = skeleton->GetRestPose();
		wave->Sample(binding, 0.125f, eighth);
		CHECK(ElbowDegrees(scene.GetJoint(characters[1], 1)) == doctest::Approx(glm::degrees(glm::angle(eighth[1].Rotation))).epsilon(1e-3));
		// Each hand is at its own animator's wrist.
		for (const Entity character : characters)
		{
			const Entity hand = character.GetChildren()[1];
			CHECK(SameMatrices(scene.SceneRef->GetWorldTransform(hand), scene.SceneRef->GetWorldTransform(character) * scene.GetJoint(character, 2), 1e-5f));
		}
		CheckSceneCaches(*scene.SceneRef);

		// The renderer poses each body by its own animator, from palettes of their own.
		RenderScene& renderScene = RenderScene::Get(*scene.SceneRef);
		renderScene.Sync(nullptr);
		for (uint32_t slot = 0; slot < renderScene.GetMeshSlotCapacity(); slot++)
		{
			if (renderScene.GetMeshHandle(slot) == meshHandle)
				renderScene.SetMeshLayout(slot, *tube);
		}
		renderScene.Sync(nullptr);
		uint32_t paletteFirsts[2] = {};
		for (size_t index = 0; index < 2; index++)
		{
			CAPTURE(index);
			const uint32_t proxy = renderScene.FindProxy(characters[index].GetChildren()[0].GetHandle());
			REQUIRE(proxy != RenderScene::c_InvalidIndex);
			CHECK((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::Skinned) != 0);
			CHECK(renderScene.GetSkinAnimator(proxy) == characters[index].GetHandle());
			REQUIRE(renderScene.GetPaletteCount(proxy) == 3);
			paletteFirsts[index] = renderScene.GetPaletteFirst(proxy);
		}
		CHECK(paletteFirsts[0] != paletteFirsts[1]);
		// The joint above the elbows: the poses differ, and so do the palettes.
		const std::span<const RenderData::AffineTransform> palette = renderScene.GetPalette();
		CHECK(std::memcmp(&palette[paletteFirsts[0] + 2], &palette[paletteFirsts[1] + 2], sizeof(RenderData::AffineTransform)) != 0);
		std::string error;
		CHECK_MESSAGE(renderScene.Validate(&error), error);

		// An instance spawned while the scene plays (as scripts spawn prefabs) starts at the next update, posing its own body.
		const std::vector<Entity> third = prefab->Instantiate(*scene.SceneRef);
		REQUIRE(third.size() == 1);
		scene.Update(0.25f);
		CHECK(scene.System()->GetStats().Animators == 3);
		CHECK(ElbowDegrees(scene.GetJoint(third[0], 1)) == doctest::Approx(45.0f).epsilon(1e-3));
		renderScene.Sync(nullptr);
		const uint32_t spawned = renderScene.FindProxy(third[0].GetChildren()[0].GetHandle());
		REQUIRE(spawned != RenderScene::c_InvalidIndex);
		CHECK((renderScene.GetProxyFlags(spawned) & RenderProxyFlags::Skinned) != 0);
		CHECK(renderScene.GetSkinAnimator(spawned) == third[0].GetHandle());
		CHECK_MESSAGE(renderScene.Validate(&error), error);
		CheckSceneCaches(*scene.SceneRef);
	}
}

TEST_SUITE("Animation.Preview")
{
	TEST_CASE("A preview request is posed once its own pose is computed, the earlier pose showing meanwhile")
	{
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		AssetManager::SetActive(manager);
		AddAnimationTestAssets(*manager, false);
		REQUIRE(manager->LoadAssetSync(UUID(c_SkeletonHandle)));
		REQUIRE(manager->LoadAssetSync(UUID(c_WaveHandle)));
		{
			Scene scene("Preview");
			Entity rig = scene.CreateEntity("Rig");
			AnimatorComponent& animator = rig.AddComponent<AnimatorComponent>();
			animator.Skeleton = UUID(c_SkeletonHandle);
			animator.Clip = UUID(c_WaveHandle);
			AnimationPreview preview(scene);
			REQUIRE(preview.IsRegistered());

			AnimationPreview::Request request;
			request.Time = 0.5f;
			REQUIRE(preview.Set(rig, request));
			CHECK(preview.GetStatus(rig) == AnimationPreview::Status::Waiting);
			preview.Update();
			REQUIRE(preview.GetStatus(rig) == AnimationPreview::Status::Posed);
			SkeletonPose pose;
			REQUIRE(preview.GetPose(rig.GetHandle(), pose));
			const uint64_t waveVersion = pose.Version;
			CHECK(ElbowDegrees(pose.Joints[1]) == doctest::Approx(90.0f).epsilon(1e-3));

			// A clip that is not loaded yet: the wave's pose still shows, but the new request is not posed.
			request.Clip = UUID(c_WalkHandle);
			request.Time = 0.25f;
			REQUIRE(preview.Set(rig, request));
			CHECK(preview.GetStatus(rig) == AnimationPreview::Status::Waiting);
			preview.Update();
			CHECK(preview.GetStatus(rig) == AnimationPreview::Status::Waiting);
			REQUIRE(preview.GetPose(rig.GetHandle(), pose));
			CHECK(pose.Version == waveVersion);

			REQUIRE(manager->WaitForPendingLoads());
			preview.Update();
			CHECK(preview.GetStatus(rig) == AnimationPreview::Status::Posed);
			REQUIRE(preview.GetPose(rig.GetHandle(), pose));
			CHECK(pose.Version != waveVersion);
			CHECK(ElbowDegrees(pose.Joints[1]) == doctest::Approx(45.0f).epsilon(1e-3));

			// The same request again is a new request too: posed again by the next update.
			REQUIRE(preview.Set(rig, request));
			CHECK(preview.GetStatus(rig) == AnimationPreview::Status::Waiting);
			preview.Update();
			CHECK(preview.GetStatus(rig) == AnimationPreview::Status::Posed);
		}
		AssetManager::SetActive(nullptr);
	}
}
