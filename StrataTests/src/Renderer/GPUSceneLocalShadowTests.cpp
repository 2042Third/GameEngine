#include "Renderer/SceneRendererTestUtils.h"

#include "Animation/SkinnedMeshTestUtils.h"
#include "Animation/TestSkinningPoses.h"
#include "Strata/Renderer/RendererComponents.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = 128;

	SceneRendererSpecification LocalShadowSpecification(GPUDrivenMode mode)
	{
		SceneRendererSpecification specification;
		specification.GPUDriven = mode;
		specification.TextureStreaming = false;
		return specification;
	}

	Entity AddShadowedPointLight(Scene& scene, const glm::vec3& position, float intensity, float range)
	{
		Entity lamp = AddPointLight(scene, position, glm::vec3(1.0f), intensity, range);
		lamp.GetComponent<PointLightComponent>().CastShadows = true;
		return lamp;
	}

	// A spot light at `position` shining along `direction`.
	Entity AddShadowedSpotLight(Scene& scene, const glm::vec3& position, const glm::vec3& direction, float intensity, float range, float outerAngle)
	{
		Entity spot = scene.CreateEntity("Spot");
		TransformComponent& transform = spot.GetComponent<TransformComponent>();
		transform.Translation = position;
		transform.Rotation = Math::LookRotation(glm::normalize(direction), std::abs(glm::normalize(direction).y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f));
		SpotLightComponent& light = spot.AddComponent<SpotLightComponent>();
		light.Intensity = intensity;
		light.Range = range;
		light.InnerConeAngle = outerAngle * 0.8f;
		light.OuterConeAngle = outerAngle;
		light.CastShadows = true;
		return spot;
	}

	// A renderer that draws every view of every shadowed light at once (no fades), as captures do.
	SceneRendererSpecification AtOnceSpecification(GPUDrivenMode mode)
	{
		SceneRendererSpecification specification = LocalShadowSpecification(mode);
		specification.SpreadLightingUpdates = false;
		return specification;
	}

	// A light's CastShadows and Intensity, whichever kind it is.
	void SetLight(Entity light, std::optional<bool> castShadows, std::optional<float> intensity)
	{
		if (light.HasComponent<PointLightComponent>())
		{
			PointLightComponent& point = light.GetComponent<PointLightComponent>();
			point.CastShadows = castShadows.value_or(point.CastShadows);
			point.Intensity = intensity.value_or(point.Intensity);
			light.MarkModified<PointLightComponent>();
			return;
		}
		SpotLightComponent& spot = light.GetComponent<SpotLightComponent>();
		spot.CastShadows = castShadows.value_or(spot.CastShadows);
		spot.Intensity = intensity.value_or(spot.Intensity);
		light.MarkModified<SpotLightComponent>();
	}

	float ReadRed(const ReadbackImage& image, size_t pixel)
	{
		uint16_t value = 0;
		std::memcpy(&value, image.Pixels.data() + pixel * image.BytesPerPixel, sizeof(value));
		return glm::unpackHalf1x16(value);
	}

	ReadbackImage RenderHDR(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera)
	{
		REQUIRE(renderer.Render(scene, camera));
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), image));
		REQUIRE(image.Format == nvrhi::Format::RGBA16_FLOAT);
		return image;
	}

	// How much of a light reaches each pixel's surface (1 lit, 0 in full shadow; -1 where the light adds less than a fiftieth
	// of the most it adds anywhere), from the HDR images (red) of the scene as it is, with the light's shadows off and without
	// the light: the scene's lighting is linear in the light's.
	struct LightFraction
	{
		uint32_t Size = 0;
		std::vector<float> Fraction;

		float At(const glm::uvec2& pixel) const
		{
			REQUIRE(pixel.x < Size);
			REQUIRE(pixel.y < Size);
			return Fraction[static_cast<size_t>(pixel.y) * Size + pixel.x];
		}
	};

	LightFraction RenderLightFraction(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera, Entity light)
	{
		const ReadbackImage shadowed = RenderHDR(renderer, scene, camera);
		SetLight(light, false, std::nullopt);
		const ReadbackImage lit = RenderHDR(renderer, scene, camera);
		const float intensity = light.HasComponent<PointLightComponent>() ? light.GetComponent<PointLightComponent>().Intensity
			: light.GetComponent<SpotLightComponent>().Intensity;
		SetLight(light, std::nullopt, 0.0f);
		const ReadbackImage dark = RenderHDR(renderer, scene, camera);
		SetLight(light, true, intensity);
		REQUIRE(shadowed.Width == shadowed.Height);
		LightFraction result;
		result.Size = shadowed.Width;
		result.Fraction.resize(static_cast<size_t>(shadowed.Width) * shadowed.Height);
		float strongest = 0.0f;
		for (size_t pixel = 0; pixel < result.Fraction.size(); pixel++)
			strongest = std::max(strongest, ReadRed(lit, pixel) - ReadRed(dark, pixel));
		for (size_t pixel = 0; pixel < result.Fraction.size(); pixel++)
		{
			const float direct = ReadRed(lit, pixel) - ReadRed(dark, pixel);
			result.Fraction[pixel] = direct > strongest * 0.02f ? (ReadRed(shadowed, pixel) - ReadRed(dark, pixel)) / direct : -1.0f;
		}
		return result;
	}

	// A box from `min` to `max` (a scaled cube).
	Entity AddBox(Scene& scene, AssetHandle material, const glm::vec3& min, const glm::vec3& max, const std::string& name)
	{
		Entity box = AddMesh(scene, BuiltinAssets::CubeMesh, material, (min + max) * 0.5f, name);
		box.GetComponent<TransformComponent>().Scale = max - min;
		return box;
	}

	AssetHandle MatteMaterial(SceneTestAssets& assets)
	{
		MaterialProperties matte;
		matte.BaseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
		matte.Roughness = 1.0f;
		return assets.AddMaterial(matte);
	}

	void Move(Entity entity, const glm::vec3& translation)
	{
		entity.GetComponent<TransformComponent>().Translation = translation;
		entity.MarkModified<TransformComponent>();
	}

	// Texels of a depth atlas holding something nearer than the far plane (reversed-Z: above 0).
	uint32_t CountDrawnTexels(nvrhi::ITexture* atlas)
	{
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(atlas, image));
		REQUIRE(image.BytesPerPixel == sizeof(float));
		uint32_t drawn = 0;
		for (size_t offset = 0; offset < image.Pixels.size(); offset += sizeof(float))
		{
			float depth = 0.0f;
			std::memcpy(&depth, image.Pixels.data() + offset, sizeof(depth));
			drawn += depth > 0.0f ? 1u : 0u;
		}
		return drawn;
	}

	// Two lamps far apart over a floor, each with a cube beside it along +X (in that face only) and one below it.
	struct TwoLampScene
	{
		Scene SceneData;
		Entity LampA;
		Entity LampB;
		Entity CubeA; // In lamp A's +X face

		TwoLampScene()
		{
			AddNeutralPostProcess(SceneData);
			AddMesh(SceneData, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(60.0f);
			LampA = AddShadowedPointLight(SceneData, glm::vec3(-10.0f, 3.0f, 0.0f), 20.0f, 6.0f);
			LampB = AddShadowedPointLight(SceneData, glm::vec3(10.0f, 3.0f, 0.0f), 20.0f, 6.0f);
			CubeA = AddMesh(SceneData, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(-8.0f, 3.0f, 0.0f), "CubeA");
			CubeA.GetComponent<TransformComponent>().Scale = glm::vec3(0.4f);
			AddMesh(SceneData, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(10.0f, 1.0f, 0.0f), "CubeB").GetComponent<TransformComponent>().Scale = glm::vec3(0.5f);
		}
	};

	SceneCamera OverviewCamera()
	{
		return LookAt(glm::vec3(0.0f, 14.0f, 14.0f), glm::vec3(0.0f));
	}

}

