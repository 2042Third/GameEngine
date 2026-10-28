#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/ColorLookup.h"
#include "Strata/Renderer/RendererComponents.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

// The Post Process's color lookup (PostProcess/Tonemap.frag): a 3D table applied to the display-encoded image after the tone
// curve, mixed in by its intensity, and nothing while it is not loaded.
namespace
{

	constexpr uint32_t c_Size = 64;

	// An unlit wall of 8 x 8 colors spread through the color cube (one texel of an 8x8 texture per 8x8 pixels), under the
	// neutral post-processing, which shows them as their texels.
	struct ColorWall
	{
		Scene SceneData;
		PostProcessComponent* PostProcess = nullptr;
		SceneCamera Camera = OrthographicLookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f), 1.0f);

		explicit ColorWall(SceneTestAssets& assets)
		{
			PostProcess = &AddNeutralPostProcess(SceneData);
			std::vector<uint8_t> pixels;
			for (uint32_t y = 0; y < 8; y++)
			{
				for (uint32_t x = 0; x < 8; x++)
				{
					auto level = [](uint32_t step) { return static_cast<uint8_t>(step * 255 / 7); };
					AppendPixel(pixels, level(x), level(y), level((x * 3 + y * 5) % 8), 255);
				}
			}
			MaterialProperties wall = UnlitColor({ 1.0f, 1.0f, 1.0f, 1.0f });
			wall.BaseColorMap = assets.AddTexture(8, 8, pixels, TextureFormat::RGBA8SRGB, TextureFilter::Nearest, TextureWrap::Clamp);
			AddMesh(SceneData, BuiltinAssets::QuadMesh, assets.AddMaterial(wall), glm::vec3(0.0f), "Wall");
		}
	};

	ReadbackImage RenderWall(SceneRenderer& renderer, ColorWall& wall)
	{
		REQUIRE(renderer.Render(wall.SceneData, wall.Camera));
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
		return image;
	}

	// A grade with a curve and cross-talk on every channel, at `size` points each way.
	std::vector<glm::vec3> MakeGrade(uint32_t size)
	{
		std::vector<glm::vec3> colors = ColorLookup::MakeIdentity(size);
		for (glm::vec3& c : colors)
			c = glm::clamp(glm::vec3(c.r * c.r * 0.8f + 0.1f * c.g, std::sqrt(c.g) * 0.9f + 0.05f, 0.6f * c.b + 0.3f * c.r * c.g), 0.0f, 1.0f);
		return colors;
	}

	// The largest difference, in codes, between the image and the CPU's lookup of the plain image's colors (trilinear, of the
	// half-float points the texture holds), mixed in by `intensity`.
	int LookupDifference(const ReadbackImage& plain, const ReadbackImage& graded, uint32_t size, const std::vector<glm::vec3>& grade, float intensity)
	{
		std::vector<glm::vec3> stored;
		for (const glm::vec3& color : grade)
			stored.emplace_back(glm::unpackHalf1x16(glm::packHalf1x16(color.r)), glm::unpackHalf1x16(glm::packHalf1x16(color.g)), glm::unpackHalf1x16(glm::packHalf1x16(color.b)));
		int worst = 0;
		for (uint32_t y = 0; y < c_Size; y++)
		{
			for (uint32_t x = 0; x < c_Size; x++)
			{
				const glm::vec3 encoded = glm::vec3(glm::vec4(GetPixelRGBA8(plain, x, y))) / 255.0f;
				const glm::vec3 expected = glm::clamp(glm::mix(encoded, ColorLookup::Sample(size, stored, encoded), intensity), 0.0f, 1.0f) * 255.0f;
				const glm::vec3 actual(glm::vec4(GetPixelRGBA8(graded, x, y)));
				for (int channel = 0; channel < 3; channel++)
					worst = std::max(worst, static_cast<int>(std::abs(std::round(expected[channel]) - actual[channel])));
			}
		}
		return worst;
	}

}

