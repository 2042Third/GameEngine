#include "Renderer/TAATestUtils.h"

#include "Strata/Core/Log.h"
#include "Strata/Renderer/Atmosphere.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/ShaderLibrary.h"
#include "TestHelpers.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// Aerial perspective (SkyLightComponent::AerialPerspective, IBL/AtmosphereAerial.comp, Include/AerialPerspective.glsl): the air
// between the camera and a surface dims it by the atmosphere's transmittance and adds the light it scatters, as the sky does
// over its whole depth. Measured through unlit quads (Renderer::ReadTexture reads no 3D texture): a white one's light minus a
// black one's is the transmittance, the black one's the in-scattered light.

using namespace Strata;
using namespace Strata::Tests;
using namespace Strata::Tests::TAA;

namespace
{

	constexpr uint32_t c_AirViewSize = 128;

	// The sun 30 degrees up behind a camera that looks along +X: it lights the quads' fronts and stays out of view, and the view
	// is symmetric about the XY plane.
	const glm::vec3 c_AirSunDirection = glm::normalize(glm::vec3(std::cos(glm::radians(30.0f)), -std::sin(glm::radians(30.0f)), 0.0f));

	void LoadAirMeshes(SceneTestAssets& assets)
	{
		for (const AssetHandle handle : { BuiltinAssets::QuadMesh, BuiltinAssets::PlaneMesh, BuiltinAssets::DefaultMaterial })
			REQUIRE(assets.Manager->LoadAssetSync(handle));
	}

	SkyLightComponent& AddAtmosphere(Scene& scene, bool aerialPerspective, float scale)
	{
		AddNeutralPostProcess(scene);
		AddSun(scene, c_AirSunDirection, 3.0f, false);
		SkyLightComponent& sky = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
		sky.Source = SkyLightSource::Atmosphere;
		sky.AerialPerspective = aerialPerspective;
		sky.AerialPerspectiveScale = scale;
		return sky;
	}

	// An unlit quad facing the camera at the origin from `position` (on the +X side), `size` meters wide.
	Entity AddFacingQuad(Scene& scene, SceneTestAssets& assets, const glm::vec3& position, float size, const glm::vec4& color, bool blended = false)
	{
		MaterialProperties material = UnlitColor(color);
		material.DoubleSided = true;
		if (blended)
			material.AlphaMode = MaterialAlphaMode::Blend;
		Entity quad = AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(material), position, "Quad");
		TransformComponent& transform = quad.GetComponent<TransformComponent>();
		transform.Rotation = glm::quat(glm::vec3(0.0f, glm::radians(-90.0f), 0.0f));
		transform.Scale = glm::vec3(size);
		return quad;
	}

	// A camera 1 m above the ground looking along +X, with its far plane at `farPlane`.
	SceneCamera HorizontalCamera(float farPlane, float verticalFOVDegrees)
	{
		SceneCamera camera = LookAt(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f, 1.0f, 0.0f));
		camera.Far = farPlane;
		camera.VerticalFOV = glm::radians(verticalFOVDegrees);
		camera.Projection = Math::PerspectiveReverseZ(camera.VerticalFOV, 1.0f, camera.Near, camera.Far);
		return camera;
	}

	SceneRenderer CreateAirRenderer()
	{
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		specification.TemporalAntiAliasing = false;
		specification.DebugName = "Air";
		return SceneRenderer(specification);
	}

	// Renders until the atmosphere lights the scene, then once more, at a pre-exposure of 1; the HDR image.
	std::vector<glm::vec3> RenderAir(GPUContext& gpu, SceneRenderer& renderer, Scene& scene, const SceneCamera& camera)
	{
		renderer.SetViewportSize(c_AirViewSize, c_AirViewSize);
		SceneRenderOptions options;
		options.PreExposure = 1.0f;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		do
		{
			REQUIRE(renderer.Render(scene, camera, nullptr, options));
			FinishFrame(gpu);
		} while (!renderer.GetStats().EnvironmentLighting && std::chrono::steady_clock::now() < deadline);
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		FinishFrame(gpu);
		return ReadHDR(renderer.GetHDRTexture());
	}

	glm::vec3 At(const std::vector<glm::vec3>& image, uint32_t x, uint32_t y)
	{
		return image[static_cast<size_t>(y) * c_AirViewSize + x];
	}

	float AirLuminance(const glm::vec3& color)
	{
		return glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
	}

	// The transmittance and the in-scattered light of the air to a quad `distance` meters along the view: a white quad's light
	// minus a black one's, and the black one's.
	struct AirSample
	{
		glm::vec3 Transmittance;
		glm::vec3 Scattering;
	};

	AirSample MeasureAir(GPUContext& gpu, SceneTestAssets& assets, float distance, float size, float scale, float farPlane)
	{
		const SceneCamera camera = HorizontalCamera(farPlane, 60.0f);
		glm::vec3 values[2];
		for (int white = 0; white < 2; white++)
		{
			Scene scene;
			AddAtmosphere(scene, true, scale);
			AddFacingQuad(scene, assets, glm::vec3(distance, 1.0f, 0.0f), size, white ? glm::vec4(1.0f) : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
			SceneRenderer renderer = CreateAirRenderer();
			const std::vector<glm::vec3> image = RenderAir(gpu, renderer, scene, camera);
			REQUIRE(renderer.GetStats().AerialPerspective);
			values[white] = At(image, c_AirViewSize / 2, c_AirViewSize / 2);
		}
		return { values[1] - values[0], values[0] };
	}

}

