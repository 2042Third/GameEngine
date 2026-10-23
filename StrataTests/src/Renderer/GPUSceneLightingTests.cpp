#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Renderer/RendererComponents.h"

#include <cmath>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = c_SceneTestSize;

	float DecodeSRGB(int value)
	{
		const float encoded = static_cast<float>(value) / 255.0f;
		return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
	}

	// A large matte floor (facing +Y) at the origin.
	Entity AddFloor(Scene& scene, SceneTestAssets& assets, float size)
	{
		MaterialProperties matte;
		matte.Roughness = 1.0f;
		Entity floor = AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(matte), glm::vec3(0.0f), "Floor");
		floor.GetComponent<TransformComponent>().Scale = glm::vec3(size);
		return floor;
	}

}

TEST_SUITE("GPU.SceneRenderer.Lighting")
{
	TEST_CASE("Point lights illuminate nearby surfaces and tone mapping operators are applied")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		PostProcessComponent& postProcess = AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(10.0f);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 3.0f, 3.0f), glm::vec3(0.0f));
		const glm::u8vec4 dark = GetPixelRGBA8(RenderToImage(renderer, scene, camera), c_Size / 2, c_Size / 2);

		AddPointLight(scene, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f), 2.0f, 5.0f);
		const glm::u8vec4 lit = GetPixelRGBA8(RenderToImage(renderer, scene, camera), c_Size / 2, c_Size / 2);
		CHECK(lit.r > dark.r + 50);
		CHECK(GetLightCounts(renderer).Lights == 1);

		// Every operator maps the same scene into the displayable range.
		for (TonemapOperator tonemapper : { TonemapOperator::Reinhard, TonemapOperator::ACES, TonemapOperator::AgX, TonemapOperator::KhronosNeutral })
		{
			CAPTURE(static_cast<int>(tonemapper));
			postProcess.Tonemapper = tonemapper;
			const glm::u8vec4 mapped = GetPixelRGBA8(RenderToImage(renderer, scene, camera), c_Size / 2, c_Size / 2);
			CHECK(mapped.r > dark.r);
			CHECK(mapped.a == 255);
		}

		// Exposure compensation brightens.
		postProcess.Tonemapper = TonemapOperator::None;
		postProcess.Exposure = 1.0f;
		const glm::u8vec4 brighter = GetPixelRGBA8(RenderToImage(renderer, scene, camera), c_Size / 4, c_Size / 2);
		postProcess.Exposure = 0.0f;
		const glm::u8vec4 normal = GetPixelRGBA8(RenderToImage(renderer, scene, camera), c_Size / 4, c_Size / 2);
		CHECK(brighter.r > normal.r);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Light units share one scale: a 65,536 lux scene at exposure -16 renders as a 1 lux scene at exposure 0")
	{
		// Lux, candela and nits are linked by the shading without a factor (DeveloperGuide, "Light units and exposure"), and
		// manual exposure multiplies by 2^Exposure: scaling every light by a power of two and the exposure by its inverse is
		// exact while no value nears the half-float limit or its subnormals. The scene is closed (the floor fills the view)
		// and its sky black, since the clear color does not scale with the lights.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		MaterialProperties matte;
		matte.BaseColor = glm::vec4(0.5f, 0.6f, 0.7f, 1.0f);
		matte.Roughness = 1.0f;
		const AssetHandle material = assets.AddMaterial(matte);

		const auto render = [&](float scale, float exposure)
		{
			Scene scene;
			AddNeutralPostProcess(scene).Exposure = exposure;
			SkyLightComponent& sky = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
			sky.AmbientColor = glm::vec3(0.0f);
			AddMesh(scene, BuiltinAssets::PlaneMesh, material, glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(100.0f);
			AddMesh(scene, BuiltinAssets::CubeMesh, material, glm::vec3(0.0f, 0.5f, 0.0f), "Box");
			AddSun(scene, glm::vec3(1.0f, -2.0f, 0.5f), scale, true);
			AddPointLight(scene, glm::vec3(-3.0f, 2.0f, 2.5f), glm::vec3(1.0f, 0.9f, 0.8f), 4.0f * scale, 100.0f);
			Entity spot = scene.CreateEntity("Spot");
			spot.GetComponent<TransformComponent>().Translation = glm::vec3(2.5f, 3.0f, -1.5f);
			spot.GetComponent<TransformComponent>().Rotation = Math::LookRotation(glm::vec3(0.0f, -1.0f, 0.0f));
			SpotLightComponent& light = spot.AddComponent<SpotLightComponent>();
			light.Intensity = 8.0f * scale;
			light.Range = 100.0f;
			SceneRenderer renderer;
			renderer.SetViewportSize(c_Size, c_Size);
			return RenderToImage(renderer, scene, LookAt(glm::vec3(2.0f, 7.0f, 3.0f), glm::vec3(0.0f)));
		};

		constexpr float c_Sun = 65536.0f; // 2^16 lux: a sunlit scene, below the half-float limit on a matte surface
		const ReadbackImage dim = render(1.0f, 0.0f);
		const ReadbackImage bright = render(c_Sun, -16.0f);
		CHECK(bright.Pixels == dim.Pixels);
		// Lit, and the comparison sees a stop of difference.
		uint32_t litPixels = 0;
		for (uint32_t y = 0; y < c_Size; y++)
		{
			for (uint32_t x = 0; x < c_Size; x++)
				litPixels += Red(dim, x, y) > 40 ? 1u : 0u;
		}
		CHECK(litPixels > c_Size * c_Size / 2);
		CHECK(render(c_Sun, -15.0f).Pixels != dim.Pixels);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Spot lights light a cone with a smooth edge between the inner and outer angle")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddNeutralPostProcess(scene);
		AddFloor(scene, assets, 20.0f);
		Entity spot = scene.CreateEntity("Spot");
		spot.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 4.0f, 0.0f);
		spot.GetComponent<TransformComponent>().Rotation = Math::LookRotation(glm::vec3(0.0f, -1.0f, 0.0f)); // Pointing down
		SpotLightComponent& light = spot.AddComponent<SpotLightComponent>();
		light.Intensity = 40.0f;
		light.Range = 10.0f;
		light.InnerConeAngle = 20.0f;
		light.OuterConeAngle = 30.0f;

		constexpr uint32_t size = 128;
		SceneRenderer renderer;
		renderer.SetViewportSize(size, size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 9.0f, 0.0f), glm::vec3(0.0f));
		const ReadbackImage image = RenderToImage(renderer, scene, camera);
		CHECK(GetLightCounts(renderer).Lights == 1);
		spot.SetActive(false);
		const ReadbackImage unlit = RenderToImage(renderer, scene, camera);
		spot.SetActive(true);

		// Floor points seen from the light at a given angle off its axis, along +X and +Z.
		auto lightAt = [&](float degrees, bool alongZ)
		{
			const float offset = 4.0f * std::tan(glm::radians(degrees));
			const glm::uvec2 pixel = ProjectToPixel(camera, alongZ ? glm::vec3(0.0f, 0.0f, offset) : glm::vec3(offset, 0.0f, 0.0f), size);
			return DecodeSRGB(Red(image, pixel)) - DecodeSRGB(Red(unlit, pixel));
		};

		const float center = lightAt(0.0f, false);
		CHECK(center > 0.3f);
		// Inside the inner cone only distance and incidence attenuate: cos^3 of the angle (plus the range window).
		CHECK(lightAt(10.0f, false) / center == doctest::Approx(std::pow(std::cos(glm::radians(10.0f)), 3.0f)).epsilon(0.08));
		// Between inner and outer angle the cone fades smoothly: (25 - 30) / (20 - 30) squared, about 0.3, times cos^3.
		const float edge = lightAt(25.0f, false) / center;
		CHECK(edge > 0.12f);
		CHECK(edge < 0.32f);
		CHECK(lightAt(25.0f, true) / center == doctest::Approx(edge).epsilon(0.1)); // Round, not square
		// Outside the outer angle nothing arrives.
		CHECK(std::abs(lightAt(35.0f, false)) < 0.004f);
		CHECK(std::abs(lightAt(35.0f, true)) < 0.004f);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Clustered lighting matches shading every pixel with every light")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddNeutralPostProcess(scene);
		AddFloor(scene, assets, 60.0f);
		AddMesh(scene, BuiltinAssets::SphereMesh, UUID::Null(), glm::vec3(0.0f, 1.0f, -6.0f), "Sphere").GetComponent<TransformComponent>().Scale = glm::vec3(2.0f);
		// 500 small lights over the floor, in varied colors.
		constexpr uint32_t c_LightCount = 500;
		for (uint32_t index = 0; index < c_LightCount; index++)
		{
			const float x = static_cast<float>(index % 25) - 12.0f;
			const float z = -static_cast<float>(index / 25) * 1.5f + 3.0f;
			const glm::vec3 color(0.5f + 0.5f * std::sin(static_cast<float>(index)), 0.5f + 0.5f * std::sin(static_cast<float>(index) * 1.7f + 2.0f),
				0.5f + 0.5f * std::sin(static_cast<float>(index) * 2.3f + 4.0f));
			AddPointLight(scene, glm::vec3(x, 0.4f, z), color, 1.5f, 1.6f);
		}
		// A few spot lights too.
		for (int index = 0; index < 4; index++)
		{
			Entity spot = scene.CreateEntity("Spot");
			spot.GetComponent<TransformComponent>().Translation = glm::vec3(static_cast<float>(index) * 4.0f - 6.0f, 3.0f, -4.0f);
			spot.GetComponent<TransformComponent>().Rotation = Math::LookRotation(glm::vec3(0.2f, -1.0f, 0.1f));
			spot.AddComponent<SpotLightComponent>().Intensity = 15.0f;
		}

		const SceneCamera camera = LookAt(glm::vec3(0.0f, 6.0f, 8.0f), glm::vec3(0.0f, 0.0f, -8.0f), 16.0f / 9.0f);
		SceneRenderer clustered;
		clustered.SetViewportSize(160, 90);
		SceneRendererSpecification bruteForceSpecification;
		bruteForceSpecification.LightClusterGrid = glm::uvec3(1);
		bruteForceSpecification.MaxLightsPerCluster = 1024;
		SceneRenderer bruteForce(bruteForceSpecification);
		bruteForce.SetViewportSize(160, 90);

		// The CPU path, which collects the lights itself (sorted by relevance), shades the same.
		SceneRendererSpecification cpuSpecification;
		cpuSpecification.GPUDriven = GPUDrivenMode::Off;
		SceneRenderer cpuPath(cpuSpecification);
		cpuPath.SetViewportSize(160, 90);

		const ReadbackImage clusteredImage = RenderToImage(clustered, scene, camera);
		const ReadbackImage referenceImage = RenderToImage(bruteForce, scene, camera);
		const ReadbackImage cpuImage = RenderToImage(cpuPath, scene, camera);
		CHECK(MaxDifference(clusteredImage, referenceImage) <= 2);
		CHECK(MaxDifference(clusteredImage, cpuImage) <= 1);
		const LightCounts counts = GetLightCounts(clustered);
		CHECK(counts.Lights > 300);
		CHECK(counts.CulledLights > 0); // Lights beside and behind the view
		CHECK(counts.Lights + counts.CulledLights == c_LightCount + 4);
		CHECK(counts.DroppedLights == 0);
		CHECK(GetLightCounts(bruteForce).Lights == counts.Lights);
		const LightCounts cpuCounts = GetLightCounts(cpuPath);
		CHECK(cpuCounts.Lights == counts.Lights);
		CHECK(cpuCounts.CulledLights == counts.CulledLights);

		// The lights matter: without them the image is much darker (the writes signaled, as lights are render proxies).
		for (auto [entity, light] : scene.GetAllEntitiesWith<PointLightComponent>().each())
			scene.GetRegistry().patch<PointLightComponent>(entity, [](PointLightComponent& patched) { patched.Intensity = 0.0f; });
		const ReadbackImage dark = RenderToImage(clustered, scene, camera);
		CHECK(MaxDifference(dark, clusteredImage) > 60);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Lights outside the view are culled and the most relevant lights are kept when there are too many")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddNeutralPostProcess(scene);
		AddFloor(scene, assets, 40.0f);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 4.0f, 4.0f), glm::vec3(0.0f, 0.0f, -2.0f));
		// Behind the camera, and far to the side: invisible, and they cannot reach anything visible.
		AddPointLight(scene, glm::vec3(0.0f, 1.0f, 12.0f), glm::vec3(1.0f), 5.0f, 2.0f);
		AddPointLight(scene, glm::vec3(40.0f, 1.0f, 0.0f), glm::vec3(1.0f), 5.0f, 2.0f);
		Entity behindSpot = scene.CreateEntity("BehindSpot");
		behindSpot.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 3.0f, 9.0f);
		behindSpot.GetComponent<TransformComponent>().Rotation = Math::LookRotation(glm::vec3(0.0f, 0.0f, 1.0f)); // Pointing away
		behindSpot.AddComponent<SpotLightComponent>().Range = 4.0f;
		// Visible: a far light and a near one. The far one's light record comes first (the render scene reads it before the
		// near light exists), so that the order of the records is not that of relevance.
		AddPointLight(scene, glm::vec3(1.5f, 0.5f, -10.0f), glm::vec3(1.0f), 2.0f, 2.0f);
		RenderScene::Get(scene).Sync(nullptr);
		AddPointLight(scene, glm::vec3(-1.5f, 0.5f, 0.0f), glm::vec3(1.0f), 2.0f, 2.0f);

		constexpr uint32_t size = 128;
		SceneRenderer renderer;
		renderer.SetViewportSize(size, size);
		const ReadbackImage allLights = RenderToImage(renderer, scene, camera);
		const LightCounts counts = GetLightCounts(renderer);
		CHECK(counts.Lights == 2);
		CHECK(counts.CulledLights == 3);
		CHECK(counts.DroppedLights == 0);

		// With room for one light only (the CPU path's cap: the GPU-driven path draws every light in view), the one with the
		// largest projected size (the near one) stays.
		SceneRendererSpecification specification;
		specification.GPUDriven = GPUDrivenMode::Off;
		specification.MaxLights = 1;
		SceneRenderer limited(specification);
		limited.SetViewportSize(size, size);
		const ReadbackImage oneLight = RenderToImage(limited, scene, camera);
		CHECK(limited.GetStats().Lights == 1);
		CHECK(limited.GetStats().DroppedLights == 1);
		const glm::uvec2 nearPixel = ProjectToPixel(camera, glm::vec3(-1.5f, 0.0f, 0.0f), size);
		const glm::uvec2 farPixel = ProjectToPixel(camera, glm::vec3(1.5f, 0.0f, -10.0f), size);
		CHECK(Red(oneLight, nearPixel) == Red(allLights, nearPixel));
		CHECK(Red(oneLight, farPixel) < Red(allLights, farPixel) - 20);

		// Full clusters keep their most relevant lights the same way, on the GPU-driven path (which ranks the lights of a full
		// cluster on the GPU) and the CPU path (which sorts its lights).
		for (const GPUDrivenMode mode : { GPUDrivenMode::Auto, GPUDrivenMode::Off })
		{
			CAPTURE(static_cast<int>(mode));
			SceneRendererSpecification tightClusters;
			tightClusters.GPUDriven = mode;
			tightClusters.LightClusterGrid = glm::uvec3(1);
			tightClusters.MaxLightsPerCluster = 1;
			SceneRenderer tight(tightClusters);
			tight.SetViewportSize(size, size);
			const ReadbackImage oneLightPerCluster = RenderToImage(tight, scene, camera);
			CHECK(GetLightCounts(tight).Lights == 2);
			CHECK(MaxDifference(oneLightPerCluster, oneLight) <= 1);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The GPU-driven path draws every light in view, beyond the CPU path's MaxLights")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddFloor(scene, assets, 80.0f);
		// 10,000 small lights over the floor, all in view from above.
		constexpr uint32_t c_Side = 100;
		for (uint32_t index = 0; index < c_Side * c_Side; index++)
		{
			const glm::vec3 position(static_cast<float>(index % c_Side) * 0.6f - 29.7f, 0.2f, static_cast<float>(index / c_Side) * 0.6f - 29.7f);
			AddPointLight(scene, position, glm::vec3(1.0f), 1.0f, 0.5f);
		}
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 70.0f, 0.0f), glm::vec3(0.0f));
		constexpr uint32_t c_ViewSize = 128;
		SceneRenderer gpuPath;
		gpuPath.SetViewportSize(c_ViewSize, c_ViewSize);
		REQUIRE(gpuPath.GetSpecification().MaxLights == 4096);
		SceneRendererSpecification cpuSpecification;
		cpuSpecification.GPUDriven = GPUDrivenMode::Off;
		SceneRenderer cpuPath(cpuSpecification);
		cpuPath.SetViewportSize(c_ViewSize, c_ViewSize);

		const ReadbackImage gpuImage = RenderToImage(gpuPath, scene, camera);
		const ReadbackImage cpuImage = RenderToImage(cpuPath, scene, camera);
		const LightCounts gpuCounts = GetLightCounts(gpuPath);
		CHECK(gpuCounts.Lights == c_Side * c_Side);
		CHECK(gpuCounts.CulledLights == 0);
		CHECK(gpuCounts.DroppedLights == 0);
		const LightCounts cpuCounts = GetLightCounts(cpuPath);
		CHECK(cpuCounts.Lights == 4096);
		CHECK(cpuCounts.DroppedLights == c_Side * c_Side - 4096);
		// The lights the CPU path left out light the floor on the GPU-driven path: it is brighter.
		uint64_t gpuSum = 0;
		uint64_t cpuSum = 0;
		for (uint32_t y = 0; y < c_ViewSize; y++)
		{
			for (uint32_t x = 0; x < c_ViewSize; x++)
			{
				gpuSum += static_cast<uint64_t>(Red(gpuImage, x, y));
				cpuSum += static_cast<uint64_t>(Red(cpuImage, x, y));
			}
		}
		CHECK(gpuSum > cpuSum + cpuSum / 10);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Orthographic cameras view every pixel along the same direction")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		MaterialProperties glossy;
		glossy.BaseColor = glm::vec4(0.05f, 0.05f, 0.05f, 1.0f);
		glossy.Roughness = 0.2f;

		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(glossy), glm::vec3(0.0f), "Wall").GetComponent<TransformComponent>().Scale = glm::vec3(10.0f);
		// The light comes from behind the camera: every pixel mirrors it straight back.
		AddSun(scene, glm::vec3(0.0f, 0.0f, -1.0f), 1.0f, false);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = OrthographicLookAt(glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f), 6.0f);
		const ReadbackImage image = RenderToImage(renderer, scene, camera);
		const int center = Red(image, c_Size / 2, c_Size / 2);
		CHECK(center > 150);
		CHECK(std::abs(Red(image, 4, c_Size / 2) - center) <= 2);
		CHECK(std::abs(Red(image, c_Size / 2, c_Size - 4) - center) <= 2);

		// Screen-space ambient occlusion sees a flat floor as open, wherever it is on screen.
		Scene floorScene;
		AddNeutralPostProcess(floorScene).AmbientOcclusion = true;
		AddMesh(floorScene, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(40.0f);
		renderer.Render(floorScene, OrthographicLookAt(glm::vec3(0.0f, 5.0f, 5.0f), glm::vec3(0.0f), 8.0f));
		ReadbackImage occlusion;
		REQUIRE(Renderer::ReadTexture(renderer.GetAmbientOcclusionTexture(), occlusion));
		float darkest = 1.0f;
		for (uint32_t y = 0; y < c_Size; y++)
		{
			for (uint32_t x = 0; x < c_Size; x++)
				darkest = std::min(darkest, ReadOcclusionTexel(occlusion, glm::uvec2(x, y)));
		}
		CHECK(darkest > 0.95f);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Environment filtering conserves energy in every mip and face, and the BRDF lookup table is plausible")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const AssetHandle uniform = assets.Add(CreateEnvironmentTexture(glm::vec3(2.0f), glm::vec3(2.0f)), "UniformSky");

		Scene scene;
		scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().EnvironmentMap = uniform;
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		REQUIRE(renderer.Render(scene, LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f))));
		CHECK(renderer.GetStats().EnvironmentLighting);
		REQUIRE(renderer.GetIrradianceMap());

		// A uniform environment stays uniform under every normalized filter: each face and mip of the radiance cube,
		// the irradiance map and every roughness level of the prefiltered map.
		for (nvrhi::ITexture* texture : { renderer.GetEnvironmentCube(), renderer.GetIrradianceMap(), renderer.GetPrefilteredMap() })
		{
			const nvrhi::TextureDesc& desc = texture->getDesc();
			CAPTURE(desc.debugName);
			CHECK(desc.arraySize == 6);
			for (uint32_t mip = 0; mip < desc.mipLevels; mip++)
			{
				for (uint32_t face = 0; face < 6; face++)
				{
					CAPTURE(mip);
					CAPTURE(face);
					ReadbackImage image;
					REQUIRE(Renderer::ReadTexture(texture, image, mip, face));
					CHECK(image.Width == std::max(desc.width >> mip, 1u));
					for (uint32_t y = 0; y < image.Height; y += std::max(1u, image.Height / 4))
					{
						for (uint32_t x = 0; x < image.Width; x += std::max(1u, image.Width / 4))
						{
							const glm::vec4 texel = ReadHalfTexel(image, x, y);
							CHECK(texel.r == doctest::Approx(2.0f).epsilon(0.01));
							CHECK(texel.b == doctest::Approx(2.0f).epsilon(0.01));
						}
					}
				}
			}
		}
		ReadbackImage outOfRange;
		CHECK_FALSE(Renderer::ReadTexture(renderer.GetPrefilteredMap(), outOfRange, 6, 0));
		CHECK_FALSE(Renderer::ReadTexture(renderer.GetPrefilteredMap(), outOfRange, 0, 6));

		ReadbackImage lut;
		REQUIRE(Renderer::ReadTexture(renderer.GetBRDFLut(), lut));
		auto lutAt = [&](float NoV, float roughness)
		{
			uint16_t texel[2];
			const uint32_t x = std::min(lut.Width - 1, static_cast<uint32_t>(NoV * static_cast<float>(lut.Width)));
			const uint32_t y = std::min(lut.Height - 1, static_cast<uint32_t>(roughness * static_cast<float>(lut.Height)));
			std::memcpy(texel, lut.Pixels.data() + (static_cast<size_t>(y) * lut.Width + x) * lut.BytesPerPixel, sizeof(texel));
			return glm::vec2(glm::unpackHalf1x16(texel[0]), glm::unpackHalf1x16(texel[1]));
		};
		// A smooth surface seen head-on reflects F0 exactly; rough surfaces at grazing angles lose energy.
		const glm::vec2 smooth = lutAt(1.0f, 0.0f);
		CHECK(smooth.x == doctest::Approx(1.0f).epsilon(0.03));
		CHECK(smooth.y == doctest::Approx(0.0f).epsilon(0.03));
		const glm::vec2 rough = lutAt(0.1f, 1.0f);
		CHECK(rough.x + rough.y < 0.9f);
		CHECK(rough.x >= 0.0f);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Rougher prefiltered levels blur the environment")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const AssetHandle environment = assets.Add(CreateEnvironmentTexture(glm::vec3(1.0f), glm::vec3(0.0f)), "HalfSky");

		Scene scene;
		scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().EnvironmentMap = environment;
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		REQUIRE(renderer.Render(scene, LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f))));

		// On the +X face, row 0 looks just above the horizon and the bottom row just below it. The sharp level keeps
		// the step between sky and ground; rough levels blend them, symmetrically around the horizon.
		nvrhi::ITexture* prefiltered = renderer.GetPrefilteredMap();
		float previousContrast = 2.0f;
		for (uint32_t mip = 0; mip < prefiltered->getDesc().mipLevels; mip++)
		{
			CAPTURE(mip);
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(prefiltered, image, mip, 0));
			const uint32_t centerX = image.Width / 2;
			const float aboveHorizon = ReadHalfTexel(image, centerX, image.Height / 2 - 1).g;
			const float belowHorizon = ReadHalfTexel(image, centerX, image.Height / 2).g;
			CHECK(aboveHorizon > belowHorizon);
			CHECK(aboveHorizon + belowHorizon == doctest::Approx(1.0f).epsilon(0.05)); // Energy is only moved, never lost
			const float top = ReadHalfTexel(image, centerX, 0).g;
			const float bottom = ReadHalfTexel(image, centerX, image.Height - 1).g;
			const float contrast = top - bottom;
			CHECK(contrast <= previousContrast + 0.01f);
			previousContrast = contrast;
			if (mip == 0)
				CHECK(contrast > 0.95f);
		}
		CHECK(previousContrast < 0.8f);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Image-based lighting shades surfaces from the environment and shows it as background")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const AssetHandle environment = assets.Add(CreateEnvironmentTexture(glm::vec3(0.2f, 0.4f, 1.0f), glm::vec3(0.05f, 0.03f, 0.01f)), "TwoToneSky");

		Scene scene;
		AddNeutralPostProcess(scene);
		SkyLightComponent& sky = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
		sky.EnvironmentMap = environment;
		AddMesh(scene, BuiltinAssets::SphereMesh, UUID::Null(), glm::vec3(0.0f), "Sphere");

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f));
		ReadbackImage image = RenderToImage(renderer, scene, camera);

		// The upper half of the sphere faces the bright blue sky, the lower half the dark ground.
		const glm::u8vec4 upper = GetPixelRGBA8(image, c_Size / 2, c_Size / 2 - 10);
		const glm::u8vec4 lower = GetPixelRGBA8(image, c_Size / 2, c_Size / 2 + 10);
		CHECK(upper.b > lower.b + 40);
		CHECK(upper.b > upper.r);
		// Background above the horizon is the sky color (sRGB of 0.2, 0.4, 1.0).
		const glm::u8vec4 background = GetPixelRGBA8(image, 2, 2);
		CHECK(std::abs(background.r - 124) <= 6);
		CHECK(std::abs(background.g - 170) <= 6);
		CHECK(background.b >= 250);

		// Without the background the camera's clear color shows; the sphere stays lit.
		sky.ShowBackground = false;
		image = RenderToImage(renderer, scene, camera);
		CHECK(GetPixelRGBA8(image, 2, 2) == glm::u8vec4(0, 0, 0, 255));
		CHECK(GetPixelRGBA8(image, c_Size / 2, c_Size / 2 - 10) == upper);

		// Intensity scales the lighting; removing the environment falls back to the constant ambient.
		sky.Intensity = 0.0f;
		image = RenderToImage(renderer, scene, camera);
		CHECK(GetPixelRGBA8(image, c_Size / 2, c_Size / 2 - 10).b < upper.b);
		sky.EnvironmentMap = UUID::Null();
		renderer.Render(scene, camera);
		CHECK_FALSE(renderer.GetStats().EnvironmentLighting);
		CHECK(renderer.GetEnvironmentCube() == nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Switching the environment map relights the scene")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const AssetHandle reddish = assets.Add(CreateEnvironmentTexture(glm::vec3(1.0f, 0.1f, 0.1f), glm::vec3(1.0f, 0.1f, 0.1f)), "RedSky");
		const AssetHandle bluish = assets.Add(CreateEnvironmentTexture(glm::vec3(0.1f, 0.1f, 1.0f), glm::vec3(0.1f, 0.1f, 1.0f)), "BlueSky");

		Scene scene;
		AddNeutralPostProcess(scene);
		SkyLightComponent& sky = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
		sky.EnvironmentMap = reddish;
		AddMesh(scene, BuiltinAssets::SphereMesh, UUID::Null(), glm::vec3(0.0f), "Sphere");

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f));
		ReadbackImage image = RenderToImage(renderer, scene, camera);
		const glm::u8vec4 redSphere = GetPixelRGBA8(image, c_Size / 2, c_Size / 2);
		const glm::u8vec4 redBackground = GetPixelRGBA8(image, 2, 2);
		CHECK(redSphere.r > redSphere.b + 60);
		CHECK(redBackground.r > redBackground.b + 60);
		nvrhi::ITexture* firstCube = renderer.GetEnvironmentCube();

		sky.EnvironmentMap = bluish;
		image = RenderToImage(renderer, scene, camera);
		CHECK(renderer.GetEnvironmentCube() != firstCube);
		const glm::u8vec4 blueSphere = GetPixelRGBA8(image, c_Size / 2, c_Size / 2);
		const glm::u8vec4 blueBackground = GetPixelRGBA8(image, 2, 2);
		CHECK(blueSphere.b > blueSphere.r + 60);
		CHECK(blueBackground.b > blueBackground.r + 60);
		// Symmetric environments: swapping them swaps the channels.
		CHECK(std::abs(blueSphere.b - redSphere.r) <= 2);

		sky.EnvironmentMap = reddish;
		image = RenderToImage(renderer, scene, camera);
		CHECK(GetPixelRGBA8(image, c_Size / 2, c_Size / 2) == redSphere);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
