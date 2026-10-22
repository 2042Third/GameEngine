#include "Renderer/SceneRendererTestUtils.h"

#include "Animation/SkinnedMeshTestUtils.h"
#include "Animation/TestSkinningPoses.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Renderer/RenderSceneGPU.h"

#include <cstdint>
#include <utility>
#include <vector>

// Motion (SceneRenderOptions::MotionVectors): each renderer keeps its own copy of the render scene's instances and palettes as
// its latest frame drew them (the motion history), copying at the end of a frame only what the scene's GPU copy wrote since.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = 64;

	// A row of cubes and a posed skinned arm, seen from above.
	struct MotionScene
	{
		Scene Contents;
		TestSkinningPoses Poses;
		std::vector<Entity> Cubes;
		Entity Arm;
		SceneCamera Camera;

		MotionScene(SceneTestAssets& assets, uint32_t cubes)
		{
			AddNeutralPostProcess(Contents);
			AddSun(Contents, glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f)), 3.0f);
			for (uint32_t index = 0; index < cubes; index++)
				Cubes.push_back(AddMesh(Contents, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(static_cast<float>(index % 8) * 1.5f - 5.0f, 0.0f,
					-static_cast<float>(index / 8) * 1.5f), "Cube"));
			const AssetHandle tube = assets.Add(CreateSkinnedTube({ .Joints = 3, .RingsPerJoint = 4, .Sides = 12, .JointLength = 1.0f, .Radius = 0.25f }), "Tube");
			Arm = AddMesh(Contents, tube, UUID::Null(), glm::vec3(0.0f, 0.5f, 2.0f), "Arm");
			Arm.AddComponent<SkinComponent>().Animator = Arm.GetComponent<IDComponent>().ID;
			Poses.Poses[Arm.GetHandle()] = BentChain(3, 1);
			REQUIRE(Contents.RegisterService<ISkinningPoses>(Poses));
			Camera = LookAt(glm::vec3(0.0f, 14.0f, 6.0f), glm::vec3(0.0f, 0.0f, -3.0f));
		}

		~MotionScene()
		{
			Contents.UnregisterService<ISkinningPoses>(Poses);
		}

		MotionScene(const MotionScene&) = delete;
		MotionScene& operator=(const MotionScene&) = delete;

		void Move(Entity entity, const glm::vec3& offset)
		{
			entity.GetComponent<TransformComponent>().Translation += offset;
			entity.MarkModified<TransformComponent>();
		}
	};

	void LoadAssets(SceneTestAssets& assets)
	{
		for (const AssetHandle builtin : { BuiltinAssets::CubeMesh, BuiltinAssets::DefaultMaterial })
			REQUIRE(assets.Manager->LoadAssetSync(builtin));
	}

	SceneRenderer CreateRenderer(const char* name = "Motion")
	{
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		specification.DebugName = name;
		return SceneRenderer(specification);
	}

	SceneRenderOptions MotionOptions()
	{
		SceneRenderOptions options;
		options.MotionVectors = true;
		return options;
	}

	// The renderer's motion history holds the render scene's instances and joint matrices as its GPU copy holds them now.
	void CheckHistory(GPUContext& gpu, Scene& scene, SceneRenderer& renderer)
	{
		gpu.GetDevice().WaitForIdle();
		RenderScene& renderScene = RenderScene::Get(scene);
		const RenderSceneGPU& mirror = Renderer::GetRenderSceneGPUs().Get(renderScene);
		REQUIRE(renderer.GetPreviousInstanceBuffer());
		REQUIRE(renderer.GetPreviousPaletteBuffer());
		const uint64_t instanceBytes = renderScene.GetInstances().size() * sizeof(RenderData::InstanceData);
		REQUIRE(instanceBytes > 0);
		CHECK(ReadBufferRange(gpu, renderer.GetPreviousInstanceBuffer(), 0, instanceBytes) == ReadBufferRange(gpu, mirror.GetInstanceBuffer(), 0, instanceBytes));
		const uint64_t paletteBytes = renderScene.GetPalette().size() * sizeof(RenderData::AffineTransform);
		if (paletteBytes > 0)
			CHECK(ReadBufferRange(gpu, renderer.GetPreviousPaletteBuffer(), 0, paletteBytes) == ReadBufferRange(gpu, mirror.GetPaletteBuffer(), 0, paletteBytes));
	}

}

