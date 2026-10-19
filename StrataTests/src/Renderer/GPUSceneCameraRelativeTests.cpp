#include "Renderer/SceneRendererTestUtils.h"

#include "Strata/Renderer/MeshFactory.h"

#include <cstdint>
#include <cstdlib>
#include <vector>

// Camera-relative rendering: scene shaders work with the camera at the origin, so a scene far from the world's origin looks
// as it does at the origin (the vertices near the camera keep their precision, the sky its smoothness).

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = 256;

	// A finely tessellated sphere on a textured ground under the procedural sky, lit by the sun (with shadows) and a point
	// light, seen from a meter and a half (a pixel spans about 7 mm there: what 32-bit world positions resolve 100 km from
	// the origin), the sky filling the top; everything placed at `offset` (exact as floats far from the origin too).
	ReadbackImage RenderAt(const glm::vec3& offset, AssetHandle sphere, AssetHandle ground, GPUDrivenMode mode)
	{
		Scene scene;
		AddNeutralPostProcess(scene);
		SkyLightComponent& sky = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
		sky.Source = SkyLightSource::Procedural;
		Entity sun = AddSun(scene, glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f)), 3.0f);
		sun.GetComponent<DirectionalLightComponent>().ShadowDistance = 60.0f;
		AddMesh(scene, BuiltinAssets::PlaneMesh, ground, offset + glm::vec3(0.0f, 0.0f, -8.0f), "Ground").GetComponent<TransformComponent>().Scale = glm::vec3(40.0f);
		AddMesh(scene, sphere, UUID::Null(), offset + glm::vec3(0.0f, 1.0f, -1.25f), "Sphere");
		Entity lamp = scene.CreateEntity("Lamp");
		lamp.GetComponent<TransformComponent>().Translation = offset + glm::vec3(1.0f, 1.75f, -0.5f);
		PointLightComponent& light = lamp.AddComponent<PointLightComponent>();
		light.Color = glm::vec3(1.0f, 0.6f, 0.3f);
		light.Intensity = 20.0f;
		light.Range = 6.0f;

		SceneCamera camera = LookAt(offset + glm::vec3(0.0f, 1.25f, 0.5f), offset + glm::vec3(0.0f, 1.5f, -3.0f));
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(60.0f), 1.0f, 0.1f, 500.0f);
		camera.VerticalFOV = glm::radians(60.0f);
		camera.Far = 500.0f;
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		specification.GPUDriven = mode;
		SceneRenderer renderer(specification);
		renderer.SetViewportSize(c_Size, c_Size);
		// A few frames: the environment and the shadows settle, and the GPU-driven path draws from its history.
		ReadbackImage image;
		for (int frame = 0; frame < 3; frame++)
			image = RenderToImage(renderer, scene, camera);
		return image;
	}

	// The share of pixels whose every channel lies within `tolerance` of the reference's.
	double MatchingShare(const ReadbackImage& reference, const ReadbackImage& image, int tolerance)
	{
		REQUIRE(reference.Pixels.size() == image.Pixels.size());
		size_t matching = 0;
		const size_t pixels = reference.Pixels.size() / 4;
		for (size_t pixel = 0; pixel < pixels; pixel++)
		{
			bool close = true;
			for (size_t channel = 0; channel < 3; channel++)
				close = close && std::abs(int(reference.Pixels[pixel * 4 + channel]) - int(image.Pixels[pixel * 4 + channel])) <= tolerance;
			matching += close ? 1u : 0u;
		}
		return static_cast<double>(matching) / static_cast<double>(pixels);
	}

}

TEST_SUITE("GPU.SceneRenderer.CameraRelative")
{
	TEST_CASE("A scene 20 and 100 kilometers from the origin looks as it does at the origin")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		for (const AssetHandle builtin : { BuiltinAssets::PlaneMesh, BuiltinAssets::DefaultMaterial })
			REQUIRE(assets.Manager->LoadAssetSync(builtin));
		const AssetHandle sphere = assets.Add(MeshFactory::CreateSphere(0.5f, 128, 64), "FineSphere");
		// A checker of 4x4-texel squares, tiled over the ground.
		std::vector<uint8_t> pixels;
		for (uint32_t y = 0; y < 32; y++)
		{
			for (uint32_t x = 0; x < 32; x++)
			{
				const uint8_t value = ((x / 4) + (y / 4)) % 2 == 0 ? 200 : 60;
				AppendPixel(pixels, value, value, value, 255);
			}
		}
		MaterialProperties checker;
		checker.BaseColorMap = assets.AddTexture(32, 32, pixels, TextureFormat::RGBA8SRGB);
		checker.UVTiling = glm::vec2(8.0f);
		const AssetHandle ground = assets.AddMaterial(checker);

		// Both paths: the GPU's culling and the CPU's.
		for (const GPUDrivenMode mode : { GPUDrivenMode::Auto, GPUDrivenMode::Off })
		{
			CAPTURE(static_cast<int>(mode));
			const ReadbackImage origin = RenderAt(glm::vec3(0.0f), sphere, ground, mode);
			for (const float distance : { 20'000.0f, 100'000.0f })
			{
				CAPTURE(distance);
				const ReadbackImage far = RenderAt(glm::vec3(distance, 0.0f, distance), sphere, ground, mode);
				const double share = MatchingShare(origin, far, 2);
				MESSAGE(distance, " units away: ", share * 100.0, "% of the pixels within 2/255");
				CHECK(share >= 0.999);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
