#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/RendererComponents.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = c_SceneTestSize;

	SceneRendererSpecification ExactSpecification()
	{
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		return specification;
	}

	// A view ray through a pixel's center: where it starts (on the near plane) and its direction, in world space.
	struct PixelRay
	{
		glm::dvec3 Origin;
		glm::dvec3 Direction;
	};

	PixelRay GetPixelRay(const SceneCamera& camera, uint32_t x, uint32_t y)
	{
		const glm::dvec2 ndc((static_cast<double>(x) + 0.5) / c_Size * 2.0 - 1.0, 1.0 - (static_cast<double>(y) + 0.5) / c_Size * 2.0);
		const glm::dmat4 inverse = glm::inverse(glm::dmat4(camera.Projection) * glm::dmat4(camera.View));
		const glm::dvec4 nearPoint = inverse * glm::dvec4(ndc, 1.0, 1.0);
		const glm::dvec4 farPoint = inverse * glm::dvec4(ndc, 0.5, 1.0);
		const glm::dvec3 origin = glm::dvec3(nearPoint) / nearPoint.w;
		return PixelRay { origin, glm::normalize(glm::dvec3(farPoint) / farPoint.w - origin) };
	}

	// The fog's optical depth along a ray from `start` (world space) over [StartDistance, distance], by Simpson's rule: an
	// integration of its definition, independent of the shaders' closed form.
	double IntegrateOpticalDepth(const HeightFogComponent& fog, const glm::dvec3& start, const glm::dvec3& direction, double distance)
	{
		const double from = fog.StartDistance;
		if (!(distance > from))
			return 0.0;
		constexpr int c_Steps = 4096;
		const double step = (distance - from) / c_Steps;
		double sum = 0.0;
		for (int index = 0; index <= c_Steps; index++)
		{
			const double s = from + step * index;
			const double height = start.y + direction.y * s - fog.BaseHeight;
			const double weight = index == 0 || index == c_Steps ? 1.0 : (index % 2 == 1 ? 4.0 : 2.0);
			sum += weight * fog.Density * std::exp(-fog.HeightFalloff * height);
		}
		return sum * step / 3.0;
	}

	// The Henyey-Greenstein phase function of the fog's scattering of the directional light.
	double SunPhase(const HeightFogComponent& fog, const glm::dvec3& viewDirection, const glm::dvec3& lightTravels)
	{
		const double g = static_cast<double>(fog.SunScattering) * HeightFogComponent::c_MaxSunAsymmetry;
		const double cosTheta = glm::dot(viewDirection, -glm::normalize(lightTravels));
		const double denominator = 1.0 + g * g - 2.0 * g * cosTheta;
		return (1.0 - g * g) / (4.0 * glm::pi<double>() * denominator * std::sqrt(denominator));
	}

	// A linear color seen through fog of an optical depth: what the neutral post-processing writes (sRGB, 8 bits).
	glm::ivec3 ExpectFogged(const glm::dvec3& color, const glm::dvec3& inscattered, double opticalDepth, double maxOpacity)
	{
		const double opacity = std::min(1.0 - std::exp(-opticalDepth), maxOpacity);
		const glm::dvec3 linear = color * (1.0 - opacity) + inscattered * opacity;
		glm::ivec3 encoded;
		for (int channel = 0; channel < 3; channel++)
		{
			const double value = std::clamp(linear[channel], 0.0, 1.0);
			const double srgb = value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
			encoded[channel] = static_cast<int>(std::lround(srgb * 255.0));
		}
		return encoded;
	}

	int MaxDifference(const glm::u8vec4& pixel, const glm::ivec3& expected)
	{
		return std::max({ std::abs(pixel.r - expected.r), std::abs(pixel.g - expected.g), std::abs(pixel.b - expected.b) });
	}

	// A large unlit wall facing +Z at z = `z`, centered at the height `centerY`.
	Entity AddWall(Scene& scene, SceneTestAssets& assets, const MaterialProperties& material, float z, float centerY, float x = 0.0f, float size = 40.0f)
	{
		Entity wall = AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(material), glm::vec3(x, centerY, z), "Wall");
		TransformComponent& transform = wall.GetComponent<TransformComponent>();
		transform.Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f)); // The plane's +Y toward +Z
		transform.Scale = glm::vec3(size);
		wall.GetComponent<MeshRendererComponent>().CastShadows = false;
		return wall;
	}

	bool SameImages(const ReadbackImage& a, const ReadbackImage& b)
	{
		return a.Width == b.Width && a.Height == b.Height && a.Pixels == b.Pixels;
	}

}

