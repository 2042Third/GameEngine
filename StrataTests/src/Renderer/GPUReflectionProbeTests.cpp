#include "Renderer/SceneRendererTestUtils.h"
#include "Renderer/TextureStreamingTestUtils.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/TextureFormat.h"
#include "Strata/Renderer/TextureStreamer.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <initializer_list>
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

	// An equirectangular HDR capture (RGBA16F, with its levels) whose radiance in each direction `radiance` gives; the
	// directions follow the engine's mapping (Include/IBL.glsl's DirectionToEquirect).
	Ref<Texture> CreateCapture(const std::function<glm::vec3(const glm::vec3&)>& radiance, uint32_t width = 256)
	{
		const uint32_t height = width / 2;
		TextureMip level0;
		level0.Width = width;
		level0.Height = height;
		level0.Data.resize(static_cast<size_t>(width) * height * 8);
		for (uint32_t y = 0; y < height; y++)
		{
			const float theta = (static_cast<float>(y) + 0.5f) / static_cast<float>(height) * glm::pi<float>();
			for (uint32_t x = 0; x < width; x++)
			{
				const float phi = ((static_cast<float>(x) + 0.5f) / static_cast<float>(width) - 0.5f) * glm::two_pi<float>();
				const glm::vec3 direction(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
				const glm::vec3 color = radiance(direction);
				const uint16_t texel[4] = { glm::packHalf1x16(color.r), glm::packHalf1x16(color.g), glm::packHalf1x16(color.b), glm::packHalf1x16(1.0f) };
				std::memcpy(level0.Data.data() + (static_cast<size_t>(y) * width + x) * sizeof(texel), texel, sizeof(texel));
			}
		}
		std::vector<TextureMip> mips = { level0 };
		REQUIRE(TextureUtils::GenerateMips(mips, TextureFormat::RGBA16F, false));
		TextureSpecification specification;
		specification.Format = TextureFormat::RGBA16F;
		specification.Wrap = TextureWrap::Clamp;
		Ref<Texture> texture = Texture::Create(specification, std::move(mips));
		REQUIRE(texture);
		return texture;
	}

	AssetHandle AddCapture(SceneTestAssets& assets, const glm::vec3& color)
	{
		return assets.Add(CreateCapture([color](const glm::vec3&) { return color; }, 64), "Capture");
	}

	Entity AddProbe(Scene& scene, AssetHandle capture, const glm::vec3& position, const glm::vec3& extents, float blend = 0.0f, int32_t priority = 0)
	{
		Entity entity = scene.CreateEntity("Probe");
		entity.GetComponent<TransformComponent>().Translation = position;
		ReflectionProbeComponent& probe = entity.AddComponent<ReflectionProbeComponent>();
		probe.Capture = capture;
		probe.Extents = extents;
		probe.BlendDistance = blend;
		probe.Priority = priority;
		return entity;
	}

	// A smooth mirror ball (metallic, roughness 0) of radius 0.5.
	Entity AddMirrorBall(Scene& scene, SceneTestAssets& assets, const glm::vec3& position)
	{
		MaterialProperties mirror;
		mirror.BaseColor = glm::vec4(1.0f);
		mirror.Metallic = 1.0f;
		mirror.Roughness = 0.0f;
		Entity ball = AddMesh(scene, BuiltinAssets::SphereMesh, assets.AddMaterial(mirror), position, "Mirror");
		ball.GetComponent<MeshRendererComponent>().CastShadows = false;
		return ball;
	}

	// The linear value of an 8-bit sRGB channel.
	float ToLinear(uint8_t value)
	{
		const float encoded = static_cast<float>(value) / 255.0f;
		return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
	}

	glm::vec3 LinearPixel(const ReadbackImage& image, const glm::uvec2& pixel)
	{
		const glm::u8vec4 color = GetPixelRGBA8(image, pixel.x, pixel.y);
		return glm::vec3(ToLinear(color.r), ToLinear(color.g), ToLinear(color.b));
	}

	void RunFrame(GPUContext& gpu, const Ref<AssetManagerBase>& manager, SceneRenderer& renderer, Scene& scene, const SceneCamera& camera)
	{
		AssetManager::SetActive(manager);
		REQUIRE(gpu.GetDevice().BeginFrame());
		Strata::Renderer::BeginFrame();
		manager->Update();
		REQUIRE(renderer.Render(scene, camera));
		gpu.GetDevice().EndFrame();
	}

}

