#include <doctest/doctest.h>

#include "Animation/AnimationTestUtils.h"
#include "Animation/SkinnedMeshTestUtils.h"
#include "Animation/TestSkinningPoses.h"
#include "Asset/AssetTestUtils.h"
#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Animation/AnimationComponents.h"
#include "Strata/Animation/AnimationSystem.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/SceneRenderer.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

// The animation system's frame (Perf: Release and Dist) with the job system as applications start it, and the renderer's
// cost of skinned characters (PerfGPU). Statistics: medians of 60 frames (a frame of a thousand animators takes about a
// millisecond, so frames are timed one by one).

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Joints = 60;
	constexpr uint64_t c_SkeletonHandle = 0xB001;
	constexpr uint64_t c_SwayHandle = 0xB101;
	constexpr uint64_t c_NodHandle = 0xB102;
	constexpr uint64_t c_ControllerHandle = 0xB201;

	// A chain of 60 joints, 0.1 apart.
	Ref<Skeleton> CreateLongSkeleton()
	{
		std::vector<std::string> names;
		std::vector<JointIndex> parents;
		std::vector<JointPose> rest(c_Joints);
		for (uint32_t joint = 0; joint < c_Joints; joint++)
		{
			names.push_back("Joint" + std::to_string(joint));
			parents.push_back(joint == 0 ? c_NoJoint : static_cast<JointIndex>(joint - 1));
			if (joint > 0)
				rest[joint].Translation = glm::vec3(0.0f, 0.1f, 0.0f);
		}
		std::string error;
		Ref<Skeleton> skeleton = Skeleton::Create(std::move(names), std::move(parents), std::move(rest), c_NoJoint, &error);
		REQUIRE_MESSAGE(skeleton, error);
		return skeleton;
	}

	// A second of every joint turning back and forth about `axis` (linear keys at 30 per second, each joint a little later
	// than its parent), and the root travelling along +Z.
	Ref<AnimationClip> CreateWholeBodyClip(const glm::vec3& axis, std::string name)
	{
		std::vector<float> times;
		for (int key = 0; key <= 30; key++)
			times.push_back(static_cast<float>(key) / 30.0f);
		std::vector<AnimationChannelData> channels;
		for (uint32_t joint = 0; joint < c_Joints; joint++)
		{
			std::vector<float> rotations;
			for (const float time : times)
			{
				const float angle = glm::radians(20.0f) * std::sin(glm::two_pi<float>() * time + 0.1f * static_cast<float>(joint));
				const glm::quat rotation = glm::angleAxis(angle, axis);
				rotations.insert(rotations.end(), { rotation.x, rotation.y, rotation.z, rotation.w });
			}
			channels.push_back(MakeChannel("Joint" + std::to_string(joint), AnimationPath::Rotation, AnimationInterpolation::Linear, times, rotations));
		}
		channels.push_back(MakeChannel("Joint0", AnimationPath::Translation, AnimationInterpolation::Linear, { 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f }));
		std::string error;
		Ref<AnimationClip> clip = AnimationClip::Create(std::move(name), std::move(channels), &error);
		REQUIRE_MESSAGE(clip, error);
		return clip;
	}

	// Two states that cross-fade into each other at every half loop for half a second: every animator always blends two
	// clips.
	Ref<AnimationController> CreateCrossFadeController()
	{
		AnimationControllerData data;
		AnimationState sway;
		sway.Name = "Sway";
		sway.Clip = UUID(c_SwayHandle);
		AnimationState nod;
		nod.Name = "Nod";
		nod.Clip = UUID(c_NodHandle);
		data.States = { sway, nod };
		AnimationTransition toNod;
		toNod.From = 0;
		toNod.To = 1;
		toNod.ExitTime = 0.5f;
		toNod.Duration = 0.5f;
		AnimationTransition toSway = toNod;
		toSway.From = 1;
		toSway.To = 0;
		data.Transitions = { toNod, toSway };
		return CreateController(std::move(data));
	}

	// The assets of the animators, loaded, in the active asset manager while it exists.
	class PerfAnimationAssets
	{
	public:
		PerfAnimationAssets()
		{
			AssetManager::SetActive(m_Manager);
			m_Manager->AddStored(c_SkeletonHandle, AssetType::Skeleton, CreateLongSkeleton()->Serialize(), "Perf/Skeleton");
			m_Manager->AddStored(c_SwayHandle, AssetType::AnimationClip, CreateWholeBodyClip(glm::vec3(0.0f, 0.0f, 1.0f), "Sway")->Serialize(), "Perf/Sway");
			m_Manager->AddStored(c_NodHandle, AssetType::AnimationClip, CreateWholeBodyClip(glm::vec3(1.0f, 0.0f, 0.0f), "Nod")->Serialize(), "Perf/Nod");
			m_Manager->AddStored(c_ControllerHandle, AssetType::AnimationController, StoreController(*CreateCrossFadeController()), "Perf/CrossFade.stanimator");
			for (const uint64_t handle : { c_SkeletonHandle, c_SwayHandle, c_NodHandle, c_ControllerHandle })
				REQUIRE(m_Manager->LoadAssetSync(UUID(handle)));
		}

		~PerfAnimationAssets()
		{
			AssetManager::SetActive(nullptr);
		}

		PerfAnimationAssets(const PerfAnimationAssets&) = delete;
		PerfAnimationAssets& operator=(const PerfAnimationAssets&) = delete;
	private:
		Ref<FakeAssetManager> m_Manager = CreateRef<FakeAssetManager>();
	};

	// `count` animators in a grid, playing the cross-fading controller at slightly different speeds.
	void CreateAnimators(Scene& scene, uint32_t count, AnimatorCullingMode culling)
	{
		for (uint32_t index = 0; index < count; index++)
		{
			Entity entity = scene.CreateEntity("Animator");
			entity.GetComponent<TransformComponent>().Translation = glm::vec3(static_cast<float>(index % 100) * 2.0f, 0.0f, static_cast<float>(index / 100) * 2.0f);
			AnimatorComponent& animator = entity.AddComponent<AnimatorComponent>();
			animator.Skeleton = UUID(c_SkeletonHandle);
			animator.Controller = UUID(c_ControllerHandle);
			animator.Speed = 0.9f + 0.2f * static_cast<float>(index) / static_cast<float>(count);
			animator.CullingMode = culling;
		}
	}

}

