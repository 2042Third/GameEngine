#include "Renderer/SceneRendererTestUtils.h"

#include "Renderer/TextureStreamingTestUtils.h"
#include "Strata/Renderer/BufferReadback.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Renderer/RenderScene.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// Visibility feedback (SceneRendererSpecification::VisibilityFeedback): GPU-driven frames take their asset requests and the
// screen sizes texture streaming follows from the GPU's culling of an earlier frame, and the CPU's walk of the view runs
// only where the feedback cannot tell yet.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = 128;
	constexpr uint32_t c_Materials = 24;
	// The grid's near corners lie out of view within it.
	constexpr float c_PrefetchDistance = 20.0f;

	bool DeviceDrawsGPUDriven()
	{
		return Renderer::GetGraphicsDevice().GetInfo().SupportsDrawIndirectFirstInstance;
	}

	SceneCamera View(const glm::vec3& position, const glm::vec3& target)
	{
		SceneCamera camera = LookAt(position, target);
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(60.0f), 1.0f, 0.1f, 500.0f);
		camera.Far = 500.0f;
		return camera;
	}

	// A pack of textured materials (each its own texture) and a grid of cubes, one per material, part of it in view.
	struct FeedbackWorld
	{
		Ref<RuntimeAssetManager> Manager;
		std::vector<AssetHandle> Materials;
		std::vector<AssetHandle> Textures;
		std::vector<Entity> Cubes;
		Scene World;

		FeedbackWorld()
		{
			std::vector<StreamingPackAsset> assets;
			for (uint32_t index = 0; index < c_Materials; index++)
			{
				const AssetHandle texture(0xF000 + 2 * index);
				const AssetHandle material(0xF001 + 2 * index);
				const std::array<uint8_t, 4> color = { static_cast<uint8_t>(40 + index * 9), static_cast<uint8_t>(200 - index * 5), 120, 255 };
				MaterialProperties properties = UnlitColor(glm::vec4(1.0f));
				properties.BaseColorMap = texture;
				assets.push_back(StreamingPackAsset { texture, AssetType::Texture, "Textures/Color" + std::to_string(index) + ".png", CookSolidTexture(16, color) });
				assets.push_back(StreamingPackAsset { material, AssetType::Material, "Materials/Color" + std::to_string(index) + ".stmat", SerializeMaterial(properties) });
				Textures.push_back(texture);
				Materials.push_back(material);
			}
			std::string error;
			Manager = RuntimeAssetManager::Create(WriteStreamingPack(assets), &error);
			REQUIRE_MESSAGE(Manager, error);
			AssetManager::SetActive(Manager);
			AddNeutralPostProcess(World);
			// Six columns, four rows reaching away from the camera; the view takes in the middle columns.
			for (uint32_t index = 0; index < c_Materials; index++)
			{
				const glm::vec3 position(static_cast<float>(index % 6) * 6.0f - 15.0f, 0.0f, -static_cast<float>(index / 6) * 6.0f);
				Cubes.push_back(AddMesh(World, BuiltinAssets::CubeMesh, Materials[index], position, "Cube"));
			}
		}

		~FeedbackWorld()
		{
			AssetManager::SetActive(nullptr);
		}

		FeedbackWorld(const FeedbackWorld&) = delete;
		FeedbackWorld& operator=(const FeedbackWorld&) = delete;
	};

	SceneRendererSpecification Specification(bool feedback)
	{
		SceneRendererSpecification specification;
		specification.VisibilityFeedback = feedback;
		specification.PrefetchDistance = c_PrefetchDistance;
		return specification;
	}

	// A device frame of the application: the asset manager's update, then each renderer's frame.
	void Frame(GPUContext& gpu, FeedbackWorld& world, std::initializer_list<SceneRenderer*> renderers, const SceneCamera& camera)
	{
		REQUIRE(gpu.GetDevice().BeginFrame());
		Renderer::BeginFrame();
		world.Manager->Update();
		for (SceneRenderer* renderer : renderers)
			REQUIRE(renderer->Render(world.World, camera));
		gpu.GetDevice().EndFrame();
	}

	// A frame after which the renderer has read its culling's counters and feedback back (the next frame can use them).
	void FrameAndResolve(GPUContext& gpu, FeedbackWorld& world, SceneRenderer& renderer, const SceneCamera& camera)
	{
		Frame(gpu, world, { &renderer }, camera);
		renderer.ResolveGPUStats(true);
	}

	std::map<AssetHandle, VisibleMaterialUse> ByMaterial(std::span<const VisibleMaterialUse> uses)
	{
		std::map<AssetHandle, VisibleMaterialUse> byMaterial;
		for (const VisibleMaterialUse& use : uses)
			byMaterial[use.Material] = use;
		return byMaterial;
	}

}

