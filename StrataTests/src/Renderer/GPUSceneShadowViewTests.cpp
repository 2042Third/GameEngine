#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/ShadowView.h"

#include <array>
#include <cmath>
#include <optional>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_ViewSize = 256;

	// A 400 m floor under a sun, seen from 3 m above it, 20 degrees down with a 60 degree field of view: every row of the image
	// shows the floor at one view depth (or the sky), through all four cascades and beyond the shadow distance.
	struct ShadowViewScene
	{
		Scene SceneData;
		Entity Sun;
		SceneCamera Camera = PitchedCamera(glm::vec3(0.0f, 3.0f, 0.0f), 20.0f, 60.0f);

		ShadowViewScene()
		{
			AddNeutralPostProcess(SceneData);
			Entity floor = AddMesh(SceneData, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor");
			floor.GetComponent<TransformComponent>().Scale = glm::vec3(400.0f);
			Sun = AddSun(SceneData, SunDirection(50.0f, 30.0f), 3.0f);
		}

		// Where the floor of a row lies (world space), or nullopt for the sky.
		std::optional<glm::vec3> FloorPoint(uint32_t row) const
		{
			const float depth = FloorRowDepth(Camera, row, c_ViewSize);
			if (!(depth > 0.0f))
				return std::nullopt;
			const float ndcY = 1.0f - 2.0f * (static_cast<float>(row) + 0.5f) / static_cast<float>(c_ViewSize);
			const glm::vec3 viewRay(0.0f, ndcY * std::tan(Camera.VerticalFOV * 0.5f), -1.0f);
			const glm::vec3 point = Camera.Position + glm::mat3(glm::inverse(Camera.View)) * viewRay * depth;
			if (std::abs(point.x) > 200.0f || std::abs(point.z) > 200.0f)
				return std::nullopt;
			return point;
		}
	};

	SceneRendererSpecification ShadowViewSpecification(GPUDrivenMode mode)
	{
		SceneRendererSpecification specification;
		specification.GPUDriven = mode;
		specification.TextureStreaming = false;
		specification.Quality.ShadowCascades = 4;
		specification.Quality.ShadowResolution = ShadowMapResolution::Size2048;
		return specification;
	}

	// Renders a frame with the shadows view and waits for its counts.
	ReadbackImage RenderShadowView(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera)
	{
		SceneRenderOptions options;
		options.ShowShadowView = true;
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image)); // Waits for the GPU
		renderer.ResolveShadowViewStats();
		REQUIRE(renderer.GetShadowViewStats().Render == renderer.GetRenderCount());
		return image;
	}

	glm::ivec4 PixelAt(const ReadbackImage& image, uint32_t x, uint32_t y)
	{
		const uint8_t* pixel = image.Pixels.data() + (static_cast<size_t>(y) * image.Width + x) * image.BytesPerPixel;
		return glm::ivec4(pixel[0], pixel[1], pixel[2], pixel[3]);
	}

	// Whether two values of pixels per texel lie within one bin (an eighth of a stop) of each other.
	bool WithinOneBin(float a, float b)
	{
		return a > 0.0f && b > 0.0f && std::abs(std::log2(a / b)) <= 1.0f / static_cast<float>(RenderData::c_ShadowViewBinsPerStop) + 1e-4f;
	}

	bool DeviceDrawsGPUDriven()
	{
		return Renderer::GetGraphicsDevice().GetInfo().SupportsDrawIndirectFirstInstance;
	}

}

