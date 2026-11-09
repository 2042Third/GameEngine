#include "Renderer/FlickerMetrics.h"
#include "Renderer/GIScenes.h"
#include "Renderer/SceneRendererTestUtils.h"

#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Width = 1280;
	constexpr uint32_t c_Height = 720;
	constexpr uint32_t c_Frames = 12;
	constexpr float c_Aspect = static_cast<float>(c_Width) / static_cast<float>(c_Height);
	// The gi view's strafe bounds (Gate W, correction 9): 1.5 times the measured no-AA values, never above the spec's guesses
	// (fence Blocks 0.10%, BlocksP99 1.0%, grass Blocks 0.2%). Measured at ee97deb9: fence 0.079% and 0.59%, grass 0.13%; with
	// TAA 0.072% and 0.40%, 0.10%. At ac289ad9 the fence read 0.247% and 7.2%, the grass 0.217%.
	constexpr double c_GIFenceBlocks = 0.0010;
	constexpr double c_GIFenceBlocksP99 = 0.0088;
	constexpr double c_GIGrassBlocks = 0.0019;

	using CameraPath = std::function<SceneCamera(uint32_t frame)>;

	// Renders c_Frames frames of the path with the indirect view (HDR output, pre-exposure 1) through a fresh renderer and
	// measures their flicker from frame 3 on (frame 0 is a cut, frames 1 and 2 settle the first triple).
	FlickerResult MeasurePath(Scene& scene, const CameraPath& path, const SceneRendererSpecification& specification = ExactSpecification(),
		const std::function<void(const FlickerFrame&)>& onFrame = {})
	{
		Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(specification);
		renderer->SetViewportSize(c_Width, c_Height);
		SceneRenderOptions options;
		options.HDROutput = true;
		options.PreExposure = 1.0f;
		options.LightingView = SceneLightingView::Indirect;
		options.AdaptationSeconds = 1.0f / 60.0f;
		FlickerSequence sequence(3);
		for (uint32_t frame = 0; frame < c_Frames; frame++)
		{
			const SceneCamera camera = path(frame);
			REQUIRE(renderer->Render(scene, camera, nullptr, options));
			FlickerFrame measured = ReadFlickerFrame(*renderer, camera);
			if (onFrame)
				onFrame(measured);
			sequence.Add(std::move(measured));
		}
		return sequence.GetResult();
	}

	bool IsFinite(const FlickerResult& result)
	{
		return std::isfinite(result.Mean) && std::isfinite(result.P99) && std::isfinite(result.Blocks) && std::isfinite(result.BlocksP99)
			&& std::isfinite(result.MeasuredShare);
	}

	// The five numbers, in percent as the audit states them.
	void Report(const std::string& name, const FlickerResult& result)
	{
		MESSAGE(name << ": mean " << result.Mean * 100.0 << "%, p99 " << result.P99 * 100.0 << "%, blocks " << result.Blocks * 100.0 << "%, blocks p99 "
					 << result.BlocksP99 * 100.0 << "%, measured " << result.MeasuredShare * 100.0 << "% over " << result.Frames << " triples");
	}

	SceneCamera Fence(uint32_t frame)
	{
		return YardStrafe(frame, 1.0f, c_Aspect);
	}

	void UseTemporalAntiAliasing(Scene& scene)
	{
		PostProcessComponent& postProcess = GetPostProcess(scene);
		postProcess.AntiAliasing = true;
		postProcess.AntiAliasingMethod = AntiAliasingMode::TAA;
		postProcess.TAAPreset = TemporalAntiAliasingPreset::Sharp;
	}

	// The gi view's flicker on a path (6.2's strafes, Gate W's regression form): 14 frames through a fresh renderer with the GI
	// at High and every bounce, triples from frame 3 (the yard's bounces settle within them: 0.079% of the fence's blocks against
	// 0.028% after settling first). The yard builds with AddNeutralPostProcess, so ambient occlusion is off and the meter reads the
	// field alone (correction 9).
	FlickerResult MeasureGlobalIllumination(Scene& scene, const CameraPath& path, bool temporal)
	{
		if (temporal)
			UseTemporalAntiAliasing(scene);
		SceneRendererSpecification specification = ExactSpecification();
		specification.Quality.GlobalIllumination = GlobalIlluminationLevel::High;
		specification.GlobalIllumination.VoxelizeTrianglesOnCut = 0; // The first frame is complete
		Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(specification);
		renderer->SetViewportSize(c_Width, c_Height);
		SceneRenderOptions options;
		options.HDROutput = true;
		options.PreExposure = 1.0f;
		options.LightingView = SceneLightingView::GlobalIllumination;
		options.AdaptationSeconds = 1.0f / 60.0f;
		FlickerSequence sequence(3);
		for (uint32_t frame = 0; frame < 14; frame++)
		{
			const SceneCamera camera = path(frame);
			REQUIRE(RenderGIFrame(*renderer, scene, camera, options));
			REQUIRE(renderer->GetStats().GlobalIllumination);
			sequence.Add(ReadFlickerFrame(*renderer, camera));
		}
		return sequence.GetResult();
	}

}

