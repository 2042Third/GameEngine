#include "Renderer/SceneRendererTestUtils.h"

#include "Asset/AssetTestUtils.h"
#include "Perf/StressProject.h"
#include "Renderer/MeshTestUtils.h"
#include "Renderer/TextureStreamingTestUtils.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Scene/Prefab.h"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

// The scene renderer's persistent render scene (RenderScene): material-free instancing, uploads that follow change,
// deterministic parallel extraction and asset requests that follow visibility.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = 128;

	// `count` cubes on a square grid two units apart, centered on the origin, each with the material
	// materials[i % size] (or the default material).
	std::vector<Entity> AddCubeGrid(Scene& scene, uint32_t count, const std::vector<AssetHandle>& materials)
	{
		const uint32_t side = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(count))));
		const float offset = (static_cast<float>(side) - 1.0f);
		std::vector<Entity> cubes;
		cubes.reserve(count);
		for (uint32_t index = 0; index < count; index++)
		{
			const glm::vec3 position(static_cast<float>(index % side) * 2.0f - offset, 0.5f, static_cast<float>(index / side) * 2.0f - offset);
			cubes.push_back(AddMesh(scene, BuiltinAssets::CubeMesh, materials.empty() ? UUID::Null() : materials[index % materials.size()], position, "Cube"));
		}
		return cubes;
	}

	// A perspective view (60 degree field of view) reaching `farClip` units.
	SceneCamera ViewFrom(const glm::vec3& position, const glm::vec3& target, float farClip, float aspectRatio = 1.0f)
	{
		SceneCamera camera = LookAt(position, target, aspectRatio);
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(60.0f), aspectRatio, 0.1f, farClip);
		camera.Far = farClip;
		return camera;
	}

	ReadbackImage ReadEntityIDs(SceneRenderer& renderer)
	{
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetEntityIDTexture(), image));
		return image;
	}

	// What a frame drew, to compare frames: batches (with their meshes) and draw lists.
	struct Extraction
	{
		std::vector<SceneDrawBatch> Opaque;
		std::vector<SceneDrawBatch> Transparent;
		std::vector<SceneDrawBatch> Shadow;
		std::vector<uint32_t> DrawList;
		std::vector<uint32_t> ShadowDrawList;

		bool operator==(const Extraction&) const = default;
	};

	Extraction Capture(const SceneRenderer& renderer)
	{
		Extraction extraction;
		extraction.Opaque.assign(renderer.GetOpaqueBatches().begin(), renderer.GetOpaqueBatches().end());
		extraction.Transparent.assign(renderer.GetTransparentBatches().begin(), renderer.GetTransparentBatches().end());
		extraction.Shadow.assign(renderer.GetShadowBatches().begin(), renderer.GetShadowBatches().end());
		extraction.DrawList.assign(renderer.GetDrawList().begin(), renderer.GetDrawList().end());
		extraction.ShadowDrawList.assign(renderer.GetShadowDrawList().begin(), renderer.GetShadowDrawList().end());
		return extraction;
	}

	// Frames as an application runs them: the device's frame, the asset manager's update, then the scene.
	void RunFrame(GPUContext& gpu, AssetManagerBase& manager, SceneRenderer& renderer, Scene& scene, const SceneCamera& camera)
	{
		REQUIRE(gpu.GetDevice().BeginFrame());
		Renderer::BeginFrame();
		manager.Update();
		CHECK(renderer.Render(scene, camera));
		gpu.GetDevice().EndFrame();
	}

	// The stored form of a texture of one color (2x2 texels, one level), as packs and the asset cache hold it.
	std::vector<uint8_t> StoreSolidTexture(const glm::u8vec4& color)
	{
		TextureMip level0;
		level0.Width = 2;
		level0.Height = 2;
		for (int texel = 0; texel < 4; texel++)
			AppendPixel(level0.Data, color.r, color.g, color.b, color.a);
		TextureSpecification specification;
		specification.Format = TextureFormat::RGBA8SRGB;
		specification.DebugName = "StoredTexture";
		const Ref<Texture> texture = Texture::Create(specification, { level0 });
		REQUIRE(texture);
		return texture->Serialize();
	}

	std::vector<uint8_t> StoreMaterial(const MaterialProperties& properties)
	{
		const std::string document = Material::Create(properties)->Serialize().dump();
		return std::vector<uint8_t>(document.begin(), document.end());
	}

}

