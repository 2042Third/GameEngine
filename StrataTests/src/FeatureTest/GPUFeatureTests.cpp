#include <doctest/doctest.h>

#include "FeatureTest/FeatureTestUtils.h"
#include "Renderer/GPUTestUtils.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Animation/AnimationPreview.h"
#include "Strata/Core/FrameProfiler.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/SceneRenderer.h"
#include "Strata/Scene/Entity.h"

#include <cstdint>
#include <string>
#include <string_view>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Width = 320;
	constexpr uint32_t c_Height = 180;

	// A yellow HUD pixel (the HUD text color is (1, 0.9, 0.2), drawn with exact display colors).
	bool IsHudPixel(const glm::u8vec4& pixel)
	{
		return pixel.r > 200 && pixel.g > 170 && pixel.b < 120;
	}

}

TEST_SUITE("GPU.FeatureTest")
{
	TEST_CASE("The feature scene renders its meshes, lights, sky, text and game UI")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		// Created after the device, so the asset manager uploads meshes and textures to it.
		FeatureProject project;
		LoadAllAssets(*project.GetAssetManager());
		const Ref<Scene> scene = project.LoadStartScene();

		const Entity cameraEntity = scene->GetPrimaryCameraEntity();
		REQUIRE(cameraEntity.IsValid());
		CHECK(cameraEntity.GetName() == "Main Camera");
		const SceneCamera camera = SceneCamera::FromEntity(*scene, cameraEntity, static_cast<float>(c_Width) / static_cast<float>(c_Height));

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Width, c_Height);
		// A few frames, as an application renders them: assets finish uploading and the environment is prepared. The scene's
		// post-processing is temporal anti-aliasing: the still view's history grows with each frame.
		for (uint32_t frame = 0; frame < 4; frame++)
		{
			project.GetAssetManager()->Update();
			REQUIRE(renderer.Render(*scene, camera));
			CHECK(renderer.GetStats().TemporalAntiAliasing);
			CHECK(renderer.GetStats().TemporalFrames == frame + 1);
		}

		const SceneRendererStats& stats = renderer.GetStats();
		CHECK(stats.Rendered);
		CHECK(stats.PendingAssets == 0);
		// The GPU culls the scene and builds its draws wherever the device can (the default).
		CHECK(stats.GPUDriven == gpu.GetDevice().GetInfo().SupportsDrawIndirectFirstInstance);

		// The checker texture (64x64) loads its resident tail first and its finest level streams in as the platform needs it
		// (TextureStreamer); the other textures are no larger than the tail.
		const Ref<AssetManagerBase> assets = project.GetAssetManager();
		for (int32_t frame = 0; frame < 120; frame++)
		{
			const AssetDetailStats detail = assets->GetStats().Detail;
			if (detail.LevelsRaised > 0 && detail.Reads == 0 && detail.Changes == 0)
				break;
			assets->Update();
			REQUIRE(renderer.Render(*scene, camera));
		}
		CHECK(assets->GetStats().PartialLoads >= 1);
		CHECK(assets->GetStats().Detail.LevelsRaised >= 1);
		CHECK(stats.PendingAssets == 0);
		CHECK(stats.EnvironmentLighting); // The sky light's HDR environment map
		CHECK(stats.HeightFog);           // The Fog entity's
		CHECK(stats.ReflectionProbes == 1); // The Probe entity's, its capture converted
		CHECK(stats.ReflectionProbesPending == 0);
		CHECK(GetLightCounts(renderer).Lights >= 2); // The sun, plus the point and spot lights in view (the GPU's count on its path)
		CHECK(GetDrawnCounts(renderer).Instances >= 8);
		CHECK(stats.Texts == 2); // The HUD (with the project's font) and the world-space sign
		CHECK(stats.TextGlyphs > 0);

		// Meshes are where the scene puts them: the entity-ID buffer under points of their surfaces.
		struct VisiblePoint
		{
			std::string_view Entity;
			glm::vec3 Position;
		};
		const VisiblePoint points[] = {
			{ "Ground", { 1.5f, 0.0f, 6.0f } },
			{ "Platform", { -4.0f, 1.25f, 1.5f } },  // The imported mesh (top face)
			{ "Ball", { -4.0f, 4.0f, 0.0f } },       // Built-in sphere with an imported material
			{ "Crate", { 6.0f, 2.0f, 2.0f } },       // Prefab instance
			{ "Pill Visual", { 2.0f, 3.1f, 0.0f } },
			{ "Compound Box Visual", { 5.0f, 3.25f, -2.0f } },
		};
		for (const VisiblePoint& point : points)
		{
			INFO("Entity ", std::string(point.Entity));
			const Entity expected = scene->FindEntityByName(point.Entity);
			REQUIRE(expected.IsValid());
			const glm::uvec2 pixel = ProjectToPixel(camera, point.Position, c_Width, c_Height);
			REQUIRE(pixel.x < c_Width);
			REQUIRE(pixel.y < c_Height);
			CHECK(renderer.GetEntityAt(*scene, pixel.x, pixel.y) == expected);
		}

		// The HUD text is drawn with the project's block font in the top-left corner, and nowhere near the bottom right.
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
		REQUIRE(image.Width == c_Width);
		uint32_t hudPixels = 0;
		for (uint32_t y = 0; y < c_Height / 4; y++)
		{
			for (uint32_t x = 0; x < c_Width / 2; x++)
				hudPixels += IsHudPixel(GetPixelRGBA8(image, x, y)) ? 1 : 0;
		}
		CHECK(hudPixels > 100);
		CHECK_FALSE(IsHudPixel(GetPixelRGBA8(image, c_Width - 2, c_Height - 2)));

		// The menu canvas in the top-right corner (ScaleWithScreen at 1280x720: a quarter of its units here): its panel's dark
		// blue in its left padding, over the scene; without the game UI the scene shows there. The world sign is a canvas in
		// the scene: drawn either way.
		CHECK(stats.Overlays.Canvases == 3);
		CHECK(stats.Overlays.Elements >= 20);
		CHECK(stats.Overlays.PendingGlyphs == 0);
		const glm::u8vec4 panel = GetPixelRGBA8(image, 236, 37);
		CHECK(panel.b > panel.r);
		CHECK(panel.r < 60);
		CHECK(panel.b < 100);
		SceneRenderOptions hidden;
		hidden.DrawScreenSpaceText = false;
		REQUIRE(renderer.Render(*scene, camera, nullptr, hidden));
		CHECK(renderer.GetStats().Overlays.HiddenCanvases == 2);
		CHECK(renderer.GetStats().Overlays.Canvases == 1);
		ReadbackImage withoutUI;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), withoutUI));
		CHECK(GetPixelRGBA8(withoutUI, 236, 37) != panel);

		// The GPU's time per stage of the scene's frames resolves, frames as an application runs them (where the device writes
		// timestamps).
		if (Renderer::GetGPUProfiler().IsEnabled())
		{
			for (int32_t frame = 0; frame < 2; frame++)
			{
				REQUIRE(gpu.GetDevice().BeginFrame());
				Renderer::BeginFrame();
				REQUIRE(renderer.Render(*scene, camera));
				gpu.GetDevice().EndFrame();
			}
			renderer.ResolveGPUStats(true);
			const SceneRendererGPUStats& gpuStats = renderer.GetGPUStats();
			REQUIRE(gpuStats.HasTimes);
			CHECK(gpuStats.BusyMs > 0.0f);
			const RendererGPUStages& stages = RendererGPUStages::Get();
			const auto stageMs = [&gpuStats](FrameStageID stage)
			{
				for (const GPUStageTime& time : gpuStats.StageMs)
				{
					if (time.Stage == stage)
						return time.Milliseconds;
				}
				return -1.0f;
			};
			CHECK(stageMs(stages.Setup) >= 0.0f);
			CHECK(stageMs(stages.Shadows) > 0.0f);
			CHECK(stageMs(stages.Prepass) > 0.0f);
			CHECK(stageMs(stages.Opaque) > 0.0f);
			CHECK(stageMs(stages.Post) > 0.0f);
			CHECK(stageMs(stages.Overlays) >= 0.0f); // The HUD text
			CHECK(stageMs(FrameProfiler::Get().FindStage("GPU.UI")) >= 0.0f); // The menu canvas and the world sign
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The skinned arm shows the pose of an animation preview in the entity-ID buffer")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		FeatureProject project;
		LoadAllAssets(*project.GetAssetManager());
		const Ref<Scene> scene = project.LoadStartScene();
		const Entity rig = scene->FindEntityByName("Arm Rig");
		const Entity arm = scene->FindEntityByName("Arm");
		// A cube on the "Arm Tip", which is attached to the wrist (AttachToJoint).
		const Entity tipMarker = scene->FindEntityByName("Tip Marker");
		REQUIRE(rig.IsValid());
		REQUIRE(arm.IsValid());
		REQUIRE(tipMarker.IsValid());

		// The arm from the front: it stands 1 unit tall on the rig, its elbow halfway up.
		constexpr uint32_t c_Size = 128;
		const glm::vec3 origin = glm::vec3(scene->GetWorldTransform(rig)[3]);
		const SceneCamera camera = OrthographicLookAt(origin + glm::vec3(0.0f, 0.5f, 4.0f), origin + glm::vec3(0.0f, 0.5f, 0.0f), 2.0f);
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		auto render = [&]()
		{
			for (int32_t frame = 0; frame < 2; frame++)
			{
				project.GetAssetManager()->Update();
				REQUIRE(renderer.Render(*scene, camera));
			}
		};
		const glm::uvec2 upright = ProjectToPixel(camera, origin + glm::vec3(0.0f, 0.85f, 0.08f), c_Size, c_Size);
		const glm::uvec2 bent = ProjectToPixel(camera, origin + glm::vec3(-0.35f, 0.5f, 0.08f), c_Size, c_Size);
		// The tip marker's side beyond the wrist: above the upright arm, left of the bent one.
		const glm::uvec2 uprightTip = ProjectToPixel(camera, origin + glm::vec3(0.0f, 1.05f, 0.1f), c_Size, c_Size);
		const glm::uvec2 bentTip = ProjectToPixel(camera, origin + glm::vec3(-0.545f, 0.5f, 0.1f), c_Size, c_Size);

		// Not playing, nothing poses the arm: the bind pose, the forearm upright.
		render();
		CHECK(renderer.GetEntityAt(*scene, upright.x, upright.y) == arm);
		CHECK(renderer.GetEntityAt(*scene, bent.x, bent.y) != arm);
		CHECK(renderer.GetEntityAt(*scene, uprightTip.x, uprightTip.y) == tipMarker);
		CHECK(renderer.GetStats().SkinnedInstances == 0);

		// Half a second into Wave the elbow is bent 90 degrees: the forearm points along -X.
		AnimationPreview preview(*scene);
		REQUIRE(preview.IsRegistered());
		AnimationPreview::Request request;
		request.State = "Wave";
		request.Time = 0.5f;
		REQUIRE(preview.Set(rig, request));
		preview.Update();
		REQUIRE(preview.GetStatus(rig) == AnimationPreview::Status::Posed);
		render();
		CHECK(renderer.GetEntityAt(*scene, bent.x, bent.y) == arm);
		CHECK(renderer.GetEntityAt(*scene, upright.x, upright.y) != arm);
		CHECK(GetDrawnCounts(renderer).SkinnedInstances >= 1);
		// The attachment follows the wrist in the preview too (where it is drawn; the scene keeps its transform).
		CHECK(renderer.GetEntityAt(*scene, bentTip.x, bentTip.y) == tipMarker);
		CHECK(renderer.GetEntityAt(*scene, uprightTip.x, uprightTip.y) != tipMarker);
		const Entity armTip = scene->FindEntityByName("Arm Tip");
		REQUIRE(armTip.IsValid());
		CHECK(armTip.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f, 1.0f, 0.0f));

		// Without the preview it is back at its own transform.
		REQUIRE(preview.Clear(rig));
		render();
		CHECK(renderer.GetEntityAt(*scene, uprightTip.x, uprightTip.y) == tipMarker);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
