#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/PostProcessData.h"
#include "Strata/Renderer/PostProcessMath.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/RendererQuality.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

// Film grain and lateral chromatic aberration (PostProcess/Tonemap.frag): restrained flaws of film and lenses, off by default.
namespace
{

	// A wall of one display code (unlit, the neutral post-processing shows it unchanged) filling a square view.
	struct GrayWall
	{
		Scene SceneData;
		PostProcessComponent* PostProcess = nullptr;

		GrayWall(SceneTestAssets& assets, int code)
		{
			PostProcess = &AddNeutralPostProcess(SceneData);
			AddMesh(SceneData, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(PostProcessMath::SRGBToLinear(static_cast<float>(code) / 255.0f))),
				glm::vec3(0.0f), "Wall")
				.GetComponent<TransformComponent>().Scale = glm::vec3(40.0f, 40.0f, 1.0f);
		}
	};

	ReadbackImage RenderFrame(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera)
	{
		REQUIRE(renderer.Render(scene, camera));
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
		return image;
	}

	// The position, in pixels, where a channel crosses half of a rising step of linear light along row `y` near column `around`.
	float LinearCrossing(const ReadbackImage& image, uint32_t y, uint32_t around, int channel)
	{
		auto linear = [&](uint32_t x) { return PostProcessMath::SRGBToLinear(static_cast<float>(GetPixelRGBA8(image, x, y)[channel]) / 255.0f); };
		for (uint32_t x = around - 8; x < around + 8; x++)
		{
			const float a = linear(x);
			const float b = linear(x + 1);
			if (a < 0.5f && b >= 0.5f)
				return static_cast<float>(x) + (0.5f - a) / (b - a);
		}
		FAIL("no crossing near column ", around);
		return 0.0f;
	}

}

TEST_SUITE("GPU.SceneRenderer.GrainAndAberration")
{
	TEST_CASE("Film grain keeps the image's brightness, changes every frame, and is off for players who turn it off")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		GrayWall wall(assets, 128);
		constexpr uint32_t c_Size = 512;
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f));
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		const ReadbackImage plain = RenderFrame(renderer, wall.SceneData, camera);
		CHECK(Red(plain, c_Size / 2, c_Size / 2) == 128);

		wall.PostProcess->FilmGrain = 1.0f;
		const ReadbackImage first = RenderFrame(renderer, wall.SceneData, camera);
		const ReadbackImage second = RenderFrame(renderer, wall.SceneData, camera);
		double sum = 0.0;
		double squares = 0.0;
		uint32_t changed = 0;
		for (uint32_t y = 0; y < c_Size; y++)
		{
			for (uint32_t x = 0; x < c_Size; x++)
			{
				const glm::u8vec4 pixel = GetPixelRGBA8(first, x, y);
				// The same grain on every channel: luminance noise, no color speckle.
				CHECK(pixel.r == pixel.g);
				CHECK(pixel.g == pixel.b);
				const double value = static_cast<double>(pixel.r) - 128.0;
				sum += value;
				squares += value * value;
				changed += GetPixelRGBA8(second, x, y) != pixel ? 1u : 0u;
			}
		}
		const double count = static_cast<double>(c_Size) * c_Size;
		const double mean = sum / count;
		const double spread = std::sqrt(squares / count - mean * mean);
		MESSAGE("film grain 1 on code 128: mean ", mean, " codes off, spread ", spread, " codes");
		CHECK(std::abs(mean) <= 0.1);
		// RenderData::c_FilmGrainAmplitude of 255 codes in the mid-tones.
		CHECK(spread == doctest::Approx(RenderData::c_FilmGrainAmplitude * 255.0).epsilon(0.1));
		CHECK(changed > count / 2);
		// A seed keeps one pattern (single images): the same picture every time.
		SceneRenderOptions seeded;
		seeded.GrainSeed = 7;
		REQUIRE(renderer.Render(wall.SceneData, camera, nullptr, seeded));
		ReadbackImage still;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), still));
		REQUIRE(renderer.Render(wall.SceneData, camera, nullptr, seeded));
		ReadbackImage again;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), again));
		CHECK(MaxDifference(still, again) == 0);
		CHECK(MaxDifference(still, first) > 0);

		// The players' row: off renders as without grain.
		SceneRendererSpecification off = ExactSpecification();
		off.Quality.FilmGrain = SceneEffectChoice::Off;
		SceneRenderer withoutGrain(off);
		withoutGrain.SetViewportSize(c_Size, c_Size);
		CHECK(MaxDifference(RenderFrame(withoutGrain, wall.SceneData, camera), plain) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Chromatic aberration separates red and blue toward the corners, half a pixel at a 3840x2160 corner")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene("Edges");
		PostProcessComponent& postProcess = AddNeutralPostProcess(scene);
		constexpr uint32_t c_Width = 3840;
		constexpr uint32_t c_Height = 2160;
		const float aspect = static_cast<float>(c_Width) / static_cast<float>(c_Height);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f), aspect);
		// Black left of x = 0 and of x = 1.8 (near the right edge of the view), white between: rising steps at the center
		// and near the side.
		const float halfWidth = 2.0f * std::tan(glm::radians(30.0f)) * aspect;
		AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(1.0f)), glm::vec3(0.45f, 0.0f, 0.0f), "White")
			.GetComponent<TransformComponent>().Scale = glm::vec3(0.9f, 20.0f, 1.0f);
		AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(1.0f)), glm::vec3(0.5f * (1.8f + halfWidth + 1.0f), 0.0f, 0.0f), "Side")
			.GetComponent<TransformComponent>().Scale = glm::vec3(halfWidth + 1.0f - 1.8f, 20.0f, 1.0f);
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		const ReadbackImage sharp = RenderFrame(renderer, scene, camera);
		postProcess.ChromaticAberration = 1.0f;
		const ReadbackImage fringed = RenderFrame(renderer, scene, camera);
		const uint32_t row = c_Height / 2;
		const uint32_t center = c_Width / 2;
		const uint32_t side = ProjectToPixel(camera, glm::vec3(1.8f, 0.0f, 0.0f), c_Width, c_Height).x;
		REQUIRE(side > center + 1000);
		// At the center nothing moves.
		for (int channel = 0; channel < 3; channel++)
			CHECK(std::abs(LinearCrossing(fringed, row, center, channel) - LinearCrossing(sharp, row, center, channel)) <= 0.02f);
		// Near the side red is read k farther out and blue k nearer in, so red's edge shows k times the distance from the
		// center nearer it and blue's farther (0.5 pixel at the corner's 1920).
		const float expected = RenderData::c_ChromaticAberrationScale * (static_cast<float>(side) - 0.5f * c_Width);
		const float green = LinearCrossing(fringed, row, side, 1);
		CHECK(std::abs(green - LinearCrossing(sharp, row, side, 1)) <= 0.02f);
		const float red = LinearCrossing(fringed, row, side, 0) - green;
		const float blue = LinearCrossing(fringed, row, side, 2) - green;
		MESSAGE("at column ", side, ": red ", red, ", blue ", blue, " pixels from green; expected ", expected);
		CHECK(std::abs(red + expected) <= 0.1f);
		CHECK(std::abs(blue - expected) <= 0.1f);
		CHECK(expected <= 0.5f);
		CHECK(expected > 0.35f);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
