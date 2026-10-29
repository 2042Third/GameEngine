#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/PostProcessMath.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/RendererQuality.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

// Morphological anti-aliasing (PostProcess/Morphological*.frag): edges, the area table's blend weights, the blend.
namespace
{

	constexpr uint32_t c_Size = 512;
	constexpr float c_WallDistance = 2.0f;

	SceneCamera Camera()
	{
		return LookAt(glm::vec3(0.0f, 0.0f, c_WallDistance), glm::vec3(0.0f));
	}

	// A black wall with a white half-plane over it whose edge passes through the view's center tilted `slope` (rise over run)
	// from the horizontal: the white side is below the edge.
	struct TiltedEdge
	{
		Scene SceneData;
		PostProcessComponent* PostProcess = nullptr;
		float Slope = 0.0f;

		TiltedEdge(SceneTestAssets& assets, float slope)
			: Slope(slope)
		{
			PostProcess = &AddNeutralPostProcess(SceneData);
			AddMesh(SceneData, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(0.0f)), glm::vec3(0.0f, 0.0f, -0.01f), "Black")
				.GetComponent<TransformComponent>().Scale = glm::vec3(40.0f, 40.0f, 1.0f);
			Entity white = AddMesh(SceneData, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(1.0f)), glm::vec3(0.0f), "White");
			TransformComponent& transform = white.GetComponent<TransformComponent>();
			transform.Scale = glm::vec3(40.0f, 20.0f, 1.0f);
			// Its top edge through the origin: centered 10 below it along the edge's normal.
			const float angle = std::atan(slope);
			transform.Rotation = glm::angleAxis(angle, glm::vec3(0.0f, 0.0f, 1.0f));
			transform.Translation = glm::vec3(10.0f * std::sin(angle), -10.0f * std::cos(angle), 0.0f);
			white.MarkModified<TransformComponent>();
		}

		// The share of pixel (x, y) below the edge (the white side), by 16 x 16 samples: the edge passes through the image's
		// center rising `Slope` per pixel to the right (image y grows downward).
		float Coverage(uint32_t x, uint32_t y) const
		{
			int inside = 0;
			for (int sy = 0; sy < 16; sy++)
			{
				for (int sx = 0; sx < 16; sx++)
				{
					const float px = static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / 16.0f - 0.5f * c_Size;
					const float py = static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / 16.0f - 0.5f * c_Size;
					inside += py > -Slope * px ? 1 : 0;
				}
			}
			return static_cast<float>(inside) / 256.0f;
		}
	};

	SceneRendererSpecification Specification(GPUDrivenMode path = GPUDrivenMode::Auto)
	{
		SceneRendererSpecification specification = ExactSpecification();
		specification.GPUDriven = path;
		return specification;
	}

	ReadbackImage RenderWith(SceneRenderer& renderer, Scene& scene, PostProcessComponent& postProcess, bool antiAliasing, AntiAliasingMode method)
	{
		postProcess.AntiAliasing = antiAliasing;
		postProcess.AntiAliasingMethod = method;
		renderer.SetViewportSize(c_Size, c_Size);
		REQUIRE(renderer.Render(scene, Camera()));
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
		return image;
	}

	// The mean absolute difference, in linear light, between the red channel and the edge's coverage over the pixels the edge
	// crosses.
	float CoverageError(const ReadbackImage& image, const TiltedEdge& edge)
	{
		double error = 0.0;
		int pixels = 0;
		for (uint32_t y = c_Size / 4; y < 3 * c_Size / 4; y++)
		{
			for (uint32_t x = c_Size / 4; x < 3 * c_Size / 4; x++)
			{
				const float coverage = edge.Coverage(x, y);
				if (coverage <= 0.01f || coverage >= 0.99f)
					continue;
				error += std::abs(PostProcessMath::SRGBToLinear(static_cast<float>(Red(image, x, y)) / 255.0f) - coverage);
				pixels++;
			}
		}
		REQUIRE(pixels > 100);
		return static_cast<float>(error / pixels);
	}

}

