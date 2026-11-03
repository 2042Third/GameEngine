#include "Renderer/ReflectionMetrics.h"
#include "Renderer/TAATestUtils.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
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

	// What the CPU predicts of a floor pixel's reflected ray (Include/ScreenReflections.slang, BeginScreenRay and
	// GetHitConfidence) when the scene is a mirror floor (y = 0) and a wall facing +Z at z = c_FadeWallZ.
	constexpr float c_FadeWallZ = -4.0f;
	constexpr float c_FadeWallTop = 8.0f;
	constexpr float c_FadeWallHalfWidth = 30.0f;

	struct PredictedRay
	{
		bool HitsWall = false;      // The ray from the floor meets the wall's face (2 px or more inside its outline)
		bool LeavesThroughTop = false;
		float Remaining = 0.0f;     // Pixels from the hit to where the ray leaves the screen (0 when it does not)
		float Confidence = 0.0f;    // The hit's confidence with the direction-aware fade
	};

	PredictedRay PredictFloorRay(const SceneCamera& camera, uint32_t size, const glm::vec2& pixel)
	{
		PredictedRay predicted;
		const glm::mat4 viewProjection = camera.Projection * camera.View;
		const glm::mat4 inverseViewProjection = glm::inverse(viewProjection);
		const glm::vec2 ndc(2.0f * pixel.x / static_cast<float>(size) - 1.0f, 1.0f - 2.0f * pixel.y / static_cast<float>(size));
		const glm::vec4 nearPoint = inverseViewProjection * glm::vec4(ndc, 1.0f, 1.0f);
		const glm::vec3 direction = glm::normalize(glm::vec3(nearPoint) / nearPoint.w - camera.Position);
		if (!(direction.y < -1e-4f))
			return predicted;
		const glm::vec3 floorPoint = camera.Position + direction * (-camera.Position.y / direction.y);
		if (floorPoint.z < c_FadeWallZ)
			return predicted;
		const glm::vec3 reflected(direction.x, -direction.y, direction.z);
		// The screen-space ray to a far point, cut where it leaves the screen or passes the far plane (depth 0).
		const auto toScreen = [&viewProjection](const glm::vec3& point)
		{
			const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
			return glm::vec3(clip.x / clip.w * 0.5f + 0.5f, 0.5f - clip.y / clip.w * 0.5f, clip.z / clip.w);
		};
		const glm::vec3 start = toScreen(floorPoint);
		const glm::vec3 delta = toScreen(floorPoint + reflected * 10000.0f) - start;
		float screenExtent = 1.0f;
		for (int axis = 0; axis < 2; axis++)
		{
			if (delta[axis] > 0.0f)
				screenExtent = std::min(screenExtent, (1.0f - start[axis]) / delta[axis]);
			else if (delta[axis] < 0.0f)
				screenExtent = std::min(screenExtent, -start[axis] / delta[axis]);
		}
		const float depthExtent = delta.z < 0.0f ? -start.z / delta.z : 1.0f;
		const bool leavesScreen = screenExtent < 1.0f && screenExtent <= depthExtent;
		const glm::vec2 exit = glm::vec2(start + delta * std::min(screenExtent, depthExtent));
		if (!(reflected.z < -1e-4f))
			return predicted;
		const glm::vec3 wallPoint = floorPoint + reflected * ((c_FadeWallZ - floorPoint.z) / reflected.z);
		// 2 px inside the wall's outline: hits at its silhouette depend on the march's rounding.
		const float marginMeters = 2.0f * 2.0f * std::tan(camera.VerticalFOV * 0.5f) * (camera.Position.z - c_FadeWallZ) / static_cast<float>(size);
		if (wallPoint.y > c_FadeWallTop - marginMeters || std::abs(wallPoint.x) > c_FadeWallHalfWidth - marginMeters)
			return predicted;
		const glm::vec2 hit = glm::vec2(toScreen(wallPoint));
		if (hit.x < 0.0f || hit.x > 1.0f || hit.y < 0.0f || hit.y > 1.0f)
			return predicted;
		predicted.HitsWall = true;
		predicted.LeavesThroughTop = leavesScreen && std::abs(exit.y) < 1e-3f;
		predicted.Remaining = leavesScreen ? glm::length((exit - hit) * static_cast<float>(size)) : 0.0f;
		const float facing = std::clamp(-4.0f * reflected.z, 0.0f, 1.0f); // The wall's normal is +Z
		const float fade = leavesScreen ? std::clamp(predicted.Remaining / (0.1f * static_cast<float>(size)), 0.0f, 1.0f) : 1.0f;
		predicted.Confidence = facing * fade;
		return predicted;
	}

	// The strafe test's view: 1280x720, a vertical field of view of `fov` degrees.
	constexpr uint32_t c_StrafeWidth = 1280;
	constexpr uint32_t c_StrafeHeight = 720;

	SceneCamera StrafeCamera(const glm::vec3& position, const glm::vec3& target, float fov)
	{
		constexpr float c_Aspect = static_cast<float>(c_StrafeWidth) / static_cast<float>(c_StrafeHeight);
		SceneCamera camera = LookAt(position, target, c_Aspect);
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(fov), c_Aspect, 0.1f, 500.0f);
		camera.VerticalFOV = glm::radians(fov);
		camera.Far = 500.0f;
		return camera;
	}

	// A camera path of the strafe: its first position and target, both moving by Step a frame, the surface measured and the
	// share of its pixels the meter reprojects at least.
	struct StrafePath
	{
		std::string Name;
		glm::vec3 Position;
		glm::vec3 Target;
		float Fov;
		glm::vec3 Step;
		MeasuredRectangle Surface;
		double MinimumMeasured;
	};

	enum class StrafeAntiAliasing
	{
		Off,
		FXAA,
		TAA
	};

	struct StrafeMeasure
	{
		FlickerResult Flicker;
		double SurfaceShare = 0.0; // The measured surface's pixels (its mask) over all pixels, over the measured frames
	};

	// A fresh renderer's strafe along a path: settled still frames (the reflections from the third see a frame that had them,
	// the TAA's history over c_SettleFrames), then two more still frames and 24 moving ones, read as the person sees them and
	// measured on the path's surface from the first triple in steady motion.
	StrafeMeasure MeasureStrafe(GPUContext& gpu, SpecularLab& lab, const StrafePath& path, ReflectionsQuality reflections, StrafeAntiAliasing antiAliasing,
		bool screenReflections)
	{
		constexpr uint32_t c_StillFrames = 2;
		constexpr uint32_t c_MovingFrames = 24;
		PostProcessComponent& post = GetPostProcess(lab.Contents);
		post.ScreenSpaceReflections = screenReflections;
		post.AntiAliasing = antiAliasing != StrafeAntiAliasing::Off;
		post.AntiAliasingMethod = antiAliasing == StrafeAntiAliasing::TAA ? AntiAliasingMode::TAA : AntiAliasingMode::FXAA;
		SceneRendererSpecification specification = ExactSpecification();
		specification.Quality.Reflections = reflections;
		// The cascades stay where they are: the measure is the reflections', not the sun's fit following the camera.
		specification.FitShadowsToView = false;
		specification.DebugName = "Strafe";
		SceneRenderer renderer(specification);
		renderer.SetViewportSize(c_StrafeWidth, c_StrafeHeight);
		SceneCamera camera = StrafeCamera(path.Position, path.Target, path.Fov);
		WarmUp(gpu, renderer, lab.Contents, camera);
		RenderFrames(gpu, renderer, lab.Contents, camera, antiAliasing == StrafeAntiAliasing::TAA ? c_SettleFrames : 3);
		FlickerSequence sequence(c_StillFrames + 1);
		size_t surfacePixels = 0;
		for (uint32_t frame = 0; frame < c_StillFrames + c_MovingFrames; frame++)
		{
			const float moved = static_cast<float>(frame >= c_StillFrames ? frame - c_StillFrames + 1 : 0);
			camera = StrafeCamera(path.Position + path.Step * moved, path.Target + path.Step * moved, path.Fov);
			REQUIRE(renderer.Render(lab.Contents, camera));
			FlickerFrame read = ReadDisplayFlickerFrame(renderer, camera);
			const size_t kept = MaskFlickerFrame(read, path.Surface);
			if (frame > c_StillFrames)
				surfacePixels += kept;
			sequence.Add(std::move(read));
			FinishFrame(gpu);
		}
		CHECK(renderer.GetStats().ScreenSpaceReflections == screenReflections);
		CHECK(renderer.GetStats().TemporalAntiAliasing == (antiAliasing == StrafeAntiAliasing::TAA));
		StrafeMeasure measure;
		measure.Flicker = sequence.GetResult();
		measure.SurfaceShare = static_cast<double>(surfacePixels) / (static_cast<double>(c_MovingFrames - 1) * c_StrafeWidth * c_StrafeHeight);
		return measure;
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

	TEST_CASE("A rough metal sheet's reflection shows no screen door")
	{
		// The specular lab's sheet (roughness 0.18) seen as lookdev.sheet sees it, across its reflection of the far wall and the
		// posts and of the ladder's spheres (the shot's crops). Whether a ray reaches what it reflects, and what it meets on the
		// way, depends on where along its steps the pixel's fixed offset put it; a resolve whose weights do not hold each offset
		// equally leaves the offsets' pattern locked to the screen. Pattern3 and Pattern4 measure that part alone (the posts'
		// edges are content, which averages out over the phases). Strata 0.7.0: Pattern4 0.66% (Spatial) and 0.11% (SpatialHigh)
		// over the wall, 0.27-0.30% over the spheres.
		constexpr float c_Bound = 0.2f; // Percent of full scale
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		SpecularLab lab(assets);
		const SceneCamera camera = StrafeCamera(glm::vec3(16.0f, 1.6f, -3.6f), glm::vec3(0.0f, 1.5f, -6.0f), 50.0f);
		for (const ReflectionsQuality reflections : { ReflectionsQuality::Spatial, ReflectionsQuality::SpatialHigh })
		{
			for (const bool fxaa : { true, false })
			{
				CAPTURE(static_cast<int>(reflections));
				CAPTURE(fxaa);
				PostProcessComponent& post = GetPostProcess(lab.Contents);
				post.AntiAliasingMethod = AntiAliasingMode::FXAA;
				post.AntiAliasing = fxaa;
				SceneRendererSpecification specification = ExactSpecification();
				specification.Quality.Reflections = reflections;
				specification.DebugName = "ScreenDoor";
				SceneRenderer renderer(specification);
				renderer.SetViewportSize(c_StrafeWidth, c_StrafeHeight);
				WarmUp(gpu, renderer, lab.Contents, camera);
				RenderFrames(gpu, renderer, lab.Contents, camera, 3);
				const ReadbackImage image = RenderToImage(renderer, lab.Contents, camera);
				REQUIRE(renderer.GetStats().ScreenSpaceReflections);
				// lookdev.sheet's crops (Tools/LookDev/Shots.json), normalized: the wall and posts, the spheres.
				const std::pair<const char*, glm::vec4> crops[] = { { "post-wall", glm::vec4(0.61f, 0.28f, 0.73f, 0.52f) },
					{ "spheres", glm::vec4(0.56f, 0.52f, 0.73f, 0.65f) } };
				for (const auto& crop : crops)
				{
					const glm::vec4 rect = crop.second * glm::vec4(c_StrafeWidth, c_StrafeHeight, c_StrafeWidth, c_StrafeHeight);
					const NoiseMeasure measure = MeasureNoise(image, glm::uvec4(rect));
					MESSAGE(fmt::format("Screen door, {}, {}, {}: Noise {:.3f}%, Pattern3 {:.3f}%, Pattern4 {:.3f}% over {} pixels",
						reflections == ReflectionsQuality::Spatial ? "Spatial" : "SpatialHigh", fxaa ? "FXAA" : "no AA", crop.first, measure.Noise,
						measure.Pattern3, measure.Pattern4, measure.Pixels));
					CHECK(measure.Pixels > 15000u);
					CHECK(measure.Pattern3 <= c_Bound);
					CHECK(measure.Pattern4 <= c_Bound);
				}
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Hits fade only where their rays leave the screen")
	{
		// A mirror floor and an unlit wall 60 m wide spanning the view: every floor pixel's reflected ray goes up the screen to
		// the wall. A hit is doubtful only when the ray would have gone on beyond the screen's edge, so it fades over the last
		// 10% of the screen's smaller side the ray has left to its exit (Include/ScreenReflections.slang, GetHitConfidence):
		// texels beside the left and right edges whose rays leave through the top far beyond their hits keep their whole
		// confidence (Strata 0.7.0 faded every hit within 10% of every side), and every hit's confidence is the CPU's.
		constexpr uint32_t c_FadeSize = 512;
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		for (const ReflectionMode& mode : c_ReflectionModes)
		{
			CAPTURE(mode.Name);
			MirrorScene scene(assets, 0.0f);
			scene.Floor.GetComponent<TransformComponent>().Scale = glm::vec3(40.0f);
			TransformComponent& wall = scene.Cube.GetComponent<TransformComponent>();
			wall.Translation = glm::vec3(0.0f, 0.5f * c_FadeWallTop, c_FadeWallZ - 0.1f);
			wall.Scale = glm::vec3(2.0f * c_FadeWallHalfWidth, c_FadeWallTop, 0.2f);
			scene.Cube.GetComponent<MeshRendererComponent>().Material = assets.AddMaterial(UnlitColor({ 0.8f, 0.8f, 0.8f, 1.0f }));
			// Looking down 18 degrees: rays from the nearer floor rise more steeply than the view's top and leave through it beyond the
			// wall, which rises above the view.
			scene.Camera = LookAt(glm::vec3(0.0f, 2.0f, 6.0f), glm::vec3(0.0f));
			SetAntiAliasingFor(scene.Contents, mode.Quality);
			SceneRenderer renderer = CreateReflectionRenderer("Fade", mode.Quality);
			renderer.SetViewportSize(c_FadeSize, c_FadeSize);
			WarmUp(gpu, renderer, scene.Contents, scene.Camera);
			RenderFrames(gpu, renderer, scene.Contents, scene.Camera, mode.Quality == ReflectionsQuality::Checkerboard ? c_SettleFrames : 3u);
			REQUIRE(renderer.GetStats().ScreenSpaceReflections);
			// The trace's own output: Spatial's before its resolve, Checkerboard's half-resolution target (each texel traced
			// from one pixel of its 2x2 block, another each frame: the CPU predicts all four).
			nvrhi::ITexture* traced = mode.Quality == ReflectionsQuality::Checkerboard ? renderer.GetScreenReflectionTexture() : renderer.GetTracedReflectionTexture();
			REQUIRE(traced);
			const ReadbackImage image = ReadHalfImage(traced);
			const uint32_t scale = c_FadeSize / image.Width;
			uint32_t sideTexels = 0;
			uint32_t sideWhole = 0;
			uint32_t compared = 0;
			uint32_t agreeing = 0;
			for (uint32_t y = 0; y < image.Height; y++)
			{
				for (uint32_t x = 0; x < image.Width; x++)
				{
					float lowest = 2.0f;
					float highest = -1.0f;
					bool allHit = true;
					bool allLeaveFarThroughTop = true;
					for (uint32_t pixel = 0; pixel < scale * scale; pixel++)
					{
						const glm::vec2 center(static_cast<float>(x * scale + pixel % scale) + 0.5f, static_cast<float>(y * scale + pixel / scale) + 0.5f);
						const PredictedRay predicted = PredictFloorRay(scene.Camera, c_FadeSize, center);
						allHit = allHit && predicted.HitsWall;
						allLeaveFarThroughTop = allLeaveFarThroughTop && predicted.LeavesThroughTop && predicted.Remaining > 0.1f * c_FadeSize + 1.0f;
						lowest = std::min(lowest, predicted.Confidence);
						highest = std::max(highest, predicted.Confidence);
					}
					if (!allHit)
						continue;
					const float confidence = ReadHalfTexel(image, x, y).a;
					const float side = static_cast<float>(x * scale);
					if (allLeaveFarThroughTop && (side < 0.1f * c_FadeSize || side >= 0.9f * c_FadeSize - static_cast<float>(scale)))
					{
						sideTexels++;
						sideWhole += confidence >= 0.99f ? 1u : 0u;
					}
					// Every seventh hit texel against the CPU's confidence.
					if ((x + y * image.Width) % 7 == 0)
					{
						compared++;
						agreeing += confidence >= lowest - 0.1f && confidence <= highest + 0.1f ? 1u : 0u;
					}
				}
			}
			CAPTURE(sideTexels);
			CAPTURE(sideWhole);
			CAPTURE(compared);
			CAPTURE(agreeing);
			CHECK(sideTexels >= 200u / (scale * scale));
			CHECK(sideWhole >= sideTexels - sideTexels / 100);
			CHECK(compared >= 64u);
			// Within 0.1, at 90% of them: near its exit the march places a hit up to 5 px past the exact crossing (the fade then
			// reads 0.1 lower), and Checkerboard's half-resolution march misses some hits there.
			CHECK(agreeing >= compared - compared / 10);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A strafe past a rough metal sheet and over a wet floor: the reflections do not flicker")
	{
		// The look-dev scene's specular lab (SpecularLab), strafed 1 cm a frame toward the rough sheet and across the wet plane, as
		// the person sees it (the 8-bit output) without anti-aliasing, with FXAA and with TAA, measured on that surface by the
		// wave's flicker meter. Blocks catches regions that pump; a screen-locked pattern sliding under moving content averages
		// out within a 32 px block, so the per-pixel Mean is what shows a crawl the reflections add.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		SpecularLab lab(assets);
		// The wet path's eye is a standing person's (1.6 m): the meter's depth tolerance (1% plus 1 cm) leaves out a grazing plane
		// where adjacent rows differ by more, from 4.5 m on at 0.5 m above it, from 14.7 m on at 1.6 m.
		const StrafePath paths[] = {
			{ "sheet", glm::vec3(16.0f, 1.6f, -3.6f), glm::vec3(0.0f, 1.5f, -6.0f), 50.0f, glm::vec3(0.0f, 0.0f, -0.01f), SpecularLab::GetSheetFace(), 0.9 },
			{ "wet", glm::vec3(-12.0f, 1.6f, 26.0f), glm::vec3(-13.0f, 0.5f, 8.0f), 45.0f, glm::vec3(0.01f, 0.0f, 0.0f), SpecularLab::GetWetPlane(), 0.5 },
		};
		const std::pair<StrafeAntiAliasing, std::string> antiAliasingModes[] = { { StrafeAntiAliasing::Off, "no AA" }, { StrafeAntiAliasing::FXAA, "FXAA" },
			{ StrafeAntiAliasing::TAA, "TAA" } };
		const std::pair<ReflectionsQuality, std::string> reflectionModes[] = { { ReflectionsQuality::Spatial, "Spatial" },
			{ ReflectionsQuality::SpatialHigh, "SpatialHigh" } };
		const auto report = [](const StrafePath& path, const std::string& antiAliasing, const std::string& reflections, const StrafeMeasure& measure)
		{
			const FlickerResult& flicker = measure.Flicker;
			MESSAGE(fmt::format("Strafe {}, {}, {}: Blocks {:.4f}%, BlocksP99 {:.4f}%, Mean {:.4f}%, P99 {:.4f}% over {} triples, {:.2f}% of the view "
				"measured ({:.2f}% its surface)", path.Name, antiAliasing, reflections, 100.0 * flicker.Blocks, 100.0 * flicker.BlocksP99, 100.0 * flicker.Mean,
				100.0 * flicker.P99, flicker.Frames, 100.0 * flicker.MeasuredShare, 100.0 * measure.SurfaceShare));
			CHECK(flicker.Frames == 23u);
			CHECK(std::isfinite(flicker.Blocks));
			CHECK(std::isfinite(flicker.BlocksP99));
			CHECK(std::isfinite(flicker.Mean));
			CHECK(std::isfinite(flicker.P99));
			CHECK(measure.SurfaceShare > 0.02);
			CHECK(flicker.MeasuredShare > path.MinimumMeasured * measure.SurfaceShare);
		};
		for (const StrafePath& path : paths)
		{
			CAPTURE(path.Name);
			for (const auto& antiAliasing : antiAliasingModes)
			{
				CAPTURE(antiAliasing.second);
				// Without screen-space reflections (the mode then traces nothing) the surface holds still: the floor every
				// mode's flicker is read against.
				const StrafeMeasure off = MeasureStrafe(gpu, lab, path, ReflectionsQuality::Spatial, antiAliasing.first, false);
				report(path, antiAliasing.second, "reflections off", off);
				CHECK(off.Flicker.Blocks <= 0.003);
				for (const auto& reflections : reflectionModes)
				{
					CAPTURE(reflections.second);
					report(path, antiAliasing.second, reflections.second, MeasureStrafe(gpu, lab, path, reflections.first, antiAliasing.first, true));
				}
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
