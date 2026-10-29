#include "Renderer/TAATestUtils.h"

#include "Strata/Renderer/RendererQuality.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Specular that holds at 4K (Include/PBR.glsl's WidenRoughness and GetHorizonFade, RendererQualitySettings::EnvironmentResolution):
// specular anti-aliasing calms a bumpy highlight and keeps its light, the prepass's surface target holds the roughness the
// forward pass shades, reflections a normal map turns into the surface fade out, and the prefiltered sky's size follows the
// quality.

using namespace Strata;
using namespace Strata::Tests;
using namespace Strata::Tests::TAA;

namespace
{

	constexpr uint32_t c_SpecularViewSize = 256;

	SceneRenderer CreateSpecularRenderer(const char* name, bool specularAntiAliasing, bool temporal = false)
	{
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		specification.TemporalAntiAliasing = temporal;
		specification.SpecularAntiAliasing = specularAntiAliasing;
		specification.DebugName = name;
		return SceneRenderer(specification);
	}

	void LoadMeshes(SceneTestAssets& assets)
	{
		for (const AssetHandle handle : { BuiltinAssets::SphereMesh, BuiltinAssets::PlaneMesh, BuiltinAssets::DefaultMaterial })
			REQUIRE(assets.Manager->LoadAssetSync(handle));
	}

	float Luminance(const glm::vec3& color)
	{
		return glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
	}

