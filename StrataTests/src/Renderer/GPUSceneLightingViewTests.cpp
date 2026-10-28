#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/RendererComponents.h"

#include <array>
#include <cmath>
#include <optional>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = 96; // Three quads of 32 pixels side by side

	constexpr std::array<GPUDrivenMode, 2> c_Paths = { GPUDrivenMode::Off, GPUDrivenMode::Auto };

	float EncodeSRGB(float linear)
	{
		return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
	}

	// The display byte of a color channel the overlay writes (sRGB values, unorm).
	int ToByte(float value)
	{
		return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
	}

	Scope<SceneRenderer> CreateRenderer(GPUDrivenMode path)
	{
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		specification.GPUDriven = path;
		Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(specification);
		renderer->SetViewportSize(c_Size, c_Size);
		return renderer;
	}

	// Renders a frame with `options` and reads the output back.
	ReadbackImage RenderWith(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera, const SceneRenderOptions& options)
	{
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
		return image;
	}

	ReadbackImage ReadHDR(SceneRenderer& renderer)
	{
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), image));
		return image;
	}

	// Renders frames with the view until its statistics of a frame drawn with it arrived (they come back frames later).
	const LightingViewStats& RenderUntilMeasured(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera, const SceneRenderOptions& options)
	{
		for (int frame = 0; frame < 8 && !renderer.GetLightingViewStats(); frame++)
		{
			REQUIRE(renderer.Render(scene, camera, nullptr, options));
			renderer.ResolveGPUStats(true);
		}
		REQUIRE(renderer.GetLightingViewStats());
		CHECK(renderer.GetLightingViewStats()->View == options.LightingView);
		return *renderer.GetLightingViewStats();
	}

	// A quad of `size` (x by z) at `center` on the ground, facing up.
	Entity AddQuad(Scene& scene, AssetHandle material, const glm::vec3& center, const glm::vec2& size)
	{
		Entity quad = AddMesh(scene, BuiltinAssets::PlaneMesh, material, center, "Quad");
		quad.GetComponent<TransformComponent>().Scale = glm::vec3(size.x, 1.0f, size.y);
		return quad;
	}

	// Looks straight down at the ground, showing 3 by 3 meters.
	SceneCamera TopView()
	{
		return OrthographicLookAt(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f), 3.0f);
	}

	MaterialProperties Matte(float albedo)
	{
		MaterialProperties properties;
		properties.BaseColor = glm::vec4(albedo, albedo, albedo, 1.0f);
		properties.Roughness = 0.5f;
		return properties;
	}

}