TEST_SUITE("GPU.SceneRenderer.MotionVectors")
{
	TEST_CASE("The motion history holds what the renderer's latest frame drew and copies only what was written since")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		MotionScene scene(assets, 24);
		SceneRenderer renderer = CreateRenderer();
		renderer.SetViewportSize(c_Size, c_Size);

		// The first frame has nothing to compare with and copies everything.
		REQUIRE(renderer.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		CHECK_FALSE(renderer.GetStats().MotionHistory);
		CHECK(renderer.GetStats().MotionFullCopy);
		CheckHistory(gpu, scene.Contents, renderer);

		// A still scene copies nothing.
		REQUIRE(renderer.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		CHECK(renderer.GetStats().MotionHistory);
		CHECK_FALSE(renderer.GetStats().MotionFullCopy);
		CHECK(renderer.GetStats().MotionCopiedInstances == 0);
		CHECK(renderer.GetStats().MotionCopiedJoints == 0);

		// A moved cube copies its instance, a new pose its joints.
		scene.Move(scene.Cubes[7], glm::vec3(0.0f, 1.0f, 0.0f));
		REQUIRE(renderer.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		CHECK(renderer.GetStats().MotionHistory);
		CHECK_FALSE(renderer.GetStats().MotionFullCopy);
		CHECK(renderer.GetStats().MotionCopiedInstances == 1);
		CHECK(renderer.GetStats().MotionCopiedJoints == 0);
		CheckHistory(gpu, scene.Contents, renderer);
		scene.Poses.Poses[scene.Arm.GetHandle()] = BentChain(3, 2);
		scene.Poses.Version++;
		REQUIRE(renderer.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		CHECK(renderer.GetStats().MotionCopiedJoints == 3);
		CheckHistory(gpu, scene.Contents, renderer);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A renderer that never asks keeps no history; a frame without motion, a resize or another scene starts it again")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		MotionScene scene(assets, 8);
		SceneRenderer renderer = CreateRenderer();
		renderer.SetViewportSize(c_Size, c_Size);
		REQUIRE(renderer.Render(scene.Contents, scene.Camera));
		REQUIRE(renderer.Render(scene.Contents, scene.Camera));
		CHECK(renderer.GetPreviousInstanceBuffer() == nullptr);
		CHECK(renderer.GetPreviousPaletteBuffer() == nullptr);
		CHECK_FALSE(renderer.GetStats().MotionHistory);

		REQUIRE(renderer.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		CHECK_FALSE(renderer.GetStats().MotionHistory);
		REQUIRE(renderer.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		CHECK(renderer.GetStats().MotionHistory);
		// A frame that does not ask leaves the history behind.
		REQUIRE(renderer.Render(scene.Contents, scene.Camera));
		REQUIRE(renderer.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		CHECK_FALSE(renderer.GetStats().MotionHistory);
		CHECK(renderer.GetStats().MotionFullCopy);
		CheckHistory(gpu, scene.Contents, renderer);

		// The history goes with the render targets.
		renderer.SetViewportSize(c_Size * 2, c_Size);
		CHECK(renderer.GetPreviousInstanceBuffer() == nullptr);
		REQUIRE(renderer.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		CHECK_FALSE(renderer.GetStats().MotionHistory);
		CheckHistory(gpu, scene.Contents, renderer);

		// Another scene's slots mean something else; a copy of the scene (a play start) has the same ones.
		MotionScene other(assets, 3);
		REQUIRE(renderer.Render(other.Contents, other.Camera, nullptr, MotionOptions()));
		CHECK_FALSE(renderer.GetStats().MotionHistory);
		CheckHistory(gpu, other.Contents, renderer);
		Ref<Scene> source = CreateRef<Scene>("Source");
		AddMesh(*source, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f), "Cube");
		REQUIRE(renderer.Render(*source, scene.Camera, nullptr, MotionOptions()));
		Ref<Scene> copy = Scene::Copy(source);
		REQUIRE(renderer.Render(*copy, scene.Camera, nullptr, MotionOptions()));
		CHECK(renderer.GetStats().MotionHistory);
		CheckHistory(gpu, *copy, renderer);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A renderer further behind than the scene's write log copies its whole history")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		MotionScene scene(assets, 40);
		SceneRenderer behind = CreateRenderer("Behind");
		SceneRenderer current = CreateRenderer("Current");
		behind.SetViewportSize(c_Size, c_Size);
		current.SetViewportSize(c_Size, c_Size);
		REQUIRE(behind.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		const RenderSceneGPU& mirror = Renderer::GetRenderSceneGPUs().Get(RenderScene::Get(scene.Contents));
		const uint64_t version = mirror.GetWriteVersion();

		// The other renderer's frames write every cube again and again, until the log no longer reaches back to the version.
		std::vector<std::pair<uint32_t, uint32_t>> ranges;
		uint32_t frames = 0;
		for (; frames < 16 && mirror.GetInstanceWrites(version, ranges); frames++)
		{
			for (const Entity cube : scene.Cubes)
				scene.Move(cube, glm::vec3(0.0f, 0.1f, 0.0f));
			REQUIRE(current.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
			ranges.clear();
		}
		REQUIRE_FALSE(mirror.GetInstanceWrites(version, ranges));
		CHECK(frames > 1);
		REQUIRE(behind.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		CHECK(behind.GetStats().MotionHistory);
		CHECK(behind.GetStats().MotionFullCopy);
		CheckHistory(gpu, scene.Contents, behind);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The history grows with the scene's buffers and keeps what it held")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		MotionScene scene(assets, 8);
		SceneRenderer renderer = CreateRenderer();
		renderer.SetViewportSize(c_Size, c_Size);
		REQUIRE(renderer.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		REQUIRE(renderer.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		const uint64_t capacity = Renderer::GetRenderSceneGPUs().Get(RenderScene::Get(scene.Contents)).GetInstanceCapacity();
		const uint64_t historyBytes = renderer.GetPreviousInstanceBuffer()->getDesc().byteSize;

		// New cubes just beyond the buffers' room: the frame copies only them, so the older instances come from the grown history.
		const uint64_t added = capacity - RenderScene::Get(scene.Contents).GetInstances().size() + 1;
		for (uint32_t index = 0; index < added; index++)
			AddMesh(scene.Contents, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(static_cast<float>(index % 16), -2.0f, -static_cast<float>(index / 16)), "New");
		REQUIRE(renderer.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		CHECK(Renderer::GetRenderSceneGPUs().Get(RenderScene::Get(scene.Contents)).GetInstanceCapacity() > capacity);
		CHECK(renderer.GetPreviousInstanceBuffer()->getDesc().byteSize > historyBytes);
		CHECK(renderer.GetStats().MotionHistory);
		CHECK_FALSE(renderer.GetStats().MotionFullCopy);
		CHECK(renderer.GetStats().MotionCopiedInstances == added);
		CheckHistory(gpu, scene.Contents, renderer);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
