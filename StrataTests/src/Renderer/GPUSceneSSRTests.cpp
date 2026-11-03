#include "Renderer/ReflectionMetrics.h"
#include "Renderer/TAATestUtils.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <span>
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

	// A row of upright square posts standing on y = 0: their centers' x, their z, half width and height.
	struct PostRow
	{
		std::span<const float> X;
		float Z = 0.0f;
		float HalfWidth = 0.0f;
		float Height = 0.0f;
	};

	// How many posts of `posts` a reflected ray from `origin` passes behind before `reach` as a camera at `eye`, looking toward
	// `target`, sees them (by more than 5 cm): within a post's interval of angles in the horizontal plane, beyond its square, at a
	// height where the line of sight crosses the post.
	uint32_t CountPostsPassedBehind(const glm::vec3& eye, const glm::vec3& target, const PostRow& posts, const glm::vec3& origin, const glm::vec3& direction, float reach)
	{
		const glm::vec2 eyePoint(eye.x, eye.z);
		const glm::vec2 forward = glm::normalize(glm::vec2(target.x, target.z) - eyePoint);
		const auto cross2 = [](const glm::vec2& a, const glm::vec2& b) { return a.x * b.y - a.y * b.x; };
		const auto angleOf = [&](const glm::vec2& towards) { return std::atan2(cross2(forward, towards), glm::dot(forward, towards)); };
		const auto directionAt = [&](float angle) { return forward * std::cos(angle) + glm::vec2(-forward.y, forward.x) * std::sin(angle); };
		uint32_t count = 0;
		const glm::vec2 start(origin.x, origin.z);
		const glm::vec2 along(direction.x, direction.z);
		for (const float postX : posts.X)
		{
			float lowAngle = 1e9f;
			float highAngle = -1e9f;
			float farthest = 0.0f;
			const glm::vec2 corners[4] = { glm::vec2(postX - posts.HalfWidth, posts.Z - posts.HalfWidth), glm::vec2(postX + posts.HalfWidth, posts.Z - posts.HalfWidth),
				glm::vec2(postX - posts.HalfWidth, posts.Z + posts.HalfWidth), glm::vec2(postX + posts.HalfWidth, posts.Z + posts.HalfWidth) };
			for (const glm::vec2& corner : corners)
			{
				const float angle = angleOf(corner - eyePoint);
				lowAngle = std::min(lowAngle, angle);
				highAngle = std::max(highAngle, angle);
				farthest = std::max(farthest, glm::length(corner - eyePoint));
			}
			// The ray's distances where its point is seen at the interval's two angles (a line that misses the eye is seen at
			// monotonic angles).
			float bounds[2] = { 0.0f, 0.0f };
			bool solvable = true;
			for (int side = 0; side < 2; side++)
			{
				const glm::vec2 sight = directionAt(side == 0 ? lowAngle : highAngle);
				const float facing = cross2(sight, along);
				if (std::abs(facing) < 1e-6f)
					solvable = false;
				else
					bounds[side] = -cross2(sight, start - eyePoint) / facing;
			}
			if (!solvable)
				continue;
			const float enter = std::max(0.0f, std::min(bounds[0], bounds[1]));
			const float leave = std::min(reach, std::max(bounds[0], bounds[1]));
			if (!(enter < leave))
				continue;
			const glm::vec3 point = origin + direction * (0.5f * (enter + leave));
			const glm::vec2 seen = glm::vec2(point.x, point.z) - eyePoint;
			if (glm::dot(seen, forward) <= 0.0f || glm::length(seen) < farthest + 0.05f)
				continue;
			const float postDistance = glm::length(glm::vec2(postX, posts.Z) - eyePoint);
			const float sightHeight = eye.y + (point.y - eye.y) * postDistance / glm::length(seen);
			if (sightHeight > 0.05f && sightHeight < posts.Height - 0.05f)
				count++;
		}
		return count;
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
		std::vector<double> SurfaceLight; // Per moving frame: the displayed luminance summed over the surface's mask
	};

	// How much a strafe's light on the measured surface pumps from frame to frame beyond a steady drift: the mean |second
	// difference| of the frames' sums over their mean. A highlight that slides smoothly keeps its sum; one narrower than the
	// pixels it lands between brightens and dims as it moves (it shimmers).
	double GetLightPumping(const std::vector<double>& light)
	{
		REQUIRE(light.size() >= 3);
		double change = 0.0;
		double total = 0.0;
		for (size_t frame = 2; frame < light.size(); frame++)
			change += std::abs(light[frame] - 2.0 * light[frame - 1] + light[frame - 2]);
		for (const double value : light)
			total += value;
		const double mean = total / static_cast<double>(light.size());
		return mean > 0.0 ? change / static_cast<double>(light.size() - 2) / mean : 0.0;
	}

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
		std::vector<double> surfaceLight;
		for (uint32_t frame = 0; frame < c_StillFrames + c_MovingFrames; frame++)
		{
			const float moved = static_cast<float>(frame >= c_StillFrames ? frame - c_StillFrames + 1 : 0);
			camera = StrafeCamera(path.Position + path.Step * moved, path.Target + path.Step * moved, path.Fov);
			REQUIRE(renderer.Render(lab.Contents, camera));
			FlickerFrame read = ReadDisplayFlickerFrame(renderer, camera);
			const size_t kept = MaskFlickerFrame(read, path.Surface);
			if (frame > c_StillFrames)
				surfacePixels += kept;
			if (frame >= c_StillFrames)
			{
				double light = 0.0;
				for (size_t pixel = 0; pixel < read.Value.size(); pixel++)
					light += read.ViewDepth[pixel] > 0.0f ? read.Value[pixel] : 0.0f;
				surfaceLight.push_back(light);
			}
			sequence.Add(std::move(read));
			FinishFrame(gpu);
		}
		CHECK(renderer.GetStats().ScreenSpaceReflections == screenReflections);
		CHECK(renderer.GetStats().TemporalAntiAliasing == (antiAliasing == StrafeAntiAliasing::TAA));
		StrafeMeasure measure;
		measure.Flicker = sequence.GetResult();
		measure.SurfaceShare = static_cast<double>(surfacePixels) / (static_cast<double>(c_MovingFrames - 1) * c_StrafeWidth * c_StrafeHeight);
		measure.SurfaceLight = std::move(surfaceLight);
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

		// The face's mirror image in the floor, away from the screen's edges and up to 2 px from the face's own. Rays that reach
		// it near its top edge, last and in their longest steps (the bottom of its reflection), step from the wall onto the sky
		// beyond it: the crossing found at the previous step's pixel keeps them on the wall (a test of the step's pixel alone
		// left that edge ragged over 15% of the reflection's height).
		glm::vec2 low(1e9f);
		glm::vec2 high(-1e9f);
		for (const glm::vec3& corner : { glm::vec3(-3.0f, 0.0f, -3.9f), glm::vec3(3.0f, 0.0f, -3.9f), glm::vec3(-3.0f, -2.0f, -3.9f), glm::vec3(3.0f, -2.0f, -3.9f) })
		{
			const glm::vec2 pixel = glm::vec2(ProjectToPixel(scene.Camera, corner, c_WideSize));
			low = glm::min(low, pixel);
			high = glm::max(high, pixel);
		}
		const float margin = 0.12f * static_cast<float>(c_WideSize);
		const float raggedEdge = 2.0f;
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

	TEST_CASE("Ambient occlusion leaves a mirror's reflection alone")
	{
		// What a screen-space ray found is visible light: screen-space ambient occlusion (which sees what blocks the ray at the
		// screen's scale, as the trace did) must not darken it again, as it made dark smudges beside objects on wet ground. A
		// mirror floor under the red cube, now resting on it: the cube's reflection next to its base, where ambient occlusion
		// is below 0.8 (what GTAO reaches at 128 px), is the same with it on and off; beside the cube, where the rays leave the
		// screen and find nothing, the sky's reflection is still darker with it. A material's own occlusion map (crevices no
		// screen-space ray sees) still darkens the reflection: by Lagarde's factor of the map's 0.5.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		const auto render = [&](bool ambientOcclusion, AssetHandle occlusionMap, std::vector<glm::vec3>& hdr, ReadbackImage& occlusion, ReadbackImage& reflections,
			SceneCamera& camera)
		{
			MirrorScene scene(assets, 0.0f);
			TransformComponent& cube = scene.Cube.GetComponent<TransformComponent>();
			cube.Translation = glm::vec3(0.0f, 0.5f, 0.0f);
			if (occlusionMap)
			{
				MaterialProperties floor;
				floor.BaseColor = glm::vec4(1.0f);
				floor.Metallic = 1.0f;
				floor.Roughness = 0.0f;
				floor.OcclusionMap = occlusionMap;
				floor.OcclusionStrength = 1.0f;
				scene.Floor.GetComponent<MeshRendererComponent>().Material = assets.AddMaterial(floor);
			}
			scene.Camera = LookAt(glm::vec3(0.0f, 1.2f, 3.0f), glm::vec3(0.0f, 0.3f, 0.0f));
			camera = scene.Camera;
			SetAntiAliasingFor(scene.Contents, ReflectionsQuality::Spatial);
			GetPostProcess(scene.Contents).AmbientOcclusion = ambientOcclusion;
			SceneRenderer renderer = CreateReflectionRenderer("Occlusion", ReflectionsQuality::Spatial);
			renderer.SetViewportSize(c_ViewSize, c_ViewSize);
			WarmUp(gpu, renderer, scene.Contents, scene.Camera);
			RenderFrames(gpu, renderer, scene.Contents, scene.Camera, 3);
			REQUIRE(renderer.GetStats().ScreenSpaceReflections);
			hdr = ReadHDR(renderer.GetHDRTexture());
			if (ambientOcclusion)
			{
				REQUIRE(renderer.GetAmbientOcclusionTexture());
				REQUIRE(Renderer::ReadTexture(renderer.GetAmbientOcclusionTexture(), occlusion));
			}
			reflections = ReadHalfImage(renderer.GetScreenReflectionTexture());
		};
		std::vector<glm::vec3> off;
		std::vector<glm::vec3> on;
		ReadbackImage occlusion;
		ReadbackImage reflections;
		ReadbackImage unused;
		SceneCamera camera;
		render(false, UUID::Null(), off, unused, reflections, camera);
		render(true, UUID::Null(), on, occlusion, reflections, camera);
		REQUIRE(occlusion.Width == c_ViewSize);

		// The cube's front face (z = 0.5) mirrored in the floor, within 0.3 m of its base.
		uint32_t reflected = 0;
		for (float x = -0.4f; x <= 0.4f; x += 0.05f)
		{
			for (float height = 0.02f; height <= 0.3f; height += 0.02f)
			{
				const glm::uvec2 pixel = MirrorPixel(camera, glm::vec3(x, height, 0.5f));
				if (ReadOcclusionTexel(occlusion, pixel) >= 0.8f || ReadHalfTexel(reflections, pixel.x, pixel.y).a < 0.9f)
					continue;
				const glm::vec3 withOcclusion = TexelAt(on, pixel);
				const glm::vec3 without = TexelAt(off, pixel);
				CAPTURE(pixel);
				CAPTURE(withOcclusion);
				CAPTURE(without);
				CHECK(std::abs(withOcclusion.r - without.r) <= 0.01f * without.r);
				reflected++;
			}
		}
		CHECK(reflected >= 8u);
		// Beside the cube on the floor: its reflected rays leave the screen (no screen reflection), ambient occlusion still
		// darkens the sky's reflection there.
		uint32_t beside = 0;
		for (const glm::vec3& point : { glm::vec3(0.55f, 0.0f, 0.3f), glm::vec3(-0.55f, 0.0f, 0.3f), glm::vec3(0.55f, 0.0f, 0.0f), glm::vec3(-0.55f, 0.0f, 0.0f) })
		{
			const glm::uvec2 pixel = ProjectToPixel(camera, point, c_ViewSize);
			if (ReadHalfTexel(reflections, pixel.x, pixel.y).a > 0.0f || ReadOcclusionTexel(occlusion, pixel) >= 0.95f)
				continue;
			CAPTURE(pixel);
			CHECK(TexelAt(on, pixel).b < 0.99f * TexelAt(off, pixel).b);
			beside++;
		}
		CHECK(beside >= 1u);

		// The floor with a constant occlusion map of 0.5: its reflection of the cube is the plain floor's times Lagarde's factor.
		std::vector<uint8_t> half;
		for (int texel = 0; texel < 16; texel++)
			AppendPixel(half, 128, 128, 128, 255);
		const AssetHandle occlusionMap = assets.AddTexture(4, 4, half, TextureFormat::RGBA8);
		std::vector<glm::vec3> occluded;
		render(false, occlusionMap, occluded, unused, reflections, camera);
		const float occlusionValue = 128.0f / 255.0f;
		uint32_t compared = 0;
		for (float x = -0.4f; x <= 0.4f; x += 0.1f)
		{
			const glm::vec3 point(x, 0.25f, 0.5f);
			const glm::uvec2 pixel = MirrorPixel(camera, point);
			if (ReadHalfTexel(reflections, pixel.x, pixel.y).a < 0.99f)
				continue;
			// The floor point the pixel shows and its view: N.V of the mirror's upward normal.
			const glm::vec3 mirrored(point.x, -point.y, point.z);
			const float noV = glm::normalize(camera.Position - mirrored).y;
			const float roughness = 0.0f; // The mirror's, as the forward pass gives it to the occlusion
			const float factor = std::clamp(std::pow(noV + occlusionValue, std::exp2(-16.0f * roughness - 1.0f)) - 1.0f + occlusionValue, 0.0f, 1.0f);
			CAPTURE(pixel);
			CAPTURE(factor);
			CHECK(TexelAt(occluded, pixel).r == doctest::Approx(TexelAt(off, pixel).r * factor).epsilon(0.02));
			compared++;
		}
		CHECK(compared >= 4u);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Thin posts in front of a mirror leave no ghost columns")
	{
		// The specular audit's view along a vertical mirror at a grazing angle (about 6 degrees) with posts in front of it: the
		// reflected rays cross the screen sideways toward a blue wall and pass behind the posts' images on the way. A test that
		// took any point behind a surface by less than its step's change of distance for a hit made each post a ghost column
		// beside its reflection; a crossing must be validated (behind its own surface by no more than the tolerance, on one
		// continuous surface), so those rays go on to the wall. On the CPU each mirror texel's reflected ray is classified: it
		// meets a post (expected white), or it meets the wall after passing behind a post as the camera sees it (expected blue).
		// The posts are 20 cm wide: narrower ones fall between the march's steps whatever counts as a hit (of the rays that meet
		// a 5 cm post, a fifth find it).
		constexpr uint32_t c_PostsSize = 512;
		constexpr float c_PostX[5] = { -6.0f, -3.0f, 0.0f, 3.0f, 6.0f };
		constexpr float c_PostZ = 1.0f;
		constexpr float c_PostHalfWidth = 0.1f;
		constexpr float c_PostHeight = 2.0f;
		constexpr float c_WallX = -16.0f; // Facing +X, across the rays' way
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		// Where a ray from `origin` first meets a post's box grown by `grow` on every side within `reach` (slabs), -1 if nowhere.
		const auto meetsPost = [&](const glm::vec3& origin, const glm::vec3& direction, float reach, float grow)
		{
			float nearest = -1.0f;
			for (const float postX : c_PostX)
			{
				const glm::vec3 low(postX - c_PostHalfWidth - grow, -grow, c_PostZ - c_PostHalfWidth - grow);
				const glm::vec3 high(postX + c_PostHalfWidth + grow, c_PostHeight + grow, c_PostZ + c_PostHalfWidth + grow);
				float enter = 0.0f;
				float leave = reach;
				for (int axis = 0; axis < 3; axis++)
				{
					if (std::abs(direction[axis]) < 1e-8f)
					{
						if (origin[axis] < low[axis] || origin[axis] > high[axis])
							leave = -1.0f;
						continue;
					}
					const float first = (low[axis] - origin[axis]) / direction[axis];
					const float second = (high[axis] - origin[axis]) / direction[axis];
					enter = std::max(enter, std::min(first, second));
					leave = std::min(leave, std::max(first, second));
				}
				if (enter <= leave && (nearest < 0.0f || enter < nearest))
					nearest = enter;
			}
			return nearest;
		};
		for (const ReflectionsQuality reflections : { ReflectionsQuality::Spatial, ReflectionsQuality::SpatialHigh })
		{
			for (const bool fxaa : { true, false })
			{
				CAPTURE(static_cast<int>(reflections));
				CAPTURE(fxaa);
				Scene scene;
				PostProcessComponent& post = AddNeutralPostProcess(scene);
				post.ScreenSpaceReflections = true;
				post.AntiAliasingMethod = AntiAliasingMode::FXAA;
				post.AntiAliasing = fxaa;
				scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().EnvironmentMap =
					assets.Add(CreateEnvironmentTexture(glm::vec3(0.1f, 0.2f, 0.5f), glm::vec3(0.05f)), "Sky");
				MaterialProperties mirror;
				mirror.Metallic = 1.0f;
				mirror.Roughness = 0.0f;
				AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(mirror), glm::vec3(0.0f, 1.5f, -0.05f), "Mirror")
					.GetComponent<TransformComponent>().Scale = glm::vec3(20.0f, 3.0f, 0.1f);
				const AssetHandle white = assets.AddMaterial(UnlitColor({ 1.0f, 1.0f, 1.0f, 1.0f }));
				for (const float x : c_PostX)
				{
					AddMesh(scene, BuiltinAssets::CubeMesh, white, glm::vec3(x, 0.5f * c_PostHeight, c_PostZ), "Post").GetComponent<TransformComponent>().Scale =
						glm::vec3(2.0f * c_PostHalfWidth, c_PostHeight, 2.0f * c_PostHalfWidth);
				}
				AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(UnlitColor({ 0.05f, 0.1f, 1.0f, 1.0f })), glm::vec3(c_WallX - 0.1f, 1.5f, 0.0f), "Wall")
					.GetComponent<TransformComponent>().Scale = glm::vec3(0.2f, 12.0f, 60.0f);
				const glm::vec3 target(0.0f, 1.2f, 0.0f);
				const SceneCamera camera = LookAt(glm::vec3(14.0f, 1.2f, 1.4f), target);
				SceneRendererSpecification specification = ExactSpecification();
				specification.Quality.Reflections = reflections;
				specification.DebugName = "Posts";
				SceneRenderer renderer(specification);
				renderer.SetViewportSize(c_PostsSize, c_PostsSize);
				WarmUp(gpu, renderer, scene, camera);
				RenderFrames(gpu, renderer, scene, camera, 3);
				REQUIRE(renderer.GetStats().ScreenSpaceReflections);
				REQUIRE(renderer.GetTracedReflectionTexture());
				const ReadbackImage image = ReadHalfImage(renderer.GetTracedReflectionTexture());

				const PostRow posts { c_PostX, c_PostZ, c_PostHalfWidth, c_PostHeight };

				const glm::mat4 viewProjection = camera.Projection * camera.View;
				const glm::mat4 inverseViewProjection = glm::inverse(viewProjection);
				uint32_t postTexels = 0;
				uint32_t postWhite = 0;
				uint32_t ghostTexels = 0;
				uint32_t ghostBlue = 0;
				for (uint32_t y = 0; y < c_PostsSize; y++)
				{
					for (uint32_t x = 0; x < c_PostsSize; x++)
					{
						const glm::vec2 ndc(2.0f * (static_cast<float>(x) + 0.5f) / c_PostsSize - 1.0f, 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / c_PostsSize);
						const glm::vec4 nearPoint = inverseViewProjection * glm::vec4(ndc, 1.0f, 1.0f);
						const glm::vec3 view = glm::normalize(glm::vec3(nearPoint) / nearPoint.w - camera.Position);
						// The mirror's face (z = 0), seen past the posts (and 1 cm from their edges).
						if (!(view.z < -1e-4f))
							continue;
						const float mirrorReach = -camera.Position.z / view.z;
						const glm::vec3 mirrorPoint = camera.Position + view * mirrorReach;
						if (std::abs(mirrorPoint.x) > 9.5f || mirrorPoint.y < 0.1f || mirrorPoint.y > 2.9f || meetsPost(camera.Position, view, mirrorReach, 0.01f) >= 0.0f)
							continue;
						const glm::vec3 ray(view.x, view.y, -view.z);
						// Where it meets the wall, away from the screen's edges (no fade).
						if (!(ray.x < -1e-4f))
							continue;
						const float wallReach = (c_WallX - mirrorPoint.x) / ray.x;
						const glm::vec3 atWall = mirrorPoint + ray * wallReach;
						const glm::vec4 wallClip = viewProjection * glm::vec4(atWall, 1.0f);
						const glm::vec2 wallUV(wallClip.x / wallClip.w * 0.5f + 0.5f, 0.5f - wallClip.y / wallClip.w * 0.5f);
						const bool meetsWall = std::abs(atWall.z) < 29.0f && wallClip.w > 0.0f && wallUV.x > 0.15f && wallUV.x < 0.85f && wallUV.y > 0.15f && wallUV.y < 0.85f;
						const glm::vec4 texel = ReadHalfTexel(image, x, y);
						// Well inside a post's silhouette as the ray sees it (it meets the box shrunk by 2 cm), where the camera sees the
						// post 4 cm to either side (a face toward it, no other post in front: a sliver of a post seen beside a nearer one
						// is narrower than the march's steps), after passing behind at most one other post (one rejected candidate,
						// which every mode allows): the post's white.
						const float postReach = meetsPost(mirrorPoint, ray, wallReach, -0.02f);
						if (postReach >= 0.0f)
						{
							const glm::vec3 atPost = mirrorPoint + ray * meetsPost(mirrorPoint, ray, wallReach, 0.0f);
							bool hidden = CountPostsPassedBehind(camera.Position, target, posts, mirrorPoint, ray, postReach) > 1u;
							for (const float side : { -0.04f, 0.0f, 0.04f })
							{
								const glm::vec3 around = atPost + glm::vec3(0.0f, 0.0f, side);
								const float seen = glm::length(around - camera.Position);
								hidden = hidden || meetsPost(camera.Position, (around - camera.Position) / seen, seen - 0.01f, 0.0f) >= 0.0f;
							}
							if (hidden)
								continue;
							postTexels++;
							postWhite += texel.a > 0.5f && texel.r > 0.5f && texel.g > 0.5f && texel.b > 0.5f ? 1u : 0u;
							continue;
						}
						// Behind exactly one post on the way to the wall, 5 cm clear of every post: the wall's blue.
						if (!meetsWall || meetsPost(mirrorPoint, ray, wallReach, 0.05f) >= 0.0f || CountPostsPassedBehind(camera.Position, target, posts, mirrorPoint, ray, wallReach) != 1u)
							continue;
						ghostTexels++;
						ghostBlue += texel.a >= 0.9f && texel.b > 2.0f * texel.r && texel.b > 2.0f * texel.g ? 1u : 0u;
					}
				}
				CAPTURE(postTexels);
				CAPTURE(postWhite);
				CAPTURE(ghostTexels);
				CAPTURE(ghostBlue);
				MESSAGE("Posts: " << postWhite << " of " << postTexels << " white, " << ghostBlue << " of " << ghostTexels << " behind them blue");
				CHECK(postTexels >= 20u);
				CHECK(ghostTexels >= 50u);
				CHECK(postWhite >= postTexels - postTexels / 10);
				CHECK(ghostBlue >= ghostTexels - ghostTexels / 50);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Rays from the rough sheet that pass behind thin posts go on to the wall beyond them")
	{
		// lookdev.sheet's view of the specular lab at the editor's clip planes: the sheet's reflected rays cross the screen sideways
		// toward the yellow block, some of them behind the images of the 12 cm posts in front of it. The march's steps grow toward
		// the ray's far end: one step spans the last tens of meters. A ray whose last offset step fell short of the block, or
		// landed on a post's image (a candidate rejected, as it must be: the ray passes behind the post), found nothing, and so did
		// one whose next candidate's bisection closed in on that post again: the sky's reflection filled the ghost columns where
		// Strata 0.7.0 showed copies of the posts. On the CPU each sheet texel's reflected ray is followed to the block's face,
		// clear of everything else by 5 cm, where the camera sees it; the trace must find the block there. (The trace precedes the
		// anti-aliasing, so one run per mode covers no AA and FXAA. The posts' own reflections are not checked: 12 cm at 30 m is
		// narrower than the march's far steps, as DG "Reflections" says.)
		constexpr uint32_t c_Width = 1920;
		constexpr uint32_t c_Height = 1080;
		constexpr float c_Aspect = static_cast<float>(c_Width) / static_cast<float>(c_Height);
		constexpr float c_EditorFar = 2000.0f; // EditorCamera's far plane, as the look-dev captures render
		constexpr float c_PostX[6] = { -20.0f, -14.0f, -8.0f, 8.0f, 14.0f, 20.0f };
		constexpr float c_BlockFace = -44.0f; // The yellow block's face toward the lab (+X)
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		SpecularLab lab(assets);
		const glm::vec3 eye(16.0f, 1.6f, -3.6f);
		const glm::vec3 target(0.0f, 1.5f, -6.0f);
		SceneCamera camera = LookAt(eye, target, c_Aspect);
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(50.0f), c_Aspect, 0.1f, c_EditorFar);
		camera.VerticalFOV = glm::radians(50.0f);
		camera.Far = c_EditorFar;

		// What of the lab (SpecularLab) a ray or a line of sight can meet before the block: the posts, crates and the box beside
		// the sheet, and the ladder's spheres.
		struct Box
		{
			glm::vec3 Low;
			glm::vec3 High;
		};
		std::vector<Box> boxes;
		for (const float x : c_PostX)
			boxes.push_back({ glm::vec3(x - 0.06f, 0.0f, 8.94f), glm::vec3(x + 0.06f, 3.0f, 9.06f) });
		for (const glm::vec2& place : { glm::vec2(-17.0f, 13.0f), glm::vec2(-11.0f, 11.0f), glm::vec2(12.0f, 11.0f), glm::vec2(17.0f, 14.0f) })
			boxes.push_back({ glm::vec3(place.x - 0.5f, 0.0f, place.y - 0.5f), glm::vec3(place.x + 0.5f, 1.0f, place.y + 0.5f) });
		boxes.push_back({ glm::vec3(-14.0f, 0.2f, -7.25f), glm::vec3(-2.0f, 3.2f, -4.75f) });
		const auto blocked = [&boxes](const glm::vec3& origin, const glm::vec3& direction, float reach, float grow)
		{
			for (const Box& box : boxes)
			{
				float enter = 0.0f;
				float leave = reach;
				for (int axis = 0; axis < 3 && enter <= leave; axis++)
				{
					if (std::abs(direction[axis]) < 1e-8f)
					{
						if (origin[axis] < box.Low[axis] - grow || origin[axis] > box.High[axis] + grow)
							leave = -1.0f;
						continue;
					}
					const float first = (box.Low[axis] - grow - origin[axis]) / direction[axis];
					const float second = (box.High[axis] + grow - origin[axis]) / direction[axis];
					enter = std::max(enter, std::min(first, second));
					leave = std::min(leave, std::max(first, second));
				}
				if (enter <= leave)
					return true;
			}
			for (int index = 0; index < 8; index++)
			{
				const glm::vec3 offset = origin - glm::vec3(-7.0f + 2.0f * static_cast<float>(index), 0.5f, 1.0f);
				const float b = glm::dot(offset, direction);
				const float discriminant = b * b - (glm::dot(offset, offset) - (0.5f + grow) * (0.5f + grow));
				if (discriminant >= 0.0f && -b - std::sqrt(discriminant) < reach && -b + std::sqrt(discriminant) > 0.0f)
					return true;
			}
			return false;
		};

		// The texels whose ray meets the block, and whether it passes behind a post on the way as the camera sees it (one at most
		// here: one rejected candidate, which every mode allows).
		struct Expected
		{
			uint32_t X = 0;
			uint32_t Y = 0;
			bool BehindPost = false;
		};
		std::vector<Expected> expected;
		const glm::mat4 viewProjection = camera.Projection * camera.View;
		const glm::mat4 inverseViewProjection = glm::inverse(viewProjection);
		const PostRow posts { c_PostX, 9.0f, 0.06f, 3.0f };
		for (uint32_t y = 0; y < c_Height; y++)
		{
			for (uint32_t x = 0; x < c_Width; x++)
			{
				const glm::vec2 ndc(2.0f * (static_cast<float>(x) + 0.5f) / c_Width - 1.0f, 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / c_Height);
				const glm::vec4 nearPoint = inverseViewProjection * glm::vec4(ndc, 1.0f, 1.0f);
				const glm::vec3 view = glm::normalize(glm::vec3(nearPoint) / nearPoint.w - eye);
				// The sheet's face (z = -4.75), away from its edges.
				if (!(view.z < -1e-4f))
					continue;
				const glm::vec3 sheetPoint = eye + view * ((-4.75f - eye.z) / view.z);
				if (sheetPoint.x < 2.3f || sheetPoint.x > 13.7f || sheetPoint.y < 0.4f || sheetPoint.y > 3.0f)
					continue;
				const glm::vec3 ray(view.x, view.y, -view.z);
				if (!(ray.x < -1e-4f))
					continue;
				const float blockReach = (c_BlockFace - sheetPoint.x) / ray.x;
				const glm::vec3 atBlock = sheetPoint + ray * blockReach;
				if (atBlock.y < 0.3f || atBlock.y > 13.5f || std::abs(atBlock.z) > 29.5f || blocked(sheetPoint, ray, blockReach, 0.05f))
					continue;
				const float seen = glm::length(atBlock - eye);
				const glm::vec4 clip = viewProjection * glm::vec4(atBlock, 1.0f);
				const glm::vec2 uv(clip.x / clip.w * 0.5f + 0.5f, 0.5f - clip.y / clip.w * 0.5f);
				if (clip.w <= 0.0f || uv.x < 0.02f || uv.x > 0.98f || uv.y < 0.02f || uv.y > 0.98f || blocked(eye, (atBlock - eye) / seen, seen - 0.05f, 0.02f))
					continue;
				expected.push_back({ x, y, CountPostsPassedBehind(eye, target, posts, sheetPoint, ray, blockReach) > 0u });
			}
		}

		for (const ReflectionsQuality reflections : { ReflectionsQuality::Spatial, ReflectionsQuality::SpatialHigh })
		{
			CAPTURE(static_cast<int>(reflections));
			SceneRendererSpecification specification = ExactSpecification();
			specification.Quality.Reflections = reflections;
			specification.DebugName = "SheetPosts";
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(c_Width, c_Height);
			WarmUp(gpu, renderer, lab.Contents, camera);
			RenderFrames(gpu, renderer, lab.Contents, camera, 3);
			REQUIRE(renderer.GetStats().ScreenSpaceReflections);
			REQUIRE(renderer.GetTracedReflectionTexture());
			const ReadbackImage image = ReadHalfImage(renderer.GetTracedReflectionTexture());
			uint32_t texels[2] = {}; // Clear of the posts, behind a post
			uint32_t found[2] = {};
			for (const Expected& texel : expected)
			{
				// The block's yellow at full confidence, not the sky's blue.
				const glm::vec4 value = ReadHalfTexel(image, texel.X, texel.Y);
				const size_t group = texel.BehindPost ? 1 : 0;
				texels[group]++;
				found[group] += value.a >= 0.9f && value.r > 1.5f * value.b && value.g > 1.5f * value.b ? 1u : 0u;
			}
			MESSAGE(fmt::format("Sheet toward the block, {}: {} of {} clear rays and {} of {} behind a post find it",
				reflections == ReflectionsQuality::Spatial ? "Spatial" : "SpatialHigh", found[0], texels[0], found[1], texels[1]));
			CHECK(texels[0] >= 10000u);
			CHECK(texels[1] >= 200u);
			CHECK(found[0] >= texels[0] - texels[0] / 50);
			CHECK(found[1] >= texels[1] - texels[1] / 20);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A strafe past a rough metal sheet and over a wet floor: the reflections do not flicker")
	{
		// The look-dev scene's specular lab (SpecularLab), strafed 1 cm a frame toward the rough sheet and across the wet plane, as
		// the person sees it (the 8-bit output) without anti-aliasing, with FXAA and with TAA, measured on that surface by the
		// wave's flicker meter. Blocks catches regions that pump; a screen-locked pattern sliding under moving content averages
		// out within a 32 px block, so the per-pixel Mean is what shows a crawl the reflections add. Blocks stays within 1% in
		// every mode (0.7.0's too). What the reflections add to Mean (their Mean less the SSR-off run's) was, on 0.7.0, 1.36% on
		// the sheet at Spatial and 0.92% at SpatialHigh without AA (0.94% and 0.71% with TAA), 0.49% and 0.41% on the wet plane;
		// the resolve and the validated crossings took it to about 0.57% and 0.28% (0.63% and 0.40% with TAA). Half of 0.7.0's
		// holds only on the sheet at Spatial without TAA, so the bounds keep what was gained (1.25 times the measure), and 0.7.0
		// exceeds the sheet's.
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
		const double addedMeanBounds[] = { 0.008, 0.005 }; // The sheet's, the wet plane's
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
		for (size_t pathIndex = 0; pathIndex < std::size(paths); pathIndex++)
		{
			const StrafePath& path = paths[pathIndex];
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
					const StrafeMeasure on = MeasureStrafe(gpu, lab, path, reflections.first, antiAliasing.first, true);
					report(path, antiAliasing.second, reflections.second, on);
					CHECK(on.Flicker.Blocks <= 0.01);
					CHECK(on.Flicker.Mean - off.Flicker.Mean <= addedMeanBounds[pathIndex]);
				}
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Sized highlights shimmer no more than points in a strafe over wet ground")
	{
		// Highlights judged in motion: the sun's glint column on the wet plane (a 15-degree sun ahead, the camera 1.6 m above the
		// ground: the glint lies 6 m ahead, on the plane, clear of the posts' and crates' shadows and of the back wall's, which
		// covers the plane at lookdev.wet.sunward's 12 degrees) and the nearer lantern's glint at dusk (the sun off, the sky at
		// 1%; lookdev.lantern.wet's view), strafed 1 cm a frame sideways
		// at Spatial with no AA, FXAA and TAA, as the person sees them, each at an exposure that keeps the point glint's core below
		// white (everything else then shows black). The sized forms (the sun's 0.53-degree disk, lanterns of SourceRadius 0.06)
		// spread the same light wider than the point forms (a disk of 0, radius 0): a wider lobe covers more pixels and must not
		// shimmer more. A highlight slides over the surface as the camera moves, so the flicker meter, which follows world points,
		// measures the glint's own passing (its profile's curvature, larger for a wider glint over more pixels) as much as any
		// shimmer: it is reported. What is bounded is shimmer itself, the pumping of the glint's light from frame to frame
		// (GetLightPumping): a glint narrower than a pixel brightens and dims as it crosses pixel centers.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		SpecularLab lab(assets);
		struct HighlightPath
		{
			StrafePath Path;
			bool Dusk = false;
			float Exposure = 0.0f;
		};
		const HighlightPath paths[] = {
			{ { "sun glint", glm::vec3(-23.0f, 1.6f, 2.0f), glm::vec3(-23.0f, 0.3f, 30.0f), 55.0f, glm::vec3(0.01f, 0.0f, 0.0f), SpecularLab::GetWetPlane(), 0.5 },
				false, -15.0f },
			{ { "lanterns at dusk", glm::vec3(-13.0f, 1.0f, 20.0f), glm::vec3(-11.0f, 1.0f, 12.0f), 50.0f, glm::vec3(0.01f, 0.0f, 0.0f), SpecularLab::GetWetPlane(),
				0.5 }, true, -14.0f },
		};
		const std::pair<StrafeAntiAliasing, std::string> antiAliasingModes[] = { { StrafeAntiAliasing::Off, "no AA" }, { StrafeAntiAliasing::FXAA, "FXAA" },
			{ StrafeAntiAliasing::TAA, "TAA" } };
		DirectionalLightComponent& sun = lab.Sun.GetComponent<DirectionalLightComponent>();
		for (const HighlightPath& highlightPath : paths)
		{
			const StrafePath& path = highlightPath.Path;
			CAPTURE(path.Name);
			GetPostProcess(lab.Contents).Exposure = highlightPath.Exposure;
			lab.Sun.SetActive(!highlightPath.Dusk);
			lab.Sky.GetComponent<SkyLightComponent>().Intensity = highlightPath.Dusk ? 0.01f : 1.0f;
			lab.Sky.MarkModified<SkyLightComponent>();
			lab.Sun.GetComponent<TransformComponent>().Rotation = Math::LookRotation(SunDirection(15.0f, 90.0f));
			lab.Sun.MarkModified<TransformComponent>();
			for (Entity lantern : lab.Lanterns)
				lantern.SetActive(highlightPath.Dusk);
			const auto setSized = [&](bool sized)
			{
				sun.ShadowSoftness = sized ? 0.53f : 0.0f;
				lab.Sun.MarkModified<DirectionalLightComponent>();
				for (Entity lantern : lab.Lanterns)
				{
					lantern.GetComponent<PointLightComponent>().SourceRadius = sized ? 0.06f : 0.0f;
					lantern.MarkModified<PointLightComponent>();
				}
			};
			for (const auto& antiAliasing : antiAliasingModes)
			{
				CAPTURE(antiAliasing.second);
				setSized(false);
				const StrafeMeasure point = MeasureStrafe(gpu, lab, path, ReflectionsQuality::Spatial, antiAliasing.first, true);
				setSized(true);
				const StrafeMeasure sized = MeasureStrafe(gpu, lab, path, ReflectionsQuality::Spatial, antiAliasing.first, true);
				const double pointPumping = GetLightPumping(point.SurfaceLight);
				const double sizedPumping = GetLightPumping(sized.SurfaceLight);
				MESSAGE(fmt::format("Highlight strafe {}, {}: point pumping {:.4f}%, Mean {:.4f}%, Blocks {:.4f}%; sized pumping {:.4f}%, Mean {:.4f}%, "
					"Blocks {:.4f}%; light {:.1f} and {:.1f}; {:.2f}% of the view measured", path.Name, antiAliasing.second, 100.0 * pointPumping,
					100.0 * point.Flicker.Mean, 100.0 * point.Flicker.Blocks, 100.0 * sizedPumping, 100.0 * sized.Flicker.Mean, 100.0 * sized.Flicker.Blocks,
					point.SurfaceLight.front(), sized.SurfaceLight.front(), 100.0 * sized.Flicker.MeasuredShare));
				CHECK(point.Flicker.Frames == 23u);
				CHECK(sized.Flicker.MeasuredShare > path.MinimumMeasured * sized.SurfaceShare);
				CHECK(point.SurfaceLight.front() > 0.0); // The glint is in view and below white: what is measured is its light
				CHECK(sizedPumping <= pointPumping);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
