#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/RendererComponents.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

// The sun's shadow filter (Include/Shadows.slang; Docs/DeveloperGuide.md, "Sun shadows"): penumbrae at the physical width,
// identical in every frame, thin casters kept, narrow edges smooth, the cascades' depth ranges conservative, contacts close.

namespace
{

	// t10 of a uniform disk: the distance (in radii) from its center of the chord beyond which a tenth of its area lies, by
	// bisection of A(t) = (acos(t) - t sqrt(1 - t^2)) / pi = 0.1. A straight edge filtered by a disk of radius r falls from
	// 90% to 10% over 2 t10 r.
	double DiskTenPercentChord()
	{
		double low = 0.0;
		double high = 1.0;
		for (int iteration = 0; iteration < 60; iteration++)
		{
			const double t = 0.5 * (low + high);
			const double beyond = (std::acos(t) - t * std::sqrt(1.0 - t * t)) / 3.14159265358979323846;
			(beyond > 0.1 ? low : high) = t;
		}
		return 0.5 * (low + high);
	}

	// A plate of 4 x 0.05 x 4 m above a floor, its edge along z at x = 0, under a sun straight down; seen from 1 m above the
	// floor (below the plate) by an orthographic camera 1 m across, looking straight down at the edge's shadow.
	struct PlateScene
	{
		Scene SceneData;
		Entity Sun;
		Entity Plate;
		SceneCamera Camera = OrthographicLookAt(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f), 1.0f);

		explicit PlateScene(float height)
		{
			AddNeutralPostProcess(SceneData);
			AddMesh(SceneData, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(40.0f);
			Plate = AddMesh(SceneData, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(-2.0f, height, 0.0f), "Plate");
			Plate.GetComponent<TransformComponent>().Scale = glm::vec3(4.0f, 0.05f, 4.0f);
			Sun = AddSun(SceneData, glm::vec3(0.0f, -1.0f, 0.0f), 3.0f);
			Sun.GetComponent<DirectionalLightComponent>().ShadowDistance = 20.0f;
		}
	};

	SceneRendererSpecification OneCascade(uint32_t cascades, ShadowMapResolution resolution, GPUDrivenMode mode = GPUDrivenMode::Off)
	{
		SceneRendererSpecification specification;
		specification.GPUDriven = mode;
		specification.TextureStreaming = false;
		specification.Quality.ShadowCascades = cascades;
		specification.Quality.ShadowResolution = resolution;
		return specification;
	}

	// The sun's share across the image's columns, averaged over its rows (the edge runs along them: the disk's per-pixel
	// turn averages out).
	std::vector<double> ColumnProfile(const SunlightImage& image)
	{
		std::vector<double> profile(image.Size, 0.0);
		for (uint32_t x = 0; x < image.Size; x++)
		{
			for (uint32_t y = 0; y < image.Size; y++)
				profile[x] += image.At(x, y);
			profile[x] /= image.Size;
		}
		return profile;
	}

	// Where a rising profile first reaches a level, interpolated between pixels (in pixels from the left edge).
	double CrossingAt(const std::vector<double>& profile, double level)
	{
		for (size_t x = 1; x < profile.size(); x++)
		{
			if (profile[x - 1] < level && profile[x] >= level)
				return static_cast<double>(x - 1) + (level - profile[x - 1]) / (profile[x] - profile[x - 1]);
		}
		return -1.0;
	}

	// The 10-90% width of the edge in the image (pixels), from the profile's darkest to its brightest end.
	double EdgeWidth(const std::vector<double>& profile)
	{
		return CrossingAt(profile, 0.9) - CrossingAt(profile, 0.1);
	}

	// The share of a pixel's sun in the image of a renderer, for a square image of `size`.
	SunlightImage Sunlight(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera, Entity sun)
	{
		return RenderSunlight(renderer, scene, camera, sun);
	}

	bool DeviceDrawsGPUDriven()
	{
		return Renderer::GetGraphicsDevice().GetInfo().SupportsDrawIndirectFirstInstance;
	}

}