TEST_SUITE("GPU.SceneRenderer.LocalShadows")
{
	TEST_CASE("Lights without CastShadows allocate no atlas and draw no view; with it a point light draws six views")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			TwoLampScene twoLamps;
			Scene& scene = twoLamps.SceneData;
			for (Entity lamp : { twoLamps.LampA, twoLamps.LampB })
			{
				lamp.GetComponent<PointLightComponent>().CastShadows = false;
				lamp.MarkModified<PointLightComponent>();
			}
			SceneRenderer renderer(LocalShadowSpecification(mode));
			renderer.SetViewportSize(c_Size, c_Size);
			const SceneCamera camera = OverviewCamera();
			REQUIRE(renderer.Render(scene, camera));
			const DrawnCounts without = GetDrawnCounts(renderer);
			CHECK(renderer.GetLocalShadowAtlas() == nullptr);
			CHECK(renderer.GetStats().LocalShadowLights == 0);
			CHECK(renderer.GetStats().LocalShadowViewsDrawn == 0);
			CHECK(renderer.GetStats().LocalShadowViewsPending == 0);

			// A renderer that never saw shadowed local lights draws the same.
			SceneRenderer fresh(LocalShadowSpecification(mode));
			fresh.SetViewportSize(c_Size, c_Size);
			REQUIRE(fresh.Render(scene, camera));
			const DrawnCounts freshCounts = GetDrawnCounts(fresh);
			CHECK(freshCounts.Instances == without.Instances);
			CHECK(freshCounts.Draws == without.Draws);
			CHECK(freshCounts.ShadowCasters == without.ShadowCasters);
			CHECK(fresh.GetStats().DrawCommands == renderer.GetStats().DrawCommands);

			twoLamps.LampA.GetComponent<PointLightComponent>().CastShadows = true;
			twoLamps.LampA.MarkModified<PointLightComponent>();
			REQUIRE(renderer.Render(scene, camera));
			REQUIRE(renderer.GetLocalShadowAtlas() != nullptr);
			CHECK(renderer.GetLocalShadowAtlas()->getDesc().width == 4096);
			CHECK(renderer.GetStats().LocalShadowLights == 1);
			CHECK(renderer.GetStats().LocalShadowViewsDrawn == 6);
			CHECK(renderer.GetStats().LocalShadowViewsPending == 0);
			const uint32_t drawn = CountDrawnTexels(renderer.GetLocalShadowAtlas());
			CHECK(drawn > 0);

			// Without casters the views hold nothing; with shadows off again the atlas stays (with the targets) but draws nothing.
			twoLamps.CubeA.GetComponent<MeshRendererComponent>().CastShadows = false;
			twoLamps.CubeA.MarkModified<MeshRendererComponent>();
			REQUIRE(renderer.Render(scene, camera));
			CHECK(renderer.GetStats().LocalShadowViewsDrawn >= 1);
			CHECK(CountDrawnTexels(renderer.GetLocalShadowAtlas()) < drawn);
			twoLamps.LampA.GetComponent<PointLightComponent>().CastShadows = false;
			twoLamps.LampA.MarkModified<PointLightComponent>();
			REQUIRE(renderer.Render(scene, camera));
			CHECK(renderer.GetStats().LocalShadowLights == 0);
			CHECK(renderer.GetStats().LocalShadowViewsDrawn == 0);
			gpu.GetDevice().WaitForIdle();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A still scene draws no view once settled; a moving caster draws only the views it reaches, a moved light only its own")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			TwoLampScene twoLamps;
			Scene& scene = twoLamps.SceneData;
			SceneRenderer renderer(LocalShadowSpecification(mode));
			renderer.SetViewportSize(c_Size, c_Size);
			const SceneCamera camera = OverviewCamera();
			REQUIRE(renderer.Render(scene, camera));
			CHECK(renderer.GetStats().LocalShadowLights == 2);
			CHECK(renderer.GetStats().LocalShadowViewsDrawn == 12);
			CHECK(renderer.GetStats().LocalShadowViewsPending == 0);
			REQUIRE(renderer.Render(scene, camera));
			CHECK(renderer.GetStats().LocalShadowViewsDrawn == 0);
			// The camera is not part of what the views show.
			REQUIRE(renderer.Render(scene, LookAt(glm::vec3(3.0f, 12.0f, 15.0f), glm::vec3(1.0f, 0.0f, 0.0f))));
			CHECK(renderer.GetStats().LocalShadowViewsDrawn == 0);

			// The cube beside lamp A, within its +X face, moves: that face alone, in each frame it moves.
			for (uint32_t step = 1; step <= 3; step++)
			{
				Move(twoLamps.CubeA, glm::vec3(-8.0f, 3.0f, 0.05f * static_cast<float>(step)));
				REQUIRE(renderer.Render(scene, camera));
				CHECK(renderer.GetStats().LocalShadowViewsDrawn == 1);
			}
			REQUIRE(renderer.Render(scene, camera));
			CHECK(renderer.GetStats().LocalShadowViewsDrawn == 0);

			// Lamp B moves: its six views, none of lamp A's.
			Move(twoLamps.LampB, glm::vec3(10.5f, 3.0f, 0.0f));
			REQUIRE(renderer.Render(scene, camera));
			CHECK(renderer.GetStats().LocalShadowViewsDrawn == 6);
			REQUIRE(renderer.Render(scene, camera));
			CHECK(renderer.GetStats().LocalShadowViewsDrawn == 0);
			gpu.GetDevice().WaitForIdle();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The redraw budget holds while lighting updates are spread; without it, or without the cache, every view is drawn in the frame")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(60.0f);
		for (int lamp = 0; lamp < 3; lamp++)
		{
			const float x = static_cast<float>(lamp - 1) * 8.0f;
			AddShadowedPointLight(scene, glm::vec3(x, 3.0f, 0.0f), 20.0f, 6.0f);
			AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(x, 1.0f, 0.0f), "Cube").GetComponent<TransformComponent>().Scale = glm::vec3(0.5f);
		}
		const SceneCamera camera = OverviewCamera();

		SceneRenderer spread(LocalShadowSpecification(GPUDrivenMode::Off));
		spread.SetViewportSize(c_Size, c_Size);
		REQUIRE(spread.Render(scene, camera));
		CHECK(spread.GetStats().LocalShadowLights == 3);
		CHECK(spread.GetStats().LocalShadowViewsDrawn == 12);
		CHECK(spread.GetStats().LocalShadowViewsPending == 6);
		REQUIRE(spread.Render(scene, camera));
		CHECK(spread.GetStats().LocalShadowViewsDrawn == 6);
		CHECK(spread.GetStats().LocalShadowViewsPending == 0);
		REQUIRE(spread.Render(scene, camera));
		CHECK(spread.GetStats().LocalShadowViewsDrawn == 0);

		SceneRendererSpecification atOnce = LocalShadowSpecification(GPUDrivenMode::Off);
		atOnce.SpreadLightingUpdates = false;
		SceneRenderer single(atOnce);
		single.SetViewportSize(c_Size, c_Size);
		REQUIRE(single.Render(scene, camera));
		CHECK(single.GetStats().LocalShadowViewsDrawn == 18);
		CHECK(single.GetStats().LocalShadowViewsPending == 0);
		REQUIRE(single.Render(scene, camera));
		CHECK(single.GetStats().LocalShadowViewsDrawn == 0);

		SceneRendererSpecification uncached = LocalShadowSpecification(GPUDrivenMode::Off);
		uncached.CacheLocalShadows = false;
		SceneRenderer measured(uncached);
		measured.SetViewportSize(c_Size, c_Size);
		for (uint32_t frame = 0; frame < 2; frame++)
		{
			REQUIRE(measured.Render(scene, camera));
			CHECK(measured.GetStats().LocalShadowViewsDrawn == 18);
			CHECK(measured.GetStats().LocalShadowViewsPending == 0);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("At most MaxShadowedLocalLights lights have shadows, and a 2048 atlas fits sixteen point lights")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(80.0f);
		for (int lamp = 0; lamp < 16; lamp++)
		{
			const glm::vec3 position(static_cast<float>(lamp % 4) * 6.0f - 9.0f, 2.5f, static_cast<float>(lamp / 4) * 6.0f - 9.0f);
			AddShadowedPointLight(scene, position, 20.0f, 5.0f);
			AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), position - glm::vec3(0.0f, 1.5f, 0.0f), "Cube").GetComponent<TransformComponent>().Scale = glm::vec3(0.5f);
		}
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 30.0f, 22.0f), glm::vec3(0.0f));

		SceneRendererSpecification fewer = LocalShadowSpecification(GPUDrivenMode::Off);
		fewer.MaxShadowedLocalLights = 3;
		SceneRenderer limited(fewer);
		limited.SetViewportSize(c_Size, c_Size);
		REQUIRE(limited.Render(scene, camera));
		CHECK(limited.GetStats().LocalShadowLights == 3);

		SceneRendererSpecification small = LocalShadowSpecification(GPUDrivenMode::Off);
		small.LocalShadowAtlasSize = 2048;
		small.SpreadLightingUpdates = false;
		SceneRenderer renderer(small);
		renderer.SetViewportSize(c_Size, c_Size);
		REQUIRE(renderer.Render(scene, camera));
		REQUIRE(renderer.GetLocalShadowAtlas() != nullptr);
		CHECK(renderer.GetLocalShadowAtlas()->getDesc().width == 2048);
		CHECK(renderer.GetStats().LocalShadowLights == 16);
		CHECK(renderer.GetStats().LocalShadowViewsDrawn == 96);
		CHECK(renderer.GetStats().LocalShadowViewsPending == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}

