#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/RendererComponents.h"

using namespace Strata;
using namespace Strata::Tests;

TEST_SUITE("GPU.SceneRenderer.ReflectionProbes")
{
	TEST_CASE("A scene without reflection probes samples a one-slot black cube array")
	{
		// Every scene shader's layout holds the probes' cube array (the device's imageCubeArray feature is required): without
		// probes the renderer binds the renderer's shared one-slot black array, and the scene renders without validation errors.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::ITexture* black = Strata::Renderer::GetBlackCubeArrayTexture();
		REQUIRE(black);
		const nvrhi::TextureDesc& desc = black->getDesc();
		CHECK(desc.dimension == nvrhi::TextureDimension::TextureCubeArray);
		CHECK(desc.arraySize == 6);
		CHECK(desc.width == 1);
		CHECK(desc.height == 1);

		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::vec3(-0.3f, -1.0f, -0.2f), 1.0f);
		AddMesh(scene, BuiltinAssets::SphereMesh, BuiltinAssets::DefaultMaterial, glm::vec3(0.0f), "Ball");
		SceneRenderer renderer;
		renderer.SetViewportSize(c_SceneTestSize, c_SceneTestSize);
		const ReadbackImage image = RenderToImage(renderer, scene, LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f)));
		const glm::u8vec4 ball = GetPixelRGBA8(image, c_SceneTestSize / 2, c_SceneTestSize / 2);
		CHECK(ball.r + ball.g + ball.b > 30);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