TEST_SUITE("GPU.SceneRenderer.ShadowFilter")
{
	TEST_CASE("A 0.53 degree sun's penumbra is as wide as the sun makes it")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const double t10 = DiskTenPercentChord();
		CHECK(t10 == doctest::Approx(0.6870).epsilon(1e-3));
		constexpr uint32_t size = 512; // Over 1 m

		for (const float height : { 4.0f, 6.0f })
		{
			CAPTURE(height);
			PlateScene plate(height);
			SceneRenderer renderer(OneCascade(1, ShadowMapResolution::Size4096));
			renderer.SetViewportSize(size, size);
			float texel = 0.0f;
			const auto widthAt = [&](float softness)
			{
				plate.Sun.GetComponent<DirectionalLightComponent>().ShadowSoftness = softness;
				plate.Sun.MarkModified<DirectionalLightComponent>();
				const SunlightImage sunlight = Sunlight(renderer, plate.SceneData, plate.Camera, plate.Sun);
				REQUIRE(sunlight.Stats.CascadeCount == 1);
				texel = sunlight.Stats.Cascades[0].WorldPerTexel;
				return EdgeWidth(ColumnProfile(sunlight)) / static_cast<double>(size); // Meters
			};
			const double width = widthAt(0.53f);
			// The edge spans at least three texels, so the filter, not the texels, makes its width.
			MESSAGE("Texel ", texel * 1000.0f, " mm; 10-90% width ", width * 1000.0, " mm at ", height, " m");
			CHECK(texel <= 0.008f);
			// The blockers' distance is from the plate's top to the floor.
			const double separation = static_cast<double>(height) + 0.025;
			const double expected = 2.0 * t10 * separation * std::tan(glm::radians(0.265));
			CHECK(std::abs(width / expected - 1.0) <= 0.2);
			// A degree: tan(0.5) / tan(0.265) = 1.89 times as wide.
			const double wider = widthAt(1.0f);
			const double ratio = std::tan(glm::radians(0.5)) / std::tan(glm::radians(0.265));
			CHECK(std::abs(wider / width / ratio - 1.0) <= 0.1);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A still frame's shadow does not change with the TAA frame")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		PlateScene plate(4.0f);
		plate.Sun.GetComponent<DirectionalLightComponent>().ShadowSoftness = 2.0f; // A wide penumbra: the disk's taps
		PostProcessComponent& postProcess = GetPostProcess(plate.SceneData);
		postProcess.AntiAliasing = true;
		postProcess.AntiAliasingMethod = AntiAliasingMode::TAA;
		postProcess.AmbientOcclusion = false;
		postProcess.ScreenSpaceReflections = false;

		SceneRenderer renderer(OneCascade(1, ShadowMapResolution::Size4096));
		renderer.SetViewportSize(256, 256);
		// Frames 1 and 9 of the history share the jitter's phase (c_TemporalJitterPhases) and differ in the frame's noise turn.
		std::vector<uint8_t> first;
		for (uint32_t frame = 1; frame <= SceneRenderer::c_TemporalJitterPhases + 1; frame++)
		{
			REQUIRE(renderer.Render(plate.SceneData, plate.Camera));
			REQUIRE(renderer.GetStats().TemporalFrames == frame);
			if (frame != 1 && frame != SceneRenderer::c_TemporalJitterPhases + 1)
				continue;
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), image));
			if (frame == 1)
				first = image.Pixels;
			else
				CHECK(image.Pixels == first);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A thin caster's shadow is never lost to the search")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(40.0f);
		// A wire of 1 x 1 cm along X, 4 m long, 3 m above the floor, under a 2 degree sun straight down.
		Entity wire = AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f, 3.0f, 0.0f), "Wire");
		wire.GetComponent<TransformComponent>().Scale = glm::vec3(4.0f, 0.01f, 0.01f);
		Entity sun = AddSun(scene, glm::vec3(0.0f, -1.0f, 0.0f), 3.0f);
		DirectionalLightComponent& light = sun.GetComponent<DirectionalLightComponent>();
		light.ShadowSoftness = 2.0f;
		light.ShadowDistance = 8.0f;

		// The wire is above the view (the floor under it is seen); a 40 degree field of view keeps the one cascade's texels
		// at most 6 mm, so the wire covers at least 1.6 texels and is never missed by the rasterization itself.
		constexpr uint32_t size = 512;
		SceneCamera camera = LookAt(glm::vec3(0.0f, 1.2f, 2.5f), glm::vec3(0.0f));
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(40.0f), 1.0f, 0.1f, 100.0f);
		camera.VerticalFOV = glm::radians(40.0f);

		for (GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			if (mode == GPUDrivenMode::Auto && !DeviceDrawsGPUDriven())
				continue;
			CAPTURE(static_cast<int>(mode));
			SceneRenderer renderer(OneCascade(1, ShadowMapResolution::Size2048, mode));
			renderer.SetViewportSize(size, size);
			const SunlightImage sunlight = Sunlight(renderer, scene, camera, sun);
			REQUIRE(sunlight.Stats.CascadeCount == 1);
			const float texel = sunlight.Stats.Cascades[0].WorldPerTexel;
			MESSAGE("Texel ", texel * 1000.0f, " mm");
			REQUIRE(texel <= 0.006f);

			// Along the shadow's centerline (the floor at z = 0), against a strip under a disk: 1 - (2/pi)(asin(u) + u sqrt(1 - u^2)),
			// u = w / (2 h tan(a)).
			const double u = 0.01 / (2.0 * 3.0 * std::tan(glm::radians(1.0)));
			const double expected = 1.0 - 2.0 / 3.14159265358979323846 * (std::asin(u) + u * std::sqrt(1.0 - u * u));
			double sum = 0.0;
			uint32_t samples = 0;
			for (float x = -1.5f; x <= 1.5f; x += 0.01f)
			{
				const glm::vec4 clip = camera.Projection * camera.View * glm::vec4(x, 0.0f, 0.0f, 1.0f);
				if (!(std::abs(clip.x) < 0.98f * clip.w && std::abs(clip.y) < 0.98f * clip.w))
					continue; // Off the image
				const glm::uvec2 pixel = ProjectToPixel(camera, glm::vec3(x, 0.0f, 0.0f), size);
				sum += sunlight.At(pixel.x, pixel.y);
				samples++;
			}
			REQUIRE(samples >= 150);
			const double mean = sum / samples;
			MESSAGE("Centerline mean ", mean, ", analytic ", expected);
			CHECK(std::abs(mean - expected) <= 0.03);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Narrow penumbrae are smooth and the same at every pixel")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		PlateScene plate(4.0f);
		plate.Sun.GetComponent<DirectionalLightComponent>().ShadowSoftness = 0.0f;
		constexpr uint32_t size = 512;
		SceneRenderer renderer(OneCascade(1, ShadowMapResolution::Size4096));
		renderer.SetViewportSize(size, size);
		const SunlightImage sunlight = Sunlight(renderer, plate.SceneData, plate.Camera, plate.Sun);
		REQUIRE(sunlight.Stats.CascadeCount == 1);
		const double pixelsPerTexel = sunlight.Stats.Cascades[0].WorldPerTexel * static_cast<double>(size);

		// Every row's profile rises monotonically (within a code of 8 bits) and is the same in every row: no dither.
		for (uint32_t y = 0; y < size; y += 7)
		{
			CAPTURE(y);
			for (uint32_t x = 1; x + 1 < size; x++)
			{
				const float left = sunlight.At(x - 1, y);
				const float right = sunlight.At(x + 1, y);
				const float value = sunlight.At(x, y);
				const bool within = value >= std::min(left, right) - 1.0f / 255.0f && value <= std::max(left, right) + 1.0f / 255.0f;
				if (!within)
				{
					CAPTURE(x);
					CHECK(within);
					break;
				}
			}
		}
		const double width = EdgeWidth(ColumnProfile(sunlight)) / pixelsPerTexel;
		MESSAGE("10-90% width ", width, " texels");
		CHECK(width >= 1.2);
		CHECK(width <= 2.2);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The cascades' depth ranges hold every texel's depth")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(200.0f);
		std::mt19937 random(11);
		std::uniform_real_distribution<float> place(-30.0f, 30.0f);
		std::uniform_real_distribution<float> height(0.2f, 6.0f);
		for (int index = 0; index < 60; index++)
		{
			Entity caster = AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(place(random), height(random), place(random) - 20.0f), "Caster");
			caster.GetComponent<TransformComponent>().Scale = glm::vec3(0.5f + 0.1f * static_cast<float>(index % 7));
		}
		AddSun(scene, SunDirection(50.0f, 30.0f), 3.0f);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 4.0f, 6.0f), glm::vec3(0.0f, 0.0f, -10.0f));

		// The ranges of a layer against its shadow map: each level's cell (of 4 << level texels) holds every texel's depth.
		const auto checkRanges = [](SceneRenderer& renderer, uint32_t layer)
		{
			CAPTURE(layer);
			ReadbackImage depth;
			REQUIRE(Renderer::ReadTexture(renderer.GetShadowMap(), depth, 0, layer));
			REQUIRE(depth.BytesPerPixel == 4);
			const uint32_t size = depth.Width;
			for (uint32_t level = 0; level < RenderData::c_ShadowRangeLevels; level++)
			{
				CAPTURE(level);
				ReadbackImage ranges;
				REQUIRE(Renderer::ReadTexture(renderer.GetShadowRanges(), ranges, level, layer));
				const uint32_t cell = RenderData::c_ShadowRangeCellTexels << level;
				REQUIRE(ranges.Width == size / cell);
				uint32_t outside = 0;
				for (uint32_t y = 0; y < size; y++)
				{
					for (uint32_t x = 0; x < size; x++)
					{
						float value = 0.0f;
						std::memcpy(&value, depth.Pixels.data() + (static_cast<size_t>(y) * size + x) * 4, sizeof(value));
						uint32_t packed = 0;
						std::memcpy(&packed, ranges.Pixels.data() + (static_cast<size_t>(y / cell) * ranges.Width + x / cell) * 4, sizeof(packed));
						const float nearest = static_cast<float>(packed >> 16) / 65535.0f;
						const float farthest = static_cast<float>(packed & 0xFFFFu) / 65535.0f;
						outside += value > nearest + 1e-7f || value < farthest - 1e-7f ? 1u : 0u;
					}
				}
				CHECK(outside == 0);
			}
		};

		for (GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			if (mode == GPUDrivenMode::Auto && !DeviceDrawsGPUDriven())
				continue;
			CAPTURE(static_cast<int>(mode));
			SceneRenderer renderer(OneCascade(4, ShadowMapResolution::Size1024, mode));
			renderer.SetViewportSize(128, 128);
			RenderToImage(renderer, scene, camera);
			REQUIRE(renderer.GetShadowRanges());
			for (uint32_t layer = 0; layer < 4; layer++)
				checkRanges(renderer, layer);
			if (mode == GPUDrivenMode::Auto)
			{
				// A still frame keeps every layer, with its ranges as they were.
				std::array<std::vector<uint8_t>, 4> before;
				for (uint32_t layer = 0; layer < 4; layer++)
				{
					ReadbackImage ranges;
					REQUIRE(Renderer::ReadTexture(renderer.GetShadowRanges(), ranges, 0, layer));
					before[layer] = ranges.Pixels;
				}
				RenderToImage(renderer, scene, camera);
				CHECK(renderer.GetStats().CachedCascadeMask == 0xFu);
				for (uint32_t layer = 0; layer < 4; layer++)
				{
					ReadbackImage ranges;
					REQUIRE(Renderer::ReadTexture(renderer.GetShadowRanges(), ranges, 0, layer));
					CHECK(ranges.Pixels == before[layer]);
				}
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Contacts recede at most 2.5 texels")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(40.0f);
		// A cube on the floor; the sun at 45 degrees from -X casts its shadow toward +X, from the cube's base edge at x = 0.5.
		AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f, 0.5f, 0.0f), "Cube");
		Entity sun = AddSun(scene, SunDirection(45.0f, 180.0f), 3.0f);
		sun.GetComponent<DirectionalLightComponent>().ShadowDistance = 20.0f;
		constexpr uint32_t size = 512;
		// Straight down over the shadow beside the cube's +X face: 1 m across.
		const SceneCamera camera = OrthographicLookAt(glm::vec3(0.75f, 5.0f, 0.0f), glm::vec3(0.75f, 0.0f, 0.0f), 1.0f);
		for (const ShadowMapResolution resolution : { ShadowMapResolution::Size1024, ShadowMapResolution::Size2048, ShadowMapResolution::Size4096 })
		{
			CAPTURE(static_cast<int>(resolution));
			SceneRenderer renderer(OneCascade(1, resolution));
			renderer.SetViewportSize(size, size);
			const SunlightImage sunlight = Sunlight(renderer, scene, camera, sun);
			REQUIRE(sunlight.Stats.CascadeCount == 1);
			const double texel = sunlight.Stats.Cascades[0].WorldPerTexel;
			// Along the row through the cube's middle, from its base edge outward: the first pixel in shadow (below half).
			const uint32_t row = size / 2;
			const uint32_t edge = ProjectToPixel(camera, glm::vec3(0.5f, 0.0f, 0.0f), size).x;
			uint32_t first = size;
			for (uint32_t x = edge; x < size; x++)
			{
				const float share = sunlight.At(x, row);
				if (share >= 0.0f && share < 0.5f)
				{
					first = x;
					break;
				}
			}
			REQUIRE(first < size);
			const double gap = static_cast<double>(first - edge) / static_cast<double>(size); // Meters
			MESSAGE("Texel ", texel * 1000.0, " mm, gap ", gap * 1000.0, " mm");
			CHECK(gap <= 2.5 * texel + 2.0 / static_cast<double>(size));
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