TEST_SUITE("GPU.SceneRenderer.LightingViews")
{
	TEST_CASE("Without a lighting view nothing is made and the image is unchanged")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddQuad(scene, assets.AddMaterial(Matte(0.5f)), glm::vec3(0.0f), glm::vec2(3.0f));
		AddMesh(scene, BuiltinAssets::SphereMesh, UUID::Null(), glm::vec3(0.0f, 0.5f, 0.0f), "Sphere");
		AddSun(scene, glm::vec3(0.3f, -1.0f, 0.2f), 2.0f);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 2.0f, 2.5f), glm::vec3(0.0f));
		for (GPUDrivenMode path : c_Paths)
		{
			CAPTURE(static_cast<int>(path));
			Scope<SceneRenderer> neverScope = CreateRenderer(path);
			SceneRenderer& never = *neverScope;
			const ReadbackImage reference = RenderWith(never, scene, camera, SceneRenderOptions {});
			never.ResolveGPUStats(true);
			CHECK_FALSE(never.GetLightingViewStats());
			CHECK_FALSE(never.GetStats().LightingViewPending);

			// A renderer that showed a view and left it draws what the other draws.
			Scope<SceneRenderer> viewedScope = CreateRenderer(path);
			SceneRenderer& viewed = *viewedScope;
			SceneRenderOptions options;
			options.LightingView = SceneLightingView::Albedo;
			RenderWith(viewed, scene, camera, options);
			CHECK(viewed.GetStats().LightingViewPending);
			const ReadbackImage after = RenderWith(viewed, scene, camera, SceneRenderOptions {});
			CHECK(MaxDifference(reference, after) == 0);
			viewed.ResolveGPUStats(true);
			CHECK_FALSE(viewed.GetLightingViewStats());
			CHECK_FALSE(viewed.GetStats().LightingViewPending);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The albedo view shows the base color and marks values outside 0.03 to 0.85")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		// Three quads of a third of the view each: too dark, measured, too bright.
		AddQuad(scene, assets.AddMaterial(Matte(0.02f)), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec2(1.0f, 3.0f));
		AddQuad(scene, assets.AddMaterial(Matte(0.5f)), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec2(1.0f, 3.0f));
		AddQuad(scene, assets.AddMaterial(Matte(0.9f)), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec2(1.0f, 3.0f));
		AddSun(scene, glm::vec3(0.3f, -1.0f, 0.2f), 3.0f);
		SceneRenderOptions options;
		options.LightingView = SceneLightingView::Albedo;
		const LightingViewColors& colors = options.LightingColors;
		for (GPUDrivenMode path : c_Paths)
		{
			CAPTURE(static_cast<int>(path));
			Scope<SceneRenderer> rendererScope = CreateRenderer(path);
			SceneRenderer& renderer = *rendererScope;
			const LightingViewStats stats = RenderUntilMeasured(renderer, scene, TopView(), options);
			const ReadbackImage image = RenderWith(renderer, scene, TopView(), options);

			// The measured quad: its base color as the display shows linear 0.5.
			const glm::u8vec4 middle = GetPixelRGBA8(image, c_Size / 2, c_Size / 2);
			CHECK(std::abs(static_cast<int>(middle.r) - ToByte(EncodeSRGB(0.5f))) <= 1);
			CHECK(middle.r == middle.g);
			// The dark quad carries the low mark.
			const glm::vec3 low = glm::mix(glm::vec3(EncodeSRGB(0.02f)), glm::vec3(colors.AlbedoLow), 0.7f);
			const glm::u8vec4 dark = GetPixelRGBA8(image, c_Size / 6, c_Size / 2);
			CHECK(std::abs(static_cast<int>(dark.r) - ToByte(low.r)) <= 1);
			CHECK(std::abs(static_cast<int>(dark.b) - ToByte(low.b)) <= 1);
			// The bright quad: stripes of the high mark between its own color.
			const glm::vec3 high = glm::mix(glm::vec3(EncodeSRGB(0.9f)), glm::vec3(colors.AlbedoHigh), 0.7f);
			const uint32_t x = c_Size * 5 / 6;
			for (uint32_t y = c_Size / 2; y < c_Size / 2 + 8; y++)
			{
				const glm::u8vec4 pixel = GetPixelRGBA8(image, x, y);
				const bool striped = ((x + y) / 4) % 2 == 0;
				CAPTURE(y);
				CHECK(std::abs(static_cast<int>(pixel.g) - ToByte(striped ? high.g : EncodeSRGB(0.9f))) <= 1);
			}

			CHECK(stats.SurfaceShare == doctest::Approx(1.0f).epsilon(0.001));
			CHECK(stats.BelowRangeShare == doctest::Approx(1.0f / 3.0f).epsilon(0.01));
			CHECK(stats.AboveRangeShare == doctest::Approx(1.0f / 3.0f).epsilon(0.01));
			CHECK(stats.ValueMedian == doctest::Approx(0.5f).epsilon(0.02));
			CHECK(std::isnan(stats.SurfaceEV100.y));
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The luminance view bands EV100 and reports its percentiles")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddQuad(scene, assets.AddMaterial(Emitting(100.0f)), glm::vec3(0.0f), glm::vec2(4.0f));
		const float expectedEV = std::log2(100.0f) + 3.0f; // 9.64
		for (GPUDrivenMode path : c_Paths)
		{
			for (const float preExposure : { 1.0f, std::ldexp(1.0f, -10) })
			{
				CAPTURE(static_cast<int>(path));
				CAPTURE(preExposure);
				Scope<SceneRenderer> rendererScope = CreateRenderer(path);
				SceneRenderer& renderer = *rendererScope;
				SceneRenderOptions options;
				options.LightingView = SceneLightingView::Luminance;
				options.PreExposure = preExposure;
				const LightingViewStats stats = RenderUntilMeasured(renderer, scene, TopView(), options);
				CHECK(std::abs(stats.SurfaceEV100.y - expectedEV) <= 0.25f);
				CHECK(stats.SurfaceEV100.x <= stats.SurfaceEV100.y);
				CHECK(stats.SurfaceEV100.y <= stats.SurfaceEV100.z);
				CHECK(std::isnan(stats.BackgroundEV100));
				CHECK(std::isnan(stats.ValueMedian));

				const ReadbackImage image = RenderWith(renderer, scene, TopView(), options);
				const int band = static_cast<int>(std::floor(expectedEV - options.LightingColors.LuminanceFirstEV100));
				const glm::vec4 color = options.LightingColors.Luminance[static_cast<size_t>(band)];
				const glm::u8vec4 pixel = GetPixelRGBA8(image, c_Size / 2, c_Size / 2);
				CHECK(std::abs(static_cast<int>(pixel.r) - ToByte(color.r)) <= 1);
				CHECK(std::abs(static_cast<int>(pixel.g) - ToByte(color.g)) <= 1);
				CHECK(std::abs(static_cast<int>(pixel.b) - ToByte(color.b)) <= 1);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The AO view shows the occlusion the forward pass applies")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene).AmbientOcclusion = true;
		AddMesh(scene, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(20.0f);
		AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f, 0.5f, 0.0f), "Box");
		scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().AmbientColor = glm::vec3(0.5f);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 2.5f, 3.0f), glm::vec3(0.0f, 0.3f, 0.0f));
		const glm::uvec2 crease = ProjectToPixel(camera, glm::vec3(0.0f, 0.0f, 0.56f), c_Size);
		const glm::uvec2 open = ProjectToPixel(camera, glm::vec3(1.3f, 0.0f, 1.2f), c_Size);
		SceneRenderOptions options;
		options.LightingView = SceneLightingView::AmbientOcclusion;
		for (GPUDrivenMode path : c_Paths)
		{
			CAPTURE(static_cast<int>(path));
			Scope<SceneRenderer> rendererScope = CreateRenderer(path);
			SceneRenderer& renderer = *rendererScope;
			const LightingViewStats stats = RenderUntilMeasured(renderer, scene, camera, options);
			const ReadbackImage image = RenderWith(renderer, scene, camera, options);
			ReadbackImage occlusion;
			REQUIRE(Renderer::ReadTexture(renderer.GetAmbientOcclusionTexture(), occlusion));
			for (const glm::uvec2& pixel : { crease, open })
			{
				CAPTURE(pixel);
				const float ao = ReadOcclusionTexel(occlusion, pixel);
				CHECK(std::abs(static_cast<int>(GetPixelRGBA8(image, pixel.x, pixel.y).r) - ToByte(EncodeSRGB(ao))) <= 2);
			}
			CHECK(ReadOcclusionTexel(occlusion, crease) < 0.9f);
			CHECK(stats.ValueMedian > 0.5f);
			CHECK(stats.ValueP05 < stats.ValueMedian);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The indirect view leaves out direct light and emission")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddQuad(scene, assets.AddMaterial(Matte(0.6f)), glm::vec3(-0.75f, 0.0f, 0.0f), glm::vec2(1.5f, 3.0f));
		AddQuad(scene, assets.AddMaterial(Emitting(5.0f)), glm::vec3(0.75f, 0.0f, 0.0f), glm::vec2(1.5f, 3.0f));
		scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().AmbientColor = glm::vec3(0.25f, 0.5f, 1.0f);
		AddSun(scene, glm::vec3(0.0f, -1.0f, 0.1f), 10.0f);
		SceneRenderOptions options;
		options.LightingView = SceneLightingView::Indirect;
		options.PreExposure = 1.0f;
		for (GPUDrivenMode path : c_Paths)
		{
			CAPTURE(static_cast<int>(path));
			Scope<SceneRenderer> rendererScope = CreateRenderer(path);
			SceneRenderer& renderer = *rendererScope;
			const LightingViewStats stats = RenderUntilMeasured(renderer, scene, TopView(), options);
			const ReadbackImage hdr = ReadHDR(renderer);
			const glm::vec4 lit = ReadHalfTexel(hdr, c_Size / 4, c_Size / 2);
			CHECK(lit.r == doctest::Approx(0.6f * 0.25f).epsilon(0.01));
			CHECK(lit.g == doctest::Approx(0.6f * 0.5f).epsilon(0.01));
			CHECK(lit.b == doctest::Approx(0.6f * 1.0f).epsilon(0.01));
			const glm::vec4 emitter = ReadHalfTexel(hdr, c_Size * 3 / 4, c_Size / 2);
			CHECK(emitter.r == 0.0f);
			CHECK(emitter.g == 0.0f);
			CHECK(emitter.b == 0.0f);
			// The sun (straight down, after no atmosphere) and the ambient as illuminance on flat ground.
			const float sunUp = 1.0f / std::sqrt(1.01f);
			CHECK(stats.SunIlluminance == doctest::Approx(10.0f * sunUp).epsilon(0.01));
			const float ambient = 0.2126f * 0.25f + 0.7152f * 0.5f + 0.0722f;
			CHECK(stats.SkyIlluminance == doctest::Approx(glm::pi<float>() * ambient).epsilon(0.01));
			CHECK(stats.SunToSkyRatio == doctest::Approx(stats.SunIlluminance / stats.SkyIlluminance).epsilon(0.001));
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The lighting view shades with white albedo")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const auto render = [&](float albedo, SceneLightingView view, GPUDrivenMode path)
		{
			Scene scene;
			AddNeutralPostProcess(scene);
			AddQuad(scene, assets.AddMaterial(Matte(albedo)), glm::vec3(0.0f), glm::vec2(4.0f));
			AddSun(scene, glm::vec3(0.3f, -1.0f, 0.2f), 3.0f, false);
			scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().AmbientColor = glm::vec3(0.1f);
			Scope<SceneRenderer> rendererScope = CreateRenderer(path);
			SceneRenderer& renderer = *rendererScope;
			SceneRenderOptions options;
			options.LightingView = view;
			options.PreExposure = 1.0f;
			const SceneCamera camera = LookAt(glm::vec3(0.0f, 2.0f, 1.5f), glm::vec3(0.0f));
			REQUIRE(renderer.Render(scene, camera, nullptr, options));
			return ReadHalfTexel(ReadHDR(renderer), c_Size / 2, c_Size / 2);
		};
		for (GPUDrivenMode path : c_Paths)
		{
			CAPTURE(static_cast<int>(path));
			const glm::vec4 white = render(1.0f, SceneLightingView::None, path);
			const glm::vec4 view = render(0.2f, SceneLightingView::Lighting, path);
			const glm::vec4 normal = render(0.2f, SceneLightingView::None, path);
			CHECK(white.r > 0.1f);
			CHECK(view.r == doctest::Approx(white.r).epsilon(0.001));
			CHECK(view.g == doctest::Approx(white.g).epsilon(0.001));
			CHECK(view.b == doctest::Approx(white.b).epsilon(0.001));
			CHECK(normal.r < 0.5f * view.r);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The sun to sky ratio is measured on flat ground")
	{
		// Strata 0.6.0's Basic 3D template: the procedural sky's colors of then and a 3 lux sun 55 degrees up. The lighting audit
		// read 1.6 from a sunlit and a shaded pixel, which the sky's specular reflection brightens in both; on flat ground the
		// illuminances are the sun's 3 sin(55) = 2.46 lux and the sky's pi times its cosine-weighted mean radiance: the gradient
		// (IBL/ProceduralSky.comp) is the zenith's color but within (1 - h)^8 of the horizon, a mean of zenith + (horizon -
		// zenith) / 45, 0.321 nits, so 1.01 lux and a ratio of 2.43.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddQuad(scene, assets.AddMaterial(Matte(0.5f)), glm::vec3(0.0f), glm::vec2(4.0f));
		SkyLightComponent& sky = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
		sky.Source = SkyLightSource::Procedural;
		sky.ZenithColor = glm::vec3(0.18f, 0.32f, 0.62f);
		sky.HorizonColor = glm::vec3(0.66f, 0.74f, 0.84f);
		sky.GroundColor = glm::vec3(0.22f, 0.20f, 0.18f);
		const float elevation = glm::radians(55.0f);
		const float azimuth = glm::radians(20.0f);
		const glm::vec3 towardSun(std::cos(elevation) * std::cos(azimuth), std::sin(elevation), std::cos(elevation) * std::sin(azimuth));
		AddSun(scene, -towardSun, 3.0f);
		SceneRenderOptions options;
		options.LightingView = SceneLightingView::Luminance;
		for (GPUDrivenMode path : c_Paths)
		{
			CAPTURE(static_cast<int>(path));
			Scope<SceneRenderer> rendererScope = CreateRenderer(path);
			SceneRenderer& renderer = *rendererScope;
			const LightingViewStats stats = RenderUntilMeasured(renderer, scene, TopView(), options);
			CHECK(stats.SunIlluminance == doctest::Approx(3.0f * std::sin(elevation)).epsilon(0.01));
			CHECK(stats.SkyIlluminance > 0.0f);
			const float zenith = 0.2126f * 0.18f + 0.7152f * 0.32f + 0.0722f * 0.62f;
			const float horizon = 0.2126f * 0.66f + 0.7152f * 0.74f + 0.0722f * 0.84f;
			CHECK(stats.SkyIlluminance == doctest::Approx(glm::pi<float>() * (zenith + (horizon - zenith) / 45.0f)).epsilon(0.03));
			CHECK(stats.SunToSkyRatio == doctest::Approx(2.43f).epsilon(0.03));
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Changing the view resets TAA and leaves the exposure where it was")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		PostProcessComponent& postProcess = AddTemporalPostProcess(scene, TemporalAntiAliasingPreset::Sharp);
		postProcess.AutoExposure = true;
		// Adapts at once and without a limit: a metering of the view's values (0.5 over the pre-exposure: about four stops
		// above the sunlit ground's luminance) would move the pre-exposure within a frame or two.
		postProcess.AutoExposureSpeed = 0.0f;
		postProcess.AutoExposureMaxEV = 24.0f;
		AddQuad(scene, assets.AddMaterial(Matte(0.5f)), glm::vec3(0.0f), glm::vec2(10.0f));
		// Daylight: the metered pre-exposure is far from 1, where the albedo view's unexposed values would pull it.
		AddSun(scene, glm::vec3(0.3f, -1.0f, 0.2f), 20000.0f, false);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 2.0f, 1.5f), glm::vec3(0.0f));
		for (GPUDrivenMode path : c_Paths)
		{
			CAPTURE(static_cast<int>(path));
			Scope<SceneRenderer> rendererScope = CreateRenderer(path);
			SceneRenderer& renderer = *rendererScope;
			SceneRenderOptions lit;
			for (int frame = 0; frame < 32 && !(renderer.GetStats().PreExposureSettled && frame > 4); frame++)
			{
				REQUIRE(renderer.Render(scene, camera, nullptr, lit));
				renderer.ResolveGPUStats(true);
			}
			REQUIRE(renderer.GetStats().PreExposureSettled);
			REQUIRE(renderer.GetStats().TemporalAntiAliasing);
			const float preExposure = renderer.GetStats().PreExposure;
			CHECK(preExposure < 1.0f / 256.0f);
			const uint32_t resets = renderer.GetStats().HistoryResets;

			SceneRenderOptions albedo;
			albedo.LightingView = SceneLightingView::Albedo;
			for (int frame = 0; frame < 5; frame++)
			{
				REQUIRE(renderer.Render(scene, camera, nullptr, albedo));
				renderer.ResolveGPUStats(true);
				CAPTURE(frame);
				CHECK(renderer.GetStats().PreExposure == preExposure);
				CHECK(renderer.GetStats().PreExposureSettled);
			}
			REQUIRE(renderer.Render(scene, camera, nullptr, lit));
			CHECK(renderer.GetStats().PreExposure == preExposure);
			CHECK(renderer.GetStats().HistoryResets == resets + 2);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
