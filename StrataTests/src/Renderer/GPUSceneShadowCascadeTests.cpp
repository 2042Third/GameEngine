#include "Renderer/FlickerMetrics.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Scene/InterpolationTestUtils.h"
#include "Strata/Renderer/RendererComponents.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	bool DeviceDrawsGPUDriven()
	{
		return Renderer::GetGraphicsDevice().GetInfo().SupportsDrawIndirectFirstInstance;
	}

	SceneRendererSpecification CascadeSpecification(GPUDrivenMode mode = GPUDrivenMode::Off)
	{
		SceneRendererSpecification specification = ExactSpecification();
		specification.GPUDriven = mode;
		specification.Quality.ShadowCascades = 4;
		specification.Quality.ShadowResolution = ShadowMapResolution::Size2048;
		return specification;
	}

	// Frames 1/60 s apart, whatever the machine's clock says.
	SceneRenderOptions StepOptions(float seconds = 1.0f / 60.0f)
	{
		SceneRenderOptions options;
		options.AdaptationSeconds = seconds;
		return options;
	}

	// A floor around the camera with boxes and posts of several heights on every side (so a turn always faces shadows near and
	// far) and a tall mast whose long shadow ends in a soft tip, under a sun 35 degrees above the horizon.
	struct YardScene
	{
		Scene SceneData;
		Entity Sun;

		explicit YardScene(SceneTestAssets& assets)
		{
			AddNeutralPostProcess(SceneData);
			MaterialProperties matte;
			matte.Roughness = 1.0f;
			const AssetHandle material = assets.AddMaterial(matte);
			AddMesh(SceneData, BuiltinAssets::PlaneMesh, material, glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(240.0f);
			for (int z = -6; z <= 6; z++)
			{
				for (int x = -6; x <= 6; x++)
				{
					if (std::abs(x) < 1 && std::abs(z) < 1)
						continue;
					const float height = 0.5f + static_cast<float>((x * 7 + z * 3 + 50) % 5) * 0.6f;
					Entity box = AddMesh(SceneData, BuiltinAssets::CubeMesh, material, glm::vec3(static_cast<float>(x) * 5.0f, 0.5f * height, static_cast<float>(z) * 5.0f), "Box");
					box.GetComponent<TransformComponent>().Scale = (x + z) % 2 == 0 ? glm::vec3(0.8f, height, 0.8f) : glm::vec3(0.15f, height, 0.15f);
				}
			}
			Entity mast = AddMesh(SceneData, BuiltinAssets::CubeMesh, material, glm::vec3(-3.0f, 6.0f, -9.0f), "Mast");
			mast.GetComponent<TransformComponent>().Scale = glm::vec3(0.3f, 12.0f, 0.3f);
			Sun = AddSun(SceneData, SunDirection(35.0f, 30.0f), 3.0f);
		}
	};

	// A person's view in the yard: 1.7 m up, 15 degrees down, toward -Z turned by `yaw` degrees about the vertical.
	SceneCamera YardCamera(uint32_t width, uint32_t height, float yaw, const glm::vec3& position = glm::vec3(0.0f, 1.7f, 0.0f))
	{
		const float pitch = glm::radians(15.0f);
		const float turn = glm::radians(yaw);
		const glm::vec3 forward(-std::sin(turn) * std::cos(pitch), -std::sin(pitch), -std::cos(turn) * std::cos(pitch));
		return LookAt(position, position + forward, static_cast<float>(width) / static_cast<float>(height));
	}

	ReadbackImage RenderHDR(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera)
	{
		REQUIRE(renderer.Render(scene, camera, nullptr, StepOptions()));
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), image)); // Waits for the GPU: the frame's readbacks are complete
		REQUIRE(image.Format == nvrhi::Format::RGBA16_FLOAT);
		return image;
	}

	// Pixels whose HDR color differs between two frames of one size.
	uint32_t CountDifferentPixels(const ReadbackImage& a, const ReadbackImage& b)
	{
		REQUIRE(a.Width == b.Width);
		REQUIRE(a.Height == b.Height);
		REQUIRE(a.BytesPerPixel == b.BytesPerPixel);
		uint32_t count = 0;
		for (size_t pixel = 0; pixel < static_cast<size_t>(a.Width) * a.Height; pixel++)
		{
			if (std::memcmp(a.Pixels.data() + pixel * a.BytesPerPixel, b.Pixels.data() + pixel * b.BytesPerPixel, a.BytesPerPixel) != 0)
				count++;
		}
		return count;
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
	// the receivers lie 20-62 m away.
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

	float RedAt(const ReadbackImage& image, size_t pixel)
	{
		uint16_t value = 0;
		std::memcpy(&value, image.Pixels.data() + pixel * image.BytesPerPixel, sizeof(value));
		return glm::unpackHalf1x16(value);
	}

	// The most sun reaching a floor pixel nearer than `maxDepth` in `shadowed` (an HDR frame), as a share of the sun's light
	// there: against frames of the same view without the sun's shadows and without the sun, drawn by a renderer of their own.
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

	// Whether two output frames are equal in the columns from `firstColumn` on.
	bool SameColumns(const ReadbackImage& a, const ReadbackImage& b, uint32_t firstColumn)
	{
		REQUIRE(a.Width == b.Width);
		REQUIRE(a.Height == b.Height);
		const size_t rowBytes = static_cast<size_t>(a.Width) * a.BytesPerPixel;
		const size_t offset = static_cast<size_t>(firstColumn) * a.BytesPerPixel;
		for (uint32_t row = 0; row < a.Height; row++)
		{
			if (std::memcmp(a.Pixels.data() + row * rowBytes + offset, b.Pixels.data() + row * rowBytes + offset, rowBytes - offset) != 0)
				return false;
		}
		return true;
	}

	void Report(const std::string& name, const FlickerResult& result)
	{
		MESSAGE(name << ": mean " << result.Mean * 100.0 << "%, p99 " << result.P99 * 100.0 << "%, blocks " << result.Blocks * 100.0 << "%, blocks p99 "
					 << result.BlocksP99 * 100.0 << "%, measured " << result.MeasuredShare * 100.0 << "% over " << result.Frames << " triples");
	}

}

