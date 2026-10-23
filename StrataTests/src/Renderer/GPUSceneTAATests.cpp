#include "Renderer/TAATestUtils.h"

#include "Strata/Renderer/RenderScene.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

// Temporal anti-aliasing (PostProcessComponent::AntiAliasingMethod TAA): a sub-pixel jitter in the frame's copy of the camera,
// blended with the renderer's own history of earlier frames along the motion vectors.

using namespace Strata;
using namespace Strata::Tests;
using namespace Strata::Tests::TAA;

TEST_SUITE("GPU.SceneRenderer.TAA")
{
	TEST_CASE("The jitter's eight phases are distinct sub-pixel offsets that cover the pixel evenly")
	{
		std::vector<glm::vec2> phases;
		for (uint32_t phase = 0; phase < SceneRenderer::c_TemporalJitterPhases; phase++)
		{
			const glm::vec2 jitter = SceneRenderer::GetTemporalJitter(phase);
			CHECK(glm::all(glm::greaterThan(jitter, glm::vec2(-0.5f))));
			CHECK(glm::all(glm::lessThan(jitter, glm::vec2(0.5f))));
			for (const glm::vec2& earlier : phases)
				CHECK(jitter != earlier);
			phases.push_back(jitter);
		}
		// The sequence repeats; each half of the pixel holds four phases in each direction.
		CHECK(SceneRenderer::GetTemporalJitter(SceneRenderer::c_TemporalJitterPhases) == phases[0]);
		CHECK(std::count_if(phases.begin(), phases.end(), [](const glm::vec2& jitter) { return jitter.x < 0.0f; }) == 4);
		CHECK(std::count_if(phases.begin(), phases.end(), [](const glm::vec2& jitter) { return jitter.y < 0.0f; }) == 4);
	}

	TEST_CASE("The jitter changes only the frame's constants: culling, levels of detail and the cached cascades follow the camera")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(PathName(mode));
			TemporalScene scene(TemporalAntiAliasingPreset::Original);
			// The same scene drawn with FXAA: a renderer that does not do temporal anti-aliasing.
			SceneRenderer temporal = CreateRenderer("Temporal", mode);
			SceneRenderer plain = CreateRenderer("Plain", mode, false);
			temporal.SetViewportSize(c_Size, c_Size);
			plain.SetViewportSize(c_Size, c_Size);
			const uint32_t allCascades = (1u << temporal.GetSpecification().ShadowCascades) - 1u;
			for (uint32_t frame = 0; frame < 12; frame++)
			{
				CAPTURE(frame);
				REQUIRE(temporal.Render(scene.Contents, scene.Camera));
				REQUIRE(plain.Render(scene.Contents, scene.Camera));
				const SceneRendererStats& stats = temporal.GetStats();
				CHECK(stats.TemporalAntiAliasing);
				CHECK(stats.TemporalFrames == frame + 1);
				CHECK(stats.HistoryResets == 1);
				CHECK(stats.Jitter == SceneRenderer::GetTemporalJitter(frame % SceneRenderer::c_TemporalJitterPhases));
				CHECK_FALSE(plain.GetStats().TemporalAntiAliasing);
				CHECK(plain.GetStats().TemporalFrames == 0);
				CHECK(plain.GetStats().Jitter == glm::vec2(0.0f));

				const DrawnCounts temporalCounts = GetDrawnCounts(temporal);
				const DrawnCounts plainCounts = GetDrawnCounts(plain);
				CHECK(temporalCounts.Instances == plainCounts.Instances);
				CHECK(temporalCounts.InstancesPerLOD == plainCounts.InstancesPerLOD);
				CHECK(temporalCounts.VisibleProxies == plainCounts.VisibleProxies);
				CHECK(temporalCounts.ShadowCasters == plainCounts.ShadowCasters);
				CHECK(temporalCounts.ShadowDraws == plainCounts.ShadowDraws);
				CHECK(temporal.GetStats().CulledInstances == plain.GetStats().CulledInstances);
				CHECK(temporal.GetStats().CachedCascadeMask == plain.GetStats().CachedCascadeMask);
				// A still view over a still scene keeps every cascade from its second frame on.
				if (temporal.GetStats().GPUDriven && temporal.GetSpecification().CacheShadowCascades && frame > 0)
					CHECK(temporal.GetStats().CachedCascadeMask == allCascades);
			}
			CHECK(temporal.GetStats().GPUDriven == (mode == GPUDrivenMode::Auto));
			CHECK(plain.GetStats().GPUDriven == (mode == GPUDrivenMode::Auto));
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Cuts, other scenes, resizes, a skipped device frame and ResetHistory start the history again; slow frames do not")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		TemporalScene scene(TemporalAntiAliasingPreset::Original);
		SceneRenderer renderer = CreateRenderer("Resets");
		renderer.SetViewportSize(c_Size, c_Size);
		const auto render = [&](Scene& contents, uint32_t frames)
		{
			for (uint32_t frame = 0; frame < frames; frame++)
			{
				REQUIRE(renderer.Render(contents, scene.Camera));
				FinishFrame(gpu);
			}
		};

		render(scene.Contents, 1);
		CheckReset(renderer, 1);
		render(scene.Contents, 3);
		CHECK(renderer.GetStats().TemporalFrames == 4);
		CHECK(renderer.GetStats().HistoryResets == 1);

		// A camera cut.
		const SceneCamera before = scene.Camera;
		scene.Camera = LookAt(glm::vec3(60.0f, 9.0f, 8.0f), glm::vec3(60.0f, 0.0f, -2.5f));
		render(scene.Contents, 1);
		CheckReset(renderer, 2);
		scene.Camera = before;
		render(scene.Contents, 1);
		CheckReset(renderer, 3);
		render(scene.Contents, 2);
		CHECK(renderer.GetStats().TemporalFrames == 3);

		// Another render scene, then back; a copy of the scene (entering Play) goes on from its source.
		TemporalScene other(TemporalAntiAliasingPreset::Original);
		render(other.Contents, 1);
		CheckReset(renderer, 4);
		render(scene.Contents, 1);
		CheckReset(renderer, 5);
		Ref<Scene> source = CreateRef<Scene>("Source");
		AddTemporalPostProcess(*source, TemporalAntiAliasingPreset::Original);
		AddMesh(*source, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f), "Box");
		render(*source, 2);
		CHECK(renderer.GetStats().HistoryResets == 6);
		Ref<Scene> copy = Scene::Copy(source);
		render(*copy, 1);
		CHECK(renderer.GetStats().TemporalFrames == 3);
		CHECK(renderer.GetStats().HistoryResets == 6);

		// A resize.
		render(scene.Contents, 2);
		CHECK(renderer.GetStats().HistoryResets == 7);
		renderer.SetViewportSize(c_Size * 2, c_Size);
		render(scene.Contents, 1);
		CheckReset(renderer, 8);

		// Device frames: a renderer may render several times in one, goes on in the next, and starts again after one it skipped.
		render(scene.Contents, 1);
		Renderer::BeginFrame();
		render(scene.Contents, 2);
		CHECK(renderer.GetStats().TemporalFrames == 4);
		Renderer::BeginFrame();
		Renderer::BeginFrame();
		render(scene.Contents, 1);
		CheckReset(renderer, 9);

		// Asked for (a level load).
		render(scene.Contents, 1);
		renderer.ResetHistory();
		render(scene.Contents, 1);
		CheckReset(renderer, 10);

		// A frame without temporal anti-aliasing leaves the history behind.
		render(scene.Contents, 1);
		PostProcessComponent& postProcess = GetPostProcess(scene.Contents);
		postProcess.AntiAliasingMethod = AntiAliasingMode::FXAA;
		render(scene.Contents, 1);
		CHECK_FALSE(renderer.GetStats().TemporalAntiAliasing);
		CHECK(renderer.GetStats().TemporalFrames == 0);
		CHECK(renderer.GetStats().Jitter == glm::vec2(0.0f));
		postProcess.AntiAliasingMethod = AntiAliasingMode::TAA;
		render(scene.Contents, 1);
		CheckReset(renderer, 11);

		// Frames far apart in time (slower than any frame rate) go on, each in the device frame after the previous one's.
		for (uint32_t frame = 0; frame < 3; frame++)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			Renderer::BeginFrame();
			render(scene.Contents, 1);
		}
		CHECK(renderer.GetStats().TemporalFrames == 4);
		CHECK(renderer.GetStats().HistoryResets == 11);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("At eight frames a still edge is the mean of its eight jittered frames, and then stays near it")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		EdgeScene scene(assets, TemporalAntiAliasingPreset::Original);
		SceneRenderer renderer = CreateRenderer("Edge");
		renderer.SetViewportSize(c_Size, c_Size);

		// The eight phases' own images (the HDR target holds the frame before the resolve).
		std::vector<std::vector<glm::vec3>> phases;
		for (uint32_t frame = 0; frame < SceneRenderer::c_TemporalJitterPhases; frame++)
		{
			REQUIRE(renderer.Render(scene.Contents, scene.Camera));
			phases.push_back(ReadHDR(renderer.GetHDRTexture()));
		}
		CHECK(renderer.GetStats().TemporalFrames == SceneRenderer::c_TemporalJitterPhases);
		const size_t pixels = phases[0].size();
		std::vector<glm::vec3> mean(pixels, glm::vec3(0.0f));
		std::vector<glm::vec3> spread(pixels, glm::vec3(0.0f));
		uint32_t edgePixels = 0;
		for (size_t pixel = 0; pixel < pixels; pixel++)
		{
			glm::vec3 low(1e30f);
			glm::vec3 high(-1e30f);
			for (const std::vector<glm::vec3>& phase : phases)
			{
				mean[pixel] += phase[pixel] / static_cast<float>(phases.size());
				low = glm::min(low, phase[pixel]);
				high = glm::max(high, phase[pixel]);
			}
			spread[pixel] = high - low;
			edgePixels += glm::any(glm::greaterThan(spread[pixel], glm::vec3(0.05f))) ? 1u : 0u;
			// Both colors have the same luminance.
			CHECK(glm::dot(phases[0][pixel], c_LuminanceWeights) == doctest::Approx(glm::dot(scene.Ground, c_LuminanceWeights)).epsilon(0.002));
		}
		// The jitter moves the edge across a pixel along its whole length.
		CHECK(edgePixels >= c_Size);

		// Within half a percent of the mean (the history is stored in half floats).
		const auto within = [](const glm::vec3& value, const glm::vec3& expected, const glm::vec3& tolerance)
		{
			return glm::all(glm::lessThanEqual(glm::abs(value - expected), tolerance));
		};
		const std::vector<glm::vec3> resolved = ReadHDR(renderer.GetResolvedHDRTexture());
		uint32_t converged = 0;
		for (size_t pixel = 0; pixel < pixels; pixel++)
			converged += within(resolved[pixel], mean[pixel], mean[pixel] * 0.005f + 1e-4f) ? 1u : 0u;
		CHECK(converged == pixels);

		// From then on the history is exponential (the current frame weighs 1/8): each pixel oscillates around the mean by at most
		// 13% of its eight samples' spread.
		for (uint32_t frame = 0; frame < 32; frame++)
		{
			CAPTURE(frame);
			REQUIRE(renderer.Render(scene.Contents, scene.Camera));
			const std::vector<glm::vec3> image = ReadHDR(renderer.GetResolvedHDRTexture());
			uint32_t steady = 0;
			for (size_t pixel = 0; pixel < pixels; pixel++)
				steady += within(image[pixel], mean[pixel], spread[pixel] * 0.15f + mean[pixel] * 0.005f + 1e-4f) ? 1u : 0u;
			CHECK(steady == pixels);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A moving object leaves no trail beyond two pixels once it stops")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		Scene scene;
		AddTemporalPostProcess(scene, TemporalAntiAliasingPreset::Original);
		const glm::vec3 background(0.05f, 0.05f, 0.05f);
		Entity ground = AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(UnlitColor(glm::vec4(background, 1.0f))), glm::vec3(0.0f), "Ground");
		ground.GetComponent<TransformComponent>().Scale = glm::vec3(40.0f);
		Entity box = AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(UnlitColor({ 1.0f, 0.9f, 0.2f, 1.0f })), glm::vec3(-2.0f, 0.5f, 0.0f), "Box");
		const SceneCamera camera = OrthographicLookAt(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f), 8.0f);
		SceneRenderer renderer = CreateRenderer("Trail");
		renderer.SetViewportSize(c_Size, c_Size);

		// Let the history settle, then move the box two pixels a frame (8 world units over 64 pixels).
		for (uint32_t frame = 0; frame < 8; frame++)
		{
			REQUIRE(renderer.Render(scene, camera));
			FinishFrame(gpu);
		}
		for (uint32_t frame = 0; frame < 12; frame++)
		{
			box.GetComponent<TransformComponent>().Translation.x += 0.25f;
			box.MarkModified<TransformComponent>();
			REQUIRE(renderer.Render(scene, camera));
			FinishFrame(gpu);
		}
		// Still: pixels more than two pixels from the box show the ground alone.
		for (uint32_t frame = 0; frame < 8; frame++)
		{
			CAPTURE(frame);
			REQUIRE(renderer.Render(scene, camera));
			CHECK(renderer.GetStats().TemporalFrames > 20);
			const std::vector<glm::vec3> image = ReadHDR(renderer.GetResolvedHDRTexture());
			const std::vector<uint32_t> ids = ReadIDs(renderer);
			uint32_t boxPixels = 0;
			uint32_t trail = 0;
			for (uint32_t y = 0; y < c_Size; y++)
			{
				for (uint32_t x = 0; x < c_Size; x++)
				{
					bool nearBox = false;
					for (uint32_t ny = y > 2 ? y - 2 : 0; ny <= std::min(y + 2, c_Size - 1) && !nearBox; ny++)
					{
						for (uint32_t nx = x > 2 ? x - 2 : 0; nx <= std::min(x + 2, c_Size - 1) && !nearBox; nx++)
							nearBox = ids[ny * c_Size + nx] == GetID(box);
					}
					boxPixels += ids[y * c_Size + x] == GetID(box) ? 1u : 0u;
					if (!nearBox && glm::any(glm::greaterThan(glm::abs(image[y * c_Size + x] - background), glm::vec3(0.01f))))
						trail++;
				}
			}
			CHECK(boxPixels > 16);
			CHECK(trail == 0);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Scenes with FXAA, and renderers without temporal anti-aliasing, allocate no history and draw as without it")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		TemporalScene scene(TemporalAntiAliasingPreset::Original);
		// FXAA chosen: no history, no motion, no jitter.
		TemporalScene fxaa(TemporalAntiAliasingPreset::Original);
		PostProcessComponent& postProcess = GetPostProcess(fxaa.Contents);
		postProcess.AntiAliasingMethod = AntiAliasingMode::FXAA;
		SceneRenderer renderer = CreateRenderer("FXAA");
		renderer.SetViewportSize(c_Size, c_Size);
		ReadbackImage fxaaImage;
		for (uint32_t frame = 0; frame < 3; frame++)
			fxaaImage = RenderToImage(renderer, fxaa.Contents, fxaa.Camera);
		CHECK_FALSE(renderer.GetStats().TemporalAntiAliasing);
		CHECK(renderer.GetTemporalHistoryTexture() == nullptr);
		CHECK(renderer.GetVelocityTexture() == nullptr);
		CHECK(renderer.GetResolvedHDRTexture() == renderer.GetHDRTexture());

		// A renderer of single images draws a TAA scene as with FXAA.
		SceneRenderer single = CreateRenderer("Single", GPUDrivenMode::Off, false);
		single.SetViewportSize(c_Size, c_Size);
		ReadbackImage singleImage;
		for (uint32_t frame = 0; frame < 3; frame++)
			singleImage = RenderToImage(single, scene.Contents, scene.Camera);
		CHECK_FALSE(single.GetStats().TemporalAntiAliasing);
		CHECK(single.GetTemporalHistoryTexture() == nullptr);
		CHECK(single.GetVelocityTexture() == nullptr);
		CHECK(singleImage.Pixels == fxaaImage.Pixels);
		CHECK(single.GetStats().DrawCommands == renderer.GetStats().DrawCommands);

		// And a renderer that draws it with TAA does not use FXAA: its first frame is the scene's image as drawn without
		// anti-aliasing (jitter phase 0 offsets x by 0).
		SceneRenderer temporal = CreateRenderer("Temporal");
		temporal.SetViewportSize(c_Size, c_Size);
		const ReadbackImage temporalImage = RenderToImage(temporal, scene.Contents, scene.Camera);
		CHECK(temporal.GetStats().TemporalAntiAliasing);
		CHECK(temporal.GetTemporalHistoryTexture() != nullptr);
		CHECK(temporal.GetVelocityTexture() != nullptr);
		CHECK(temporalImage.Pixels != singleImage.Pixels);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("With TAA the shadows' and the ambient occlusion's noise turns every frame, and the history smooths it")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		Scene scene;
		PostProcessComponent& postProcess = AddTemporalPostProcess(scene, TemporalAntiAliasingPreset::Original);
		postProcess.AmbientOcclusion = true;
		postProcess.Exposure = -2.0f;
		Entity sun = AddSun(scene, glm::normalize(glm::vec3(-0.3f, -1.0f, 0.0f)), 1.0f);
		sun.GetComponent<DirectionalLightComponent>().ShadowSoftness = 6.0f; // A wide penumbra
		Entity ground = AddMesh(scene, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Ground");
		ground.GetComponent<TransformComponent>().Scale = glm::vec3(30.0f);
		// A roof above the camera (seen from below it is culled) whose shadow's edge crosses the view, and a box in the
		// penumbra whose contact with the ground the ambient occlusion darkens.
		Entity roof = AddMesh(scene, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(5.0f, 4.0f, 0.0f), "Roof");
		roof.GetComponent<TransformComponent>().Scale = glm::vec3(10.0f);
		AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(-1.2f, 0.25f, 1.0f), "Box").GetComponent<TransformComponent>().Scale = glm::vec3(0.5f);
		const SceneCamera camera = OrthographicLookAt(glm::vec3(-1.2f, 3.0f, 0.0f), glm::vec3(-1.2f, 0.0f, 0.0f), 4.0f);
		SceneRenderer renderer = CreateRenderer("Noise");
		renderer.SetViewportSize(c_Size, c_Size);

		// Luminance of the ground's pixels away from other surfaces (no geometric edge in their neighborhood); NaN elsewhere.
		const auto groundLuminance = [&](const std::vector<glm::vec3>& image)
		{
			const std::vector<uint32_t> ids = ReadIDs(renderer);
			std::vector<float> luminance(image.size(), std::numeric_limits<float>::quiet_NaN());
			for (uint32_t y = 1; y + 1 < c_Size; y++)
			{
				for (uint32_t x = 1; x + 1 < c_Size; x++)
				{
					bool interior = true;
					for (int dy = -1; dy <= 1; dy++)
					{
						for (int dx = -1; dx <= 1; dx++)
							interior = interior && ids[(y + dy) * c_Size + x + dx] == GetID(ground);
					}
					if (interior)
						luminance[y * c_Size + x] = glm::dot(image[y * c_Size + x], c_LuminanceWeights);
				}
			}
			return luminance;
		};
		// How rough the shading of the ground's penumbra (`penumbra`) is: the squared second differences along rows and columns,
		// which a smooth penumbra (a ramp) keeps small and noise does not.
		std::vector<bool> penumbra(static_cast<size_t>(c_Size) * c_Size, false);
		const auto roughness = [&penumbra](const std::vector<float>& luminance)
		{
			double energy = 0.0;
			for (uint32_t y = 1; y + 1 < c_Size; y++)
			{
				for (uint32_t x = 1; x + 1 < c_Size; x++)
				{
					const size_t pixel = static_cast<size_t>(y) * c_Size + x;
					if (!penumbra[pixel])
						continue;
					for (const size_t step : { size_t(1), size_t(c_Size) })
					{
						const float curve = luminance[pixel - step] + luminance[pixel + step] - 2.0f * luminance[pixel];
						if (!std::isnan(curve))
							energy += static_cast<double>(curve) * curve;
					}
				}
			}
			return energy;
		};
		const auto readOcclusion = [&]()
		{
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(renderer.GetAmbientOcclusionTexture(), image));
			return image.Pixels;
		};

		// Frames 1 and 9 have the same jitter phase: only the noise's turn tells them apart.
		std::vector<float> first;
		std::vector<float> ninth;
		std::vector<uint8_t> firstOcclusion;
		std::vector<uint8_t> ninthOcclusion;
		for (uint32_t frame = 0; frame < 16; frame++)
		{
			REQUIRE(renderer.Render(scene, camera));
			if (frame == 0 || frame == SceneRenderer::c_TemporalJitterPhases)
			{
				(frame == 0 ? first : ninth) = groundLuminance(ReadHDR(renderer.GetHDRTexture()));
				(frame == 0 ? firstOcclusion : ninthOcclusion) = readOcclusion();
			}
		}
		uint32_t groundPixels = 0;
		uint32_t changed = 0;
		for (size_t pixel = 0; pixel < first.size(); pixel++)
		{
			if (std::isnan(first[pixel]))
				continue;
			groundPixels++;
			changed += first[pixel] != ninth[pixel] ? 1u : 0u;
		}
		REQUIRE(groundPixels > c_Size * c_Size / 2);
		CHECK(changed > c_Size);
		CHECK(firstOcclusion != ninthOcclusion);

		const std::vector<float> converged = groundLuminance(ReadHDR(renderer.GetResolvedHDRTexture()));
		// The penumbra: the ground between its shadowed and its lit brightness.
		float shadowed = std::numeric_limits<float>::max();
		float lit = 0.0f;
		for (const float value : converged)
		{
			if (!std::isnan(value))
			{
				shadowed = std::min(shadowed, value);
				lit = std::max(lit, value);
			}
		}
		uint32_t penumbraPixels = 0;
		for (size_t pixel = 0; pixel < converged.size(); pixel++)
		{
			penumbra[pixel] = converged[pixel] > shadowed + 0.05f * (lit - shadowed) && converged[pixel] < lit - 0.05f * (lit - shadowed);
			penumbraPixels += penumbra[pixel] ? 1u : 0u;
		}
		CHECK(penumbraPixels > c_Size * 2);
		const double singleEnergy = roughness(first);
		const double convergedEnergy = roughness(converged);
		INFO("changed " << changed << " of " << groundPixels << ", penumbra " << penumbraPixels << ", roughness " << singleEnergy << " -> " << convergedEnergy);
		CHECK(convergedEnergy < 0.5 * singleEnergy);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Both paths resolve the same image over sixteen frames of a moving scene")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		TemporalScene scene(TemporalAntiAliasingPreset::Original);
		SceneRenderer cpu = CreateRenderer("CPU", GPUDrivenMode::Off);
		SceneRenderer gpuDriven = CreateRenderer("GPU", GPUDrivenMode::Auto);
		cpu.SetViewportSize(c_Size, c_Size);
		gpuDriven.SetViewportSize(c_Size, c_Size);
		for (uint32_t frame = 0; frame < 16; frame++)
		{
			CAPTURE(frame);
			const ReadbackImage cpuImage = RenderToImage(cpu, scene.Contents, scene.Camera);
			const ReadbackImage gpuImage = RenderToImage(gpuDriven, scene.Contents, scene.Camera);
			CHECK(MaxDifference(cpuImage, gpuImage) <= 1);
			CHECK(ReadIDs(cpu) == ReadIDs(gpuDriven));
			CHECK(cpu.GetStats().TemporalFrames == frame + 1);
			CHECK(gpuDriven.GetStats().TemporalFrames == frame + 1);
			CHECK(gpuDriven.GetStats().GPUDriven);
			Entity box = scene.Boxes[static_cast<size_t>(frame) % scene.Boxes.size()];
			box.GetComponent<TransformComponent>().Translation.y += 0.1f;
			box.MarkModified<TransformComponent>();
			scene.Camera = LookAt(glm::vec3(0.05f * static_cast<float>(frame + 1), 9.0f, 8.0f), glm::vec3(0.0f, 0.0f, -2.5f));
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
