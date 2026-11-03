#include "Renderer/SceneRendererTestUtils.h"
#include "Scene/InterpolationTestUtils.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/ShadowFit.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	bool DeviceDrawsGPUDriven()
	{
		return Renderer::GetGraphicsDevice().GetInfo().SupportsDrawIndirectFirstInstance;
	}

	SceneRendererSpecification FittedSpecification(GPUDrivenMode mode = GPUDrivenMode::Off)
	{
		SceneRendererSpecification specification;
		specification.GPUDriven = mode;
		specification.TextureStreaming = false;
		specification.Quality.ShadowCascades = 4;
		specification.Quality.ShadowResolution = ShadowMapResolution::Size2048;
		return specification;
	}

	// A floor under a roof that shadows all of it (a sun from +X 45 degrees above the horizon, shadows to 50 m).
	struct RoofScene
	{
		Scene SceneData;
		Entity Sun;

		explicit RoofScene(SceneTestAssets& assets)
		{
			AddNeutralPostProcess(SceneData);
			MaterialProperties matte;
			matte.Roughness = 1.0f;
			const AssetHandle material = assets.AddMaterial(matte);
			AddMesh(SceneData, BuiltinAssets::PlaneMesh, material, glm::vec3(0.0f, 0.0f, -100.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(400.0f);
			Entity roof = AddMesh(SceneData, BuiltinAssets::CubeMesh, material, glm::vec3(0.0f, 30.5f, -100.0f), "Roof");
			roof.GetComponent<TransformComponent>().Scale = glm::vec3(400.0f, 1.0f, 400.0f);
			Sun = AddSun(SceneData, SunDirection(45.0f, 0.0f), 3.0f);
			Sun.GetComponent<DirectionalLightComponent>().ShadowDistance = 50.0f;
		}
	};

	// A 60 m floor with rows of boxes, seen from (0, 22, 30) toward the origin as the Physics Playground frames its dominoes:
	// the receivers lie 20-62 m away, where Strata 0.6.0's layout from the near clip puts them all in its last cascades.
	struct PlaygroundScene
	{
		Scene SceneData;
		Entity Sun;
		Entity Mover;

		explicit PlaygroundScene(SceneTestAssets& assets)
		{
			AddNeutralPostProcess(SceneData);
			MaterialProperties matte;
			matte.Roughness = 1.0f;
			const AssetHandle material = assets.AddMaterial(matte);
			AddMesh(SceneData, BuiltinAssets::PlaneMesh, material, glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(60.0f);
			for (int z = -3; z <= 3; z++)
			{
				for (int x = -3; x <= 3; x++)
				{
					Entity box = AddMesh(SceneData, BuiltinAssets::CubeMesh, material, glm::vec3(static_cast<float>(x) * 6.0f, 1.0f, static_cast<float>(z) * 6.0f), "Box");
					box.GetComponent<TransformComponent>().Scale = glm::vec3(0.4f, 2.0f, 1.2f);
					if (x == 0 && z == 0)
						Mover = box;
				}
			}
			Sun = AddSun(SceneData, SunDirection(50.0f, 30.0f), 3.0f);
		}
	};

	SceneCamera PlaygroundCamera(uint32_t width, uint32_t height)
	{
		return LookAt(glm::vec3(0.0f, 22.0f, 30.0f), glm::vec3(0.0f), static_cast<float>(width) / static_cast<float>(height));
	}

	// Frames 1/60 s apart, whatever the machine's clock says: the fit's holds count these seconds (ShadowFitView::Seconds), so
	// no comparison depends on how long frames take.
	SceneRenderOptions StepOptions(float seconds = 1.0f / 60.0f)
	{
		SceneRenderOptions options;
		options.AdaptationSeconds = seconds;
		return options;
	}

	ReadbackImage RenderHDR(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera)
	{
		REQUIRE(renderer.Render(scene, camera, nullptr, StepOptions()));
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), image)); // Waits for the GPU: the frame's readbacks are complete
		REQUIRE(image.Format == nvrhi::Format::RGBA16_FLOAT);
		return image;
	}

	// Renders frames, each waited for, until the cascades' fit settled; the frames it took (frames + 1: never).
	uint32_t RenderUntilFitSettled(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera, uint32_t frames)
	{
		for (uint32_t frame = 1; frame <= frames; frame++)
		{
			RenderToImage(renderer, scene, camera, StepOptions());
			if (renderer.GetStats().ShadowFitSettled)
				return frame;
		}
		return frames + 1;
	}

	float RedAt(const ReadbackImage& image, size_t pixel)
	{
		uint16_t value = 0;
		std::memcpy(&value, image.Pixels.data() + pixel * image.BytesPerPixel, sizeof(value));
		return glm::unpackHalf1x16(value);
	}

	// The most sun reaching a floor pixel nearer than `maxDepth` in `shadowed` (an HDR frame of the fitted renderer), as a share
	// of the sun's light there: against frames of the same view without the sun's shadows and without the sun, drawn by a
	// renderer of their own, so the fitted renderer's frames and fit go on undisturbed.
	float MostFloorSunlight(const ReadbackImage& shadowed, SceneRenderer& reference, Scene& scene, const SceneCamera& camera, Entity sun, float maxDepth, uint32_t& pixels)
	{
		DirectionalLightComponent& light = sun.GetComponent<DirectionalLightComponent>();
		light.CastShadows = false;
		sun.MarkModified<DirectionalLightComponent>();
		const ReadbackImage lit = RenderHDR(reference, scene, camera);
		const float intensity = light.Intensity;
		light.Intensity = 0.0f;
		sun.MarkModified<DirectionalLightComponent>();
		const ReadbackImage dark = RenderHDR(reference, scene, camera);
		light.CastShadows = true;
		light.Intensity = intensity;
		sun.MarkModified<DirectionalLightComponent>();

		const uint32_t size = shadowed.Width;
		float most = 0.0f;
		pixels = 0;
		for (uint32_t row = 0; row < shadowed.Height; row++)
		{
			const float depth = FloorRowDepth(camera, row, size);
			if (depth < 0.0f || depth > maxDepth)
				continue;
			for (uint32_t x = 0; x < size; x++)
			{
				const size_t pixel = static_cast<size_t>(row) * size + x;
				const float direct = RedAt(lit, pixel) - RedAt(dark, pixel);
				if (!(direct > 1e-3f))
					continue;
				most = std::max(most, (RedAt(shadowed, pixel) - RedAt(dark, pixel)) / direct);
				pixels++;
			}
		}
		return most;
	}

	// The camera turned about the vertical through its position.
	SceneCamera Turned(const SceneCamera& camera, float degrees)
	{
		SceneCamera turned = camera;
		const glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), glm::radians(degrees), glm::vec3(0.0f, 1.0f, 0.0f));
		turned.View = camera.View * glm::translate(glm::mat4(1.0f), camera.Position) * glm::inverse(rotation) * glm::translate(glm::mat4(1.0f), -camera.Position);
		return turned;
	}

	ShadowViewStats RenderShadowView(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera)
	{
		SceneRenderOptions options = StepOptions();
		options.ShowShadowView = true;
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image)); // Waits for the GPU
		renderer.ResolveShadowViewStats();
		REQUIRE(renderer.GetShadowViewStats().Render == renderer.GetRenderCount());
		return renderer.GetShadowViewStats();
	}

}

