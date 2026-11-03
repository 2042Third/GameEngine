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
#include <Strata/Renderer/PostProcessData.h>
#include <Strata/Renderer/RenderScene.h>
#include <Strata/Renderer/RendererQuality.h>
#include <Strata/Renderer/TextureStreamer.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
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

	// The view of GridView: its eye's height and distance in halves of the grid's extent.
	constexpr float c_GridViewHeight = 1.6f;
	constexpr float c_GridViewDistance = 1.3f;

	// A view of the generated grid of `count` cubes from above, the whole grid in view; `yaw` turns it (radians) about the
	// vertical through its eye.
	SceneCamera GridView(size_t count, float yaw = 0.0f)
	{
		const float half = static_cast<float>(std::ceil(std::sqrt(static_cast<double>(count)))) * Perf::c_GridSpacing * 0.5f;
		const glm::vec3 eye(0.0f, half * c_GridViewHeight, half * c_GridViewDistance);
		const glm::vec3 toCenter = -eye;
		const glm::vec3 turned(toCenter.x * std::cos(yaw) + toCenter.z * std::sin(yaw), toCenter.y, -toCenter.x * std::sin(yaw) + toCenter.z * std::cos(yaw));
		SceneCamera camera = LookAt(eye, eye + turned, static_cast<float>(c_Width) / static_cast<float>(c_Height));
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(60.0f), static_cast<float>(c_Width) / static_cast<float>(c_Height), 0.1f, half * 10.0f);
		camera.Far = half * 10.0f;
		return camera;
	}

	// The median over the profiler's recorded frames (the measured ones) of a GPU stage's time, in milliseconds.
	double MedianGPUStageMs(const char* name)
	{
		const FrameProfiler& profiler = FrameProfiler::Get();
		std::vector<float> samples;
		for (FrameStageID stage = 0; stage < profiler.GetStageCount(); stage++)
		{
			if (profiler.GetStageName(stage) != name)
				continue;
			for (uint32_t age = 0; age < std::min<uint32_t>(profiler.GetRecordedFrameCount(), c_MeasuredFrames); age++)
			{
				const FrameSample frame = profiler.GetFrame(age);
				if (frame.HasGPU)
					samples.push_back(frame.GPUStageMs[stage]);
			}
		}
		if (samples.empty())
			return 0.0;
		std::sort(samples.begin(), samples.end());
		return samples[samples.size() / 2];
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
		// The GPU stages' medians over the measured frames, to compare builds pass by pass.
		std::string stages;
		const FrameProfiler& profiler = FrameProfiler::Get();
		for (FrameStageID stage = 0; stage < profiler.GetStageCount(); stage++)
		{
			const std::string& stageName = profiler.GetStageName(stage);
			if (stageName.starts_with("GPU."))
				stages += fmt::format(" {} {:.4f}", stageName, MedianGPUStageMs(stageName.c_str()));
		}
		MESSAGE(name, " GPU stages (median ms):", stages);
		return measurement;
	}

	// Render100kCubes' scene at 3840x2160 under High with TAA Sharp and the post chain at its defaults (Filmic, automatic
	// exposure metered center-weighted with the key, bloom at the default radius, the lens vignette, the dither) and a
	// grading that runs (white balance 5600 K, a slope): the median of the GPU stage `stage` over the measured frames,
	// recorded as `key` on devices that write timestamps. `motionBlur` turns the Post Process's motion blur on; the camera
	// turns `turnPixels` pixels a frame at the image's center (0: still); with `lens`, the frames have depth of field; `method`
	// is the anti-aliasing method (the others leave TAA).
	void MeasurePostStage4K(const std::string& key, const char* stage, bool motionBlur, float turnPixels, std::optional<CameraLens> lens = std::nullopt,
		AntiAliasingMode method = AntiAliasingMode::TAA)
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!Perf::MeasuresGPU())
		{
			MESSAGE(key, ": not measured, the device writes no timestamps");
			return;
		}
		Perf::ScopedApplicationJobSystem jobs;
		SceneTestAssets assets;
		constexpr size_t c_Count = 100'000;
		constexpr uint32_t c_Width4K = 3840;
		constexpr uint32_t c_Height4K = 2160;
		Scene scene("PostCubes4K");
		Perf::AddCameraAndLight(scene);
		Perf::CreateCubeGrid(scene, c_Count);
		PostProcessComponent& postProcess = scene.CreateEntity("Post Process").AddComponent<PostProcessComponent>();
		postProcess.AntiAliasingMethod = method;
		postProcess.WhiteBalanceTemperature = 5600.0f;
		postProcess.ColorSlope = glm::vec3(1.05f, 1.0f, 0.95f);
		postProcess.MotionBlur = motionBlur;
		SceneRendererSpecification specification = Perf::MeasuredRendererSpecification();
		specification.Quality = RendererQuality::GetPreset(RendererQualityPreset::High);
		SceneRenderer renderer(specification);
		renderer.SetViewportSize(c_Width4K, c_Height4K);
		// A turn of `turnPixels` at the center: the view's 60 degrees span its height, and a turn about the vertical moves
		// the center by the yaw times the cosine of the view's pitch.
		const float pixelsPerRadian = 0.5f * static_cast<float>(c_Height4K) / std::tan(glm::radians(30.0f));
		const float cosPitch = c_GridViewDistance / std::sqrt(c_GridViewHeight * c_GridViewHeight + c_GridViewDistance * c_GridViewDistance);
		const float yawPerFrame = turnPixels / (pixelsPerRadian * cosPitch);
		uint32_t frame = 0;
		Perf::CheckBudgetsConfirmed({ key }, [&]
		{
			// Every attempt turns from the same view (the jump back is a warm-up frame).
			frame = 0;
			const Perf::FrameMeasurement measurement = Perf::MeasureFrames(gpu, c_WarmupFrames, c_MeasuredFrames, [&]
			{
				SceneRenderOptions options;
				options.Lens = lens;
				REQUIRE(renderer.Render(scene, GridView(c_Count, yawPerFrame * static_cast<float>(frame)), nullptr, options));
				frame++;
			});
			const SceneRendererStats& stats = renderer.GetStats();
			CHECK(stats.TemporalAntiAliasing == (method == AntiAliasingMode::TAA));
			CHECK(stats.MorphologicalAntiAliasing == (method == AntiAliasingMode::Morphological));
			CHECK(stats.Dithered);
			CHECK(stats.MotionBlurred == motionBlur);
			if (motionBlur)
				CHECK(stats.MotionBlurSamples == 12u);
			CHECK(stats.DepthOfField == lens.has_value());
			const double stageMs = MedianGPUStageMs(stage);
			MESSAGE(key, ": ", std::string(stage), " ", stageMs, " ms median, GPU ", measurement.GPU ? measurement.GPU->MedianMs : 0.0, " ms busy");
			return Perf::MetricValues { { key, stageMs } };
		});
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	// Render100kCubes' scene with the post-processing's defaults but TAA under `preset`: its jitter, motion vectors, motion
	// history and resolve, measured as `key` (its CPU median) and `key`GPU (the GPU's median busy time); with `reflections`,
	// screen-space reflections traced from every surface (the maximum roughness 1).
	void MeasureTemporalCubes(TemporalAntiAliasingPreset preset, const std::string& key, bool reflections = false)
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		SceneTestAssets assets;
		constexpr size_t c_Count = 100'000;
		Scene scene("TemporalCubes");
		Perf::AddCameraAndLight(scene);
		Perf::CreateCubeGrid(scene, c_Count);
		PostProcessComponent& postProcess = scene.CreateEntity("Post Process").AddComponent<PostProcessComponent>();
		postProcess.AntiAliasingMethod = AntiAliasingMode::TAA;
		postProcess.TAAPreset = preset;
		postProcess.ScreenSpaceReflections = reflections;
		postProcess.ScreenSpaceReflectionsMaxRoughness = 1.0f;
		SceneRenderer renderer(Perf::MeasuredRendererSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		const bool measuresGPU = Perf::MeasuresGPU();
		std::vector<std::string> keys = { key };
		if (measuresGPU)
			keys.push_back(key + "GPU");
		Perf::CheckBudgetsConfirmed(keys, [&]
		{
			const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, GridView(c_Count), key);
			CHECK(GetDrawnCounts(renderer).Instances == c_Count);
			// A still view: the history goes on from frame to frame, and the motion history copies nothing.
			CHECK(renderer.GetStats().TemporalAntiAliasing);
			CHECK(renderer.GetStats().TemporalFrames > static_cast<uint32_t>(c_MeasuredFrames));
			CHECK(renderer.GetStats().MotionCopiedInstances == 0);
			CHECK(renderer.GetStats().ScreenSpaceReflections == reflections);
			if (measuresGPU)
				MESSAGE(key, ": GPU.Reflections ", MedianGPUStageMs("GPU.Reflections"), " ms, GPU.Opaque ", MedianGPUStageMs("GPU.Opaque"), " ms");
			Perf::MetricValues values { { key, measurement.CPU.MedianMs } };
			if (measuresGPU)
				values.emplace(key + "GPU", Perf::MeasureGPU(measurement));
			return values;
		});
		CHECK(gpu.GetNewErrorCount() == 0);
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
		// The default TAA preset, Sharp (provisional: the reference machine sets the budgets).
		MeasureTemporalCubes(TemporalAntiAliasingPreset::Sharp, "PerfGPU.Renderer.Render100kCubesTAA");
	}

	TEST_CASE("Render cost of 100,000 cubes with temporal anti-aliasing's Original preset")
	{
		// The first TAA, without Sharp's techniques: what Sharp costs against it (provisional).
		MeasureTemporalCubes(TemporalAntiAliasingPreset::Original, "PerfGPU.Renderer.Render100kCubesTAAOriginal");
	}

	TEST_CASE("Render cost of 100,000 cubes with temporal anti-aliasing and screen-space reflections")
	{
		// Render100kCubesTAA's scene with screen-space reflections traced from every surface: the prepass's surface target, the
		// half-resolution trace (GPU.Reflections) and the forward pass's lookups, against Render100kCubesTAA (provisional: the
		// reference machine sets the budgets).
		MeasureTemporalCubes(TemporalAntiAliasingPreset::Sharp, "PerfGPU.Renderer.SSR", true);
	}

	TEST_CASE("The post chain's cost at 3840x2160")
	{
		// GPU.Post with every post effect at its default and a grading, the camera still (provisional: the reference machine
		// sets the budget).
		MeasurePostStage4K("PerfGPU.Renderer.Post4K", "GPU.Post", false, 0.0f);
	}

	TEST_CASE("Motion blur's cost at 3840x2160")
	{
		// GPU.MotionBlur at High (12 samples) with the camera turning 16 pixels a frame, and still: the tile passes and the
		// gather's copy of every pixel (provisional: the reference machine sets the budgets).
		MeasurePostStage4K("PerfGPU.Renderer.MotionBlur4K", "GPU.MotionBlur", true, 16.0f);
		MeasurePostStage4K("PerfGPU.Renderer.MotionBlur4KStill", "GPU.MotionBlur", true, 0.0f);
	}

	TEST_CASE("Morphological anti-aliasing's cost at 3840x2160")
	{
		// GPU.Post with morphological anti-aliasing instead of TAA: the tone mapping into the 10-bit image, the edges, the blend
		// weights and the blend, beside Post4K's TAA (provisional: the reference machine sets the budget).
		MeasurePostStage4K("PerfGPU.Renderer.Morphological4K", "GPU.Post", false, 0.0f, std::nullopt, AntiAliasingMode::Morphological);
	}

	TEST_CASE("Depth of field's cost at 3840x2160")
	{
		// GPU.DepthOfField at its worst: a 100 mm sensor's f/1.4 lens of the grid view focused 2 m away, so the whole grid, hundreds
		// of meters off, blurs at the largest circle (2% of the height) and every pixel gathers (provisional: the reference
		// machine sets the budget).
		CameraLens lens;
		lens.SensorHeight = 100.0f;
		lens.FocalLength = 0.5f * lens.SensorHeight / std::tan(glm::radians(30.0f));
		lens.FNumber = 1.4f;
		lens.FocusDistance = 2.0f;
		REQUIRE(lens.GetCircleOfConfusionScale(2160.0f) > RenderData::c_DepthOfFieldMaxDiameter * 2160.0f);
		MeasurePostStage4K("PerfGPU.Renderer.DepthOfField4K", "GPU.DepthOfField", false, 0.0f, lens);
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

	TEST_CASE("Render cost of 100,000 cubes under 16 reflection probes")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		SceneTestAssets assets;
		// Render100kCubes' scene under 16 reflection probes tiling the grid, each with a capture of its own, all converted before
		// the measurement: the forward pass's box tests and lookups in every pixel, against Render100kCubes (provisional: the
		// reference machine sets the budgets).
		constexpr size_t c_Count = 100'000;
		constexpr int c_ProbesPerSide = 4;
		Scene scene("ProbedCubes");
		Perf::AddCameraAndLight(scene);
		Perf::CreateCubeGrid(scene, c_Count);
		const float side = static_cast<float>(std::ceil(std::sqrt(static_cast<double>(c_Count)))) * Perf::c_GridSpacing;
		const float cell = side / static_cast<float>(c_ProbesPerSide);
		for (int z = 0; z < c_ProbesPerSide; z++)
		{
			for (int x = 0; x < c_ProbesPerSide; x++)
			{
				const float shade = 0.2f + 0.05f * static_cast<float>(z * c_ProbesPerSide + x);
				Entity probe = scene.CreateEntity("Probe");
				probe.GetComponent<TransformComponent>().Translation = glm::vec3((static_cast<float>(x) + 0.5f) * cell - side * 0.5f, 0.0f,
					(static_cast<float>(z) + 0.5f) * cell - side * 0.5f);
				ReflectionProbeComponent& component = probe.AddComponent<ReflectionProbeComponent>();
				component.Capture = assets.Add(CreateEnvironmentTexture(glm::vec3(shade, shade, 1.0f), glm::vec3(0.1f)), "Capture");
				component.Extents = glm::vec3(cell * 0.6f, 20.0f, cell * 0.6f);
				component.BlendDistance = cell * 0.1f;
			}
		}
		SceneRenderer renderer(Perf::MeasuredRendererSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		const SceneCamera camera = GridView(c_Count);
		for (int frame = 0; frame < 32 && renderer.GetStats().ReflectionProbes < c_ProbesPerSide * c_ProbesPerSide; frame++)
			REQUIRE(renderer.Render(scene, camera));
		REQUIRE(renderer.GetStats().ReflectionProbes == c_ProbesPerSide * c_ProbesPerSide);
		const bool measuresGPU = Perf::MeasuresGPU();
		std::vector<std::string> keys = { "PerfGPU.Renderer.ReflectionProbes16" };
		if (measuresGPU)
			keys.push_back("PerfGPU.Renderer.ReflectionProbes16GPU");
		Perf::CheckBudgetsConfirmed(keys, [&]
		{
			const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, camera, "100k cubes under 16 reflection probes");
			CHECK(GetDrawnCounts(renderer).Instances == c_Count);
			CHECK(renderer.GetStats().ReflectionProbes == c_ProbesPerSide * c_ProbesPerSide);
			CHECK(renderer.GetStats().ReflectionProbeConversions == 0);
			Perf::MetricValues values { { "PerfGPU.Renderer.ReflectionProbes16", measurement.CPU.MedianMs } };
			if (measuresGPU)
				values.emplace("PerfGPU.Renderer.ReflectionProbes16GPU", Perf::MeasureGPU(measurement));
			return values;
		});
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("GPU time of the frames that convert six reflection probes, at 128 and 256 texels per face")
	{
		// Six captures of the bakes' size (1024x512, with their levels) in view, converted with SpreadLightingUpdates (two a frame)
		// and screen-space reflections off, so GPU.Reflections holds only the conversions: its largest frame, at Medium's 128 and
		// High's 256 texels per face (RendererQualitySettings::ReflectionProbeResolution; provisional).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		SceneTestAssets assets;
		constexpr int c_Probes = 6;
		const auto createCapture = [](float shade)
		{
			constexpr uint32_t c_CaptureWidth = 1024;
			constexpr uint32_t c_CaptureHeight = 512;
			TextureMip level0;
			level0.Width = c_CaptureWidth;
			level0.Height = c_CaptureHeight;
			level0.Data.resize(static_cast<size_t>(c_CaptureWidth) * c_CaptureHeight * 8);
			for (uint32_t y = 0; y < c_CaptureHeight; y++)
			{
				for (uint32_t x = 0; x < c_CaptureWidth; x++)
				{
					const float value = shade * (0.5f + 0.5f * static_cast<float>((x / 32 + y / 32) % 2));
					const uint16_t texel[4] = { glm::packHalf1x16(value), glm::packHalf1x16(value), glm::packHalf1x16(1.0f), glm::packHalf1x16(1.0f) };
					std::memcpy(level0.Data.data() + (static_cast<size_t>(y) * c_CaptureWidth + x) * sizeof(texel), texel, sizeof(texel));
				}
			}
			std::vector<TextureMip> mips = { level0 };
			REQUIRE(TextureUtils::GenerateMips(mips, TextureFormat::RGBA16F, false));
			TextureSpecification specification;
			specification.Format = TextureFormat::RGBA16F;
			specification.Wrap = TextureWrap::Clamp;
			return Texture::Create(specification, std::move(mips));
		};
		Scene scene("ProbeConversions");
		Perf::AddCameraAndLight(scene);
		for (int index = 0; index < c_Probes; index++)
		{
			Entity probe = scene.CreateEntity("Probe");
			probe.GetComponent<TransformComponent>().Translation = glm::vec3(static_cast<float>(index) * 3.0f - 7.5f, 0.0f, 0.0f);
			ReflectionProbeComponent& component = probe.AddComponent<ReflectionProbeComponent>();
			component.Capture = assets.Add(createCapture(0.2f + 0.1f * static_cast<float>(index)), "Capture");
			component.Extents = glm::vec3(1.4f);
		}
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 2.0f, 14.0f), glm::vec3(0.0f), static_cast<float>(c_Width) / static_cast<float>(c_Height));
		const bool measuresGPU = Perf::MeasuresGPU();
		const auto largestConversionFrame = [&](uint32_t resolution)
		{
			SceneRendererSpecification specification = Perf::MeasuredRendererSpecification();
			specification.Quality.ReflectionProbeResolution = resolution;
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(c_Width, c_Height);
			uint32_t converted = 0;
			Perf::MeasureFrames(gpu, 0, 6, [&]
			{
				REQUIRE(renderer.Render(scene, camera));
				converted += renderer.GetStats().ReflectionProbeConversions;
				CHECK(renderer.GetStats().ReflectionProbeConversions <= SceneRenderer::c_ReflectionProbeConversionsPerFrame);
			});
			CHECK(converted == static_cast<uint32_t>(c_Probes));
			CHECK(renderer.GetStats().ReflectionProbes == static_cast<uint32_t>(c_Probes));
			CHECK_FALSE(renderer.GetStats().ScreenSpaceReflections);
			double largest = 0.0;
			const FrameProfiler& profiler = FrameProfiler::Get();
			for (FrameStageID stage = 0; stage < profiler.GetStageCount(); stage++)
			{
				if (profiler.GetStageName(stage) != "GPU.Reflections")
					continue;
				for (uint32_t age = 0; age < profiler.GetRecordedFrameCount(); age++)
				{
					const FrameSample frame = profiler.GetFrame(age);
					if (frame.HasGPU)
						largest = std::max(largest, static_cast<double>(frame.GPUStageMs[stage]));
				}
			}
			MESSAGE(fmt::format("Probe conversions at {} texels per face: the largest frame's GPU.Reflections {:.3f} ms", resolution, largest));
			return largest;
		};
		if (!measuresGPU)
		{
			MESSAGE("No GPU timing on this device: nothing to measure");
			return;
		}
		Perf::CheckBudgetsConfirmed({ "PerfGPU.Renderer.ProbeConversion128MaxStageGPU", "PerfGPU.Renderer.ProbeConversion256MaxStageGPU" }, [&]
		{
			return Perf::MetricValues { { "PerfGPU.Renderer.ProbeConversion128MaxStageGPU", largestConversionFrame(128) },
				{ "PerfGPU.Renderer.ProbeConversion256MaxStageGPU", largestConversionFrame(256) } };
		});
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Render cost of 100,000 cubes seen through glass")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		SceneTestAssets assets;
		// Render100kCubes' scene through a glass pane over the whole view, near the camera: the scene-color copy and its levels,
		// and every pixel shaded as glass reading it, against Render100kCubes (provisional: the reference machine sets the
		// budgets).
		constexpr size_t c_Count = 100'000;
		Scene scene("GlassCubes");
		Perf::AddCameraAndLight(scene);
		Perf::CreateCubeGrid(scene, c_Count);
		const SceneCamera camera = GridView(c_Count);
		MaterialProperties glass;
		glass.BaseColor = glm::vec4(0.9f, 0.95f, 1.0f, 1.0f);
		glass.Roughness = 0.1f;
		glass.Transmission = 1.0f;
		glass.Thickness = 0.05f;
		glass.DoubleSided = true;
		{
			// A tenth of the way to the grid's center, facing the camera, larger than the view there.
			const float distance = glm::length(camera.Position) * 0.1f;
			const glm::vec3 forward = -glm::normalize(camera.Position);
			Entity pane = scene.CreateEntity("Pane");
			TransformComponent& transform = pane.GetComponent<TransformComponent>();
			transform.Translation = camera.Position + forward * distance;
			transform.Rotation = Math::LookRotation(forward);
			transform.Scale = glm::vec3(distance * 4.0f);
			MeshRendererComponent& meshRenderer = pane.AddComponent<MeshRendererComponent>();
			meshRenderer.Mesh = BuiltinAssets::QuadMesh;
			meshRenderer.Material = assets.AddMaterial(glass);
			meshRenderer.CastShadows = false;
		}
		SceneRenderer renderer(Perf::MeasuredRendererSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		const bool measuresGPU = Perf::MeasuresGPU();
		std::vector<std::string> keys = { "PerfGPU.Renderer.Transmission" };
		if (measuresGPU)
			keys.push_back("PerfGPU.Renderer.TransmissionGPU");
		Perf::CheckBudgetsConfirmed(keys, [&]
		{
			const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, camera, "100k cubes through glass");
			CHECK(GetDrawnCounts(renderer).Instances == c_Count + 1);
			CHECK(renderer.GetStats().SceneColorCopies == 1u);
			Perf::MetricValues values { { "PerfGPU.Renderer.Transmission", measurement.CPU.MedianMs } };
			if (measuresGPU)
				values.emplace("PerfGPU.Renderer.TransmissionGPU", Perf::MeasureGPU(measurement));
			return values;
		});
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Render cost of 100,000 cubes under the atmosphere, with a still sun and with a moving one over 256 and 512 prefiltered skies")
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

		// The moving sun again with High's prefiltered sky (RendererQualitySettings::EnvironmentResolution 512, the other knobs
		// Medium's so the environment's refresh is all that differs): its costliest step filters a 512 level.
		SceneRendererSpecification fineSky = Perf::MeasuredRendererSpecification();
		fineSky.Quality.EnvironmentResolution = 512;
		SceneRenderer fineRenderer(fineSky);
		fineRenderer.SetViewportSize(c_Width, c_Height);
		std::vector<std::string> fineKeys = { "PerfGPU.Renderer.AtmosphereSunMovingMaxFrameHigh" };
		if (measuresGPU)
			fineKeys.push_back("PerfGPU.Renderer.AtmosphereSunMovingMaxFrameHighGPU");
		Perf::CheckBudgetsConfirmed(fineKeys, [&]
		{
			const SceneCamera camera = GridView(c_Count);
			float angle = -50.0f;
			uint32_t updates = 0;
			const Perf::FrameMeasurement moving = Perf::MeasureFrames(gpu, c_WarmupFrames, c_MeasuredFrames, [&]
			{
				angle -= 0.1f;
				sun.GetComponent<TransformComponent>().Rotation = glm::angleAxis(glm::radians(angle), glm::vec3(1.0f, 0.0f, 0.0f));
				sun.MarkModified<TransformComponent>();
				REQUIRE(fineRenderer.Render(scene, camera));
				updates += fineRenderer.GetStats().EnvironmentUpdates;
			});
			MESSAGE("100k cubes under the atmosphere with the sun moving, a 512 prefiltered sky: ", moving.CPU.MaxMs, " ms slowest frame, GPU ",
				moving.GPU ? moving.GPU->MaxMs : 0.0, " ms slowest busy; ", updates, " new environments");
			CHECK(updates > 0);
			CHECK(fineRenderer.GetStats().Quality.EnvironmentResolution == 512u);
			Perf::MetricValues values { { "PerfGPU.Renderer.AtmosphereSunMovingMaxFrameHigh", moving.CPU.MaxMs } };
			if (measuresGPU)
			{
				Perf::MeasureGPU(moving); // Checks that its frames' times arrived
				values.emplace("PerfGPU.Renderer.AtmosphereSunMovingMaxFrameHighGPU", moving.GPU->MaxMs);
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

	TEST_CASE("Render cost of 100,000 cubes whose shadows cover a 4K view, at Medium and at High")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		SceneTestAssets assets;

		// Render100kCubesShadowed's scene at 3840x2160 with every cascade drawn each frame (no cascade cache): Medium draws
		// 2048-texel cascades (0.6.0's), High 4096 from 1440 rows. Medium is the reference High's GPU time is held to.
		constexpr size_t c_Count = 100'000;
		constexpr uint32_t c_Width4K = 3840;
		constexpr uint32_t c_Height4K = 2160;
		const float halfExtent = static_cast<float>(std::ceil(std::sqrt(static_cast<double>(c_Count)))) * Perf::c_GridSpacing * 0.5f;
		Scene scene("ShadowedCubes4K");
		Perf::AddCameraAndLight(scene, 4.0f * halfExtent);
		Perf::CreateCubeGrid(scene, c_Count);
		const bool measuresGPU = Perf::MeasuresGPU();
		std::array<double, 2> redrawMs = {}; // GPU.Shadows at Medium and High
		for (const RendererQualityPreset preset : { RendererQualityPreset::Medium, RendererQualityPreset::High })
		{
			CAPTURE(RendererQuality::PresetToString(preset));
			SceneRendererSpecification specification = Perf::MeasuredRendererSpecification();
			specification.CacheShadowCascades = false;
			specification.Quality = RendererQuality::GetPreset(preset);
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(c_Width4K, c_Height4K);
			CHECK(renderer.GetShadowMapSize() == (preset == RendererQualityPreset::High ? 4096u : 2048u));
			const bool high = preset == RendererQualityPreset::High;
			const std::string key = high ? "PerfGPU.Renderer.Render100kCubesShadowedHigh4K" : "PerfGPU.Renderer.Render100kCubesShadowed4K";
			// The render cost of the sun's shadows: GPU.ScreenShadows (the mask, its blur and the cascades' fit to the view; no
			// preset draws contact shadows) and GPU.Shadows with every cascade drawn and its depth ranges built.
			const std::string screenKey = high ? "PerfGPU.Renderer.ScreenShadows4KHigh" : "PerfGPU.Renderer.ScreenShadows4KMedium";
			const std::string redrawKey = high ? "PerfGPU.Renderer.ShadowRedraw4KHigh" : "PerfGPU.Renderer.ShadowRedraw4KMedium";
			std::vector<std::string> keys = { key };
			if (measuresGPU)
				keys.insert(keys.end(), { key + "GPU", screenKey, redrawKey });
			Perf::CheckBudgetsConfirmed(keys, [&]
			{
				const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, GridView(c_Count), key);
				CHECK(GetDrawnCounts(renderer).Instances == c_Count);
				CHECK(renderer.GetStats().CachedCascadeMask == 0u);
				CHECK(renderer.GetStats().ScreenShadows);
				Perf::MetricValues values { { key, measurement.CPU.MedianMs } };
				if (measuresGPU)
				{
					values.emplace(key + "GPU", Perf::MeasureGPU(measurement));
					values.emplace(screenKey, MedianGPUStageMs("GPU.ScreenShadows"));
					values.emplace(redrawKey, MedianGPUStageMs("GPU.Shadows"));
				}
				return values;
			});
			if (measuresGPU)
				redrawMs[high ? 1 : 0] = MedianGPUStageMs("GPU.Shadows");
		}
		// High draws four times Medium's texels with the same casters: its redraw should cost at most about 3.5 times as much.
		if (measuresGPU && redrawMs[0] > 0.0)
			MESSAGE("GPU.Shadows High / Medium: ", redrawMs[1] / redrawMs[0]);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Ambient occlusion's cost on 100,000 cubes in a 4K view, at Medium and at High")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		SceneTestAssets assets;

		// Render100kCubesShadowed4K's scene without a Post Process, so ambient occlusion runs with the component's defaults
		// (its 1 m radius): Medium's 2 x 6 samples per side (0.6.0's), High's 3 x 8. GPU.AO's median is the stage's cost;
		// High's is held to its 1.8 ms target (provisional until the reference machine measures it), Medium's is recorded for
		// the campaign to compare with 0.6.0's.
		constexpr size_t c_Count = 100'000;
		constexpr uint32_t c_Width4K = 3840;
		constexpr uint32_t c_Height4K = 2160;
		const float halfExtent = static_cast<float>(std::ceil(std::sqrt(static_cast<double>(c_Count)))) * Perf::c_GridSpacing * 0.5f;
		Scene scene("AmbientOcclusionCubes4K");
		Perf::AddCameraAndLight(scene, 4.0f * halfExtent);
		Perf::CreateCubeGrid(scene, c_Count);
		const bool measuresGPU = Perf::MeasuresGPU();
		for (const RendererQualityPreset preset : { RendererQualityPreset::Medium, RendererQualityPreset::High })
		{
			CAPTURE(RendererQuality::PresetToString(preset));
			SceneRendererSpecification specification = Perf::MeasuredRendererSpecification();
			specification.Quality = RendererQuality::GetPreset(preset);
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(c_Width4K, c_Height4K);
			const AmbientOcclusionSamples samples = RendererQuality::GetAmbientOcclusionSamples(specification.Quality.AmbientOcclusionQuality);
			const bool high = preset == RendererQualityPreset::High;
			std::vector<std::string> keys;
			if (high)
				keys.push_back("PerfGPU.Renderer.AOHigh4K");
			if (measuresGPU)
			{
				if (high)
				{
					keys.push_back("PerfGPU.Renderer.AOHigh4KGPU");
					keys.push_back("PerfGPU.Renderer.AOHigh4KStageGPU");
				}
				else
				{
					keys.push_back("PerfGPU.Renderer.AOMedium4KStageGPU");
				}
			}
			if (keys.empty())
				continue; // Medium without timestamps: nothing of it is recorded
			Perf::CheckBudgetsConfirmed(keys, [&]
			{
				const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, GridView(c_Count), high ? "AO High 4K" : "AO Medium 4K");
				CHECK(GetDrawnCounts(renderer).Instances == c_Count);
				CHECK(renderer.GetStats().AmbientOcclusionSamples == samples.Slices * samples.StepsPerSide * 2);
				Perf::MetricValues values;
				if (high)
					values.emplace("PerfGPU.Renderer.AOHigh4K", measurement.CPU.MedianMs);
				if (measuresGPU)
				{
					const double busy = Perf::MeasureGPU(measurement);
					const double stage = MedianGPUStageMs("GPU.AO");
					CHECK(stage > 0.0);
					if (high)
					{
						values.emplace("PerfGPU.Renderer.AOHigh4KGPU", busy);
						values.emplace("PerfGPU.Renderer.AOHigh4KStageGPU", stage);
					}
					else
					{
						values.emplace("PerfGPU.Renderer.AOMedium4KStageGPU", stage);
					}
				}
				return values;
			});
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Aerial perspective's cost on 100,000 cubes under the atmosphere in a 4K view")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		SceneTestAssets assets;

		// Render100kCubesShadowed4K's scene under the atmosphere, whose Sky Light draws aerial perspective by default: the
		// frame's volume (GPU.Aerial, target 0.05 ms) and its lookup in the forward pass (GPU.Opaque).
		constexpr size_t c_Count = 100'000;
		constexpr uint32_t c_Width4K = 3840;
		constexpr uint32_t c_Height4K = 2160;
		const float halfExtent = static_cast<float>(std::ceil(std::sqrt(static_cast<double>(c_Count)))) * Perf::c_GridSpacing * 0.5f;
		Scene scene("AerialCubes4K");
		Perf::AddCameraAndLight(scene, 4.0f * halfExtent);
		Perf::CreateCubeGrid(scene, c_Count);
		scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().Source = SkyLightSource::Atmosphere;
		for (auto [entity, light] : scene.GetRegistry().view<DirectionalLightComponent>().each())
		{
			light.Intensity = 100000.0f;
			Entity(entity, &scene).MarkModified<DirectionalLightComponent>();
		}
		SceneRenderer renderer(Perf::MeasuredRendererSpecification());
		renderer.SetViewportSize(c_Width4K, c_Height4K);
		const bool measuresGPU = Perf::MeasuresGPU();
		std::vector<std::string> keys = { "PerfGPU.Renderer.Aerial4K" };
		if (measuresGPU)
		{
			keys.push_back("PerfGPU.Renderer.Aerial4KGPU");
			keys.push_back("PerfGPU.Renderer.Aerial4KStageGPU");
		}
		Perf::CheckBudgetsConfirmed(keys, [&]
		{
			const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, GridView(c_Count), "Aerial perspective 4K");
			CHECK(GetDrawnCounts(renderer).Instances == c_Count);
			CHECK(renderer.GetStats().Atmosphere);
			CHECK(renderer.GetStats().AerialPerspective);
			Perf::MetricValues values { { "PerfGPU.Renderer.Aerial4K", measurement.CPU.MedianMs } };
			if (measuresGPU)
			{
				values.emplace("PerfGPU.Renderer.Aerial4KGPU", Perf::MeasureGPU(measurement));
				const double stage = MedianGPUStageMs("GPU.Aerial");
				CHECK(stage > 0.0);
				values.emplace("PerfGPU.Renderer.Aerial4KStageGPU", stage);
			}
			return values;
		});
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Spatial screen-space reflections' cost on 100,000 cubes in a 4K view")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		SceneTestAssets assets;

		// Render100kCubesShadowed4K's scene with screen-space reflections from every surface (the maximum roughness 1) at
		// RendererQualitySettings::Reflections Spatial and SpatialHigh, with FXAA: the full-resolution trace and the resolve
		// (GPU.Reflections, targets 1.2 and 1.8 ms: SpatialHigh marches 48 steps, bisects 8 times and refines up to three
		// candidates a ray) and the color copy the next frame reads (in GPU.Transparent). Clutter is where rejected
		// candidates cost.
		const auto run = [&](ReflectionsQuality reflections, const std::string& name)
		{
			constexpr size_t c_Count = 100'000;
			constexpr uint32_t c_Width4K = 3840;
			constexpr uint32_t c_Height4K = 2160;
			const float halfExtent = static_cast<float>(std::ceil(std::sqrt(static_cast<double>(c_Count)))) * Perf::c_GridSpacing * 0.5f;
			Scene scene("SpatialReflectionCubes4K");
			Perf::AddCameraAndLight(scene, 4.0f * halfExtent);
			Perf::CreateCubeGrid(scene, c_Count);
			PostProcessComponent& postProcess = scene.CreateEntity("Post Process").AddComponent<PostProcessComponent>();
			postProcess.AntiAliasingMethod = AntiAliasingMode::FXAA;
			postProcess.ScreenSpaceReflections = true;
			postProcess.ScreenSpaceReflectionsMaxRoughness = 1.0f;
			SceneRendererSpecification specification = Perf::MeasuredRendererSpecification();
			specification.Quality.Reflections = reflections;
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(c_Width4K, c_Height4K);
			const bool measuresGPU = Perf::MeasuresGPU();
			const std::string key = "PerfGPU.Renderer." + name + "4K";
			std::vector<std::string> keys = { key };
			if (measuresGPU)
			{
				keys.push_back(key + "GPU");
				keys.push_back(key + "StageGPU");
			}
			Perf::CheckBudgetsConfirmed(keys, [&]
			{
				const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, GridView(c_Count), name + " reflections 4K");
				CHECK(GetDrawnCounts(renderer).Instances == c_Count);
				CHECK(renderer.GetStats().ScreenSpaceReflections);
				CHECK_FALSE(renderer.GetStats().TemporalAntiAliasing);
				Perf::MetricValues values { { key, measurement.CPU.MedianMs } };
				if (measuresGPU)
				{
					values.emplace(key + "GPU", Perf::MeasureGPU(measurement));
					const double stage = MedianGPUStageMs("GPU.Reflections");
					CHECK(stage > 0.0);
					values.emplace(key + "StageGPU", stage);
				}
				return values;
			});
		};
		run(ReflectionsQuality::Spatial, "SSRSpatial");
		run(ReflectionsQuality::SpatialHigh, "SSRSpatialHigh");
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