TEST_SUITE("Perf.Animation")
{
	TEST_CASE("A frame of 1,000 animators of 60 joints, each cross-fading two clips")
	{
		Perf::ScopedApplicationJobSystem jobs;
		PerfAnimationAssets assets;
		Scene scene("Animators");
		CreateAnimators(scene, 1'000, AnimatorCullingMode::AlwaysAnimate);
		scene.OnRuntimeStart();
		AnimationSystem* system = scene.GetSystem<AnimationSystem>();
		REQUIRE(system);
		// Into the steady state: the first half second plays one clip, then the cross-fades never end.
		for (int frame = 0; frame < 40; frame++)
			scene.OnUpdateRuntime(1.0f / 60.0f);
		Perf::CheckBudgetConfirmed("Perf.Animation.Evaluate1000", [&]
		{
			const Perf::Measurement frame = Perf::Measure("Perf.Animation.Evaluate1000", 10, 60, [&]
			{
				scene.OnUpdateRuntime(1.0f / 60.0f);
			});
			CHECK(system->GetStats().Evaluated == 1'000);
			return frame.MedianMs;
		});
		// Cross-fades all along.
		uint32_t blending = 0;
		for (const auto [handle, component] : scene.GetRegistry().view<AnimatorComponent>().each())
			blending += system->FindInstance(Entity(handle, &scene))->IsInTransition() ? 1 : 0;
		CHECK(blending > 900);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A frame of 10,000 WhenVisible animators no renderer sees")
	{
		Perf::ScopedApplicationJobSystem jobs;
		PerfAnimationAssets assets;
		Scene scene("Unseen");
		CreateAnimators(scene, 10'000, AnimatorCullingMode::WhenVisible);
		scene.OnRuntimeStart();
		AnimationSystem* system = scene.GetSystem<AnimationSystem>();
		REQUIRE(system);
		for (int frame = 0; frame < 40; frame++)
			scene.OnUpdateRuntime(1.0f / 60.0f);
		Perf::CheckBudgetConfirmed("Perf.Animation.CulledAnimators10k", [&]
		{
			const Perf::Measurement frame = Perf::Measure("Perf.Animation.CulledAnimators10k", 10, 60, [&]
			{
				scene.OnUpdateRuntime(1.0f / 60.0f);
			});
			// Time goes on for all of them, poses for none.
			CHECK(system->GetStats().Advanced == 10'000);
			CHECK(system->GetStats().Evaluated == 0);
			CHECK(system->GetStats().Culled == 10'000);
			return frame.MedianMs;
		});
		scene.OnRuntimeStop();
	}
}

TEST_SUITE("PerfGPU.Renderer")
{
	TEST_CASE("Render cost of 1,000 skinned characters of about 5,000 vertices, posed anew every frame")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		SceneTestAssets assets;
		// 20 joints and 161 rings of 32 vertices: 5,152 vertices.
		const AssetHandle character = assets.Add(CreateSkinnedTube({ .Joints = 20, .RingsPerJoint = 8, .Sides = 32, .JointLength = 0.1f, .Radius = 0.25f }), "Character");
		Scene scene("Characters");
		Perf::AddCameraAndLight(scene);
		TestSkinningPoses poses;
		REQUIRE(scene.RegisterService<ISkinningPoses>(poses));
		// Two poses the characters alternate between: every frame each one has a new pose.
		const std::vector<glm::mat4> poseA = BentChain(20, 6, 0.1f);
		const std::vector<glm::mat4> poseB = BentChain(20, 12, 0.1f);
		constexpr uint32_t c_Characters = 1'000;
		std::vector<entt::entity> characters;
		for (uint32_t index = 0; index < c_Characters; index++)
		{
			const glm::vec3 position((static_cast<float>(index % 40) - 20.0f) * Perf::c_GridSpacing, 0.0f, (static_cast<float>(index / 40) - 12.5f) * Perf::c_GridSpacing);
			Entity entity = AddMesh(scene, character, BuiltinAssets::DefaultMaterial, position, "Character");
			entity.AddComponent<SkinComponent>().Animator = entity.GetUUID();
			poses.Poses[entity.GetHandle()] = poseA;
			characters.push_back(entity.GetHandle());
		}
		const float half = 40.0f * Perf::c_GridSpacing * 0.5f;
		SceneCamera camera = LookAt(glm::vec3(0.0f, half * 1.2f, half * 1.6f), glm::vec3(0.0f), 1920.0f / 1080.0f);
		SceneRenderer renderer;
		renderer.SetViewportSize(1920, 1080);

		uint64_t frameIndex = 0;
		Perf::CheckBudgetConfirmed("PerfGPU.Renderer.Skinned1000", [&]
		{
			std::vector<double> samples;
			for (int frame = 0; frame < 70; frame++)
			{
				// The animation system's part: new poses (outside the measured renderer).
				frameIndex++;
				for (const entt::entity handle : characters)
					poses.Poses[handle] = (frameIndex & 1) ? poseB : poseA;
				poses.Version++;
				REQUIRE(gpu.GetDevice().BeginFrame());
				Renderer::BeginFrame();
				const auto start = std::chrono::steady_clock::now();
				REQUIRE(renderer.Render(scene, camera));
				const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
				gpu.GetDevice().EndFrame();
				if (frame >= 10)
					samples.push_back(milliseconds);
			}
			const Perf::Measurement measurement = Perf::Summarize(samples);
			const SceneRendererStats& stats = renderer.GetStats();
			const uint32_t skinnedInstances = GetDrawnCounts(renderer).SkinnedInstances;
			MESSAGE("1000 skinned characters: ", measurement.MedianMs, " ms median (", skinnedInstances, " skinned instances, ", stats.PaletteBytesUploaded,
				" palette bytes uploaded, extract ", stats.ExtractMs, " ms)");
			CHECK(skinnedInstances == c_Characters);
			CHECK(stats.PaletteBytesUploaded > 0);
			return measurement.MedianMs;
		});
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