TEST_SUITE("GPU.SceneRenderer.Fog")
{
	TEST_CASE("Height fog over surfaces matches the integral of its density along each view ray")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddNeutralPostProcess(scene);
		const glm::dvec3 wallColor(0.1, 0.5, 0.2);
		constexpr float c_WallZ = -10.0f;
		AddWall(scene, assets, UnlitColor(glm::vec4(glm::vec3(wallColor), 1.0f)), c_WallZ, 3.0f);
		HeightFogComponent& fog = scene.CreateEntity("Fog").AddComponent<HeightFogComponent>();
		fog.Density = 0.06f;
		fog.HeightFalloff = 0.25f;
		fog.BaseHeight = 1.0f;
		fog.StartDistance = 2.0f;
		fog.MaxOpacity = 0.85f;
		fog.Color = glm::vec3(0.9f, 0.6f, 0.3f);
		fog.ColorFromSky = false;
		fog.SunScattering = 0.6f;

		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		const glm::vec3 cameraPosition(0.0f, 3.0f, 0.0f);

		// Every pixel shows the wall: rows above and below the camera's height (rays that rise and fall), nearer and farther.
		// Returns how many pixels the fog changed visibly.
		const auto checkWall = [&](const SceneCamera& camera, const glm::dvec3& fogLight, const glm::dvec3* sunTravels, const glm::dvec3& sunColor)
		{
			const ReadbackImage image = RenderToImage(renderer, scene, camera);
			CHECK(renderer.GetStats().HeightFog);
			int worst = 0;
			int fogged = 0;
			for (uint32_t y = 0; y < c_Size; y++)
			{
				for (uint32_t x = 0; x < c_Size; x++)
				{
					const PixelRay ray = GetPixelRay(camera, x, y);
					const glm::dvec3 hit = ray.Origin + ray.Direction * ((c_WallZ - ray.Origin.z) / ray.Direction.z);
					// Perspective rays start at the camera, orthographic ones on its plane.
					glm::dvec3 start(cameraPosition);
					glm::dvec3 direction = glm::normalize(hit - start);
					double distance = glm::length(hit - start);
					if (camera.Orthographic)
					{
						direction = ray.Direction;
						distance = glm::dot(hit - glm::dvec3(cameraPosition), direction);
						start = hit - direction * distance;
					}
					glm::dvec3 light = fogLight;
					if (sunTravels)
						light += sunColor * SunPhase(fog, direction, *sunTravels);
					const glm::ivec3 expected = ExpectFogged(wallColor, glm::dvec3(fog.Color) * light, IntegrateOpticalDepth(fog, start, direction, distance), fog.MaxOpacity);
					const glm::u8vec4 pixel = GetPixelRGBA8(image, x, y);
					worst = std::max(worst, MaxDifference(pixel, expected));
					if (MaxDifference(pixel, ExpectFogged(wallColor, glm::dvec3(0.0), 0.0, 0.0)) > 8)
						fogged++;
				}
			}
			CHECK(worst <= 1);
			return fogged;
		};
		constexpr int c_Half = static_cast<int>(c_Size * c_Size / 2);

		// Lit by its own color (ColorFromSky off), without a sun.
		const SceneCamera camera = LookAt(cameraPosition, glm::vec3(0.0f, 3.0f, -1.0f));
		CHECK(checkWall(camera, glm::dvec3(1.0), nullptr, glm::dvec3(0.0)) > c_Half);

		// The start distance keeps the fog off what is nearer: a later start, past the wall's center (10 m), leaves it clear.
		fog.StartDistance = 11.0f;
		{
			const ReadbackImage image = RenderToImage(renderer, scene, camera);
			CHECK(GetPixelRGBA8(image, c_Size / 2, c_Size / 2) == glm::u8vec4(glm::u8vec3(ExpectFogged(wallColor, glm::dvec3(0.0), 0.0, 0.0)), 255));
		}
		CHECK(checkWall(camera, glm::dvec3(1.0), nullptr, glm::dvec3(0.0)) > 0);
		fog.StartDistance = 2.0f;

		// A sun ahead and above scatters through the fog, the most around its direction.
		const glm::dvec3 sunTravels = glm::normalize(glm::dvec3(0.3, -0.4, 0.9));
		Entity sun = AddSun(scene, glm::vec3(sunTravels), 2.0f, false);
		sun.GetComponent<DirectionalLightComponent>().Color = glm::vec3(1.0f, 0.9f, 0.8f);
		CHECK(checkWall(camera, glm::dvec3(1.0), &sunTravels, glm::dvec3(2.0, 1.8, 1.6)) > c_Half);
		sun.SetActive(false);

		// ColorFromSky without an environment: the sky light's ambient color lights the fog, as it lights surfaces.
		fog.ColorFromSky = true;
		SkyLightComponent& sky = scene.CreateEntity("Ambient").AddComponent<SkyLightComponent>();
		sky.AmbientColor = glm::vec3(0.4f, 0.3f, 0.2f);
		sky.Intensity = 1.5f;
		CHECK(checkWall(camera, glm::dvec3(0.6, 0.45, 0.3), nullptr, glm::dvec3(0.0)) > c_Half);
		fog.ColorFromSky = false;

		// Orthographic views look along parallel rays from the camera's plane.
		CHECK(checkWall(OrthographicLookAt(cameraPosition, glm::vec3(0.0f, 3.0f, -1.0f), 12.0f), glm::dvec3(1.0), nullptr, glm::dvec3(0.0)) > c_Half);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The sky is fogged at an infinite distance: rays that never leave the fog get its maximum opacity")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddNeutralPostProcess(scene);
		// A uniform sky without a sun's disk: its radiance and irradiance are the same color everywhere.
		const glm::dvec3 skyColor(0.3, 0.35, 0.4);
		SkyLightComponent& sky = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
		sky.Source = SkyLightSource::Procedural;
		sky.ZenithColor = sky.HorizonColor = sky.GroundColor = glm::vec3(skyColor);
		sky.SunSize = 0.0f;
		HeightFogComponent& fog = scene.CreateEntity("Fog").AddComponent<HeightFogComponent>();
		fog.Density = 0.05f;
		fog.HeightFalloff = 0.2f;
		fog.BaseHeight = 0.0f;
		fog.MaxOpacity = 0.7f;
		fog.Color = glm::vec3(0.8f, 0.6f, 0.4f);

		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		const glm::vec3 cameraPosition(0.0f, 2.0f, 0.0f);
		const SceneCamera camera = LookAt(cameraPosition, glm::vec3(0.0f, 2.0f, -1.0f));
		const ReadbackImage image = RenderToImage(renderer, scene, camera);
		REQUIRE(renderer.GetStats().EnvironmentLighting);
		CHECK(renderer.GetStats().HeightFog);

		// The sky lights the fog: the procedural sky's horizon color, here the sky's color.
		const glm::dvec3 inscattered = glm::dvec3(fog.Color) * skyColor;
		int worstBelow = 0;
		int worstAbove = 0;
		for (uint32_t y = 0; y < c_Size; y++)
		{
			const PixelRay ray = GetPixelRay(camera, c_Size / 4, y);
			double opticalDepth = std::numeric_limits<double>::infinity();
			if (ray.Direction.y > 0.0)
			{
				// Far enough that what is left of the integral is below e^-50 of its start.
				const double distance = fog.StartDistance + 50.0 / (fog.HeightFalloff * ray.Direction.y);
				opticalDepth = IntegrateOpticalDepth(fog, glm::dvec3(cameraPosition), ray.Direction, distance);
			}
			const int difference = MaxDifference(GetPixelRGBA8(image, c_Size / 4, y), ExpectFogged(skyColor, inscattered, opticalDepth, fog.MaxOpacity));
			int& worst = ray.Direction.y > 0.0 ? worstAbove : worstBelow;
			worst = std::max(worst, difference);
		}
		CHECK(worstBelow <= 1);
		CHECK(worstAbove <= 1);
		// Looking up, the fog thins: the top row shows more of the sky than the horizon does.
		CHECK(GetPixelRGBA8(image, c_Size / 4, 0).b > GetPixelRGBA8(image, c_Size / 4, c_Size / 2).b + 10);

		// A density of 0 leaves the sky as it is, also where rays never leave the fog.
		fog.Density = 0.0f;
		const ReadbackImage clear = RenderToImage(renderer, scene, camera);
		CHECK(MaxDifference(GetPixelRGBA8(clear, c_Size / 4, c_Size - 1), ExpectFogged(skyColor, inscattered, 0.0, 0.0)) <= 1);

		// Fog that hides the sky entirely (below the horizon, at full opacity) takes the procedural sky's horizon color,
		// whatever its gradient, so the horizon shows no seam.
		fog.Density = 0.05f;
		fog.MaxOpacity = 1.0f;
		fog.Color = glm::vec3(1.0f, 0.9f, 0.8f);
		sky.ZenithColor = glm::vec3(0.1f, 0.2f, 0.6f);
		sky.HorizonColor = glm::vec3(0.6f, 0.65f, 0.7f);
		sky.GroundColor = glm::vec3(0.2f, 0.15f, 0.1f);
		const auto checkHidden = [&](const glm::dvec3& light, int tolerance)
		{
			const ReadbackImage image = RenderToImage(renderer, scene, camera);
			const glm::ivec3 expected = ExpectFogged(glm::dvec3(0.0), glm::dvec3(fog.Color) * light, std::numeric_limits<double>::infinity(), 1.0);
			int worst = 0;
			for (uint32_t y = c_Size / 2 + 1; y < c_Size; y++)
				worst = std::max(worst, MaxDifference(GetPixelRGBA8(image, c_Size / 4, y), expected));
			CHECK(worst <= tolerance);
		};
		checkHidden(glm::dvec3(sky.HorizonColor), 1);

		// An environment map lights the fog with its irradiance from above: a map of one color above the horizon and another
		// below gives the upper one (within its filtering).
		const glm::vec3 upper(0.5f, 0.6f, 0.8f);
		sky.Source = SkyLightSource::EnvironmentMap;
		sky.EnvironmentMap = assets.Add(CreateEnvironmentTexture(upper, glm::vec3(0.1f, 0.08f, 0.05f)), "TwoToneSky");
		checkHidden(glm::dvec3(upper), 2);
		REQUIRE(renderer.GetStats().EnvironmentLighting);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Transparent surfaces are fogged like opaque ones at the same depth")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddNeutralPostProcess(scene);
		// Two walls side by side at the same depth, the right one alpha-blended (fully opaque alpha): mirrored pixels have
		// the same distance and height.
		const glm::vec4 color(0.2f, 0.6f, 0.3f, 1.0f);
		AddWall(scene, assets, UnlitColor(color), -8.0f, 2.0f, -10.0f, 20.0f);
		MaterialProperties blended = UnlitColor(color);
		blended.AlphaMode = MaterialAlphaMode::Blend;
		AddWall(scene, assets, blended, -8.0f, 2.0f, 10.0f, 20.0f);
		HeightFogComponent& fog = scene.CreateEntity("Fog").AddComponent<HeightFogComponent>();
		fog.Density = 0.08f;
		fog.ColorFromSky = false;

		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, 2.0f, -1.0f));
		const ReadbackImage image = RenderToImage(renderer, scene, camera);
		const glm::ivec3 unfogged = ExpectFogged(glm::dvec3(glm::vec3(color)), glm::dvec3(0.0), 0.0, 0.0);
		int worst = 0;
		for (uint32_t y = 0; y < c_Size; y += 3)
		{
			for (uint32_t x = 0; x < c_Size / 2 - 1; x += 3)
			{
				const glm::u8vec4 opaque = GetPixelRGBA8(image, x, y);
				const glm::u8vec4 transparent = GetPixelRGBA8(image, c_Size - 1 - x, y);
				CHECK(MaxDifference(opaque, unfogged) > 8); // Fogged
				worst = std::max(worst, MaxDifference(transparent, glm::ivec3(opaque)));
			}
		}
		CHECK(worst <= 1);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The first active height fog applies")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddNeutralPostProcess(scene);
		AddWall(scene, assets, UnlitColor(glm::vec4(0.5f, 0.5f, 0.5f, 1.0f)), -10.0f, 2.0f);
		Entity red = scene.CreateEntity("Red Fog");
		HeightFogComponent& redFog = red.AddComponent<HeightFogComponent>();
		redFog.Color = glm::vec3(1.0f, 0.0f, 0.0f);
		redFog.ColorFromSky = false;
		redFog.Density = 0.1f;
		Entity blue = scene.CreateEntity("Blue Fog");
		HeightFogComponent& blueFog = blue.AddComponent<HeightFogComponent>();
		blueFog.Color = glm::vec3(0.0f, 0.0f, 1.0f);
		blueFog.ColorFromSky = false;
		blueFog.Density = 0.1f;

		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, 2.0f, -1.0f));
		const HeightFogComponent* first = SceneRenderer::FindHeightFog(scene);
		REQUIRE(first);
		Entity firstEntity = first == &redFog ? red : blue;
		Entity secondEntity = first == &redFog ? blue : red;
		const ReadbackImage both = RenderToImage(renderer, scene, camera);

		secondEntity.SetActive(false);
		const ReadbackImage firstOnly = RenderToImage(renderer, scene, camera);
		secondEntity.SetActive(true);
		firstEntity.SetActive(false);
		CHECK(SceneRenderer::FindHeightFog(scene) == &secondEntity.GetComponent<HeightFogComponent>());
		const ReadbackImage secondOnly = RenderToImage(renderer, scene, camera);
		CHECK(SameImages(both, firstOnly));
		CHECK_FALSE(SameImages(firstOnly, secondOnly));

		// Neither active: no fog.
		secondEntity.SetActive(false);
		CHECK(SceneRenderer::FindHeightFog(scene) == nullptr);
		RenderToImage(renderer, scene, camera);
		CHECK_FALSE(renderer.GetStats().HeightFog);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A scene renders the same without height fog, with a disabled one and with one of density 0")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		// Lit surfaces with shadows under a procedural sky, the sky in view.
		Scene scene;
		AddNeutralPostProcess(scene);
		MaterialProperties matte;
		matte.Roughness = 0.8f;
		Entity floor = AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(matte), glm::vec3(0.0f), "Floor");
		floor.GetComponent<TransformComponent>().Scale = glm::vec3(60.0f);
		AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(matte), glm::vec3(0.0f, 0.5f, -6.0f), "Cube");
		AddSun(scene, glm::vec3(0.4f, -0.7f, -0.5f), 3.0f).GetComponent<DirectionalLightComponent>().CascadeBlend = 0.0f;
		scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().Source = SkyLightSource::Procedural;
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 2.0f, 2.0f), glm::vec3(0.0f, 1.0f, -10.0f));

		const auto render = [&]()
		{
			SceneRenderer renderer(ExactSpecification());
			renderer.SetViewportSize(c_Size, c_Size);
			RenderToImage(renderer, scene, camera);
			ReadbackImage image = RenderToImage(renderer, scene, camera);
			return std::make_pair(std::move(image), renderer.GetStats().HeightFog);
		};
		const auto [without, withoutFog] = render();
		CHECK_FALSE(withoutFog);

		Entity fogEntity = scene.CreateEntity("Fog");
		HeightFogComponent& fog = fogEntity.AddComponent<HeightFogComponent>();
		fog.Density = 0.5f;
		fogEntity.SetActive(false);
		const auto [disabled, disabledFog] = render();
		CHECK_FALSE(disabledFog);
		CHECK(SameImages(without, disabled));

		// The fog path runs and adds exactly nothing.
		fogEntity.SetActive(true);
		fog.Density = 0.0f;
		const auto [empty, emptyFog] = render();
		CHECK(emptyFog);
		CHECK(SameImages(without, empty));

		// And the same fog with a density does change the picture.
		fog.Density = 0.05f;
		const auto [fogged, foggedFog] = render();
		CHECK(foggedFog);
		CHECK_FALSE(SameImages(without, fogged));
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
