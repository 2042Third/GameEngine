#include "Renderer/SceneRendererTestUtils.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = 128;
	// Display values of the background may differ from the color they are derived from by this much (0..1 scale).
	constexpr float c_ColorTolerance = 0.03f;

	float EncodeSRGB(float value)
	{
		value = std::clamp(value, 0.0f, 1.0f);
		return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
	}

	float DecodeSRGB(int value)
	{
		const float encoded = static_cast<float>(value) / 255.0f;
		return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
	}

	// The display color neutral post-processing turns a linear radiance into (0..1 per channel).
	glm::vec3 DisplayColor(const glm::vec3& radiance)
	{
		return glm::vec3(EncodeSRGB(radiance.r), EncodeSRGB(radiance.g), EncodeSRGB(radiance.b));
	}

	glm::vec3 PixelColor(const ReadbackImage& image, uint32_t x, uint32_t y)
	{
		return glm::vec3(GetPixelRGBA8(image, x, y)) / 255.0f;
	}

	float Luminance(const glm::u8vec4& pixel)
	{
		return 0.2126f * DecodeSRGB(pixel.r) + 0.7152f * DecodeSRGB(pixel.g) + 0.0722f * DecodeSRGB(pixel.b);
	}

	SkyLightComponent& AddProceduralSky(Scene& scene)
	{
		SkyLightComponent& sky = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
		sky.Source = SkyLightSource::Procedural;
		return sky;
	}

	// A camera at the origin looking away from the default sun (so its disk stays out of view), pitched 45 degrees up
	// with a 90 degree field of view: the top row of the image looks at the zenith, the bottom row along the horizon.
	SceneCamera HorizonToZenithCamera()
	{
		const glm::vec3 sun = SceneRenderer::GetDefaultSunDirection();
		const glm::vec3 away = glm::normalize(glm::vec3(-sun.x, 0.0f, -sun.z));
		const glm::vec3 forward = glm::normalize(away + glm::vec3(0.0f, 1.0f, 0.0f));
		SceneCamera camera = LookAt(glm::vec3(0.0f), forward);
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
		camera.VerticalFOV = glm::radians(90.0f);
		return camera;
	}

}