	// Renders until the sky's environment lights the scene (made over a few frames), or once without a sky; returns how many
	// times the frames computed the environment (SceneRendererStats::EnvironmentUpdates counts a frame's).
	uint32_t RenderSettled(GPUContext& gpu, SceneRenderer& renderer, Scene& scene, const SceneCamera& camera, bool environment)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		uint32_t updates = 0;
		do
		{
			REQUIRE(renderer.Render(scene, camera));
			FinishFrame(gpu);
			updates += renderer.GetStats().EnvironmentUpdates;
		} while (environment && !renderer.GetStats().EnvironmentLighting && std::chrono::steady_clock::now() < deadline);
		REQUIRE(renderer.GetStats().EnvironmentLighting == environment);
		return updates;
	}

	struct SphereImage
	{
		std::vector<glm::vec3> HDR;
		std::vector<uint32_t> IDs;
	};

	// A smooth metal sphere with the bumpy normal map, lit by a sun alone (a metal reflects no constant ambient), filling most of
	// a 256 x 256 view.
	SphereImage RenderBumpySphere(GPUContext& gpu, AssetHandle material, bool specularAntiAliasing)
	{
		Scene scene;
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::normalize(glm::vec3(0.3f, -0.4f, -1.0f)), 3.0f, false);
		AddMesh(scene, BuiltinAssets::SphereMesh, material, glm::vec3(0.0f), "Sphere");
		SceneRenderer renderer = CreateSpecularRenderer("SpecularAA", specularAntiAliasing);
		renderer.SetViewportSize(c_SpecularViewSize, c_SpecularViewSize);
		RenderSettled(gpu, renderer, scene, LookAt(glm::vec3(0.0f, 0.0f, 1.6f), glm::vec3(0.0f)), false);
		return { ReadHDR(renderer.GetHDRTexture()), ReadIDs(renderer) };
	}

	// An equirectangular sky of a checker of `cell` texels each way, alternating 0.1 and 1 nit: detail as fine as the
	// prefiltered sky's texels.
	Ref<Texture> CreateCheckerSky(uint32_t width, uint32_t cell)
	{
		const uint32_t height = width / 2;
		TextureMip level0;
		level0.Width = width;
		level0.Height = height;
		level0.Data.resize(static_cast<size_t>(width) * height * 8);
		for (uint32_t y = 0; y < height; y++)
		{
			for (uint32_t x = 0; x < width; x++)
			{
				const float value = ((x / cell) + (y / cell)) % 2 == 0 ? 1.0f : 0.1f;
				const uint16_t texel[4] = { glm::packHalf1x16(value), glm::packHalf1x16(value), glm::packHalf1x16(value), glm::packHalf1x16(1.0f) };
				std::memcpy(level0.Data.data() + (static_cast<size_t>(y) * width + x) * sizeof(texel), texel, sizeof(texel));
			}
		}
		std::vector<TextureMip> mips = { level0 };
		REQUIRE(TextureUtils::GenerateMips(mips, TextureFormat::RGBA16F, false));
		TextureSpecification specification;
		specification.Format = TextureFormat::RGBA16F;
		specification.Wrap = TextureWrap::Clamp;
		Ref<Texture> texture = Texture::Create(specification, std::move(mips));
		REQUIRE(texture);
		return texture;
	}

	RendererQualitySettings WithEnvironmentResolution(uint32_t size)
	{
		RendererQualitySettings quality = RendererQuality::GetPreset(RendererQualityPreset::Medium);
		quality.Preset = RendererQualityPreset::Custom;
		quality.EnvironmentResolution = size;
		return quality;
	}

	// The squared luminance differences of horizontally and vertically adjacent pixels: how much detail an image holds.
	double GradientEnergy(const std::vector<glm::vec3>& image, uint32_t size)
	{
		double energy = 0.0;
		for (uint32_t y = 0; y + 1 < size; y++)
		{
			for (uint32_t x = 0; x + 1 < size; x++)
			{
				const float center = Luminance(image[static_cast<size_t>(y) * size + x]);
				const float right = Luminance(image[static_cast<size_t>(y) * size + x + 1]);
				const float below = Luminance(image[static_cast<size_t>(y + 1) * size + x]);
				energy += static_cast<double>((right - center) * (right - center) + (below - center) * (below - center));
			}
		}
		return energy;
	}

	// A mirror floor reflecting the checker sky, seen at 45 degrees through a narrow view (15 degrees), so the sky's texels are
	// magnified on screen: what the prefiltered sky's size decides.
	std::vector<glm::vec3> RenderMirror(GPUContext& gpu, SceneTestAssets& assets, AssetHandle sky, uint32_t environmentSize)
	{
		Scene scene;
		AddNeutralPostProcess(scene);
		SkyLightComponent& skyLight = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
		skyLight.EnvironmentMap = sky;
		skyLight.ShowBackground = false;
		MaterialProperties mirror;
		mirror.Metallic = 1.0f;
		mirror.Roughness = 0.0f;
		AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(mirror), glm::vec3(0.0f), "Mirror").GetComponent<TransformComponent>().Scale = glm::vec3(50.0f);
		// Specular anti-aliasing off: a flat mirror has no normal variation to widen, and this measures the map alone.
		SceneRenderer renderer = CreateSpecularRenderer("Mirror", false);
		renderer.SetQuality(WithEnvironmentResolution(environmentSize));
		renderer.SetViewportSize(c_SpecularViewSize, c_SpecularViewSize);
		SceneCamera camera = LookAt(glm::vec3(0.0f, 2.0f, 2.0f), glm::vec3(0.0f));
		camera.VerticalFOV = glm::radians(15.0f);
		camera.Projection = Math::PerspectiveReverseZ(camera.VerticalFOV, 1.0f, camera.Near, camera.Far);
		RenderSettled(gpu, renderer, scene, camera, true);
		REQUIRE(renderer.GetPrefilteredMap());
		CHECK(renderer.GetPrefilteredMap()->getDesc().width == environmentSize);
		CHECK(renderer.GetPrefilteredMap()->getDesc().mipLevels == 6);
		return ReadHDR(renderer.GetHDRTexture());
	}

}

