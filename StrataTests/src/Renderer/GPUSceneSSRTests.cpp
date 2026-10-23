#include "Renderer/TAATestUtils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

// Screen-space reflections (PostProcessComponent::ScreenSpaceReflections, Effects/ScreenSpaceReflections.comp): a mirror floor
// under a red cube, against what the CPU predicts its reflection to be.

using namespace Strata;
using namespace Strata::Tests;
using namespace Strata::Tests::TAA;

namespace
{

	constexpr uint32_t c_ViewSize = 128;
	// Frames until the TAA's history (and with it the reflections, which fetch the previous frame) settled.
	constexpr uint32_t c_SettleFrames = 24;

	// A mirror (white metal, smooth unless `roughness` says otherwise) under an unlit red cube, a two-tone sky around them, the
	// camera above the floor looking at the cube.
	struct MirrorScene
	{
		Scene Contents;
		Entity Floor;
		Entity Cube;
		SceneCamera Camera;

		MirrorScene(SceneTestAssets& assets, float roughness, TemporalAntiAliasingPreset preset = TemporalAntiAliasingPreset::Original)
		{
			AddTemporalPostProcess(Contents, preset).ScreenSpaceReflections = true;
			Contents.CreateEntity("Sky").AddComponent<SkyLightComponent>().EnvironmentMap =
				assets.Add(CreateEnvironmentTexture(glm::vec3(0.1f, 0.2f, 0.5f), glm::vec3(0.05f)), "Sky");
			MaterialProperties mirror;
			mirror.BaseColor = glm::vec4(1.0f);
			mirror.Metallic = 1.0f;
			mirror.Roughness = roughness;
			Floor = AddMesh(Contents, BuiltinAssets::PlaneMesh, assets.AddMaterial(mirror), glm::vec3(0.0f), "Floor");
			Floor.GetComponent<TransformComponent>().Scale = glm::vec3(20.0f);
			Cube = AddMesh(Contents, BuiltinAssets::CubeMesh, assets.AddMaterial(UnlitColor({ 1.0f, 0.0f, 0.0f, 1.0f })), glm::vec3(0.0f, 1.0f, 0.0f), "Cube");
			Camera = LookAt(glm::vec3(0.0f, 1.5f, 5.0f), glm::vec3(0.0f, 0.5f, 0.0f));
		}
	};

	void RenderFrames(GPUContext& gpu, SceneRenderer& renderer, Scene& scene, const SceneCamera& camera, uint32_t frames)
	{
		for (uint32_t frame = 0; frame < frames; frame++)
		{
			REQUIRE(renderer.Render(scene, camera));
			FinishFrame(gpu);
		}
	}

	// Renders until the sky's environment lights the scene (made over a few frames), then starts the TAA's history again: what
	// follows depends on the frames counted from there only.
	void WarmUp(GPUContext& gpu, SceneRenderer& renderer, Scene& scene, const SceneCamera& camera)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		do
		{
			REQUIRE(renderer.Render(scene, camera));
			FinishFrame(gpu);
		} while (!renderer.GetStats().EnvironmentLighting && std::chrono::steady_clock::now() < deadline);
		REQUIRE(renderer.GetStats().EnvironmentLighting);
		renderer.ResetHistory();
	}

	// The settled image of the scene with and without screen-space reflections, from one renderer (the same environment).
	std::pair<std::vector<uint16_t>, std::vector<uint16_t>> RenderWithAndWithout(GPUContext& gpu, Scene& scene, const SceneCamera& camera)
	{
		SceneRenderer renderer = CreateRenderer("Reflections");
		renderer.SetViewportSize(c_ViewSize, c_ViewSize);
		GetPostProcess(scene).ScreenSpaceReflections = true;
		WarmUp(gpu, renderer, scene, camera);
		RenderFrames(gpu, renderer, scene, camera, c_SettleFrames);
		CHECK(renderer.GetStats().ScreenSpaceReflections);
		std::vector<uint16_t> with = ReadHDRBits(renderer.GetResolvedHDRTexture());
		GetPostProcess(scene).ScreenSpaceReflections = false;
		renderer.ResetHistory();
		RenderFrames(gpu, renderer, scene, camera, c_SettleFrames);
		CHECK_FALSE(renderer.GetStats().ScreenSpaceReflections);
		return { std::move(with), ReadHDRBits(renderer.GetResolvedHDRTexture()) };
	}

	glm::vec3 TexelAt(const std::vector<glm::vec3>& image, const glm::uvec2& pixel)
	{
		REQUIRE(pixel.x < c_ViewSize);
		REQUIRE(pixel.y < c_ViewSize);
		return image[static_cast<size_t>(pixel.y) * c_ViewSize + pixel.x];
	}

	// Where the floor shows a point's mirror image (the point mirrored about the floor's plane, y = 0).
	glm::uvec2 MirrorPixel(const SceneCamera& camera, const glm::vec3& point)
	{
		return ProjectToPixel(camera, glm::vec3(point.x, -point.y, point.z), c_ViewSize);
	}

	// The red texels of the reflections' target (the cube's reflection): how many, and their mean column.
	struct RedTexels
	{
		uint32_t Count = 0;
		float MeanX = 0.0f;
	};

	RedTexels FindRedTexels(SceneRenderer& renderer)
	{
		const ReadbackImage image = ReadHalfImage(renderer.GetScreenReflectionTexture());
		RedTexels red;
		double sumX = 0.0;
		for (uint32_t y = 0; y < image.Height; y++)
		{
			for (uint32_t x = 0; x < image.Width; x++)
			{
				const glm::vec4 texel = ReadHalfTexel(image, x, y);
				if (texel.a > 0.5f && texel.r > 0.5f && texel.b < 0.2f)
				{
					red.Count++;
					sumX += x;
				}
			}
		}
		red.MeanX = red.Count > 0 ? static_cast<float>(sumX / red.Count) : 0.0f;
		return red;
	}

}

