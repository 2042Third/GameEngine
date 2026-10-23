#include "Renderer/SceneRendererTestUtils.h"

#include "Perf/LocalShadowsMemory.h"
#include "Perf/PerfUtils.h"
#include "Perf/RendererPerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Renderer/TextureStreamingTestUtils.h"

#include <Strata/Asset/RuntimeAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/FrameProfiler.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Process.h>
#include <Strata/Renderer/Material.h>
#include <Strata/Renderer/RenderScene.h>
#include <Strata/Renderer/TextureStreamer.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

// The scene renderer's own CPU cost on the scaling scenes (PerfGPU: Release and Dist, with a GPU): SceneRenderer::Render
// timed alone, frame after frame as an application runs them (the device's frame around each), with the job system as
// applications start it. The editor's frames on the same scenes are measured by RendererFramePerfTests.cpp.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Width = 1920;
	constexpr uint32_t c_Height = 1080;
	constexpr int c_WarmupFrames = 10;
	constexpr int c_MeasuredFrames = 60;

	// A view of the generated grid of `count` cubes from above, the whole grid in view.
	SceneCamera GridView(size_t count)
	{
		const float half = static_cast<float>(std::ceil(std::sqrt(static_cast<double>(count)))) * Perf::c_GridSpacing * 0.5f;
		SceneCamera camera = LookAt(glm::vec3(0.0f, half * 1.6f, half * 1.3f), glm::vec3(0.0f), static_cast<float>(c_Width) / static_cast<float>(c_Height));
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(60.0f), static_cast<float>(c_Width) / static_cast<float>(c_Height), 0.1f, half * 10.0f);
		camera.Far = half * 10.0f;
		return camera;
	}

	// The CPU time of SceneRenderer::Render and the GPU's busy time, frames run like an application's (Perf::MeasureFrames).
	Perf::FrameMeasurement MeasureRender(GPUContext& gpu, SceneRenderer& renderer, Scene& scene, const SceneCamera& camera, std::string_view name)
	{
		const Perf::FrameMeasurement measurement = Perf::MeasureFrames(gpu, c_WarmupFrames, c_MeasuredFrames, [&] { REQUIRE(renderer.Render(scene, camera)); });
		const SceneRendererStats& stats = renderer.GetStats();
		const DrawnCounts drawn = GetDrawnCounts(renderer);
		MESSAGE(name, ": ", measurement.CPU.MedianMs, " ms median, GPU ", measurement.GPU ? measurement.GPU->MedianMs : 0.0, " ms busy (",
			std::string(stats.GPUDriven ? "GPU-driven, " : ""), std::string(stats.FeedbackRequests ? "requests from feedback, " : ""), drawn.Instances, " instances, ", drawn.Draws, " draws, ", stats.DrawCommands, " draw commands, ", drawn.Triangles,
			" triangles, extract ", stats.ExtractMs, " ms, sort ", stats.SortMs, " ms; GPU timer ", measurement.TimerCost.MedianMs, " ms per frame)");
		return measurement;
	}

}

