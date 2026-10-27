#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/Atmosphere.h"
#include "Strata/Renderer/RendererComponents.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = 128;

	// A sun `elevation` degrees above the horizon, toward -Z.
	glm::vec3 TowardSun(float elevation)
	{
		const float radians = glm::radians(elevation);
		return glm::vec3(0.0f, std::sin(radians), -std::cos(radians));
	}

	SkyLightComponent& AddAtmosphere(Scene& scene)
	{
		SkyLightComponent& sky = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
		sky.Source = SkyLightSource::Atmosphere;
		return sky;
	}

	void PointSun(Entity sun, float elevation)
	{
		sun.GetComponent<TransformComponent>().Rotation = Math::LookRotation(-TowardSun(elevation));
		sun.MarkModified<TransformComponent>();
	}

	SceneRenderOptions Forced(float preExposure)
	{
		SceneRenderOptions options;
		options.PreExposure = preExposure;
		return options;
	}

	std::vector<glm::vec4> ReadHalfImage(nvrhi::ITexture* texture, uint32_t& width, uint32_t& height)
	{
		REQUIRE(texture);
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(texture, image));
		REQUIRE(image.BytesPerPixel == 8);
		width = image.Width;
		height = image.Height;
		std::vector<glm::vec4> texels;
		texels.reserve(static_cast<size_t>(width) * height);
		for (uint32_t y = 0; y < height; y++)
		{
			for (uint32_t x = 0; x < width; x++)
				texels.push_back(ReadHalfTexel(image, x, y));
		}
		return texels;
	}

	// The sky-view table's texel looking along `direction` (Include/Atmosphere.glsl's mapping, nearest texel).
	glm::vec3 SkyViewTexel(const std::vector<glm::vec4>& table, uint32_t width, uint32_t height, const glm::vec3& direction)
	{
		const float azimuth = std::atan2(direction.z, direction.x);
		float u = azimuth / glm::two_pi<float>();
		u -= std::floor(u);
		const float elevation = std::asin(std::clamp(direction.y, -1.0f, 1.0f));
		const float t = (elevation < 0.0f ? -1.0f : 1.0f) * std::sqrt(std::abs(elevation) / glm::half_pi<float>());
		const float unit = t * 0.5f + 0.5f;
		const uint32_t x = std::min(static_cast<uint32_t>(u * static_cast<float>(width)), width - 1);
		const uint32_t y = std::min(static_cast<uint32_t>(std::lround(unit * static_cast<float>(height - 1))), height - 1);
		return glm::vec3(table[static_cast<size_t>(y) * width + x]);
	}

}

