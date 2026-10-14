#include "Renderer/SceneRendererTestUtils.h"

#include "Strata/Renderer/RenderScene.h"
#include "Strata/Renderer/RenderSceneGPU.h"

#include <cstring>
#include <random>
#include <vector>

// The GPU's copy of a render scene (RenderSceneGPU): shared by the scene's renderers, uploaded where it changed, new proxies
// within a frame's budget, grown by GPU copies, released with its scene.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = 96;

	std::vector<Entity> AddCubeRow(Scene& scene, uint32_t count, float z = 0.0f)
	{
		std::vector<Entity> cubes;
		for (uint32_t index = 0; index < count; index++)
			cubes.push_back(AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(static_cast<float>(index % 100) * 2.0f, 0.0f, z - static_cast<float>(index / 100) * 2.0f), "Cube"));
		return cubes;
	}

	// A camera above the rows of cubes, seeing all of them.
	SceneCamera ViewOfRows()
	{
		SceneCamera camera = LookAt(glm::vec3(100.0f, 120.0f, 60.0f), glm::vec3(100.0f, 0.0f, -10.0f));
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(75.0f), 1.0f, 0.1f, 1000.0f);
		camera.VerticalFOV = glm::radians(75.0f);
		camera.Far = 1000.0f;
		return camera;
	}

	// The meshes and the material the tests draw, loaded before the first frame, so frames draw what they place.
	void LoadBuiltinAssets(SceneTestAssets& assets)
	{
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::SphereMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::DefaultMaterial));
	}

	template<typename Record>
	std::vector<Record> ReadRecords(GPUContext& gpu, nvrhi::IBuffer* buffer, uint64_t count)
	{
		REQUIRE(buffer);
		REQUIRE(buffer->getDesc().byteSize >= count * sizeof(Record));
		const std::vector<uint8_t> bytes = ReadBufferRange(gpu, buffer, 0, count * sizeof(Record));
		std::vector<Record> records(count);
		std::memcpy(records.data(), bytes.data(), bytes.size());
		return records;
	}

	// The mirror's buffers hold every resident proxy's record and instances as the render scene has them, and a record of
	// zeros for every other proxy.
	void CheckMirror(GPUContext& gpu, const RenderScene& renderScene, const RenderSceneGPU& mirror)
	{
		gpu.GetDevice().WaitForIdle();
		const uint32_t proxies = renderScene.GetProxyCapacity();
		const std::vector<RenderData::GPUProxy> records = ReadRecords<RenderData::GPUProxy>(gpu, mirror.GetProxyBuffer(), proxies);
		const std::span<const RenderData::InstanceData> instances = renderScene.GetInstances();
		const std::vector<RenderData::InstanceData> gpuInstances = ReadRecords<RenderData::InstanceData>(gpu, mirror.GetInstanceBuffer(), instances.size());
		for (uint32_t proxy = 0; proxy < proxies; proxy++)
		{
			CAPTURE(proxy);
			const RenderData::GPUProxy expected = mirror.IsResident(proxy) ? RenderSceneGPU::MakeRecord(renderScene, proxy) : RenderData::GPUProxy {};
			REQUIRE(std::memcmp(&records[proxy], &expected, sizeof(expected)) == 0);
			if (!mirror.IsResident(proxy))
				continue;
			for (uint32_t index = 0; index < renderScene.GetInstanceCount(proxy); index++)
			{
				const uint32_t instance = renderScene.GetFirstInstance(proxy) + index;
				REQUIRE(std::memcmp(&gpuInstances[instance], &instances[instance], sizeof(RenderData::InstanceData)) == 0);
			}
		}
	}

	// A device frame around `work` (the renderer's frame begins with it: budgets start again, released copies go).
	template<typename Work>
	void Frame(GPUContext& gpu, const Work& work)
	{
		REQUIRE(gpu.GetDevice().BeginFrame());
		Renderer::BeginFrame();
		work();
		gpu.GetDevice().EndFrame();
	}

	void EmptyFrame(GPUContext& gpu)
	{
		Frame(gpu, []() {});
	}

}

