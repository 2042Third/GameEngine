#include "Renderer/AmbientOcclusionTestUtils.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/AmbientOcclusionFit.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/RendererQuality.h"

#include <array>
#include <cmath>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr std::array<GPUDrivenMode, 2> c_Paths = { GPUDrivenMode::Off, GPUDrivenMode::Auto };
	constexpr std::array<AmbientOcclusionLevel, 4> c_Levels = { AmbientOcclusionLevel::Low, AmbientOcclusionLevel::Medium,
		AmbientOcclusionLevel::High, AmbientOcclusionLevel::Ultra };

	SceneRendererSpecification Specification(GPUDrivenMode path, AmbientOcclusionLevel level)
	{
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		specification.TemporalAntiAliasing = false;
		specification.GPUDriven = path;
		specification.Quality.AmbientOcclusionQuality = level;
		return specification;
	}

	// The renderer's ambient occlusion texture as floats.
	std::vector<float> ReadOcclusion(SceneRenderer& renderer)
	{
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetAmbientOcclusionTexture(), image));
		std::vector<float> values(static_cast<size_t>(image.Width) * image.Height);
		for (uint32_t y = 0; y < image.Height; y++)
		{
			for (uint32_t x = 0; x < image.Width; x++)
				values[static_cast<size_t>(y) * image.Width + x] = ReadOcclusionTexel(image, glm::uvec2(x, y));
		}
		return values;
	}

	float ReadHDRRed(SceneRenderer& renderer, const glm::uvec2& pixel)
	{
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), image));
		return ReadHalfTexel(image, pixel.x, pixel.y).r;
	}

	// A wall facing +Z with its front at z = 0, 10 m wide and 5 m tall, and a camera 8 m in front of it: far enough that the
	// radius's cap of 12% of the view's height (1.1 m there) keeps the 1 m default whole.
	struct WallScene
	{
		Scene SceneData;
		SceneCamera Camera;

		WallScene()
		{
			PostProcessComponent& postProcess = AddNeutralPostProcess(SceneData);
			postProcess.AmbientOcclusion = true;
			Entity wall = AddMesh(SceneData, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f, 2.5f, -0.5f), "Wall");
			wall.GetComponent<TransformComponent>().Scale = glm::vec3(10.0f, 5.0f, 1.0f);
			SceneData.CreateEntity("Sky").AddComponent<SkyLightComponent>().AmbientColor = glm::vec3(0.5f);
			Camera = LookAt(glm::vec3(0.0f, 1.5f, 8.0f), glm::vec3(0.0f, 1.5f, 0.0f));
		}
	};

}

