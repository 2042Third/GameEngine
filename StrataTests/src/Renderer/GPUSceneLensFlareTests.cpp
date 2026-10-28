#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/PostProcessData.h"
#include "Strata/Renderer/RendererComponents.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace Strata;
using namespace Strata::Tests;

// Lens flares (PostProcess/LensFlare.comp, the tone mapping's set 1): ghosts and a halo of the bloom chain's bright light and
// the directional light's starburst, off by default.
namespace
{

	constexpr uint32_t c_Size = 256;

	ReadbackImage RenderImage(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera)
	{
		REQUIRE(renderer.Render(scene, camera));
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
		return image;
	}

	// The red code at a point of the image given in UV.
	int RedAt(const ReadbackImage& image, glm::vec2 uv)
	{
		return Red(image, static_cast<uint32_t>(uv.x * c_Size), static_cast<uint32_t>(uv.y * c_Size));
	}

	// A black view (orthographic, two units high) with one small, very bright unlit square, under the neutral
	// post-processing without bloom: what the flare adds is all that changes.
	struct BrightSource
	{
		Scene SceneData;
		PostProcessComponent* PostProcess = nullptr;
		SceneCamera Camera = OrthographicLookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f), 2.0f);

		BrightSource(SceneTestAssets& assets, glm::vec2 uv)
		{
			PostProcess = &AddNeutralPostProcess(SceneData);
			const glm::vec3 position(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f);
			AddMesh(SceneData, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(2000.0f)), position, "Source")
				.GetComponent<TransformComponent>().Scale = glm::vec3(0.08f, 0.08f, 1.0f);
		}
	};

	// What the flare added to each pixel's red code.
	std::vector<int> FlareOf(SceneRenderer& renderer, BrightSource& source)
	{
		source.PostProcess->LensFlare = 0.0f;
		const ReadbackImage plain = RenderImage(renderer, source.SceneData, source.Camera);
		source.PostProcess->LensFlare = 1.0f;
		const ReadbackImage flared = RenderImage(renderer, source.SceneData, source.Camera);
		std::vector<int> added(c_Size * c_Size);
		for (uint32_t y = 0; y < c_Size; y++)
		{
			for (uint32_t x = 0; x < c_Size; x++)
				added[y * c_Size + x] = Red(flared, x, y) - Red(plain, x, y);
		}
		return added;
	}

}

TEST_SUITE("GPU.SceneRenderer.LensFlare")
{
	TEST_CASE("Off, lens flares cost nothing and change nothing")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		BrightSource source(assets, glm::vec2(0.25f, 0.3f));
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		const ReadbackImage first = RenderImage(renderer, source.SceneData, source.Camera);
		CHECK_FALSE(renderer.GetStats().LensFlare);
		CHECK(renderer.GetStats().BloomLevels == 0);
		CHECK(renderer.GetLensFlareTexture() == nullptr);
		// Bloom off and LensFlare 0: the source alone, nothing around it.
		CHECK(RedAt(first, glm::vec2(0.6125f, 0.59f)) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Ghosts mirror a bright source through the image's center")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		BrightSource left(assets, glm::vec2(0.25f, 0.3f));
		const std::vector<int> added = FlareOf(renderer, left);
		CHECK(renderer.GetStats().LensFlare);
		CHECK_FALSE(renderer.GetStats().LensFlareSun);
		// The chain ran for the flare, but the bloom is off: no levels count.
		CHECK(renderer.GetStats().BloomLevels == 0);
		REQUIRE(renderer.GetLensFlareTexture());
		CHECK(renderer.GetLensFlareTexture()->getDesc().width == c_Size / 4);

		// The ghosts land at 0.5 + (0.5 - source) * k, faint (restraint: at most RenderData::c_LensFlareScale of the compressed
		// bright light), and nothing far from them.
		auto addedAt = [&](const std::vector<int>& image, glm::vec2 uv)
		{
			return image[static_cast<uint32_t>(uv.y * c_Size) * c_Size + static_cast<uint32_t>(uv.x * c_Size)];
		};
		for (const float scale : { 0.45f, 1.2f })
		{
			CAPTURE(scale);
			const int ghost = addedAt(added, 0.5f + (0.5f - glm::vec2(0.25f, 0.3f)) * scale);
			MESSAGE("the ghost of scale ", scale, " adds ", ghost, " codes of red at its center");
			CHECK(ghost >= 20);
			CHECK(ghost <= 200);
		}
		CHECK(std::abs(addedAt(added, glm::vec2(0.85f, 0.12f))) <= 1);

		// A source at the mirrored place flares as the mirror image.
		BrightSource right(assets, glm::vec2(0.75f, 0.7f));
		const std::vector<int> mirrored = FlareOf(renderer, right);
		int worst = 0;
		for (uint32_t y = 0; y < c_Size; y++)
		{
			for (uint32_t x = 0; x < c_Size; x++)
				worst = std::max(worst, std::abs(mirrored[y * c_Size + x] - added[(c_Size - 1 - y) * c_Size + (c_Size - 1 - x)]));
		}
		MESSAGE("the mirrored source's flare is within ", worst, " codes of the mirror image");
		CHECK(worst <= 2);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The sun's starburst shows only while the sky around its disk is seen")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		PostProcessComponent& postProcess = AddNeutralPostProcess(scene);
		// The light shines along -Z: its disk lies toward +Z, in the middle of a camera looking that way. A white card in its
		// light has a luminance of 1.
		scene.CreateEntity("Sun").AddComponent<DirectionalLightComponent>().Intensity = glm::pi<float>();
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, -3.0f), glm::vec3(0.0f));
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		const ReadbackImage plain = RenderImage(renderer, scene, camera);
		postProcess.LensFlare = 1.0f;
		const ReadbackImage burst = RenderImage(renderer, scene, camera);
		CHECK(renderer.GetStats().LensFlareSun);

		// Along a ray (straight right of the disk) the rays show; between two rays (30 degrees around) they do not.
		const glm::vec2 onRay(0.5f + 0.03f, 0.5f);
		const glm::vec2 betweenRays = glm::vec2(0.5f) + 0.03f * glm::vec2(std::cos(glm::radians(30.0f)), std::sin(glm::radians(30.0f)));
		CHECK(RedAt(plain, onRay) == 0);
		const int ray = RedAt(burst, onRay);
		MESSAGE("the starburst 0.03 image heights right of the disk: ", ray, " codes");
		CHECK(ray >= 40);
		CHECK(RedAt(burst, betweenRays) <= ray / 4);

		// A black wall in front of the disk: no starburst.
		AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(UnlitColor({ 0.0f, 0.0f, 0.0f, 1.0f })), glm::vec3(0.0f), "Wall")
			.GetComponent<TransformComponent>().Rotation = glm::angleAxis(glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
		const ReadbackImage covered = RenderImage(renderer, scene, camera);
		CHECK(RedAt(covered, onRay) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