TEST_SUITE("GPU.SceneRenderer.Morphological")
{
	TEST_CASE("A tilted edge's pixels take the coverage of the line its staircase stands for")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		for (const float slope : { 1.0f / 7.0f, 1.0f / 3.0f, 3.0f })
		{
			TiltedEdge edge(assets, slope);
			SceneRenderer plain(Specification());
			SceneRenderer renderer(Specification());
			const ReadbackImage aliased = RenderWith(plain, edge.SceneData, *edge.PostProcess, false, AntiAliasingMode::FXAA);
			const ReadbackImage smoothed = RenderWith(renderer, edge.SceneData, *edge.PostProcess, true, AntiAliasingMode::Morphological);
			CHECK(renderer.GetStats().MorphologicalAntiAliasing);
			const float aliasedError = CoverageError(aliased, edge);
			const float smoothedError = CoverageError(smoothed, edge);
			MESSAGE("slope ", slope, ": mean coverage error ", smoothedError, " (", aliasedError, " without anti-aliasing)");
			// The reconstructed line runs through the middle of each run; the true one crosses the run anywhere within half a pixel
			// of it, so the error grows with the slope (a third of a pixel's height per pixel at 1:3): shallow edges (1:7, the
			// spec's measure) come within a tenth of their coverage, steeper ones nearer it than without anti-aliasing.
			if (slope < 0.2f)
				CHECK(smoothedError < 0.1f);
			CHECK(smoothedError < 0.6f * aliasedError);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Flat areas keep the tone mapping's values, as FXAA leaves them")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene("Flat");
		PostProcessComponent& postProcess = AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(0.3f)), glm::vec3(0.0f), "Wall")
			.GetComponent<TransformComponent>().Scale = glm::vec3(40.0f, 40.0f, 1.0f);
		SceneRenderer renderer(Specification());
		SceneRenderer other(Specification());
		const ReadbackImage morphological = RenderWith(renderer, scene, postProcess, true, AntiAliasingMode::Morphological);
		const ReadbackImage fxaa = RenderWith(other, scene, postProcess, true, AntiAliasingMode::FXAA);
		CHECK(MaxDifference(morphological, fxaa) == 0);
		ReadbackImage weights;
		REQUIRE(Renderer::ReadTexture(renderer.GetMorphologicalWeightsTexture(), weights));
		CHECK(std::all_of(weights.Pixels.begin(), weights.Pixels.end(), [](uint8_t value) { return value == 0; }));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Both draw paths anti-alias equally")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		TiltedEdge edge(assets, 1.0f / 5.0f);
		AddMesh(edge.SceneData, BuiltinAssets::CubeMesh, assets.AddMaterial(Emitting(0.5f)), glm::vec3(0.3f, 0.2f, 0.5f), "Box")
			.GetComponent<TransformComponent>().Scale = glm::vec3(0.2f);
		SceneRenderer cpuRenderer(Specification(GPUDrivenMode::Off));
		SceneRenderer gpuDriven(Specification(GPUDrivenMode::Auto));
		const ReadbackImage cpu = RenderWith(cpuRenderer, edge.SceneData, *edge.PostProcess, true, AntiAliasingMode::Morphological);
		const ReadbackImage driven = RenderWith(gpuDriven, edge.SceneData, *edge.PostProcess, true, AntiAliasingMode::Morphological);
		CHECK(gpuDriven.GetStats().GPUDriven);
		CHECK(MaxDifference(cpu, driven) <= 1);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Other methods make no morphological targets")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		TiltedEdge edge(assets, 0.5f);
		for (const bool antiAliasing : { false, true })
		{
			SceneRenderer renderer(Specification());
			RenderWith(renderer, edge.SceneData, *edge.PostProcess, antiAliasing, AntiAliasingMode::FXAA);
			CHECK_FALSE(renderer.GetStats().MorphologicalAntiAliasing);
			CHECK(renderer.GetMorphologicalWeightsTexture() == nullptr);
		}
		// The players' row chooses it whatever the scene's method.
		SceneRendererSpecification specification = Specification();
		specification.Quality.AntiAliasing = AntiAliasingChoice::Morphological;
		SceneRenderer chosen(specification);
		chosen.SetViewportSize(c_Size, c_Size);
		edge.PostProcess->AntiAliasing = false;
		REQUIRE(chosen.Render(edge.SceneData, Camera()));
		CHECK(chosen.GetStats().MorphologicalAntiAliasing);
		CHECK(chosen.GetMorphologicalWeightsTexture() != nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