TEST_SUITE("GPU.SceneRenderer.SSR")
{
	TEST_CASE("A mirror floor reflects a red cube where the CPU predicts, alike on both paths")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		MirrorScene scene(assets, 0.0f);
		SceneRenderer cpu = CreateRenderer("CPU", GPUDrivenMode::Off);
		SceneRenderer gpuDriven = CreateRenderer("GPU", GPUDrivenMode::Auto);
		cpu.SetViewportSize(c_ViewSize, c_ViewSize);
		gpuDriven.SetViewportSize(c_ViewSize, c_ViewSize);
		WarmUp(gpu, cpu, scene.Contents, scene.Camera);
		WarmUp(gpu, gpuDriven, scene.Contents, scene.Camera);
		for (uint32_t frame = 0; frame < c_SettleFrames; frame++)
		{
			CAPTURE(frame);
			const ReadbackImage cpuImage = RenderToImage(cpu, scene.Contents, scene.Camera);
			const ReadbackImage gpuImage = RenderToImage(gpuDriven, scene.Contents, scene.Camera);
			CHECK(MaxDifference(cpuImage, gpuImage) <= 1);
			// The first frame of the history has no previous frame to fetch from.
			CHECK(cpu.GetStats().ScreenSpaceReflections == (frame > 0));
			CHECK(gpuDriven.GetStats().ScreenSpaceReflections == (frame > 0));
			CHECK(gpuDriven.GetStats().GPUDriven);
		}

		for (SceneRenderer* renderer : { &cpu, &gpuDriven })
		{
			CAPTURE(renderer->GetSpecification().DebugName);
			REQUIRE(renderer->GetSurfaceTexture());
			REQUIRE(renderer->GetScreenReflectionTexture());
			CHECK(renderer->GetScreenReflectionTexture()->getDesc().width == c_ViewSize / 2);
			const std::vector<glm::vec3> image = ReadHDR(renderer->GetResolvedHDRTexture());
			// The cube's front face, seen in the floor below it: red.
			for (const glm::vec3& point : { glm::vec3(0.0f, 1.0f, 0.5f), glm::vec3(-0.3f, 0.8f, 0.5f), glm::vec3(0.3f, 1.2f, 0.5f) })
			{
				CAPTURE(point);
				const glm::vec3 color = TexelAt(image, MirrorPixel(scene.Camera, point));
				CAPTURE(color);
				CHECK(color.r > 0.6f);
				CHECK(color.g < 0.1f);
				CHECK(color.b < 0.25f);
			}
			// Beside the cube and above it the floor mirrors the sky (blue), as it does without the reflections.
			for (const glm::vec3& point : { glm::vec3(1.6f, 1.0f, 0.5f), glm::vec3(-1.6f, 1.0f, 0.5f), glm::vec3(0.0f, 2.2f, 0.5f) })
			{
				CAPTURE(point);
				const glm::vec3 color = TexelAt(image, MirrorPixel(scene.Camera, point));
				CAPTURE(color);
				CHECK(color.r < 0.2f);
				CHECK(color.b > color.r);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The reflection of a face the rays cross is whole in every frame, without holes")
	{
		// Seen from low over the floor, the rays from the floor cross the cube's front face between two of their points: each
		// must hit it, whatever the per-pixel noise places those points, though the face's own depth changes between them.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		MirrorScene scene(assets, 0.0f);
		scene.Camera = LookAt(glm::vec3(0.0f, 1.2f, 6.0f), glm::vec3(0.0f, 0.4f, 0.0f));
		SceneRenderer renderer = CreateRenderer("Holes");
		renderer.SetViewportSize(c_ViewSize, c_ViewSize);
		// The mirrored front face's texels of the half-resolution target, away from its edges: rays that cross the face near an
		// edge may step past it (its edges are ragged in a frame and smooth only over frames), most toward its top edge, which
		// the rays reach last and in the longest steps (the bottom of its reflection).
		glm::vec2 low(1e9f);
		glm::vec2 high(-1e9f);
		for (const glm::vec3& corner : { glm::vec3(-0.5f, 0.5f, 0.5f), glm::vec3(0.5f, 0.5f, 0.5f), glm::vec3(-0.5f, 1.5f, 0.5f), glm::vec3(0.5f, 1.5f, 0.5f) })
		{
			const glm::vec2 pixel = glm::vec2(MirrorPixel(scene.Camera, corner)) * 0.5f;
			low = glm::min(low, pixel);
			high = glm::max(high, pixel);
		}
		const glm::uvec2 first = glm::uvec2(low + glm::vec2(2.5f, 1.5f));
		const glm::uvec2 last = glm::uvec2(high - glm::vec2(2.5f, 4.5f));
		REQUIRE(last.x >= first.x + 4);
		REQUIRE(last.y >= first.y + 3);
		REQUIRE(renderer.Render(scene.Contents, scene.Camera));
		FinishFrame(gpu);
		for (uint32_t frame = 1; frame < 9; frame++)
		{
			CAPTURE(frame);
			REQUIRE(renderer.Render(scene.Contents, scene.Camera));
			FinishFrame(gpu);
			REQUIRE(renderer.GetStats().ScreenSpaceReflections);
			const ReadbackImage image = ReadHalfImage(renderer.GetScreenReflectionTexture());
			uint32_t holes = 0;
			for (uint32_t y = first.y; y <= last.y; y++)
			{
				for (uint32_t x = first.x; x <= last.x; x++)
				{
					const glm::vec4 texel = ReadHalfTexel(image, x, y);
					holes += texel.a > 0.5f && texel.r > 0.5f ? 0u : 1u;
				}
			}
			CHECK(holes == 0u);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Rough surfaces and rays that leave the screen keep the sky's reflection")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		SUBCASE("A floor rougher than the reflections' maximum roughness")
		{
			MirrorScene scene(assets, 0.8f);
			const auto [with, without] = RenderWithAndWithout(gpu, scene.Contents, scene.Camera);
			CHECK(with == without);
		}
		SUBCASE("A cube above the view, whose reflection the floor would show")
		{
			MirrorScene scene(assets, 0.0f);
			scene.Camera = LookAt(glm::vec3(0.0f, 1.0f, 5.0f), glm::vec3(0.0f, -4.0f, 0.0f));
			// The cube's lowest point is above the view, its mirror image in it.
			const glm::vec4 cubeClip = scene.Camera.Projection * scene.Camera.View * glm::vec4(0.0f, 0.5f, 0.5f, 1.0f);
			REQUIRE(cubeClip.y / cubeClip.w > 1.0f);
			const glm::vec4 mirrorClip = scene.Camera.Projection * scene.Camera.View * glm::vec4(0.0f, -1.0f, 0.5f, 1.0f);
			REQUIRE(std::abs(mirrorClip.y / mirrorClip.w) < 0.8f);
			const auto [with, without] = RenderWithAndWithout(gpu, scene.Contents, scene.Camera);
			CHECK(with == without);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Without TAA nothing traces and no surface target is made")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		MirrorScene scene(assets, 0.0f);
		SUBCASE("FXAA")
		{
			GetPostProcess(scene.Contents).AntiAliasingMethod = AntiAliasingMode::FXAA;
			SceneRenderer renderer = CreateRenderer("FXAA");
			renderer.SetViewportSize(c_ViewSize, c_ViewSize);
			RenderFrames(gpu, renderer, scene.Contents, scene.Camera, 3);
			CHECK_FALSE(renderer.GetStats().ScreenSpaceReflections);
			CHECK(renderer.GetSurfaceTexture() == nullptr);
			CHECK(renderer.GetScreenReflectionTexture() == nullptr);
		}
		SUBCASE("A renderer of single images")
		{
			SceneRenderer renderer = CreateRenderer("Single", GPUDrivenMode::Off, false);
			renderer.SetViewportSize(c_ViewSize, c_ViewSize);
			RenderFrames(gpu, renderer, scene.Contents, scene.Camera, 3);
			CHECK_FALSE(renderer.GetStats().TemporalAntiAliasing);
			CHECK_FALSE(renderer.GetStats().ScreenSpaceReflections);
			CHECK(renderer.GetSurfaceTexture() == nullptr);
		}
		SUBCASE("Released with the targets, made again by the next frame that traces")
		{
			SceneRenderer renderer = CreateRenderer("Resized");
			renderer.SetViewportSize(c_ViewSize, c_ViewSize);
			RenderFrames(gpu, renderer, scene.Contents, scene.Camera, 2);
			REQUIRE(renderer.GetSurfaceTexture());
			renderer.SetViewportSize(c_ViewSize / 2, c_ViewSize / 2);
			CHECK(renderer.GetSurfaceTexture() == nullptr);
			CHECK(renderer.GetScreenReflectionTexture() == nullptr);
			RenderFrames(gpu, renderer, scene.Contents, scene.Camera, 2);
			REQUIRE(renderer.GetScreenReflectionTexture());
			CHECK(renderer.GetScreenReflectionTexture()->getDesc().width == c_ViewSize / 4);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A moving cube's reflection keeps up with it: the history is fetched along the hit's motion")
	{
		// The cube moves half its size each frame; the reflection fetches it where the previous frame showed it. Fetched at the
		// hit's own position instead, only the part the cube's two positions share would be red, behind its motion.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		MirrorScene moving(assets, 0.0f, TemporalAntiAliasingPreset::Sharp);
		SceneRenderer renderer = CreateRenderer("Moving");
		renderer.SetViewportSize(c_ViewSize, c_ViewSize);
		constexpr float c_Step = 0.5f;
		constexpr uint32_t c_Frames = 7;
		const float start = -c_Step * static_cast<float>(c_Frames - 1) * 0.5f;
		for (uint32_t frame = 0; frame < c_Frames; frame++)
		{
			moving.Cube.GetComponent<TransformComponent>().Translation.x = start + c_Step * static_cast<float>(frame);
			moving.Cube.MarkModified<TransformComponent>();
			REQUIRE(renderer.Render(moving.Contents, moving.Camera));
			FinishFrame(gpu);
		}
		REQUIRE(renderer.GetStats().ScreenSpaceReflections);
		const RedTexels movingRed = FindRedTexels(renderer);
		const float cubeX = moving.Cube.GetComponent<TransformComponent>().Translation.x;

		// The same cube standing still where the moving one ended.
		MirrorScene still(assets, 0.0f, TemporalAntiAliasingPreset::Sharp);
		still.Cube.GetComponent<TransformComponent>().Translation.x = cubeX;
		SceneRenderer stillRenderer = CreateRenderer("Still");
		stillRenderer.SetViewportSize(c_ViewSize, c_ViewSize);
		RenderFrames(gpu, stillRenderer, still.Contents, still.Camera, c_Frames);
		const RedTexels stillRed = FindRedTexels(stillRenderer);

		REQUIRE(stillRed.Count > 20u);
		const float predictedX = static_cast<float>(MirrorPixel(moving.Camera, glm::vec3(cubeX, 1.0f, 0.5f)).x) * 0.5f;
		CAPTURE(movingRed.Count);
		CAPTURE(stillRed.Count);
		CAPTURE(movingRed.MeanX);
		CAPTURE(stillRed.MeanX);
		CAPTURE(predictedX);
		CHECK(std::abs(stillRed.MeanX - predictedX) < 1.5f);
		CHECK(static_cast<float>(movingRed.Count) > 0.85f * static_cast<float>(stillRed.Count));
		CHECK(std::abs(movingRed.MeanX - stillRed.MeanX) < 1.0f);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