TEST_SUITE("GPU.RenderScene")
{
	TEST_CASE("A thousand materials draw with one opaque call per pass, with the pixels of batches split by material")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		std::vector<AssetHandle> materials;
		for (uint32_t index = 0; index < 1000; index++)
		{
			MaterialProperties properties;
			properties.BaseColor = glm::vec4(static_cast<float>(index % 10) / 9.0f, static_cast<float>(index / 10 % 10) / 9.0f, static_cast<float>(index / 100) / 9.0f, 1.0f);
			properties.Roughness = 0.3f + static_cast<float>(index % 7) * 0.1f;
			materials.push_back(assets.AddMaterial(properties));
		}

		Scene scene;
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)), 3.0f, true);
		AddCubeGrid(scene, 1000, materials);
		const SceneCamera camera = ViewFrom(glm::vec3(0.0f, 80.0f, 70.0f), glm::vec3(0.0f), 500.0f);

		// The CPU path's batches.
		SceneRendererSpecification cpuSpecification;
		cpuSpecification.GPUDriven = GPUDrivenMode::Off;
		SceneRenderer batched(cpuSpecification);
		batched.SetViewportSize(c_Size, c_Size);
		const ReadbackImage instanced = RenderToImage(batched, scene, camera);
		const ReadbackImage instancedIDs = ReadEntityIDs(batched);
		CHECK(batched.GetStats().Instances == 1000);
		CHECK(batched.GetStats().Materials == 1000);
		CHECK(batched.GetStats().OpaqueDrawCalls == 1);
		CHECK(batched.GetStats().DrawCalls == 1);
		// One draw per shadow cascade.
		CHECK(batched.GetStats().ShadowDrawCalls > 0);
		std::set<uint32_t> cascades;
		for (const SceneDrawBatch& batch : batched.GetShadowBatches())
			CHECK(cascades.insert(batch.Cascade).second);

		// The GPU's draws: one in the view, one per cascade, with the same pixels.
		SceneRenderer gpuDriven;
		gpuDriven.SetViewportSize(c_Size, c_Size);
		CHECK(MaxDifference(RenderToImage(gpuDriven, scene, camera), instanced) <= 1);
		CHECK(ReadEntityIDs(gpuDriven).Pixels == instancedIDs.Pixels);
		if (gpuDriven.GetStats().GPUDriven)
		{
			const DrawnCounts drawn = GetDrawnCounts(gpuDriven);
			CHECK(drawn.Instances == 1000);
			CHECK(drawn.Draws == 1);
			CHECK(drawn.ShadowDraws == cascades.size());
		}

		SceneRendererSpecification specification = cpuSpecification;
		specification.BatchAcrossMaterials = false;
		SceneRenderer split(specification);
		split.SetViewportSize(c_Size, c_Size);
		const ReadbackImage perMaterial = RenderToImage(split, scene, camera);
		CHECK(split.GetStats().OpaqueDrawCalls == 1000);
		CHECK(MaxDifference(instanced, perMaterial) == 0);
		const ReadbackImage perMaterialIDs = ReadEntityIDs(split);
		CHECK(instancedIDs.Pixels == perMaterialIDs.Pixels);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Ten thousand cubes with ten thousand unique materials take a few draw calls")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		std::vector<AssetHandle> materials;
		for (uint32_t index = 0; index < 10'000; index++)
		{
			MaterialProperties properties;
			properties.BaseColor = glm::vec4(static_cast<float>(index % 101) / 100.0f, 0.5f, static_cast<float>(index % 37) / 36.0f, 1.0f);
			materials.push_back(assets.AddMaterial(properties));
		}
		Scene scene;
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)), 3.0f, true);
		AddCubeGrid(scene, 10'000, materials);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		REQUIRE(renderer.Render(scene, ViewFrom(glm::vec3(0.0f, 230.0f, 180.0f), glm::vec3(0.0f), 1000.0f)));
		CHECK(GetDrawnCounts(renderer).Instances == 10'000);
		CHECK(renderer.GetStats().Materials == 10'000);
		CHECK(GetDrawnCounts(renderer).Draws <= 4);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Static frames upload nothing, and moving one of 100,000 cubes updates one proxy and uploads one range")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)), 3.0f, true);
		const std::vector<Entity> cubes = AddCubeGrid(scene, 100'000, {});
		const SceneCamera camera = ViewFrom(glm::vec3(0.0f, 600.0f, 450.0f), glm::vec3(0.0f), 3000.0f);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		REQUIRE(renderer.Render(scene, camera));
		const SceneRendererStats first = renderer.GetStats();
		CHECK(first.ProxyUpdates >= 100'000);
		CHECK(first.VisibleProxies == 100'000);
		CHECK(first.InstanceBytesUploaded >= 100'000 * sizeof(RenderData::InstanceData));

		REQUIRE(renderer.Render(scene, camera));
		const SceneRendererStats& still = renderer.GetStats();
		CHECK(still.ProxyUpdates == 0);
		CHECK(still.InstanceBytesUploaded == 0);
		CHECK(still.InstanceUploadRanges == 0);
		CHECK(still.MaterialBytesUploaded == 0);
		CHECK(still.Instances == first.Instances);

		// A small move keeps the cube in its place of the spatial index and of the draw list: only its instance changes.
		Entity moved = cubes[54'321];
		moved.GetComponent<TransformComponent>().Translation.y += 0.01f;
		moved.MarkModified<TransformComponent>();
		REQUIRE(renderer.Render(scene, camera));
		const SceneRendererStats& afterMove = renderer.GetStats();
		CHECK(afterMove.ProxyUpdates == 1);
		CHECK(afterMove.InstanceUploadRanges == 1);
		CHECK(afterMove.InstanceBytesUploaded == sizeof(RenderData::InstanceData));
		CHECK(afterMove.MaterialBytesUploaded == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Extraction on the job system is identical to a single-threaded run, frame after frame")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const Ref<Mesh> lodSphere = MeshFactory::CreateSphere(0.5f, 32, 16);
		REQUIRE(lodSphere);
		const AssetHandle sphere = assets.Add(lodSphere, "Sphere");

		// Opaque, alpha-masked, double-sided and transparent materials over three meshes, some mirrored, with shadows.
		std::vector<AssetHandle> materials;
		for (uint32_t index = 0; index < 24; index++)
		{
			MaterialProperties properties;
			properties.BaseColor = glm::vec4(static_cast<float>(index) / 24.0f, 0.4f, 0.7f, index % 6 == 5 ? 0.5f : 1.0f);
			properties.AlphaMode = index % 6 == 5 ? MaterialAlphaMode::Blend : (index % 6 == 4 ? MaterialAlphaMode::Mask : MaterialAlphaMode::Opaque);
			properties.DoubleSided = index % 3 == 0;
			materials.push_back(assets.AddMaterial(properties));
		}
		Scene scene;
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)), 3.0f, true);
		const AssetHandle meshes[] = { BuiltinAssets::CubeMesh, sphere, BuiltinAssets::QuadMesh };
		for (uint32_t index = 0; index < 30'000; index++)
		{
			const glm::vec3 position(static_cast<float>(index % 173) * 1.5f - 130.0f, static_cast<float>(index % 7) * 0.3f, static_cast<float>(index / 173) * 1.5f - 130.0f);
			Entity entity = AddMesh(scene, meshes[index % 3], materials[(index * 7) % materials.size()], position, "Object");
			if (index % 11 == 0)
				entity.GetComponent<TransformComponent>().Scale = glm::vec3(-1.0f, 1.0f, 1.0f);
		}
		const SceneCamera camera = ViewFrom(glm::vec3(10.0f, 60.0f, 90.0f), glm::vec3(0.0f), 1000.0f);

		// The CPU path's extraction (the GPU path builds its draws on the GPU).
		SceneRendererSpecification specification;
		specification.GPUDriven = GPUDrivenMode::Off;
		SceneRenderer reference(specification);
		reference.SetViewportSize(c_Size, c_Size);
		REQUIRE(reference.Render(scene, camera));
		REQUIRE(reference.Render(scene, camera));
		const Extraction single = Capture(reference);
		REQUIRE(!single.Opaque.empty());
		REQUIRE(!single.Transparent.empty());
		REQUIRE(!single.Shadow.empty());

		ScopedJobSystem jobs(8, 2);
		SceneRenderer parallel(specification);
		parallel.SetViewportSize(c_Size, c_Size);
		for (int repetition = 0; repetition < 20; repetition++)
		{
			CAPTURE(repetition);
			REQUIRE(parallel.Render(scene, camera));
			CHECK(Capture(parallel) == single);
			// Lets the device retire the frame, as an application's frames do (its constant buffer versions are limited).
			gpu.GetDevice().WaitForIdle();
			gpu.GetNvrhiDevice()->runGarbageCollection();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Assets are looked up once per frame: no more lookups than distinct meshes, materials and textures drawn")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		std::vector<AssetHandle> textures;
		for (uint8_t index = 0; index < 4; index++)
			textures.push_back(assets.AddSolidTexture({ static_cast<uint8_t>(60 * index), 200, 90, 255 }, TextureFormat::RGBA8SRGB));
		std::vector<AssetHandle> materials;
		for (uint32_t index = 0; index < 10; index++)
		{
			MaterialProperties properties;
			properties.BaseColorMap = textures[index % textures.size()];
			materials.push_back(assets.AddMaterial(properties));
		}
		Scene scene;
		AddNeutralPostProcess(scene);
		AddCubeGrid(scene, 2000, materials);
		const SceneCamera camera = ViewFrom(glm::vec3(0.0f, 110.0f, 90.0f), glm::vec3(0.0f), 500.0f);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		REQUIRE(renderer.Render(scene, camera));
		// Counted by the asset manager, so that renderer code asking it for assets past the resolve cache counts too.
		const uint64_t callsBefore = assets.Manager->GetStats().GetAssetCalls;
		REQUIRE(renderer.Render(scene, camera));
		const uint64_t calls = assets.Manager->GetStats().GetAssetCalls - callsBefore;
		const SceneRendererStats& stats = renderer.GetStats();
		CHECK(GetDrawnCounts(renderer).Instances == 2000);
		const uint32_t distinct = 1 + static_cast<uint32_t>(materials.size() + textures.size()); // The cube mesh, materials, textures
		CHECK(stats.RequestsByVisibility == distinct);
		CHECK(calls <= distinct);
		CHECK(stats.GetAssetCalls == calls);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Frames that use the same materials take their data over from the frame before; what changed is resolved again")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		// Two materials with textures of their own, read from a pack: loaded objects of a pack can be replaced, as editors
		// publish the documents they edit.
		constexpr AssetHandle c_RedTexture(0xD100);
		constexpr AssetHandle c_RedMaterial(0xD101);
		constexpr AssetHandle c_BlueTexture(0xD102);
		constexpr AssetHandle c_BlueMaterial(0xD103);
		MaterialProperties red = UnlitColor(glm::vec4(1.0f));
		red.BaseColorMap = c_RedTexture;
		MaterialProperties blue = UnlitColor(glm::vec4(1.0f));
		blue.BaseColorMap = c_BlueTexture;
		const std::filesystem::path pack = WriteStreamingPack({
			StreamingPackAsset { c_RedTexture, AssetType::Texture, "Textures/Red.png", CookSolidTexture(4, { 255, 0, 0, 255 }) },
			StreamingPackAsset { c_RedMaterial, AssetType::Material, "Materials/Red.stmat", SerializeMaterial(red) },
			StreamingPackAsset { c_BlueTexture, AssetType::Texture, "Textures/Blue.png", CookSolidTexture(4, { 0, 0, 255, 255 }) },
			StreamingPackAsset { c_BlueMaterial, AssetType::Material, "Materials/Blue.stmat", SerializeMaterial(blue) } });
		std::string error;
		const Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(pack, &error);
		REQUIRE_MESSAGE(manager, error);
		AssetManager::SetActive(manager);

		Scene scene;
		AddNeutralPostProcess(scene);
		const glm::vec3 leftPosition(-1.2f, 0.0f, 0.0f);
		const glm::vec3 rightPosition(1.2f, 0.0f, 0.0f);
		AddMesh(scene, BuiltinAssets::CubeMesh, c_RedMaterial, leftPosition, "Left");
		Entity right = AddMesh(scene, BuiltinAssets::CubeMesh, c_BlueMaterial, rightPosition, "Right");
		// The materials each frame uses are those the CPU's marks find in its view: visibility feedback would tell of a
		// material no longer used frames later (GPU.SceneRenderer.Feedback tests that latency).
		SceneRendererSpecification specification;
		specification.VisibilityFeedback = false;
		SceneRenderer renderer(specification);
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f));
		const glm::uvec2 left = ProjectToPixel(camera, leftPosition + glm::vec3(0.0f, 0.0f, 0.5f), c_Size);
		const glm::uvec2 rightPixel = ProjectToPixel(camera, rightPosition + glm::vec3(0.0f, 0.0f, 0.5f), c_Size);
		uint64_t calls = 0; // The asset manager's lookups during the latest frame
		const auto frame = [&](const SceneCamera& view)
		{
			manager->Update();
			const uint64_t before = manager->GetStats().GetAssetCalls;
			ReadbackImage image = RenderToImage(renderer, scene, view);
			calls = manager->GetStats().GetAssetCalls - before;
			return image;
		};
		const auto settle = [&]()
		{
			for (int attempt = 0; attempt < 200; attempt++)
			{
				frame(camera);
				if (renderer.GetStats().PendingAssets == 0 && !manager->HasPendingLoads())
					return;
			}
			FAIL("The scene's assets did not load");
		};
		settle();
		frame(camera);

		// A still frame: the materials' data is the frame before's, their assets looked up in one call; the picture and the
		// statistics are those of a renderer that resolves them one by one (a new one, which has no frame before).
		SceneRenderer fresh;
		fresh.SetViewportSize(c_Size, c_Size);
		const ReadbackImage resolved = RenderToImage(fresh, scene, camera);
		const SceneRendererStats resolvedStats = fresh.GetStats();
		REQUIRE(resolvedStats.ReusedMaterials == 0);
		const ReadbackImage still = frame(camera);
		const SceneRendererStats& stats = renderer.GetStats();
		CHECK(stats.ReusedMaterials == 2);
		CHECK(stats.Materials == 2);
		CHECK(MaxDifference(still, resolved) == 0);
		CHECK(GetPixelRGBA8(still, left.x, left.y).r > 200);
		CHECK(GetPixelRGBA8(still, rightPixel.x, rightPixel.y).b > 200);
		CHECK(stats.GetAssetCalls == calls);
		CHECK(stats.GetAssetCalls == resolvedStats.GetAssetCalls);
		CHECK(stats.RequestsByVisibility == resolvedStats.RequestsByVisibility);
		CHECK(stats.PendingAssets == 0);
		CHECK(stats.MaterialBytesUploaded == 0);

		// The view moved: the same materials, used the same ways.
		frame(LookAt(glm::vec3(0.3f, 0.2f, 5.0f), glm::vec3(0.0f)));
		CHECK(renderer.GetStats().ReusedMaterials == 2);

		// A material replaced (an edit published): resolved again at once, then taken over again.
		MaterialProperties edited = UnlitColor(glm::vec4(1.0f));
		edited.BaseColorMap = c_BlueTexture;
		REQUIRE(manager->ReplaceLoadedAsset(c_RedMaterial, Material::Create(edited)));
		ReadbackImage image = frame(camera);
		CHECK(renderer.GetStats().ReusedMaterials == 0);
		CHECK(GetPixelRGBA8(image, left.x, left.y).b > 200);
		CHECK(GetPixelRGBA8(image, left.x, left.y).r < 50);
		CHECK(renderer.GetStats().MaterialBytesUploaded > 0);
		image = frame(camera);
		CHECK(renderer.GetStats().ReusedMaterials == 2);
		CHECK(GetPixelRGBA8(image, left.x, left.y).b > 200);

		// A texture unloaded: the materials that show it are resolved again (the loading texture stands in), until it is back.
		manager->UnloadAsset(c_BlueTexture);
		image = frame(camera);
		CHECK(renderer.GetStats().ReusedMaterials == 0);
		CHECK(GetPixelRGBA8(image, rightPixel.x, rightPixel.y).r > 200); // White while it loads
		settle();
		image = frame(camera);
		CHECK(renderer.GetStats().ReusedMaterials == 2);
		CHECK(GetPixelRGBA8(image, rightPixel.x, rightPixel.y).b > 200);
		CHECK(GetPixelRGBA8(image, rightPixel.x, rightPixel.y).r < 50);

		// A material no longer used: the frame resolves the other one again, the next takes it over.
		right.SetActive(false);
		frame(camera);
		CHECK(renderer.GetStats().ReusedMaterials == 0);
		CHECK(renderer.GetStats().Materials == 1);
		frame(camera);
		CHECK(renderer.GetStats().ReusedMaterials == 1);
		CHECK(renderer.GetStats().Materials == 1);

		AssetManager::SetActive(nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Visibility sets the priority of requests: High above 5% of the view, Normal below it and for unknown bounds, Low within the prefetch distance")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ScopedJobSystem jobs(2, 2);
		const Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		// A load that takes all the bytes loads may have in flight waits at the gate: the frame's requests stay queued.
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.InFlightBytes = 1;
		manager->SetResidencyBudgets(budgets);
		auto holdLoads = [&](uint64_t handle)
		{
			const AssetHandle blocker = manager->AddStored(handle, AssetType::Mesh, MeshFactory::CreateCube()->Serialize());
			manager->CloseGate();
			manager->RequestLoad(blocker, AssetPriority::High);
			REQUIRE(manager->WaitForWaitingReads(1));
		};
		auto queued = [&](AssetPriority priority) { return manager->GetStats().QueuedLoads[static_cast<size_t>(priority)]; };

		Scene scene;
		AddNeutralPostProcess(scene);
		std::vector<AssetHandle> stored;
		auto addObject = [&](uint64_t handle, const glm::vec3& position, const std::string& name)
		{
			const AssetHandle mesh = manager->AddStored(handle, AssetType::Mesh, MeshFactory::CreateCube()->Serialize());
			const AssetHandle material = manager->AddStored(handle + 1, AssetType::Material, StoreMaterial(UnlitColor({ 1.0f, 0.5f, 0.0f, 1.0f })));
			AddMesh(scene, mesh, material, position, name);
			stored.push_back(mesh);
			stored.push_back(material);
		};
		addObject(0x6E31, glm::vec3(0.0f), "Near");                  // A third of the view
		addObject(0x6E33, glm::vec3(0.0f, 0.0f, -70.0f), "Far");     // Under 5% of the view
		addObject(0x6E35, glm::vec3(0.0f, 0.0f, 20.0f), "Behind");   // Out of view, within the prefetch distance
		AssetManager::SetActive(manager);
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 8.0f), glm::vec3(0.0f));

		// The meshes never loaded: their bounds are unknown, so each is requested as if in view, at Normal priority, or at
		// High where its placeholder bounds are large in view. The materials follow their proxies' placeholders.
		holdLoads(0xB10C);
		RunFrame(gpu, *manager, renderer, scene, camera);
		CHECK(renderer.GetStats().MeshesWithoutBounds == 3);
		CHECK(queued(AssetPriority::High) == 2);   // Near's mesh and material
		CHECK(queued(AssetPriority::Normal) == 3); // Far's mesh and material, Behind's mesh
		CHECK(queued(AssetPriority::Low) == 1);    // Behind's material
		// The meshes, the materials in view, and the default material, which stands in for materials still loading.
		CHECK(renderer.GetStats().RequestsByVisibility == 6);
		CHECK(renderer.GetStats().PrefetchRequests == 1);

		// Loaded once, the meshes' layouts are known; unloaded again, they are culled with their real bounds.
		manager->OpenGate();
		REQUIRE(manager->WaitForPendingLoads());
		RunFrame(gpu, *manager, renderer, scene, camera);
		REQUIRE(RenderScene::Get(scene).GetMeshesWithoutBounds().empty());
		gpu.GetDevice().WaitForIdle();
		for (const AssetHandle handle : stored)
			manager->UnloadAsset(handle);

		holdLoads(0xB10D);
		RunFrame(gpu, *manager, renderer, scene, camera);
		CHECK(renderer.GetStats().MeshesWithoutBounds == 0);
		CHECK(queued(AssetPriority::High) == 2);   // Near's mesh and material
		CHECK(queued(AssetPriority::Normal) == 2); // Far's mesh and material
		CHECK(queued(AssetPriority::Low) == 2);    // Behind's mesh and material, prefetched
		CHECK(renderer.GetStats().RequestsByVisibility == 5);
		CHECK(renderer.GetStats().PrefetchRequests == 2);
		CHECK(manager->GetAssetState(stored[4]) == AssetState::Loading);

		manager->OpenGate();
		REQUIRE(manager->WaitForPendingLoads());
		gpu.GetDevice().WaitForIdle();
		AssetManager::SetActive(nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("An idle renderer keeps nothing: what it drew last can be evicted, and its asset manager can go")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Scene scene;
		AddNeutralPostProcess(scene);
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.0f, 4.0f), glm::vec3(0.0f));
		std::weak_ptr<AssetManagerBase> closed;
		{
			const Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
			closed = manager;
			const AssetHandle texture = manager->AddStored(0x7E01, AssetType::Texture, StoreSolidTexture({ 200, 80, 40, 255 }));
			MaterialProperties properties;
			properties.BaseColorMap = texture;
			const AssetHandle material = manager->AddStored(0x3A01, AssetType::Material, StoreMaterial(properties));
			const AssetHandle mesh = manager->AddStored(0x3E01, AssetType::Mesh, MeshFactory::CreateCube()->Serialize());
			AssetManager::SetActive(manager);
			AddMesh(scene, mesh, material, glm::vec3(0.0f), "Cube");

			// Frames until the cube shows with its texture, as a capture renders them; then the renderer idles.
			for (int frame = 0; frame < 120 && (frame < 2 || GetDrawnCounts(renderer).Instances == 0 || renderer.GetStats().PendingAssets > 0); frame++)
				RunFrame(gpu, *manager, renderer, scene, camera);
			REQUIRE(GetDrawnCounts(renderer).Instances == 1);
			REQUIRE(manager->GetAssetState(texture) == AssetState::Ready);

			// The manager's frames go on without the renderer: what it drew goes like anything else no longer used.
			const uint32_t grace = manager->GetEvictionGraceFrames();
			for (uint32_t frame = 0; frame <= grace; frame++)
				manager->Update();
			CHECK(manager->TrimUnused(grace) == 3);
			CHECK(manager->GetAssetState(texture) == AssetState::Unloaded);
			CHECK(manager->GetAssetState(material) == AssetState::Unloaded);
			CHECK(manager->GetAssetState(mesh) == AssetState::Unloaded);
			gpu.GetDevice().WaitForIdle();
			AssetManager::SetActive(nullptr);
		}
		// The renderer holds no reference to the manager either: it went with its owner, as a closed project's does.
		CHECK(closed.expired());
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A large mesh whose origin is out of view and beyond the prefetch distance is requested while its bounds are unknown, then drawn")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle ground = manager->AddStored(0x6E01, AssetType::Mesh, MeshFactory::CreatePlane(1000.0f)->Serialize());
		AssetManager::SetActive(manager);
		Scene scene;
		AddNeutralPostProcess(scene);
		// Ground 1,000 units wide whose origin lies behind the camera, far away: only its corner is under the camera.
		const glm::vec3 origin(400.0f, 0.0f, 400.0f);
		const Entity entity = AddMesh(scene, ground, UUID::Null(), origin, "Ground");
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = ViewFrom(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(-20.0f, 0.0f, -20.0f), 500.0f);
		REQUIRE(glm::distance(camera.Position, origin) > renderer.GetSpecification().PrefetchDistance * 2.0f);

		// Its proxy's placeholder bounds are out of view and beyond the prefetch distance, but they say nothing about the
		// mesh: it is requested as if in view.
		RunFrame(gpu, *manager, renderer, scene, camera);
		CHECK(renderer.GetStats().VisibleProxies == 0);
		CHECK(renderer.GetStats().MeshesWithoutBounds == 1);
		CHECK(renderer.GetStats().RequestsByVisibility == 1);
		CHECK(manager->GetAssetState(ground) != AssetState::Unloaded);
		for (int frame = 0; frame < 120 && GetDrawnCounts(renderer).Instances == 0; frame++)
			RunFrame(gpu, *manager, renderer, scene, camera);
		CHECK(GetDrawnCounts(renderer).Instances == 1);
		CHECK(renderer.GetStats().MeshesWithoutBounds == 0);
		CHECK(renderer.GetEntityAt(scene, c_Size / 2, c_Size - 4) == entity);
		gpu.GetDevice().WaitForIdle();
		AssetManager::SetActive(nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Meshes with recorded bounds are culled with them before they load: requested where those reach into view, not elsewhere")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		// Both meshes' bounds recorded, as the editor's imports and a game's pack record them.
		const Ref<Mesh> plane = MeshFactory::CreatePlane(1000.0f);
		const AssetHandle ground = manager->AddStored(0x6E41, AssetType::Mesh, plane->Serialize());
		manager->SetDependencies(ground, {}, plane->GetBounds());
		const Ref<Mesh> cube = MeshFactory::CreateCube();
		const AssetHandle box = manager->AddStored(0x6E42, AssetType::Mesh, cube->Serialize());
		manager->SetDependencies(box, {}, cube->GetBounds());
		AssetManager::SetActive(manager);
		Scene scene;
		AddNeutralPostProcess(scene);
		// The ground's origin lies behind the camera, far away, but its bounds reach under the camera; the box lies behind the
		// camera beyond the prefetch distance.
		const glm::vec3 boxPosition(300.0f, 0.0f, 300.0f);
		const Entity groundEntity = AddMesh(scene, ground, UUID::Null(), glm::vec3(400.0f, 0.0f, 400.0f), "Ground");
		AddMesh(scene, box, UUID::Null(), boxPosition, "Box");
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = ViewFrom(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(-20.0f, 0.0f, -20.0f), 500.0f);
		REQUIRE(glm::distance(camera.Position, boxPosition) > renderer.GetSpecification().PrefetchDistance * 2.0f);

		// Neither has loaded, yet both are placed where they are: the ground is in view and requested, the box is culled and
		// not requested (without its record it would be requested as if in view).
		RunFrame(gpu, *manager, renderer, scene, camera);
		CHECK(renderer.GetStats().MeshesWithoutBounds == 0);
		CHECK(renderer.GetStats().VisibleProxies == 1);
		CHECK(manager->GetAssetState(ground) != AssetState::Unloaded);
		CHECK(manager->GetAssetState(box) == AssetState::Unloaded);
		for (int frame = 0; frame < 120 && GetDrawnCounts(renderer).Instances == 0; frame++)
			RunFrame(gpu, *manager, renderer, scene, camera);
		CHECK(GetDrawnCounts(renderer).Instances == 1);
		CHECK(renderer.GetEntityAt(scene, c_Size / 2, c_Size - 4) == groundEntity);
		CHECK(manager->GetAssetState(box) == AssetState::Unloaded);
		REQUIRE(manager->WaitForPendingLoads());
		gpu.GetDevice().WaitForIdle();
		AssetManager::SetActive(nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Meshes of inactive entities are not requested while their bounds are unknown")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle mesh = manager->AddStored(0x6E05, AssetType::Mesh, MeshFactory::CreateCube()->Serialize());
		AssetManager::SetActive(manager);
		Scene scene;
		AddNeutralPostProcess(scene);
		Entity entity = AddMesh(scene, mesh, UUID::Null(), glm::vec3(0.0f), "Hidden");
		entity.SetActive(false);
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.0f, 4.0f), glm::vec3(0.0f));
		RunFrame(gpu, *manager, renderer, scene, camera);
		CHECK(renderer.GetStats().MeshesWithoutBounds == 0);
		CHECK(manager->GetAssetState(mesh) == AssetState::Unloaded);

		entity.SetActive(true);
		RunFrame(gpu, *manager, renderer, scene, camera);
		CHECK(renderer.GetStats().MeshesWithoutBounds == 1);
		CHECK(manager->GetAssetState(mesh) != AssetState::Unloaded);
		REQUIRE(manager->WaitForPendingLoads());
		gpu.GetDevice().WaitForIdle();
		AssetManager::SetActive(nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A mesh reloaded with larger bounds is culled with them, also when its old bounds were out of view")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle mesh = manager->AddStored(0x6E11, AssetType::Mesh, MeshFactory::CreateCube(0.1f)->Serialize());
		AssetManager::SetActive(manager);
		Scene scene;
		AddNeutralPostProcess(scene);
		const Entity entity = AddMesh(scene, mesh, UUID::Null(), glm::vec3(0.0f), "Platform");
		SceneRendererSpecification specification;
		specification.PrefetchDistance = 0.0f; // Only the view, and the mesh's unknown bounds at first, load the mesh
		SceneRenderer renderer(specification);
		renderer.SetViewportSize(c_Size, c_Size);
		// Looking past the small mesh, which stays out of view.
		const SceneCamera camera = ViewFrom(glm::vec3(0.0f, 0.0f, 20.0f), glm::vec3(20.0f, 0.0f, 0.0f), 100.0f);
		RenderScene& renderScene = RenderScene::Get(scene);
		for (int frame = 0; frame < 10; frame++)
			RunFrame(gpu, *manager, renderer, scene, camera);
		REQUIRE(renderScene.GetMeshesWithoutBounds().empty());
		REQUIRE(manager->GetAssetState(mesh) == AssetState::Ready);
		CHECK(GetDrawnCounts(renderer).Instances == 0);

		// Reimported 300 times larger: it fills the view now, though its old bounds never entered it.
		manager->SetStoredData(mesh, MeshFactory::CreateCube(30.0f)->Serialize());
		manager->ReloadAsset(mesh);
		for (int frame = 0; frame < 60 && GetDrawnCounts(renderer).Instances == 0; frame++)
			RunFrame(gpu, *manager, renderer, scene, camera);
		CHECK(GetDrawnCounts(renderer).Instances == 1);
		CHECK(renderScene.GetWorldBounds(renderScene.FindProxy(entity.GetHandle())).Max.x == doctest::Approx(15.0f));
		gpu.GetDevice().WaitForIdle();
		AssetManager::SetActive(nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A mesh evicted, then reimported larger, is drawn where only its new bounds are in view")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle mesh = manager->AddStored(0x6E15, AssetType::Mesh, MeshFactory::CreateCube(0.1f)->Serialize());
		AssetManager::SetActive(manager);
		Scene scene;
		AddNeutralPostProcess(scene);
		const Entity entity = AddMesh(scene, mesh, UUID::Null(), glm::vec3(0.0f), "Platform");
		SceneRendererSpecification specification;
		specification.PrefetchDistance = 0.0f; // Only the view, and the mesh's unknown bounds, load the mesh
		SceneRenderer renderer(specification);
		renderer.SetViewportSize(c_Size, c_Size);
		// Looking past the small mesh, which stays out of view.
		const SceneCamera camera = ViewFrom(glm::vec3(0.0f, 0.0f, 20.0f), glm::vec3(20.0f, 0.0f, 0.0f), 100.0f);
		RenderScene& renderScene = RenderScene::Get(scene);
		for (int frame = 0; frame < 10; frame++)
			RunFrame(gpu, *manager, renderer, scene, camera);
		REQUIRE(renderScene.GetMeshesWithoutBounds().empty());
		REQUIRE(manager->GetAssetState(mesh) == AssetState::Ready);

		// Nothing requests it out of view: it is evicted, and its proxy keeps the layout without loading it again.
		const uint32_t grace = manager->GetEvictionGraceFrames();
		for (uint32_t frame = 0; frame <= grace; frame++)
			RunFrame(gpu, *manager, renderer, scene, camera);
		CHECK(manager->TrimUnused(grace) == 1);
		RunFrame(gpu, *manager, renderer, scene, camera);
		CHECK(renderer.GetStats().MeshesWithoutBounds == 0);
		CHECK(manager->GetAssetState(mesh) == AssetState::Unloaded);

		// Reimported 300 times larger while it is not loaded: it fills the view now, though its old bounds never entered it.
		manager->SetStoredData(mesh, MeshFactory::CreateCube(30.0f)->Serialize());
		manager->ReloadAsset(mesh);
		for (int frame = 0; frame < 60 && GetDrawnCounts(renderer).Instances == 0; frame++)
			RunFrame(gpu, *manager, renderer, scene, camera);
		CHECK(GetDrawnCounts(renderer).Instances == 1);
		CHECK(renderScene.GetWorldBounds(renderScene.FindProxy(entity.GetHandle())).Max.x == doctest::Approx(15.0f));
		CHECK(renderer.GetEntityAt(scene, c_Size / 2, c_Size / 2) == entity);
		gpu.GetDevice().WaitForIdle();
		AssetManager::SetActive(nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A scene not rendered while its mesh gained a submesh draws both when it is rendered again")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle mesh = manager->AddStored(0x6E21, AssetType::Mesh, CreatePartedCube(2.0f, 1)->Serialize());
		AssetManager::SetActive(manager);
		// The edited scene and, as in play mode, another scene the same renderer draws meanwhile.
		Scene edited;
		AddNeutralPostProcess(edited);
		AddMesh(edited, mesh, UUID::Null(), glm::vec3(0.0f), "Model");
		Scene playing;
		AddNeutralPostProcess(playing);
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 2.0f, 6.0f), glm::vec3(0.0f));
		for (int frame = 0; frame < 60 && GetDrawnCounts(renderer).Instances == 0; frame++)
			RunFrame(gpu, *manager, renderer, edited, camera);
		REQUIRE(GetDrawnCounts(renderer).Instances == 1);

		manager->SetStoredData(mesh, CreatePartedCube(2.0f, 2)->Serialize());
		manager->ReloadAsset(mesh);
		for (int frame = 0; frame < 5; frame++)
			RunFrame(gpu, *manager, renderer, playing, camera);
		REQUIRE(manager->GetAssetState(mesh) == AssetState::Ready);

		RunFrame(gpu, *manager, renderer, edited, camera);
		CHECK(GetDrawnCounts(renderer).Instances == 2);
		gpu.GetDevice().WaitForIdle();
		AssetManager::SetActive(nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The materials a frame draws are published with their largest size on screen")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		MaterialProperties tiled = UnlitColor({ 1.0f, 0.0f, 0.0f, 1.0f });
		tiled.UVTiling = glm::vec2(3.0f, -2.0f);
		const AssetHandle nearMaterial = assets.AddMaterial(tiled);
		const AssetHandle farMaterial = assets.AddMaterial(UnlitColor({ 0.0f, 1.0f, 0.0f, 1.0f }));
		const AssetHandle hidden = assets.AddMaterial(UnlitColor({ 0.0f, 0.0f, 1.0f, 1.0f }));
		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::CubeMesh, nearMaterial, glm::vec3(-1.0f, 0.0f, 0.0f), "Near");
		AddMesh(scene, BuiltinAssets::CubeMesh, farMaterial, glm::vec3(2.0f, 0.0f, -10.0f), "Far");
		AddMesh(scene, BuiltinAssets::CubeMesh, hidden, glm::vec3(0.0f, 0.0f, 10.0f), "Behind");

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 4.0f), glm::vec3(0.0f));
		REQUIRE(renderer.Render(scene, camera));
		const std::span<const VisibleMaterialUse> uses = renderer.GetVisibleMaterials();
		REQUIRE(uses.size() == 2);
		const VisibleMaterialUse& nearUse = uses[0].Material == nearMaterial ? uses[0] : uses[1];
		const VisibleMaterialUse& farUse = uses[0].Material == nearMaterial ? uses[1] : uses[0];
		CHECK(nearUse.Material == nearMaterial);
		CHECK(farUse.Material == farMaterial);
		CHECK(nearUse.MaxUVScale == doctest::Approx(3.0f));
		CHECK(farUse.MaxUVScale == doctest::Approx(1.0f));
		CHECK(nearUse.PixelSize > farUse.PixelSize * 2.0f);
		// The cube's bounding sphere (radius 0.87) seen from 4.1 units with a 60 degree field of view.
		const float expected = 0.866f / (std::sqrt(17.0f) * std::tan(glm::radians(30.0f))) * static_cast<float>(c_Size);
		CHECK(nearUse.PixelSize == doctest::Approx(expected).epsilon(0.01));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The loaded texture maps a frame draws are published with their material's use; materials without them publish none")
	{
		// What the texture streamer reads (TextureStreamer::ReportView): the renderer resolved the maps for the frame, so the
		// streamer looks nothing up, and draws of materials without texture maps work out nothing for streaming.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const AssetHandle texture = assets.AddSolidTexture({ 200, 100, 50, 255 }, TextureFormat::RGBA8SRGB);
		MaterialProperties textured = UnlitColor({ 1.0f, 1.0f, 1.0f, 1.0f });
		textured.BaseColorMap = texture;
		textured.UVTiling = glm::vec2(2.0f, 1.0f);
		const AssetHandle texturedMaterial = assets.AddMaterial(textured);
		const AssetHandle plain = assets.AddMaterial(UnlitColor({ 0.0f, 1.0f, 0.0f, 1.0f }));
		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::CubeMesh, texturedMaterial, glm::vec3(-1.0f, 0.0f, 0.0f), "Textured");
		AddMesh(scene, BuiltinAssets::CubeMesh, plain, glm::vec3(1.5f, 0.0f, 0.0f), "Plain");

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 4.0f), glm::vec3(0.0f));
		REQUIRE(renderer.Render(scene, camera));
		const std::span<const VisibleMaterialUse> materials = renderer.GetVisibleMaterials();
		REQUIRE(materials.size() == 2);
		const VisibleMaterialUse& texturedUse = materials[0].Material == texturedMaterial ? materials[0] : materials[1];
		const VisibleMaterialUse& plainUse = materials[0].Material == texturedMaterial ? materials[1] : materials[0];
		REQUIRE(texturedUse.Material == texturedMaterial);
		REQUIRE(plainUse.Material == plain);
		CHECK(texturedUse.UVPixels > 0.0f);
		CHECK(plainUse.UVPixels == 0.0f);

		const std::span<const VisibleTextureUse> textures = renderer.GetVisibleTextures();
		REQUIRE(textures.size() == 1);
		CHECK(textures[0].Texture == texture);
		CHECK(textures[0].Size == 2);
		CHECK(textures[0].MipCount == 1);
		CHECK(textures[0].PixelSize == texturedUse.PixelSize);
		CHECK(textures[0].UVScale == doctest::Approx(2.0f));
		CHECK(textures[0].UVPixels == texturedUse.UVPixels);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Requests follow visibility: the stress world seen from an empty corner requests nothing beyond the view and the prefetch distance")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ScopedJobSystem jobs(4, 2);

		StressProjectSpec spec;
		spec.Textures2k = 4;
		spec.Textures4k = 1;
		spec.Materials = 9;
		spec.Entities = 400;
		spec.GridResolution = 33;
		spec.SmallTextureSize = 64;
		spec.LargeTextureSize = 128;
		const std::filesystem::path directory = CreateTemporaryDirectory("RenderSceneRequests");
		std::string error;
		const std::optional<StressProject> project = WriteStressProject(directory / "Stress", spec, error);
		REQUIRE_MESSAGE(project, error);
		REQUIRE_MESSAGE(BuildStressPack(*project, directory / "Stress.stpak", error), error);
		const Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(directory / "Stress.stpak", &error);
		REQUIRE_MESSAGE(manager, error);
		AssetManager::SetActive(manager);
		Ref<SceneAsset> sceneAsset = AssetManager::LoadAssetSync<SceneAsset>(project->WorldScene);
		REQUIRE(sceneAsset);
		const Ref<Scene> scene = sceneAsset->CreateScene(&error);
		REQUIRE_MESSAGE(scene, error);
		sceneAsset = nullptr;

		SceneRendererSpecification specification;
		specification.PrefetchDistance = 100.0f;
		SceneRenderer renderer(specification);
		renderer.SetViewportSize(160, 90);
		// From just inside the world's corner, looking out of the world: the objects of the corner are behind the camera.
		const glm::vec3 corner(-c_StressWorldSize * 0.5f + 10.0f, 20.0f, -c_StressWorldSize * 0.5f + 10.0f);
		const SceneCamera outward = ViewFrom(corner, corner + glm::vec3(-100.0f, -10.0f, -100.0f), 5000.0f, 160.0f / 90.0f);

		// What the view and the prefetch distance contain, by brute force over the scene.
		scene->UpdateWorldTransforms();
		const Frustum frustum(outward.Projection * outward.View);
		std::set<AssetHandle> allowed;
		uint32_t nearObjects = 0;
		for (auto [entity, meshRenderer, world] : scene->GetRegistry().view<MeshRendererComponent, WorldTransformComponent>().each())
		{
			const AABB bounds = AABB(glm::vec3(-0.5f), glm::vec3(0.5f)).Transformed(world.Matrix);
			const bool visible = frustum.IsAABBVisible(bounds);
			const bool nearby = SpatialIndex::IntersectsSphere(bounds, outward.Position, specification.PrefetchDistance);
			if ((visible || nearby) && meshRenderer.Mesh != project->GridMesh)
			{
				allowed.insert(meshRenderer.Material);
				nearObjects++;
			}
		}
		REQUIRE(nearObjects > 0);
		REQUIRE(allowed.size() < project->Materials.size());

		RunFrame(gpu, *manager, renderer, *scene, outward);
		// The pack recorded the terrain mesh's bounds: before the mesh loads, its proxy has its real bounds, which lie under the
		// camera, within the prefetch distance, so it is requested. It is the only thing the view may show.
		RenderScene& renderScene = RenderScene::Get(*scene);
		CHECK(renderScene.GetMeshesWithoutBounds().empty());
		CHECK(renderer.GetStats().MeshesWithoutBounds == 0);
		CHECK(manager->GetAssetState(project->GridMesh) != AssetState::Unloaded);
		const uint32_t terrain = renderScene.FindProxy(scene->FindEntityByName("Terrain").GetHandle());
		REQUIRE(terrain != RenderScene::c_InvalidIndex);
		CHECK((renderScene.GetProxyFlags(terrain) & RenderProxyFlags::MeshBounds) != 0);
		for (int frame = 1; frame < 30; frame++)
			RunFrame(gpu, *manager, renderer, *scene, outward);
		CHECK(renderer.GetStats().PrefetchRequests > 0);
		CHECK(renderer.GetStats().MeshesWithoutBounds == 0);
		const AABB& terrainBounds = renderScene.GetWorldBounds(terrain);
		CHECK(terrainBounds.Min.x == doctest::Approx(-c_StressWorldSize * 0.5f));
		CHECK(terrainBounds.Max.z == doctest::Approx(c_StressWorldSize * 0.5f));
		CHECK(SpatialIndex::IntersectsSphere(terrainBounds, outward.Position, specification.PrefetchDistance));
		CHECK(GetDrawnCounts(renderer).Instances == (frustum.IsAABBVisible(terrainBounds) ? 1u : 0u));
		// The other districts' materials are out of view and beyond the prefetch distance.
		for (const AssetHandle material : project->Materials)
		{
			CAPTURE(material);
			CHECK((manager->GetAssetState(material) != AssetState::Unloaded) == allowed.contains(material));
		}

		// Seen, then left behind: what the view no longer uses becomes evictable (the renderer holds no references).
		const SceneCamera inward = ViewFrom(corner, glm::vec3(0.0f, 0.0f, 0.0f), 5000.0f, 160.0f / 90.0f);
		const AssetHandle seen = project->Materials[0];
		for (int frame = 0; frame < 600 && (frame < 30 || manager->GetAssetState(seen) != AssetState::Ready); frame++)
			RunFrame(gpu, *manager, renderer, *scene, inward);
		CHECK(GetDrawnCounts(renderer).Instances > 0);
		REQUIRE(manager->GetAssetState(seen) == AssetState::Ready);
		const SceneCamera away = ViewFrom(glm::vec3(-5000.0f, 20.0f, -5000.0f), glm::vec3(-6000.0f, 0.0f, -6000.0f), 5000.0f, 160.0f / 90.0f);
		const uint32_t grace = manager->GetEvictionGraceFrames();
		for (uint32_t frame = 0; frame < grace + 2; frame++)
		{
			// Nothing in view or near and every mesh's bounds known: a frame asks the asset manager for nothing.
			const uint64_t calls = manager->GetStats().GetAssetCalls;
			RunFrame(gpu, *manager, renderer, *scene, away);
			CHECK(manager->GetStats().GetAssetCalls == calls);
			CHECK(renderer.GetStats().RequestsByVisibility == 0);
		}
		CHECK(manager->TrimUnused(grace) > 0);
		CHECK(manager->GetAssetState(seen) == AssetState::Unloaded);

		gpu.GetDevice().WaitForIdle();
		AssetManager::SetActive(nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
