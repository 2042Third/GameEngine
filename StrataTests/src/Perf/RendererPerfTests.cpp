#include "Renderer/SceneRendererTestUtils.h"

#include "Perf/PerfUtils.h"
#include "Perf/RendererPerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Renderer/TextureStreamingTestUtils.h"

#include <Strata/Asset/RuntimeAssetManager.h>
#include <Strata/Renderer/Material.h>
#include <Strata/Renderer/TextureStreamer.h>

#include <spdlog/fmt/fmt.h>

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
		MESSAGE(name, ": ", measurement.CPU.MedianMs, " ms median, GPU ", measurement.GPU ? measurement.GPU->MedianMs : 0.0, " ms busy (", stats.Instances, " instances, ",
			stats.DrawCommands, " draw commands, ", stats.Triangles, " triangles, extract ", stats.ExtractMs, " ms, sort ", stats.SortMs, " ms; GPU timer ",
			measurement.TimerCostMs, " ms per frame)");
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
			// The median frame (every attempt renders its own warm-up and measured frames); for 100,000 cubes also the GPU's
			// median busy time and what timing the GPU costs the CPU per frame (GPUProfiler's own time: writes, resolves).
			if (count == 100'000)
			{
				Perf::CheckBudgetsConfirmed({ "PerfGPU.Renderer.Render100kCubes", "PerfGPU.Renderer.Render100kCubesGPU", "PerfGPU.Renderer.GPUTimerCost" }, [&]
				{
					const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, GridView(count), "100k cubes");
					CHECK(renderer.GetStats().Instances == count);
					CHECK(renderer.GetStats().InstanceBytesUploaded == 0); // A still frame uploads nothing
					return Perf::MetricValues { { "PerfGPU.Renderer.Render100kCubes", measurement.CPU.MedianMs },
						{ "PerfGPU.Renderer.Render100kCubesGPU", Perf::MeasureGPU(measurement) }, { "PerfGPU.Renderer.GPUTimerCost", measurement.TimerCostMs } };
				});
				continue;
			}
			Perf::CheckBudgetConfirmed("PerfGPU.Renderer.Render300kCubes", [&]
			{
				const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, GridView(count), "300k cubes");
				CHECK(renderer.GetStats().Instances == count);
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
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Width, c_Height);
		// The median frame, and the opaque draw calls of the last one: batching is material-free, so the 10,000 materials
		// share the draws of the cube's pipeline state (a count, the same in every frame).
		Perf::CheckBudgetsConfirmed({ "PerfGPU.Renderer.Render10kMaterials", "PerfGPU.Renderer.Render10kMaterials.OpaqueDrawCalls" }, [&]
		{
			const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, GridView(materials.size()), "10k materials");
			CHECK(renderer.GetStats().Materials == materials.size());
			return Perf::MetricValues { { "PerfGPU.Renderer.Render10kMaterials", measurement.CPU.MedianMs },
				{ "PerfGPU.Renderer.Render10kMaterials.OpaqueDrawCalls", static_cast<double>(renderer.GetStats().OpaqueDrawCalls) } };
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
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Width, c_Height);
		// The median Render (extraction, recording and submitting) and the draws the CPU recorded: the meshes share the
		// geometry pool's buffers, so draws follow pipeline states (buckets), not meshes.
		Perf::CheckBudgetsConfirmed({ "PerfGPU.Renderer.Render10kUniqueMeshes", "PerfGPU.Renderer.Render10kUniqueMeshes.DrawCommands" }, [&]
		{
			const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, GridView(c_Count), "10k unique meshes");
			const SceneRendererStats& stats = renderer.GetStats();
			CHECK(stats.Instances == c_Count);
			MESSAGE("10k unique meshes: ", stats.OpaqueDrawCalls, " batches, ", stats.ShadowDrawCalls, " shadow batches, ", stats.DrawCommands, " draw commands");
			return Perf::MetricValues { { "PerfGPU.Renderer.Render10kUniqueMeshes", measurement.CPU.MedianMs },
				{ "PerfGPU.Renderer.Render10kUniqueMeshes.DrawCommands", static_cast<double>(stats.DrawCommands) } };
		});
		CHECK(gpu.GetNewErrorCount() == 0);
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
			SceneRenderer renderer;
			renderer.SetViewportSize(c_Width, c_Height);
			const char* key = count == 100'000 ? "PerfGPU.Renderer.Render100kCubesShadowed" : "PerfGPU.Renderer.Render300kCubesShadowed";
			Perf::CheckBudgetConfirmed(key, [&]
			{
				const Perf::FrameMeasurement measurement = MeasureRender(gpu, renderer, scene, GridView(count), count == 100'000 ? "100k shadowed cubes"
					: "300k shadowed cubes");
				const SceneRendererStats& stats = renderer.GetStats();
				CHECK(stats.Instances == count);
				// Every cube is a caster: drawn into the cascades where it spans a texel, left out of those where it does not (the
				// far cascades' texels are wider than a cube: SubTexelCasters). Both are summed over the cascades.
				CHECK(stats.ShadowCasters + stats.SubTexelCasters >= count * 9 / 10);
				CHECK(stats.ShadowCasters >= count / 10);
				MESSAGE("shadow casters ", stats.ShadowCasters, ", left out below a texel ", stats.SubTexelCasters, ", shadow draws ", stats.ShadowDrawCalls);
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
		SceneRenderer renderer;
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