TEST_SUITE("GPU.SceneRenderer.Feedback")
{
	TEST_CASE("Feedback frames request what the CPU's walk of the view requests, and report the same screen sizes for streaming")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!DeviceDrawsGPUDriven())
			return;
		FeedbackWorld world;
		SceneRenderer feedback(Specification(true));
		SceneRenderer marks(Specification(false));
		feedback.SetViewportSize(c_Size, c_Size);
		marks.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = View(glm::vec3(0.0f, 7.0f, 9.0f), glm::vec3(0.0f, 0.0f, -8.0f));

		// Until what the view shows is loaded, and the feedback renderer's frames take their requests from feedback.
		bool settled = false;
		for (int frame = 0; frame < 600 && !settled; frame++)
		{
			Frame(gpu, world, { &feedback, &marks }, camera);
			feedback.ResolveGPUStats(true);
			settled = feedback.GetStats().FeedbackRequests && feedback.GetStats().PendingAssets == 0 && marks.GetStats().PendingAssets == 0
				&& !world.Manager->HasPendingLoads();
		}
		REQUIRE(settled);
		Frame(gpu, world, { &feedback, &marks }, camera);
		const SceneRendererStats& feedbackStats = feedback.GetStats();
		const SceneRendererStats& marksStats = marks.GetStats();
		REQUIRE(feedbackStats.FeedbackRequests);
		CHECK_FALSE(marksStats.FeedbackRequests);
		CHECK(marksStats.Materials > 0);
		CHECK(marksStats.Materials < c_Materials); // Part of the grid is out of view
		CHECK(feedbackStats.Materials == marksStats.Materials);
		CHECK(feedbackStats.RequestsByVisibility == marksStats.RequestsByVisibility);
		CHECK(feedbackStats.PrefetchRequests == marksStats.PrefetchRequests);
		CHECK(marksStats.PrefetchRequests > 0);

		// The GPU's screen sizes and UV pixels (Include/TextureStreaming.glsl) are the CPU's.
		const std::map<AssetHandle, VisibleMaterialUse> expected = ByMaterial(marks.GetVisibleMaterials());
		const std::map<AssetHandle, VisibleMaterialUse> actual = ByMaterial(feedback.GetVisibleMaterials());
		REQUIRE(actual.size() == expected.size());
		for (const auto& [material, use] : expected)
		{
			CAPTURE(material);
			const auto found = actual.find(material);
			REQUIRE(found != actual.end());
			CHECK(found->second.PixelSize == doctest::Approx(use.PixelSize).epsilon(1e-4));
			CHECK(found->second.UVPixels == doctest::Approx(use.UVPixels).epsilon(1e-4));
			CHECK(use.UVPixels > 0.0f);
		}
		CHECK(feedback.GetVisibleTextures().size() == marks.GetVisibleTextures().size());
		// Out of view and beyond the prefetch distance: requested by neither.
		const Frustum frustum(camera.Projection * camera.View);
		for (uint32_t index = 0; index < c_Materials; index++)
		{
			CAPTURE(index);
			const AABB bounds = AABB(glm::vec3(-0.5f), glm::vec3(0.5f)).Transformed(world.Cubes[index].GetComponent<WorldTransformComponent>().Matrix);
			const bool wanted = frustum.IsAABBVisible(bounds) || SpatialIndex::IntersectsSphere(bounds, camera.Position, c_PrefetchDistance);
			CHECK((world.Manager->GetAssetState(world.Materials[index]) != AssetState::Unloaded) == wanted);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Camera cuts and assets loaded since the feedback's frame run the CPU's walk until feedback has seen them")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!DeviceDrawsGPUDriven())
			return;
		FeedbackWorld world;
		for (const AssetHandle material : world.Materials)
			REQUIRE(world.Manager->LoadAssetSync(material));
		for (const AssetHandle texture : world.Textures)
			REQUIRE(world.Manager->LoadAssetSync(texture));
		SceneRendererSpecification specification = Specification(true);
		specification.TextureStreaming = false;
		SceneRenderer renderer(specification);
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = View(glm::vec3(0.0f, 7.0f, 9.0f), glm::vec3(0.0f, 0.0f, -8.0f));

		// The first frame has no feedback; the second takes the first one's.
		FrameAndResolve(gpu, world, renderer, camera);
		CHECK_FALSE(renderer.GetStats().FeedbackRequests);
		FrameAndResolve(gpu, world, renderer, camera);
		CHECK(renderer.GetStats().FeedbackRequests);

		// A cut: the CPU walks the new view; the frame after takes the cut frame's feedback, not that of the view before.
		const SceneCamera elsewhere = View(glm::vec3(0.0f, 7.0f, -40.0f), glm::vec3(0.0f, 0.0f, -10.0f));
		FrameAndResolve(gpu, world, renderer, elsewhere);
		CHECK_FALSE(renderer.GetStats().FeedbackRequests);
		FrameAndResolve(gpu, world, renderer, elsewhere);
		CHECK(renderer.GetStats().FeedbackRequests);

		// A material replaced (its slot's data built again): feedback culled before then did not see it.
		MaterialProperties edited = UnlitColor(glm::vec4(0.2f, 0.9f, 0.3f, 1.0f));
		edited.BaseColorMap = world.Textures[0];
		REQUIRE(world.Manager->ReplaceLoadedAsset(world.Materials[7], Material::Create(edited)));
		FrameAndResolve(gpu, world, renderer, elsewhere);
		CHECK_FALSE(renderer.GetStats().FeedbackRequests);
		FrameAndResolve(gpu, world, renderer, elsewhere);
		CHECK(renderer.GetStats().FeedbackRequests);

		// Without feedback, every frame walks the view.
		SceneRenderer walking(Specification(false));
		walking.SetViewportSize(c_Size, c_Size);
		for (int frame = 0; frame < 3; frame++)
		{
			FrameAndResolve(gpu, world, walking, elsewhere);
			CHECK_FALSE(walking.GetStats().FeedbackRequests);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Feedback is a few frames late: a material no longer drawn stops being requested once the GPU's culling saw it go")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!DeviceDrawsGPUDriven())
			return;
		FeedbackWorld world;
		for (const AssetHandle material : world.Materials)
			REQUIRE(world.Manager->LoadAssetSync(material));
		SceneRenderer renderer(Specification(true));
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = View(glm::vec3(0.0f, 7.0f, 9.0f), glm::vec3(0.0f, 0.0f, -8.0f));
		FrameAndResolve(gpu, world, renderer, camera);
		FrameAndResolve(gpu, world, renderer, camera);
		REQUIRE(renderer.GetStats().FeedbackRequests);
		const uint32_t materials = renderer.GetStats().Materials;
		REQUIRE(materials > 1);

		// A cube in the middle of the view goes: the next frame still requests its material (the feedback is of the frame
		// before), the one after no longer.
		Entity middle = world.Cubes[2];
		REQUIRE(Frustum(camera.Projection * camera.View).IsAABBVisible(AABB(glm::vec3(-0.5f), glm::vec3(0.5f)).Transformed(middle.GetComponent<WorldTransformComponent>().Matrix)));
		middle.SetActive(false);
		FrameAndResolve(gpu, world, renderer, camera);
		REQUIRE(renderer.GetStats().FeedbackRequests);
		CHECK(renderer.GetStats().Materials == materials);
		FrameAndResolve(gpu, world, renderer, camera);
		REQUIRE(renderer.GetStats().FeedbackRequests);
		CHECK(renderer.GetStats().Materials == materials - 1);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Without a GPU-written draw count, draws beyond the slots the feedback foresaw run the CPU's walk once")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!DeviceDrawsGPUDriven())
			return;
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::DefaultMaterial));
		Scene scene;
		AddNeutralPostProcess(scene);
		// Sixty meshes of their own (a draw slot each) beside the view, and one in it.
		constexpr uint32_t c_Meshes = 60;
		AddMesh(scene, assets.Add(MeshFactory::CreateCube(1.0f), "Seen"), UUID::Null(), glm::vec3(0.0f, 0.0f, -10.0f), "Seen");
		for (uint32_t index = 0; index < c_Meshes; index++)
		{
			const AssetHandle mesh = assets.Add(MeshFactory::CreateCube(0.5f + static_cast<float>(index) * 0.001f), "Beside");
			AddMesh(scene, mesh, UUID::Null(), glm::vec3(-14.0f + static_cast<float>(index % 10) * 0.8f, static_cast<float>(index / 10) * 0.8f - 2.0f, -10.0f), "Beside");
		}
		SceneRendererSpecification specification;
		specification.IndirectMode = IndirectDrawMode::NoCount;
		specification.PrefetchDistance = 0.0f;
		SceneRenderer renderer(specification);
		renderer.SetViewportSize(c_Size, c_Size);
		const auto frame = [&](const SceneCamera& camera)
		{
			REQUIRE(gpu.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			REQUIRE(renderer.Render(scene, camera));
			gpu.GetDevice().EndFrame();
			renderer.ResolveGPUStats(true);
			REQUIRE(renderer.GetGPUStats().CountersRender == renderer.GetRenderCount());
		};
		// The view at first: the one mesh. Then turned left by 35 degrees (no cut): the sixty come into view.
		const SceneCamera ahead = View(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -10.0f));
		const glm::vec3 turned = glm::vec3(-std::sin(glm::radians(35.0f)), 0.0f, -std::cos(glm::radians(35.0f))) * 10.0f;
		const SceneCamera left = View(glm::vec3(0.0f), turned);
		frame(ahead);
		frame(ahead);
		REQUIRE(renderer.GetStats().FeedbackRequests);
		CHECK(renderer.GetGPUStats().Instances == 1);

		// The feedback saw one mesh in view: its slots overflow, and what overflows is not drawn.
		frame(left);
		CHECK(renderer.GetStats().FeedbackRequests);
		CHECK(renderer.GetGPUStats().Overflow > 0);
		// The next frame walks the view on the CPU and bounds the slots exactly: everything is drawn.
		frame(left);
		CHECK_FALSE(renderer.GetStats().FeedbackRequests);
		CHECK(renderer.GetGPUStats().Overflow == 0);
		CHECK(renderer.GetGPUStats().Instances >= c_Meshes);
		// And feedback takes over again.
		frame(left);
		CHECK(renderer.GetStats().FeedbackRequests);
		CHECK(renderer.GetGPUStats().Overflow == 0);
		CHECK(renderer.GetGPUStats().Instances >= c_Meshes);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}

