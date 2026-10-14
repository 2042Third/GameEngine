#include "Renderer/SceneRendererTestUtils.h"

#include "Animation/SkinnedMeshTestUtils.h"
#include "Animation/TestSkinningPoses.h"
#include "Strata/Renderer/LODSelection.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Renderer/RenderSceneGPU.h"

#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

// The GPU-driven path (SceneRendererSpecification::GPUDriven) against the CPU path, the reference: the same pixels (within
// 1/255) and the same entity IDs, scene by scene, through every way of issuing the GPU's draws the device allows.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = 128;

	struct PathFrame
	{
		ReadbackImage Color;
		ReadbackImage IDs;
		DrawnCounts Counts;
		bool GPUDriven = false;
	};

	// A view of the scene, perspective, reaching 500 units.
	SceneCamera View(const glm::vec3& position, const glm::vec3& target)
	{
		SceneCamera camera = LookAt(position, target);
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(60.0f), 1.0f, 0.1f, 500.0f);
		camera.Far = 500.0f;
		return camera;
	}

	// Pixels whose entity IDs differ (the first one is reported).
	uint32_t CountDifferentIDs(const ReadbackImage& expected, const ReadbackImage& actual)
	{
		REQUIRE(expected.Pixels.size() == actual.Pixels.size());
		uint32_t different = 0;
		for (size_t offset = 0; offset < expected.Pixels.size(); offset += sizeof(uint32_t))
		{
			uint32_t a = 0;
			uint32_t b = 0;
			std::memcpy(&a, expected.Pixels.data() + offset, sizeof(a));
			std::memcpy(&b, actual.Pixels.data() + offset, sizeof(b));
			if (a == b)
				continue;
			if (different++ == 0)
			{
				const size_t pixel = offset / sizeof(uint32_t);
				MESSAGE("First different entity ID at ", pixel % expected.Width, ",", pixel / expected.Width, ": ", a, " instead of ", b);
			}
		}
		return different;
	}

	// The two paths render each scene through the same steps: `frames` frames, `edit` between the first and the second.
	struct Comparison
	{
		SceneRenderer CPU;
		SceneRenderer GPU;

		static SceneRendererSpecification Specification(GPUDrivenMode mode, IndirectDrawMode indirect)
		{
			SceneRendererSpecification specification;
			specification.GPUDriven = mode;
			specification.IndirectMode = indirect;
			specification.TextureStreaming = false; // Every level of every texture, on both paths
			return specification;
		}

		explicit Comparison(IndirectDrawMode indirect)
			: CPU(Specification(GPUDrivenMode::Off, IndirectDrawMode::Auto)), GPU(Specification(GPUDrivenMode::Auto, indirect))
		{
			CPU.SetViewportSize(c_Size, c_Size);
			GPU.SetViewportSize(c_Size, c_Size);
		}

		static PathFrame Capture(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera)
		{
			PathFrame frame;
			frame.Color = RenderToImage(renderer, scene, camera);
			REQUIRE(Renderer::ReadTexture(renderer.GetEntityIDTexture(), frame.IDs));
			frame.Counts = GetDrawnCounts(renderer);
			frame.GPUDriven = renderer.GetStats().GPUDriven;
			return frame;
		}

		// Renders with both and checks they agree; returns the GPU path's frame.
		PathFrame Check(Scene& scene, const SceneCamera& camera)
		{
			const PathFrame reference = Capture(CPU, scene, camera);
			const PathFrame frame = Capture(GPU, scene, camera);
			REQUIRE(frame.GPUDriven);
			CHECK_FALSE(reference.GPUDriven);
			CHECK(MaxDifference(reference.Color, frame.Color) <= 1);
			CHECK(CountDifferentIDs(reference.IDs, frame.IDs) == 0);
			CHECK(frame.Counts.Instances == reference.Counts.Instances);
			CHECK(frame.Counts.Draws == reference.Counts.Draws);
			CHECK(frame.Counts.Triangles == reference.Counts.Triangles);
			CHECK(frame.Counts.InstancesPerLOD == reference.Counts.InstancesPerLOD);
			CHECK(frame.Counts.SkinnedInstances == reference.Counts.SkinnedInstances);
			CHECK(frame.Counts.ShadowCasters == reference.Counts.ShadowCasters);
			CHECK(frame.Counts.ShadowDraws == reference.Counts.ShadowDraws);
			return frame;
		}
	};

	bool DeviceDrawsGPUDriven()
	{
		return Renderer::GetGraphicsDevice().GetInfo().SupportsDrawIndirectFirstInstance;
	}

	// A sphere with three levels of detail.
	Ref<Mesh> CreateLODSphere()
	{
		Ref<Mesh> source = MeshFactory::CreateSphere(0.5f, 48, 24);
		REQUIRE(source);
		std::vector<uint32_t> indices = source->GetIndices();
		const std::vector<std::vector<uint32_t>> levels = MeshUtils::GenerateLODs(source->GetPositions(), indices, 2);
		REQUIRE(levels.size() == 2);
		Submesh submesh = source->GetSubmeshes()[0];
		submesh.LODs = { MeshLOD { 0, static_cast<uint32_t>(indices.size()) } };
		for (const std::vector<uint32_t>& level : levels)
		{
			submesh.LODs.push_back(MeshLOD { static_cast<uint32_t>(indices.size()), static_cast<uint32_t>(level.size()) });
			indices.insert(indices.end(), level.begin(), level.end());
		}
		std::string error;
		Ref<Mesh> mesh = Mesh::Create(source->GetPositions(), source->GetAttributes(), std::move(indices), { submesh }, &error);
		REQUIRE_MESSAGE(mesh, error);
		return mesh;
	}

	// The scenes both paths render. Each fills `scene` (assets through `assets`) and returns the camera; the skinned scene
	// registers `poses`.
	struct TestScene
	{
		const char* Name;
		std::function<SceneCamera(Scene&, SceneTestAssets&, TestSkinningPoses&)> Build;
	};

	AssetHandle LitMaterial(SceneTestAssets& assets, const glm::vec4& color, float roughness = 0.6f)
	{
		MaterialProperties properties;
		properties.BaseColor = color;
		properties.Roughness = roughness;
		return assets.AddMaterial(properties);
	}

	SceneCamera BuildManyMeshes(Scene& scene, SceneTestAssets& assets, TestSkinningPoses&)
	{
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)), 3.0f, false);
		const AssetHandle meshes[] = { BuiltinAssets::CubeMesh, BuiltinAssets::SphereMesh, BuiltinAssets::CylinderMesh, BuiltinAssets::ConeMesh,
			BuiltinAssets::TorusMesh, BuiltinAssets::CapsuleMesh };
		std::vector<AssetHandle> materials;
		for (uint32_t index = 0; index < 5; index++)
			materials.push_back(LitMaterial(assets, glm::vec4(0.2f + 0.15f * static_cast<float>(index), 0.5f, 0.9f - 0.15f * static_cast<float>(index), 1.0f)));
		for (uint32_t index = 0; index < 60; index++)
		{
			const glm::vec3 position(static_cast<float>(index % 10) * 1.6f - 7.2f, 0.0f, static_cast<float>(index / 10) * 1.6f - 4.0f);
			AddMesh(scene, meshes[index % std::size(meshes)], materials[index % materials.size()], position, "Shape")
				.GetComponent<TransformComponent>().Rotation = glm::vec3(0.0f, static_cast<float>(index) * 0.37f, 0.0f);
		}
		return View(glm::vec3(0.0f, 9.0f, 11.0f), glm::vec3(0.0f));
	}

	SceneCamera BuildAlphaMasked(Scene& scene, SceneTestAssets& assets, TestSkinningPoses&)
	{
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::normalize(glm::vec3(-0.3f, -1.0f, -0.5f)), 3.0f);
		// A checker of opaque and clear texels, cut out at half alpha, on a double-sided quad and on cubes.
		std::vector<uint8_t> pixels;
		for (uint32_t y = 0; y < 8; y++)
		{
			for (uint32_t x = 0; x < 8; x++)
				AppendPixel(pixels, 230, 120, 40, ((x + y) % 2) == 0 ? 255 : 0);
		}
		MaterialProperties masked;
		masked.BaseColorMap = assets.AddTexture(8, 8, pixels, TextureFormat::RGBA8SRGB);
		masked.AlphaMode = MaterialAlphaMode::Mask;
		masked.AlphaCutoff = 0.5f;
		masked.DoubleSided = true;
		const AssetHandle maskedMaterial = assets.AddMaterial(masked);
		const AssetHandle floor = LitMaterial(assets, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f));
		AddMesh(scene, BuiltinAssets::PlaneMesh, floor, glm::vec3(0.0f, -1.0f, 0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(20.0f);
		for (uint32_t index = 0; index < 6; index++)
		{
			Entity entity = AddMesh(scene, index % 2 == 0 ? BuiltinAssets::QuadMesh : BuiltinAssets::CubeMesh, maskedMaterial,
				glm::vec3(static_cast<float>(index) * 1.7f - 4.25f, 0.2f, 0.0f), "Masked");
			entity.GetComponent<TransformComponent>().Rotation = glm::vec3(0.3f, static_cast<float>(index) * 0.5f, 0.0f);
			entity.GetComponent<TransformComponent>().Scale = glm::vec3(1.3f);
		}
		return View(glm::vec3(0.0f, 4.0f, 8.0f), glm::vec3(0.0f));
	}

	SceneCamera BuildMirrored(Scene& scene, SceneTestAssets& assets, TestSkinningPoses&)
	{
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::normalize(glm::vec3(-0.4f, -1.0f, -0.6f)), 3.0f);
		const AssetHandle material = LitMaterial(assets, glm::vec4(0.3f, 0.7f, 0.4f, 1.0f));
		const glm::vec3 scales[] = { glm::vec3(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f), glm::vec3(1.0f, -1.0f, 1.0f), glm::vec3(-1.0f, -1.0f, -1.0f),
			glm::vec3(1.0f, 1.0f, -0.6f) };
		const AssetHandle meshes[] = { BuiltinAssets::ConeMesh, BuiltinAssets::TorusMesh, BuiltinAssets::CapsuleMesh };
		for (uint32_t index = 0; index < 15; index++)
		{
			Entity entity = AddMesh(scene, meshes[index % std::size(meshes)], material, glm::vec3(static_cast<float>(index % 5) * 1.8f - 3.6f, 0.0f,
				static_cast<float>(index / 5) * 1.8f - 1.8f), "Shape");
			entity.GetComponent<TransformComponent>().Scale = scales[index % std::size(scales)] * 0.8f;
			entity.GetComponent<TransformComponent>().Rotation = glm::vec3(0.4f, static_cast<float>(index) * 0.6f, 0.0f);
		}
		AddMesh(scene, BuiltinAssets::PlaneMesh, material, glm::vec3(0.0f, -1.0f, 0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(16.0f);
		return View(glm::vec3(0.0f, 6.0f, 8.0f), glm::vec3(0.0f));
	}

	SceneCamera BuildSkinned(Scene& scene, SceneTestAssets& assets, TestSkinningPoses& poses)
	{
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::normalize(glm::vec3(0.3f, -1.0f, -0.4f)), 3.0f);
		const AssetHandle material = LitMaterial(assets, glm::vec4(0.8f, 0.5f, 0.2f, 1.0f), 0.7f);
		const AssetHandle tube = assets.Add(CreateSkinnedTube({ .Joints = 3, .RingsPerJoint = 4, .Sides = 12, .JointLength = 1.0f, .Radius = 0.25f }), "Tube");
		AddMesh(scene, BuiltinAssets::PlaneMesh, material, glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(12.0f);
		for (uint32_t index = 0; index < 4; index++)
		{
			Entity arm = AddMesh(scene, tube, material, glm::vec3(static_cast<float>(index) * 1.4f - 2.1f, 0.25f, 0.0f), "Arm");
			// Three posed, one in its bind pose (the Skinned arena drawn through the plain pipelines).
			if (index < 3)
			{
				arm.AddComponent<SkinComponent>().Animator = arm.GetComponent<IDComponent>().ID;
				poses.Poses[arm.GetHandle()] = BentChain(3, 1 + index % 2);
			}
		}
		poses.Version++;
		REQUIRE(scene.RegisterService<ISkinningPoses>(poses));
		return View(glm::vec3(0.5f, 3.5f, 7.0f), glm::vec3(0.0f, 1.2f, 0.0f));
	}

	SceneCamera BuildShadowed(Scene& scene, SceneTestAssets& assets, TestSkinningPoses&)
	{
		AddNeutralPostProcess(scene);
		Entity sun = AddSun(scene, glm::normalize(glm::vec3(-0.5f, -1.0f, -0.4f)), 3.0f);
		sun.GetComponent<DirectionalLightComponent>().ShadowDistance = 150.0f;
		const AssetHandle material = LitMaterial(assets, glm::vec4(0.7f, 0.7f, 0.75f, 1.0f));
		AddMesh(scene, BuiltinAssets::PlaneMesh, material, glm::vec3(0.0f, 0.0f, -60.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(200.0f);
		// Pillars from close by to far away, in every cascade, and a grain too small for the far ones.
		for (uint32_t index = 0; index < 40; index++)
		{
			const float distance = 2.0f + static_cast<float>(index) * static_cast<float>(index) * 0.08f;
			Entity pillar = AddMesh(scene, BuiltinAssets::CubeMesh, material, glm::vec3(static_cast<float>(index % 2) * 4.0f - 2.0f, 1.0f, -distance), "Pillar");
			pillar.GetComponent<TransformComponent>().Scale = glm::vec3(0.6f + distance * 0.02f, 2.0f + distance * 0.05f, 0.6f);
		}
		AddMesh(scene, BuiltinAssets::SphereMesh, material, glm::vec3(1.0f, 0.3f, -110.0f), "Grain").GetComponent<TransformComponent>().Scale = glm::vec3(0.02f);
		SceneCamera camera = View(glm::vec3(0.0f, 4.0f, 6.0f), glm::vec3(0.0f, 1.0f, -20.0f));
		return camera;
	}

	SceneCamera BuildTransparent(Scene& scene, SceneTestAssets& assets, TestSkinningPoses&)
	{
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::normalize(glm::vec3(-0.4f, -1.0f, -0.5f)), 3.0f);
		const AssetHandle opaque = LitMaterial(assets, glm::vec4(0.2f, 0.4f, 0.8f, 1.0f));
		MaterialProperties glass;
		glass.BaseColor = glm::vec4(0.9f, 0.3f, 0.2f, 0.45f);
		glass.AlphaMode = MaterialAlphaMode::Blend;
		const AssetHandle transparent = assets.AddMaterial(glass);
		for (uint32_t index = 0; index < 12; index++)
		{
			AddMesh(scene, index % 3 == 0 ? BuiltinAssets::SphereMesh : BuiltinAssets::CubeMesh, index % 2 == 0 ? transparent : opaque,
				glm::vec3(static_cast<float>(index % 4) * 1.5f - 2.25f, 0.0f, -static_cast<float>(index / 4) * 1.5f), "Mixed");
		}
		return View(glm::vec3(0.0f, 3.0f, 6.0f), glm::vec3(0.0f, 0.0f, -1.5f));
	}

	SceneCamera BuildLODs(Scene& scene, SceneTestAssets& assets, TestSkinningPoses&)
	{
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::normalize(glm::vec3(-0.4f, -1.0f, -0.5f)), 3.0f);
		const AssetHandle sphere = assets.Add(CreateLODSphere(), "LODSphere");
		const AssetHandle material = LitMaterial(assets, glm::vec4(0.9f, 0.9f, 0.9f, 1.0f));
		// Spheres from 2 to 40 units away: every level, each chosen well away from its switching distance.
		const float distances[] = { 2.0f, 3.5f, 6.0f, 9.0f, 14.0f, 22.0f, 30.0f, 40.0f };
		for (uint32_t index = 0; index < std::size(distances); index++)
			AddMesh(scene, sphere, material, glm::vec3(static_cast<float>(index % 2) * 1.2f - 0.6f, 0.0f, -distances[index]), "Sphere");
		SceneCamera camera = View(glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.0f, 0.0f, -10.0f));
		return camera;
	}

	const TestScene c_Scenes[] = {
		{ "Many meshes", BuildManyMeshes },
		{ "Alpha-masked", BuildAlphaMasked },
		{ "Mirrored", BuildMirrored },
		{ "Posed skinned", BuildSkinned },
		{ "Shadowed", BuildShadowed },
		{ "Transparent", BuildTransparent },
		{ "LODs", BuildLODs },
	};

	void CompareScenes(GPUContext& gpu, IndirectDrawMode indirect)
	{
		for (const TestScene& testScene : c_Scenes)
		{
			const std::string name = testScene.Name;
			CAPTURE(name);
			SceneTestAssets assets;
			for (const AssetHandle mesh : { BuiltinAssets::CubeMesh, BuiltinAssets::SphereMesh, BuiltinAssets::PlaneMesh, BuiltinAssets::QuadMesh,
				BuiltinAssets::CylinderMesh, BuiltinAssets::ConeMesh, BuiltinAssets::TorusMesh, BuiltinAssets::CapsuleMesh, BuiltinAssets::DefaultMaterial })
				REQUIRE(assets.Manager->LoadAssetSync(mesh));
			TestSkinningPoses poses;
			Scene scene;
			const SceneCamera camera = testScene.Build(scene, assets, poses);
			Comparison comparison(indirect);
			const PathFrame frame = comparison.Check(scene, camera);
			CHECK(frame.Counts.Instances > 0);
			if (scene.GetService<ISkinningPoses>())
			{
				CHECK(frame.Counts.SkinnedInstances == 3);
				scene.UnregisterService<ISkinningPoses>(poses);
			}
			gpu.GetDevice().WaitForIdle();
		}
	}

}