TEST_SUITE("GPU.SceneRenderer.ShadowCascades")
{
	TEST_CASE("A turned camera's first frame shows its final shadows, those of a fresh renderer at that view")
	{
		// The owner's report (2026-11-04): after a turn the sun's shadows were coarse and sharpened over a second as the cascades
		// were fitted again to the receivers read back from the GPU. Stable cascades depend on the camera's view alone: a renderer
		// that looked elsewhere for a while draws a turned view's first frame exactly as a renderer that never drew anything else,
		// and every frame after it alike.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		YardScene yard(assets);
		Scene& scene = yard.SceneData;
		constexpr uint32_t width = 384;
		constexpr uint32_t height = 216;
		std::vector<GPUDrivenMode> modes = { GPUDrivenMode::Off };
		if (DeviceDrawsGPUDriven())
			modes.push_back(GPUDrivenMode::Auto);
		for (const GPUDrivenMode mode : modes)
		{
			CAPTURE(static_cast<int>(mode));
			SceneRenderer renderer(CascadeSpecification(mode));
			renderer.SetViewportSize(width, height);
			const SceneCamera camera = YardCamera(width, height, 0.0f);
			for (int frame = 0; frame < 10; frame++)
				RenderHDR(renderer, scene, camera);

			// A quarter turn, in one frame and in six frames of 15 degrees.
			for (const int steps : { 1, 6 })
			{
				CAPTURE(steps);
				const float startYaw = steps == 1 ? 0.0f : 90.0f;
				SceneCamera turned = camera;
				for (int step = 1; step <= steps; step++)
				{
					turned = YardCamera(width, height, startYaw + 90.0f * static_cast<float>(step) / static_cast<float>(steps));
					RenderHDR(renderer, scene, turned);
				}
				const ReadbackImage first = RenderHDR(renderer, scene, turned);
				SceneRenderer fresh(CascadeSpecification(mode));
				fresh.SetViewportSize(width, height);
				const ReadbackImage reference = RenderHDR(fresh, scene, turned);
				CHECK(renderer.GetStats().Cascades == fresh.GetStats().Cascades);
				CHECK(CountDifferentPixels(first, reference) == 0);
				// Nothing settles afterwards: the still view's later frames are the first one.
				for (int frame = 0; frame < 8; frame++)
				{
					CAPTURE(frame);
					CHECK(CountDifferentPixels(RenderHDR(renderer, scene, turned), first) == 0);
				}
				CHECK(renderer.GetStats().Cascades == fresh.GetStats().Cascades);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A slow strafe moves the sun's shadows without shimmer")
	{
		// The cascades move with the camera in whole texels of light space, so a shadow edge stays where it is on the ground
		// while the camera slides past it a centimeter a frame: the flicker meter measures the same light at each world point.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		YardScene yard(assets);
		Scene& scene = yard.SceneData;
		constexpr uint32_t width = 1280;
		constexpr uint32_t height = 720;
		constexpr uint32_t frames = 14;
		const auto measure = [&](bool shadows)
		{
			DirectionalLightComponent& light = yard.Sun.GetComponent<DirectionalLightComponent>();
			light.CastShadows = shadows;
			yard.Sun.MarkModified<DirectionalLightComponent>();
			SceneRenderer renderer(CascadeSpecification());
			renderer.SetViewportSize(width, height);
			SceneRenderOptions options = StepOptions();
			options.HDROutput = true;
			options.PreExposure = 1.0f;
			FlickerSequence sequence(3);
			for (uint32_t frame = 0; frame < frames; frame++)
			{
				// Sideways (+X for a camera toward -Z turned 20 degrees), 1.2 cm a frame.
				const float yaw = 20.0f;
				const glm::vec3 side(std::cos(glm::radians(yaw)), 0.0f, -std::sin(glm::radians(yaw)));
				const SceneCamera camera = YardCamera(width, height, yaw, glm::vec3(0.0f, 1.7f, 0.0f) + side * (0.012f * static_cast<float>(frame)));
				REQUIRE(renderer.Render(scene, camera, nullptr, options));
				sequence.Add(ReadFlickerFrame(renderer, camera));
			}
			return sequence.GetResult();
		};
		const FlickerResult shadowed = measure(true);
		const FlickerResult unshadowed = measure(false);
		Report("shadowed", shadowed);
		Report("unshadowed", unshadowed);
		CHECK(shadowed.MeasuredShare > 0.5);
		// The shadows add no flicker of their own beyond what the moving pixels' sampling of their edges shows: 0.014% of blocks
		// and 0.31% at the 99th percentile of blocks, where squares centered on the spheres without the snapping crawl at
		// 0.20% and 5.0%.
		CHECK(shadowed.Blocks <= unshadowed.Blocks + 0.0005);
		CHECK(shadowed.BlocksP99 < 0.01);
		CHECK(shadowed.P99 <= unshadowed.P99 + 0.002);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Every receiver is shadowed in the first frame after a turn or a change of the scene, as a fresh renderer shows it")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		RoofScene roofScene(assets);
		Scene& scene = roofScene.SceneData;
		// A wall 3 m ahead hides the floor beyond it until it goes.
		Entity wall = AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f, 2.0f, -3.0f), "Wall");
		wall.GetComponent<TransformComponent>().Scale = glm::vec3(60.0f, 30.0f, 0.2f);

		constexpr uint32_t size = 256;
		SceneRenderer renderer(CascadeSpecification());
		renderer.SetViewportSize(size, size);
		SceneRendererSpecification referenceSpecification = CascadeSpecification();
		SceneRenderer reference(referenceSpecification);
		reference.SetViewportSize(size, size);
		const SceneCamera camera = PitchedCamera(glm::vec3(0.0f, 2.0f, 0.0f), 10.0f, 60.0f);
		for (int frame = 0; frame < 4; frame++)
			RenderHDR(renderer, scene, camera);
		const auto cascades = renderer.GetStats().Cascades;
		REQUIRE(renderer.GetStats().CascadeCount == 4);
		CHECK(cascades[3].Far == 50.0f);
		const auto checkFresh = [&](const ReadbackImage& image, const SceneCamera& view)
		{
			SceneRenderer fresh(CascadeSpecification());
			fresh.SetViewportSize(size, size);
			CHECK(CountDifferentPixels(image, RenderHDR(fresh, scene, view)) == 0);
		};

		// The wall goes: the floor beyond it is shadowed at once, by the cascades it had (they never depended on it).
		wall.RemoveComponent<MeshRendererComponent>();
		uint32_t pixels = 0;
		const ReadbackImage revealed = RenderHDR(renderer, scene, camera);
		CHECK(renderer.GetStats().Cascades == cascades);
		CHECK(MostFloorSunlight(revealed, reference, scene, camera, roofScene.Sun, 45.0f, pixels) < 0.05f);
		CHECK(pixels > 1000);
		checkFresh(revealed, camera);

		// Turned 30 and 120 degrees: the same slices and texels, every receiver in shadow.
		for (const float degrees : { 30.0f, 120.0f })
		{
			CAPTURE(degrees);
			const SceneCamera turned = Turned(camera, degrees);
			const ReadbackImage first = RenderHDR(renderer, scene, turned);
			for (uint32_t cascade = 0; cascade < 4; cascade++)
			{
				CHECK(renderer.GetStats().Cascades[cascade].Near == cascades[cascade].Near);
				CHECK(renderer.GetStats().Cascades[cascade].Far == cascades[cascade].Far);
				CHECK(renderer.GetStats().Cascades[cascade].WorldPerTexel == cascades[cascade].WorldPerTexel);
			}
			CHECK(MostFloorSunlight(first, reference, scene, turned, roofScene.Sun, 45.0f, pixels) < 0.05f);
			CHECK(pixels > 1000);
			checkFresh(first, turned);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A still camera keeps its cascades and their cached layers")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!DeviceDrawsGPUDriven())
			return;
		SceneTestAssets assets;
		PlaygroundScene playground(assets);
		Scene& scene = playground.SceneData;
		constexpr uint32_t size = 128;
		SceneRenderer renderer(CascadeSpecification(GPUDrivenMode::Auto));
		renderer.SetViewportSize(size, size);
		REQUIRE(renderer.IsGPUDriven());
		const SceneCamera camera = PlaygroundCamera(size, size);
		for (int frame = 0; frame < 2; frame++)
			RenderToImage(renderer, scene, camera, StepOptions());
		const auto cascades = renderer.GetStats().Cascades;
		for (int frame = 0; frame < 10; frame++)
		{
			RenderToImage(renderer, scene, camera, StepOptions());
			CHECK(renderer.GetStats().CachedCascadeMask == 0xFu);
			CHECK(renderer.GetStats().Cascades == cascades);
		}
		// A box moving 10 cm changes no cascade (only what its shadow reaches is drawn again).
		playground.Mover.GetComponent<TransformComponent>().Translation.x += 0.1f;
		playground.Mover.MarkModified<TransformComponent>();
		RenderToImage(renderer, scene, camera, StepOptions());
		CHECK(renderer.GetStats().CachedCascadeMask != 0xFu);
		for (int frame = 0; frame < 3; frame++)
		{
			RenderToImage(renderer, scene, camera, StepOptions());
			CHECK(renderer.GetStats().CachedCascadeMask == 0xFu);
		}
		CHECK(renderer.GetStats().Cascades == cascades);

		// With temporal anti-aliasing each frame is drawn through another sub-pixel jitter of the projection, which moves no
		// cascade: they stay cached.
		PostProcessComponent& postProcess = GetPostProcess(scene);
		postProcess.AntiAliasing = true;
		postProcess.AntiAliasingMethod = AntiAliasingMode::TAA;
		RenderToImage(renderer, scene, camera, StepOptions());
		for (uint32_t frame = 0; frame < 2 * SceneRenderer::c_TemporalJitterPhases; frame++)
		{
			RenderToImage(renderer, scene, camera, StepOptions());
			CHECK(renderer.GetStats().TemporalAntiAliasing);
			CHECK(renderer.GetStats().CachedCascadeMask == 0xFu);
			CHECK(renderer.GetStats().Cascades == cascades);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Both paths lay out alike and stay pixel-equal, also with a moving camera, which keeps the visibility feedback")
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
		SceneRenderer cpu(CascadeSpecification(GPUDrivenMode::Off));
		SceneRenderer gpuDriven(CascadeSpecification(GPUDrivenMode::Auto));
		cpu.SetViewportSize(width, height);
		gpuDriven.SetViewportSize(width, height);
		REQUIRE(gpuDriven.IsGPUDriven());
		SceneCamera camera = PlaygroundCamera(width, height);

		const auto compare = [&]()
		{
			const ReadbackImage cpuImage = RenderToImage(cpu, scene, camera, StepOptions());
			const ReadbackImage gpuImage = RenderToImage(gpuDriven, scene, camera, StepOptions());
			gpuDriven.ResolveGPUStats(true);
			CHECK(cpu.GetStats().Cascades == gpuDriven.GetStats().Cascades);
			CHECK(cpuImage.Pixels == gpuImage.Pixels);
		};
		for (int frame = 0; frame < 6; frame++)
			compare();

		// Panning 0.5 m a frame: the squares move texel by texel, and the GPU path's requests still come from its visibility
		// feedback (every cascade gets the slots of every caster the feedback saw).
		for (int frame = 0; frame < 10; frame++)
		{
			camera.Position.x += 0.5f;
			camera.View = glm::lookAt(camera.Position, glm::vec3(camera.Position.x, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			compare();
			CHECK(gpuDriven.GetStats().FeedbackRequests);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A ball shot from the camera leaves the boxes' shadows where they are")
	{
		// The Physics Playground's shot: a 0.25 m ball leaves the camera at 1.5 m and 24 m/s (0.4 m a 1/60 s frame). The cascades
		// depend on the view alone, so nothing it does re-cuts them: the shadows of the boxes it never covers (the right third
		// of the view; its own shadow falls to the left) stay the same to the bit in every frame of its flight.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		PlaygroundScene playground(assets);
		Scene& scene = playground.SceneData;
		MaterialProperties matte;
		const AssetHandle material = assets.AddMaterial(matte);
		constexpr uint32_t size = 256;
		SceneRenderer renderer(CascadeSpecification());
		renderer.SetViewportSize(size, size);
		const SceneCamera camera = PlaygroundCamera(size, size);
		// Out of view until it is shot.
		Entity ball = AddMesh(scene, BuiltinAssets::SphereMesh, material, glm::vec3(0.0f, 60.0f, 80.0f), "Ball");
		ball.GetComponent<TransformComponent>().Scale = glm::vec3(0.5f);
		ball.MarkModified<TransformComponent>();
		RenderToImage(renderer, scene, camera, StepOptions());
		const ReadbackImage before = RenderToImage(renderer, scene, camera, StepOptions());
		const auto cascades = renderer.GetStats().Cascades;
		const glm::vec3 forward = glm::normalize(-camera.Position);
		uint32_t changedFrames = 0;
		for (int step = 0; step < 40; step++)
		{
			CAPTURE(step);
			ball.GetComponent<TransformComponent>().Translation = camera.Position + forward * (1.5f + 0.4f * static_cast<float>(step));
			ball.MarkModified<TransformComponent>();
			const ReadbackImage frame = RenderToImage(renderer, scene, camera, StepOptions());
			CHECK(renderer.GetStats().Cascades == cascades);
			CHECK(SameColumns(frame, before, 2 * size / 3));
			changedFrames += frame.Pixels != before.Pixels ? 1u : 0u;
		}
		CHECK(changedFrames == 40); // The ball and its shadow show in every frame
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A ball shot from the camera leaves the cascades alone, also drawn between fixed steps")
	{
		// The same shot with the ball on a fixed-step mover in a running scene that interpolates (Scene::SetInterpolation): the
		// ball is drawn moving on every frame, at 240 and 1,000 frames a second.
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
			SceneRenderer renderer(CascadeSpecification());
			renderer.SetViewportSize(size, size);
			const SceneCamera camera = PlaygroundCamera(size, size);
			RenderToImage(renderer, scene, camera, StepOptions(frameLength));
			const ReadbackImage before = RenderToImage(renderer, scene, camera, StepOptions(frameLength));
			const auto cascades = renderer.GetStats().Cascades;

			const glm::vec3 forward = glm::normalize(-camera.Position);
			ball.GetComponent<TransformComponent>().Translation = camera.Position + forward * 1.5f;
			ball.MarkModified<TransformComponent>();
			AddFixedMover(scene, ball, forward * speed);
			// Until it flew 16 m, as the case above: 160 frames at 240 per second, 667 at 1,000.
			const int frames = static_cast<int>(std::ceil(16.0f / speed / frameLength));
			uint32_t between = 0;
			uint32_t moved = 0;
			for (int frame = 0; frame < frames; frame++)
			{
				CAPTURE(frame);
				const uint64_t steps = scene.GetFixedStepCount();
				scene.OnUpdateRuntime(Timestep(frameLength));
				const ReadbackImage image = RenderToImage(renderer, scene, camera, StepOptions(frameLength));
				const glm::mat4* drawn = scene.FindDrawnTransform(ball.GetHandle());
				if (scene.GetFixedStepCount() == steps && drawn && *drawn != scene.GetWorldTransform(ball))
					between++;
				CHECK(renderer.GetStats().Cascades == cascades);
				moved += SameColumns(image, before, 2 * size / 3) ? 0u : 1u;
			}
			// Drawn between its steps on most frames: what this case adds to the one above.
			CHECK(between > static_cast<uint32_t>(frames) / 2);
			CHECK(moved == 0);
			scene.OnRuntimeStop();
			gpu.GetDevice().WaitForIdle();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