TEST_SUITE("GPU.Renderer.BufferReadback")
{
	TEST_CASE("A buffer's copy reads back from any offset once the GPU ran it, without waiting")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();
		std::vector<uint32_t> values(256);
		for (uint32_t index = 0; index < values.size(); index++)
			values[index] = index * 7 + 3;
		nvrhi::BufferHandle source = device->createBuffer(nvrhi::BufferDesc()
			.setByteSize(values.size() * sizeof(uint32_t))
			.setInitialState(nvrhi::ResourceStates::ShaderResource)
			.setKeepInitialState(true)
			.setDebugName("ReadbackSource"));
		REQUIRE(source);
		BufferReadback readback(device, values.size() * sizeof(uint32_t), "TestReadback");
		REQUIRE(readback.IsValid());
		nvrhi::CommandListHandle commandList = device->createCommandList();
		commandList->open();
		commandList->writeBuffer(source, values.data(), values.size() * sizeof(uint32_t));
		readback.Record(commandList, source, 64 * sizeof(uint32_t), 128 * sizeof(uint32_t));
		commandList->close();
		device->executeCommandList(commandList);
		readback.Submit();
		CHECK(readback.IsPending());
		readback.Wait();
		REQUIRE(readback.IsReady());

		uint32_t first = 0;
		REQUIRE(readback.Read(&first, sizeof(first)));
		CHECK(first == values[64]);
		std::vector<uint32_t> tail(16);
		REQUIRE(readback.Read(tail.data(), tail.size() * sizeof(uint32_t), 100 * sizeof(uint32_t)));
		for (uint32_t index = 0; index < tail.size(); index++)
			CHECK(tail[index] == values[164 + index]);
		// Beyond what was copied: only what the copy holds, and nothing past its end.
		uint32_t last[4] = { 0, 0, 0, 0 };
		REQUIRE(readback.Read(last, sizeof(last), 126 * sizeof(uint32_t)));
		CHECK(last[0] == values[190]);
		CHECK(last[1] == values[191]);
		CHECK(last[2] == 0);
		CHECK_FALSE(readback.Read(last, sizeof(last), 129 * sizeof(uint32_t)));
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
