#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/RendererQuality.h"
#include "Strata/Renderer/ShadowAtlasAllocator.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = 96;

	// A floor and a box lit by a sun from the side: the box's shadow falls on the floor at -X, the floor at +X is lit.
	struct QualityScene
	{
		Scene SceneData;
		SceneCamera Camera;
		glm::vec3 Shadowed = glm::vec3(-0.8f, 0.0f, 0.3f); // Within the shadow (-1.4 to 0.2 along X) by more than a cascade texel
		glm::vec3 Lit = glm::vec3(1.5f, 0.0f, 0.0f);

		QualityScene()
		{
			AddNeutralPostProcess(SceneData);
			Entity floor = AddMesh(SceneData, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor");
			floor.GetComponent<TransformComponent>().Scale = glm::vec3(20.0f);
			AddMesh(SceneData, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f, 1.0f, 0.0f), "Box");
			Entity sun = AddSun(SceneData, glm::normalize(glm::vec3(-0.6f, -1.0f, 0.0f)), 3.0f);
			sun.GetComponent<DirectionalLightComponent>().ShadowSoftness = 0.0f;
			Camera = LookAt(glm::vec3(0.0f, 8.0f, 8.0f), glm::vec3(0.0f));
			Camera.Projection = Math::PerspectiveReverseZ(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
			Camera.VerticalFOV = glm::radians(60.0f);
			Camera.Far = 100.0f;
		}

		// The box's shadow shows: the shadowed floor is far darker than the lit floor.
		void CheckShadow(const ReadbackImage& image) const
		{
			const int shadowed = Red(image, ProjectToPixel(Camera, Shadowed, c_Size));
			const int lit = Red(image, ProjectToPixel(Camera, Lit, c_Size));
			CAPTURE(shadowed);
			CAPTURE(lit);
			CHECK(lit > 40);
			CHECK(shadowed * 2 < lit);
		}
	};

	SceneRendererSpecification MakeSpecification(GPUDrivenMode mode)
	{
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		specification.GPUDriven = mode;
		return specification;
	}

	uint32_t GetShadowMapLayers(const SceneRenderer& renderer)
	{
		nvrhi::ITexture* shadowMap = renderer.GetShadowMap();
		REQUIRE(shadowMap);
		return shadowMap->getDesc().arraySize;
	}

	uint32_t GetShadowMapWidth(const SceneRenderer& renderer)
	{
		nvrhi::ITexture* shadowMap = renderer.GetShadowMap();
		REQUIRE(shadowMap);
		return shadowMap->getDesc().width;
	}

	// The quality scene with a shadowed point light beside the box: the renderer makes both the cascades and the atlas.
	void AddShadowedLamp(QualityScene& scene)
	{
		Entity lamp = AddPointLight(scene.SceneData, glm::vec3(1.5f, 2.0f, 1.0f), glm::vec3(1.0f, 0.8f, 0.6f), 30.0f, 8.0f);
		lamp.GetComponent<PointLightComponent>().CastShadows = true;
	}

	ReadbackImage RenderFrames(SceneRenderer& renderer, QualityScene& scene, int frames)
	{
		ReadbackImage image;
		for (int frame = 0; frame < frames; frame++)
			image = RenderToImage(renderer, scene.SceneData, scene.Camera);
		return image;
	}

	// A quality that differs from Medium in every knob that sizes something.
	RendererQualitySettings MakeCostlyQuality()
	{
		RendererQualitySettings quality = RendererQuality::GetPreset(RendererQualityPreset::Medium);
		quality.Preset = RendererQualityPreset::Custom;
		quality.ShadowResolution = ShadowMapResolution::Size4096;
		quality.ShadowCascades = 2;
		quality.LocalShadowAtlasSize = 2048;
		quality.LocalShadowMaxTile = 512;
		return quality;
	}

}

