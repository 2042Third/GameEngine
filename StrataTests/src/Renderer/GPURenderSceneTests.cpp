#include "Renderer/SceneRendererTestUtils.h"

#include "Asset/AssetTestUtils.h"
#include "Perf/StressProject.h"
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

		SceneRenderer batched;
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

		SceneRendererSpecification specification;
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
		CHECK(renderer.GetStats().Instances == 10'000);
		CHECK(renderer.GetStats().Materials == 10'000);
		CHECK(renderer.GetStats().OpaqueDrawCalls <= 4);
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

		SceneRenderer reference;
		reference.SetViewportSize(c_Size, c_Size);
		REQUIRE(reference.Render(scene, camera));
		REQUIRE(reference.Render(scene, camera));
		const Extraction single = Capture(reference);
		REQUIRE(!single.Opaque.empty());
		REQUIRE(!single.Transparent.empty());
		REQUIRE(!single.Shadow.empty());

		ScopedJobSystem jobs(8, 2);
		SceneRenderer parallel;
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
		REQUIRE(renderer.Render(scene, camera));
		const SceneRendererStats& stats = renderer.GetStats();
		CHECK(stats.Instances == 2000);
		const uint32_t distinct = 1 + static_cast<uint32_t>(materials.size() + textures.size()); // The cube mesh, materials, textures
		CHECK(stats.RequestsByVisibility == distinct);
		CHECK(stats.GetAssetCalls <= distinct);
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
			for (int frame = 0; frame < 120 && (frame < 2 || renderer.GetStats().Instances == 0 || renderer.GetStats().PendingAssets > 0); frame++)
				RunFrame(gpu, *manager, renderer, scene, camera);
			REQUIRE(renderer.GetStats().Instances == 1);
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

	TEST_CASE("The materials a frame draws are published with their largest size on screen")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		MaterialProperties tiled = UnlitColor({ 1.0f, 0.0f, 0.0f, 1.0f });
		tiled.UVTiling = glm::vec2(3.0f, -2.0f);
		const AssetHandle near = assets.AddMaterial(tiled);
		const AssetHandle far = assets.AddMaterial(UnlitColor({ 0.0f, 1.0f, 0.0f, 1.0f }));
		const AssetHandle hidden = assets.AddMaterial(UnlitColor({ 0.0f, 0.0f, 1.0f, 1.0f }));
		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::CubeMesh, near, glm::vec3(-1.0f, 0.0f, 0.0f), "Near");
		AddMesh(scene, BuiltinAssets::CubeMesh, far, glm::vec3(2.0f, 0.0f, -10.0f), "Far");
		AddMesh(scene, BuiltinAssets::CubeMesh, hidden, glm::vec3(0.0f, 0.0f, 10.0f), "Behind");

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 4.0f), glm::vec3(0.0f));
		REQUIRE(renderer.Render(scene, camera));
		const std::span<const VisibleMaterialUse> uses = renderer.GetVisibleMaterials();
		REQUIRE(uses.size() == 2);
		const VisibleMaterialUse& nearUse = uses[0].Material == near ? uses[0] : uses[1];
		const VisibleMaterialUse& farUse = uses[0].Material == near ? uses[1] : uses[0];
		CHECK(nearUse.Material == near);
		CHECK(farUse.Material == far);
		CHECK(nearUse.MaxUVScale == doctest::Approx(3.0f));
		CHECK(farUse.MaxUVScale == doctest::Approx(1.0f));
		CHECK(nearUse.PixelSize > farUse.PixelSize * 2.0f);
		// The cube's bounding sphere (radius 0.87) seen from 4.1 units with a 60 degree field of view.
		const float expected = 0.866f / (std::sqrt(17.0f) * std::tan(glm::radians(30.0f))) * static_cast<float>(c_Size);
		CHECK(nearUse.PixelSize == doctest::Approx(expected).epsilon(0.01));
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
			const bool near = SpatialIndex::IntersectsSphere(bounds, outward.Position, specification.PrefetchDistance);
			if ((visible || near) && meshRenderer.Mesh != project->GridMesh)
			{
				allowed.insert(meshRenderer.Material);
				nearObjects++;
			}
		}
		REQUIRE(nearObjects > 0);
		REQUIRE(allowed.size() < project->Materials.size());

		for (int frame = 0; frame < 30; frame++)
			RunFrame(gpu, *manager, renderer, *scene, outward);
		CHECK(renderer.GetStats().Instances == 0);
		CHECK(renderer.GetStats().PrefetchRequests > 0);
		// The terrain is out of view (its bounds are not known before it loads), and so are the other districts.
		CHECK(manager->GetAssetState(project->GridMesh) == AssetState::Unloaded);
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
		CHECK(renderer.GetStats().Instances > 0);
		REQUIRE(manager->GetAssetState(seen) == AssetState::Ready);
		const SceneCamera away = ViewFrom(glm::vec3(-5000.0f, 20.0f, -5000.0f), glm::vec3(-6000.0f, 0.0f, -6000.0f), 5000.0f, 160.0f / 90.0f);
		const uint32_t grace = manager->GetEvictionGraceFrames();
		for (uint32_t frame = 0; frame < grace + 2; frame++)
			RunFrame(gpu, *manager, renderer, *scene, away);
		CHECK(manager->TrimUnused(grace) > 0);
		CHECK(manager->GetAssetState(seen) == AssetState::Unloaded);

		gpu.GetDevice().WaitForIdle();
		AssetManager::SetActive(nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
