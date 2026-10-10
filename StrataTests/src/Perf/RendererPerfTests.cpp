#include "Renderer/SceneRendererTestUtils.h"

#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"

#include <chrono>
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

	// The median CPU time of SceneRenderer::Render, frames run like an application's.
	Perf::Measurement MeasureRender(GPUContext& gpu, SceneRenderer& renderer, Scene& scene, const SceneCamera& camera, std::string_view name)
	{
		std::vector<double> samples;
		for (int frame = 0; frame < c_WarmupFrames + c_MeasuredFrames; frame++)
		{
			REQUIRE(gpu.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			const auto start = std::chrono::steady_clock::now();
			REQUIRE(renderer.Render(scene, camera));
			const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			gpu.GetDevice().EndFrame();
			if (frame >= c_WarmupFrames)
				samples.push_back(milliseconds);
		}
		const Perf::Measurement measurement = Perf::Summarize(samples);
		const SceneRendererStats& stats = renderer.GetStats();
		MESSAGE(name, ": ", measurement.MedianMs, " ms median (", stats.Instances, " instances, ", stats.DrawCalls, " draw calls, extract ", stats.ExtractMs, " ms, sort ",
			stats.SortMs, " ms)");
		return measurement;
	}

}

TEST_SUITE("PerfGPU.Renderer")
{
	TEST_CASE("Render cost of 100,000 and 300,000 cubes and of 10,000 cubes with unique materials")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		SceneTestAssets assets;

		for (const size_t count : { size_t(100'000), size_t(300'000) })
		{
			CAPTURE(count);
			Scene scene("Cubes");
			Perf::AddCameraAndLight(scene);
			Perf::CreateCubeGrid(scene, count);
			SceneRenderer renderer;
			renderer.SetViewportSize(c_Width, c_Height);
			const Perf::Measurement measurement = MeasureRender(gpu, renderer, scene, GridView(count), count == 100'000 ? "100k cubes" : "300k cubes");
			CHECK(renderer.GetStats().Instances == count);
			CHECK(renderer.GetStats().InstanceBytesUploaded == 0); // A still frame uploads nothing
			Perf::CheckBudget(count == 100'000 ? "PerfGPU.Renderer.Render100kCubes" : "PerfGPU.Renderer.Render300kCubes", measurement.MedianMs);
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
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Width, c_Height);
		const Perf::Measurement measurement = MeasureRender(gpu, renderer, scene, GridView(materials.size()), "10k materials");
		CHECK(renderer.GetStats().Materials == materials.size());
		CHECK(renderer.GetStats().OpaqueDrawCalls <= 4);
		Perf::CheckBudget("PerfGPU.Renderer.Render10kMaterials", measurement.MedianMs);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