TEST_SUITE("GPU.SceneRenderer.ShadowFit")
{
	TEST_CASE("The fitted cascades shadow every receiver, also in the first frame after the camera turned")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		RoofScene roofScene(assets);
		Scene& scene = roofScene.SceneData;

		constexpr uint32_t size = 256;
		SceneRenderer renderer(FittedSpecification());
		renderer.SetViewportSize(size, size);
		SceneRendererSpecification referenceSpecification = FittedSpecification();
		referenceSpecification.FitShadowsToView = false;
		SceneRenderer reference(referenceSpecification);
		reference.SetViewportSize(size, size);
		const SceneCamera camera = PitchedCamera(glm::vec3(0.0f, 2.0f, 0.0f), 10.0f, 60.0f);

		CHECK(RenderUntilFitSettled(renderer, scene, camera, 8) <= 8);
		const SceneRendererStats& stats = renderer.GetStats();
		CHECK(stats.ShadowFit);
		CHECK(stats.ShadowFitNear > camera.Near);
		CHECK(stats.ShadowFitFar == doctest::Approx(50.0f)); // The floor reaches the shadow distance
		uint32_t pixels = 0;
		const ReadbackImage settled = RenderHDR(renderer, scene, camera);
		CHECK(renderer.GetStats().ShadowFit);
		CHECK(MostFloorSunlight(settled, reference, scene, camera, roofScene.Sun, 45.0f, pixels) < 0.05f);
		CHECK(pixels > 1000);

		// Turned 30 degrees (not a cut): the newest readback is of the old view, and what it did not see falls to a coarser
		// cascade by containment, never outside every cascade.
		const SceneCamera turned = Turned(camera, 30.0f);
		const ReadbackImage first = RenderHDR(renderer, scene, turned);
		CHECK(renderer.GetStats().ShadowFit);
		CHECK(MostFloorSunlight(first, reference, scene, turned, roofScene.Sun, 45.0f, pixels) < 0.05f);
		CHECK(pixels > 1000);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A floor revealed beyond a fitted far bound is shadowed in its first frame")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		RoofScene roofScene(assets);
		Scene& scene = roofScene.SceneData;
		// A wall 3 m ahead hides the floor beyond it: the receivers end there, so the first three cascades end near 4 m.
		Entity wall = AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f, 2.0f, -3.0f), "Wall");
		wall.GetComponent<TransformComponent>().Scale = glm::vec3(60.0f, 30.0f, 0.2f);

		constexpr uint32_t size = 256;
		SceneRenderer renderer(FittedSpecification());
		renderer.SetViewportSize(size, size);
		SceneRendererSpecification referenceSpecification = FittedSpecification();
		referenceSpecification.FitShadowsToView = false;
		SceneRenderer reference(referenceSpecification);
		reference.SetViewportSize(size, size);
		const SceneCamera camera = PitchedCamera(glm::vec3(0.0f, 2.0f, 0.0f), 10.0f, 60.0f);
		CHECK(RenderUntilFitSettled(renderer, scene, camera, 8) <= 8);
		const SceneRendererStats& stats = renderer.GetStats();
		REQUIRE(stats.ShadowFit);
		CHECK(stats.ShadowFitFar < 8.0f);
		REQUIRE(stats.CascadeCount == 4);
		CHECK(stats.Cascades[2].Far == doctest::Approx(stats.ShadowFitFar));
		CHECK(stats.Cascades[3].Far == doctest::Approx(50.0f)); // The last cascade still reaches the shadow distance

		// The wall goes: before any readback saw the floor, the floor beyond the fitted far bound falls to the last cascade.
		wall.RemoveComponent<MeshRendererComponent>();
		uint32_t pixels = 0;
		const ReadbackImage revealed = RenderHDR(renderer, scene, camera);
		CHECK(renderer.GetStats().ShadowFitFar < 8.0f); // Still the wall's layout
		CHECK(MostFloorSunlight(revealed, reference, scene, camera, roofScene.Sun, 45.0f, pixels) < 0.05f);
		CHECK(pixels > 1000);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A floor that appears beside what the readback saw is shadowed in its first frame")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		RoofScene roofScene(assets);
		Scene& scene = roofScene.SceneData;
		// Looking down at a box on a floor that is not drawn yet: the box is the only receiver, so the first three cascades'
		// squares shrink to it and their slices to its distances.
		Entity floor = scene.FindEntityByName("Floor");
		REQUIRE(floor);
		const MeshRendererComponent floorRenderer = floor.GetComponent<MeshRendererComponent>();
		floor.RemoveComponent<MeshRendererComponent>();
		AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f, 0.25f, -3.5f), "Box").GetComponent<TransformComponent>().Scale = glm::vec3(0.5f);

		constexpr uint32_t size = 256;
		SceneRenderer renderer(FittedSpecification());
		renderer.SetViewportSize(size, size);
		SceneRendererSpecification referenceSpecification = FittedSpecification();
		referenceSpecification.FitShadowsToView = false;
		SceneRenderer reference(referenceSpecification);
		reference.SetViewportSize(size, size);
		const SceneCamera camera = PitchedCamera(glm::vec3(0.0f, 2.0f, 0.0f), 30.0f, 60.0f);
		CHECK(RenderUntilFitSettled(renderer, scene, camera, 8) <= 8);
		const SceneRendererStats& stats = renderer.GetStats();
		REQUIRE(stats.ShadowFit);
		CHECK(stats.ShadowFitSquares);
		REQUIRE(stats.CascadeCount == 4);
		const float boxTexel = stats.Cascades[1].WorldPerTexel;
		CHECK(stats.Cascades[1].Fitted);
		CHECK(boxTexel * static_cast<float>(stats.ShadowMapSize) < 4.0f); // Far smaller than the floor the slice reaches

		// The floor is drawn: in its first frame the cascades are still the box's, and the floor around the box, at the
		// distances of their slices but outside their squares, takes the last cascade's sphere (containment), shadowed.
		floor.AddComponent<MeshRendererComponent>(floorRenderer);
		uint32_t pixels = 0;
		const ReadbackImage appeared = RenderHDR(renderer, scene, camera);
		CHECK(renderer.GetStats().Cascades[1].WorldPerTexel == boxTexel); // Still the box's
		CHECK(MostFloorSunlight(appeared, reference, scene, camera, roofScene.Sun, 45.0f, pixels) < 0.05f);
		CHECK(pixels > 1000);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A high camera gets finer texels where its receivers are")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		PlaygroundScene playground(assets);
		Scene& scene = playground.SceneData;
		constexpr uint32_t width = 384;
		constexpr uint32_t height = 216;
		const SceneCamera camera = PlaygroundCamera(width, height);
		SceneRenderer fitted(FittedSpecification());
		fitted.SetViewportSize(width, height);
		SceneRendererSpecification unfittedSpecification = FittedSpecification();
		unfittedSpecification.FitShadowsToView = false;
		SceneRenderer unfitted(unfittedSpecification);
		unfitted.SetViewportSize(width, height);

		CHECK(RenderUntilFitSettled(fitted, scene, camera, 8) <= 8);
		const ShadowViewStats fittedView = RenderShadowView(fitted, scene, camera);
		const ShadowViewStats unfittedView = RenderShadowView(unfitted, scene, camera);
		REQUIRE(fitted.GetStats().ShadowFit);
		CHECK_FALSE(unfitted.GetStats().ShadowFit);
		CHECK(fittedView.UncoveredPixels == 0);
		CHECK(unfittedView.UncoveredPixels == 0);
		CHECK(fittedView.Covered.MedianPixelsPerTexel > 0.0f);
		CHECK(fittedView.Covered.MedianPixelsPerTexel <= 0.6f * unfittedView.Covered.MedianPixelsPerTexel);
		// The first cascade starts near the receivers instead of the camera's near clip.
		CHECK(fitted.GetStats().Cascades[0].Near > 10.0f);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A sea-sized transparent surface around the camera does not pull the fitted range to the camera")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		PlaygroundScene playground(assets);
		Scene& scene = playground.SceneData;
		// Transparent surfaces are in no depth the fit measures: their nearest visible point bounds the near end, and a plane
		// whose box holds the camera's position must count from where the view can see it, not from its box.
		MaterialProperties water;
		water.BaseColor = glm::vec4(0.2f, 0.4f, 0.6f, 0.5f);
		water.AlphaMode = MaterialAlphaMode::Blend;
		AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(water), glm::vec3(0.0f, -10.0f, 0.0f), "Sea").GetComponent<TransformComponent>().Scale =
			glm::vec3(400.0f);
		constexpr uint32_t size = 128;
		SceneRenderer renderer(FittedSpecification());
		renderer.SetViewportSize(size, size);
		const SceneCamera camera = PlaygroundCamera(size, size);
		CHECK(RenderUntilFitSettled(renderer, scene, camera, 8) <= 8);
		REQUIRE(renderer.GetStats().ShadowFit);
		CHECK(renderer.GetStats().ShadowFitNear > 10.0f);
		CHECK(renderer.GetTransparentBatches().size() == 1); // The sea is drawn
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("First frames and camera cuts use Strata 0.6.0's layout")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		PlaygroundScene playground(assets);
		Scene& scene = playground.SceneData;
		constexpr uint32_t size = 128;
		SceneRenderer renderer(FittedSpecification());
		renderer.SetViewportSize(size, size);
		const SceneCamera camera = PlaygroundCamera(size, size);

		const auto checkUnfitted = [&](const SceneRendererStats& stats)
		{
			CHECK_FALSE(stats.ShadowFit);
			CHECK_FALSE(stats.ShadowFitSettled);
			REQUIRE(stats.CascadeCount == 4);
			for (uint32_t cascade = 0; cascade < 4; cascade++)
				CHECK(stats.Cascades[cascade].Far == doctest::Approx(ShadowFitTracker::GetSplit(camera.Near, 80.0f, 0.8f, cascade, 4)));
			CHECK(stats.Cascades[0].Near == doctest::Approx(camera.Near));
		};
		RenderToImage(renderer, scene, camera, StepOptions());
		checkUnfitted(renderer.GetStats());
		CHECK(RenderUntilFitSettled(renderer, scene, camera, 8) <= 8);
		CHECK(renderer.GetStats().ShadowFit);

		// A cut (the camera jumps): readbacks of the old view say nothing about the new one.
		SceneCamera cut = camera;
		cut.Position = glm::vec3(0.0f, 22.0f, 130.0f);
		cut.View = glm::lookAt(cut.Position, glm::vec3(0.0f, 0.0f, 100.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		RenderToImage(renderer, scene, cut, StepOptions());
		checkUnfitted(renderer.GetStats());

		// ResetShadowFit likewise (scene loads, a capture's first frame).
		CHECK(RenderUntilFitSettled(renderer, scene, cut, 8) <= 8);
		renderer.ResetShadowFit();
		RenderToImage(renderer, scene, cut, StepOptions());
		CHECK_FALSE(renderer.GetStats().ShadowFit);

		// With the fit off: 0.6.0's layout, always settled.
		SceneRendererSpecification off = FittedSpecification();
		off.FitShadowsToView = false;
		SceneRenderer plain(off);
		plain.SetViewportSize(size, size);
		for (int frame = 0; frame < 4; frame++)
		{
			RenderToImage(plain, scene, camera, StepOptions());
			CHECK_FALSE(plain.GetStats().ShadowFit);
			CHECK(plain.GetStats().ShadowFitSettled);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A still camera keeps its fitted cascades and their cached layers")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!DeviceDrawsGPUDriven())
			return;
		SceneTestAssets assets;
		PlaygroundScene playground(assets);
		Scene& scene = playground.SceneData;
		constexpr uint32_t size = 128;
		SceneRenderer renderer(FittedSpecification(GPUDrivenMode::Auto));
		renderer.SetViewportSize(size, size);
		REQUIRE(renderer.IsGPUDriven());
		const SceneCamera camera = PlaygroundCamera(size, size);
		CHECK(RenderUntilFitSettled(renderer, scene, camera, 8) <= 8);
		RenderToImage(renderer, scene, camera, StepOptions());
		REQUIRE(renderer.GetStats().ShadowFit);
		const uint64_t changes = renderer.GetStats().ShadowFitChanges;
		const auto cascades = renderer.GetStats().Cascades;
		for (int frame = 0; frame < 10; frame++)
		{
			RenderToImage(renderer, scene, camera, StepOptions());
			CHECK(renderer.GetStats().CachedCascadeMask == 0xFu);
			CHECK(renderer.GetStats().ShadowFitChanges == changes);
			CHECK(renderer.GetStats().ShadowFitSettled);
		}
		// A box moving 10 cm changes no cascade's square or slice (only what its shadow reaches is drawn again).
		playground.Mover.GetComponent<TransformComponent>().Translation.x += 0.1f;
		playground.Mover.MarkModified<TransformComponent>();
		for (int frame = 0; frame < 4; frame++)
			RenderToImage(renderer, scene, camera, StepOptions());
		CHECK(renderer.GetStats().ShadowFitChanges == changes);
		CHECK(renderer.GetStats().Cascades == cascades);

		// With temporal anti-aliasing each frame's receivers are seen through another sub-pixel jitter: on a still camera a wider
		// square some phases ask for waits (c_GrowSeconds) and is dropped at the next readback that does not ask, so the squares
		// stay where they hold the receivers, and the cascades stay cached.
		PostProcessComponent& postProcess = GetPostProcess(scene);
		postProcess.AntiAliasing = true;
		postProcess.AntiAliasingMethod = AntiAliasingMode::TAA;
		for (uint32_t frame = 0; frame < 2 * SceneRenderer::c_TemporalJitterPhases; frame++)
			RenderToImage(renderer, scene, camera, StepOptions());
		REQUIRE(renderer.GetStats().TemporalAntiAliasing);
		const uint64_t temporalChanges = renderer.GetStats().ShadowFitChanges;
		uint32_t holdingFrames = 0;
		for (uint32_t frame = 0; frame < 2 * SceneRenderer::c_TemporalJitterPhases; frame++)
		{
			RenderToImage(renderer, scene, camera, StepOptions());
			CHECK(renderer.GetStats().ShadowFit);
			CHECK(renderer.GetStats().CachedCascadeMask == 0xFu);
			CHECK(renderer.GetStats().ShadowFitChanges == temporalChanges);
			holdingFrames += renderer.GetStats().ShadowFitHolding ? 1 : 0;
		}
		CHECK_MESSAGE(!renderer.GetStats().ShadowFitHolding, "frames holding a growth: ", holdingFrames, " of ", 2 * SceneRenderer::c_TemporalJitterPhases);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Both paths fit alike and stay pixel-equal, also with a moving camera, which keeps the visibility feedback")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!DeviceDrawsGPUDriven())
			return;
		SceneTestAssets assets;
		PlaygroundScene playground(assets);
		Scene& scene = playground.SceneData;
		constexpr uint32_t width = 192;
		constexpr uint32_t height = 108;
		SceneRenderer cpu(FittedSpecification(GPUDrivenMode::Off));
		SceneRenderer gpuDriven(FittedSpecification(GPUDrivenMode::Auto));
		cpu.SetViewportSize(width, height);
		gpuDriven.SetViewportSize(width, height);
		REQUIRE(gpuDriven.IsGPUDriven());
		SceneCamera camera = PlaygroundCamera(width, height);

		const auto compare = [&]()
		{
			const ReadbackImage cpuImage = RenderToImage(cpu, scene, camera, StepOptions());
			const ReadbackImage gpuImage = RenderToImage(gpuDriven, scene, camera, StepOptions());
			gpuDriven.ResolveGPUStats(true);
			CHECK(cpu.GetStats().ShadowFit == gpuDriven.GetStats().ShadowFit);
			CHECK(cpu.GetStats().Cascades == gpuDriven.GetStats().Cascades);
			CHECK(cpuImage.Pixels == gpuImage.Pixels);
		};
		for (int frame = 0; frame < 6; frame++)
			compare();
		CHECK(cpu.GetStats().ShadowFit);
		CHECK(cpu.GetStats().ShadowFitSquares);

		// Panning 0.5 m a frame: squares move texel by texel and sides may change, and the GPU path's requests still come from
		// its visibility feedback (every cascade gets the slots of every caster the feedback saw).
		for (int frame = 0; frame < 10; frame++)
		{
			camera.Position.x += 0.5f;
			camera.View = glm::lookAt(camera.Position, glm::vec3(camera.Position.x, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			compare();
			CHECK(gpuDriven.GetStats().FeedbackRequests);
			CHECK(gpuDriven.GetStats().ShadowFit);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A ball shot from the camera leaves the cascades alone")
	{
		// The Physics Playground's shot: a 0.25 m ball leaves the camera at 1.5 m and 24 m/s (0.4 m a 1/60 s frame). It holds
		// under a tenth of the receivers and is gone before the near bound's half second: no slice or square is re-cut.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		PlaygroundScene playground(assets);
		Scene& scene = playground.SceneData;
		MaterialProperties matte;
		const AssetHandle material = assets.AddMaterial(matte);
		constexpr uint32_t size = 256;
		SceneRenderer renderer(FittedSpecification());
		renderer.SetViewportSize(size, size);
		const SceneCamera camera = PlaygroundCamera(size, size);
		// Out of view until it is shot.
		Entity ball = AddMesh(scene, BuiltinAssets::SphereMesh, material, glm::vec3(0.0f, 60.0f, 80.0f), "Ball");
		ball.GetComponent<TransformComponent>().Scale = glm::vec3(0.5f);
		ball.MarkModified<TransformComponent>();
		CHECK(RenderUntilFitSettled(renderer, scene, camera, 8) <= 8);
		RenderToImage(renderer, scene, camera, StepOptions());
		REQUIRE(renderer.GetStats().ShadowFit);
		const uint64_t changes = renderer.GetStats().ShadowFitChanges;
		const auto cascades = renderer.GetStats().Cascades;
		const glm::vec3 forward = glm::normalize(-camera.Position);
		for (int step = 0; step < 40; step++)
		{
			CAPTURE(step);
			ball.GetComponent<TransformComponent>().Translation = camera.Position + forward * (1.5f + 0.4f * static_cast<float>(step));
			ball.MarkModified<TransformComponent>();
			RenderToImage(renderer, scene, camera, StepOptions());
			CHECK(renderer.GetStats().ShadowFitChanges == changes);
			CHECK(renderer.GetStats().Cascades == cascades);
		}
		CHECK_FALSE(renderer.GetStats().ShadowFitHolding);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A ball shot from the camera leaves the cascades alone, also drawn between fixed steps")
	{
		// The same shot with the ball on a fixed-step mover in a running scene that interpolates (Scene::SetInterpolation): the
		// ball is drawn moving on every frame, at 240 and 1,000 frames a second, and the fit's holds count those frames' seconds.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		MaterialProperties matte;
		const AssetHandle material = assets.AddMaterial(matte);
		constexpr uint32_t size = 256;
		constexpr float speed = 24.0f;
		for (const float frameLength : { 1.0f / 240.0f, 1.0f / 1000.0f })
		{
			CAPTURE(frameLength);
			ScopedFixedMover moverScope;
			PlaygroundScene playground(assets);
			Scene& scene = playground.SceneData;
			scene.GetSettings().FixedTimestep = 1.0f / 64.0f;
			// Out of view until it is shot.
			Entity ball = AddMesh(scene, BuiltinAssets::SphereMesh, material, glm::vec3(0.0f, 60.0f, 80.0f), "Ball");
			ball.GetComponent<TransformComponent>().Scale = glm::vec3(0.5f);
			ball.MarkModified<TransformComponent>();
			scene.SetInterpolation(true);
			scene.OnRuntimeStart();
			SceneRenderer renderer(FittedSpecification());
			renderer.SetViewportSize(size, size);
			const SceneCamera camera = PlaygroundCamera(size, size);
			CHECK(RenderUntilFitSettled(renderer, scene, camera, 8) <= 8);
			RenderToImage(renderer, scene, camera, StepOptions(frameLength));
			REQUIRE(renderer.GetStats().ShadowFit);
			const uint64_t changes = renderer.GetStats().ShadowFitChanges;
			const auto cascades = renderer.GetStats().Cascades;

			const glm::vec3 forward = glm::normalize(-camera.Position);
			ball.GetComponent<TransformComponent>().Translation = camera.Position + forward * 1.5f;
			ball.MarkModified<TransformComponent>();
			AddFixedMover(scene, ball, forward * speed);
			// Until it flew 16 m, as the case above: 160 frames at 240 per second, 667 at 1,000.
			const int frames = static_cast<int>(std::ceil(16.0f / speed / frameLength));
			uint32_t between = 0;
			for (int frame = 0; frame < frames; frame++)
			{
				CAPTURE(frame);
				const uint64_t steps = scene.GetFixedStepCount();
				scene.OnUpdateRuntime(Timestep(frameLength));
				RenderToImage(renderer, scene, camera, StepOptions(frameLength));
				const glm::mat4* drawn = scene.FindDrawnTransform(ball.GetHandle());
				if (scene.GetFixedStepCount() == steps && drawn && *drawn != scene.GetWorldTransform(ball))
					between++;
				CHECK(renderer.GetStats().ShadowFitChanges == changes);
				CHECK(renderer.GetStats().Cascades == cascades);
			}
			// Drawn between its steps on most frames: what this case adds to the one above.
			CHECK(between > static_cast<uint32_t>(frames) / 2);
			CHECK_FALSE(renderer.GetStats().ShadowFitHolding);
			scene.OnRuntimeStop();
			gpu.GetDevice().WaitForIdle();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The fit's holds count the frame's AdaptationSeconds")
	{
		// A box parked 2 m in front of the camera, on a few percent of the receivers: its growth of the near bound waits half a
		// second of the frames' SceneRenderOptions::AdaptationSeconds, never of the machine's clock.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		PlaygroundScene playground(assets);
		Scene& scene = playground.SceneData;
		constexpr uint32_t size = 256;
		const SceneCamera camera = PlaygroundCamera(size, size);
		const glm::vec3 forward = glm::normalize(-camera.Position);
		for (const float seconds : { 0.1f, 0.0f })
		{
			CAPTURE(seconds);
			SceneRenderer renderer(FittedSpecification());
			renderer.SetViewportSize(size, size);
			Entity box = AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f, 60.0f, 80.0f), "Box");
			box.GetComponent<TransformComponent>().Scale = glm::vec3(0.4f);
			box.MarkModified<TransformComponent>();
			CHECK(RenderUntilFitSettled(renderer, scene, camera, 8) <= 8);
			const float nearBound = renderer.GetStats().ShadowFitNear;
			REQUIRE(nearBound > 4.0f);
			box.GetComponent<TransformComponent>().Translation = camera.Position + forward * 2.0f;
			box.MarkModified<TransformComponent>();
			if (seconds > 0.0f)
			{
				// The readback that sees the box arrives a frame later: from then on the growth waits, and the fit is not settled.
				uint32_t frame = 0;
				while (frame < 3 && !renderer.GetStats().ShadowFitHolding)
				{
					RenderToImage(renderer, scene, camera, StepOptions(seconds));
					frame++;
				}
				CHECK(renderer.GetStats().ShadowFitHolding);
				CHECK_FALSE(renderer.GetStats().ShadowFitSettled);
				CHECK(renderer.GetStats().ShadowFitNear == nearBound);
				while (frame < 12 && renderer.GetStats().ShadowFitNear == nearBound)
				{
					RenderToImage(renderer, scene, camera, StepOptions(seconds));
					frame++;
				}
				CAPTURE(frame);
				CHECK(renderer.GetStats().ShadowFitNear < 1.75f);
				CHECK(frame >= 5); // Half a second of 0.1 s frames
			}
			else
			{
				for (int frame = 0; frame < 30; frame++)
					RenderToImage(renderer, scene, camera, StepOptions(seconds));
				CHECK(renderer.GetStats().ShadowFitHolding);
				CHECK(renderer.GetStats().ShadowFitNear == nearBound);
			}
			scene.DestroyEntity(box);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
