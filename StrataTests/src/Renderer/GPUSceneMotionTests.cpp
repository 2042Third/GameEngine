#include "Renderer/SceneRendererTestUtils.h"

#include "Animation/SkinnedMeshTestUtils.h"
#include "Animation/TestSkinningPoses.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Renderer/RenderSceneGPU.h"

#include <glm/gtc/packing.hpp>

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Motion vectors (SceneRenderOptions::MotionVectors): each pixel's motion since the renderer's previous frame, checked against
// where the surface point the depth buffer shows was then; each renderer keeps its own copy of the render scene's instances and
// palettes as its latest frame drew them (the motion history), copying at the end of a frame only what the scene's GPU copy
// wrote since.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = 64;

	// Rows of cubes, a mirrored cone and a posed skinned arm on a ground, seen from above.
	struct MotionScene
	{
		Scene Contents;
		TestSkinningPoses Poses;
		std::vector<Entity> Cubes;
		Entity Arm;
		Entity Mirrored;
		Entity Ground;
		SceneCamera Camera;

		MotionScene(SceneTestAssets& assets, uint32_t cubes, const glm::vec3& origin = glm::vec3(0.0f))
		{
			AddNeutralPostProcess(Contents);
			AddSun(Contents, glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f)), 3.0f);
			for (uint32_t index = 0; index < cubes; index++)
				Cubes.push_back(AddMesh(Contents, BuiltinAssets::CubeMesh, UUID::Null(), origin + glm::vec3(static_cast<float>(index % 8) * 1.5f - 5.0f, 0.0f,
					-static_cast<float>(index / 8) * 1.5f), "Cube"));
			const AssetHandle tube = assets.Add(CreateSkinnedTube({ .Joints = 3, .RingsPerJoint = 4, .Sides = 12, .JointLength = 1.0f, .Radius = 0.25f }), "Tube");
			Arm = AddMesh(Contents, tube, UUID::Null(), origin + glm::vec3(0.0f, 0.5f, 2.0f), "Arm");
			Arm.AddComponent<SkinComponent>().Animator = Arm.GetComponent<IDComponent>().ID;
			Poses.Poses[Arm.GetHandle()] = BentChain(3, 1);
			REQUIRE(Contents.RegisterService<ISkinningPoses>(Poses));
			Mirrored = AddMesh(Contents, BuiltinAssets::ConeMesh, UUID::Null(), origin + glm::vec3(3.0f, 0.5f, 2.0f), "Mirrored");
			Mirrored.GetComponent<TransformComponent>().Scale = glm::vec3(-1.2f, 1.2f, 1.2f);
			Ground = AddMesh(Contents, BuiltinAssets::PlaneMesh, UUID::Null(), origin + glm::vec3(0.0f, -0.5f, 0.0f), "Ground");
			Ground.GetComponent<TransformComponent>().Scale = glm::vec3(60.0f);
			Camera = LookAt(origin + glm::vec3(0.0f, 14.0f, 6.0f), origin + glm::vec3(0.0f, 0.0f, -3.0f));
		}

		// Moves every vertex of the arm's pose by `offset` (in the arm's space, which is world space but for its position).
		void MovePose(const glm::vec3& offset)
		{
			for (glm::mat4& joint : Poses.Poses[Arm.GetHandle()])
				joint = glm::translate(glm::mat4(1.0f), offset) * joint;
			Poses.Version++;
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
		for (const AssetHandle builtin : { BuiltinAssets::CubeMesh, BuiltinAssets::ConeMesh, BuiltinAssets::PlaneMesh, BuiltinAssets::DefaultMaterial })
			REQUIRE(assets.Manager->LoadAssetSync(builtin));
	}

	SceneRenderer CreateRenderer(const char* name = "Motion", GPUDrivenMode mode = GPUDrivenMode::Off)
	{
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		specification.GPUDriven = mode;
		specification.DebugName = name;
		return SceneRenderer(specification);
	}

	const char* PathName(GPUDrivenMode mode)
	{
		return mode == GPUDrivenMode::Off ? "CPU path" : "GPU-driven path";
	}

	uint32_t GetID(Entity entity)
	{
		return static_cast<uint32_t>(entity.GetHandle()) + 1;
	}

	// What a frame wrote: its motion in pixels (the velocity target times the viewport size), depth and entity IDs.
	struct MotionFrame
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::vector<glm::vec2> Motion;
		std::vector<float> Depth;
		std::vector<uint32_t> IDs;
	};

	MotionFrame ReadMotionFrame(SceneRenderer& renderer)
	{
		ReadbackImage velocity;
		ReadbackImage depth;
		ReadbackImage ids;
		REQUIRE(renderer.GetVelocityTexture());
		REQUIRE(Renderer::ReadTexture(renderer.GetVelocityTexture(), velocity));
		REQUIRE(Renderer::ReadTexture(renderer.GetDepthTexture(), depth));
		REQUIRE(Renderer::ReadTexture(renderer.GetEntityIDTexture(), ids));
		REQUIRE(velocity.BytesPerPixel == 4);
		MotionFrame frame;
		frame.Width = velocity.Width;
		frame.Height = velocity.Height;
		const size_t pixels = static_cast<size_t>(frame.Width) * frame.Height;
		REQUIRE(depth.Pixels.size() == pixels * sizeof(float));
		REQUIRE(ids.Pixels.size() == pixels * sizeof(uint32_t));
		std::vector<uint16_t> halves(pixels * 2);
		std::memcpy(halves.data(), velocity.Pixels.data(), halves.size() * sizeof(uint16_t));
		frame.Motion.resize(pixels);
		for (size_t pixel = 0; pixel < pixels; pixel++)
			frame.Motion[pixel] = glm::vec2(glm::unpackHalf1x16(halves[pixel * 2]), glm::unpackHalf1x16(halves[pixel * 2 + 1])) * glm::vec2(frame.Width, frame.Height);
		frame.Depth.resize(pixels);
		std::memcpy(frame.Depth.data(), depth.Pixels.data(), depth.Pixels.size());
		frame.IDs.resize(pixels);
		std::memcpy(frame.IDs.data(), ids.Pixels.data(), ids.Pixels.size());
		return frame;
	}

	// Where the surface point a pixel shows moved on screen since the previous frame, in pixels: the point `current` sees at the
	// pixel's center and depth, less `displacement` (its own motion), as `previous` saw it. Computed relative to the cameras'
	// positions, as the renderer works, in double.
	glm::dvec2 ExpectedMotion(const SceneCamera& current, const SceneCamera& previous, uint32_t width, uint32_t height, uint32_t x, uint32_t y, float depth,
		const glm::dvec3& displacement)
	{
		const glm::dvec2 size(width, height);
		const glm::dvec2 uv((x + 0.5) / size.x, (y + 0.5) / size.y);
		const glm::dmat4 currentViewProjection = glm::dmat4(current.Projection) * glm::dmat4(glm::mat4(glm::mat3(current.View)));
		const glm::dvec4 relative = glm::inverse(currentViewProjection) * glm::dvec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
		const glm::dvec3 point = glm::dvec3(relative) / relative.w + glm::dvec3(current.Position) - displacement;
		const glm::dmat4 previousViewProjection = glm::dmat4(previous.Projection) * glm::dmat4(glm::mat4(glm::mat3(previous.View)));
		const glm::dvec4 clip = previousViewProjection * glm::dvec4(point - glm::dvec3(previous.Position), 1.0);
		const glm::dvec2 previousUV(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
		return (uv - previousUV) * size;
	}

	// Checks every pixel of a frame: where something was drawn its motion is ExpectedMotion with the displacement of the entity
	// drawn there (none for entities not listed) within `tolerance` pixels plus 0.1% of the motion's length; elsewhere 0.
	// Returns the drawn pixels.
	uint32_t CheckMotion(const MotionFrame& frame, const SceneCamera& current, const SceneCamera& previous,
		const std::unordered_map<uint32_t, glm::dvec3>& displacements = {}, double tolerance = 0.01)
	{
		uint32_t drawn = 0;
		uint32_t wrong = 0;
		double worst = 0.0;
		for (uint32_t y = 0; y < frame.Height; y++)
		{
			for (uint32_t x = 0; x < frame.Width; x++)
			{
				const size_t pixel = static_cast<size_t>(y) * frame.Width + x;
				const glm::dvec2 motion(frame.Motion[pixel]);
				glm::dvec2 expected(0.0);
				if (frame.Depth[pixel] > 0.0f)
				{
					drawn++;
					const auto found = displacements.find(frame.IDs[pixel]);
					const glm::dvec3 displacement = found != displacements.end() ? found->second : glm::dvec3(0.0);
					expected = ExpectedMotion(current, previous, frame.Width, frame.Height, x, y, frame.Depth[pixel], displacement);
				}
				const double error = glm::length(motion - expected);
				worst = std::max(worst, error);
				if (error > tolerance + 0.001 * glm::length(expected) || (frame.Depth[pixel] == 0.0f && motion != glm::dvec2(0.0)))
				{
					if (wrong++ == 0)
						MESSAGE("Pixel ", x, ",", y, " (entity ", frame.IDs[pixel], ") moved ", motion.x, ",", motion.y, " pixels instead of ", expected.x, ",",
							expected.y);
				}
			}
		}
		MESSAGE("Largest motion error: ", worst, " pixels over ", drawn, " drawn pixels");
		CHECK(wrong == 0);
		return drawn;
	}

	// Pixels showing the entity.
	uint32_t CountPixels(const MotionFrame& frame, Entity entity)
	{
		uint32_t count = 0;
		for (const uint32_t id : frame.IDs)
			count += id == GetID(entity) ? 1u : 0u;
		return count;
	}

	// A sphere with three levels of detail, chosen by halving the screen fraction.
	Ref<Mesh> CreateLODSphere()
	{
		Ref<Mesh> source = MeshFactory::CreateSphere(0.5f, 48, 24);
		REQUIRE(source);
		std::vector<uint32_t> indices = source->GetIndices();
		const std::vector<MeshUtils::GeneratedLOD> levels = MeshUtils::GenerateLODs(source->GetPositions(), source->GetAttributes(), indices, 2);
		REQUIRE(levels.size() == 2);
		Submesh submesh = source->GetSubmeshes()[0];
		submesh.LODs = { MeshLOD { 0, static_cast<uint32_t>(indices.size()) } };
		for (const MeshUtils::GeneratedLOD& generated : levels)
		{
			submesh.LODs.push_back(MeshLOD { static_cast<uint32_t>(indices.size()), static_cast<uint32_t>(generated.Indices.size()) });
			indices.insert(indices.end(), generated.Indices.begin(), generated.Indices.end());
		}
		std::string error;
		Ref<Mesh> mesh = Mesh::Create(source->GetPositions(), source->GetAttributes(), std::move(indices), { submesh }, &error);
		REQUIRE_MESSAGE(mesh, error);
		return mesh;
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
		CHECK(renderer.GetVelocityTexture() == nullptr);
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
		CHECK(renderer.GetVelocityTexture() == nullptr);
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
		const uint64_t id = RenderScene::Get(scene.Contents).GetId();
		const uint64_t version = mirror.GetWriteVersion();

		// The other renderer's frames write every cube again and again, until the log no longer reaches back to the version.
		std::vector<std::pair<uint32_t, uint32_t>> ranges;
		uint32_t frames = 0;
		for (; frames < 16 && mirror.GetInstanceWrites(id, version, ranges); frames++)
		{
			for (const Entity cube : scene.Cubes)
				scene.Move(cube, glm::vec3(0.0f, 0.1f, 0.0f));
			REQUIRE(current.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
			ranges.clear();
		}
		REQUIRE_FALSE(mirror.GetInstanceWrites(id, version, ranges));
		CHECK(frames > 1);
		REQUIRE(behind.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		CHECK(behind.GetStats().MotionHistory);
		CHECK(behind.GetStats().MotionFullCopy);
		CheckHistory(gpu, scene.Contents, behind);
		// Its next frame's motion is exact again: one step of the cubes.
		std::unordered_map<uint32_t, glm::dvec3> moved;
		for (const Entity cube : scene.Cubes)
		{
			scene.Move(cube, glm::vec3(0.0f, 0.0f, 0.25f));
			moved[GetID(cube)] = glm::dvec3(0.0, 0.0, 0.25);
		}
		REQUIRE(behind.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		CHECK(CheckMotion(ReadMotionFrame(behind), scene.Camera, scene.Camera, moved) > 0);
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

	TEST_CASE("A history that grows in a frame that draws nothing holds the frame's whole copy")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		// No light casts shadows and the camera looks away: no pass of the frame reads the history between its growth and its
		// copy, so nothing else orders the two writes (synchronization validation reports a write-after-write hazard there).
		Scene scene;
		AddNeutralPostProcess(scene);
		for (uint32_t index = 0; index < 8; index++)
			AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(static_cast<float>(index) * 1.5f - 5.0f, 0.0f, -4.0f), "Cube");
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.0f, 2.0f), glm::vec3(0.0f, 1.0f, 10.0f));
		SceneRenderer renderer = CreateRenderer();
		renderer.SetViewportSize(c_Size, c_Size);
		REQUIRE(renderer.Render(scene, camera, nullptr, MotionOptions()));
		REQUIRE(renderer.Render(scene, camera, nullptr, MotionOptions()));
		const uint64_t historyBytes = renderer.GetPreviousInstanceBuffer()->getDesc().byteSize;

		// More new cubes than half the grown buffers' room: the frame grows the history, then copies all of it.
		const uint64_t added = Renderer::GetRenderSceneGPUs().Get(RenderScene::Get(scene)).GetInstanceCapacity() * 3;
		for (uint32_t index = 0; index < added; index++)
			AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(static_cast<float>(index % 16), 0.0f, -6.0f - static_cast<float>(index / 16)), "New");
		REQUIRE(renderer.Render(scene, camera, nullptr, MotionOptions()));
		CHECK(GetDrawnCounts(renderer).Instances == 0);
		CHECK(renderer.GetPreviousInstanceBuffer()->getDesc().byteSize > historyBytes);
		CHECK(renderer.GetStats().MotionHistory);
		CHECK(renderer.GetStats().MotionFullCopy);
		CheckHistory(gpu, scene, renderer);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A still view has no motion and a moving camera moves every pixel as it reprojects, on both paths")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(PathName(mode));
			MotionScene scene(assets, 16);
			SceneRenderer renderer = CreateRenderer("Motion", mode);
			renderer.SetViewportSize(c_Size, c_Size);
			for (int frame = 0; frame < 3; frame++)
			{
				REQUIRE(renderer.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
				CHECK(renderer.GetStats().MotionHistory == (frame > 0));
				const MotionFrame motion = ReadMotionFrame(renderer);
				uint32_t moving = 0;
				for (const glm::vec2& pixel : motion.Motion)
					moving += pixel != glm::vec2(0.0f) ? 1u : 0u;
				CHECK(moving == 0);
			}
			// The camera moves and turns: every drawn pixel moves as its point reprojects, the empty ones not at all.
			const SceneCamera previous = scene.Camera;
			scene.Camera = LookAt(glm::vec3(0.6f, 13.5f, 5.2f), glm::vec3(0.3f, 0.0f, -3.4f));
			REQUIRE(renderer.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
			CHECK(renderer.GetStats().MotionHistory);
			CHECK(CheckMotion(ReadMotionFrame(renderer), scene.Camera, previous) > c_Size * c_Size / 2);
			gpu.GetDevice().WaitForIdle();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Moved entities, skinned poses and mirrored instances carry their own motion, the rest the camera's")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(PathName(mode));
			MotionScene scene(assets, 16);
			SceneRenderer renderer = CreateRenderer("Motion", mode);
			renderer.SetViewportSize(c_Size, c_Size);
			REQUIRE(renderer.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
			REQUIRE(renderer.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));

			const glm::vec3 cubeOffset(0.4f, 0.0f, -0.3f);
			const glm::vec3 mirroredOffset(0.0f, 0.2f, 0.3f);
			const glm::vec3 poseOffset(0.3f, 0.2f, 0.0f);
			scene.Move(scene.Cubes[3], cubeOffset);
			scene.Move(scene.Mirrored, mirroredOffset);
			scene.MovePose(poseOffset);
			const SceneCamera previous = scene.Camera;
			scene.Camera = LookAt(glm::vec3(0.2f, 14.0f, 6.1f), glm::vec3(0.1f, 0.0f, -3.0f));
			REQUIRE(renderer.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
			const MotionFrame motion = ReadMotionFrame(renderer);
			for (const Entity entity : { scene.Cubes[3], scene.Mirrored, scene.Arm })
				CHECK(CountPixels(motion, entity) > 8);
			CheckMotion(motion, scene.Camera, previous,
				{ { GetID(scene.Cubes[3]), glm::dvec3(cubeOffset) }, { GetID(scene.Mirrored), glm::dvec3(mirroredOffset) }, { GetID(scene.Arm), glm::dvec3(poseOffset) } });
			gpu.GetDevice().WaitForIdle();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("An instance slot another entity held, and an instance whose upload waited, start without motion and then move")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		MotionScene scene(assets, 16);
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		// One new proxy per frame: the others wait for their upload.
		specification.NewInstanceBytesPerFrame = 1;
		SceneRenderer renderer(specification);
		renderer.SetViewportSize(c_Size, c_Size);
		// Frames of the device: the upload budget starts again with each.
		const auto render = [&]()
		{
			REQUIRE(gpu.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			const bool rendered = renderer.Render(scene.Contents, scene.Camera, nullptr, MotionOptions());
			gpu.GetDevice().EndFrame();
			REQUIRE(rendered);
		};
		for (int frame = 0; frame < 40 && (frame < 2 || renderer.GetStats().DeferredProxies > 0); frame++)
			render();
		REQUIRE(renderer.GetStats().DeferredProxies == 0);

		// Another entity takes the destroyed cube's instance slot: it did not move, whatever the slot held before.
		RenderScene& renderScene = RenderScene::Get(scene.Contents);
		const uint32_t slot = renderScene.GetFirstInstance(renderScene.FindProxy(scene.Cubes[5].GetHandle()));
		const Entity destroyed[] = { scene.Cubes[5] };
		scene.Contents.DestroyEntities(destroyed);
		Entity replacement = AddMesh(scene.Contents, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(3.0f, 0.0f, -3.0f), "Replacement");
		render();
		REQUIRE(renderScene.GetFirstInstance(renderScene.FindProxy(replacement.GetHandle())) == slot);
		MotionFrame motion = ReadMotionFrame(renderer);
		CHECK(CountPixels(motion, replacement) > 8);
		CheckMotion(motion, scene.Camera, scene.Camera);

		// New cubes come one per frame; each moves from its first drawn frame on.
		std::vector<Entity> added;
		for (uint32_t index = 0; index < 4; index++)
			added.push_back(AddMesh(scene.Contents, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(-4.0f + static_cast<float>(index) * 2.0f, 0.0f, 1.0f), "Added"));
		std::unordered_map<uint32_t, glm::dvec3> displacements;
		for (uint32_t frame = 0; frame < 6; frame++)
		{
			for (const auto& [id, displacement] : displacements)
				scene.Move(Entity(static_cast<entt::entity>(id - 1), &scene.Contents), glm::vec3(displacement));
			render();
			CheckMotion(ReadMotionFrame(renderer), scene.Camera, scene.Camera, displacements);
			// What is drawn now moves from the next frame on.
			RenderSceneGPU& mirror = Renderer::GetRenderSceneGPUs().Get(renderScene);
			for (const Entity cube : added)
			{
				if (mirror.IsResident(renderScene.FindProxy(cube.GetHandle())))
					displacements[GetID(cube)] = glm::dvec3(0.0, 0.1, 0.0);
			}
		}
		CHECK(displacements.size() == added.size());
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A level-of-detail cross-fade carries the instance's motion through both levels, on both paths")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		Scene scene;
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::normalize(glm::vec3(-0.4f, -1.0f, -0.5f)), 3.0f, false);
		Entity sphere = AddMesh(scene, assets.Add(CreateLODSphere(), "FadingSphere"), UUID::Null(), glm::vec3(0.0f), "Sphere");
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(PathName(mode));
			SceneRenderer renderer = CreateRenderer("Motion", mode);
			renderer.SetViewportSize(c_Size, c_Size);
			// The camera backs away in steps short of a camera cut until the sphere's level changes and fades.
			float distance = 1.5f;
			SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, distance), glm::vec3(0.0f));
			REQUIRE(renderer.Render(scene, camera, nullptr, MotionOptions()));
			REQUIRE(GetDrawnCounts(renderer).InstancesPerLOD[0] == 1);
			bool fading = false;
			SceneCamera previous = camera;
			for (int step = 0; step < 40 && !fading; step++)
			{
				previous = camera;
				distance += 0.25f;
				camera = LookAt(glm::vec3(0.0f, 0.0f, distance), glm::vec3(0.0f));
				REQUIRE(renderer.Render(scene, camera, nullptr, MotionOptions()));
				fading = GetDrawnCounts(renderer).Instances == 2;
			}
			REQUIRE(fading);
			CheckMotion(ReadMotionFrame(renderer), camera, previous);
			// The fading sphere moves: both levels' pixels move with it.
			const glm::vec3 offset(0.05f, 0.03f, 0.0f);
			sphere.GetComponent<TransformComponent>().Translation += offset;
			sphere.MarkModified<TransformComponent>();
			REQUIRE(renderer.Render(scene, camera, nullptr, MotionOptions()));
			CHECK(GetDrawnCounts(renderer).Instances == 2);
			const MotionFrame motion = ReadMotionFrame(renderer);
			CHECK(CountPixels(motion, sphere) > 50);
			CheckMotion(motion, camera, camera, { { GetID(sphere), glm::dvec3(offset) } });
			sphere.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f);
			sphere.MarkModified<TransformComponent>();
			gpu.GetDevice().WaitForIdle();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Two renderers of one scene each compare with their own previous frame")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		MotionScene scene(assets, 8);
		SceneRenderer first = CreateRenderer("First");
		SceneRenderer second = CreateRenderer("Second");
		first.SetViewportSize(c_Size, c_Size);
		second.SetViewportSize(c_Size, c_Size);
		REQUIRE(first.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		REQUIRE(second.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		const glm::vec3 step(0.0f, 0.0f, 0.2f);
		scene.Move(scene.Cubes[2], step);
		REQUIRE(first.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		CheckMotion(ReadMotionFrame(first), scene.Camera, scene.Camera, { { GetID(scene.Cubes[2]), glm::dvec3(step) } });
		scene.Move(scene.Cubes[2], step);
		REQUIRE(first.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		CheckMotion(ReadMotionFrame(first), scene.Camera, scene.Camera, { { GetID(scene.Cubes[2]), glm::dvec3(step) } });
		// The second renderer last drew the cube two steps ago.
		REQUIRE(second.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
		const MotionFrame motion = ReadMotionFrame(second);
		CHECK(CountPixels(motion, scene.Cubes[2]) > 8);
		CheckMotion(motion, scene.Camera, scene.Camera, { { GetID(scene.Cubes[2]), glm::dvec3(step) * 2.0 } });
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A renderer that drew the source after its copy changed apart from it follows the copy from what it drew")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		Ref<Scene> source = CreateRef<Scene>("Source");
		AddNeutralPostProcess(*source);
		AddSun(*source, glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f)), 3.0f);
		std::vector<Entity> cubes;
		for (uint32_t index = 0; index < 4; index++)
			cubes.push_back(AddMesh(*source, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(static_cast<float>(index) * 2.0f - 3.0f, 0.0f, -2.0f), "Cube"));
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 8.0f, 4.0f), glm::vec3(0.0f, 0.0f, -2.0f));
		const auto move = [](Entity cube)
		{
			cube.GetComponent<TransformComponent>().Translation.y += 0.5f;
			cube.MarkModified<TransformComponent>();
		};
		SceneRenderer first = CreateRenderer("First");
		SceneRenderer second = CreateRenderer("Second");
		first.SetViewportSize(c_Size, c_Size);
		second.SetViewportSize(c_Size, c_Size);
		REQUIRE(first.Render(*source, camera, nullptr, MotionOptions()));

		// The copy's GPU copy is made from the source's, with its write log.
		Ref<Scene> copy = Scene::Copy(source);
		RenderSceneGPUCache& mirrors = Renderer::GetRenderSceneGPUs();
		const uint64_t sourceVersion = mirrors.Get(RenderScene::Get(*source)).GetWriteVersion();
		REQUIRE(second.Render(*copy, camera, nullptr, MotionOptions()));
		REQUIRE(mirrors.Get(RenderScene::Get(*copy)).GetWriteVersion() == sourceVersion);

		// The copy writes a cube, then the source another, which the first renderer draws.
		move(copy->GetEntityByUUID(cubes[0].GetUUID()));
		REQUIRE(second.Render(*copy, camera, nullptr, MotionOptions()));
		move(cubes[1]);
		REQUIRE(first.Render(*source, camera, nullptr, MotionOptions()));

		// Drawing the copy, the first renderer compares with the source as it drew it, and then holds the copy.
		REQUIRE(first.Render(*copy, camera, nullptr, MotionOptions()));
		CHECK(first.GetStats().MotionHistory);
		const uint32_t movedInCopy = GetID(copy->GetEntityByUUID(cubes[0].GetUUID()));
		const uint32_t movedInSource = GetID(copy->GetEntityByUUID(cubes[1].GetUUID()));
		CheckMotion(ReadMotionFrame(first), camera, camera, { { movedInCopy, glm::dvec3(0.0, 0.5, 0.0) }, { movedInSource, glm::dvec3(0.0, -0.5, 0.0) } });
		CheckHistory(gpu, *copy, first);
		REQUIRE(first.Render(*copy, camera, nullptr, MotionOptions()));
		CHECK(first.GetStats().MotionHistory);
		CheckMotion(ReadMotionFrame(first), camera, camera);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The motion view colors moving pixels by their direction over the image and darkens still ones")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		MotionScene scene(assets, 16);
		SceneRenderer plain = CreateRenderer("Plain");
		SceneRenderer view = CreateRenderer("View");
		plain.SetViewportSize(c_Size, c_Size);
		view.SetViewportSize(c_Size, c_Size);
		SceneRenderOptions options;
		options.ShowMotionVectors = true;
		REQUIRE(view.Render(scene.Contents, scene.Camera, nullptr, options));
		CHECK(view.GetVelocityTexture() != nullptr); // The view implies motion vectors
		// The cube moves right on screen (+X); the camera stays.
		scene.Move(scene.Cubes[9], glm::vec3(0.5f, 0.0f, 0.0f));
		REQUIRE(view.Render(scene.Contents, scene.Camera, nullptr, options));
		CHECK(view.GetStats().MotionHistory);
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(view.GetOutputTexture(), image));
		const MotionFrame motion = ReadMotionFrame(view);
		const ReadbackImage reference = RenderToImage(plain, scene.Contents, scene.Camera);
		uint32_t red = 0;
		uint32_t moving = 0;
		uint32_t darkened = 0;
		uint32_t still = 0;
		for (size_t pixel = 0; pixel < motion.IDs.size(); pixel++)
		{
			const uint8_t* color = &image.Pixels[pixel * 4];
			if (motion.IDs[pixel] == GetID(scene.Cubes[9]))
			{
				moving++;
				red += color[0] > color[1] + 10 && color[0] > color[2] + 10 ? 1u : 0u;
				continue;
			}
			// Still pixels: the image under 65% black.
			still++;
			bool close = true;
			for (int channel = 0; channel < 3; channel++)
				close = close && std::abs(static_cast<int>(color[channel]) - static_cast<int>(reference.Pixels[pixel * 4 + channel] * 0.35f + 0.5f)) <= 1;
			darkened += close ? 1u : 0u;
		}
		CHECK(moving > 8);
		CHECK(red == moving);
		CHECK(darkened == still);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Asking for motion vectors leaves the image bit-identical on both paths, with skinned and mirrored instances")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(PathName(mode));
			MotionScene scene(assets, 16);
			SceneRenderer plain = CreateRenderer("Plain", mode);
			SceneRenderer motion = CreateRenderer("Motion", mode);
			plain.SetViewportSize(c_Size, c_Size);
			motion.SetViewportSize(c_Size, c_Size);
			for (int frame = 0; frame < 4; frame++)
			{
				CAPTURE(frame);
				REQUIRE(plain.Render(scene.Contents, scene.Camera));
				REQUIRE(motion.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
				ReadbackImage plainHDR;
				ReadbackImage motionHDR;
				REQUIRE(Renderer::ReadTexture(plain.GetHDRTexture(), plainHDR));
				REQUIRE(Renderer::ReadTexture(motion.GetHDRTexture(), motionHDR));
				CHECK(plainHDR.Pixels == motionHDR.Pixels);
				ReadbackImage plainLDR;
				ReadbackImage motionLDR;
				REQUIRE(Renderer::ReadTexture(plain.GetOutputTexture(), plainLDR));
				REQUIRE(Renderer::ReadTexture(motion.GetOutputTexture(), motionLDR));
				CHECK(plainLDR.Pixels == motionLDR.Pixels);
				CHECK(plain.GetStats().DrawCommands == motion.GetStats().DrawCommands);
				scene.Move(scene.Cubes[1], glm::vec3(0.1f, 0.0f, 0.0f));
				scene.Move(scene.Mirrored, glm::vec3(0.0f, 0.0f, 0.1f));
				scene.MovePose(glm::vec3(0.0f, 0.05f, 0.0f));
				scene.Camera = LookAt(glm::vec3(0.1f * static_cast<float>(frame + 1), 14.0f, 6.0f), glm::vec3(0.0f, 0.0f, -3.0f));
			}
			gpu.GetDevice().WaitForIdle();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}

TEST_SUITE("GPU.SceneRenderer.CameraRelative")
{
	TEST_CASE("Motion vectors 20 kilometers from the origin are as exact as at the origin")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		for (const glm::vec3 origin : { glm::vec3(0.0f), glm::vec3(20'000.0f, 0.0f, 20'000.0f) })
		{
			CAPTURE(origin.x);
			MotionScene scene(assets, 16, origin);
			SceneRenderer renderer = CreateRenderer();
			renderer.SetViewportSize(c_Size, c_Size);
			REQUIRE(renderer.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
			const glm::vec3 offset(0.25f, 0.0f, 0.125f);
			scene.Move(scene.Cubes[4], offset);
			const SceneCamera previous = scene.Camera;
			scene.Camera = LookAt(origin + glm::vec3(0.5f, 13.75f, 5.5f), origin + glm::vec3(0.25f, 0.0f, -3.25f));
			REQUIRE(renderer.Render(scene.Contents, scene.Camera, nullptr, MotionOptions()));
			CHECK(CheckMotion(ReadMotionFrame(renderer), scene.Camera, previous, { { GetID(scene.Cubes[4]), glm::dvec3(offset) } }, 0.05) > c_Size * c_Size / 2);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