TEST_SUITE("GPU.SceneRenderer.GPUDriven")
{
	TEST_CASE("The GPU path draws what the CPU path draws: meshes, masks, mirrors, skins, shadows, transparency and levels of detail")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!DeviceDrawsGPUDriven())
		{
			MESSAGE("The device has no drawIndirectFirstInstance: the CPU path draws");
			return;
		}
		CompareScenes(gpu, IndirectDrawMode::Auto);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Entities created between frames are drawn in their first frame, also when the buffers grow")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!DeviceDrawsGPUDriven())
			return;
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::DefaultMaterial));
		TestSkinningPoses poses;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)), 3.0f);
		const AssetHandle material = LitMaterial(assets, glm::vec4(0.6f, 0.4f, 0.3f, 1.0f));
		for (uint32_t index = 0; index < 10; index++)
			AddMesh(scene, BuiltinAssets::CubeMesh, material, glm::vec3(static_cast<float>(index) * 1.5f - 6.75f, 0.0f, 0.0f), "Cube");
		const SceneCamera camera = View(glm::vec3(0.0f, 40.0f, 30.0f), glm::vec3(0.0f, 0.0f, -10.0f));
		Comparison comparison(IndirectDrawMode::Auto);
		comparison.Check(scene, camera);
		const RenderSceneGPU* mirror = Renderer::GetRenderSceneGPUs().Find(RenderScene::Get(scene).GetId());
		REQUIRE(mirror);
		const uint64_t version = mirror->GetBufferVersion();

		// Five thousand more behind them (none touching another: coplanar faces would leave the winner to the draw order): the
		// instance and proxy buffers grow (by a GPU copy), the draw list too, and all are drawn now.
		for (uint32_t index = 0; index < 5000; index++)
		{
			AddMesh(scene, BuiltinAssets::CubeMesh, material, glm::vec3(static_cast<float>(index % 100) * 1.2f - 60.0f, 0.0f,
				-2.0f - static_cast<float>(index / 100) * 1.2f), "New");
		}
		const PathFrame frame = comparison.Check(scene, camera);
		CHECK(mirror->GetBufferVersion() != version);
		CHECK(frame.Counts.Instances > 1000);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Mesh renderers below half a pixel are left out by both paths; MinProjectedPixels 0 draws them")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!DeviceDrawsGPUDriven())
			return;
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::DefaultMaterial));
		Scene scene;
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)), 3.0f, false);
		// A field of small cubes reaching far: the far ones are far below a pixel.
		for (uint32_t index = 0; index < 4000; index++)
		{
			Entity cube = AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(static_cast<float>(index % 40) - 20.0f, 0.0f,
				-static_cast<float>(index / 40) * 4.0f), "Grain");
			cube.GetComponent<TransformComponent>().Scale = glm::vec3(0.1f);
		}
		const SceneCamera camera = View(glm::vec3(0.0f, 2.0f, 2.0f), glm::vec3(0.0f, 0.0f, -50.0f));
		Comparison comparison(IndirectDrawMode::Auto);
		const PathFrame culled = comparison.Check(scene, camera);
		REQUIRE(comparison.GPU.GetGPUStats().HasCounters);
		const uint32_t sizeCulled = comparison.GPU.GetGPUStats().SizeCulledProxies;
		CHECK(sizeCulled > 0);
		CHECK(comparison.CPU.GetStats().SizeCulledProxies == sizeCulled);

		SceneRendererSpecification everything = Comparison::Specification(GPUDrivenMode::Auto, IndirectDrawMode::Auto);
		everything.MinProjectedPixels = 0.0f;
		SceneRenderer all(everything);
		all.SetViewportSize(c_Size, c_Size);
		const PathFrame allFrame = Comparison::Capture(all, scene, camera);
		CHECK(allFrame.Counts.Instances == culled.Counts.Instances + sizeCulled);
		CHECK(all.GetGPUStats().SizeCulledProxies == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}