TEST_SUITE("GPU.Atmosphere")
{
	TEST_CASE("Aerial perspective is clear at the camera and follows the CPU's transmittance")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAirMeshes(assets);
		constexpr float c_Far = 1000.0f;
		constexpr float c_Scale = 8.0f;

		// The sky at the view's center without a quad: what "little in-scattered light" is measured against.
		Scene open;
		AddAtmosphere(open, true, c_Scale);
		SceneRenderer openRenderer = CreateAirRenderer();
		const glm::vec3 horizon = At(RenderAir(gpu, openRenderer, open, HorizontalCamera(c_Far, 60.0f)), c_AirViewSize / 2, c_AirViewSize / 2);
		REQUIRE(AirLuminance(horizon) > 0.0f);

		// At the camera (0.3 m: the first slice): nothing dims (to the half-float target's precision) and nothing is added.
		const AirSample atCamera = MeasureAir(gpu, assets, 0.3f, 0.2f, c_Scale, c_Far);
		CAPTURE(atCamera.Transmittance);
		CAPTURE(atCamera.Scattering);
		CHECK(std::min({ atCamera.Transmittance.r, atCamera.Transmittance.g, atCamera.Transmittance.b }) >= 0.999f);
		CHECK(AirLuminance(atCamera.Scattering) <= 1.0e-4f * AirLuminance(horizon));

		// At the last slice's center, D (31.5 / 32)^2 along a horizontal ray, the air's transmittance as the CPU integrates it
		// over the same distance (times the scale) from 1 m up, within 1% per channel.
		const float distance = c_Far * (31.5f / 32.0f) * (31.5f / 32.0f);
		const AirSample atLastSlice = MeasureAir(gpu, assets, distance, distance * 0.3f, c_Scale, c_Far);
		const Atmosphere::Medium medium = Atmosphere::GetMedium(1.0f, 1.0f);
		const glm::dvec3 expected = Atmosphere::ComputePathTransmittance(medium, Atmosphere::c_BottomRadius + 0.001, 0.0,
			static_cast<double>(distance) * c_Scale / 1000.0);
		CAPTURE(atLastSlice.Transmittance);
		CAPTURE(expected);
		for (int channel = 0; channel < 3; channel++)
			CHECK(atLastSlice.Transmittance[channel] == doctest::Approx(expected[channel]).epsilon(0.01));
		CHECK(expected.b < 0.9); // The case measures real dimming
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("At the end of a horizontal ray aerial perspective matches the sky at the horizon")
	{
		// A black quad 20 km away under the largest scale (1,280 km of air, beyond the 1,132 km a horizontal ray from the
		// ground crosses to the air's top) shows the light of the whole ray, which the sky-view table shows behind it: the same
		// within 5% just above the horizon (below it the sky adds the ground's light, which a surface hides).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAirMeshes(assets);
		const SceneCamera camera = HorizontalCamera(25000.0f, 10.0f);
		std::vector<glm::vec3> images[2];
		for (int quad = 0; quad < 2; quad++)
		{
			Scene scene;
			AddAtmosphere(scene, true, SkyLightComponent::c_MaxAerialPerspectiveScale);
			if (quad)
				AddFacingQuad(scene, assets, glm::vec3(20000.0f, 1.0f, 0.0f), 8000.0f, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
			SceneRenderer renderer = CreateAirRenderer();
			images[quad] = RenderAir(gpu, renderer, scene, camera);
		}
		for (const uint32_t row : { c_AirViewSize / 2 - 8, c_AirViewSize / 2 - 16 })
		{
			CAPTURE(row);
			const glm::vec3 sky = At(images[0], c_AirViewSize / 2, row);
			const glm::vec3 hazed = At(images[1], c_AirViewSize / 2, row);
			CAPTURE(sky);
			CAPTURE(hazed);
			REQUIRE(AirLuminance(sky) > 0.0f);
			CHECK(AirLuminance(hazed) == doctest::Approx(AirLuminance(sky)).epsilon(0.05));
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Aerial perspective off costs nothing, and orthographic views have none")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAirMeshes(assets);
		const SceneCamera camera = HorizontalCamera(2000.0f, 60.0f);
		const auto render = [&](bool on, float scale, const SceneCamera& view, std::vector<glm::vec3>& image)
		{
			Scene scene;
			AddAtmosphere(scene, on, scale);
			AddFacingQuad(scene, assets, glm::vec3(500.0f, 1.0f, 0.0f), 200.0f, glm::vec4(0.5f, 0.5f, 0.5f, 1.0f));
			AddMesh(scene, BuiltinAssets::PlaneMesh, BuiltinAssets::DefaultMaterial, glm::vec3(0.0f), "Ground").GetComponent<TransformComponent>().Scale =
				glm::vec3(2000.0f);
			SceneRenderer renderer = CreateAirRenderer();
			image = RenderAir(gpu, renderer, scene, view);
			return std::pair(renderer.GetStats().AerialPerspective, renderer.GetAerialVolume() != nullptr);
		};
		std::vector<glm::vec3> disabled, unscaled, hazed;
		CHECK(render(false, 1.0f, camera, disabled) == std::pair(false, false));
		CHECK(render(true, 0.0f, camera, unscaled) == std::pair(false, false));
		const bool offAlike = disabled == unscaled;
		CHECK(offAlike);
		CHECK(render(true, 8.0f, camera, hazed) == std::pair(true, true));
		const bool hazedDiffers = hazed != disabled;
		CHECK(hazedDiffers);
		std::vector<glm::vec3> orthographic;
		CHECK(render(true, 8.0f, OrthographicLookAt(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f, 1.0f, 0.0f), 50.0f), orthographic) == std::pair(false, false));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Transparent surfaces are hazed like opaque ones at the same distance")
	{
		// An opaque gray quad and an alpha-blended one of alpha 1 and the same gray, side by side 500 m away, mirror images of
		// each other about the view's vertical plane (the sun lies in it): the same color within 1%.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAirMeshes(assets);
		Scene scene;
		AddAtmosphere(scene, true, 8.0f);
		AddFacingQuad(scene, assets, glm::vec3(500.0f, 1.0f, -150.0f), 250.0f, glm::vec4(0.5f, 0.5f, 0.5f, 1.0f));
		AddFacingQuad(scene, assets, glm::vec3(500.0f, 1.0f, 150.0f), 250.0f, glm::vec4(0.5f, 0.5f, 0.5f, 1.0f), true);
		SceneRenderer renderer = CreateAirRenderer();
		const std::vector<glm::vec3> image = RenderAir(gpu, renderer, scene, HorizontalCamera(2000.0f, 60.0f));
		REQUIRE(renderer.GetStats().AerialPerspective);
		// The quads' centers: about a quarter of the view either side of the middle (the camera looks along +X, +Z is to its right).
		const glm::vec3 left = At(image, c_AirViewSize / 2 - 33, c_AirViewSize / 2);
		const glm::vec3 right = At(image, c_AirViewSize / 2 + 33, c_AirViewSize / 2);
		CAPTURE(left);
		CAPTURE(right);
		for (int channel = 0; channel < 3; channel++)
			CHECK(right[channel] == doctest::Approx(left[channel]).epsilon(0.01));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Aerial perspective that cannot be made is reported once, and the frames are drawn without the haze")
	{
		// Each frame tries to make the volume again (an allocation can succeed later), but its error is logged only when it
		// appears: the frame counts as degraded, which keeps the renderer's last error instead of clearing it at its end.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAirMeshes(assets);
		Scene scene;
		AddAtmosphere(scene, true, 8.0f);
		AddFacingQuad(scene, assets, glm::vec3(500.0f, 1.0f, 0.0f), 200.0f, glm::vec4(0.5f, 0.5f, 0.5f, 1.0f));
		const SceneCamera camera = HorizontalCamera(2000.0f, 60.0f);
		SceneRenderer renderer = CreateAirRenderer();
		ShaderLibrary& shaders = Renderer::GetShaderLibrary();
		size_t reports = 0;
		{
			const LogCapture log;
			shaders.SetUnavailable("IBL/AtmosphereAerial.comp", true);
			RenderAir(gpu, renderer, scene, camera);
			for (int frame = 0; frame < 4; frame++)
			{
				REQUIRE(renderer.Render(scene, camera));
				FinishFrame(gpu);
				CHECK(renderer.GetStats().EnvironmentLighting);
				CHECK_FALSE(renderer.GetStats().AerialPerspective);
			}
			shaders.SetUnavailable("IBL/AtmosphereAerial.comp", false);
			for (const LogEntry& entry : log.GetEntries())
			{
				if (entry.Message.find("aerial perspective's volume could not be created") != std::string::npos)
					reports++;
			}
		}
		CHECK(reports == 1);
		CHECK(renderer.GetAerialVolume() == nullptr);

		// Once it can be made, the next frame hazes.
		RenderAir(gpu, renderer, scene, camera);
		CHECK(renderer.GetStats().AerialPerspective);
		CHECK(renderer.GetAerialVolume() != nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