TEST_SUITE("GPU.SceneRenderer.Materials")
{
	TEST_CASE("Specular anti-aliasing lowers a bumpy sphere's highlight variance and keeps its light")
	{
		// A smooth metal (roughness 0.1) whose normal map turns every two texels: without the widening its highlight breaks into
		// single-pixel glints; with it, each pixel's lobe covers the normals its footprint holds, so the glints even out and
		// the sphere reflects as much of the sun.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadMeshes(assets);
		MaterialProperties bumpy;
		bumpy.BaseColor = glm::vec4(0.9f, 0.9f, 0.9f, 1.0f);
		bumpy.Metallic = 1.0f;
		bumpy.Roughness = 0.1f;
		bumpy.NormalMap = assets.AddBumpyNormalMap(256);
		const AssetHandle material = assets.AddMaterial(bumpy);
		const SphereImage off = RenderBumpySphere(gpu, material, false);
		const SphereImage on = RenderBumpySphere(gpu, material, true);
		REQUIRE(off.IDs == on.IDs);

		float peakOn = 0.0f;
		double sphereOff = 0.0, sphereOn = 0.0;
		for (size_t index = 0; index < off.HDR.size(); index++)
		{
			if (off.IDs[index] == 0)
				continue;
			peakOn = std::max(peakOn, Luminance(on.HDR[index]));
			sphereOff += Luminance(off.HDR[index]);
			sphereOn += Luminance(on.HDR[index]);
		}
		REQUIRE(peakOn > 0.0f);
		// The highlight: the pixels where the widened image is above 2% of its brightest, the footprint of the lobes the glints
		// sample. (The image without the widening peaks in a single glint, so its own half maximum holds one pixel.)
		double sumOff = 0.0, sumOn = 0.0, squaresOff = 0.0, squaresOn = 0.0;
		size_t highlight = 0;
		for (size_t index = 0; index < off.HDR.size(); index++)
		{
			if (off.IDs[index] == 0 || Luminance(on.HDR[index]) <= 0.02f * peakOn)
				continue;
			const double luminanceOff = Luminance(off.HDR[index]);
			const double luminanceOn = Luminance(on.HDR[index]);
			sumOff += luminanceOff;
			sumOn += luminanceOn;
			squaresOff += luminanceOff * luminanceOff;
			squaresOn += luminanceOn * luminanceOn;
			highlight++;
		}
		REQUIRE(highlight >= 20);
		const double count = static_cast<double>(highlight);
		const double varianceOff = squaresOff / count - (sumOff / count) * (sumOff / count);
		const double varianceOn = squaresOn / count - (sumOn / count) * (sumOn / count);
		CAPTURE(highlight);
		CAPTURE(varianceOff);
		CAPTURE(varianceOn);
		CAPTURE(sphereOff);
		CAPTURE(sphereOn);
		CHECK(varianceOn <= 0.5 * varianceOff);
		// The sphere's light: within 15% of the light without the widening.
		CHECK(sphereOn >= 0.85 * sphereOff);
		CHECK(sphereOn <= 1.15 * sphereOff);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The surface target holds the roughness the forward pass shades")
	{
		// Screen-space reflections trace the prepass's surface target: a flat plane keeps its material's roughness (to the
		// target's half-float precision), a bumpy one the widened roughness the forward pass shades it with.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadMeshes(assets);
		Scene scene;
		AddTemporalPostProcess(scene, TemporalAntiAliasingPreset::Original).ScreenSpaceReflections = true;
		AddSun(scene, glm::normalize(glm::vec3(0.3f, -1.0f, -0.4f)), 3.0f, false);
		MaterialProperties flat;
		flat.Roughness = 0.3f;
		MaterialProperties bumpy = flat;
		bumpy.NormalMap = assets.AddBumpyNormalMap(256);
		AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(flat), glm::vec3(-1.0f, 0.0f, 0.0f), "Flat").GetComponent<TransformComponent>().Scale = glm::vec3(2.0f);
		AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(bumpy), glm::vec3(1.0f, 0.0f, 0.0f), "Bumpy").GetComponent<TransformComponent>().Scale = glm::vec3(2.0f);
		SceneRenderer renderer = CreateSpecularRenderer("Surface", true, true);
		renderer.SetViewportSize(c_SpecularViewSize, c_SpecularViewSize);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 3.0f, 0.01f), glm::vec3(0.0f));
		// Screen-space reflections trace from the second frame on (they reflect the TAA's history).
		for (int frame = 0; frame < 3; frame++)
		{
			REQUIRE(renderer.Render(scene, camera));
			FinishFrame(gpu);
		}
		REQUIRE(renderer.GetSurfaceTexture());
		const std::vector<glm::vec4> surface = ReadHDRA(renderer.GetSurfaceTexture());
		REQUIRE(surface.size() == static_cast<size_t>(c_SpecularViewSize) * c_SpecularViewSize);
		// The planes' centers: a quarter of the view left and right of the middle row's center.
		const float half = glm::unpackHalf1x16(glm::packHalf1x16(0.3f));
		const float flatRoughness = surface[static_cast<size_t>(c_SpecularViewSize / 2) * c_SpecularViewSize + c_SpecularViewSize / 4].z;
		CHECK(flatRoughness == doctest::Approx(half).epsilon(0.001));
		double bumpyRoughness = 0.0;
		uint32_t samples = 0;
		for (uint32_t y = c_SpecularViewSize / 2 - 16; y < c_SpecularViewSize / 2 + 16; y++)
		{
			for (uint32_t x = 3 * c_SpecularViewSize / 4 - 16; x < 3 * c_SpecularViewSize / 4 + 16; x++)
			{
				bumpyRoughness += surface[static_cast<size_t>(y) * c_SpecularViewSize + x].z;
				samples++;
			}
		}
		bumpyRoughness /= samples;
		CAPTURE(bumpyRoughness);
		CHECK(bumpyRoughness >= 0.35);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Normal-mapped reflections fade below the geometric surface")
	{
		// A mirror-like metal plane whose normal map tilts every normal 60 degrees away from a camera 70 degrees up: the
		// reflected direction points 50 degrees below the plane, at a sky bright only below the horizon. Without the fade the
		// pixel would reflect that light, the bottom's 4 nits times the metal's reflectance (above 0.9); with it, at most 0.3 of
		// that.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadMeshes(assets);
		const AssetHandle sky = assets.Add(CreateEnvironmentTexture(glm::vec3(0.0f), glm::vec3(4.0f)), "Sky");
		Scene scene;
		AddNeutralPostProcess(scene);
		SkyLightComponent& skyLight = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
		skyLight.EnvironmentMap = sky;
		skyLight.ShowBackground = false;
		// The plane's tangent is +X: a normal tilted toward -X (x = -sin 60, z = cos 60 in tangent space).
		std::vector<uint8_t> tilted;
		for (int texel = 0; texel < 4; texel++)
			AppendPixel(tilted, 17, 128, 191, 255);
		MaterialProperties metal;
		metal.Metallic = 1.0f;
		metal.Roughness = 0.1f;
		metal.NormalMap = assets.AddTexture(2, 2, tilted, TextureFormat::RGBA8);
		AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(metal), glm::vec3(0.0f), "Plane").GetComponent<TransformComponent>().Scale = glm::vec3(10.0f);
		SceneRenderer renderer = CreateSpecularRenderer("HorizonFade", true);
		renderer.SetViewportSize(c_SpecularViewSize, c_SpecularViewSize);
		const glm::vec3 toCamera(std::cos(glm::radians(70.0f)), std::sin(glm::radians(70.0f)), 0.0f);
		RenderSettled(gpu, renderer, scene, LookAt(toCamera * 3.0f, glm::vec3(0.0f)), true);
		const std::vector<glm::vec3> image = ReadHDR(renderer.GetHDRTexture());
		const glm::vec3 center = image[static_cast<size_t>(c_SpecularViewSize / 2) * c_SpecularViewSize + c_SpecularViewSize / 2];
		CAPTURE(center);
		CHECK(Luminance(center) <= 0.3f * 4.0f * 0.9f);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}

