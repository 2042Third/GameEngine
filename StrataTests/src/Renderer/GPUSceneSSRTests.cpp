#include "Renderer/TAATestUtils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

// Screen-space reflections (PostProcessComponent::ScreenSpaceReflections; RendererQualitySettings::Reflections Checkerboard:
// Effects/ScreenSpaceReflections.comp, Spatial: ScreenSpaceReflectionsSpatial.comp and ReflectionResolve.comp): a mirror floor
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

	// A renderer as CreateRenderer makes them (Medium: Checkerboard) with another Reflections mode.
	SceneRenderer CreateReflectionRenderer(const char* name, ReflectionsQuality reflections, GPUDrivenMode mode = GPUDrivenMode::Off)
	{
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		specification.GPUDriven = mode;
		specification.DebugName = name;
		specification.Quality.Reflections = reflections;
		return SceneRenderer(specification);
	}

	// The modes the tests run in: Checkerboard with TAA (its only way), Spatial without it (FXAA; it needs no history).
	struct ReflectionMode
	{
		ReflectionsQuality Quality;
		const char* Name;
	};
	constexpr ReflectionMode c_ReflectionModes[] = { { ReflectionsQuality::Checkerboard, "Checkerboard with TAA" },
		{ ReflectionsQuality::Spatial, "Spatial with FXAA" } };

	// The scene's anti-aliasing for a mode's tests.
	void SetAntiAliasingFor(Scene& scene, ReflectionsQuality reflections)
	{
		GetPostProcess(scene).AntiAliasingMethod = reflections == ReflectionsQuality::Checkerboard ? AntiAliasingMode::TAA : AntiAliasingMode::FXAA;
	}

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
	std::pair<std::vector<uint16_t>, std::vector<uint16_t>> RenderWithAndWithout(GPUContext& gpu, Scene& scene, const SceneCamera& camera,
		ReflectionsQuality reflections)
	{
		SetAntiAliasingFor(scene, reflections);
		SceneRenderer renderer = CreateReflectionRenderer("Reflections", reflections);
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

	// The red texels of the reflections' target (the cube's reflection): how many, and their mean column in the view's pixels
	// (the Checkerboard target is half the view's size).
	struct RedTexels
	{
		uint32_t Count = 0;
		float MeanX = 0.0f;
	};

	RedTexels FindRedTexels(SceneRenderer& renderer)
	{
		const ReadbackImage image = ReadHalfImage(renderer.GetScreenReflectionTexture());
		const float scale = static_cast<float>(c_ViewSize) / static_cast<float>(image.Width);
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
		red.MeanX = red.Count > 0 ? (static_cast<float>(sumX / red.Count) + 0.5f) * scale - 0.5f : 0.0f;
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

	TEST_CASE("A reflection's edges blend into the probes' or the sky's reflection without darkening")
	{
		// The forward pass samples the half-resolution reflections bilinearly: where texels that hit the cube and texels that
		// found nothing meet, its pixels must show a mix of the cube's reflection and the probe's or the sky's. Mixed by the
		// filtered confidence over a color the filter had blended with the misses' black, they lost a fifth of both.
		// The Spatial mode resolves its full-resolution target before the same lookup.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		for (const auto& [mode, probe] : { std::pair(c_ReflectionModes[0], false), std::pair(c_ReflectionModes[0], true),
				 std::pair(c_ReflectionModes[1], false), std::pair(c_ReflectionModes[1], true) })
		{
			CAPTURE(mode.Name);
			CAPTURE(probe);
			MirrorScene scene(assets, 0.0f);
			SetAntiAliasingFor(scene.Contents, mode.Quality);
			if (probe)
			{
				// A green probe whose box holds everything in view, in place of the sky's reflection.
				ReflectionProbeComponent& component = scene.Contents.CreateEntity("Probe").AddComponent<ReflectionProbeComponent>();
				component.Capture = assets.Add(CreateEnvironmentTexture(glm::vec3(0.0f, 0.6f, 0.0f), glm::vec3(0.0f, 0.6f, 0.0f)), "Probe");
				component.Extents = glm::vec3(40.0f);
			}
			SceneRenderer renderer = CreateReflectionRenderer("Edges", mode.Quality);
			renderer.SetViewportSize(c_ViewSize, c_ViewSize);
			auto ready = [&renderer, probe]()
			{
				const SceneRendererStats& stats = renderer.GetStats();
				return stats.EnvironmentLighting && (!probe || (stats.ReflectionProbes == 1 && stats.ReflectionProbesPending == 0));
			};
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			do
			{
				REQUIRE(renderer.Render(scene.Contents, scene.Camera));
				FinishFrame(gpu);
			} while (!ready() && std::chrono::steady_clock::now() < deadline);
			REQUIRE(ready());
			renderer.ResetHistory();
			RenderFrames(gpu, renderer, scene.Contents, scene.Camera, c_SettleFrames);
			REQUIRE(renderer.GetStats().ScreenSpaceReflections);
			// The forward pass's own image, before the TAA's resolve blends it with the history.
			const std::vector<glm::vec3> image = ReadHDR(renderer.GetHDRTexture());

			// A row across the middle of the cube's reflection: the probe's or the sky's reflection beside it (outside), the
			// cube's red in its middle (inside). Each pixel between is outside * a + inside * b with a + b = 1.
			const glm::uvec2 left = MirrorPixel(scene.Camera, glm::vec3(-1.6f, 1.0f, 0.5f));
			const glm::uvec2 middle = MirrorPixel(scene.Camera, glm::vec3(0.0f, 1.0f, 0.5f));
			const glm::uvec2 right = MirrorPixel(scene.Camera, glm::vec3(1.6f, 1.0f, 0.5f));
			const glm::vec3 outside = TexelAt(image, left);
			const glm::vec3 inside = TexelAt(image, middle);
			CAPTURE(outside);
			CAPTURE(inside);
			CHECK(inside.r > 0.6f);
			if (probe)
				CHECK((outside.g > 0.3f && outside.r < 0.1f && outside.b < 0.1f));
			else
				CHECK((outside.b > 0.2f && outside.r < 0.2f));
			REQUIRE(left.x + 8 < right.x);
			// a and b by least squares over the pixel's three channels.
			const double aa = glm::dot(outside, outside);
			const double ab = glm::dot(outside, inside);
			const double bb = glm::dot(inside, inside);
			const double determinant = aa * bb - ab * ab;
			REQUIRE(determinant > 1e-6);
			double lowest = 1e9;
			for (uint32_t x = left.x; x <= right.x; x++)
			{
				const glm::vec3 pixel = TexelAt(image, glm::uvec2(x, middle.y));
				const double pa = glm::dot(pixel, outside);
				const double pb = glm::dot(pixel, inside);
				lowest = std::min(lowest, ((pa * bb - pb * ab) + (pb * aa - pa * ab)) / determinant);
			}
			CAPTURE(lowest);
			CHECK(lowest > 0.9);
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
			for (const ReflectionMode& mode : c_ReflectionModes)
			{
				CAPTURE(mode.Name);
				MirrorScene scene(assets, 0.8f);
				const auto [with, without] = RenderWithAndWithout(gpu, scene.Contents, scene.Camera, mode.Quality);
				CHECK(with == without);
			}
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
			for (const ReflectionMode& mode : c_ReflectionModes)
			{
				CAPTURE(mode.Name);
				const auto [with, without] = RenderWithAndWithout(gpu, scene.Contents, scene.Camera, mode.Quality);
				CHECK(with == without);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Checkerboard without TAA traces nothing and makes no surface target")
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
		SUBCASE("The players' anti-aliasing row over a TAA scene: only TAA traces")
		{
			for (const AntiAliasingChoice choice : { AntiAliasingChoice::Off, AntiAliasingChoice::FXAA, AntiAliasingChoice::Morphological,
					 AntiAliasingChoice::TAA })
			{
				CAPTURE(static_cast<int>(choice));
				SceneRendererSpecification specification;
				specification.TextureStreaming = false;
				specification.DebugName = "PlayersRow";
				specification.Quality.AntiAliasing = choice;
				SceneRenderer renderer(specification);
				renderer.SetViewportSize(c_ViewSize, c_ViewSize);
				RenderFrames(gpu, renderer, scene.Contents, scene.Camera, 3);
				const bool temporal = choice == AntiAliasingChoice::TAA;
				CHECK(renderer.GetStats().TemporalAntiAliasing == temporal);
				CHECK(renderer.GetStats().ScreenSpaceReflections == temporal);
				CHECK((renderer.GetSurfaceTexture() != nullptr) == temporal);
			}
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
		// Spatial reads the previous frame's color copy the same way.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		for (const ReflectionMode& mode : c_ReflectionModes)
		{
			CAPTURE(mode.Name);
			MirrorScene moving(assets, 0.0f, TemporalAntiAliasingPreset::Sharp);
			SetAntiAliasingFor(moving.Contents, mode.Quality);
			SceneRenderer renderer = CreateReflectionRenderer("Moving", mode.Quality);
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
			SetAntiAliasingFor(still.Contents, mode.Quality);
			still.Cube.GetComponent<TransformComponent>().Translation.x = cubeX;
			SceneRenderer stillRenderer = CreateReflectionRenderer("Still", mode.Quality);
			stillRenderer.SetViewportSize(c_ViewSize, c_ViewSize);
			RenderFrames(gpu, stillRenderer, still.Contents, still.Camera, c_Frames);
			const RedTexels stillRed = FindRedTexels(stillRenderer);

			REQUIRE(stillRed.Count > 20u);
			const float predictedX = static_cast<float>(MirrorPixel(moving.Camera, glm::vec3(cubeX, 1.0f, 0.5f)).x);
			CAPTURE(movingRed.Count);
			CAPTURE(stillRed.Count);
			CAPTURE(movingRed.MeanX);
			CAPTURE(stillRed.MeanX);
			CAPTURE(predictedX);
			CHECK(std::abs(stillRed.MeanX - predictedX) < 3.0f);
			CHECK(static_cast<float>(movingRed.Count) > 0.85f * static_cast<float>(stillRed.Count));
			CHECK(std::abs(movingRed.MeanX - stillRed.MeanX) < 2.0f);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Spatial reflections trace every pixel without TAA, where the CPU predicts, alike on both paths and in every frame")
	{
		// Each frame traces from the previous frame's color: the first after a reset has none to read (pending), every later one
		// is complete in itself, so a still view gives the same image frame after frame (no history averages anything).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		for (const bool fxaa : { true, false })
		{
			CAPTURE(fxaa);
			MirrorScene scene(assets, 0.0f);
			PostProcessComponent& post = GetPostProcess(scene.Contents);
			post.AntiAliasingMethod = AntiAliasingMode::FXAA;
			post.AntiAliasing = fxaa;
			SceneRenderer cpu = CreateReflectionRenderer("CPU", ReflectionsQuality::Spatial, GPUDrivenMode::Off);
			SceneRenderer gpuDriven = CreateReflectionRenderer("GPU", ReflectionsQuality::Spatial, GPUDrivenMode::Auto);
			cpu.SetViewportSize(c_ViewSize, c_ViewSize);
			gpuDriven.SetViewportSize(c_ViewSize, c_ViewSize);
			WarmUp(gpu, cpu, scene.Contents, scene.Camera);
			WarmUp(gpu, gpuDriven, scene.Contents, scene.Camera);
			std::vector<uint16_t> previous;
			for (uint32_t frame = 0; frame < 5; frame++)
			{
				CAPTURE(frame);
				const ReadbackImage cpuImage = RenderToImage(cpu, scene.Contents, scene.Camera);
				const ReadbackImage gpuImage = RenderToImage(gpuDriven, scene.Contents, scene.Camera);
				CHECK(MaxDifference(cpuImage, gpuImage) <= 1);
				for (SceneRenderer* renderer : { &cpu, &gpuDriven })
				{
					const SceneRendererStats& stats = renderer->GetStats();
					CHECK_FALSE(stats.TemporalAntiAliasing);
					CHECK(stats.ScreenSpaceReflections == (frame > 0));
					CHECK(stats.ScreenSpaceReflectionsPending == (frame == 0));
					CHECK(stats.SceneColorCopies == 1u);
				}
				CHECK(gpuDriven.GetStats().GPUDriven);
				// From the third frame on the reflections see a frame that had them too: nothing changes any more.
				const std::vector<uint16_t> bits = ReadHDRBits(cpu.GetHDRTexture());
				if (frame >= 3)
					CHECK(bits == previous);
				previous = bits;
			}

			for (SceneRenderer* renderer : { &cpu, &gpuDriven })
			{
				CAPTURE(renderer->GetSpecification().DebugName);
				REQUIRE(renderer->GetScreenReflectionTexture());
				CHECK(renderer->GetScreenReflectionTexture()->getDesc().width == c_ViewSize);
				const std::vector<glm::vec3> image = ReadHDR(renderer->GetHDRTexture());
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
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Spatial reflections of a wall across a grazing mirror floor are whole")
	{
		// From low over a mirror floor, the rays from the floor travel hundreds of pixels toward a red wall, just above floor
		// pixels nearer than they are. Each must reach the wall: a hierarchical march over a nearest-depth pyramid ran out of
		// iterations there (every coarse cell holds nearer floor below the ray), so the wall's reflection broke into stripes.
		constexpr uint32_t c_WideSize = 1536;
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		MirrorScene scene(assets, 0.0f);
		// The cube becomes a wall 8 m wide and 2 m high whose front face is 3.9 m behind the origin.
		TransformComponent& wall = scene.Cube.GetComponent<TransformComponent>();
		wall.Translation = glm::vec3(0.0f, 1.0f, -4.0f);
		wall.Scale = glm::vec3(8.0f, 2.0f, 0.2f);
		scene.Cube.MarkModified<TransformComponent>();
		scene.Camera = LookAt(glm::vec3(0.0f, 0.6f, 6.0f), glm::vec3(0.0f, 0.3f, 0.0f));
		SetAntiAliasingFor(scene.Contents, ReflectionsQuality::Spatial);
		SceneRenderer renderer = CreateReflectionRenderer("Wall", ReflectionsQuality::Spatial);
		renderer.SetViewportSize(c_WideSize, c_WideSize);
		WarmUp(gpu, renderer, scene.Contents, scene.Camera);
		RenderFrames(gpu, renderer, scene.Contents, scene.Camera, 3);
		REQUIRE(renderer.GetStats().ScreenSpaceReflections);

		// The face's mirror image in the floor, away from the screen's edges (where hits fade) and from the face's: rays that
		// reach it near its top edge, last and in their longest steps (the bottom of its reflection), may step past it, so
		// that edge is ragged over a few pixels, as it is for the Checkerboard trace in a frame.
		glm::vec2 low(1e9f);
		glm::vec2 high(-1e9f);
		for (const glm::vec3& corner : { glm::vec3(-3.0f, 0.0f, -3.9f), glm::vec3(3.0f, 0.0f, -3.9f), glm::vec3(-3.0f, -2.0f, -3.9f), glm::vec3(3.0f, -2.0f, -3.9f) })
		{
			const glm::vec2 pixel = glm::vec2(ProjectToPixel(scene.Camera, corner, c_WideSize));
			low = glm::min(low, pixel);
			high = glm::max(high, pixel);
		}
		const float margin = 0.12f * static_cast<float>(c_WideSize);
		const float raggedEdge = 0.15f * (high.y - low.y);
		const glm::uvec2 first = glm::uvec2(glm::max(low + glm::vec2(4.0f), glm::vec2(margin)));
		const glm::uvec2 last = glm::uvec2(glm::min(high - glm::vec2(4.0f, 4.0f + raggedEdge), glm::vec2(static_cast<float>(c_WideSize) - margin)));
		REQUIRE(last.x >= first.x + 100);
		REQUIRE(last.y >= first.y + 40);
		REQUIRE(renderer.GetScreenReflectionTexture());
		const ReadbackImage image = ReadHalfImage(renderer.GetScreenReflectionTexture());
		REQUIRE(image.Width == c_WideSize);
		uint32_t holes = 0;
		uint32_t texels = 0;
		for (uint32_t y = first.y; y <= last.y; y++)
		{
			for (uint32_t x = first.x; x <= last.x; x++)
			{
				const glm::vec4 texel = ReadHalfTexel(image, x, y);
				holes += texel.a > 0.5f && texel.r > 0.5f && texel.b < 0.25f * texel.r ? 0u : 1u;
				texels++;
			}
		}
		CAPTURE(holes);
		CAPTURE(texels);
		CHECK(holes == 0u);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The Reflections row: Off traces nothing, and a changed mode releases one mode's targets for the other's")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		MirrorScene scene(assets, 0.0f);
		SceneRenderer renderer = CreateRenderer("Modes");
		renderer.SetViewportSize(c_ViewSize, c_ViewSize);
		CHECK(renderer.GetQuality().Reflections == ReflectionsQuality::Checkerboard);
		RenderFrames(gpu, renderer, scene.Contents, scene.Camera, 3);
		CHECK(renderer.GetStats().ScreenSpaceReflections);
		REQUIRE(renderer.GetScreenReflectionTexture());
		CHECK(renderer.GetScreenReflectionTexture()->getDesc().width == c_ViewSize / 2);

		const auto setReflections = [&renderer](ReflectionsQuality reflections)
		{
			RendererQualitySettings quality = renderer.GetQuality();
			quality.Reflections = reflections;
			return renderer.SetQuality(quality);
		};
		SUBCASE("Spatial, with TAA too")
		{
			CHECK(setReflections(ReflectionsQuality::SpatialHigh));
			CHECK(renderer.GetScreenReflectionTexture() == nullptr);
			CHECK(renderer.GetSurfaceTexture() == nullptr);
			RenderFrames(gpu, renderer, scene.Contents, scene.Camera, 3);
			CHECK(renderer.GetStats().TemporalAntiAliasing);
			CHECK(renderer.GetStats().ScreenSpaceReflections);
			REQUIRE(renderer.GetScreenReflectionTexture());
			CHECK(renderer.GetScreenReflectionTexture()->getDesc().width == c_ViewSize);
			// The same mode again changes nothing.
			CHECK_FALSE(setReflections(ReflectionsQuality::SpatialHigh));
			CHECK(renderer.GetScreenReflectionTexture());
		}
		SUBCASE("Off")
		{
			CHECK(setReflections(ReflectionsQuality::Off));
			CHECK(renderer.GetScreenReflectionTexture() == nullptr);
			RenderFrames(gpu, renderer, scene.Contents, scene.Camera, 3);
			CHECK_FALSE(renderer.GetStats().ScreenSpaceReflections);
			CHECK_FALSE(renderer.GetStats().ScreenSpaceReflectionsPending);
			CHECK(renderer.GetStats().SceneColorCopies == 0u);
			CHECK(renderer.GetScreenReflectionTexture() == nullptr);
			CHECK(renderer.GetSurfaceTexture() == nullptr);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