TEST_SUITE("PerfGPU.Renderer")
{
	TEST_CASE("Render cost of 100,000, 300,000 and 1,000,000 cubes and of 10,000 cubes with unique materials")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		SceneTestAssets assets;

		for (const size_t count : { size_t(100'000), size_t(300'000), size_t(1'000'000) })
		{
			CAPTURE(count);
			Scene scene("Cubes");
			Perf::AddCameraAndLight(scene);
			Perf::CreateCubeGrid(scene, count);
			SceneRenderer renderer(Perf::MeasuredRendererSpecification());
			renderer.SetViewportSize(c_Width, c_Height);
			// The median frame (every attempt renders its own warm-up and measured frames); for 100,000 cubes also the GPU's
			// median busy time and what timing the GPU costs the CPU per frame (GPUProfiler's own time: writes, resolves). The
			// timer cost is the median frame's too: each frame writes and resolves the same timestamps, so a frame the machine
			// preempted says nothing about it. GPU metrics only on devices that write timestamps.
			const bool measuresGPU = Perf::MeasuresGPU();
			if (count == 100'000)
			{
				std::vector<std::string> keys = { "PerfGPU.Renderer.Render100kCubes", "PerfGPU.Renderer.GPUTimerCost" };
				if (measuresGPU)
					keys.push_back("PerfGPU.Renderer.Render100kCubesGPU");
				Perf::CheckBudgetsConfirmed(keys, [&]
				{
					const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, GridView(count), "100k cubes");
					CHECK(GetDrawnCounts(renderer).Instances == count);
					CHECK(renderer.GetStats().InstanceBytesUploaded == 0); // A still frame uploads nothing
					Perf::MetricValues values { { "PerfGPU.Renderer.Render100kCubes", measurement.CPU.MedianMs },
						{ "PerfGPU.Renderer.GPUTimerCost", measurement.TimerCost.MedianMs } };
					if (measuresGPU)
						values.emplace("PerfGPU.Renderer.Render100kCubesGPU", Perf::MeasureGPU(measurement));
					return values;
				});
				continue;
			}
			// A million: the GPU-driven path's CPU cost does not grow with the instances (its frames take their requests from
			// the GPU's visibility feedback); far ones below half a pixel are left out of the draws.
			if (count == 1'000'000)
			{
				std::vector<std::string> keys = { "PerfGPU.Renderer.Render1MCubes" };
				if (measuresGPU)
					keys.push_back("PerfGPU.Renderer.Render1MCubesGPU");
				Perf::CheckBudgetsConfirmed(keys, [&]
				{
					const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, GridView(count), "1M cubes");
					CHECK(GetDrawnCounts(renderer).VisibleProxies == count);
					CHECK(renderer.GetStats().InstanceBytesUploaded == 0);
					Perf::MetricValues values { { "PerfGPU.Renderer.Render1MCubes", measurement.CPU.MedianMs } };
					if (measuresGPU)
						values.emplace("PerfGPU.Renderer.Render1MCubesGPU", Perf::MeasureGPU(measurement));
					return values;
				});
				continue;
			}
			Perf::CheckBudgetConfirmed("PerfGPU.Renderer.Render300kCubes", [&]
			{
				const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, GridView(count), "300k cubes");
				CHECK(GetDrawnCounts(renderer).Instances == count);
				CHECK(renderer.GetStats().InstanceBytesUploaded == 0); // A still frame uploads nothing
				return measurement.CPU.MedianMs;
			});
		}

		std::vector<AssetHandle> materials;
		for (uint32_t index = 0; index < 10'000; index++)
		{
			MaterialProperties properties;
			properties.BaseColor = glm::vec4(static_cast<float>(index % 101) / 100.0f, 0.5f, static_cast<float>(index % 37) / 36.0f, 1.0f);
			materials.push_back(assets.AddMaterial(properties));
		}
		Scene scene("Materials");
		Perf::AddCameraAndLight(scene);
		Perf::CreateCubeGrid(scene, materials.size(), materials);
		SceneRenderer renderer(Perf::MeasuredRendererSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		// The median frame, and the opaque draws of the last one (the GPU's on the GPU-driven path): batching is material-free,
		// so the 10,000 materials share the draws of the cube's pipeline state (a count, the same in every frame).
		Perf::CheckBudgetsConfirmed({ "PerfGPU.Renderer.Render10kMaterials", "PerfGPU.Renderer.Render10kMaterials.OpaqueDrawCalls" }, [&]
		{
			const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, GridView(materials.size()), "10k materials");
			CHECK(renderer.GetStats().Materials == materials.size());
			return Perf::MetricValues { { "PerfGPU.Renderer.Render10kMaterials", measurement.CPU.MedianMs },
				{ "PerfGPU.Renderer.Render10kMaterials.OpaqueDrawCalls", static_cast<double>(GetDrawnCounts(renderer).Draws) } };
		});
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Starting play on 1,000,000 rendered cubes clones their render scene and its GPU copy")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		SceneTestAssets assets;
		constexpr size_t c_Count = 1'000'000;
		Ref<Scene> source = CreateRef<Scene>("Cubes");
		Perf::AddCameraAndLight(*source);
		Perf::CreateCubeGrid(*source, c_Count);
		SceneRenderer renderer(Perf::MeasuredRendererSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		const SceneCamera camera = GridView(c_Count);
		// The copy hook's time in Scene::Copy (the render scene's clone), and what the copy's first Render costs beyond the
		// source's median Render (its GPU copy cloned on the GPU, no proxy built or uploaded).
		Perf::CheckBudgetConfirmed("PerfGPU.Renderer.PlayStartRender1MCubes", [&]
		{
			const Perf::FrameMeasurement steady = MeasureRender(gpu, renderer, *source, camera, "1M cubes before play");
			Ref<Scene> copy = Scene::Copy(source);
			const RenderScene* clone = RenderScene::Find(*copy);
			REQUIRE(clone != nullptr);
			const double hookMs = clone->GetStats().CloneMs;
			REQUIRE(gpu.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			const auto start = std::chrono::steady_clock::now();
			REQUIRE(renderer.Render(*copy, camera));
			const double firstMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			gpu.GetDevice().EndFrame();
			CHECK(renderer.GetStats().ProxyBytesUploaded == 0);
			CHECK(renderer.GetStats().ProxyUpdates == 0);
			MESSAGE("Play start of 1M cubes: clone ", hookMs, " ms, first Render ", firstMs, " ms (", steady.CPU.MedianMs, " ms before)");
			Scene::ReleaseInBackground(std::move(copy));
			return hookMs + std::max(0.0, firstMs - steady.CPU.MedianMs);
		});
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Render cost of 10,000 distinct meshes")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		SceneTestAssets assets;
		constexpr size_t c_Count = 10'000;
		Scene scene("UniqueMeshes");
		Perf::AddCameraAndLight(scene);
		Perf::CreateUniqueMeshGrid(scene, assets, c_Count);
		SceneRenderer renderer(Perf::MeasuredRendererSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		// The median Render (extraction, recording and submitting) and the draws the CPU recorded: the meshes share the
		// geometry pool's buffers, so draws follow pipeline states (buckets), not meshes.
		Perf::CheckBudgetsConfirmed({ "PerfGPU.Renderer.Render10kUniqueMeshes", "PerfGPU.Renderer.Render10kUniqueMeshes.DrawCommands" }, [&]
		{
			const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, GridView(c_Count), "10k unique meshes");
			const SceneRendererStats& stats = renderer.GetStats();
			const DrawnCounts drawn = GetDrawnCounts(renderer);
			CHECK(drawn.Instances == c_Count);
			MESSAGE("10k unique meshes: ", drawn.Draws, " draws, ", drawn.ShadowDraws, " shadow draws, ", stats.DrawCommands, " draw commands");
			return Perf::MetricValues { { "PerfGPU.Renderer.Render10kUniqueMeshes", measurement.CPU.MedianMs },
				{ "PerfGPU.Renderer.Render10kUniqueMeshes.DrawCommands", static_cast<double>(stats.DrawCommands) } };
		});
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Render cost of 100,000 cubes with temporal anti-aliasing")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		SceneTestAssets assets;
		// Render100kCubes' scene with the post-processing's defaults but TAA: its jitter, motion vectors, motion history and
		// resolve (provisional: the reference machine sets the budgets).
		constexpr size_t c_Count = 100'000;
		Scene scene("TemporalCubes");
		Perf::AddCameraAndLight(scene);
		Perf::CreateCubeGrid(scene, c_Count);
		scene.CreateEntity("Post Process").AddComponent<PostProcessComponent>().AntiAliasingMethod = AntiAliasingMode::TAA;
		SceneRenderer renderer(Perf::MeasuredRendererSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		const bool measuresGPU = Perf::MeasuresGPU();
		std::vector<std::string> keys = { "PerfGPU.Renderer.Render100kCubesTAA" };
		if (measuresGPU)
			keys.push_back("PerfGPU.Renderer.Render100kCubesTAAGPU");
		Perf::CheckBudgetsConfirmed(keys, [&]
		{
			const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, GridView(c_Count), "100k cubes with TAA");
			CHECK(GetDrawnCounts(renderer).Instances == c_Count);
			// A still view: the history goes on from frame to frame, and the motion history copies nothing.
			CHECK(renderer.GetStats().TemporalAntiAliasing);
			CHECK(renderer.GetStats().TemporalFrames > static_cast<uint32_t>(c_MeasuredFrames));
			CHECK(renderer.GetStats().MotionCopiedInstances == 0);
			Perf::MetricValues values { { "PerfGPU.Renderer.Render100kCubesTAA", measurement.CPU.MedianMs } };
			if (measuresGPU)
				values.emplace("PerfGPU.Renderer.Render100kCubesTAAGPU", Perf::MeasureGPU(measurement));
			return values;
		});
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Render cost of 100,000 cubes in height fog")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		SceneTestAssets assets;
		// Render100kCubes' scene with the default height fog over it: its integral, phase function and inscattering in every
		// pixel the forward pass shades, against Render100kCubes (provisional: the reference machine sets the budgets).
		constexpr size_t c_Count = 100'000;
		Scene scene("FoggyCubes");
		Perf::AddCameraAndLight(scene);
		Perf::CreateCubeGrid(scene, c_Count);
		scene.CreateEntity("Height Fog").AddComponent<HeightFogComponent>();
		SceneRenderer renderer(Perf::MeasuredRendererSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		const bool measuresGPU = Perf::MeasuresGPU();
		std::vector<std::string> keys = { "PerfGPU.Renderer.HeightFog" };
		if (measuresGPU)
			keys.push_back("PerfGPU.Renderer.HeightFogGPU");
		Perf::CheckBudgetsConfirmed(keys, [&]
		{
			const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, GridView(c_Count), "100k cubes in height fog");
			CHECK(GetDrawnCounts(renderer).Instances == c_Count);
			CHECK(renderer.GetStats().HeightFog);
			Perf::MetricValues values { { "PerfGPU.Renderer.HeightFog", measurement.CPU.MedianMs } };
			if (measuresGPU)
				values.emplace("PerfGPU.Renderer.HeightFogGPU", Perf::MeasureGPU(measurement));
			return values;
		});
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Render cost of 100,000 cubes under the atmosphere, with a still sun and with a moving one")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		SceneTestAssets assets;
		// Render100kCubes' scene under the atmosphere (provisional: the reference machine sets the budgets). Still: its
		// background and the light through it, the tables computed once. Moving: the sun turns 0.1 degrees a frame, so the
		// sky-view table is computed every frame and the environment refreshes a step a frame, a new one every 0.5 degrees;
		// the slowest frame is the metric (a hitch is a maximum by nature: the refresh's costliest step).
		constexpr size_t c_Count = 100'000;
		Scene scene("AtmosphereCubes");
		Perf::AddCameraAndLight(scene);
		Perf::CreateCubeGrid(scene, c_Count);
		scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().Source = SkyLightSource::Atmosphere;
		Entity sun;
		for (auto [entity, light] : scene.GetRegistry().view<DirectionalLightComponent>().each())
			sun = Entity(entity, &scene);
		REQUIRE(sun);
		sun.GetComponent<DirectionalLightComponent>().Intensity = 100000.0f;
		sun.MarkModified<DirectionalLightComponent>();
		SceneRenderer renderer(Perf::MeasuredRendererSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		const bool measuresGPU = Perf::MeasuresGPU();
		std::vector<std::string> keys = { "PerfGPU.Renderer.Atmosphere", "PerfGPU.Renderer.AtmosphereSunMovingMaxFrame" };
		if (measuresGPU)
		{
			keys.push_back("PerfGPU.Renderer.AtmosphereGPU");
			keys.push_back("PerfGPU.Renderer.AtmosphereSunMovingMaxFrameGPU");
		}
		Perf::CheckBudgetsConfirmed(keys, [&]
		{
			const SceneCamera camera = GridView(c_Count);
			const Perf::FrameMeasurement still = MeasureRender(gpu, renderer, scene, camera, "100k cubes under the atmosphere");
			CHECK(GetDrawnCounts(renderer).Instances == c_Count);
			CHECK(renderer.GetStats().Atmosphere);
			CHECK(renderer.GetStats().AtmosphereSkyViewUpdates == 0);

			float angle = -50.0f;
			uint32_t refreshSteps = 0;
			uint32_t updates = 0;
			const Perf::FrameMeasurement moving = Perf::MeasureFrames(gpu, c_WarmupFrames, c_MeasuredFrames, [&]
			{
				angle -= 0.1f;
				sun.GetComponent<TransformComponent>().Rotation = glm::angleAxis(glm::radians(angle), glm::vec3(1.0f, 0.0f, 0.0f));
				sun.MarkModified<TransformComponent>();
				REQUIRE(renderer.Render(scene, camera));
				refreshSteps += renderer.GetStats().EnvironmentRefreshSteps;
				updates += renderer.GetStats().EnvironmentUpdates;
			});
			MESSAGE("100k cubes under the atmosphere with the sun moving: ", moving.CPU.MaxMs, " ms slowest frame, GPU ", moving.GPU ? moving.GPU->MaxMs : 0.0,
				" ms slowest busy; ", refreshSteps, " refresh steps, ", updates, " new environments");
			CHECK(updates > 0);
			Perf::MetricValues values { { "PerfGPU.Renderer.Atmosphere", still.CPU.MedianMs },
				{ "PerfGPU.Renderer.AtmosphereSunMovingMaxFrame", moving.CPU.MaxMs } };
			if (measuresGPU)
			{
				values.emplace("PerfGPU.Renderer.AtmosphereGPU", Perf::MeasureGPU(still));
				Perf::MeasureGPU(moving); // Checks that its frames' times arrived
				values.emplace("PerfGPU.Renderer.AtmosphereSunMovingMaxFrameGPU", moving.GPU->MaxMs);
			}
			return values;
		});
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Render cost of 100,000 cubes under 16 shadowed point lights, every view drawn each frame and cached")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		SceneTestAssets assets;
		// Render100kCubes' scene under 16 point lights that cast shadows, 4 by 4 over the grid (provisional: the reference machine
		// sets the budgets): uncached, all 96 views drawn and culled every frame (the worst case, D8), and cached, a still
		// scene's steady state (no view drawn, every light's shadow sampled).
		constexpr size_t c_Count = 100'000;
		Scene scene("ShadowedLampCubes");
		Perf::AddCameraAndLight(scene);
		Perf::CreateCubeGrid(scene, c_Count);
		Perf::CreateShadowedLampGrid(scene, 16, 60.0f);
		const bool measuresGPU = Perf::MeasuresGPU();
		for (const bool cached : { false, true })
		{
			CAPTURE(cached);
			SceneRendererSpecification specification = Perf::MeasuredRendererSpecification();
			specification.CacheLocalShadows = cached;
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(c_Width, c_Height);
			const std::string key = cached ? "PerfGPU.Renderer.LocalShadows16LightsCached" : "PerfGPU.Renderer.LocalShadows16Lights";
			std::vector<std::string> keys = { key };
			if (measuresGPU)
				keys.push_back(key + "GPU");
			Perf::CheckBudgetsConfirmed(keys, [&]
			{
				const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, GridView(c_Count),
					cached ? "100k cubes under 16 cached shadowed lamps" : "100k cubes under 16 shadowed lamps, every view drawn");
				const SceneRendererStats& stats = renderer.GetStats();
				const DrawnCounts drawn = GetDrawnCounts(renderer);
				CHECK(drawn.Instances == c_Count);
				CHECK(stats.LocalShadowLights == 16);
				CHECK(stats.LocalShadowViewsDrawn == (cached ? 0u : 96u));
				if (!cached)
					CHECK(drawn.LocalShadowCasters > 0);
				// The views draw in GPU.Shadows (with the sun's cascades): its time is what the views add.
				std::string stages;
				for (const GPUStageTime& stage : renderer.GetGPUStats().StageMs)
					stages += fmt::format(" {} {:.3f} ms", FrameProfiler::Get().GetStageName(stage.Stage), stage.Milliseconds);
				MESSAGE("local shadow casters ", drawn.LocalShadowCasters, ", draws ", drawn.LocalShadowDraws, "; GPU stages of the latest frame:", stages);
				Perf::MetricValues values { { key, measurement.CPU.MedianMs } };
				if (measuresGPU)
					values.emplace(key + "GPU", Perf::MeasureGPU(measurement));
				return values;
			});
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Peak memory of a TAA scene of 100,000 cubes under 16 shadowed point lights")
	{
		// What a renderer with motion vectors, the temporal history and the local shadows' atlas needs at once, as a game runs
		// it at 1920x1080 (D2, D8; provisional): in a process of its own, so that no other test's allocations count.
		const std::filesystem::path directory = CreateTemporaryDirectory("LocalShadowsMemory");
		Perf::CheckBudgetConfirmed("PerfGPU.Renderer.TAALocalShadowsPeakMemory", [&]
		{
			const std::filesystem::path resultFile = directory / "Result.json";
			ProcessSpecification process;
			process.Executable = Platform::GetExecutablePath();
			process.Arguments = { "--strata-test-helper=local-shadows-memory", FileSystem::ToUTF8(resultFile) };
			const Process::RunResult run = Process::Run(process, std::chrono::minutes(10));
			INFO("Helper output:\n", run.Output);
			REQUIRE_MESSAGE(run.Started, run.Error);
			REQUIRE_FALSE(run.TimedOut);
			REQUIRE(run.ExitCode == 0);
			std::string error;
			const std::optional<LocalShadowsMemoryResult> result = ReadLocalShadowsMemoryResult(resultFile, error);
			REQUIRE_MESSAGE(result, error);
			REQUIRE_MESSAGE(result->Completed, result->Error);
			REQUIRE_MESSAGE(!result->ValidationEnabled, "the helper's device validates: run PerfGPU with STRATA_TEST_GPU_VALIDATION=0 (ctest -L perf does)");
			CHECK(result->NewErrors == 0);
			CHECK(result->LocalShadowLights == 16);
			CHECK(result->TemporalAntiAliasing);
			MESSAGE("TAA scene of 100k cubes under 16 shadowed lamps: peak private ", result->PeakPrivateMB, " MiB");
			return result->PeakPrivateMB;
		});
	}

	TEST_CASE("Render cost of 100,000 and 300,000 cubes whose shadows cover the view")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		SceneTestAssets assets;

		for (const size_t count : { size_t(100'000), size_t(300'000) })
		{
			CAPTURE(count);
			// The sun's shadows reach four times the grid's half extent from the camera, as the scale audit's scenes have it:
			// every cube of the view casts into the cascades.
			const float halfExtent = static_cast<float>(std::ceil(std::sqrt(static_cast<double>(count)))) * Perf::c_GridSpacing * 0.5f;
			Scene scene("ShadowedCubes");
			Perf::AddCameraAndLight(scene, 4.0f * halfExtent);
			Perf::CreateCubeGrid(scene, count);
			SceneRenderer renderer(Perf::MeasuredRendererSpecification());
			renderer.SetViewportSize(c_Width, c_Height);
			const char* key = count == 100'000 ? "PerfGPU.Renderer.Render100kCubesShadowed" : "PerfGPU.Renderer.Render300kCubesShadowed";
			Perf::CheckBudgetConfirmed(key, [&]
			{
				const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, GridView(count), count == 100'000 ? "100k shadowed cubes"
					: "300k shadowed cubes");
				const DrawnCounts drawn = GetDrawnCounts(renderer);
				CHECK(drawn.Instances == count);
				// Every cube is a caster: drawn into the cascades where it spans a texel, left out of those where it does not (the
				// far cascades' texels are wider than a cube: SubTexelCasters). Both are summed over the cascades.
				CHECK(drawn.ShadowCasters + drawn.SubTexelCasters >= count * 9 / 10);
				CHECK(drawn.ShadowCasters >= count / 10);
				MESSAGE("shadow casters ", drawn.ShadowCasters, ", left out below a texel ", drawn.SubTexelCasters, ", shadow draws ", drawn.ShadowDraws);
				return measurement.CPU.MedianMs;
			});
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Idle frames of 3,000 streamed textures in view")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		// 3,000 textures in an asset pack (64x64 BC7 of a color each, loaded partly: their 32x32 tail first), a material for
		// each and a cube for each, the whole grid in view.
		constexpr size_t c_Textures = 3'000;
		std::vector<StreamingPackAsset> packAssets;
		std::vector<AssetHandle> materials;
		for (size_t index = 0; index < c_Textures; index++)
		{
			const AssetHandle texture(0xB00000 + 2 * index);
			const AssetHandle material(0xB00001 + 2 * index);
			const std::array<uint8_t, 4> color = { static_cast<uint8_t>(index * 7), static_cast<uint8_t>(index / 13), static_cast<uint8_t>(255 - index % 200), 255 };
			packAssets.push_back(StreamingPackAsset { texture, AssetType::Texture, fmt::format("Textures/Color{}.png", index), CookSolidTexture(64, color) });
			MaterialProperties properties;
			properties.BaseColorMap = texture;
			packAssets.push_back(StreamingPackAsset { material, AssetType::Material, fmt::format("Materials/Color{}.stmat", index), SerializeMaterial(properties) });
			materials.push_back(material);
		}
		std::string error;
		const Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(WriteStreamingPack(packAssets), &error);
		REQUIRE_MESSAGE(manager, error);
		AssetManager::SetActive(manager);
		TextureStreamer* streamer = TextureStreamer::Find(*manager);
		REQUIRE(streamer);
		Scene scene("Textures");
		Perf::AddCameraAndLight(scene);
		Perf::CreateCubeGrid(scene, c_Textures, materials);
		SceneRenderer renderer(Perf::MeasuredRendererSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		const SceneCamera camera = GridView(c_Textures);

		// An application frame's work for the assets and the view: the manager's update (finalization and its texture
		// streamer) and the renderer's frame, which reports what it drew to the streamer.
		auto runFrame = [&]()
		{
			REQUIRE(gpu.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			const auto start = std::chrono::steady_clock::now();
			manager->Update();
			REQUIRE(renderer.Render(scene, camera));
			const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			gpu.GetDevice().EndFrame();
			return milliseconds;
		};
		// Until every texture is in at the level the view wants and nothing streams.
		bool settled = false;
		for (int frame = 0; frame < 3'000 && !settled; frame++)
		{
			runFrame();
			const AssetManagerStats stats = manager->GetStats();
			settled = renderer.GetStats().PendingAssets == 0 && stats.Detail.Reads == 0 && stats.Detail.Changes == 0 && stats.Detail.AssetsBelowWanted == 0
				&& streamer->GetTexturesLookedAt() == 0;
		}
		REQUIRE(settled);
		CHECK(renderer.GetStats().Materials == c_Textures);

		// The median frame: a still view of settled textures costs what drawing them costs, nothing per texture in the
		// streamer.
		Perf::CheckBudgetConfirmed("PerfGPU.Renderer.IdleFrame3kStreamedTextures", [&]
		{
			std::vector<double> samples;
			for (int frame = 0; frame < c_WarmupFrames + c_MeasuredFrames; frame++)
			{
				const double milliseconds = runFrame();
				if (frame >= c_WarmupFrames)
					samples.push_back(milliseconds);
			}
			CHECK(streamer->GetTexturesLookedAt() == 0);
			const Perf::Measurement measurement = Perf::Summarize(samples);
			MESSAGE("3,000 streamed textures: ", measurement.MedianMs, " ms median frame");
			return measurement.MedianMs;
		});
		AssetManager::SetActive(nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
