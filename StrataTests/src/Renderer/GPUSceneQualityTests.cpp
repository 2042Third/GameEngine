#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/RendererQuality.h"

#include <cstdint>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = 96;

	// A floor and a box lit by a sun from the side: the box's shadow falls on the floor at -X, the floor at +X is lit.
	struct QualityScene
	{
		Scene SceneData;
		SceneCamera Camera;
		glm::vec3 Shadowed = glm::vec3(-0.8f, 0.0f, 0.3f); // Within the shadow (-1.4 to 0.2 along X) by more than a cascade texel
		glm::vec3 Lit = glm::vec3(1.5f, 0.0f, 0.0f);

		QualityScene()
		{
			AddNeutralPostProcess(SceneData);
			Entity floor = AddMesh(SceneData, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor");
			floor.GetComponent<TransformComponent>().Scale = glm::vec3(20.0f);
			AddMesh(SceneData, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f, 1.0f, 0.0f), "Box");
			Entity sun = AddSun(SceneData, glm::normalize(glm::vec3(-0.6f, -1.0f, 0.0f)), 3.0f);
			sun.GetComponent<DirectionalLightComponent>().ShadowSoftness = 0.0f;
			Camera = LookAt(glm::vec3(0.0f, 8.0f, 8.0f), glm::vec3(0.0f));
			Camera.Projection = Math::PerspectiveReverseZ(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
			Camera.VerticalFOV = glm::radians(60.0f);
			Camera.Far = 100.0f;
		}

		// The box's shadow shows: the shadowed floor is far darker than the lit floor.
		void CheckShadow(const ReadbackImage& image) const
		{
			const int shadowed = Red(image, ProjectToPixel(Camera, Shadowed, c_Size));
			const int lit = Red(image, ProjectToPixel(Camera, Lit, c_Size));
			CAPTURE(shadowed);
			CAPTURE(lit);
			CHECK(lit > 40);
			CHECK(shadowed * 2 < lit);
		}
	};

	SceneRendererSpecification MakeSpecification(GPUDrivenMode mode)
	{
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		specification.GPUDriven = mode;
		return specification;
	}

	uint32_t GetShadowMapLayers(const SceneRenderer& renderer)
	{
		nvrhi::ITexture* shadowMap = renderer.GetShadowMap();
		REQUIRE(shadowMap);
		return shadowMap->getDesc().arraySize;
	}

}

TEST_SUITE("GPU.SceneRenderer.Quality")
{
	TEST_CASE("The cascades' depth array has a layer per cascade the quality draws")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::PlaneMesh));
		QualityScene scene;
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			for (const uint32_t cascades : { 1u, 2u, 4u })
			{
				CAPTURE(cascades);
				SceneRendererSpecification specification = MakeSpecification(mode);
				specification.Quality.ShadowCascades = cascades;
				SceneRenderer renderer(specification);
				renderer.SetViewportSize(c_Size, c_Size);
				CHECK(renderer.GetShadowMap() == nullptr); // Made by the first frame with a shadowed sun
				// Two frames: the GPU-driven path's second culls the cascades against their pyramids (a layer each).
				ReadbackImage image;
				for (int frame = 0; frame < 2; frame++)
					image = RenderToImage(renderer, scene.SceneData, scene.Camera);
				CHECK(GetShadowMapLayers(renderer) == cascades);
				CHECK(renderer.GetShadowMap()->getDesc().width == 2048);
				scene.CheckShadow(image);
			}
		}
		// Thumbnails draw one 1024-texel cascade: a quarter of 0.6.0's four layers.
		SceneRenderer thumbnails(SceneRendererSpecification::ForThumbnails());
		thumbnails.SetViewportSize(c_Size, c_Size);
		scene.CheckShadow(RenderToImage(thumbnails, scene.SceneData, scene.Camera));
		CHECK(GetShadowMapLayers(thumbnails) == 1);
		CHECK(thumbnails.GetShadowMap()->getDesc().width == 1024);
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Renderers of single images release their cascades with their targets")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::PlaneMesh));
		QualityScene scene;
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			SceneRenderer renderer(MakeSpecification(mode));
			renderer.SetViewportSize(c_Size, c_Size);
			ReadbackImage first;
			for (int frame = 0; frame < 2; frame++)
				first = RenderToImage(renderer, scene.SceneData, scene.Camera);
			REQUIRE(renderer.GetShadowMap());

			// A resize keeps them (only the viewport's targets follow its size).
			nvrhi::ITexture* kept = renderer.GetShadowMap();
			renderer.SetViewportSize(c_Size / 2, c_Size / 2);
			CHECK(renderer.GetShadowMap() == kept);

			// Released altogether (a capture renderer between pictures, a minimized game): the cascades go too, and the next
			// frame makes them again and draws the same picture.
			renderer.SetViewportSize(0, 0);
			CHECK(renderer.GetShadowMap() == nullptr);
			renderer.SetViewportSize(c_Size, c_Size);
			ReadbackImage again;
			for (int frame = 0; frame < 2; frame++)
				again = RenderToImage(renderer, scene.SceneData, scene.Camera);
			REQUIRE(renderer.GetShadowMap());
			CHECK(GetShadowMapLayers(renderer) == 4);
			scene.CheckShadow(again);
			CHECK(MaxDifference(first, again) == 0);
		}
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