TEST_SUITE("GPU.SceneRenderer.AmbientOcclusion")
{
	TEST_CASE("Ambient occlusion has no pattern without TAA at any quality")
	{
		// The 4x4 pattern of slice rotations, resolved within the frame. The camera faces the box's front squarely, so each row
		// of pixels on the floor in front of it lies at one distance from the box, where the occlusion is the same all along:
		// between 0.1 and 0.5 m in front of the box and within 0.3 m of its middle, the occlusion differs from its row's
		// straight-line fit (which takes the slight fall toward the box's ends) by at most 1% RMS, and from the mean of its two
		// neighbors in the row (what a pattern of pixels leaves) by at most 0.02% RMS: Strata 0.6.0's interleaved noise under
		// its blur left 0.11% at Medium (0.55% at Low's 1 x 4, 0.04% at High), the resolved pattern under 0.01%. With FXAA and
		// without anti-aliasing; and two frames of a still view are the same (nothing turns with the frame).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		CreaseScene crease;
		crease.Camera = LookAt(glm::vec3(0.0f, 1.2f, 2.2f), glm::vec3(0.0f, 0.0f, 0.5f));
		constexpr uint32_t c_Size = 512;
		const glm::uvec2 nearPixel = ProjectToPixel(crease.Camera, glm::vec3(0.0f, 0.0f, 0.6f), c_Size);
		const glm::uvec2 farPixel = ProjectToPixel(crease.Camera, glm::vec3(0.0f, 0.0f, 1.0f), c_Size);
		REQUIRE(farPixel.y > nearPixel.y + 10);
		for (const bool fxaa : { true, false })
		{
			crease.PostProcess->AntiAliasing = fxaa;
			crease.PostProcess->AntiAliasingMethod = AntiAliasingMode::FXAA;
			for (GPUDrivenMode path : c_Paths)
			{
				for (AmbientOcclusionLevel level : c_Levels)
				{
					CAPTURE(fxaa);
					CAPTURE(static_cast<int>(path));
					CAPTURE(static_cast<int>(level));
					SceneRenderer renderer(Specification(path, level));
					renderer.SetViewportSize(c_Size, c_Size);
					REQUIRE(renderer.Render(crease.SceneData, crease.Camera));
					const std::vector<float> first = ReadOcclusion(renderer);
					REQUIRE(renderer.Render(crease.SceneData, crease.Camera));
					const std::vector<float> second = ReadOcclusion(renderer);
					CHECK(first == second);

					double sum = 0.0;
					double patternSum = 0.0;
					uint32_t count = 0;
					for (uint32_t y = nearPixel.y + 2; y + 2 < farPixel.y; y++)
					{
						// The row's extent: the floor within 0.3 m of the box's middle at this row's distance.
						const float rowZ = 0.6f + 0.4f * static_cast<float>(y - nearPixel.y) / static_cast<float>(farPixel.y - nearPixel.y);
						const glm::uvec2 left = ProjectToPixel(crease.Camera, glm::vec3(-0.3f, 0.0f, rowZ), c_Size);
						const glm::uvec2 right = ProjectToPixel(crease.Camera, glm::vec3(0.3f, 0.0f, rowZ), c_Size);
						// Least squares line a + b x through the row, and the residue from it.
						double n = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
						for (uint32_t x = left.x; x <= right.x; x++)
						{
							const double value = first[static_cast<size_t>(y) * c_Size + x];
							n += 1.0;
							sx += x;
							sy += value;
							sxx += static_cast<double>(x) * x;
							sxy += x * value;
						}
						const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
						const double offset = (sy - slope * sx) / n;
						for (uint32_t x = left.x; x <= right.x; x++)
						{
							const double residue = first[static_cast<size_t>(y) * c_Size + x] - (offset + slope * x);
							sum += residue * residue;
							count++;
							const size_t at = static_cast<size_t>(y) * c_Size + x;
							const double curve = first[at] - 0.5 * (first[at - 1] + first[at + 1]);
							patternSum += curve * curve;
						}
					}
					REQUIRE(count > 1000u);
					const double rms = std::sqrt(sum / count);
					CAPTURE(rms);
					const double pattern = std::sqrt(patternSum / count);
					CAPTURE(pattern);
					CHECK(rms <= 0.01);
					CHECK(pattern <= 2e-4);
					// The floor there is occluded at all (the band is not empty).
					CHECK(first[static_cast<size_t>(nearPixel.y + 2) * c_Size + c_Size / 2] < 0.95f);
				}
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The quality row sets the samples")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		CreaseScene crease;
		constexpr std::array<uint32_t, 4> c_Samples = { 8, 24, 48, 96 };
		for (size_t index = 0; index < c_Levels.size(); index++)
		{
			CAPTURE(index);
			SceneRenderer renderer(Specification(GPUDrivenMode::Off, c_Levels[index]));
			renderer.SetViewportSize(64, 64);
			REQUIRE(renderer.Render(crease.SceneData, crease.Camera));
			CHECK(renderer.GetStats().AmbientOcclusionSamples == c_Samples[index]);
			// A quality change takes effect in the next frame and starts nothing again.
			RendererQualitySettings quality = renderer.GetQuality();
			quality.AmbientOcclusionQuality = c_Levels[(index + 1) % c_Levels.size()];
			CHECK_FALSE(renderer.SetQuality(quality));
			REQUIRE(renderer.Render(crease.SceneData, crease.Camera));
			CHECK(renderer.GetStats().AmbientOcclusionSamples == c_Samples[(index + 1) % c_Levels.size()]);
		}
		// SceneRendererSpecification's default keeps Strata 0.6.0's 2 x 6; without ambient occlusion nothing is sampled.
		SceneRenderer renderer;
		renderer.SetViewportSize(64, 64);
		REQUIRE(renderer.Render(crease.SceneData, crease.Camera));
		CHECK(renderer.GetStats().AmbientOcclusionSamples == 24u);
		crease.PostProcess->AmbientOcclusion = false;
		REQUIRE(renderer.Render(crease.SceneData, crease.Camera));
		CHECK(renderer.GetStats().AmbientOcclusionSamples == 0u);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The radius stays within its screen fractions")
	{
		// At 1024 rows the radius on screen lies between 3.1 and 123 pixels: a 1 m box seen from 1 m (where the 1 m radius
		// would span 900 pixels) darkens a band of the floor at most 123 pixels deep, and a 50 m box 2 km away (where it spans
		// half a pixel) still darkens the floor at its base.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		constexpr uint32_t c_Size = 1024;
		{
			CreaseScene crease;
			const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.6f, 1.5f), glm::vec3(0.0f, 0.0f, 0.5f));
			SceneRenderer renderer(Specification(GPUDrivenMode::Off, AmbientOcclusionLevel::Medium));
			renderer.SetViewportSize(c_Size, c_Size);
			REQUIRE(renderer.Render(crease.SceneData, camera));
			const std::vector<float> occlusion = ReadOcclusion(renderer);
			const glm::uvec2 base = ProjectToPixel(camera, glm::vec3(0.0f, 0.0f, 0.5f), c_Size);
			uint32_t band = 0;
			for (uint32_t y = base.y + 1; y < c_Size && occlusion[static_cast<size_t>(y) * c_Size + base.x] < 0.95f; y++)
				band++;
			CAPTURE(band);
			CHECK(band > 3u);
			CHECK(band <= 123u);
		}
		{
			Scene scene;
			AddNeutralPostProcess(scene).AmbientOcclusion = true;
			AddMesh(scene, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f, 0.0f, -2000.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(8000.0f);
			Entity box = AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f, 25.0f, -2000.0f), "Box");
			box.GetComponent<TransformComponent>().Scale = glm::vec3(50.0f);
			scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().AmbientColor = glm::vec3(0.5f);
			SceneCamera camera = LookAt(glm::vec3(0.0f, 60.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1975.0f));
			camera.Projection = Math::PerspectiveReverseZ(glm::radians(60.0f), 1.0f, 0.5f, 5000.0f);
			camera.Near = 0.5f;
			camera.Far = 5000.0f;
			SceneRenderer renderer(Specification(GPUDrivenMode::Off, AmbientOcclusionLevel::Medium));
			renderer.SetViewportSize(c_Size, c_Size);
			REQUIRE(renderer.Render(scene, camera));
			const std::vector<float> occlusion = ReadOcclusion(renderer);
			const glm::uvec2 base = ProjectToPixel(camera, glm::vec3(0.0f, 0.0f, -1975.0f), c_Size);
			float darkest = 1.0f;
			for (uint32_t y = base.y + 1; y <= base.y + 3; y++)
				darkest = std::min(darkest, occlusion[static_cast<size_t>(y) * c_Size + base.x]);
			CAPTURE(base);
			CHECK(darkest < 0.95f);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Thin occluders darken less than thick ones")
	{
		// A 2 cm pole 0.3 m in front of a wall is seen by few of a slice's samples, after which the horizon relaxes: the wall
		// beside its silhouette keeps most of its light. A board 1 m wide in its place (thick on screen: a slice's later samples
		// stay on it, high) keeps the wall darkened. Screen-space occlusion sees only what the view shows: a block 1 m deep
		// would occlude by its front face, 1.3 m from the wall, beyond the radius.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		constexpr uint32_t c_Size = 1024; // The pole is 2.6 pixels wide
		const auto wallBeside = [&](const glm::vec3& scale)
		{
			WallScene wall;
			Entity occluder = AddMesh(wall.SceneData, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f, 1.5f, 0.3f + scale.z * 0.5f), "Occluder");
			occluder.GetComponent<TransformComponent>().Scale = scale;
			SceneRenderer renderer(Specification(GPUDrivenMode::Off, AmbientOcclusionLevel::Medium));
			renderer.SetViewportSize(c_Size, c_Size);
			REQUIRE(renderer.Render(wall.SceneData, wall.Camera));
			const std::vector<float> occlusion = ReadOcclusion(renderer);
			// The wall a few pixels beside the occluder's silhouette, at its middle.
			const glm::uvec2 edge = ProjectToPixel(wall.Camera, glm::vec3(scale.x * 0.5f, 1.5f, 0.3f + scale.z), c_Size);
			return occlusion[static_cast<size_t>(edge.y) * c_Size + edge.x + 4];
		};
		const float thin = wallBeside(glm::vec3(0.02f, 3.0f, 0.02f));
		const float thick = wallBeside(glm::vec3(1.0f, 3.0f, 0.02f));
		CAPTURE(thin);
		CAPTURE(thick);
		CHECK(thin >= 0.9f);
		CHECK(thick < 0.85f);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Multi-bounce lifts occlusion by the fit on bright albedo")
	{
		// Under the constant ambient the crease's light with ambient occlusion over without is the occlusion the forward pass
		// applies: the fit's multi-bounce of the AO texture's value for the floor's albedo, or the value itself with
		// multi-bounce off.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		for (const float albedo : { 0.8f, 0.1f })
		{
			MaterialProperties matte;
			matte.BaseColor = glm::vec4(albedo, albedo, albedo, 1.0f);
			matte.Roughness = 1.0f;
			CreaseScene crease(assets.AddMaterial(matte));
			for (GPUDrivenMode path : c_Paths)
			{
				for (const bool multiBounce : { true, false })
				{
					CAPTURE(albedo);
					CAPTURE(static_cast<int>(path));
					CAPTURE(multiBounce);
					crease.PostProcess->AmbientOcclusionMultiBounce = multiBounce;
					SceneRenderer renderer(Specification(path, AmbientOcclusionLevel::Medium));
					renderer.SetViewportSize(128, 128);
					const glm::uvec2 pixel = ProjectToPixel(crease.Camera, crease.CreasePoint(), 128);
					crease.PostProcess->AmbientOcclusion = true;
					REQUIRE(renderer.Render(crease.SceneData, crease.Camera));
					const float occluded = ReadHDRRed(renderer, pixel);
					ReadbackImage texture;
					REQUIRE(Renderer::ReadTexture(renderer.GetAmbientOcclusionTexture(), texture));
					const float ao = ReadOcclusionTexel(texture, pixel);
					crease.PostProcess->AmbientOcclusion = false;
					REQUIRE(renderer.Render(crease.SceneData, crease.Camera));
					const float open = ReadHDRRed(renderer, pixel);
					REQUIRE(open > 0.0f);
					CAPTURE(ao);
					CHECK(ao < 0.8f);
					const float expected = multiBounce ? AOMultiBounce(ao, glm::vec3(albedo)).x : ao;
					CHECK(occluded / open == doctest::Approx(expected).epsilon(multiBounce ? 0.02 : 0.01));
				}
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