TEST_SUITE("GPU.SceneRenderer.ShadowView")
{
	TEST_CASE("The shadows view reports each cascade's share of the image and its pixels per texel")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		ShadowViewScene view;

		for (GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			if (mode == GPUDrivenMode::Auto && !DeviceDrawsGPUDriven())
				continue;
			CAPTURE(static_cast<int>(mode));
			SceneRenderer renderer(ShadowViewSpecification(mode));
			renderer.SetViewportSize(c_ViewSize, c_ViewSize);
			RenderShadowView(renderer, view.SceneData, view.Camera);
			const SceneRendererStats& frame = renderer.GetStats();
			const ShadowViewStats& stats = renderer.GetShadowViewStats();
			REQUIRE(frame.CascadeCount == 4);
			REQUIRE(stats.CascadeCount == 4);
			CHECK(stats.Cascades == frame.Cascades);
			CHECK(stats.ShadowMapSize == 2048);
			CHECK(stats.ViewportSize == glm::uvec2(c_ViewSize));
			CHECK(frame.Cascades[3].Far == doctest::Approx(80.0f)); // The light's shadow distance

			// The same histogram from the floor's rows: each row's view depth, its cascade by the splits, and a texel's world size
			// over a pixel's (2 d tan(fov / 2) / rows).
			std::array<std::array<uint32_t, RenderData::c_ShadowViewBins>, RenderData::c_MaxCascades> bins = {};
			uint32_t sky = 0;
			uint32_t uncovered = 0;
			for (uint32_t row = 0; row < c_ViewSize; row++)
			{
				if (!view.FloorPoint(row))
				{
					sky += c_ViewSize;
					continue;
				}
				const float depth = FloorRowDepth(view.Camera, row, c_ViewSize);
				uint32_t cascade = 0;
				while (cascade < frame.CascadeCount && !(depth < frame.Cascades[cascade].Far))
					cascade++;
				if (cascade == frame.CascadeCount)
				{
					uncovered += c_ViewSize;
					continue;
				}
				const float pixelWorld = 2.0f * depth * std::tan(view.Camera.VerticalFOV * 0.5f) / static_cast<float>(c_ViewSize);
				bins[cascade][ShadowView::GetBin(frame.Cascades[cascade].WorldPerTexel / pixelWorld)] += c_ViewSize;
			}

			const float imagePixels = static_cast<float>(c_ViewSize * c_ViewSize);
			CHECK(stats.SurfacePixels + stats.SkyPixels == c_ViewSize * c_ViewSize);
			CHECK(std::abs(static_cast<float>(stats.SkyPixels) - static_cast<float>(sky)) <= 0.005f * imagePixels);
			CHECK(std::abs(static_cast<float>(stats.UncoveredPixels) - static_cast<float>(uncovered)) <= 0.005f * imagePixels);
			for (uint32_t cascade = 0; cascade < frame.CascadeCount; cascade++)
			{
				CAPTURE(cascade);
				const ShadowViewRatioStats expected = ShadowView::Summarize(bins[cascade]);
				const ShadowViewRatioStats& measured = stats.PerCascade[cascade];
				REQUIRE(expected.Pixels > 0);
				CHECK(std::abs(static_cast<float>(measured.Pixels) - static_cast<float>(expected.Pixels)) <= 0.005f * imagePixels);
				CHECK(WithinOneBin(measured.MedianPixelsPerTexel, expected.MedianPixelsPerTexel));
				CHECK(WithinOneBin(measured.P10PixelsPerTexel, expected.P10PixelsPerTexel));
				CHECK(WithinOneBin(measured.P90PixelsPerTexel, expected.P90PixelsPerTexel));
			}
			CHECK(stats.Covered.Pixels == stats.SurfacePixels - stats.UncoveredPixels);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The shadows view changes nothing when it is off and allocates nothing")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		ShadowViewScene view;

		SceneRenderer plain(ShadowViewSpecification(GPUDrivenMode::Off));
		plain.SetViewportSize(c_ViewSize, c_ViewSize);
		const ReadbackImage reference = RenderToImage(plain, view.SceneData, view.Camera);
		CHECK_FALSE(plain.HasShadowViewResources());
		CHECK(plain.GetShadowViewStats().Render == 0);

		SceneRenderer shown(ShadowViewSpecification(GPUDrivenMode::Off));
		shown.SetViewportSize(c_ViewSize, c_ViewSize);
		const ReadbackImage withView = RenderShadowView(shown, view.SceneData, view.Camera);
		CHECK(shown.HasShadowViewResources());
		// Off again: the same pixels as a renderer that never showed it.
		const ReadbackImage after = RenderToImage(shown, view.SceneData, view.Camera);
		CHECK(after.Pixels == reference.Pixels);

		// Pixels without receivers: the sky (the top row) at 35%, the floor beyond the shadow distance at 35% under a gray veil.
		uint32_t farRow = c_ViewSize;
		for (uint32_t row = 0; row < c_ViewSize && farRow == c_ViewSize; row++)
		{
			if (view.FloorPoint(row) && FloorRowDepth(view.Camera, row, c_ViewSize) > 100.0f && FloorRowDepth(view.Camera, row, c_ViewSize) < 150.0f)
				farRow = row;
		}
		REQUIRE(farRow < c_ViewSize);
		for (uint32_t x : { 3u, c_ViewSize / 2, c_ViewSize - 3 })
		{
			const glm::ivec4 skyPlain = PixelAt(reference, x, 0);
			const glm::ivec4 skyView = PixelAt(withView, x, 0);
			const glm::ivec4 floorPlain = PixelAt(reference, x, farRow);
			const glm::ivec4 floorView = PixelAt(withView, x, farRow);
			for (int channel = 0; channel < 3; channel++)
			{
				CHECK(std::abs(skyView[channel] - static_cast<int>(std::lround(skyPlain[channel] * 0.35))) <= 1);
				CHECK(std::abs(floorView[channel] - static_cast<int>(std::lround(floorPlain[channel] * 0.35 + 0.25 * 0.65 * 255.0))) <= 1);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Without a shadowed sun the shadows view reports no cascades and veils the image")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		ShadowViewScene view;
		view.Sun.GetComponent<DirectionalLightComponent>().CastShadows = false;
		view.Sun.MarkModified<DirectionalLightComponent>();

		SceneRenderer renderer(ShadowViewSpecification(GPUDrivenMode::Off));
		renderer.SetViewportSize(c_ViewSize, c_ViewSize);
		const ReadbackImage reference = RenderToImage(renderer, view.SceneData, view.Camera);
		const ReadbackImage withView = RenderShadowView(renderer, view.SceneData, view.Camera);
		const ShadowViewStats& stats = renderer.GetShadowViewStats();
		CHECK(renderer.GetStats().CascadeCount == 0);
		CHECK(stats.CascadeCount == 0);
		CHECK(stats.Covered.Pixels == 0);
		CHECK(stats.SurfacePixels > 0);
		CHECK(stats.UncoveredPixels == stats.SurfacePixels);
		// A near floor row: no cascade covers it either.
		const uint32_t row = c_ViewSize - 2;
		REQUIRE(view.FloorPoint(row));
		const glm::ivec4 plain = PixelAt(reference, c_ViewSize / 2, row);
		const glm::ivec4 veiled = PixelAt(withView, c_ViewSize / 2, row);
		for (int channel = 0; channel < 3; channel++)
			CHECK(std::abs(veiled[channel] - static_cast<int>(std::lround(plain[channel] * 0.35 + 0.25 * 0.65 * 255.0))) <= 1);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