TEST_SUITE("GPU.Renderer.RenderSceneGPU")
{
	TEST_CASE("The renderers of a scene share its GPU copy, which uploads what changed once")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadBuiltinAssets(assets);
		Scene scene;
		AddNeutralPostProcess(scene);
		const std::vector<Entity> cubes = AddCubeRow(scene, 300);
		const SceneCamera camera = ViewOfRows();
		const size_t mirrorsBefore = Renderer::GetRenderSceneGPUs().GetMirrorCount();

		SceneRenderer first;
		first.SetViewportSize(c_Size, c_Size);
		SceneRenderer second;
		second.SetViewportSize(c_Size, c_Size);
		REQUIRE(first.Render(scene, camera));
		CHECK(first.GetStats().InstanceBytesUploaded >= 300 * sizeof(RenderData::InstanceData));
		CHECK(first.GetStats().ProxyBytesUploaded == 300 * sizeof(RenderData::GPUProxy));
		CHECK(first.GetStats().DeferredInstanceBytes == 0);
		REQUIRE(second.Render(scene, camera));
		CHECK(second.GetStats().InstanceUploadRanges == 0);
		CHECK(second.GetStats().ProxyBytesUploaded == 0);
		CHECK(GetDrawnCounts(first).Instances > 0);
		CHECK(GetDrawnCounts(second).Instances == GetDrawnCounts(first).Instances);
		CHECK(Renderer::GetRenderSceneGPUs().GetMirrorCount() == mirrorsBefore + 1);
		RenderSceneGPU* mirror = Renderer::GetRenderSceneGPUs().Find(RenderScene::Get(scene).GetId());
		REQUIRE(mirror);
		CheckMirror(gpu, RenderScene::Get(scene), *mirror);

		// A moved cube: its instance and its record, once.
		Entity moved = cubes[123];
		moved.GetComponent<TransformComponent>().Translation.y += 0.5f;
		moved.MarkModified<TransformComponent>();
		REQUIRE(first.Render(scene, camera));
		CHECK(first.GetStats().InstanceUploadRanges == 1);
		CHECK(first.GetStats().InstanceBytesUploaded == sizeof(RenderData::InstanceData));
		CHECK(first.GetStats().ProxyBytesUploaded == sizeof(RenderData::GPUProxy));
		REQUIRE(second.Render(scene, camera));
		CHECK(second.GetStats().InstanceBytesUploaded == 0);
		CHECK(second.GetStats().ProxyBytesUploaded == 0);
		CheckMirror(gpu, RenderScene::Get(scene), *mirror);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("After random edits the GPU copy holds every resident proxy as the render scene has it, grown by GPU copies")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadBuiltinAssets(assets);
		Scene scene;
		AddNeutralPostProcess(scene);
		std::vector<Entity> entities = AddCubeRow(scene, 50);
		const SceneCamera camera = ViewOfRows();
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		REQUIRE(renderer.Render(scene, camera));
		RenderSceneGPU* mirror = Renderer::GetRenderSceneGPUs().Find(RenderScene::Get(scene).GetId());
		REQUIRE(mirror);
		const uint64_t firstVersion = mirror->GetBufferVersion();

		std::mt19937 random(77);
		auto pick = [&](size_t count) { return std::uniform_int_distribution<size_t>(0, count - 1)(random); };
		for (int round = 0; round < 40; round++)
		{
			CAPTURE(round);
			for (int edit = 0; edit < 25; edit++)
			{
				std::erase_if(entities, [](const Entity& entity) { return !entity.IsValid(); });
				switch (pick(6))
				{
					case 0:
					case 1:
					{
						// Batches of new cubes make the buffers grow.
						const std::vector<Entity> added = AddCubeRow(scene, 1 + static_cast<uint32_t>(pick(60)), -static_cast<float>(pick(50)));
						entities.insert(entities.end(), added.begin(), added.end());
						break;
					}
					case 2:
						if (!entities.empty())
							scene.DestroyEntity(entities[pick(entities.size())]);
						break;
					case 3:
						if (!entities.empty())
						{
							Entity entity = entities[pick(entities.size())];
							entity.GetComponent<TransformComponent>().Translation.y += 1.0f;
							entity.MarkModified<TransformComponent>();
						}
						break;
					case 4:
						if (!entities.empty())
						{
							Entity entity = entities[pick(entities.size())];
							MeshRendererComponent& meshRenderer = entity.GetComponent<MeshRendererComponent>();
							meshRenderer.Mesh = meshRenderer.Mesh == BuiltinAssets::CubeMesh ? BuiltinAssets::SphereMesh : BuiltinAssets::CubeMesh;
							entity.MarkModified<MeshRendererComponent>();
						}
						break;
					case 5:
						if (!entities.empty())
							entities[pick(entities.size())].SetActive(pick(2) == 0);
						break;
				}
			}
			REQUIRE(renderer.Render(scene, camera));
			CheckMirror(gpu, RenderScene::Get(scene), *mirror);
		}
		CHECK(mirror->GetBufferVersion() > firstVersion);
		CHECK(mirror->GetDeferredProxies() == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("New proxies upload within the frame's budget and are drawn once they arrived; destroyed ones are cleared at once")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadBuiltinAssets(assets);
		Scene scene;
		AddNeutralPostProcess(scene);
		const std::vector<Entity> cubes = AddCubeRow(scene, 1000);
		const SceneCamera camera = ViewOfRows();
		SceneRendererSpecification specification;
		constexpr uint64_t c_ProxyBytes = sizeof(RenderData::GPUProxy) + sizeof(RenderData::InstanceData);
		specification.NewInstanceBytesPerFrame = 300 * c_ProxyBytes;
		SceneRenderer renderer(specification);
		renderer.SetViewportSize(c_Size, c_Size);

		// 300 proxies a frame: the others wait, undrawn, until later frames' budgets take them.
		Frame(gpu, [&]()
		{
			REQUIRE(renderer.Render(scene, camera));
			CHECK(renderer.GetStats().ProxyBytesUploaded == 300 * sizeof(RenderData::GPUProxy));
			CHECK(renderer.GetStats().DeferredProxies == 700);
			CHECK(renderer.GetStats().DeferredInstanceBytes == 700 * c_ProxyBytes);
			CHECK(GetDrawnCounts(renderer).Instances > 0);
			CHECK(GetDrawnCounts(renderer).Instances <= 300);
			// Another render in the same frame finds the budget spent.
			REQUIRE(renderer.Render(scene, camera));
			CHECK(renderer.GetStats().DeferredProxies == 700);
		});
		RenderSceneGPU* mirror = Renderer::GetRenderSceneGPUs().Find(RenderScene::Get(scene).GetId());
		REQUIRE(mirror);
		CheckMirror(gpu, RenderScene::Get(scene), *mirror);
		uint32_t frames = 1;
		while (renderer.GetStats().DeferredProxies > 0)
		{
			REQUIRE(++frames <= 4);
			Frame(gpu, [&]() { REQUIRE(renderer.Render(scene, camera)); });
		}
		CHECK(frames == 4);
		CHECK(renderer.GetStats().VisibleProxies == GetDrawnCounts(renderer).Instances);
		CheckMirror(gpu, RenderScene::Get(scene), *mirror);

		// Destroyed: its record holds zeros in the same frame.
		const uint32_t proxy = RenderScene::Get(scene).FindProxy(cubes[10].GetHandle());
		scene.DestroyEntity(cubes[10]);
		Frame(gpu, [&]() { REQUIRE(renderer.Render(scene, camera)); });
		CHECK_FALSE(mirror->IsResident(proxy));
		CheckMirror(gpu, RenderScene::Get(scene), *mirror);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A destroyed scene's GPU copy goes at the next frame, and so does one no renderer used for 300 frames")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadBuiltinAssets(assets);
		RenderSceneGPUCache& cache = Renderer::GetRenderSceneGPUs();
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 5.0f, 10.0f), glm::vec3(0.0f));
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		// Copies of other tests' scenes leave first.
		for (uint64_t frame = 0; frame <= RenderSceneGPUCache::c_UnusedFrames; frame++)
			EmptyFrame(gpu);
		const size_t before = cache.GetMirrorCount();

		uint64_t destroyedId = 0;
		{
			Scope<Scene> scene = CreateScope<Scene>("Destroyed");
			AddCubeRow(*scene, 10);
			REQUIRE(renderer.Render(*scene, camera));
			destroyedId = RenderScene::Get(*scene).GetId();
			CHECK(cache.Find(destroyedId) != nullptr);
		}
		CHECK(cache.Find(destroyedId) != nullptr); // Until the next frame
		EmptyFrame(gpu);
		CHECK(cache.Find(destroyedId) == nullptr);
		CHECK(cache.GetMirrorCount() == before);

		Scene idle("Idle");
		AddCubeRow(idle, 10);
		REQUIRE(renderer.Render(idle, camera));
		const uint64_t idleId = RenderScene::Get(idle).GetId();
		for (uint64_t frame = 0; frame < RenderSceneGPUCache::c_UnusedFrames; frame++)
			EmptyFrame(gpu);
		CHECK(cache.Find(idleId) != nullptr);
		EmptyFrame(gpu);
		CHECK(cache.Find(idleId) == nullptr);
		// Rendered again: uploaded again from scratch.
		REQUIRE(renderer.Render(idle, camera));
		REQUIRE(cache.Find(idleId) != nullptr);
		CHECK(renderer.GetStats().ProxyBytesUploaded == 10 * sizeof(RenderData::GPUProxy));
		CheckMirror(gpu, RenderScene::Get(idle), *cache.Find(idleId));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A loaded scene's GPU copy is filled while it is prepared, so the frame that shows it uploads no proxy")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadBuiltinAssets(assets);
		Scene scene;
		AddNeutralPostProcess(scene);
		AddCubeRow(scene, 500);
		int calls = 0;
		while (!RenderScene::PrepareLoadedScene(scene))
			REQUIRE(++calls < 100);
		RenderSceneGPU* mirror = Renderer::GetRenderSceneGPUs().Find(RenderScene::Get(scene).GetId());
		REQUIRE(mirror);
		CHECK(mirror->GetDeferredProxies() == 0);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = ViewOfRows();
		REQUIRE(renderer.Render(scene, camera));
		CHECK(renderer.GetStats().ProxyBytesUploaded == 0);
		CHECK(renderer.GetStats().InstanceUploadRanges == 0);
		CHECK(GetDrawnCounts(renderer).Instances > 0);
		CheckMirror(gpu, RenderScene::Get(scene), *mirror);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