TEST_SUITE("GPU.SceneRenderer.Quality")
{
	TEST_CASE("The cascades' depth array has a layer per cascade the quality draws")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::PlaneMesh));
		QualityScene scene;
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			for (const uint32_t cascades : { 1u, 2u, 4u })
			{
				CAPTURE(cascades);
				SceneRendererSpecification specification = MakeSpecification(mode);
				specification.Quality.ShadowCascades = cascades;
				SceneRenderer renderer(specification);
				renderer.SetViewportSize(c_Size, c_Size);
				CHECK(renderer.GetShadowMap() == nullptr); // Made by the first frame with a shadowed sun
				// Two frames: the GPU-driven path's second culls the cascades against their pyramids (a layer each).
				ReadbackImage image;
				for (int frame = 0; frame < 2; frame++)
					image = RenderToImage(renderer, scene.SceneData, scene.Camera);
				CHECK(GetShadowMapLayers(renderer) == cascades);
				CHECK(renderer.GetShadowMap()->getDesc().width == 2048);
				scene.CheckShadow(image);
			}
		}
		// Thumbnails draw one 1024-texel cascade: a quarter of 0.6.0's four layers.
		SceneRenderer thumbnails(SceneRendererSpecification::ForThumbnails());
		thumbnails.SetViewportSize(c_Size, c_Size);
		scene.CheckShadow(RenderToImage(thumbnails, scene.SceneData, scene.Camera));
		CHECK(GetShadowMapLayers(thumbnails) == 1);
		CHECK(thumbnails.GetShadowMap()->getDesc().width == 1024);
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Renderers of single images release their cascades with their targets")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::PlaneMesh));
		QualityScene scene;
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			SceneRenderer renderer(MakeSpecification(mode));
			renderer.SetViewportSize(c_Size, c_Size);
			ReadbackImage first;
			for (int frame = 0; frame < 2; frame++)
				first = RenderToImage(renderer, scene.SceneData, scene.Camera);
			REQUIRE(renderer.GetShadowMap());

			// A resize keeps them (only the viewport's targets follow its size).
			nvrhi::ITexture* kept = renderer.GetShadowMap();
			renderer.SetViewportSize(c_Size / 2, c_Size / 2);
			CHECK(renderer.GetShadowMap() == kept);

			// Released altogether (a capture renderer between pictures, a minimized game): the cascades go too, and the next
			// frame makes them again and draws the same picture.
			renderer.SetViewportSize(0, 0);
			CHECK(renderer.GetShadowMap() == nullptr);
			renderer.SetViewportSize(c_Size, c_Size);
			ReadbackImage again;
			for (int frame = 0; frame < 2; frame++)
				again = RenderToImage(renderer, scene.SceneData, scene.Camera);
			REQUIRE(renderer.GetShadowMap());
			CHECK(GetShadowMapLayers(renderer) == 4);
			scene.CheckShadow(again);
			CHECK(MaxDifference(first, again) == 0);
		}
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("SetQuality makes again only what a changed knob sizes")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::PlaneMesh));
		QualityScene scene;
		AddShadowedLamp(scene);
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			SceneRendererSpecification specification = MakeSpecification(mode);
			specification.SpreadLightingUpdates = false;
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(c_Size, c_Size);
			RenderFrames(renderer, scene, 2);
			nvrhi::ITexture* shadowMap = renderer.GetShadowMap();
			nvrhi::ITexture* atlas = renderer.GetLocalShadowAtlas();
			REQUIRE(shadowMap);
			REQUIRE(atlas);
			const auto checkKept = [&]()
			{
				CHECK(renderer.GetShadowMap() == shadowMap);
				CHECK(renderer.GetLocalShadowAtlas() == atlas);
			};

			// The same knobs under another label: nothing changes but the label.
			RendererQualitySettings quality = renderer.GetQuality();
			quality.Preset = RendererQualityPreset::Custom;
			CHECK_FALSE(renderer.SetQuality(quality));
			CHECK(renderer.GetQuality().Preset == RendererQualityPreset::Custom);
			checkKept();
			RenderFrames(renderer, scene, 1);
			checkKept();
			CHECK(renderer.GetStats().LocalShadowViewsDrawn == 0); // The lamp's views are cached

			// Knobs that size nothing apply from the next frame. These two leave the lamp's views as they are.
			quality.MinProjectedPixels = 1.0f;
			quality.MaxShadowedLocalLights = 8;
			CHECK_FALSE(renderer.SetQuality(quality));
			CHECK(renderer.GetQuality() == quality);
			scene.CheckShadow(RenderFrames(renderer, scene, 1));
			checkKept();
			CHECK(renderer.GetStats().LocalShadowViewsDrawn == 0);
			// A new level-of-detail error draws the lamp's six views again in their tiles, once: they chose their casters'
			// levels by it.
			quality.LODErrorPixels = 2.0f;
			CHECK_FALSE(renderer.SetQuality(quality));
			CHECK(renderer.GetQuality() == quality);
			scene.CheckShadow(RenderFrames(renderer, scene, 1));
			checkKept();
			CHECK(renderer.GetStats().LocalShadowViewsDrawn == 6);
			RenderFrames(renderer, scene, 1);
			CHECK(renderer.GetStats().LocalShadowViewsDrawn == 0);

			// Fewer cascades: a new array of their layers, the atlas kept.
			quality.ShadowCascades = 2;
			CHECK(renderer.SetQuality(quality));
			CHECK(renderer.GetShadowMap() == nullptr);
			CHECK(renderer.GetLocalShadowAtlas() == atlas);
			scene.CheckShadow(RenderFrames(renderer, scene, 1));
			CHECK(GetShadowMapLayers(renderer) == 2);
			CHECK(GetShadowMapWidth(renderer) == 2048);
			CHECK(renderer.GetLocalShadowAtlas() == atlas);

			// A finer resolution: a new array of that size.
			quality.ShadowResolution = ShadowMapResolution::Size4096;
			CHECK(renderer.SetQuality(quality));
			CHECK(renderer.GetShadowMapSize() == 4096);
			scene.CheckShadow(RenderFrames(renderer, scene, 1));
			CHECK(GetShadowMapWidth(renderer) == 4096);
			CHECK(GetShadowMapLayers(renderer) == 2);
			CHECK(renderer.GetLocalShadowAtlas() == atlas);
			shadowMap = renderer.GetShadowMap();

			// A smaller atlas: a new atlas, the cascades kept.
			quality.LocalShadowAtlasSize = 2048;
			CHECK(renderer.SetQuality(quality));
			CHECK(renderer.GetLocalShadowAtlas() == nullptr);
			CHECK(renderer.GetShadowMap() == shadowMap);
			scene.CheckShadow(RenderFrames(renderer, scene, 1));
			REQUIRE(renderer.GetLocalShadowAtlas());
			CHECK(renderer.GetLocalShadowAtlas()->getDesc().width == 2048);
			CHECK(renderer.GetShadowMap() == shadowMap);
			atlas = renderer.GetLocalShadowAtlas();

			// A tile cap draws the views again in the same atlas.
			quality.LocalShadowMaxTile = 512;
			CHECK_FALSE(renderer.SetQuality(quality));
			RenderFrames(renderer, scene, 1);
			checkKept();
			CHECK(renderer.GetStats().LocalShadowLights == 1);
			CHECK(renderer.GetStats().LocalShadowViewsPending == 0);
		}
		// Validation stays clean: everything released was released after the frames that used it.
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Changing the quality and back draws the same image")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::PlaneMesh));
		QualityScene scene;
		AddShadowedLamp(scene);
		std::vector<ReadbackImage> firstImages;
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			SceneRendererSpecification specification = MakeSpecification(mode);
			// Every local shadow view is drawn in the frame after the views were reset.
			specification.SpreadLightingUpdates = false;
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(c_Size, c_Size);
			const ReadbackImage first = RenderFrames(renderer, scene, 3);
			scene.CheckShadow(first);
			CHECK(renderer.SetQuality(MakeCostlyQuality()));
			const ReadbackImage costly = RenderFrames(renderer, scene, 3);
			scene.CheckShadow(costly);
			CHECK(renderer.SetQuality(RendererQuality::GetPreset(RendererQualityPreset::Medium)));
			const ReadbackImage again = RenderFrames(renderer, scene, 3);
			CHECK(MaxDifference(first, again) == 0);
			firstImages.push_back(first);
		}
		// The two paths agree as they do without a change (GPU.SceneRenderer.GPUDriven).
		if (Renderer::GetGraphicsDevice().GetInfo().SupportsDrawIndirectFirstInstance)
			CHECK(MaxDifference(firstImages[0], firstImages[1]) <= 1);
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The cascade cache's exact case holds across a quality change")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!Renderer::GetGraphicsDevice().GetInfo().SupportsDrawIndirectFirstInstance)
			return; // Caching is the GPU-driven path's
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::PlaneMesh));
		QualityScene scene;
		SceneRendererSpecification specification = MakeSpecification(GPUDrivenMode::Auto);
		SceneRenderer cached(specification);
		specification.CacheShadowCascades = false;
		SceneRenderer redrawn(specification);
		cached.SetViewportSize(c_Size, c_Size);
		redrawn.SetViewportSize(c_Size, c_Size);
		const auto frame = [&]()
		{
			const ReadbackImage expected = RenderToImage(redrawn, scene.SceneData, scene.Camera);
			const ReadbackImage image = RenderToImage(cached, scene.SceneData, scene.Camera);
			CHECK(MaxDifference(expected, image) <= 1);
			REQUIRE(cached.GetStats().GPUDriven);
			return cached.GetStats().CachedCascadeMask;
		};
		const uint32_t allCascades = (1u << cached.GetQuality().ShadowCascades) - 1u;
		frame();
		CHECK(frame() == allCascades);
		// A finer resolution: the cache starts again, then keeps the new cascades.
		RendererQualitySettings quality = cached.GetQuality();
		quality.ShadowResolution = ShadowMapResolution::Size4096;
		CHECK(cached.SetQuality(quality));
		CHECK(redrawn.SetQuality(quality));
		CHECK(frame() == 0u);
		CHECK(frame() == allCascades);
		// Fewer cascades: likewise.
		quality.ShadowCascades = 3;
		CHECK(cached.SetQuality(quality));
		CHECK(redrawn.SetQuality(quality));
		CHECK(frame() == 0u);
		CHECK(frame() == 0b111u);
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Auto follows the viewport's rows")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::PlaneMesh));
		QualityScene scene;
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			SceneRendererSpecification specification = MakeSpecification(mode);
			specification.Quality.Preset = RendererQualityPreset::Custom;
			specification.Quality.ShadowResolution = ShadowMapResolution::Auto;
			specification.Quality.ShadowCascades = 1; // 64 MiB at 4096
			SceneRenderer renderer(specification);
			CHECK(renderer.GetShadowMapSize() == 2048); // No rows yet: below the threshold
			const auto render = [&](uint32_t rows)
			{
				renderer.SetViewportSize(64, rows);
				REQUIRE(renderer.Render(scene.SceneData, scene.Camera));
				return GetShadowMapWidth(renderer);
			};
			CHECK(render(RendererQuality::c_HighResolutionRows - 1) == 2048);
			CHECK(renderer.GetShadowMapSize() == 2048);
			CHECK(render(RendererQuality::c_HighResolutionRows) == 4096);
			CHECK(renderer.GetShadowMapSize() == 4096);
			CHECK(render(RendererQuality::c_HighResolutionRows + 720) == 4096);
			CHECK(render(RendererQuality::c_HighResolutionRows - 1) == 2048);
			// A zero viewport releases them; the rows of the next size decide again.
			renderer.SetViewportSize(0, 0);
			CHECK(renderer.GetShadowMap() == nullptr);
			CHECK(render(RendererQuality::c_HighResolutionRows) == 4096);
		}
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The cascades' size is limited")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::PlaneMesh));
		QualityScene scene;
		SceneRendererSpecification specification = MakeSpecification(GPUDrivenMode::Auto);
		specification.Quality.Preset = RendererQualityPreset::Custom;
		specification.Quality.ShadowResolution = ShadowMapResolution::Size8192;
		specification.Quality.ShadowCascades = 1;
		// Without a limit: 8192, or the device's limit below it (nothing is made until a frame draws).
		{
			SceneRenderer unlimited(specification);
			const uint32_t deviceLimit = ShadowAtlasAllocator::FloorPowerOfTwo(Renderer::GetGraphicsDevice().GetInfo().MaxTextureDimension2D);
			CHECK(unlimited.GetShadowMapSize() == std::min(8192u, deviceLimit));
		}
		// A limit, floored to a power of two.
		for (const uint32_t limit : { 2048u, 3000u })
		{
			CAPTURE(limit);
			specification.MaxShadowMapSize = limit;
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(c_Size, c_Size);
			CHECK(renderer.GetShadowMapSize() == 2048);
			scene.CheckShadow(RenderToImage(renderer, scene.SceneData, scene.Camera));
			CHECK(GetShadowMapWidth(renderer) == 2048);
			// A quality change within the limit releases nothing; Size2048 is what the limit made.
			RendererQualitySettings quality = renderer.GetQuality();
			quality.ShadowResolution = ShadowMapResolution::Size4096;
			CHECK_FALSE(renderer.SetQuality(quality));
			CHECK(renderer.GetShadowMap() != nullptr);
			quality.ShadowResolution = ShadowMapResolution::Size1024;
			CHECK(renderer.SetQuality(quality));
			CHECK(renderer.GetShadowMapSize() == 1024);
		}
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A light's tile follows LocalShadowMaxTile")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::PlaneMesh));
		QualityScene scene;
		// A shadowed point light 1.5 m from the camera with a range of 10 m: it fills the view, and wants the largest tiles.
		const glm::vec3 eye(0.0f, 8.0f, 8.0f);
		const glm::vec3 lamp = eye + glm::normalize(glm::vec3(0.0f) - eye) * 1.5f;
		AddPointLight(scene.SceneData, lamp, glm::vec3(1.0f), 20.0f, 10.0f).GetComponent<PointLightComponent>().CastShadows = true;
		constexpr uint32_t size = 256;
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			SceneRendererSpecification specification = MakeSpecification(mode);
			specification.SpreadLightingUpdates = false;
			specification.Quality.Preset = RendererQualityPreset::Custom;
			specification.Quality.LocalShadowAtlasSize = 8192;
			specification.Quality.LocalShadowMaxTile = 512;
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(size, size);
			REQUIRE(renderer.Render(scene.SceneData, scene.Camera));
			REQUIRE(renderer.GetStats().LocalShadowLights == 1);
			CHECK(renderer.GetStats().LocalShadowTileSizes[0] == 512);
			CHECK(renderer.GetStats().LocalShadowTileSizes[1] == 0);

			// A larger cap: the light's views are drawn again in larger tiles of the same atlas (0.6.0 stopped at 1024).
			RendererQualitySettings quality = renderer.GetQuality();
			quality.LocalShadowMaxTile = 2048;
			nvrhi::ITexture* atlas = renderer.GetLocalShadowAtlas();
			CHECK_FALSE(renderer.SetQuality(quality));
			REQUIRE(renderer.Render(scene.SceneData, scene.Camera));
			REQUIRE(renderer.GetStats().LocalShadowLights == 1);
			CHECK(renderer.GetStats().LocalShadowTileSizes[0] == 2048);
			CHECK(renderer.GetStats().LocalShadowViewsPending == 0);
			CHECK(renderer.GetLocalShadowAtlas() == atlas);

			// The cap is at most the atlas.
			quality.LocalShadowAtlasSize = 1024;
			CHECK(renderer.SetQuality(quality));
			CHECK(renderer.GetQuality().LocalShadowMaxTile == 1024);
			REQUIRE(renderer.Render(scene.SceneData, scene.Camera));
			REQUIRE(renderer.GetStats().LocalShadowLights == 1);
			// Six faces of 1024 do not fit an atlas of 1024: they step down to fit.
			CHECK(renderer.GetStats().LocalShadowTileSizes[0] <= 512);
		}
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The stats report the effective quality")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::PlaneMesh));
		QualityScene scene;
		AddShadowedLamp(scene);
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			SceneRendererSpecification specification = MakeSpecification(mode);
			specification.SpreadLightingUpdates = false;
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(c_Size, c_Size);
			REQUIRE(renderer.Render(scene.SceneData, scene.Camera));
			CHECK(renderer.GetStats().Quality == RendererQualitySettings {});
			CHECK(renderer.GetStats().ShadowMapSize == 2048);
			CHECK(renderer.GetStats().LocalShadowLights == 1);
			CHECK(renderer.GetStats().LocalShadowTileSizes[0] >= 128);

			const RendererQualitySettings costly = MakeCostlyQuality();
			CHECK(renderer.SetQuality(costly));
			REQUIRE(renderer.Render(scene.SceneData, scene.Camera));
			CHECK(renderer.GetStats().Quality == costly);
			CHECK(renderer.GetStats().ShadowMapSize == 4096);
			CHECK(renderer.GetStats().LocalShadowTileSizes[0] <= 512);

			// Auto at the viewport's rows, and the label of equal knobs.
			RendererQualitySettings high = RendererQuality::GetPreset(RendererQualityPreset::High);
			CHECK(renderer.SetQuality(high));
			REQUIRE(renderer.Render(scene.SceneData, scene.Camera));
			CHECK(renderer.GetStats().Quality == high);
			CHECK(renderer.GetStats().Quality.Preset == RendererQualityPreset::High);
			CHECK(renderer.GetStats().ShadowMapSize == 2048);
		}
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
