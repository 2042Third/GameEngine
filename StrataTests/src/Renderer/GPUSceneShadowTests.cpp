#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/RendererComponents.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

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

	// A floor with pillars from close by to far away (casters in every cascade), and a large box far away that moves: the
	// cascades' squares cover the bounding spheres of their view slices, so near things are in every cascade, far ones only
	// in the last.
	struct CascadeCacheScene
	{
		Scene SceneData;
		Entity Sun;
		Entity Mover;

		explicit CascadeCacheScene(SceneTestAssets& assets)
		{
			AddNeutralPostProcess(SceneData);
			// A low sun: long shadows, which show where the far box stands.
			Sun = AddSun(SceneData, glm::normalize(glm::vec3(-0.8f, -0.6f, -0.1f)), 3.0f);
			Sun.GetComponent<DirectionalLightComponent>().ShadowDistance = 150.0f;
			MaterialProperties lit;
			lit.BaseColor = glm::vec4(0.7f, 0.7f, 0.75f, 1.0f);
			const AssetHandle material = assets.AddMaterial(lit);
			AddMesh(SceneData, BuiltinAssets::PlaneMesh, material, glm::vec3(0.0f, 0.0f, -60.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(200.0f);
			for (uint32_t index = 0; index < 30; index++)
			{
				const float distance = 2.0f + static_cast<float>(index * index) * 0.12f;
				Entity pillar = AddMesh(SceneData, BuiltinAssets::CubeMesh, material, glm::vec3(static_cast<float>(index % 2) * 4.0f - 2.0f, 1.0f, -distance), "Pillar");
				pillar.GetComponent<TransformComponent>().Scale = glm::vec3(0.6f + distance * 0.02f, 2.0f + distance * 0.05f, 0.6f);
			}
			Mover = AddMesh(SceneData, BuiltinAssets::CubeMesh, material, glm::vec3(4.0f, 2.0f, -80.0f), "Mover");
			Mover.GetComponent<TransformComponent>().Scale = glm::vec3(4.0f);
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

	// A camera at `position` looking along -Z, `pitch` degrees below the horizon, with a vertical field of view of `fov`
	// degrees: its rows see the floor (y = 0) each at one view depth.
	SceneCamera PitchedCamera(const glm::vec3& position, float pitch, float fov)
	{
		const float radians = glm::radians(pitch);
		SceneCamera camera = LookAt(position, position + glm::vec3(0.0f, -std::sin(radians), -std::cos(radians)));
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(fov), 1.0f, 0.1f, 500.0f);
		camera.VerticalFOV = glm::radians(fov);
		camera.Far = 500.0f;
		return camera;
	}

	// The view depth of the floor (y = 0) a row of a square image of the camera shows, or a negative value where it shows no
	// floor (along a row of a camera without roll, it does not change).
	float FloorRowDepth(const SceneCamera& camera, uint32_t row, uint32_t size)
	{
		const float ndcY = 1.0f - 2.0f * (static_cast<float>(row) + 0.5f) / static_cast<float>(size);
		const glm::vec3 viewRay(0.0f, ndcY * std::tan(camera.VerticalFOV * 0.5f), -1.0f); // One unit of view depth
		const glm::vec3 worldRay = glm::mat3(glm::inverse(camera.View)) * viewRay;
		if (!(worldRay.y < 0.0f))
			return -1.0f;
		return camera.Position.y / -worldRay.y;
	}

	// How much of the sun reaches each pixel's surface (1 lit, 0 in full shadow; -1 where the sun lights nothing), from the
	// HDR images (red) of the scene as it is, with the sun's shadows off (lit) and without the sun (dark): the scene's
	// lighting is linear in the sun's. Square images.
	struct SunlightImage
	{
		uint32_t Size = 0;
		std::vector<float> Fraction;

		float At(uint32_t x, uint32_t y) const { return Fraction[static_cast<size_t>(y) * Size + x]; }
	};

	SunlightImage RenderSunlight(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera, Entity sun)
	{
		const auto renderHDR = [&]()
		{
			REQUIRE(renderer.Render(scene, camera));
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), image));
			return image;
		};
		DirectionalLightComponent& light = sun.GetComponent<DirectionalLightComponent>();
		const ReadbackImage shadowed = renderHDR();
		const bool castShadows = light.CastShadows;
		light.CastShadows = false;
		sun.MarkModified<DirectionalLightComponent>();
		const ReadbackImage lit = renderHDR();
		const float intensity = light.Intensity;
		light.Intensity = 0.0f;
		sun.MarkModified<DirectionalLightComponent>();
		const ReadbackImage dark = renderHDR();
		light.CastShadows = castShadows;
		light.Intensity = intensity;
		sun.MarkModified<DirectionalLightComponent>();

		for (const ReadbackImage* image : { &shadowed, &lit, &dark })
		{
			REQUIRE(image->Format == nvrhi::Format::RGBA16_FLOAT);
			REQUIRE(image->Width == shadowed.Width);
			REQUIRE(image->Height == shadowed.Width);
		}
		const auto red = [](const ReadbackImage& image, size_t pixel)
		{
			uint16_t value = 0;
			std::memcpy(&value, image.Pixels.data() + pixel * image.BytesPerPixel, sizeof(value));
			return glm::unpackHalf1x16(value);
		};
		SunlightImage result;
		result.Size = shadowed.Width;
		result.Fraction.resize(static_cast<size_t>(shadowed.Width) * shadowed.Height);
		for (size_t pixel = 0; pixel < result.Fraction.size(); pixel++)
		{
			const float direct = red(lit, pixel) - red(dark, pixel);
			const float reaching = red(shadowed, pixel) - red(dark, pixel);
			result.Fraction[pixel] = direct > 1e-3f ? reaching / direct : -1.0f;
		}
		return result;
	}

	// A floor under a roof that shadows all of it, with a sun from +X 45 degrees above the horizon and shadows to 50 m: the
	// splits of its four cascades, a blend of 0.8 logarithmic and 0.2 uniform splits from the near clip of 0.1 m, lie at
	// 2.89, 6.80, 15.96 and 50 m.
	struct RoofScene
	{
		Scene SceneData;
		Entity Sun;

		explicit RoofScene(SceneTestAssets& assets)
		{
			AddNeutralPostProcess(SceneData);
			MaterialProperties matte;
			matte.Roughness = 1.0f;
			const AssetHandle material = assets.AddMaterial(matte);
			AddMesh(SceneData, BuiltinAssets::PlaneMesh, material, glm::vec3(0.0f, 0.0f, -100.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(400.0f);
			Entity roof = AddMesh(SceneData, BuiltinAssets::CubeMesh, material, glm::vec3(0.0f, 30.5f, -100.0f), "Roof");
			roof.GetComponent<TransformComponent>().Scale = glm::vec3(400.0f, 1.0f, 400.0f);
			Sun = AddSun(SceneData, SunDirection(45.0f, 0.0f), 3.0f);
			Sun.GetComponent<DirectionalLightComponent>().ShadowDistance = 50.0f;
		}
	};

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
		CHECK(GetDrawnCounts(renderer).ShadowCasters > 0);

		// The cube's top is lit: it is the closest caster to the light.
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
		CHECK(Red(image, ProjectToPixel(camera, glm::vec3(0.0f, 1.5f, 0.0f), size)) > 150);

		// Meshes that do not cast shadows, and lights without shadows, leave the floor lit.
		shadowScene.Cube.GetComponent<MeshRendererComponent>().CastShadows = false;
		shadowScene.Cube.MarkModified<MeshRendererComponent>();
		const auto [noCaster, litNoCaster] = render();
		CHECK(noCaster == litNoCaster);
		shadowScene.Cube.GetComponent<MeshRendererComponent>().CastShadows = true;
		shadowScene.Cube.MarkModified<MeshRendererComponent>();
		shadowScene.Sun.GetComponent<DirectionalLightComponent>().CastShadows = false;
		const auto [noShadows, litNoShadows] = render();
		CHECK(noShadows == litNoShadows);
		CHECK(GetDrawnCounts(renderer).ShadowCasters == 0);
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
			sun.MarkModified<TransformComponent>();

			light.CastShadows = true;
			const ReadbackImage shadowed = RenderToImage(renderer, scene, camera);
			CHECK(GetDrawnCounts(renderer).ShadowCasters > 0);
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

	TEST_CASE("Casters smaller than a texel of a cascade are left out of it")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(100.0f);
		AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f, 1.0f, 0.0f), "Cube");
		// A grain of a centimeter: the cascades of a kilometer of shadows have texels of 5 cm and more.
		Entity grain = AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(2.0f, 0.5f, 0.0f), "Grain");
		grain.GetComponent<TransformComponent>().Scale = glm::vec3(0.01f);
		Entity sun = AddSun(scene, SunDirection(60.0f, 30.0f), 3.0f);
		sun.GetComponent<DirectionalLightComponent>().ShadowDistance = 1000.0f;

		constexpr uint32_t size = 64;
		SceneRenderer renderer;
		renderer.SetViewportSize(size, size);
		SceneCamera camera = LookAt(glm::vec3(0.0f, 8.0f, 8.0f), glm::vec3(0.0f));
		camera.Far = 2000.0f;
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(60.0f), 1.0f, 0.1f, 2000.0f);
		REQUIRE(renderer.Render(scene, camera));
		const DrawnCounts withGrain = GetDrawnCounts(renderer);
		const uint32_t subTexel = withGrain.SubTexelCasters;
		CHECK(subTexel >= 1);
		CHECK(withGrain.ShadowCasters > 0); // The floor and the cube

		// Grown to a metre, it casts into every cascade it overlaps, and nothing is left out.
		grain.GetComponent<TransformComponent>().Scale = glm::vec3(1.0f);
		grain.MarkModified<TransformComponent>();
		REQUIRE(renderer.Render(scene, camera));
		const DrawnCounts grown = GetDrawnCounts(renderer);
		CHECK(grown.SubTexelCasters == 0);
		CHECK(grown.ShadowCasters == withGrain.ShadowCasters + subTexel);
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

	TEST_CASE("Cascades are kept while nothing reaches them, and redrawn when a caster in them moves, the camera or the light")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!Renderer::GetGraphicsDevice().GetInfo().SupportsDrawIndirectFirstInstance)
			return; // Caching is the GPU-driven path's
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::PlaneMesh));
		CascadeCacheScene cacheScene(assets);
		Scene& scene = cacheScene.SceneData;
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		SceneRenderer cached(specification);
		specification.CacheShadowCascades = false;
		SceneRenderer redrawn(specification);
		constexpr uint32_t size = 128;
		cached.SetViewportSize(size, size);
		redrawn.SetViewportSize(size, size);
		SceneCamera camera = LookAt(glm::vec3(0.0f, 4.0f, 6.0f), glm::vec3(0.0f, 1.0f, -20.0f));
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(60.0f), 1.0f, 0.1f, 500.0f);
		camera.Far = 500.0f;
		const uint32_t allCascades = (1u << specification.ShadowCascades) - 1u;

		// A frame of each: the same image whatever the cached renderer kept; the cascades it kept.
		ReadbackImage image;
		const auto frame = [&]()
		{
			const ReadbackImage expected = RenderToImage(redrawn, scene, camera);
			image = RenderToImage(cached, scene, camera);
			CHECK(MaxDifference(expected, image) <= 1);
			REQUIRE(cached.GetStats().GPUDriven);
			return cached.GetStats().CachedCascadeMask;
		};
		const auto move = [](Entity entity, const glm::vec3& offset)
		{
			entity.GetComponent<TransformComponent>().Translation += offset;
			entity.MarkModified<TransformComponent>();
		};

		CHECK(frame() == 0u);           // Nothing drawn yet
		CHECK(frame() == allCascades);  // Nothing changed
		// The box's first move: the cache did not follow it before, so its old bounds are not known and every cascade is
		// redrawn. From then on only the cascades its old or new bounds reach are: the last one, not the first.
		const uint32_t last = 1u << (specification.ShadowCascades - 1);
		move(cacheScene.Mover, glm::vec3(2.0f, 0.0f, 0.0f));
		CHECK(frame() == 0u);
		const ReadbackImage before = image;
		move(cacheScene.Mover, glm::vec3(3.0f, 0.0f, 0.0f));
		const uint32_t kept = frame();
		CHECK((kept & last) == 0u);
		CHECK((kept & 1u) != 0u);
		CHECK(MaxDifference(before, image) > 16); // The box and its shadow moved in view
		move(cacheScene.Mover, glm::vec3(0.0f, 0.0f, 2.0f));
		CHECK(frame() == kept);
		CHECK(frame() == allCascades);
		// The box goes: what its bounds reached is redrawn without it.
		cacheScene.Mover.SetActive(false);
		CHECK(frame() == kept);
		CHECK(frame() == allCascades);

		// The camera moves, or the light turns: every cascade is drawn again.
		camera = LookAt(glm::vec3(0.0f, 4.0f, 5.9f), glm::vec3(0.0f, 1.0f, -20.0f));
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(60.0f), 1.0f, 0.1f, 500.0f);
		camera.Far = 500.0f;
		CHECK(frame() == 0u);
		CHECK(frame() == allCascades);
		cacheScene.Sun.GetComponent<TransformComponent>().Rotation = Math::LookRotation(glm::normalize(glm::vec3(-0.45f, -1.0f, -0.4f)));
		cacheScene.Sun.MarkModified<TransformComponent>();
		CHECK(frame() == 0u);
		CHECK(frame() == allCascades);
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A shadow edge across a cascade split shows no seam while the cascades blend, and one where they switch at once")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddNeutralPostProcess(scene);
		MaterialProperties matte;
		matte.Roughness = 1.0f;
		const AssetHandle material = assets.AddMaterial(matte);
		AddMesh(scene, BuiltinAssets::PlaneMesh, material, glm::vec3(0.0f, 0.0f, -100.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(400.0f);
		// A 4 m wall along Z, 7 m to the right, lit from +X 30 degrees above the horizon: the edge of its shadow runs along
		// the floor's line x = 0, under the camera, so it stays in the image's middle column at every depth.
		constexpr float elevation = 30.0f;
		const float wallX = 0.1f + 4.0f / std::tan(glm::radians(elevation));
		Entity wall = AddMesh(scene, BuiltinAssets::CubeMesh, material, glm::vec3(wallX, 2.0f, -100.0f), "Wall");
		wall.GetComponent<TransformComponent>().Scale = glm::vec3(0.2f, 4.0f, 400.0f);
		Entity sun = AddSun(scene, SunDirection(elevation, 0.0f), 3.0f);
		DirectionalLightComponent& light = sun.GetComponent<DirectionalLightComponent>();
		// Shadows to 50 m: the third cascade ends at 15.96 m (RoofScene), and its band at the default blend of 0.1 begins
		// 0.92 m before. The normal offset, in texels of each cascade, moves the edge where the texels grow: four texels
		// rather than the default 1.5 make that move outweigh the rounding of the edge to the coarser cascade's texels.
		light.ShadowDistance = 50.0f;
		light.ShadowNormalBias = 4.0f;

		constexpr uint32_t size = 256;
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		SceneRenderer renderer(specification);
		renderer.SetViewportSize(size, size);
		// Looking down at 45 degrees, the floor from 12.6 m to 21.9 m away: about 30 rows per metre of depth at the split.
		const SceneCamera camera = PitchedCamera(glm::vec3(0.0f, 11.3f, 0.0f), 45.0f, 30.0f);

		// Per row, the edge's place: the sunlight summed across 80 columns around it (lit on the left, in shadow on the
		// right), in pixels. Steps of it from row to row inside a cascade are the shadow filter's noise.
		struct Steps
		{
			float InCascade = 0.0f; // The largest step between rows inside the third or the fourth cascade
			float AtSplit = 0.0f;   // The largest step from 14.8 m to 16.2 m: the third cascade's band and its split
		};
		const auto measure = [&](float blend)
		{
			light.CascadeBlend = blend;
			sun.MarkModified<DirectionalLightComponent>();
			const SunlightImage sunlight = RenderSunlight(renderer, scene, camera, sun);
			std::vector<float> edges(size, 0.0f);
			for (uint32_t row = 0; row < size; row++)
			{
				for (uint32_t x = size / 2 - 40; x < size / 2 + 40; x++)
					edges[row] += std::clamp(sunlight.At(x, row), 0.0f, 1.0f);
			}
			Steps steps;
			const auto inRange = [](float depth, float from, float to) { return depth >= from && depth <= to; };
			for (uint32_t row = 0; row + 1 < size; row++)
			{
				const float depth = FloorRowDepth(camera, row, size);
				const float nextDepth = FloorRowDepth(camera, row + 1, size);
				const float step = std::abs(edges[row + 1] - edges[row]);
				if ((inRange(depth, 13.0f, 14.8f) && inRange(nextDepth, 13.0f, 14.8f)) || (inRange(depth, 16.2f, 21.0f) && inRange(nextDepth, 16.2f, 21.0f)))
				{
					CHECK(std::abs(edges[row] - 40.0f) < 6.0f); // The edge stays in the middle
					steps.InCascade = std::max(steps.InCascade, step);
				}
				else if (inRange(depth, 14.8f, 16.2f) && inRange(nextDepth, 14.8f, 16.2f))
				{
					steps.AtSplit = std::max(steps.AtSplit, step);
				}
			}
			MESSAGE("Cascade blend " << blend << ": largest step in a cascade " << steps.InCascade << " px, at the split " << steps.AtSplit << " px");
			return steps;
		};

		const Steps blended = measure(DirectionalLightComponent().CascadeBlend);
		CHECK(blended.AtSplit <= blended.InCascade);
		const Steps switched = measure(0.0f);
		CHECK(switched.AtSplit > 2.0f * switched.InCascade);
		CHECK(switched.AtSplit > 1.0f);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("At the widest cascade blend, the next cascade covers every pixel of each band")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		RoofScene roofScene(assets);
		roofScene.Sun.GetComponent<DirectionalLightComponent>().CascadeBlend = DirectionalLightComponent::c_MaxCascadeBlend;

		// A narrow view along the floor across the sun's direction, 20 cm above it: from 2.3 m on, its slices are long and
		// thin, so the bands before the splits (half of each slice: from 1.45 m, 4.85 m and 11.38 m) reach past the squares
		// of the next slices' spheres unless those slices begin at the bands.
		constexpr uint32_t size = 256;
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		SceneRenderer renderer(specification);
		renderer.SetViewportSize(size, size);
		const SceneCamera camera = PitchedCamera(glm::vec3(0.0f, 0.2f, 0.0f), 0.0f, 10.0f);
		const SunlightImage sunlight = RenderSunlight(renderer, roofScene.SceneData, camera, roofScene.Sun);

		// Up to the last cascade's band (from 32.98 m) the roof's shadow is whole.
		uint32_t rows = 0;
		float brightest = 0.0f;
		for (uint32_t row = 0; row < size; row++)
		{
			const float depth = FloorRowDepth(camera, row, size);
			if (depth < 0.0f || depth > 32.5f)
				continue;
			rows++;
			for (uint32_t x = 0; x < size; x++)
				brightest = std::max(brightest, sunlight.At(x, row));
		}
		CHECK(rows > 100);
		CHECK(brightest < 0.02f);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Shadows fade out over the last cascade's band, ending at the shadow distance")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		RoofScene roofScene(assets);

		constexpr uint32_t size = 256;
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		SceneRenderer renderer(specification);
		renderer.SetViewportSize(size, size);
		// Looking down at 45 degrees from 20 m: the floor from about 15 m to 67 m away.
		const SceneCamera camera = PitchedCamera(glm::vec3(0.0f, 20.0f, 0.0f), 45.0f, 60.0f);

		// Per row, the mean sunlight; at the default blend of 0.1 the last band is the last 3.40 m before 50 m.
		const auto rowSunlight = [&](const SunlightImage& sunlight, uint32_t row)
		{
			float sum = 0.0f;
			for (uint32_t x = 0; x < size; x++)
				sum += sunlight.At(x, row);
			return sum / static_cast<float>(size);
		};
		const float bandStart = 50.0f - 0.1f * (50.0f - 15.964f);
		const SunlightImage faded = RenderSunlight(renderer, roofScene.SceneData, camera, roofScene.Sun);
		uint32_t bandRows = 0;
		float previous = 0.0f;
		for (uint32_t row = size; row-- > 0;) // Bottom up: depth grows
		{
			const float depth = FloorRowDepth(camera, row, size);
			if (depth < 0.0f)
				continue;
			CAPTURE(depth);
			const float sunlight = rowSunlight(faded, row);
			if (depth < bandStart - 0.1f)
			{
				CHECK(sunlight < 0.01f);
			}
			else if (depth > 50.1f)
			{
				CHECK(sunlight > 0.99f);
			}
			else if (depth > bandStart + 0.1f && depth < 49.9f)
			{
				bandRows++;
				CHECK(std::abs(sunlight - (depth - bandStart) / (50.0f - bandStart)) < 0.05f);
				CHECK(sunlight >= previous - 0.01f);
			}
			previous = sunlight;
		}
		CHECK(bandRows >= 5);

		// Without the blend, the shadow ends at once.
		roofScene.Sun.GetComponent<DirectionalLightComponent>().CascadeBlend = 0.0f;
		roofScene.Sun.MarkModified<DirectionalLightComponent>();
		const SunlightImage cut = RenderSunlight(renderer, roofScene.SceneData, camera, roofScene.Sun);
		for (uint32_t row = 0; row < size; row++)
		{
			const float depth = FloorRowDepth(camera, row, size);
			if (depth > 0.0f && depth < 49.9f)
				CHECK(rowSunlight(cut, row) < 0.01f);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