TEST_SUITE("GPU.SceneRenderer.IndirectFallback")
{
	TEST_CASE("Draws without a GPU-written count, and one draw per slot, draw what the CPU path draws")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!DeviceDrawsGPUDriven())
			return;
		for (const IndirectDrawMode mode : { IndirectDrawMode::NoCount, IndirectDrawMode::PerDraw })
		{
			CAPTURE(static_cast<int>(mode));
			CompareScenes(gpu, mode);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}

TEST_SUITE("GPU.Renderer.LODSelection")
{
	TEST_CASE("The GPU chooses the CPU's level of detail for 10,000 fractions")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();
		constexpr uint32_t c_Count = 10'000;
		std::mt19937 random(5);
		// Fractions from 2^-24 to 4 (no denormals, which GPUs may flush), with every exact doubling onto the threshold among them.
		std::uniform_real_distribution<float> exponent(-24.0f, 2.0f);
		std::vector<float> fractions(c_Count);
		for (uint32_t index = 0; index < c_Count; index++)
			fractions[index] = index % 100 == 0 ? std::ldexp(0.5f, -static_cast<int>(index / 100 % 20)) : std::exp2(exponent(random));

		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::Compute;
		layoutDesc.bindings = { nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0), nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),
			nvrhi::BindingLayoutItem::PushConstants(0, 4 * sizeof(uint32_t)) };
		const nvrhi::BindingLayoutHandle layout = device->createBindingLayout(layoutDesc);
		nvrhi::ComputePipelineDesc pipelineDesc;
		pipelineDesc.CS = Renderer::GetShaderLibrary().Get("Debug/LODSelection.comp");
		pipelineDesc.bindingLayouts = { layout };
		const nvrhi::ComputePipelineHandle pipeline = device->createComputePipeline(pipelineDesc);
		REQUIRE(pipeline);
		const nvrhi::BufferHandle input = device->createBuffer(nvrhi::BufferDesc().setByteSize(c_Count * sizeof(float)).setStructStride(sizeof(float))
			.setInitialState(nvrhi::ResourceStates::ShaderResource).setKeepInitialState(true).setDebugName("LODFractions"));
		const nvrhi::BufferHandle output = device->createBuffer(nvrhi::BufferDesc().setByteSize(c_Count * sizeof(uint32_t)).setStructStride(sizeof(uint32_t))
			.setCanHaveUAVs(true).setInitialState(nvrhi::ResourceStates::UnorderedAccess).setKeepInitialState(true).setDebugName("LODLevels"));
		REQUIRE(input);
		REQUIRE(output);
		nvrhi::BindingSetDesc setDesc;
		setDesc.bindings = { nvrhi::BindingSetItem::StructuredBuffer_SRV(0, input), nvrhi::BindingSetItem::StructuredBuffer_UAV(0, output),
			nvrhi::BindingSetItem::PushConstants(0, 4 * sizeof(uint32_t)) };
		const nvrhi::BindingSetHandle set = device->createBindingSet(setDesc, layout);
		REQUIRE(set);

		const float thresholds[] = { 0.5f, 0.3f, 2.0f };
		const uint32_t lodCounts[] = { 1, 2, 4, 8, 32 };
		uint32_t compared = 0;
		for (const float threshold : thresholds)
		{
			for (const uint32_t lodCount : lodCounts)
			{
				CAPTURE(threshold);
				CAPTURE(lodCount);
				nvrhi::CommandListHandle commandList = device->createCommandList();
				commandList->open();
				commandList->writeBuffer(input, fractions.data(), fractions.size() * sizeof(float));
				nvrhi::ComputeState state;
				state.pipeline = pipeline;
				state.bindings = { set };
				commandList->setComputeState(state);
				struct Parameters
				{
					uint32_t Count;
					float Threshold;
					uint32_t LODCount;
					uint32_t Padding;
				} parameters { c_Count, threshold, lodCount, 0 };
				commandList->setPushConstants(&parameters, sizeof(parameters));
				commandList->dispatch((c_Count + 63) / 64);
				commandList->close();
				REQUIRE(gpu.ExecuteAndWait(commandList));
				const std::vector<uint8_t> bytes = ReadBufferRange(gpu, output, 0, c_Count * sizeof(uint32_t));
				std::vector<uint32_t> levels(c_Count);
				std::memcpy(levels.data(), bytes.data(), bytes.size());
				for (uint32_t index = 0; index < c_Count; index++)
				{
					CAPTURE(fractions[index]);
					REQUIRE(levels[index] == LODSelection::SelectLOD(fractions[index], threshold, lodCount));
					compared++;
				}
			}
		}
		CHECK(compared == c_Count * std::size(thresholds) * std::size(lodCounts));
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