TEST_SUITE("GPU.SceneRenderer.IndirectFlicker")
{
	TEST_CASE("The measure runs on the yard's fence strafe")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		BuildYard(scene, assets);
		const FlickerResult result = MeasurePath(scene, Fence);
		Report("Fence strafe, indirect view, no AA", result);
		CHECK(IsFinite(result));
		CHECK(result.MeasuredShare > 0.5);
		CHECK(result.Frames == 9);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A still camera's indirect light does not flicker")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		BuildYard(scene, assets);
		// Every frame's image is the first's, bit for bit; what the meter then reads is only its reprojection's rounding (single
		// precision: the bilinear weights of a point that lands within a float's rounding of a texel's center), within the bounds
		// Renderer.FlickerMetrics holds a still plane to.
		std::vector<float> first;
		uint32_t differing = 0;
		const auto compare = [&](const FlickerFrame& frame)
		{
			if (first.empty())
				first = frame.Value;
			else if (frame.Value != first)
				differing++;
		};
		const FlickerResult result = MeasurePath(scene, [](uint32_t) { return YardStrafe(0, 0.0f, c_Aspect); }, ExactSpecification(), compare);
		CHECK(differing == 0);
		CHECK(result.MeasuredShare > 0.5);
		CHECK(result.Blocks < 1e-6);
		CHECK(result.Mean < 1e-5);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The grass and room scenes are measured")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		for (bool temporal : { false, true })
		{
			CAPTURE(temporal);
			const std::string mode = temporal ? "TAA" : "no AA";
			{
				SceneTestAssets assets;
				Scene scene;
				BuildYard(scene, assets, YardOptions { .GrassCards = 500 });
				if (temporal)
					UseTemporalAntiAliasing(scene);
				const FlickerResult result = MeasurePath(scene, Fence);
				Report("Grass strafe, indirect view, " + mode, result);
				CHECK(IsFinite(result));
				CHECK(result.MeasuredShare > 0.3);
			}
			{
				SceneTestAssets assets;
				Scene scene;
				BuildRoom(scene, assets);
				if (temporal)
					UseTemporalAntiAliasing(scene);
				const FlickerResult result = MeasurePath(scene, [](uint32_t frame) { return RoomStrafe(frame, c_Aspect); });
				Report("Room strafe, indirect view, " + mode, result);
				CHECK(IsFinite(result));
				CHECK(result.MeasuredShare > 0.5);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The fence strafe with TAA is measured")
	{
		// With HDR output the frame ends before the resolve: the image measured is the jittered one, whose jitter enters through
		// each frame's view-projection.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		BuildYard(scene, assets);
		UseTemporalAntiAliasing(scene);
		const FlickerResult result = MeasurePath(scene, Fence);
		Report("Fence strafe, indirect view, TAA", result);
		CHECK(IsFinite(result));
		CHECK(result.MeasuredShare > 0.5);
		CHECK(result.Frames == 9);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Today's ambient occlusion on the fence strafe is measured")
	{
		// Without ambient occlusion today's indirect light is deterministic: GTAO's own flicker in motion (High, 3 x 8, with its
		// denoise; multi-bounce off) is the reference gate G1's numbers are read against.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneRendererSpecification specification = ExactSpecification();
		specification.Quality.AmbientOcclusionQuality = AmbientOcclusionLevel::High;
		for (bool temporal : { false, true })
		{
			CAPTURE(temporal);
			SceneTestAssets assets;
			Scene scene;
			BuildYard(scene, assets);
			PostProcessComponent& postProcess = GetPostProcess(scene);
			postProcess.AmbientOcclusion = true;
			postProcess.AmbientOcclusionMultiBounce = false;
			if (temporal)
				UseTemporalAntiAliasing(scene);
			const FlickerResult result = MeasurePath(scene, Fence, specification);
			Report(temporal ? "Fence strafe, indirect view with GTAO High, TAA" : "Fence strafe, indirect view with GTAO High, no AA", result);
			CHECK(IsFinite(result));
			CHECK(result.MeasuredShare > 0.5);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}

TEST_SUITE("GPU.SceneRenderer.GlobalIllumination")
{
	TEST_CASE("The fence and grass strafes on the gi view")
	{
		// 6.2 and D14.7: the field is a function of world data, so a strafe's gi view does not crawl (bounds above). What crawled
		// was the gather's visibility: Chebyshev's bound on the probes' depths, cubed, at a point moved toward the viewer, so the
		// view's turn as the camera strafed changed the posts' probe weights by whole steps; the voxels' guard does not.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		for (bool temporal : { false, true })
		{
			CAPTURE(temporal);
			const std::string mode = temporal ? "TAA" : "no AA";
			{
				SceneTestAssets assets;
				Scene scene;
				BuildYard(scene, assets);
				const FlickerResult fence = MeasureGlobalIllumination(scene, Fence, temporal);
				Report("Fence strafe, gi view, " + mode, fence);
				CHECK(IsFinite(fence));
				CHECK(fence.MeasuredShare > 0.5);
				CHECK(fence.Blocks <= c_GIFenceBlocks);
				CHECK(fence.BlocksP99 <= c_GIFenceBlocksP99);
			}
			{
				SceneTestAssets assets;
				Scene scene;
				YardOptions options;
				options.GrassCards = 500;
				BuildYard(scene, assets, options);
				const FlickerResult grass = MeasureGlobalIllumination(scene, Fence, temporal);
				Report("Grass strafe, gi view, " + mode, grass);
				CHECK(IsFinite(grass));
				CHECK(grass.MeasuredShare > 0.3);
				CHECK(grass.Blocks <= c_GIGrassBlocks);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
