#include "Renderer/SceneRendererTestUtils.h"

#include "Renderer/TextureStreamingTestUtils.h"
#include "Strata/Renderer/MaterialData.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Renderer/RenderSceneGPU.h"
#include "Strata/Renderer/Texture.h"
#include "Strata/Renderer/TextureTable.h"

#include <cstring>
#include <random>
#include <vector>

// The GPU's copy of a render scene (RenderSceneGPU): shared by the scene's renderers, uploaded where it changed, new proxies
// within a frame's budget, grown by GPU copies, released with its scene; its mesh and material tables follow what is loaded.

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

	// The mesh and material tables' buffers hold what the mirror says they hold.
	void CheckTables(GPUContext& gpu, const RenderSceneGPU& mirror)
	{
		gpu.GetDevice().WaitForIdle();
		const uint32_t meshSlots = mirror.GetMeshSlotCount();
		const uint32_t materialSlots = mirror.GetMaterialSlotCount();
		REQUIRE(meshSlots > 0);
		REQUIRE(materialSlots > 0);
		const std::vector<uint32_t> meshTable = ReadRecords<uint32_t>(gpu, mirror.GetMeshTableBuffer(), meshSlots);
		const std::vector<uint32_t> flags = ReadRecords<uint32_t>(gpu, mirror.GetMaterialFlagBuffer(), materialSlots);
		const std::vector<RenderData::MaterialData> data = ReadRecords<RenderData::MaterialData>(gpu, mirror.GetMaterialBuffer(), materialSlots);
		for (uint32_t slot = 0; slot < meshSlots; slot++)
		{
			CAPTURE(slot);
			CHECK(meshTable[slot] == mirror.GetMeshTableEntry(slot));
		}
		for (uint32_t slot = 0; slot < materialSlots; slot++)
		{
			CAPTURE(slot);
			CHECK(flags[slot] == mirror.GetMaterialFlags(slot));
			CHECK(std::memcmp(&data[slot], mirror.GetMaterialData(slot), sizeof(RenderData::MaterialData)) == 0);
		}
	}

	// A device frame that syncs the scene's render scene and updates its GPU copy with `manager`.
	RenderSceneGPUUpdate UpdateMirror(GPUContext& gpu, Scene& scene, const Ref<AssetManagerBase>& manager)
	{
		RenderSceneGPUUpdate update;
		Frame(gpu, [&]()
		{
			RenderScene& renderScene = RenderScene::Get(scene);
			renderScene.Sync(manager);
			REQUIRE(Renderer::GetRenderSceneGPUs().Get(renderScene).Update(renderScene, manager, 64ull << 20, &update));
		});
		return update;
	}

	// What the mesh table holds for a loaded mesh, and its LOD records.
	uint32_t ExpectedMeshEntry(const AssetManagerBase& manager, AssetHandle handle, uint32_t* outRecords)
	{
		const Ref<Mesh> mesh = manager.PeekLoadedAsset<Mesh>(handle);
		REQUIRE(mesh);
		const GeometryAllocation* geometry = mesh->GetGeometry();
		REQUIRE(geometry);
		*outRecords = 0;
		for (const Submesh& submesh : mesh->GetSubmeshes())
			*outRecords += static_cast<uint32_t>(submesh.LODs.size());
		return mesh->GetGeometryHandle() | (geometry->Arena == GeometryArena::Skinned ? RenderData::c_CullMeshSkinnedArena : 0u);
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

	TEST_CASE("The GPU copy's mesh and material tables follow loads, edits and evictions, and build only what changed")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		// A material with a texture of its own, one without and a mesh, in a pack: loaded objects of a pack can be unloaded and
		// replaced (built-in assets cannot).
		constexpr AssetHandle c_Texture(0xE100);
		constexpr AssetHandle c_TexturedMaterial(0xE101);
		constexpr AssetHandle c_PlainMaterial(0xE102);
		constexpr AssetHandle c_Torus(0xE103);
		MaterialProperties textured = UnlitColor(glm::vec4(1.0f));
		textured.BaseColorMap = c_Texture;
		const std::filesystem::path pack = WriteStreamingPack({
			StreamingPackAsset { c_Texture, AssetType::Texture, "Textures/Red.png", CookSolidTexture(4, { 255, 0, 0, 255 }) },
			StreamingPackAsset { c_TexturedMaterial, AssetType::Material, "Materials/Textured.stmat", SerializeMaterial(textured) },
			StreamingPackAsset { c_PlainMaterial, AssetType::Material, "Materials/Plain.stmat", SerializeMaterial(UnlitColor(glm::vec4(0.0f, 1.0f, 0.0f, 1.0f))) },
			StreamingPackAsset { c_Torus, AssetType::Mesh, "Meshes/Torus.mesh", MeshFactory::CreateTorus()->Serialize() } });
		std::string error;
		const Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(pack, &error);
		REQUIRE_MESSAGE(manager, error);
		AssetManager::SetActive(manager);
		REQUIRE(manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		const Ref<Material> defaultMaterial = manager->LoadAssetSync<Material>(BuiltinAssets::DefaultMaterial);
		REQUIRE(defaultMaterial);

		Scene scene;
		AddMesh(scene, BuiltinAssets::CubeMesh, c_TexturedMaterial, glm::vec3(-2.0f, 0.0f, 0.0f), "Textured");
		Entity plain = AddMesh(scene, BuiltinAssets::CubeMesh, c_PlainMaterial, glm::vec3(2.0f, 0.0f, 0.0f), "Plain");
		const Ref<AssetManagerBase> assets = manager;
		UpdateMirror(gpu, scene, assets);
		RenderScene& renderScene = RenderScene::Get(scene);
		const RenderSceneGPU& mirror = Renderer::GetRenderSceneGPUs().Get(renderScene);
		const uint32_t cubeSlot = renderScene.FindMeshSlot(BuiltinAssets::CubeMesh);
		const uint32_t texturedSlot = renderScene.FindMaterialSlot(c_TexturedMaterial);
		const uint32_t plainSlot = renderScene.FindMaterialSlot(c_PlainMaterial);
		REQUIRE(cubeSlot < mirror.GetMeshSlotCount());
		REQUIRE(texturedSlot < mirror.GetMaterialSlotCount());
		REQUIRE(plainSlot < mirror.GetMaterialSlotCount());

		// The loaded cube is drawn; the materials, not loaded yet, as the default material.
		uint32_t cubeRecords = 0;
		CHECK(mirror.GetMeshTableEntry(cubeSlot) == ExpectedMeshEntry(*manager, BuiltinAssets::CubeMesh, &cubeRecords));
		CHECK(mirror.GetMeshRecords(cubeSlot) == cubeRecords);
		CHECK(mirror.GetArenaRecords(0) == cubeRecords);
		const uint32_t defaultFlags = MaterialGPU::GetFlags(defaultMaterial->GetProperties());
		CHECK(mirror.GetMaterialFlags(texturedSlot) == defaultFlags);
		CHECK(mirror.GetMaterialFlags(plainSlot) == defaultFlags);
		CHECK(mirror.GetMaterialData(texturedSlot)->BaseColor == defaultMaterial->GetProperties().BaseColor);
		CHECK(std::memcmp(mirror.GetMaterialData(texturedSlot), mirror.GetMaterialData(plainSlot), sizeof(RenderData::MaterialData)) == 0);
		CheckTables(gpu, mirror);

		// Nothing changed: nothing is built or uploaded.
		RenderSceneGPUUpdate update = UpdateMirror(gpu, scene, assets);
		CHECK(update.MeshesChecked == 0);
		CHECK(update.MaterialsBuilt == 0);
		CHECK(update.TableBytes == 0);
		CHECK(update.MaterialBytes == 0);

		// The textured material loads before its texture: the texture's place holds the loading fallback.
		REQUIRE(manager->LoadAssetSync(c_TexturedMaterial));
		update = UpdateMirror(gpu, scene, assets);
		CHECK(update.MaterialsBuilt == 1);
		CHECK(update.MaterialBytes == sizeof(RenderData::MaterialData));
		CHECK(mirror.GetMaterialFlags(texturedSlot) == MaterialGPU::GetFlags(textured));
		CHECK(mirror.GetMaterialData(texturedSlot)->BaseColorMap == TextureTable::c_White);
		CHECK(mirror.GetMaterialFlags(plainSlot) == defaultFlags);
		CheckTables(gpu, mirror);

		// The texture arrives: only the material that names it is built again, now with the texture's table index.
		const Ref<Texture> texture = manager->LoadAssetSync<Texture>(c_Texture);
		REQUIRE(texture);
		REQUIRE(texture->GetTableIndex() != TextureTable::c_InvalidIndex);
		update = UpdateMirror(gpu, scene, assets);
		CHECK(update.MaterialsBuilt == 1);
		CHECK(update.MeshesChecked == 0);
		CHECK(mirror.GetMaterialData(texturedSlot)->BaseColorMap == texture->GetTableIndex());
		CheckTables(gpu, mirror);

		// An edit publishes a new object: its flags follow (alpha-masked and double-sided now).
		REQUIRE(manager->LoadAssetSync(c_PlainMaterial));
		UpdateMirror(gpu, scene, assets);
		CHECK_FALSE(mirror.HasAlphaMaskedMaterials());
		CHECK_FALSE(mirror.HasDoubleSidedMaterials());
		MaterialProperties edited = UnlitColor(glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
		edited.AlphaMode = MaterialAlphaMode::Mask;
		edited.DoubleSided = true;
		REQUIRE(manager->ReplaceLoadedAsset(c_PlainMaterial, Material::Create(edited)));
		update = UpdateMirror(gpu, scene, assets);
		CHECK(update.MaterialsBuilt == 1);
		CHECK(mirror.GetMaterialFlags(plainSlot) == MaterialGPU::GetFlags(edited));
		CHECK(mirror.GetMaterialData(plainSlot)->BaseColor == edited.BaseColor);
		CHECK(mirror.HasAlphaMaskedMaterials());
		CHECK(mirror.HasDoubleSidedMaterials());
		CheckTables(gpu, mirror);

		// The texture is evicted: the material that names it shows the fallback again. Then the material goes too.
		manager->UnloadAsset(c_Texture);
		update = UpdateMirror(gpu, scene, assets);
		CHECK(update.MaterialsBuilt == 1);
		CHECK(mirror.GetMaterialData(texturedSlot)->BaseColorMap == TextureTable::c_White);
		manager->UnloadAsset(c_TexturedMaterial);
		UpdateMirror(gpu, scene, assets);
		CHECK(mirror.GetMaterialFlags(texturedSlot) == defaultFlags);
		CHECK(mirror.GetMaterialData(texturedSlot)->BaseColor == defaultMaterial->GetProperties().BaseColor);
		CheckTables(gpu, mirror);

		// Another mesh takes a new slot: nothing to draw until it loads, and nothing again once it is evicted.
		plain.GetComponent<MeshRendererComponent>().Mesh = c_Torus;
		plain.MarkModified<MeshRendererComponent>();
		UpdateMirror(gpu, scene, assets);
		const uint32_t torusSlot = renderScene.FindMeshSlot(c_Torus);
		REQUIRE(torusSlot < mirror.GetMeshSlotCount());
		CHECK(mirror.GetMeshTableEntry(torusSlot) == 0);
		CHECK(mirror.GetArenaRecords(0) == cubeRecords);
		REQUIRE(manager->LoadAssetSync(c_Torus));
		update = UpdateMirror(gpu, scene, assets);
		CHECK(update.MeshesChecked == 1);
		uint32_t torusRecords = 0;
		CHECK(mirror.GetMeshTableEntry(torusSlot) == ExpectedMeshEntry(*manager, c_Torus, &torusRecords));
		CHECK(mirror.GetMeshRecords(torusSlot) == torusRecords);
		CHECK(mirror.GetArenaRecords(0) == cubeRecords + torusRecords);
		CheckTables(gpu, mirror);
		manager->UnloadAsset(c_Torus);
		UpdateMirror(gpu, scene, assets);
		CHECK(mirror.GetMeshTableEntry(torusSlot) == 0);
		CHECK(mirror.GetArenaRecords(0) == cubeRecords);
		CheckTables(gpu, mirror);

		// Another asset manager: every slot is built again from its objects (its own built-in cube, nothing of the pack).
		{
			SceneTestAssets other;
			update = UpdateMirror(gpu, scene, other.Manager);
			CHECK(update.MeshesChecked == mirror.GetMeshSlotCount());
			CHECK(update.MaterialsBuilt == mirror.GetMaterialSlotCount());
			uint32_t otherCubeRecords = 0;
			CHECK(mirror.GetMeshTableEntry(cubeSlot) == ExpectedMeshEntry(*other.Manager, BuiltinAssets::CubeMesh, &otherCubeRecords));
			CHECK(mirror.GetMeshTableEntry(torusSlot) == 0);
			CHECK(mirror.GetMaterialFlags(plainSlot) == defaultFlags);
			CHECK_FALSE(mirror.HasAlphaMaskedMaterials());
			CheckTables(gpu, mirror);
		}

		AssetManager::SetActive(nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