TEST_SUITE("GPU.SceneRenderer.ColorLookup")
{
	TEST_CASE("A color lookup maps the image as the CPU's trilinear lookup, mixed in by its intensity")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		ColorWall wall(assets);
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		const ReadbackImage plain = RenderWall(renderer, wall);
		CHECK_FALSE(renderer.GetStats().ColorLookup);

		const std::vector<glm::vec3> grade = MakeGrade(9);
		wall.PostProcess->ColorLookup = assets.Add(ColorLookup::Create(9, grade), "Grade");
		const ReadbackImage graded = RenderWall(renderer, wall);
		CHECK(renderer.GetStats().ColorLookup);
		CHECK(MaxDifference(plain, graded) > 40);
		const int full = LookupDifference(plain, graded, 9, grade, 1.0f);
		MESSAGE("a 9^3 grade: at most ", full, " codes from the CPU's lookup");
		CHECK(full <= 1);

		wall.PostProcess->ColorLookupIntensity = 0.5f;
		const ReadbackImage half = RenderWall(renderer, wall);
		CHECK(LookupDifference(plain, half, 9, grade, 0.5f) <= 1);

		// The identity changes nothing beyond the half floats' rounding (one code); a lookup set again is taken at once.
		wall.PostProcess->ColorLookupIntensity = 1.0f;
		wall.PostProcess->ColorLookup = assets.Add(ColorLookup::Create(33, ColorLookup::MakeIdentity(33)), "Identity");
		const ReadbackImage identity = RenderWall(renderer, wall);
		CHECK(renderer.GetStats().ColorLookup);
		CHECK(MaxDifference(plain, identity) <= 1);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Without a loaded lookup, or at intensity 0, the image is the plain one")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		ColorWall wall(assets);
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		const ReadbackImage plain = RenderWall(renderer, wall);

		// A handle no manager knows (an asset not loaded, or missing): nothing applies.
		wall.PostProcess->ColorLookup = UUID(0xC0105DEADull);
		CHECK(MaxDifference(plain, RenderWall(renderer, wall)) == 0);
		CHECK_FALSE(renderer.GetStats().ColorLookup);

		// A loaded lookup at intensity 0 is not even requested; at intensity 1 it shows; removed, the image is plain again.
		wall.PostProcess->ColorLookup = assets.Add(ColorLookup::Create(9, MakeGrade(9)), "Grade");
		wall.PostProcess->ColorLookupIntensity = 0.0f;
		CHECK(MaxDifference(plain, RenderWall(renderer, wall)) == 0);
		CHECK_FALSE(renderer.GetStats().ColorLookup);
		wall.PostProcess->ColorLookupIntensity = 1.0f;
		CHECK(MaxDifference(plain, RenderWall(renderer, wall)) > 40);
		wall.PostProcess->ColorLookup = UUID::Null();
		CHECK(MaxDifference(plain, RenderWall(renderer, wall)) == 0);
		CHECK_FALSE(renderer.GetStats().ColorLookup);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A lookup uploads once a renderer runs, and its memory moves to the GPU")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const Ref<ColorLookup> lookup = ColorLookup::Create(17, MakeGrade(17));
		REQUIRE(lookup);
		CHECK(lookup->GetFinalizedMemoryUsage() == AssetMemoryUsage { 0, 17ull * 17 * 17 * 8, 0 });
		assets.Add(lookup, "Grade");
		REQUIRE(lookup->GetTexture());
		CHECK(lookup->GetTexture()->getDesc().dimension == nvrhi::TextureDimension::Texture3D);
		CHECK(lookup->GetTexture()->getDesc().depth == 17);
		CHECK(lookup->GetMemoryUsage() == AssetMemoryUsage { 0, 17ull * 17 * 17 * 8, 0 });
		CHECK(lookup->GetColors().empty());
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
