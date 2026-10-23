#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/RendererComponents.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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
