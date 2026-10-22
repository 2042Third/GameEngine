#include "Renderer/SceneRendererTestUtils.h"

#include "Strata/Renderer/RenderScene.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// Temporal anti-aliasing (PostProcessComponent::AntiAliasingMethod TAA): a sub-pixel jitter in the frame's copy of the camera,
// blended with the renderer's own history of earlier frames along the motion vectors.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = 64;

	const char* PathName(GPUDrivenMode mode)
	{
		return mode == GPUDrivenMode::Off ? "CPU path" : "GPU-driven path";
	}

	SceneRenderer CreateRenderer(const char* name, GPUDrivenMode mode = GPUDrivenMode::Off, bool temporal = true)
	{
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		specification.GPUDriven = mode;
		specification.TemporalAntiAliasing = temporal;
		specification.DebugName = name;
		return SceneRenderer(specification);
	}

	// The neutral post-processing (exact colors) with temporal anti-aliasing.
	PostProcessComponent& AddTemporalPostProcess(Scene& scene)
	{
		PostProcessComponent& postProcess = AddNeutralPostProcess(scene);
		postProcess.AntiAliasing = true;
		postProcess.AntiAliasingMethod = AntiAliasingMode::TAA;
		return postProcess;
	}

	// Rows of boxes on a ground under a shadowed sun, seen from above at an angle.
	struct TemporalScene
	{
		Scene Contents;
		std::vector<Entity> Boxes;
		Entity Ground;
		SceneCamera Camera;

		TemporalScene()
		{
			AddTemporalPostProcess(Contents);
			AddSun(Contents, glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f)), 3.0f);
			for (uint32_t index = 0; index < 12; index++)
				Boxes.push_back(AddMesh(Contents, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(static_cast<float>(index % 4) * 2.0f - 3.0f, 0.0f,
					-static_cast<float>(index / 4) * 2.5f), "Box"));
			Ground = AddMesh(Contents, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f, -0.5f, 0.0f), "Ground");
			Ground.GetComponent<TransformComponent>().Scale = glm::vec3(40.0f);
			Camera = LookAt(glm::vec3(0.0f, 9.0f, 8.0f), glm::vec3(0.0f, 0.0f, -2.5f));
		}
	};

	void LoadAssets(SceneTestAssets& assets)
	{
		for (const AssetHandle builtin : { BuiltinAssets::CubeMesh, BuiltinAssets::PlaneMesh, BuiltinAssets::DefaultMaterial })
			REQUIRE(assets.Manager->LoadAssetSync(builtin));
	}

	// What an application's device frame does after its work (GraphicsDevice::EndFrame): the GPU is done with it, and NVRHI
	// learns so (the frame constants' versions are used again). Tests that render many frames without reading anything back
	// need it.
	void FinishFrame(GPUContext& gpu)
	{
		gpu.GetDevice().WaitForIdle();
		gpu.GetNvrhiDevice()->runGarbageCollection();
	}

	// The temporal statistics of a frame that started the history again: the first frame, jitter phase 0.
	void CheckReset(const SceneRenderer& renderer, uint32_t resets)
	{
		const SceneRendererStats& stats = renderer.GetStats();
		CHECK(stats.TemporalAntiAliasing);
		CHECK(stats.TemporalFrames == 1);
		CHECK(stats.HistoryResets == resets);
		CHECK(stats.Jitter == SceneRenderer::GetTemporalJitter(0));
	}

}

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
			TemporalScene scene;
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
		TemporalScene scene;
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
		TemporalScene other;
		render(other.Contents, 1);
		CheckReset(renderer, 4);
		render(scene.Contents, 1);
		CheckReset(renderer, 5);
		Ref<Scene> source = CreateRef<Scene>("Source");
		AddTemporalPostProcess(*source);
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
		PostProcessComponent& postProcess = scene.Contents.GetRegistry().get<PostProcessComponent>(scene.Contents.GetRegistry().view<PostProcessComponent>().front());
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
}