TEST_SUITE("GPU.Atmosphere")
{
	TEST_CASE("The zenith is bluer than the horizon at noon, and the horizon toward the sun redder than the zenith at sunset")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Scene scene;
		AddNeutralPostProcess(scene);
		AddAtmosphere(scene);
		Entity sun = AddSun(scene, -TowardSun(60.0f), 100000.0f, false);
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.7f, 0.0f), glm::vec3(0.0f, 1.7f, -1.0f));

		const auto ratios = [&](float elevation)
		{
			PointSun(sun, elevation);
			REQUIRE(renderer.Render(scene, camera, nullptr, Forced(1.0f / 65536.0f)));
			CHECK(renderer.GetStats().Atmosphere);
			uint32_t width = 0;
			uint32_t height = 0;
			const std::vector<glm::vec4> table = ReadHalfImage(renderer.GetAtmosphereSkyViewTable(), width, height);
			CHECK(width == Atmosphere::c_SkyViewWidth);
			CHECK(height == Atmosphere::c_SkyViewHeight);
			const glm::vec3 zenith = SkyViewTexel(table, width, height, glm::vec3(0.0f, 1.0f, 0.0f));
			const glm::vec3 horizon = SkyViewTexel(table, width, height, glm::normalize(glm::vec3(0.0f, 0.02f, -1.0f)));
			CHECK(zenith.b > 0.0f);
			CHECK(horizon.b > 0.0f);
			return std::pair(zenith, horizon);
		};
		const auto [noonZenith, noonHorizon] = ratios(60.0f);
		CHECK(noonZenith.b / noonZenith.r > noonHorizon.b / noonHorizon.r);
		CHECK(noonZenith.b / noonZenith.r > 2.0f); // Deep blue straight up
		const auto [sunsetZenith, sunsetHorizon] = ratios(2.0f);
		CHECK(sunsetHorizon.r / sunsetHorizon.b > sunsetZenith.r / sunsetZenith.b);
		CHECK(sunsetHorizon.r > sunsetHorizon.b);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Straight up and straight down, the sky shows no hole where the view's direction has no horizontal part")
	{
		// The sky-view table's column is the azimuth atan(z, x), which is undefined at (0, 0): the pixel at the center of an
		// odd-sized view looking straight up or down looks exactly along the vertical axis, and must show what its neighbors do.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Scene scene;
		AddNeutralPostProcess(scene);
		AddAtmosphere(scene);
		AddSun(scene, -TowardSun(40.0f), 100000.0f, false);
		constexpr uint32_t c_OddSize = 33;
		constexpr uint32_t c_Center = c_OddSize / 2;
		SceneRenderer renderer;
		renderer.SetViewportSize(c_OddSize, c_OddSize);
		SceneRenderOptions options = Forced(1.0f / 65536.0f);
		options.HDROutput = true;
		for (const float vertical : { 1.0f, -1.0f })
		{
			CAPTURE(vertical);
			const glm::vec3 eye(0.0f, 1.7f, 0.0f);
			REQUIRE(renderer.Render(scene, LookAt(eye, eye + glm::vec3(0.0f, vertical, 0.0f)), nullptr, options));
			uint32_t width = 0;
			uint32_t height = 0;
			const std::vector<glm::vec4> image = ReadHalfImage(renderer.GetHDRTexture(), width, height);
			REQUIRE(width == c_OddSize);
			REQUIRE(height == c_OddSize);
			const auto at = [&](uint32_t x, uint32_t y) { return glm::vec3(image[static_cast<size_t>(y) * width + x]); };
			const glm::vec3 middle = at(c_Center, c_Center);
			const glm::vec3 around = (at(c_Center - 1, c_Center) + at(c_Center + 1, c_Center) + at(c_Center, c_Center - 1) + at(c_Center, c_Center + 1)) * 0.25f;
			CAPTURE(middle);
			CAPTURE(around);
			for (int channel = 0; channel < 3; channel++)
			{
				CHECK(std::isfinite(middle[channel]));
				CHECK(std::abs(middle[channel] - around[channel]) <= 0.05f * std::max(around[channel], 1.0e-4f));
			}
			if (vertical > 0.0f)
				CHECK(middle.b > 0.0f); // The zenith's blue
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The GPU's transmittance table matches the CPU's evaluation within 1%")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Scene scene;
		AddNeutralPostProcess(scene);
		SkyLightComponent& sky = AddAtmosphere(scene);
		sky.AtmosphereDensity = 1.3f;
		sky.AtmosphereHaze = 2.0f;
		AddSun(scene, -TowardSun(30.0f), 3.0f, false);
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		REQUIRE(renderer.Render(scene, LookAt(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, -1.0f))));
		uint32_t width = 0;
		uint32_t height = 0;
		const std::vector<glm::vec4> table = ReadHalfImage(renderer.GetAtmosphereTransmittanceTable(), width, height);
		REQUIRE(width == Atmosphere::c_TransmittanceWidth);
		REQUIRE(height == Atmosphere::c_TransmittanceHeight);

		const Atmosphere::Medium medium = Atmosphere::GetMedium(sky.AtmosphereDensity, sky.AtmosphereHaze);
		uint32_t compared = 0;
		uint32_t mismatched = 0;
		for (uint32_t y = 0; y < height; y++)
		{
			for (uint32_t x = 0; x < width; x++)
			{
				const glm::dvec2 radiusMu = Atmosphere::GetTransmittanceRadiusMu((glm::dvec2(x, y) + 0.5) / glm::dvec2(width, height));
				const glm::dvec3 expected = Atmosphere::ComputeTransmittance(medium, radiusMu.x, radiusMu.y);
				const glm::vec4 actual = table[static_cast<size_t>(y) * width + x];
				for (int channel = 0; channel < 3; channel++)
				{
					// Relative where half floats keep their precision, else absolute.
					const double tolerance = std::max(0.01 * expected[channel], 1.0e-4);
					compared++;
					mismatched += std::abs(static_cast<double>(actual[channel]) - expected[channel]) > tolerance ? 1u : 0u;
				}
			}
		}
		CHECK(compared == width * height * 3);
		CHECK(mismatched == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The directional light at sunset is its color times the CPU's transmittance of the atmosphere")
	{
		// The same floor lit by the same sun with a procedural sky (no atmosphere between them) and with the atmosphere, its
		// sky's own light off (Intensity 0): the floor's color differs by the transmittance, channel by channel.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		MaterialProperties matte;
		matte.BaseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
		matte.Roughness = 1.0f;
		const AssetHandle material = assets.AddMaterial(matte);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 5.0f, 3.0f), glm::vec3(0.0f));
		constexpr float c_Elevation = 4.0f;
		constexpr float c_Sun = 100000.0f;
		const auto render = [&](SkyLightSource source, SceneRendererStats& stats)
		{
			Scene scene;
			AddNeutralPostProcess(scene);
			SkyLightComponent& sky = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
			sky.Source = source;
			sky.Intensity = 0.0f;
			AddMesh(scene, BuiltinAssets::PlaneMesh, material, glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(50.0f);
			AddSun(scene, -TowardSun(c_Elevation), c_Sun, false);
			SceneRenderer renderer;
			renderer.SetViewportSize(c_Size, c_Size);
			REQUIRE(renderer.Render(scene, camera, nullptr, Forced(1.0f / 256.0f)));
			stats = renderer.GetStats();
			ReadbackImage hdr;
			REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), hdr));
			return glm::vec3(ReadHalfTexel(hdr, c_Size / 2, c_Size / 2));
		};
		SceneRendererStats plainStats;
		SceneRendererStats atmosphereStats;
		const glm::vec3 plain = render(SkyLightSource::Procedural, plainStats);
		const glm::vec3 throughAir = render(SkyLightSource::Atmosphere, atmosphereStats);
		CHECK_FALSE(plainStats.Atmosphere);
		CHECK(plainStats.SunTransmittance == glm::vec3(1.0f));
		REQUIRE(atmosphereStats.Atmosphere);

		const glm::dvec3 expected = Atmosphere::ComputeSunTransmittance(Atmosphere::GetMedium(1.0f, 1.0f), glm::dvec3(TowardSun(c_Elevation)),
			glm::radians(0.53) * 0.5);
		for (int channel = 0; channel < 3; channel++)
		{
			CAPTURE(channel);
			CHECK(atmosphereStats.SunTransmittance[channel] == doctest::Approx(expected[channel]).epsilon(1.0e-5));
			REQUIRE(plain[channel] > 0.0f);
			CHECK(throughAir[channel] / plain[channel] == doctest::Approx(expected[channel]).epsilon(0.01));
		}
		// Reddened: the blue light scatters out on the long path.
		CHECK(expected.r > 2.0 * expected.b);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The environment is computed again only after the sun moved half a degree, a step a frame, and switches when complete")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Scene scene;
		AddNeutralPostProcess(scene);
		AddAtmosphere(scene);
		Entity sun = AddSun(scene, -TowardSun(30.0f), 50000.0f, false);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, -1.0f));

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneRenderOptions options = Forced(1.0f / 65536.0f);
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		// The first environment at once.
		CHECK(renderer.GetStats().EnvironmentUpdates == 1);
		CHECK(renderer.GetStats().AtmosphereTableUpdates == 1);
		CHECK(renderer.GetStats().AtmosphereSkyViewUpdates == 1);
		nvrhi::ITexture* first = renderer.GetEnvironmentCube();
		REQUIRE(first);

		// A smaller move redraws the sky's table only.
		PointSun(sun, 30.3f);
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		CHECK(renderer.GetStats().AtmosphereSkyViewUpdates == 1);
		CHECK(renderer.GetStats().AtmosphereTableUpdates == 0);
		CHECK(renderer.GetStats().EnvironmentRefreshSteps == 0);
		CHECK_FALSE(renderer.GetStats().EnvironmentRefreshPending);
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		CHECK(renderer.GetStats().AtmosphereSkyViewUpdates == 0);

		// Past half a degree from the environment's sun: a step a frame, the old environment lighting until the last one.
		PointSun(sun, 30.6f);
		for (uint32_t frame = 1; frame <= SceneRenderer::c_AtmosphereRefreshSteps; frame++)
		{
			CAPTURE(frame);
			REQUIRE(renderer.Render(scene, camera, nullptr, options));
			const SceneRendererStats& stats = renderer.GetStats();
			CHECK(stats.EnvironmentRefreshSteps == 1);
			const bool last = frame == SceneRenderer::c_AtmosphereRefreshSteps;
			CHECK(stats.EnvironmentUpdates == (last ? 1u : 0u));
			CHECK(stats.EnvironmentRefreshPending == !last);
			CHECK((renderer.GetEnvironmentCube() == first) == !last);
		}
		nvrhi::ITexture* second = renderer.GetEnvironmentCube();
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		CHECK(renderer.GetStats().EnvironmentRefreshSteps == 0);
		CHECK(renderer.GetEnvironmentCube() == second);

		// Renderers of single images compute it at once; the two sets of maps take turns.
		SceneRendererSpecification specification;
		specification.SpreadLightingUpdates = false;
		SceneRenderer atOnce(specification);
		atOnce.SetViewportSize(c_Size, c_Size);
		REQUIRE(atOnce.Render(scene, camera, nullptr, options));
		nvrhi::ITexture* atOnceFirst = atOnce.GetEnvironmentCube();
		PointSun(sun, 32.0f);
		REQUIRE(atOnce.Render(scene, camera, nullptr, options));
		CHECK(atOnce.GetStats().EnvironmentRefreshSteps == SceneRenderer::c_AtmosphereRefreshSteps);
		CHECK(atOnce.GetStats().EnvironmentUpdates == 1);
		CHECK(atOnce.GetEnvironmentCube() != atOnceFirst);
		PointSun(sun, 34.0f);
		REQUIRE(atOnce.Render(scene, camera, nullptr, options));
		CHECK(atOnce.GetEnvironmentCube() == atOnceFirst);

		// A sky without the atmosphere keeps none of it.
		scene.GetRegistry().view<SkyLightComponent>().each([&](SkyLightComponent& sky) { sky.Source = SkyLightSource::Procedural; });
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		CHECK_FALSE(renderer.GetStats().Atmosphere);
		CHECK(renderer.GetAtmosphereTransmittanceTable() == nullptr);
		CHECK(renderer.GetAtmosphereSkyViewTable() == nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Changes of the light's intensity alone recompute none of the atmosphere's tables and refresh none of its environment")
	{
		// The tables hold the sky per lux of the light's color, which its intensity only scales: a light fading out at the
		// horizon, down to 0, changes none of them. (Its color times its intensity over the largest component is not always
		// the color's own to the bit, and an intensity of 0 has no largest component.)
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Scene scene;
		AddNeutralPostProcess(scene);
		AddAtmosphere(scene);
		Entity sun = AddSun(scene, -TowardSun(20.0f), 3.0f, false);
		sun.GetComponent<DirectionalLightComponent>().Color = glm::vec3(0.85f, 1.0f, 1.0f);
		sun.MarkModified<DirectionalLightComponent>();
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, -1.0f));
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneRenderOptions options = Forced(1.0f / 256.0f);
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		CHECK(renderer.GetStats().AtmosphereTableUpdates == 1);

		uint32_t tableUpdates = 0;
		uint32_t skyViewUpdates = 0;
		uint32_t refreshSteps = 0;
		for (int frame = 1; frame <= 50; frame++)
		{
			sun.GetComponent<DirectionalLightComponent>().Intensity = 3.0f * (1.0f - static_cast<float>(frame) / 50.0f);
			sun.MarkModified<DirectionalLightComponent>();
			REQUIRE(renderer.Render(scene, camera, nullptr, options));
			const SceneRendererStats& stats = renderer.GetStats();
			CHECK(stats.Atmosphere);
			tableUpdates += stats.AtmosphereTableUpdates;
			skyViewUpdates += stats.AtmosphereSkyViewUpdates;
			refreshSteps += stats.EnvironmentRefreshSteps;
		}
		CHECK(tableUpdates == 0);
		CHECK(skyViewUpdates == 0);
		CHECK(refreshSteps == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("An environment refresh finishes while the atmosphere's settings keep changing, and the next one catches up")
	{
		// A refresh under way when the settings change runs to its end (a script that changes them every frame must not keep
		// the environment from ever updating); the next one starts from the settings of then.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Scene scene;
		AddNeutralPostProcess(scene);
		Entity sky = scene.CreateEntity("Sky");
		sky.AddComponent<SkyLightComponent>().Source = SkyLightSource::Atmosphere;
		AddSun(scene, -TowardSun(30.0f), 3.0f, false);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, -1.0f));
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneRenderOptions options = Forced(1.0f / 256.0f);
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		REQUIRE(renderer.GetStats().EnvironmentUpdates == 1);

		constexpr uint32_t c_Steps = SceneRenderer::c_AtmosphereRefreshSteps;
		uint32_t updates = 0;
		for (uint32_t frame = 1; frame <= 2 * c_Steps; frame++)
		{
			CAPTURE(frame);
			sky.GetComponent<SkyLightComponent>().AtmosphereHaze = 1.0f + 0.01f * static_cast<float>(frame);
			sky.MarkModified<SkyLightComponent>();
			REQUIRE(renderer.Render(scene, camera, nullptr, options));
			const SceneRendererStats& stats = renderer.GetStats();
			CHECK(stats.AtmosphereTableUpdates == 1);
			CHECK(stats.EnvironmentRefreshSteps == 1);
			updates += stats.EnvironmentUpdates;
			if (frame % c_Steps == 0)
				CHECK(updates == frame / c_Steps);
		}
		CHECK(updates == 2);

		// Once they stay, the refresh then started brings the environment to them, and none follows.
		for (uint32_t frame = 1; frame <= c_Steps; frame++)
		{
			CAPTURE(frame);
			REQUIRE(renderer.Render(scene, camera, nullptr, options));
			CHECK(renderer.GetStats().EnvironmentRefreshSteps == 1);
			CHECK(renderer.GetStats().EnvironmentUpdates == (frame == c_Steps ? 1u : 0u));
		}
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		CHECK(renderer.GetStats().EnvironmentRefreshSteps == 0);
		CHECK_FALSE(renderer.GetStats().EnvironmentRefreshPending);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Switching the sky's source and the sun back and forth lights the scene as a fresh renderer does")
	{
		// A mirror and a rough metal sphere reflect the environment: after any sequence of sky sources and suns that ends with
		// the atmosphere and its sun, they look as they do to a renderer that saw only the end (0.6.0 could leave the mirror
		// black after a switch to the procedural sky and back, until the sun moved).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		MaterialProperties mirror;
		mirror.BaseColor = glm::vec4(1.0f);
		mirror.Metallic = 1.0f;
		mirror.Roughness = 0.0f;
		MaterialProperties rough = mirror;
		rough.Roughness = 0.4f;
		const AssetHandle mirrorMaterial = assets.AddMaterial(mirror);
		const AssetHandle roughMaterial = assets.AddMaterial(rough);
		constexpr float c_SunLux = 50000.0f;
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, -3.0f));
		// The spheres' centers on screen: 0.75 m either side at 3 m, with a 60 degree field of view.
		const glm::uvec2 mirrorPixel(36, 64);
		const glm::uvec2 roughPixel(92, 64);

		struct TestWorld
		{
			Scene Contents;
			Entity Sky;
			Entity Sun;
		};
		const auto makeWorld = [&](TestWorld& world, bool withSun)
		{
			// Exposure for the sky's reflection: a 50,000 lux sun's sky is thousands of nits.
			AddNeutralPostProcess(world.Contents).Exposure = -14.0f;
			world.Sky = world.Contents.CreateEntity("Sky");
			world.Sky.AddComponent<SkyLightComponent>().Source = SkyLightSource::Atmosphere;
			if (withSun)
				world.Sun = AddSun(world.Contents, -TowardSun(30.0f), c_SunLux, false);
			AddMesh(world.Contents, BuiltinAssets::SphereMesh, mirrorMaterial, glm::vec3(-0.75f, 1.0f, -3.0f), "Mirror");
			AddMesh(world.Contents, BuiltinAssets::SphereMesh, roughMaterial, glm::vec3(0.75f, 1.0f, -3.0f), "Rough");
		};

		enum class Step { Procedural, Atmosphere, SunOff, SunOn, IntensityZero, IntensityBack, AddSun };
		struct Sequence
		{
			const char* Name;
			bool StartsWithSun;
			std::vector<Step> Steps;
		};
		const Sequence sequences[] = {
			{ "procedural, sun off, sun on, atmosphere", true, { Step::Procedural, Step::SunOff, Step::SunOn, Step::Atmosphere } },
			{ "sun off, procedural, atmosphere, sun on", true, { Step::SunOff, Step::Procedural, Step::Atmosphere, Step::SunOn } },
			{ "intensity 0, procedural, atmosphere, intensity back", true, { Step::IntensityZero, Step::Procedural, Step::Atmosphere, Step::IntensityBack } },
			{ "a sun added to an atmosphere without one", false, { Step::AddSun } },
		};
		const SceneRenderOptions options = Forced(1.0f / 65536.0f);

		for (const Sequence& sequence : sequences)
		{
			for (const bool spread : { true, false })
			{
				const std::string name = sequence.Name;
				CAPTURE(name);
				CAPTURE(spread);
				SceneRendererSpecification specification;
				specification.SpreadLightingUpdates = spread;
				TestWorld world;
				makeWorld(world, sequence.StartsWithSun);
				SceneRenderer renderer(specification);
				renderer.SetViewportSize(c_Size, c_Size);
				REQUIRE(renderer.Render(world.Contents, camera, nullptr, options));
				uint32_t frames = 1;
				for (const Step step : sequence.Steps)
				{
					SkyLightComponent& sky = world.Sky.GetComponent<SkyLightComponent>();
					switch (step)
					{
						case Step::Procedural: sky.Source = SkyLightSource::Procedural; world.Sky.MarkModified<SkyLightComponent>(); break;
						case Step::Atmosphere: sky.Source = SkyLightSource::Atmosphere; world.Sky.MarkModified<SkyLightComponent>(); break;
						case Step::SunOff: world.Sun.SetActive(false); break;
						case Step::SunOn: world.Sun.SetActive(true); break;
						case Step::IntensityZero:
						case Step::IntensityBack:
							world.Sun.GetComponent<DirectionalLightComponent>().Intensity = step == Step::IntensityZero ? 0.0f : c_SunLux;
							world.Sun.MarkModified<DirectionalLightComponent>();
							break;
						case Step::AddSun: world.Sun = AddSun(world.Contents, -TowardSun(30.0f), c_SunLux, false); break;
					}
					world.Contents.UpdateWorldTransforms();
					REQUIRE(renderer.Render(world.Contents, camera, nullptr, options));
					frames++;
					// A sun turned on or off under the atmosphere lights the scene from that frame on (no refresh over frames).
					const bool sunTurned = step == Step::SunOff || step == Step::SunOn || step == Step::AddSun;
					if (sunTurned && sky.Source == SkyLightSource::Atmosphere)
					{
						CHECK(renderer.GetStats().EnvironmentUpdates == 1);
						CHECK_FALSE(renderer.GetStats().EnvironmentRefreshPending);
					}
				}
				// A refresh the last steps started runs to its end.
				for (uint32_t frame = 0; frame <= SceneRenderer::c_AtmosphereRefreshSteps && renderer.GetStats().EnvironmentRefreshPending; frame++)
				{
					REQUIRE(renderer.Render(world.Contents, camera, nullptr, options));
					frames++;
				}
				REQUIRE_FALSE(renderer.GetStats().EnvironmentRefreshPending);
				ReadbackImage image;
				REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));

				TestWorld fresh;
				makeWorld(fresh, true);
				SceneRenderer freshRenderer(specification);
				freshRenderer.SetViewportSize(c_Size, c_Size);
				for (uint32_t frame = 0; frame < frames; frame++)
					REQUIRE(freshRenderer.Render(fresh.Contents, camera, nullptr, options));
				ReadbackImage expected;
				REQUIRE(Renderer::ReadTexture(freshRenderer.GetOutputTexture(), expected));
				REQUIRE(image.BytesPerPixel == 4);

				int largest = 0;
				for (const glm::uvec2& center : { mirrorPixel, roughPixel })
				{
					for (uint32_t y = center.y - 2; y <= center.y + 2; y++)
					{
						for (uint32_t x = center.x - 2; x <= center.x + 2; x++)
						{
							for (uint32_t channel = 0; channel < 3; channel++)
							{
								const size_t index = (static_cast<size_t>(y) * image.Width + x) * 4 + channel;
								largest = std::max(largest, std::abs(static_cast<int>(image.Pixels[index]) - static_cast<int>(expected.Pixels[index])));
							}
						}
					}
				}
				CHECK(largest <= 1);
				// Not black: the mirror shows the sky (above 0.05 of the display's white in its brightest channel).
				const size_t mirrorIndex = (static_cast<size_t>(mirrorPixel.y) * image.Width + mirrorPixel.x) * 4;
				const int brightest = std::max({ image.Pixels[mirrorIndex], image.Pixels[mirrorIndex + 1], image.Pixels[mirrorIndex + 2] });
				CHECK(brightest > 13);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