TEST_SUITE("GPU.SceneRenderer.LocalShadows")
{
	TEST_CASE("A point light shadows casters in all six directions, with no seam where its views meet")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			Scene scene;
			AddNeutralPostProcess(scene);
			const AssetHandle matte = MatteMaterial(assets);
			// A room of six walls 5 m from the light, a cube 2 m from it along each axis.
			const glm::vec3 axes[] = { glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
				glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f) };
			for (const glm::vec3& axis : axes)
			{
				const glm::vec3 extent = glm::vec3(6.0f) - glm::abs(axis) * 5.9f;
				AddBox(scene, matte, axis * 5.1f - extent, axis * 5.1f + extent, "Wall");
				AddBox(scene, matte, axis * 2.0f - glm::vec3(0.3f), axis * 2.0f + glm::vec3(0.3f), "Caster");
			}
			Entity lamp = AddShadowedPointLight(scene, glm::vec3(0.0f), 60.0f, 12.0f);
			SceneRenderer renderer(AtOnceSpecification(mode));
			renderer.SetViewportSize(c_Size, c_Size);
			for (size_t index = 0; index < std::size(axes); index++)
			{
				CAPTURE(index);
				const glm::vec3 axis = axes[index];
				const glm::vec3 side = std::abs(axis.y) > 0.5f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
				const SceneCamera camera = LookAt(axis * 1.0f + side * 2.5f, axis * 5.0f);
				const LightFraction fraction = RenderLightFraction(renderer, scene, camera, lamp);
				// Behind the cube, its shadow; beside it, light.
				CHECK(fraction.At(ProjectToPixel(camera, axis * 5.0f, c_Size)) < 0.2f);
				const float beside = fraction.At(ProjectToPixel(camera, axis * 5.0f - side * 2.0f, c_Size));
				CHECK(beside > 0.8f);
			}
			REQUIRE(renderer.Render(scene, LookAt(glm::vec3(0.0f, 1.0f, 1.0f), glm::vec3(0.0f))));
			CHECK(renderer.GetStats().LocalShadowLights == 1);

			// A table 2.5 m below the light, and a plate above it across where the light's -Y view meets its +X view: the plate's
			// shadow crosses that line (x = 2.5 on the table) without a lit seam.
			AddBox(scene, matte, glm::vec3(-4.8f, -2.7f, -4.8f), glm::vec3(4.8f, -2.5f, 4.8f), "Table");
			AddBox(scene, matte, glm::vec3(1.2f, -1.55f, -0.6f), glm::vec3(2.2f, -1.45f, 0.6f), "Plate");
			const SceneCamera tableCamera = LookAt(glm::vec3(2.8f, -0.5f, 2.5f), glm::vec3(2.8f, -2.5f, 0.0f));
			const LightFraction table = RenderLightFraction(renderer, scene, tableCamera, lamp);
			for (float x = 2.2f; x <= 3.41f; x += 0.1f)
			{
				CAPTURE(x);
				CHECK(table.At(ProjectToPixel(tableCamera, glm::vec3(x, -2.5f, 0.0f), c_Size)) < 0.25f);
			}
			CHECK(table.At(ProjectToPixel(tableCamera, glm::vec3(2.8f, -2.5f, -1.8f), c_Size)) > 0.8f);
			gpu.GetDevice().WaitForIdle();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A spot light's shadow lies where the light's projection puts it, and surfaces at grazing angles show no acne")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			Scene scene;
			AddNeutralPostProcess(scene);
			const AssetHandle matte = MatteMaterial(assets);
			AddBox(scene, matte, glm::vec3(-20.0f, -1.0f, -20.0f), glm::vec3(20.0f, 0.0f, 20.0f), "Floor");
			Entity spot = AddShadowedSpotLight(scene, glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 80.0f, 10.0f, 40.0f);
			const glm::vec3 casterCenter(1.0f, 2.5f, 0.5f);
			AddBox(scene, matte, casterCenter - glm::vec3(0.2f), casterCenter + glm::vec3(0.2f), "Caster");
			SceneRenderer renderer(AtOnceSpecification(mode));
			renderer.SetViewportSize(c_Size, c_Size);
			const SceneCamera camera = LookAt(glm::vec3(0.0f, 9.0f, 7.0f), glm::vec3(0.0f));
			// The shadow's center: the caster's projected from the light onto the floor, twice as far from the light.
			const glm::vec3 predicted = glm::vec3(0.0f, 5.0f, 0.0f) + (casterCenter - glm::vec3(0.0f, 5.0f, 0.0f)) * 2.0f;
			REQUIRE(predicted.y == doctest::Approx(0.0f));
			const LightFraction fraction = RenderLightFraction(renderer, scene, camera, spot);
			CHECK(fraction.At(ProjectToPixel(camera, predicted, c_Size)) < 0.2f);
			CHECK(fraction.At(ProjectToPixel(camera, -predicted, c_Size)) > 0.9f);
			CHECK(fraction.At(ProjectToPixel(camera, predicted + glm::vec3(0.0f, 0.0f, 1.2f), c_Size)) > 0.9f);

			// Nearly along the floor: every lit pixel stays lit (no self-shadowing of the floor).
			Entity grazing = AddShadowedSpotLight(scene, glm::vec3(-4.0f, 0.5f, 0.0f), glm::normalize(glm::vec3(1.0f, -0.12f, 0.0f)), 200.0f, 16.0f, 35.0f);
			SetLight(spot, false, 0.0f);
			const LightFraction low = RenderLightFraction(renderer, scene, camera, grazing);
			uint32_t litPixels = 0;
			float lowest = 1.0f;
			for (const float value : low.Fraction)
			{
				if (value < 0.0f)
					continue;
				litPixels++;
				lowest = std::min(lowest, value);
			}
			CHECK(litPixels > 200);
			CHECK(lowest > 0.95f);
			gpu.GetDevice().WaitForIdle();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Skinned, mirrored and alpha-masked casters shadow point and spot lights")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			Scene scene;
			AddNeutralPostProcess(scene);
			const AssetHandle matte = MatteMaterial(assets);
			AddBox(scene, matte, glm::vec3(-20.0f, -1.0f, -20.0f), glm::vec3(20.0f, 0.0f, 20.0f), "Floor");
			// A spot light straight down over a mirrored box and a posed skinned tube lying along X.
			Entity spot = AddShadowedSpotLight(scene, glm::vec3(0.0f, 6.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 100.0f, 10.0f, 45.0f);
			Entity mirrored = AddBox(scene, matte, glm::vec3(1.6f, 2.8f, -0.4f), glm::vec3(2.4f, 3.2f, 0.4f), "Mirrored");
			mirrored.GetComponent<TransformComponent>().Scale.x = -0.8f;
			TestSkinningPoses poses;
			const AssetHandle tube = assets.Add(CreateSkinnedTube({ .Joints = 3, .RingsPerJoint = 4, .Sides = 12, .JointLength = 0.6f, .Radius = 0.3f }), "Tube");
			Entity arm = AddMesh(scene, tube, matte, glm::vec3(-2.0f, 3.0f, 0.0f), "Arm");
			arm.GetComponent<TransformComponent>().Rotation = glm::vec3(0.0f, 0.0f, glm::radians(-90.0f));
			arm.AddComponent<SkinComponent>().Animator = arm.GetComponent<IDComponent>().ID;
			poses.Poses[arm.GetHandle()] = BentChain(3, 3, 0.6f); // Straight along the tube, through its palette
			poses.Version++;
			REQUIRE(scene.RegisterService<ISkinningPoses>(poses));

			SceneRenderer renderer(AtOnceSpecification(mode));
			renderer.SetViewportSize(c_Size, c_Size);
			const SceneCamera camera = LookAt(glm::vec3(0.0f, 10.0f, 4.0f), glm::vec3(0.0f));
			const LightFraction fraction = RenderLightFraction(renderer, scene, camera, spot);
			// Each shadow twice as far from the light's axis as its caster (half the light's height).
			CHECK(fraction.At(ProjectToPixel(camera, glm::vec3(4.0f, 0.0f, 0.0f), c_Size)) < 0.3f);
			CHECK(fraction.At(ProjectToPixel(camera, glm::vec3(-2.2f, 0.0f, 0.0f), c_Size)) < 0.3f);
			CHECK(fraction.At(ProjectToPixel(camera, glm::vec3(0.0f, 0.0f, -3.0f), c_Size)) > 0.8f);
			CHECK(renderer.GetStats().StalePoses == 0);

			// An alpha-masked quad under a point light: a checker of opaque and clear cells, whose shadow has the same holes.
			scene.UnregisterService<ISkinningPoses>(poses);
			SetLight(spot, false, 0.0f);
			const std::vector<uint8_t> checker = {
				255, 255, 255, 255,   255, 255, 255, 0,
				255, 255, 255, 0,     255, 255, 255, 255
			};
			MaterialProperties maskedProperties;
			maskedProperties.AlphaMode = MaterialAlphaMode::Mask;
			maskedProperties.DoubleSided = true;
			maskedProperties.BaseColorMap = assets.AddTexture(2, 2, checker, TextureFormat::RGBA8SRGB);
			Entity screen = AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(maskedProperties), glm::vec3(0.0f, 2.0f, 6.0f), "Screen");
			screen.GetComponent<TransformComponent>().Rotation = glm::vec3(glm::radians(-90.0f), 0.0f, 0.0f);
			screen.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f);
			Entity lamp = AddShadowedPointLight(scene, glm::vec3(0.0f, 4.0f, 6.0f), 60.0f, 10.0f);
			const SceneCamera maskCamera = LookAt(glm::vec3(0.0f, 1.0f, 9.0f), glm::vec3(0.0f, 0.0f, 6.0f));
			const LightFraction masked = RenderLightFraction(renderer, scene, maskCamera, lamp);
			// The quad's cells, 1 m each, cast shadows 2 m across on the floor 4 m below the light.
			CHECK(masked.At(ProjectToPixel(maskCamera, glm::vec3(-1.0f, 0.0f, 5.0f), c_Size)) < 0.3f);
			CHECK(masked.At(ProjectToPixel(maskCamera, glm::vec3(1.0f, 0.0f, 7.0f), c_Size)) < 0.3f);
			CHECK(masked.At(ProjectToPixel(maskCamera, glm::vec3(1.0f, 0.0f, 5.0f), c_Size)) > 0.8f);
			CHECK(masked.At(ProjectToPixel(maskCamera, glm::vec3(-1.0f, 0.0f, 7.0f), c_Size)) > 0.8f);
			gpu.GetDevice().WaitForIdle();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The most relevant lights keep their shadows, every one of sixteen in a 2048 atlas, and new shadows fade in")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		const AssetHandle matte = MatteMaterial(assets);
		AddBox(scene, matte, glm::vec3(-30.0f, -1.0f, -30.0f), glm::vec3(30.0f, 0.0f, 30.0f), "Floor");
		std::vector<Entity> lamps;
		std::vector<glm::vec3> shadowCenters;
		for (int index = 0; index < 16; index++)
		{
			const glm::vec3 position(static_cast<float>(index % 4) * 5.0f - 7.5f, 3.0f, static_cast<float>(index / 4) * 5.0f - 7.5f);
			lamps.push_back(AddShadowedPointLight(scene, position, 30.0f, 4.5f));
			// A cube halfway down, offset along X: its shadow twice as far along X on the floor.
			const glm::vec3 caster = position + glm::vec3(0.5f, -1.5f, 0.0f);
			AddBox(scene, matte, caster - glm::vec3(0.2f), caster + glm::vec3(0.2f), "Cube");
			shadowCenters.push_back(position + glm::vec3(1.0f, -3.0f, 0.0f));
		}
		const SceneCamera camera = LookAt(glm::vec3(1.0f, 24.0f, 14.0f), glm::vec3(0.0f));
		const auto renderWithout = [&](SceneRenderer& renderer)
		{
			for (Entity lamp : lamps)
				SetLight(lamp, false, std::nullopt);
			ReadbackImage image = RenderHDR(renderer, scene, camera);
			for (Entity lamp : lamps)
				SetLight(lamp, true, std::nullopt);
			return image;
		};
		const auto red = [&](const ReadbackImage& image, const glm::vec3& point)
		{
			const glm::uvec2 pixel = ProjectToPixel(camera, point, c_Size);
			return ReadRed(image, static_cast<size_t>(pixel.y) * image.Width + pixel.x);
		};

		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			// Sixteen point lights' 96 views in a 2048 atlas: their tiles step down, every light keeps its shadow.
			SceneRendererSpecification small = AtOnceSpecification(mode);
			small.LocalShadowAtlasSize = 2048;
			SceneRenderer renderer(small);
			renderer.SetViewportSize(c_Size, c_Size);
			const ReadbackImage withShadows = RenderHDR(renderer, scene, camera);
			CHECK(renderer.GetStats().LocalShadowLights == 16);
			const ReadbackImage without = renderWithout(renderer);
			for (size_t index = 0; index < lamps.size(); index++)
			{
				CAPTURE(index);
				CHECK(red(withShadows, shadowCenters[index]) < red(without, shadowCenters[index]) * 0.5f);
			}

			// At most three: the three nearest the camera (the last row's last three).
			SceneRendererSpecification fewer = AtOnceSpecification(mode);
			fewer.MaxShadowedLocalLights = 3;
			SceneRenderer limited(fewer);
			limited.SetViewportSize(c_Size, c_Size);
			const ReadbackImage limitedImage = RenderHDR(limited, scene, camera);
			CHECK(limited.GetStats().LocalShadowLights == 3);
			uint32_t shadowed = 0;
			for (size_t index = 0; index < lamps.size(); index++)
			{
				const bool hasShadow = red(limitedImage, shadowCenters[index]) < red(without, shadowCenters[index]) * 0.5f;
				shadowed += hasShadow ? 1u : 0u;
				if (index >= 13)
					CHECK(hasShadow);
			}
			CHECK(shadowed == 3);

			// Spread over frames, a light's shadow fades in once its views are drawn.
			SceneRenderer spreading(LocalShadowSpecification(mode));
			spreading.SetViewportSize(c_Size, c_Size);
			const float lit = red(without, shadowCenters[15]);
			const float full = red(withShadows, shadowCenters[15]);
			std::vector<float> values;
			for (uint32_t frame = 0; frame < 12 + SceneRenderer::c_LocalShadowFadeFrames; frame++)
				values.push_back(red(RenderHDR(spreading, scene, camera), shadowCenters[15]));
			CHECK(values.front() > full + (lit - full) * 0.5f);
			CHECK(values.back() == doctest::Approx(full).epsilon(0.02));
			for (size_t frame = 1; frame < values.size(); frame++)
				CHECK(values[frame] <= values[frame - 1] + 1e-3f);
			gpu.GetDevice().WaitForIdle();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