TEST_SUITE("GPU.SceneRenderer.Lighting")
{
	TEST_CASE("At 512 the prefiltered sky is 512 per face with six levels and a mirror shows finer detail")
	{
		// High's and Ultra's EnvironmentResolution: a mirror magnifying a fine checker sky holds at least 1.5 times the detail of
		// Medium's 256 per face.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadMeshes(assets);
		const AssetHandle sky = assets.Add(CreateCheckerSky(2048, 4), "Checker");
		const std::vector<glm::vec3> coarse = RenderMirror(gpu, assets, sky, 256);
		const std::vector<glm::vec3> fine = RenderMirror(gpu, assets, sky, 512);
		const double coarseEnergy = GradientEnergy(coarse, c_SpecularViewSize);
		const double fineEnergy = GradientEnergy(fine, c_SpecularViewSize);
		CAPTURE(coarseEnergy);
		CAPTURE(fineEnergy);
		CHECK(coarseEnergy > 0.0);
		CHECK(fineEnergy >= 1.5 * coarseEnergy);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Renderers of different environment resolutions compute their own maps, and a new resolution recomputes them once")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadMeshes(assets);
		const AssetHandle sky = assets.Add(CreateEnvironmentTexture(glm::vec3(0.5f, 0.6f, 0.9f), glm::vec3(0.2f)), "Sky");
		Scene scene;
		AddNeutralPostProcess(scene);
		scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().EnvironmentMap = sky;
		AddMesh(scene, BuiltinAssets::SphereMesh, BuiltinAssets::DefaultMaterial, glm::vec3(0.0f), "Sphere");
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f));

		SceneRenderer medium = CreateSpecularRenderer("Medium", true);
		medium.SetViewportSize(64, 64);
		RenderSettled(gpu, medium, scene, camera, true);
		SceneRenderer high = CreateSpecularRenderer("High", true);
		REQUIRE(high.SetQuality(WithEnvironmentResolution(512)));
		high.SetViewportSize(64, 64);
		CHECK(RenderSettled(gpu, high, scene, camera, true) == 1);
		REQUIRE(medium.GetPrefilteredMap());
		REQUIRE(high.GetPrefilteredMap());
		CHECK(medium.GetPrefilteredMap() != high.GetPrefilteredMap());
		CHECK(medium.GetPrefilteredMap()->getDesc().width == 256);
		CHECK(high.GetPrefilteredMap()->getDesc().width == 512);
		// A renderer of the same size takes the maps over.
		SceneRenderer second = CreateSpecularRenderer("Second", true);
		second.SetViewportSize(64, 64);
		RenderSettled(gpu, second, scene, camera, true);
		CHECK(second.GetPrefilteredMap() == medium.GetPrefilteredMap());

		// The Medium renderer moved to 512 takes over the maps of the renderer of that size, computing nothing.
		CHECK(medium.SetQuality(WithEnvironmentResolution(512)));
		CHECK(RenderSettled(gpu, medium, scene, camera, true) == 0);
		REQUIRE(medium.GetPrefilteredMap());
		CHECK(medium.GetPrefilteredMap()->getDesc().width == 512);
		CHECK(medium.GetPrefilteredMap() == high.GetPrefilteredMap());
		// The same resolution again changes nothing.
		CHECK_FALSE(medium.SetQuality(WithEnvironmentResolution(512)));

		// The atmosphere computes its environment once at the new size too.
		Scene air;
		AddNeutralPostProcess(air);
		air.CreateEntity("Sky").AddComponent<SkyLightComponent>().Source = SkyLightSource::Atmosphere;
		AddSun(air, glm::normalize(glm::vec3(0.3f, -0.6f, -0.5f)), 3.0f, false);
		SceneRenderer atmosphere = CreateSpecularRenderer("Atmosphere", true);
		atmosphere.SetViewportSize(64, 64);
		RenderSettled(gpu, atmosphere, air, camera, true);
		REQUIRE(atmosphere.GetPrefilteredMap());
		CHECK(atmosphere.GetPrefilteredMap()->getDesc().width == 256);
		CHECK(atmosphere.SetQuality(WithEnvironmentResolution(512)));
		CHECK(RenderSettled(gpu, atmosphere, air, camera, true) == 1);
		REQUIRE(atmosphere.GetPrefilteredMap());
		CHECK(atmosphere.GetPrefilteredMap()->getDesc().width == 512);
		// And only once: the next frames light with it.
		for (int frame = 0; frame < 3; frame++)
		{
			REQUIRE(atmosphere.Render(air, camera));
			FinishFrame(gpu);
			CHECK(atmosphere.GetStats().EnvironmentUpdates == 0);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
