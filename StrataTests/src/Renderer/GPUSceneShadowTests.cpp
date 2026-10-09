#include "Renderer/SceneRendererTestUtils.h"

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	struct ShadowScene
	{
		Scene SceneData;
		Entity Sun;
		Entity Cube;

		ShadowScene()
		{
			AddNeutralPostProcess(SceneData);
			Entity floor = AddMesh(SceneData, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor");
			floor.GetComponent<TransformComponent>().Scale = glm::vec3(20.0f);
			Cube = AddMesh(SceneData, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f, 1.0f, 0.0f), "Cube");
			Sun = AddSun(SceneData, glm::vec3(0.0f, -1.0f, 0.0f), 3.0f); // Straight down
			Sun.GetComponent<DirectionalLightComponent>().ShadowSoftness = 0.0f;
		}
	};

	// Direction a sun travels at an elevation above the horizon (degrees), coming from the given azimuth (degrees
	// around +Y, 0 = from +X).
	glm::vec3 SunDirection(float elevation, float azimuth)
	{
		const float e = glm::radians(elevation);
		const float a = glm::radians(azimuth);
		return -glm::vec3(std::cos(e) * std::cos(a), std::sin(e), std::cos(e) * std::sin(a));
	}

}

TEST_SUITE("GPU.SceneRenderer.Shadows")
{
	TEST_CASE("Directional lights cast shadows from shadow-casting meshes")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		ShadowScene shadowScene;
		Scene& scene = shadowScene.SceneData;

		constexpr uint32_t size = 128;
		SceneRenderer renderer;
		renderer.SetViewportSize(size, size);
		SceneCamera camera = LookAt(glm::vec3(0.0f, 6.0f, 6.0f), glm::vec3(0.0f));
		const glm::uvec2 underCube = ProjectToPixel(camera, glm::vec3(0.0f, 0.0f, 0.3f), size);
		const glm::uvec2 openFloor = ProjectToPixel(camera, glm::vec3(2.5f, 0.0f, 0.3f), size);

		auto render = [&]()
		{
			const ReadbackImage image = RenderToImage(renderer, scene, camera);
			return std::pair { Red(image, underCube), Red(image, openFloor) };
		};

		const auto [shadowed, lit] = render();
		CHECK(lit > 150);
		CHECK(shadowed < lit / 3);
		CHECK(renderer.GetStats().ShadowCasters > 0);

		// The cube's top is lit: it is the closest caster to the light.
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
		CHECK(Red(image, ProjectToPixel(camera, glm::vec3(0.0f, 1.5f, 0.0f), size)) > 150);

		// Meshes that do not cast shadows, and lights without shadows, leave the floor lit.
		shadowScene.Cube.GetComponent<MeshRendererComponent>().CastShadows = false;
		const auto [noCaster, litNoCaster] = render();
		CHECK(noCaster == litNoCaster);
		shadowScene.Cube.GetComponent<MeshRendererComponent>().CastShadows = true;
		shadowScene.Sun.GetComponent<DirectionalLightComponent>().CastShadows = false;
		const auto [noShadows, litNoShadows] = render();
		CHECK(noShadows == litNoShadows);
		CHECK(renderer.GetStats().ShadowCasters == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A larger light gives a wider penumbra")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		ShadowScene shadowScene;
		Scene& scene = shadowScene.SceneData;
		shadowScene.Cube.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 3.0f, 0.0f); // Far from the floor
		// Sun at 45 degrees toward +X: the shadow lands beside the cube, around x = 3.
		shadowScene.Sun.GetComponent<TransformComponent>().Rotation = Math::LookRotation(glm::vec3(1.0f, -1.0f, 0.0f));

		constexpr uint32_t size = 128;
		SceneRenderer renderer;
		renderer.SetViewportSize(size, size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 8.0f, 1.0f), glm::vec3(0.0f)); // Looking down at the floor

		// Number of pixels along a row through the shadow edge that are neither fully lit nor fully shadowed.
		auto penumbraWidth = [&](float softness)
		{
			shadowScene.Sun.GetComponent<DirectionalLightComponent>().ShadowSoftness = softness;
			const ReadbackImage image = RenderToImage(renderer, scene, camera);
			const uint32_t row = ProjectToPixel(camera, glm::vec3(3.0f, 0.0f, 0.0f), size).y; // Through the shadow, crossing both edges
			int darkest = 255;
			int brightest = 0;
			for (uint32_t x = 0; x < size; x++)
			{
				darkest = std::min(darkest, Red(image, x, row));
				brightest = std::max(brightest, Red(image, x, row));
			}
			CHECK(darkest + 50 < brightest); // The row crosses the shadow
			uint32_t partial = 0;
			for (uint32_t x = 0; x < size; x++)
			{
				const int value = Red(image, x, row);
				partial += value > darkest + 10 && value < brightest - 10 ? 1u : 0u;
			}
			return partial;
		};

		const uint32_t hard = penumbraWidth(0.0f);
		const uint32_t soft = penumbraWidth(8.0f);
		CHECK(soft > hard + 2);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Surfaces lit at grazing angles show no shadow acne")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		MaterialProperties matte;
		matte.Roughness = 1.0f;
		const AssetHandle floorMaterial = assets.AddMaterial(matte);

		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::PlaneMesh, floorMaterial, glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(80.0f);
		Entity sun = AddSun(scene, glm::vec3(0.0f, -1.0f, 0.0f), 1.0f);

		constexpr uint32_t size = 128;
		SceneRenderer renderer;
		renderer.SetViewportSize(size, size);
		// Looking over the floor toward the horizon: the visible floor spans every cascade.
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 3.0f, 6.0f), glm::vec3(0.0f, 0.0f, -6.0f));

		// A plane cannot shadow itself: with shadows it must look exactly as without them.
		auto checkNoAcne = [&](float componentBias, float elevation, float azimuth)
		{
			CAPTURE(componentBias);
			CAPTURE(elevation);
			CAPTURE(azimuth);
			DirectionalLightComponent& light = sun.GetComponent<DirectionalLightComponent>();
			light.ShadowBias = componentBias;
			light.ShadowNormalBias = componentBias * 1.5f;
			light.Intensity = 2.0f / std::sin(glm::radians(elevation)); // Equally bright floors for every elevation
			sun.GetComponent<TransformComponent>().Rotation = Math::LookRotation(SunDirection(elevation, azimuth));

			light.CastShadows = true;
			const ReadbackImage shadowed = RenderToImage(renderer, scene, camera);
			CHECK(renderer.GetStats().ShadowCasters > 0);
			sun.GetComponent<DirectionalLightComponent>().CastShadows = false;
			const ReadbackImage reference = RenderToImage(renderer, scene, camera);

			uint32_t acnePixels = 0;
			int largestDifference = 0;
			for (uint32_t y = 0; y < size; y++)
			{
				for (uint32_t x = 0; x < size; x++)
				{
					const int difference = std::abs(Red(shadowed, x, y) - Red(reference, x, y));
					largestDifference = std::max(largestDifference, difference);
					acnePixels += difference > 2 ? 1u : 0u;
				}
			}
			CAPTURE(largestDifference);
			CHECK(acnePixels == 0);
			CHECK(Red(reference, size / 2, size - 1) > 100); // The floor is lit
		};

		// With the light's own depth and normal offsets, and with only the shadow pass's rasterizer depth bias.
		for (float componentBias : { 1.0f, 0.0f })
		{
			for (float elevation : { 3.0f, 8.0f, 20.0f })
			{
				for (float azimuth : { 0.0f, 35.0f, 90.0f })
					checkNoAcne(componentBias, elevation, azimuth);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Tall casters far toward a low sun still shadow the first cascade")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(400.0f);
		// A 30 m pillar 70 m toward the sun, which stands 10 degrees above the horizon in +X: its shadow reaches across
		// the camera's surroundings, while all of it is far outside the first cascade's bounds.
		Entity pillar = AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(70.0f, 15.0f, 0.0f), "Pillar");
		pillar.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f, 30.0f, 2.0f);
		Entity sun = AddSun(scene, SunDirection(10.0f, 0.0f), 10.0f);
		sun.GetComponent<DirectionalLightComponent>().ShadowSoftness = 0.0f;

		constexpr uint32_t size = 128;
		SceneRenderer renderer;
		renderer.SetViewportSize(size, size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.5f, 3.0f), glm::vec3(0.0f));
		const glm::uvec2 inShadow = ProjectToPixel(camera, glm::vec3(0.0f, 0.0f, 0.0f), size);   // View distance 3.4: first cascade
		const glm::uvec2 besideShadow = ProjectToPixel(camera, glm::vec3(0.0f, 0.0f, 1.6f), size); // Outside the 2 m wide strip

		const ReadbackImage image = RenderToImage(renderer, scene, camera);
		CHECK(Red(image, besideShadow) > 100);
		CHECK(Red(image, inShadow) < Red(image, besideShadow) / 3);

		// Moving the camera keeps the shadow (casters do not pop in and out between cascades).
		for (float offset : { 0.5f, 1.0f, 2.0f })
		{
			CAPTURE(offset);
			const SceneCamera moved = LookAt(glm::vec3(-offset, 1.5f, 3.0f), glm::vec3(-offset, 0.0f, 0.0f));
			const ReadbackImage movedImage = RenderToImage(renderer, scene, moved);
			CHECK(Red(movedImage, ProjectToPixel(moved, glm::vec3(-offset, 0.0f, 0.0f), size)) < Red(movedImage, ProjectToPixel(moved, glm::vec3(-offset, 0.0f, 1.6f), size)) / 3);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Shadows far from the camera come from the last cascade")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(400.0f);
		// A 6 m box floating 5 m above the floor, 40 m ahead; the sun at 45 degrees from +X puts its shadow at x = -14..-2.
		AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f, 8.0f, -40.0f), "Box").GetComponent<TransformComponent>().Scale = glm::vec3(6.0f);
		AddSun(scene, SunDirection(45.0f, 0.0f), 3.0f).GetComponent<DirectionalLightComponent>().ShadowSoftness = 0.0f;

		constexpr uint32_t size = 128;
		SceneRenderer renderer;
		renderer.SetViewportSize(size, size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 30.0f, 25.0f), glm::vec3(-8.0f, 0.0f, -40.0f));
		REQUIRE(glm::length(glm::vec3(-8.0f, 0.0f, -40.0f) - camera.Position) > 60.0f); // Beyond the third split (about 24 m)

		const ReadbackImage image = RenderToImage(renderer, scene, camera);
		const int shadowed = Red(image, ProjectToPixel(camera, glm::vec3(-8.0f, 0.0f, -40.0f), size));
		const int lit = Red(image, ProjectToPixel(camera, glm::vec3(8.0f, 0.0f, -40.0f), size));
		CHECK(lit > 150);
		CHECK(shadowed < lit / 3);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Alpha-masked surfaces are cut out and cast shadows with holes")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		// 2x2 checker: opaque texels on one diagonal, transparent ones on the other.
		const std::vector<uint8_t> checker = {
			255, 255, 255, 255,   255, 255, 255, 0,
			255, 255, 255, 0,     255, 255, 255, 255
		};
		MaterialProperties maskedProperties = UnlitColor({ 0.0f, 0.0f, 1.0f, 1.0f });
		maskedProperties.AlphaMode = MaterialAlphaMode::Mask;
		maskedProperties.BaseColorMap = assets.AddTexture(2, 2, checker, TextureFormat::RGBA8SRGB);
		const AssetHandle masked = assets.AddMaterial(maskedProperties);

		Scene scene;
		AddNeutralPostProcess(scene);
		MaterialProperties matte;
		matte.Roughness = 1.0f;
		AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(matte), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(40.0f);
		// A 4 m quad 4 m above the floor, facing up. Its cells: opaque at (x < 0, z < 0) and (x > 0, z > 0).
		Entity screen = AddMesh(scene, BuiltinAssets::QuadMesh, masked, glm::vec3(0.0f, 4.0f, 0.0f), "Screen");
		screen.GetComponent<TransformComponent>().Rotation = glm::angleAxis(glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
		screen.GetComponent<TransformComponent>().Scale = glm::vec3(4.0f);
		// The sun at 45 degrees from +X moves the shadow 4 m toward -X, beside the quad.
		AddSun(scene, SunDirection(45.0f, 0.0f), 3.0f).GetComponent<DirectionalLightComponent>().ShadowSoftness = 0.0f;

		constexpr uint32_t size = 128;
		SceneRenderer renderer;
		renderer.SetViewportSize(size, size);
		const SceneCamera camera = LookAt(glm::vec3(-2.0f, 12.0f, 0.0f), glm::vec3(-2.0f, 0.0f, 0.0f));
		const ReadbackImage image = RenderToImage(renderer, scene, camera);
		auto at = [&](const glm::vec3& point) { return GetPixelRGBA8(image, ProjectToPixel(camera, point, size).x, ProjectToPixel(camera, point, size).y); };

		// Opaque cells show the material, holes show the lit floor below.
		CHECK(at(glm::vec3(-1.0f, 4.0f, -1.0f)) == glm::u8vec4(0, 0, 255, 255));
		CHECK(at(glm::vec3(1.0f, 4.0f, 1.0f)) == glm::u8vec4(0, 0, 255, 255));
		const int litFloor = at(glm::vec3(1.0f, 4.0f, -1.0f)).r;
		CHECK(litFloor > 150);
		CHECK(std::abs(at(glm::vec3(-1.0f, 4.0f, 1.0f)).r - litFloor) <= 6);

		// The shadow has the same holes.
		CHECK(at(glm::vec3(-5.0f, 0.0f, -1.0f)).r < litFloor / 3);
		CHECK(at(glm::vec3(-3.0f, 0.0f, 1.0f)).r < litFloor / 3);
		CHECK(std::abs(at(glm::vec3(-3.0f, 0.0f, -1.0f)).r - litFloor) <= 6);
		CHECK(std::abs(at(glm::vec3(-5.0f, 0.0f, 1.0f)).r - litFloor) <= 6);

		// Picking ignores the holes too.
		const glm::uvec2 hole = ProjectToPixel(camera, glm::vec3(1.0f, 4.0f, -1.0f), size);
		const glm::uvec2 cell = ProjectToPixel(camera, glm::vec3(-1.0f, 4.0f, -1.0f), size);
		CHECK(renderer.GetEntityAt(scene, cell.x, cell.y) == screen);
		CHECK(renderer.GetEntityAt(scene, hole.x, hole.y) != screen);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