TEST_SUITE("GPU.SceneRenderer.ReflectionProbes")
{
	TEST_CASE("A scene without reflection probes samples a one-slot black cube array and makes nothing else")
	{
		// Every scene shader's layout holds the probes' cube array (the device's imageCubeArray feature is required): without
		// probes in view the renderer binds the renderer's shared one-slot black array, makes no array of its own, converts
		// nothing, and draws the same image as without any probe.
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
		scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().EnvironmentMap = assets.Add(CreateEnvironmentTexture(glm::vec3(0.2f, 0.3f, 0.6f),
			glm::vec3(0.1f)), "Sky");
		AddMesh(scene, BuiltinAssets::SphereMesh, BuiltinAssets::DefaultMaterial, glm::vec3(0.0f), "Ball");
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f));
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		const ReadbackImage reference = RenderToImage(renderer, scene, camera);
		const glm::u8vec4 ball = GetPixelRGBA8(reference, c_Size / 2, c_Size / 2);
		CHECK(ball.r + ball.g + ball.b > 30);
		CHECK(renderer.GetReflectionProbeArray() == nullptr);
		CHECK(renderer.GetStats().ReflectionProbes == 0);

		// A probe without a capture, and one out of view: nothing changes.
		Entity unset = AddProbe(scene, AssetHandle(0), glm::vec3(0.0f), glm::vec3(5.0f));
		AddProbe(scene, AddCapture(assets, glm::vec3(1.0f, 0.0f, 0.0f)), glm::vec3(0.0f, 0.0f, 100.0f), glm::vec3(2.0f));
		const ReadbackImage image = RenderToImage(renderer, scene, camera);
		CHECK(image.Pixels == reference.Pixels);
		CHECK(renderer.GetReflectionProbeArray() == nullptr);
		const SceneRendererStats stats = renderer.GetStats();
		CHECK(stats.ReflectionProbes == 0);
		CHECK(stats.ReflectionProbeConversions == 0);
		CHECK(stats.ReflectionProbesPending == 0);
		CHECK(unset.IsValid());
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A probe whose capture is missing or not a texture lights nothing and waits for nothing")
	{
		// Only captures on their way count as pending (captures, the project's picture and the bake wait for those): an unknown
		// handle (a deleted texture) or another type of asset never loads as a capture.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::SphereMesh, BuiltinAssets::DefaultMaterial, glm::vec3(0.0f), "Ball");
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f));
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		const ReadbackImage reference = RenderToImage(renderer, scene, camera);

		AddProbe(scene, AssetHandle(0x5EC0DE), glm::vec3(0.0f), glm::vec3(5.0f));
		MaterialProperties red;
		red.BaseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
		AddProbe(scene, assets.AddMaterial(red), glm::vec3(0.0f), glm::vec3(4.0f));
		for (int frame = 0; frame < 3; frame++)
		{
			const ReadbackImage image = RenderToImage(renderer, scene, camera);
			const SceneRendererStats stats = renderer.GetStats();
			CHECK(stats.ReflectionProbes == 0);
			CHECK(stats.ReflectionProbeConversions == 0);
			CHECK(stats.ReflectionProbesPending == 0);
			CHECK(stats.PendingAssets == 0);
			CHECK(image.Pixels == reference.Pixels);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A frame with HDROutput ends with its pre-exposed HDR image and makes no output")
	{
		// What the probe bake reads back: luminance times the forced pre-exposure, before exposure and tone mapping.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		Entity wall = AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(Emitting(3.0f)), glm::vec3(0.0f), "Wall");
		wall.GetComponent<TransformComponent>().Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
		wall.GetComponent<TransformComponent>().Scale = glm::vec3(10.0f);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f));
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		SceneRenderOptions options;
		options.HDROutput = true;
		options.PreExposure = 0.25f;
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		CHECK(renderer.GetOutputTexture() == nullptr);
		CHECK(renderer.GetStats().PreExposure == 0.25f);
		ReadbackImage hdr;
		REQUIRE(Strata::Renderer::ReadTexture(renderer.GetHDRTexture(), hdr));
		const glm::vec4 texel = ReadHalfTexel(hdr, c_Size / 2, c_Size / 2);
		CHECK(texel.r == doctest::Approx(0.75f).epsilon(0.002));
		CHECK(texel.g == doctest::Approx(0.75f).epsilon(0.002));
		// A frame with HDROutput has no target.
		Scope<SceneRenderer> other = CreateScope<SceneRenderer>(ExactSpecification());
		other->SetViewportSize(c_Size, c_Size);
		REQUIRE(other->Render(scene, camera));
		nvrhi::FramebufferHandle target = gpu.GetDevice().GetDevice()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(other->GetOutputTexture()));
		REQUIRE(target);
		CHECK_FALSE(renderer.Render(scene, camera, target, options));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A mirror ball inside a probe's box reflects the probe's capture, and one outside it the sky")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		// A blue sky, a red capture.
		scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().EnvironmentMap = assets.Add(CreateEnvironmentTexture(glm::vec3(0.0f, 0.0f, 0.8f),
			glm::vec3(0.0f, 0.0f, 0.8f)), "Sky");
		AddProbe(scene, AddCapture(assets, glm::vec3(0.8f, 0.0f, 0.0f)), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(1.0f));
		AddMirrorBall(scene, assets, glm::vec3(-1.0f, 0.0f, 0.0f));
		AddMirrorBall(scene, assets, glm::vec3(1.0f, 0.0f, 0.0f));
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 4.0f), glm::vec3(0.0f));
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		const ReadbackImage image = RenderToImage(renderer, scene, camera);
		const SceneRendererStats stats = renderer.GetStats();
		CHECK(stats.ReflectionProbes == 1);
		CHECK(stats.ReflectionProbeConversions == 1);
		CHECK(stats.ReflectionProbesPending == 0);
		REQUIRE(renderer.GetReflectionProbeArray() != nullptr);
		CHECK(renderer.GetReflectionProbeArray()->getDesc().dimension == nvrhi::TextureDimension::TextureCubeArray);

		const glm::u8vec4 inside = GetPixelRGBA8(image, ProjectToPixel(camera, glm::vec3(-1.0f, 0.0f, 0.5f), c_Size).x,
			ProjectToPixel(camera, glm::vec3(-1.0f, 0.0f, 0.5f), c_Size).y);
		const glm::u8vec4 outside = GetPixelRGBA8(image, ProjectToPixel(camera, glm::vec3(1.0f, 0.0f, 0.5f), c_Size).x,
			ProjectToPixel(camera, glm::vec3(1.0f, 0.0f, 0.5f), c_Size).y);
		CAPTURE(inside);
		CAPTURE(outside);
		CHECK(inside.r > 150);
		CHECK(inside.b < 20);
		CHECK(outside.b > 150);
		CHECK(outside.r < 20);

		// The next frame keeps the converted capture: nothing is converted again.
		const ReadbackImage again = RenderToImage(renderer, scene, camera);
		CHECK(renderer.GetStats().ReflectionProbeConversions == 0);
		CHECK(again.Pixels == image.Pixels);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Two overlapping probes blend by their weights, the stronger over the weaker")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		// The ball sits halfway into the red probe's blend band (its face at x = 0, 2 m of blend: weight 0.5 at x = -1) and
		// deep inside the larger green probe: red takes half, green half of the rest. The red one is stronger by its priority.
		const glm::vec3 red(0.8f, 0.0f, 0.0f);
		const glm::vec3 green(0.0f, 0.8f, 0.0f);
		AddProbe(scene, AddCapture(assets, red), glm::vec3(-5.0f, 0.0f, 0.0f), glm::vec3(5.0f, 20.0f, 20.0f), 2.0f, 1);
		AddProbe(scene, AddCapture(assets, green), glm::vec3(0.0f), glm::vec3(20.0f), 0.0f, 0);
		AddMirrorBall(scene, assets, glm::vec3(-1.0f, 0.0f, 0.0f));
		const SceneCamera camera = LookAt(glm::vec3(-1.0f, 0.0f, 4.0f), glm::vec3(-1.0f, 0.0f, 0.0f));
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		const ReadbackImage image = RenderToImage(renderer, scene, camera);
		CHECK(renderer.GetStats().ReflectionProbes == 2);
		// The ball's front (x = -1): a smooth metal reflects the blend, f0 = 1.
		const glm::vec3 pixel = LinearPixel(image, glm::uvec2(c_Size / 2, c_Size / 2));
		CAPTURE(pixel);
		CHECK(pixel.r == doctest::Approx(0.5f * red.r).epsilon(0.06));
		CHECK(pixel.g == doctest::Approx(0.5f * green.g).epsilon(0.06));
		CHECK(pixel.b < 0.01f);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Box projection reflects a capture's walls where they are")
	{
		// The capture is green toward +X and red toward -X, as seen from the box's center. A mirror near the box's -X face,
		// facing +Z, reflects a ray leaning slightly toward +X: projected onto the box, it meets the +Z face still on the -X
		// side of the center, so it shows red; read at infinity it would show green.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		const AssetHandle capture = assets.Add(CreateCapture([](const glm::vec3& direction)
		{
			return direction.x > 0.0f ? glm::vec3(0.0f, 0.8f, 0.0f) : glm::vec3(0.8f, 0.0f, 0.0f);
		}), "Walls");
		Entity probeEntity = AddProbe(scene, capture, glm::vec3(0.0f), glm::vec3(5.0f));
		MaterialProperties mirror;
		mirror.Metallic = 1.0f;
		mirror.Roughness = 0.0f;
		Entity plane = AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(mirror), glm::vec3(-4.0f, 0.0f, -2.0f), "Mirror");
		TransformComponent& transform = plane.GetComponent<TransformComponent>();
		transform.Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f)); // The plane's +Y toward +Z
		transform.Scale = glm::vec3(0.5f);
		const SceneCamera camera = LookAt(glm::vec3(-4.6f, 0.0f, 1.0f), glm::vec3(-4.0f, 0.0f, -2.0f));
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);

		const ReadbackImage projected = RenderToImage(renderer, scene, camera);
		const glm::u8vec4 wall = GetPixelRGBA8(projected, c_Size / 2, c_Size / 2);
		CAPTURE(wall);
		CHECK(wall.r > 150);
		CHECK(wall.g < 30);

		probeEntity.GetComponent<ReflectionProbeComponent>().BoxProjection = false;
		const ReadbackImage infinite = RenderToImage(renderer, scene, camera);
		const glm::u8vec4 far = GetPixelRGBA8(infinite, c_Size / 2, c_Size / 2);
		CAPTURE(far);
		CHECK(far.g > 150);
		CHECK(far.r < 30);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A probe's irradiance lights a matte ball as its capture's irradiance does")
	{
		// The same smooth environment as the sky (its irradiance map) and as a probe's capture (its spherical harmonics): a matte
		// ball inside the probe's box looks the same, within 2%.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const auto radiance = [](const glm::vec3& direction)
		{
			return glm::vec3(0.5f + 0.3f * direction.y, 0.6f + 0.3f * direction.y, 0.8f + 0.2f * direction.y + 0.1f * direction.x);
		};
		const AssetHandle environment = assets.Add(CreateCapture(radiance), "Environment");
		MaterialProperties matte;
		matte.BaseColor = glm::vec4(1.0f);
		matte.Metallic = 0.0f;
		matte.Roughness = 1.0f;
		const AssetHandle material = assets.AddMaterial(matte);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.0f, 3.0f), glm::vec3(0.0f));

		Scene skyLit;
		AddNeutralPostProcess(skyLit);
		SkyLightComponent& sky = skyLit.CreateEntity("Sky").AddComponent<SkyLightComponent>();
		sky.EnvironmentMap = environment;
		sky.ShowBackground = false; // As the probe's scene, which draws no sky
		AddMesh(skyLit, BuiltinAssets::SphereMesh, material, glm::vec3(0.0f), "Ball");
		Scene probeLit;
		AddNeutralPostProcess(probeLit);
		AddProbe(probeLit, environment, glm::vec3(0.0f), glm::vec3(10.0f));
		AddMesh(probeLit, BuiltinAssets::SphereMesh, material, glm::vec3(0.0f), "Ball");

		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		const ReadbackImage reference = RenderToImage(renderer, skyLit, camera);
		const ReadbackImage image = RenderToImage(renderer, probeLit, camera);
		CHECK(renderer.GetStats().ReflectionProbes == 1);
		uint32_t compared = 0;
		for (uint32_t y = 0; y < c_Size; y++)
		{
			for (uint32_t x = 0; x < c_Size; x++)
			{
				const glm::vec3 expected = LinearPixel(reference, glm::uvec2(x, y));
				if (expected.r + expected.g + expected.b < 0.3f)
					continue; // The background and the ball's darkest rim
				const glm::vec3 actual = LinearPixel(image, glm::uvec2(x, y));
				for (int channel = 0; channel < 3; channel++)
				{
					CAPTURE(x);
					CAPTURE(y);
					CHECK(std::abs(actual[channel] - expected[channel]) <= 0.02f * expected[channel] + 1.0f / 255.0f);
				}
				compared++;
			}
		}
		CHECK(compared > 100);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A probe fading out into the constant ambient meets it without a step")
	{
		// Without an environment the sky light's constant ambient lights diffuse surfaces only (no specular share). A probe of
		// almost no weight (its blend distance far beyond its box) must leave a matte ball as the ambient alone lights it, so
		// that its box's faces show no seam.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		MaterialProperties matte;
		matte.BaseColor = glm::vec4(1.0f);
		matte.Metallic = 0.0f;
		matte.Roughness = 1.0f;
		const AssetHandle material = assets.AddMaterial(matte);
		const AssetHandle black = AddCapture(assets, glm::vec3(0.0f));
		Scene scene;
		AddNeutralPostProcess(scene);
		SkyLightComponent& sky = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
		sky.AmbientColor = glm::vec3(0.6f);
		AddMesh(scene, BuiltinAssets::SphereMesh, material, glm::vec3(0.0f), "Ball");
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f));
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		const ReadbackImage reference = RenderToImage(renderer, scene, camera);
		const glm::vec3 lit = LinearPixel(reference, glm::uvec2(c_Size / 2, c_Size / 2));
		CAPTURE(lit);
		REQUIRE(lit.r > 0.3f);

		AddProbe(scene, black, glm::vec3(0.0f), glm::vec3(10.0f), 1.0e5f);
		const ReadbackImage image = RenderToImage(renderer, scene, camera);
		CHECK(renderer.GetStats().ReflectionProbes == 1);
		uint32_t compared = 0;
		for (uint32_t y = 0; y < c_Size; y++)
		{
			for (uint32_t x = 0; x < c_Size; x++)
			{
				const glm::vec3 expected = LinearPixel(reference, glm::uvec2(x, y));
				if (expected.r < 0.1f)
					continue; // The background
				const glm::vec3 actual = LinearPixel(image, glm::uvec2(x, y));
				CAPTURE(x);
				CAPTURE(y);
				CHECK(std::abs(actual.r - expected.r) <= 0.01f * expected.r + 1.0f / 255.0f);
				compared++;
			}
		}
		CHECK(compared > 100);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("With more probes in view than slots the strongest are kept")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		// Three probes side by side, each around a mirror ball; the weakest (lowest priority) is left out.
		const glm::vec3 colors[3] = { glm::vec3(0.8f, 0.0f, 0.0f), glm::vec3(0.0f, 0.8f, 0.0f), glm::vec3(0.0f, 0.0f, 0.8f) };
		const float positions[3] = { -1.2f, 0.0f, 1.2f };
		const int32_t priorities[3] = { 2, 0, 1 };
		for (int index = 0; index < 3; index++)
		{
			AddProbe(scene, AddCapture(assets, colors[index]), glm::vec3(positions[index], 0.0f, 0.0f), glm::vec3(0.55f), 0.0f, priorities[index]);
			AddMirrorBall(scene, assets, glm::vec3(positions[index], 0.0f, 0.0f)).GetComponent<TransformComponent>().Scale = glm::vec3(0.8f);
		}
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 4.0f), glm::vec3(0.0f));
		SceneRendererSpecification specification = ExactSpecification();
		specification.MaxReflectionProbes = 2;
		SceneRenderer renderer(specification);
		renderer.SetViewportSize(c_Size, c_Size);
		const ReadbackImage image = RenderToImage(renderer, scene, camera);
		CHECK(renderer.GetStats().ReflectionProbes == 2);
		CHECK(renderer.GetReflectionProbeArray()->getDesc().arraySize == 12);
		for (int index = 0; index < 3; index++)
		{
			CAPTURE(index);
			const glm::uvec2 pixel = ProjectToPixel(camera, glm::vec3(positions[index], 0.0f, 0.4f), c_Size);
			const glm::vec3 color = LinearPixel(image, pixel);
			CAPTURE(color);
			if (priorities[index] == 0)
				CHECK(color.r + color.g + color.b < 0.05f); // No probe and no sky: only the dim constant ambient
			else
				CHECK(glm::dot(color, glm::normalize(colors[index])) > 0.4f);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Spread over frames at most two captures convert per frame, else all in the first")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		for (int index = 0; index < 5; index++)
			AddProbe(scene, AddCapture(assets, glm::vec3(0.1f * static_cast<float>(index + 1))), glm::vec3(static_cast<float>(index) - 2.0f, 0.0f, 0.0f),
				glm::vec3(0.4f));
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 6.0f), glm::vec3(0.0f));

		SceneRenderer spread(ExactSpecification());
		spread.SetViewportSize(c_Size, c_Size);
		const uint32_t expected[3][3] = { { 2, 2, 3 }, { 2, 4, 1 }, { 1, 5, 0 } }; // Conversions, probes shaded, probes pending
		for (const auto& frame : expected)
		{
			REQUIRE(spread.Render(scene, camera));
			const SceneRendererStats stats = spread.GetStats();
			CHECK(stats.ReflectionProbeConversions == frame[0]);
			CHECK(stats.ReflectionProbes == frame[1]);
			CHECK(stats.ReflectionProbesPending == frame[2]);
		}

		SceneRendererSpecification single = ExactSpecification();
		single.SpreadLightingUpdates = false;
		SceneRenderer atOnce(single);
		atOnce.SetViewportSize(c_Size, c_Size);
		const ReadbackImage first = RenderToImage(atOnce, scene, camera);
		CHECK(atOnce.GetStats().ReflectionProbeConversions == 5);
		CHECK(atOnce.GetStats().ReflectionProbes == 5);
		CHECK(atOnce.GetStats().ReflectionProbesPending == 0);
		// The spread renderer's image matches once all are converted.
		const ReadbackImage settled = RenderToImage(spread, scene, camera);
		CHECK(first.Pixels == settled.Pixels);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A capture only the first of a frame's views waits for is still held whole until the streamer raises it")
	{
		// A probe bake renders six views per frame with one renderer. The view that sees the probe holds its capture whole; the
		// next view, which sees no probe, must not end that hold before the streamer's update between frames raises it.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		constexpr AssetHandle c_CaptureHandle = AssetHandle(0xA3101);
		const std::filesystem::path pack = WriteStreamingPack({ StreamingPackAsset { c_CaptureHandle, AssetType::Texture, "Lighting/Probe.hdr",
			CookEnvironmentMap(512, 256) } });
		std::string error;
		Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(pack, &error);
		REQUIRE_MESSAGE(manager, error);
		Scope<Scene> scene = CreateScope<Scene>("Probe");
		AddNeutralPostProcess(*scene);
		AddProbe(*scene, c_CaptureHandle, glm::vec3(0.0f), glm::vec3(5.0f));
		AddMesh(*scene, BuiltinAssets::SphereMesh, BuiltinAssets::DefaultMaterial, glm::vec3(0.0f), "Ball");
		const SceneCamera toward = LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f));
		const SceneCamera away = LookAt(glm::vec3(0.0f, 0.0f, 30.0f), glm::vec3(0.0f, 0.0f, 60.0f));
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);

		// By the clock, as slow machines take many frames to read the levels on I/O jobs.
		uint32_t conversions = 0;
		const auto start = std::chrono::steady_clock::now();
		while (conversions == 0 && std::chrono::steady_clock::now() - start < std::chrono::seconds(30))
		{
			RunFrame(gpu, manager, renderer, *scene, toward);
			conversions += renderer.GetStats().ReflectionProbeConversions;
			REQUIRE(renderer.Render(*scene, away)); // In the same frame (Renderer::GetFrameNumber)
			CHECK(renderer.GetStats().ReflectionProbes == 0);
		}
		CHECK(conversions == 1);

		AssetManager::SetActive(nullptr);
		scene.reset();
		manager.reset();
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A probe's capture is held whole until converted, then drops to its tail")
	{
		// The capture streams like any texture: requested through the frame's resolve cache, it arrives with its tail; nothing
		// draws it, so only the probe cache's hold raises it whole for the conversion. Once converted the hold ends and its
		// levels drop to the tail again, while the probe keeps lighting from its slot.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		constexpr AssetHandle c_CaptureHandle = AssetHandle(0xA3100);
		const std::filesystem::path pack = WriteStreamingPack({ StreamingPackAsset { c_CaptureHandle, AssetType::Texture, "Lighting/Probe.hdr",
			CookEnvironmentMap(512, 256) } });
		std::string error;
		Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(pack, &error);
		REQUIRE_MESSAGE(manager, error);
		TextureStreamer* streamer = TextureStreamer::Find(*manager);
		REQUIRE(streamer);
		Scope<Scene> scene = CreateScope<Scene>("Probe");
		AddNeutralPostProcess(*scene);
		AddProbe(*scene, c_CaptureHandle, glm::vec3(0.0f), glm::vec3(5.0f));
		AddMesh(*scene, BuiltinAssets::SphereMesh, BuiltinAssets::DefaultMaterial, glm::vec3(0.0f), "Ball");
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f));
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);

		// Until it converted (its levels load on I/O jobs): by the clock, as slow machines take many frames.
		uint32_t conversions = 0;
		bool sawPending = false;
		const auto loadStart = std::chrono::steady_clock::now();
		while (conversions == 0 && std::chrono::steady_clock::now() - loadStart < std::chrono::seconds(60))
		{
			RunFrame(gpu, manager, renderer, *scene, camera);
			conversions += renderer.GetStats().ReflectionProbeConversions;
			sawPending = sawPending || renderer.GetStats().ReflectionProbesPending > 0;
		}
		REQUIRE(conversions == 1);
		CHECK(sawPending); // It arrived with its tail first
		const Ref<Texture> capture = std::dynamic_pointer_cast<Texture>(manager->PeekLoadedAsset(c_CaptureHandle));
		REQUIRE(capture);
		REQUIRE(capture->GetTailLevel() > 0);
		CHECK(capture->GetResidentLevel() == 0); // Raised whole by the hold
		CHECK(manager->GetStats().Detail.LevelsRaised == capture->GetTailLevel());

		// Released after the conversion: after the streamer's idle drop time its levels drop to the tail, and the probe keeps its
		// slot without converting again.
		streamer->SetIdleDropSeconds(0.2);
		const auto start = std::chrono::steady_clock::now();
		bool dropped = false;
		while (!dropped && std::chrono::steady_clock::now() - start < std::chrono::seconds(30))
		{
			RunFrame(gpu, manager, renderer, *scene, camera);
			CHECK(renderer.GetStats().ReflectionProbeConversions == 0);
			CHECK(renderer.GetStats().ReflectionProbes == 1);
			dropped = capture->GetResidentLevel() == capture->GetTailLevel();
		}
		CHECK(dropped);

		AssetManager::SetActive(nullptr);
		scene.reset();
		manager.reset();
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