TEST_SUITE("GPU.ProceduralSky")
{
	TEST_CASE("The procedural sky shows the zenith color overhead and the horizon color at the horizon")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddNeutralPostProcess(scene);
		SkyLightComponent& sky = AddProceduralSky(scene);
		sky.ZenithColor = glm::vec3(0.10f, 0.25f, 0.70f);
		sky.HorizonColor = glm::vec3(0.75f, 0.60f, 0.45f);
		sky.Intensity = 0.8f;

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const ReadbackImage image = RenderToImage(renderer, scene, HorizonToZenithCamera());
		CHECK(renderer.GetStats().EnvironmentLighting);
		CHECK(renderer.GetStats().EnvironmentUpdates == 1);
		CHECK_FALSE(renderer.GetStats().PreviewLighting);

		const glm::vec3 zenith = DisplayColor(sky.ZenithColor * sky.Intensity);
		const glm::vec3 horizon = DisplayColor(sky.HorizonColor * sky.Intensity);
		for (uint32_t x = 0; x < c_Size; x += 7)
		{
			CAPTURE(x);
			const glm::vec3 top = PixelColor(image, x, 0);
			const glm::vec3 bottom = PixelColor(image, x, c_Size - 1);
			for (int channel = 0; channel < 3; channel++)
			{
				CAPTURE(channel);
				CHECK(std::abs(top[channel] - zenith[channel]) <= c_ColorTolerance);
				CHECK(std::abs(bottom[channel] - horizon[channel]) <= c_ColorTolerance);
			}
		}
		// In between, the sky blends from one into the other.
		const glm::vec3 middle = PixelColor(image, c_Size / 2, c_Size / 2);
		CHECK(middle.b > horizon.b);
		CHECK(middle.r < horizon.r);

		// Looking down, the ground color shows.
		SceneCamera down = LookAt(glm::vec3(0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
		const ReadbackImage ground = RenderToImage(renderer, scene, down);
		const glm::vec3 groundColor = DisplayColor(sky.GroundColor * sky.Intensity);
		const glm::vec3 center = PixelColor(ground, c_Size / 2, c_Size / 2);
		for (int channel = 0; channel < 3; channel++)
			CHECK(std::abs(center[channel] - groundColor[channel]) <= c_ColorTolerance);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The sun's disk sits where the directional light comes from")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddNeutralPostProcess(scene);
		SkyLightComponent& sky = AddProceduralSky(scene);
		sky.SunSize = 10.0f;
		sky.SunIntensity = 4.0f;
		const glm::vec3 toSun = glm::normalize(glm::vec3(-0.3f, 0.5f, -1.0f));
		Entity light = AddSun(scene, -toSun, 2.0f, false);
		light.GetComponent<DirectionalLightComponent>().Color = glm::vec3(1.0f, 0.5f, 0.25f);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f), toSun);
		ReadbackImage image = RenderToImage(renderer, scene, camera);
		// The disk's radiance is the light's color times SunIntensity: (4, 2, 1) shows white, its edge does not.
		const glm::u8vec4 sunCenter = GetPixelRGBA8(image, c_Size / 2, c_Size / 2);
		CHECK(sunCenter.r == 255);
		CHECK(sunCenter.g == 255);
		CHECK(sunCenter.b == 255);
		const glm::u8vec4 beside = GetPixelRGBA8(image, c_Size / 2, c_Size / 8);
		CHECK(beside.b < 250);

		// A disk of 0 degrees is no disk.
		sky.SunSize = 0.0f;
		image = RenderToImage(renderer, scene, camera);
		CHECK(GetPixelRGBA8(image, c_Size / 2, c_Size / 2).r < 250);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A white diffuse sphere lit only by the sky is brighter on top than below")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddNeutralPostProcess(scene);
		AddProceduralSky(scene);
		MaterialProperties white;
		white.BaseColor = glm::vec4(1.0f);
		white.Roughness = 1.0f;
		AddMesh(scene, BuiltinAssets::SphereMesh, assets.AddMaterial(white), glm::vec3(0.0f), "Sphere");

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f));
		const ReadbackImage image = RenderToImage(renderer, scene, camera);
		CHECK(renderer.GetStats().Lights == 0);

		// Average luminance of the upper and lower parts of the sphere's disk (it is 0.5 units in radius).
		float upper = 0.0f;
		float lower = 0.0f;
		int samples = 0;
		for (int offset = -8; offset <= 8; offset += 4)
		{
			upper += Luminance(GetPixelRGBA8(image, c_Size / 2 + offset, c_Size / 2 - 22));
			lower += Luminance(GetPixelRGBA8(image, c_Size / 2 + offset, c_Size / 2 + 22));
			samples++;
		}
		upper /= static_cast<float>(samples);
		lower /= static_cast<float>(samples);
		CAPTURE(upper);
		CAPTURE(lower);
		CHECK(lower > 0.05f); // Not black: the ground color lights it from below
		CHECK(upper >= 1.2f * lower);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The procedural cube is computed again only when the sky or the sun changes")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddNeutralPostProcess(scene);
		SkyLightComponent& sky = AddProceduralSky(scene);
		Entity light = AddSun(scene, glm::vec3(0.2f, -1.0f, 0.3f), 3.0f);
		SceneRenderer renderer;
		renderer.SetViewportSize(c_SceneTestSize, c_SceneTestSize);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.0f, 3.0f), glm::vec3(0.0f));

		auto renderUpdates = [&]()
		{
			REQUIRE(renderer.Render(scene, camera));
			return renderer.GetStats().EnvironmentUpdates;
		};
		CHECK(renderUpdates() == 1);
		nvrhi::ITexture* cube = renderer.GetEnvironmentCube();
		REQUIRE(cube);
		CHECK(renderUpdates() == 0);
		CHECK(renderUpdates() == 0);

		// Its own parameters regenerate the cube once, in place.
		sky.SunSize = 1.5f;
		CHECK(renderUpdates() == 1);
		CHECK(renderUpdates() == 0);
		CHECK(renderer.GetEnvironmentCube() == cube);
		sky.ZenithColor = glm::vec3(0.3f, 0.3f, 0.9f);
		CHECK(renderUpdates() == 1);
		sky.SunIntensity = 30.0f;
		CHECK(renderUpdates() == 1);

		// What the shading applies does not: intensity, background, blur.
		sky.Intensity = 2.0f;
		sky.ShowBackground = false;
		sky.BackgroundBlur = 0.5f;
		CHECK(renderUpdates() == 0);

		// The sun follows the light's direction and color, not its intensity.
		light.GetComponent<TransformComponent>().Rotation = Math::LookRotation(glm::vec3(-0.5f, -1.0f, 0.1f));
		CHECK(renderUpdates() == 1);
		light.GetComponent<DirectionalLightComponent>().Color = glm::vec3(1.0f, 0.9f, 0.8f);
		CHECK(renderUpdates() == 1);
		light.GetComponent<DirectionalLightComponent>().Intensity = 5.0f;
		CHECK(renderUpdates() == 0);
		CHECK(renderer.GetEnvironmentCube() == cube);

		// Values the shader cannot use are made safe and compare as equal from frame to frame.
		sky.ZenithColor = glm::vec3(std::nanf(""), 1.0f, 1.0f);
		sky.SunSize = std::numeric_limits<float>::infinity();
		CHECK(renderUpdates() == 1);
		CHECK(renderUpdates() == 0);

		// Back to an environment map without a map: no environment, its maps are released.
		sky.Source = SkyLightSource::EnvironmentMap;
		CHECK(renderUpdates() == 0);
		CHECK_FALSE(renderer.GetStats().EnvironmentLighting);
		CHECK(renderer.GetEnvironmentCube() == nullptr);
		sky.Source = SkyLightSource::Procedural;
		CHECK(renderUpdates() == 1);
		CHECK(renderer.GetStats().EnvironmentLighting);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Preview lighting lights scenes without lights of their own and leaves others unchanged")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f), "Cube");
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		// From the +X+Z side and above, like the editor camera's default view: the top, +X and +Z faces are visible.
		const SceneCamera camera = LookAt(glm::vec3(3.0f, 2.5f, 3.0f), glm::vec3(0.0f));
		SceneRenderOptions preview;
		preview.PreviewEnvironment = true;

		auto render = [&](const SceneRenderOptions& options)
		{
			renderer.ResetExposureAdaptation();
			REQUIRE(renderer.Render(scene, camera, nullptr, options));
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
			return image;
		};
		auto faceLuminance = [&](const ReadbackImage& image, const glm::vec3& faceCenter)
		{
			const glm::uvec2 pixel = ProjectToPixel(camera, faceCenter, c_Size);
			float sum = 0.0f;
			for (int dy = -2; dy <= 2; dy++)
			{
				for (int dx = -2; dx <= 2; dx++)
					sum += Luminance(GetPixelRGBA8(image, pixel.x + dx, pixel.y + dy));
			}
			return sum / 25.0f * 255.0f;
		};

		const ReadbackImage unlit = render({});
		CHECK_FALSE(renderer.GetStats().PreviewLighting);
		CHECK_FALSE(renderer.GetStats().EnvironmentLighting);
		const ReadbackImage lit = render(preview);
		CHECK(renderer.GetStats().PreviewLighting);
		CHECK(renderer.GetStats().EnvironmentLighting);
		CHECK(renderer.GetStats().Lights == 1);
		CHECK(renderer.GetStats().ShadowCasters > 0);
		CHECK(MaxDifference(unlit, lit) > 40);
		// Each visible face gets another amount of sunlight.
		const float top = faceLuminance(lit, glm::vec3(0.0f, 0.5f, 0.0f));
		const float right = faceLuminance(lit, glm::vec3(0.5f, 0.0f, 0.0f));
		const float front = faceLuminance(lit, glm::vec3(0.0f, 0.0f, 0.5f));
		CAPTURE(top);
		CAPTURE(right);
		CAPTURE(front);
		CHECK(std::abs(top - right) >= 10.0f);
		CHECK(std::abs(right - front) >= 10.0f);
		CHECK(std::abs(top - front) >= 10.0f);

		// A scene with a light of its own renders as it is.
		Entity sun = AddSun(scene, glm::vec3(0.0f, -1.0f, -0.2f), 1.0f);
		const ReadbackImage sunOnly = render({});
		CHECK(MaxDifference(render(preview), sunOnly) == 0);
		CHECK_FALSE(renderer.GetStats().PreviewLighting);
		// An inactive light is no light.
		sun.SetActive(false);
		render(preview);
		CHECK(renderer.GetStats().PreviewLighting);
		// A sky light alone (ambient color, no map) counts as lighting too.
		sun.SetActive(true);
		scene.DestroyEntity(sun);
		Entity sky = scene.CreateEntity("Sky");
		sky.AddComponent<SkyLightComponent>().AmbientColor = glm::vec3(0.2f);
		const ReadbackImage ambientOnly = render({});
		CHECK(MaxDifference(render(preview), ambientOnly) == 0);
		CHECK_FALSE(renderer.GetStats().PreviewLighting);

		// So does a point light (a dark room lit by a torch shows as the game shows it), and a spot light.
		scene.DestroyEntity(sky);
		Entity torch = scene.CreateEntity("Torch");
		torch.GetComponent<TransformComponent>().Translation = glm::vec3(1.5f, 1.5f, 1.5f);
		torch.AddComponent<PointLightComponent>().Range = 8.0f;
		CHECK_FALSE(SceneRenderer::NeedsPreviewLighting(scene));
		const ReadbackImage torchOnly = render({});
		CHECK(MaxDifference(render(preview), torchOnly) == 0);
		CHECK_FALSE(renderer.GetStats().PreviewLighting);
		CHECK(renderer.GetStats().Lights == 1);
		scene.DestroyEntity(torch);
		CHECK(SceneRenderer::NeedsPreviewLighting(scene));
		Entity spot = scene.CreateEntity("Spot");
		spot.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 3.0f, 0.0f);
		spot.GetComponent<TransformComponent>().Rotation = glm::quat(glm::radians(glm::vec3(-90.0f, 0.0f, 0.0f)));
		spot.AddComponent<SpotLightComponent>().Range = 8.0f;
		CHECK_FALSE(SceneRenderer::NeedsPreviewLighting(scene));
		const ReadbackImage spotOnly = render({});
		CHECK(MaxDifference(render(preview), spotOnly) == 0);
		CHECK_FALSE(renderer.GetStats().PreviewLighting);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
