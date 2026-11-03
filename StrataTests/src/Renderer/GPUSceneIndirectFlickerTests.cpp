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
