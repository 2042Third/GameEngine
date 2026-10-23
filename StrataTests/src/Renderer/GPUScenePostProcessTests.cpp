#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/RendererComponents.h"

#include <cstring>
#include <optional>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = c_SceneTestSize;
	constexpr int c_MiddleGray = 118; // 0.18 encoded as sRGB

	// A box resting on a floor, lit by ambient light only, so ambient occlusion is all that differs between pixels.
	struct CreaseScene
	{
		Scene SceneData;
		PostProcessComponent* PostProcess = nullptr;
		SceneCamera Camera;

		CreaseScene()
		{
			PostProcess = &AddNeutralPostProcess(SceneData);
			PostProcess->AmbientOcclusion = true;
			AddMesh(SceneData, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(20.0f);
			AddMesh(SceneData, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f, 0.5f, 0.0f), "Box");
			SceneData.CreateEntity("Sky").AddComponent<SkyLightComponent>().AmbientColor = glm::vec3(0.5f);
			Camera = LookAt(glm::vec3(0.0f, 2.5f, 3.0f), glm::vec3(0.0f, 0.3f, 0.0f));
		}

		glm::vec3 CreasePoint() const { return glm::vec3(0.0f, 0.0f, 0.56f); } // Floor at the box's front face
		glm::vec3 OpenFloorPoint() const { return glm::vec3(1.3f, 0.0f, 1.2f); } // More than the occlusion radius from the box
	};

	// Renders a frame with the pre-exposure forced (or chosen by the renderer) and reads the output back.
	ReadbackImage RenderPreExposed(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera, std::optional<float> preExposure)
	{
		SceneRenderOptions options;
		options.PreExposure = preExposure;
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
		return image;
	}

	ReadbackImage ReadHDRImage(SceneRenderer& renderer)
	{
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), image));
		REQUIRE(image.BytesPerPixel == 8);
		return image;
	}

	// The exposure buffer's floats (Include/Exposure.glsl): the adapted log2 luminance and the multiplier.
	glm::vec2 ReadExposure(GPUContext& gpu, SceneRenderer& renderer)
	{
		const std::vector<uint8_t> bytes = ReadBufferRange(gpu, renderer.GetExposureBuffer(), 0, 2 * sizeof(float));
		REQUIRE(bytes.size() == 2 * sizeof(float));
		glm::vec2 values;
		std::memcpy(&values, bytes.data(), sizeof(values));
		return values;
	}

	// Pixels of an HDR image with a channel at SanitizeHDR's clamp.
	uint32_t CountClampedPixels(const ReadbackImage& hdr)
	{
		uint32_t count = 0;
		for (uint32_t y = 0; y < hdr.Height; y++)
		{
			for (uint32_t x = 0; x < hdr.Width; x++)
			{
				const glm::vec4 texel = ReadHalfTexel(hdr, x, y);
				count += std::max({ texel.r, texel.g, texel.b }) >= 64000.0f ? 1u : 0u;
			}
		}
		return count;
	}

	// The automatic pre-exposure settles from readbacks of the renderer's own metering, frames later: frames until it did (each
	// waited for, as RenderToImage does), at most `frames`.
	uint32_t RenderUntilPreExposureSettled(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera, uint32_t frames)
	{
		for (uint32_t frame = 1; frame <= frames; frame++)
		{
			RenderToImage(renderer, scene, camera);
			if (renderer.GetStats().PreExposureSettled)
				return frame;
		}
		return frames + 1;
	}

}

TEST_SUITE("GPU.SceneRenderer.PostProcess")
{
	TEST_CASE("Automatic exposure maps the average brightness to middle gray and adapts over time")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const AssetHandle dim = assets.AddMaterial(Emitting(0.5f));
		const AssetHandle bright = assets.AddMaterial(Emitting(8.0f));
		const AssetHandle veryDark = assets.AddMaterial(Emitting(1.0f / 256.0f));

		Scene scene;
		PostProcessComponent& postProcess = AddNeutralPostProcess(scene);
		postProcess.AutoExposure = true;
		Entity wall = AddMesh(scene, BuiltinAssets::QuadMesh, dim, glm::vec3(0.0f), "Wall");
		wall.GetComponent<TransformComponent>().Scale = glm::vec3(20.0f); // Fills the view

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f));
		auto renderCenter = [&](bool reset)
		{
			if (reset)
				renderer.ResetExposureAdaptation();
			return Red(RenderToImage(renderer, scene, camera), c_Size / 2, c_Size / 2);
		};

		// Different brightness, same image.
		CHECK(std::abs(renderCenter(true) - c_MiddleGray) <= 4);
		wall.GetComponent<MeshRendererComponent>().Material = bright;
		wall.MarkModified<MeshRendererComponent>();
		const int adapting = renderCenter(false); // Still exposed for the dim wall
		CHECK(adapting > 200);
		CHECK(std::abs(renderCenter(true) - c_MiddleGray) <= 4);

		// Exposure compensation shifts the result by stops.
		postProcess.Exposure = 1.0f;
		CHECK(std::abs(renderCenter(true) - 161) <= 5); // 0.36 encoded as sRGB
		postProcess.Exposure = 0.0f;

		// Scenes darker than the allowed range stay dark.
		wall.GetComponent<MeshRendererComponent>().Material = veryDark;
		wall.MarkModified<MeshRendererComponent>();
		CHECK(renderCenter(true) < 60);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Automatic exposure meters the lit content, not the black background around it")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		PostProcessComponent& postProcess = AddNeutralPostProcess(scene);
		postProcess.AutoExposure = true;
		// A small object (about 4% of the image) on a black background.
		AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(4.0f)), glm::vec3(0.0f), "Lamp").GetComponent<TransformComponent>().Scale = glm::vec3(0.25f);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const ReadbackImage image = RenderToImage(renderer, scene, LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f)));
		CHECK(std::abs(Red(image, c_Size / 2, c_Size / 2) - c_MiddleGray) <= 4);
		CHECK(Red(image, 2, 2) == 0);

		// A completely black image keeps a defined exposure.
		Scene empty;
		AddNeutralPostProcess(empty).AutoExposure = true;
		renderer.ResetExposureAdaptation();
		CHECK(Red(RenderToImage(renderer, empty, LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f))), c_Size / 2, c_Size / 2) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Contrast is applied around middle gray")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		PostProcessComponent& postProcess = AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(0.36f)), glm::vec3(0.0f), "Wall").GetComponent<TransformComponent>().Scale = glm::vec3(20.0f);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f));
		auto renderCenter = [&]() { return Red(RenderToImage(renderer, scene, camera), c_Size / 2, c_Size / 2); };
		CHECK(std::abs(renderCenter() - 161) <= 2); // 0.36
		postProcess.Contrast = 2.0f;
		CHECK(std::abs(renderCenter() - 220) <= 3); // 0.18 * 2^2 = 0.72
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Bloom spreads bright light around its source")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		PostProcessComponent& postProcess = AddNeutralPostProcess(scene);
		Entity light = AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(200.0f)), glm::vec3(0.0f), "Light");
		light.GetComponent<TransformComponent>().Scale = glm::vec3(0.1f);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f));
		auto render = [&]() { return RenderToImage(renderer, scene, camera); };

		const ReadbackImage sharp = render();
		CHECK(Red(sharp, c_Size / 2, c_Size / 2) == 255);
		CHECK(Red(sharp, c_Size / 2 + 6, c_Size / 2) == 0);

		postProcess.Bloom = true;
		postProcess.BloomIntensity = 0.1f;
		const ReadbackImage glowing = render();
		CHECK(Red(glowing, c_Size / 2, c_Size / 2) == 255);
		const int nearSource = Red(glowing, c_Size / 2 + 6, c_Size / 2);
		const int farFromSource = Red(glowing, c_Size / 2 + 20, c_Size / 2);
		CHECK(nearSource > 20);
		CHECK(nearSource > farFromSource);

		// A threshold above the light's brightness leaves nothing to bloom.
		postProcess.BloomThreshold = 1000.0f;
		CHECK(Red(render(), c_Size / 2 + 6, c_Size / 2) <= 1);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Bloom with a threshold adds glow without dimming the rest of the image")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		PostProcessComponent& postProcess = AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(0.18f)), glm::vec3(0.0f, 0.0f, -0.1f), "Wall")
			.GetComponent<TransformComponent>().Scale = glm::vec3(20.0f);
		Entity light = AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(0.5f)), glm::vec3(0.0f), "Light");
		light.GetComponent<TransformComponent>().Scale = glm::vec3(0.1f);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f));
		const ReadbackImage plain = RenderToImage(renderer, scene, camera);
		CHECK(std::abs(Red(plain, 2, 2) - c_MiddleGray) <= 1);

		// Nothing exceeds the threshold: the image is unchanged (mixing in the empty bloom would darken it).
		postProcess.Bloom = true;
		postProcess.BloomIntensity = 0.5f;
		postProcess.BloomThreshold = 1.0f;
		CHECK(MaxDifference(RenderToImage(renderer, scene, camera), plain) <= 1);

		// A light above the threshold glows; the glow only ever adds light.
		light.GetComponent<MeshRendererComponent>().Material = assets.AddMaterial(Emitting(500.0f));
		light.MarkModified<MeshRendererComponent>();
		postProcess.Bloom = false;
		const ReadbackImage sharp = RenderToImage(renderer, scene, camera);
		postProcess.Bloom = true;
		const ReadbackImage glowing = RenderToImage(renderer, scene, camera);
		CHECK(Red(glowing, c_Size / 2 + 6, c_Size / 2) > Red(sharp, c_Size / 2 + 6, c_Size / 2) + 20);
		for (const glm::uvec2& pixel : { glm::uvec2(2, 2), glm::uvec2(c_Size - 3, 2), glm::uvec2(2, c_Size - 3), glm::uvec2(c_Size - 3, c_Size - 3) })
		{
			CHECK(Red(glowing, pixel) >= Red(sharp, pixel));
			CHECK(Red(glowing, pixel) < Red(glowing, c_Size / 2 + 6, c_Size / 2));
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("FXAA smooths aliased edges and keeps flat areas")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		PostProcessComponent& postProcess = AddNeutralPostProcess(scene);
		Entity quad = AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(UnlitColor({ 1.0f, 1.0f, 1.0f, 1.0f })), glm::vec3(0.0f), "Quad");
		quad.GetComponent<TransformComponent>().Rotation = glm::angleAxis(glm::radians(20.0f), glm::vec3(0.0f, 0.0f, 1.0f));

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f));
		auto countPartial = [](const ReadbackImage& image)
		{
			uint32_t count = 0;
			for (uint32_t y = 0; y < image.Height; y++)
			{
				for (uint32_t x = 0; x < image.Width; x++)
				{
					const int value = Red(image, x, y);
					count += value > 20 && value < 235 ? 1u : 0u;
				}
			}
			return count;
		};

		const ReadbackImage aliased = RenderToImage(renderer, scene, camera);
		CHECK(countPartial(aliased) == 0);
		postProcess.AntiAliasing = true;
		const ReadbackImage smoothed = RenderToImage(renderer, scene, camera);
		CHECK(countPartial(smoothed) > 20);
		CHECK(GetPixelRGBA8(smoothed, c_Size / 2, c_Size / 2) == glm::u8vec4(255, 255, 255, 255));
		CHECK(GetPixelRGBA8(smoothed, 1, 1) == glm::u8vec4(0, 0, 0, 255));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Ambient occlusion darkens creases and leaves open surfaces untouched")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		CreaseScene crease;

		constexpr uint32_t size = 128;
		SceneRenderer renderer;
		renderer.SetViewportSize(size, size);
		const glm::uvec2 creasePixel = ProjectToPixel(crease.Camera, crease.CreasePoint(), size);
		const glm::uvec2 openFloor = ProjectToPixel(crease.Camera, crease.OpenFloorPoint(), size);
		const glm::uvec2 boxTop = ProjectToPixel(crease.Camera, glm::vec3(0.0f, 1.0f, 0.0f), size);

		const ReadbackImage withOcclusion = RenderToImage(renderer, crease.SceneData, crease.Camera);
		ReadbackImage occlusion;
		REQUIRE(Renderer::ReadTexture(renderer.GetAmbientOcclusionTexture(), occlusion));
		// Half precision holds occlusion values: devices that store r16f images get a target half the size.
		const bool halfPrecision = gpu.GetDevice().GetInfo().SupportsExtendedStorageFormats;
		CHECK(occlusion.Format == (halfPrecision ? nvrhi::Format::R16_FLOAT : nvrhi::Format::R32_FLOAT));
		CHECK(occlusion.BytesPerPixel == (halfPrecision ? 2u : 4u));
		CHECK(ReadOcclusionTexel(occlusion, creasePixel) < 0.85f);
		CHECK(ReadOcclusionTexel(occlusion, openFloor) > 0.95f);
		CHECK(ReadOcclusionTexel(occlusion, boxTop) > 0.95f);
		CHECK(ReadOcclusionTexel(occlusion, glm::uvec2(size / 2, 1)) == 1.0f); // Background

		crease.PostProcess->AmbientOcclusion = false;
		const ReadbackImage withoutOcclusion = RenderToImage(renderer, crease.SceneData, crease.Camera);
		const int creaseWith = Red(withOcclusion, creasePixel);
		const int creaseWithout = Red(withoutOcclusion, creasePixel);
		CHECK(creaseWithout > 100);
		CHECK(creaseWith < creaseWithout - 10);
		CHECK(std::abs(Red(withOcclusion, openFloor) - Red(withoutOcclusion, openFloor)) <= 3);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Transparent surfaces do not receive the ambient occlusion of the surfaces behind them")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		CreaseScene crease;
		// A lit, alpha-blended (but fully opaque) pane in front of the crease.
		MaterialProperties paneProperties;
		paneProperties.AlphaMode = MaterialAlphaMode::Blend;
		paneProperties.Roughness = 1.0f;
		Entity pane = AddMesh(crease.SceneData, BuiltinAssets::QuadMesh, assets.AddMaterial(paneProperties), glm::vec3(0.0f, 0.15f, 0.9f), "Pane");
		pane.GetComponent<TransformComponent>().Scale = glm::vec3(0.6f);
		pane.GetComponent<TransformComponent>().Rotation = Math::LookRotation(glm::normalize(glm::vec3(0.0f, 0.15f, 0.9f) - crease.Camera.Position));

		constexpr uint32_t size = 128;
		SceneRenderer renderer;
		renderer.SetViewportSize(size, size);
		const glm::uvec2 creasePixel = ProjectToPixel(crease.Camera, crease.CreasePoint(), size);
		const ReadbackImage withOcclusion = RenderToImage(renderer, crease.SceneData, crease.Camera);
		ReadbackImage occlusion;
		REQUIRE(Renderer::ReadTexture(renderer.GetAmbientOcclusionTexture(), occlusion));
		CHECK(ReadOcclusionTexel(occlusion, creasePixel) < 0.85f); // The crease behind the pane is occluded
		crease.PostProcess->AmbientOcclusion = false;
		const ReadbackImage withoutOcclusion = RenderToImage(renderer, crease.SceneData, crease.Camera);
		CHECK(Red(withOcclusion, creasePixel) > 50); // The pane covers the crease
		CHECK(std::abs(Red(withOcclusion, creasePixel) - Red(withoutOcclusion, creasePixel)) <= 1);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Bloom and ambient occlusion keep working after a resize")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		CreaseScene crease;
		crease.PostProcess->BloomIntensity = 0.1f;
		const glm::vec3 lampPosition(-1.0f, 0.4f, 1.2f);
		AddMesh(crease.SceneData, BuiltinAssets::SphereMesh, assets.AddMaterial(Emitting(100.0f)), lampPosition, "Lamp")
			.GetComponent<TransformComponent>().Scale = glm::vec3(0.2f);

		SceneRenderer renderer;
		for (const glm::uvec2& size : { glm::uvec2(64, 64), glm::uvec2(150, 90), glm::uvec2(104, 128) })
		{
			CAPTURE(size);
			renderer.SetViewportSize(size.x, size.y);
			SceneCamera camera = crease.Camera;
			camera.Projection = Math::PerspectiveReverseZ(glm::radians(60.0f), static_cast<float>(size.x) / static_cast<float>(size.y), 0.1f, 100.0f);
			crease.PostProcess->Bloom = false;
			const ReadbackImage sharp = RenderToImage(renderer, crease.SceneData, camera);
			crease.PostProcess->Bloom = true;
			const ReadbackImage glowing = RenderToImage(renderer, crease.SceneData, camera);
			REQUIRE(glowing.Height == size.y);

			ReadbackImage occlusion;
			REQUIRE(Renderer::ReadTexture(renderer.GetAmbientOcclusionTexture(), occlusion));
			CHECK(occlusion.Width == size.x);
			CHECK(ReadOcclusionTexel(occlusion, ProjectToPixel(camera, crease.CreasePoint(), size.x, size.y)) < 0.9f);
			CHECK(ReadOcclusionTexel(occlusion, ProjectToPixel(camera, crease.OpenFloorPoint(), size.x, size.y)) > 0.95f);

			ReadbackImage bloom;
			REQUIRE(Renderer::ReadTexture(renderer.GetBloomTexture(), bloom));
			CHECK(bloom.Width == (size.x + 1) / 2);
			// The lamp glows onto the floor beside it.
			const glm::uvec2 lamp = ProjectToPixel(camera, lampPosition, size.x, size.y);
			const glm::uvec2 beside(lamp.x, lamp.y + size.y / 8);
			CHECK(Red(glowing, beside) > Red(sharp, beside) + 10);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The full post-processing chain renders the same into an external framebuffer")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		PostProcessComponent& postProcess = scene.CreateEntity("PostProcess").AddComponent<PostProcessComponent>(); // Defaults: auto exposure, bloom, FXAA
		postProcess.BloomIntensity = 0.2f;
		AddMesh(scene, BuiltinAssets::SphereMesh, UUID::Null(), glm::vec3(-0.6f, 0.0f, 0.0f), "Sphere");
		Entity lamp = AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(100.0f)), glm::vec3(0.7f, 0.2f, 0.0f), "Lamp");
		lamp.GetComponent<TransformComponent>().Scale = glm::vec3(0.3f);
		lamp.GetComponent<TransformComponent>().Rotation = glm::angleAxis(glm::radians(25.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		AddSun(scene, glm::vec3(-0.3f, -0.5f, -1.0f), 3.0f);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f));
		renderer.ResetExposureAdaptation();
		const ReadbackImage internal = RenderToImage(renderer, scene, camera);

		nvrhi::TextureDesc desc;
		desc.width = c_Size;
		desc.height = c_Size;
		desc.format = nvrhi::Format::BGRA8_UNORM;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		nvrhi::TextureHandle target = gpu.GetNvrhiDevice()->createTexture(desc);
		REQUIRE(target);
		nvrhi::FramebufferHandle framebuffer = gpu.GetNvrhiDevice()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));
		renderer.ResetExposureAdaptation();
		REQUIRE(renderer.Render(scene, camera, framebuffer));
		ReadbackImage external;
		REQUIRE(Renderer::ReadTexture(target, external));

		// BGRA: swap red and blue back.
		for (size_t index = 0; index + 3 < external.Pixels.size(); index += 4)
			std::swap(external.Pixels[index], external.Pixels[index + 2]);
		CHECK(MaxDifference(internal, external) <= 1);
		// Every effect contributed: glow around the lamp, smoothed edges, an exposed sphere.
		const glm::uvec2 lampPixel = ProjectToPixel(camera, glm::vec3(0.7f, 0.2f, 0.0f), c_Size);
		CHECK(Red(internal, lampPixel.x, lampPixel.y - 8) > 0);
		CHECK(Red(internal, ProjectToPixel(camera, glm::vec3(-0.6f, 0.0f, 0.5f), c_Size)) > 60);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The pre-exposure is a power of two that changes no image: forced values, the clear color and the sky")
	{
		// Multiplying by a power of two is exact, so a scene renders the same at any pre-exposure while no value overflows or
		// becomes subnormal; the clear color is a luminance like the rest (a skyless view), and so is the sky.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		MaterialProperties matte;
		matte.BaseColor = glm::vec4(0.6f, 0.5f, 0.4f, 1.0f);
		matte.Roughness = 0.6f;
		const AssetHandle material = assets.AddMaterial(matte);
		const auto build = [&](Scene& scene, bool sky)
		{
			AddNeutralPostProcess(scene);
			AddMesh(scene, BuiltinAssets::PlaneMesh, material, glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(4.0f);
			AddMesh(scene, BuiltinAssets::SphereMesh, material, glm::vec3(0.0f, 0.5f, 0.0f), "Sphere");
			AddSun(scene, glm::vec3(0.4f, -1.0f, -0.3f), 2.0f);
			SkyLightComponent& light = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
			light.AmbientColor = glm::vec3(0.2f); // Keeps the shadows far from subnormal values at 2^-8
			if (sky)
			{
				light.Source = SkyLightSource::Procedural;
				light.Intensity = 0.5f;
			}
		};
		// The floor covers the lower part of the view, the sky or the clear color the upper part.
		SceneCamera camera = LookAt(glm::vec3(0.0f, 1.2f, 3.5f), glm::vec3(0.0f, 0.8f, 0.0f));
		camera.ClearColor = glm::vec4(0.35f, 0.45f, 0.6f, 1.0f);
		for (const bool sky : { false, true })
		{
			CAPTURE(sky);
			Scene scene;
			build(scene, sky);
			SceneRenderer renderer;
			renderer.SetViewportSize(c_Size, c_Size);
			const ReadbackImage eight = RenderPreExposed(renderer, scene, camera, 8.0f);
			CHECK(renderer.GetStats().PreExposure == 8.0f);
			CHECK(renderer.GetStats().PreExposureSettled);
			const ReadbackImage eightHDR = ReadHDRImage(renderer);
			const ReadbackImage sixteen = RenderPreExposed(renderer, scene, camera, 16.0f);
			CHECK(renderer.GetStats().PreExposure == 16.0f);
			const ReadbackImage sixteenHDR = ReadHDRImage(renderer);
			CHECK(sixteen.Pixels == eight.Pixels);
			// The HDR target holds the luminance times the pre-exposure, to the bit: twice as much at 2^4 as at 2^3.
			uint32_t doubled = 0;
			for (uint32_t y = 0; y < c_Size; y++)
			{
				for (uint32_t x = 0; x < c_Size; x++)
					doubled += ReadHalfTexel(sixteenHDR, x, y) == ReadHalfTexel(eightHDR, x, y) * glm::vec4(2.0f, 2.0f, 2.0f, 1.0f) ? 1u : 0u;
			}
			CHECK(doubled == c_Size * c_Size);
			// The neutral image at 2^-8 and 1 as well; values that are not powers of two are rounded to the nearest one.
			CHECK(RenderPreExposed(renderer, scene, camera, 1.0f / 256.0f).Pixels == eight.Pixels);
			CHECK(RenderPreExposed(renderer, scene, camera, 1.0f).Pixels == eight.Pixels);
			CHECK(RenderPreExposed(renderer, scene, camera, 11.0f).Pixels == eight.Pixels);
			CHECK(renderer.GetStats().PreExposure == 8.0f);
			RenderPreExposed(renderer, scene, camera, 0.0f);
			CHECK(renderer.GetStats().PreExposure == 1.0f);
			// The upper part shows the clear color or the sky.
			CHECK(eight.Pixels[(4 * c_Size + c_Size / 2) * 4 + 2] > 60);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A 100,000 lux sun with its disk in view reaches no HDR clamp once the pre-exposure settled")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		// A glossy metal floor toward the sun, low over the horizon: its disk above the horizon, its highlight on the floor below.
		MaterialProperties glossy;
		glossy.BaseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
		glossy.Metallic = 1.0f;
		glossy.Roughness = 0.15f;

		Scene scene;
		PostProcessComponent& postProcess = AddNeutralPostProcess(scene);
		postProcess.AutoExposure = true;
		postProcess.AutoExposureMaxEV = 16.0f;
		const glm::vec3 toSun = glm::normalize(glm::vec3(0.0f, 0.15f, -1.0f));
		AddSun(scene, -toSun, 100000.0f);
		SkyLightComponent& sky = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
		sky.Source = SkyLightSource::Procedural;
		sky.Intensity = 4000.0f; // A daylight sky: thousands of nits, its disk 80,000
		sky.SunSize = 3.0f;      // Wider than the test image's pixels
		AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(glossy), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(400.0f);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, -1.0f));
		const glm::uvec2 sunPixel = ProjectToPixel(camera, camera.Position + toSun * 100.0f, c_Size);
		REQUIRE(sunPixel.x < c_Size);
		REQUIRE(sunPixel.y < c_Size / 2);
		const auto brightestNearSun = [&](const ReadbackImage& hdr)
		{
			float brightest = 0.0f;
			for (uint32_t y = sunPixel.y - 2; y <= sunPixel.y + 2; y++)
			{
				for (uint32_t x = sunPixel.x - 2; x <= sunPixel.x + 2; x++)
					brightest = std::max(brightest, ReadHalfTexel(hdr, x, y).g);
			}
			return brightest;
		};

		// Without pre-exposure the disk and the highlight would be clamped.
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		RenderPreExposed(renderer, scene, camera, 1.0f);
		CHECK(CountClampedPixels(ReadHDRImage(renderer)) > 0);

		renderer.ResetExposureAdaptation();
		const uint32_t frames = RenderUntilPreExposureSettled(renderer, scene, camera, 8);
		CHECK(frames <= 4);
		CHECK(renderer.GetStats().PreExposure <= 1.0f / 4096.0f);
		const ReadbackImage hdr = ReadHDRImage(renderer);
		CHECK(CountClampedPixels(hdr) == 0);
		// The disk: tens of thousands of nits.
		CHECK(brightestNearSun(hdr) > 20000.0f * renderer.GetStats().PreExposure);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Automatic exposure meters a 65,536 lux scene 16 stops below a 1 lux one, whatever the pre-exposure")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		MaterialProperties matte;
		matte.BaseColor = glm::vec4(0.5f, 0.6f, 0.7f, 1.0f);
		matte.Roughness = 1.0f;
		const AssetHandle material = assets.AddMaterial(matte);
		const SceneCamera camera = LookAt(glm::vec3(2.0f, 7.0f, 3.0f), glm::vec3(0.0f));
		const auto build = [&](Scene& scene, float sun)
		{
			PostProcessComponent& postProcess = AddNeutralPostProcess(scene);
			postProcess.AutoExposure = true;
			postProcess.AutoExposureMaxEV = 16.0f;
			scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().AmbientColor = glm::vec3(0.0f);
			AddMesh(scene, BuiltinAssets::PlaneMesh, material, glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(100.0f);
			AddMesh(scene, BuiltinAssets::CubeMesh, material, glm::vec3(0.0f, 0.5f, 0.0f), "Box");
			AddSun(scene, glm::vec3(1.0f, -2.0f, 0.5f), sun);
		};
		struct Metered
		{
			glm::vec2 Exposure;
			ReadbackImage Image;
		};
		const auto meter = [&](float sun)
		{
			Scene scene;
			build(scene, sun);
			SceneRenderer renderer;
			renderer.SetViewportSize(c_Size, c_Size);
			CHECK(RenderUntilPreExposureSettled(renderer, scene, camera, 8) <= 4);
			Metered result;
			result.Image = RenderToImage(renderer, scene, camera);
			result.Exposure = ReadExposure(gpu, renderer);
			// The pre-exposure follows the metering: within a stop of a quarter of its multiplier.
			const float stops = std::log2(renderer.GetStats().PreExposure);
			CHECK(std::abs(stops - (std::log2(result.Exposure.y) - SceneRenderer::c_PreExposureHeadroomStops)) <= 1.0f);
			return result;
		};
		const Metered dim = meter(1.0f);
		const Metered bright = meter(65536.0f);
		// Half a histogram bin (0.11 EV) at most, and images that agree within the brightness it implies.
		CHECK(std::abs(std::log2(dim.Exposure.y) - std::log2(bright.Exposure.y) - 16.0f) <= 0.055f);
		const auto decode = [](uint8_t value)
		{
			const float encoded = static_cast<float>(value) / 255.0f;
			return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
		};
		uint32_t disagreeing = 0;
		for (size_t index = 0; index < dim.Image.Pixels.size(); index++)
		{
			if (index % 4 == 3)
				continue;
			const float a = decode(dim.Image.Pixels[index]);
			const float b = decode(bright.Image.Pixels[index]);
			disagreeing += std::abs(a - b) > 0.04f * std::max(a, b) + 1.0f / 255.0f ? 1u : 0u;
		}
		CHECK(disagreeing == 0);

		// The adapted value does not depend on the pre-exposure the frame was drawn with.
		Scene scene;
		build(scene, 65536.0f);
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		RenderPreExposed(renderer, scene, camera, 1.0f / 65536.0f);
		const glm::vec2 low = ReadExposure(gpu, renderer);
		renderer.ResetExposureAdaptation();
		RenderPreExposed(renderer, scene, camera, 1.0f / 1024.0f);
		CHECK(ReadExposure(gpu, renderer) == low);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A fresh renderer settles its pre-exposure within a few frames, and starts again from 1 after a reset")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		PostProcessComponent& postProcess = AddNeutralPostProcess(scene);
		postProcess.AutoExposure = true;
		postProcess.AutoExposureMaxEV = 16.0f;
		AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(20000.0f)), glm::vec3(0.0f), "Wall").GetComponent<TransformComponent>().Scale = glm::vec3(20.0f);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f));

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const int first = Red(RenderToImage(renderer, scene, camera), c_Size / 2, c_Size / 2);
		CHECK(renderer.GetStats().PreExposure == 1.0f);
		CHECK_FALSE(renderer.GetStats().PreExposureSettled);
		CHECK(RenderUntilPreExposureSettled(renderer, scene, camera, 8) <= 3);
		// 20,000 nits metered to middle gray: a multiplier of 2^-16.8, pre-exposed two stops below it.
		CHECK(renderer.GetStats().PreExposure == std::ldexp(1.0f, -19));
		CHECK(std::abs(Red(RenderToImage(renderer, scene, camera), c_Size / 2, c_Size / 2) - first) <= 1);

		// Manual exposure settles at once, at the power of two nearest its multiplier.
		postProcess.AutoExposure = false;
		postProcess.Exposure = -14.4f;
		RenderToImage(renderer, scene, camera);
		CHECK(renderer.GetStats().PreExposure == std::ldexp(1.0f, -14));
		CHECK(renderer.GetStats().PreExposureSettled);

		postProcess.AutoExposure = true;
		postProcess.Exposure = 0.0f;
		RenderToImage(renderer, scene, camera);
		CHECK_FALSE(renderer.GetStats().PreExposureSettled);
		renderer.ResetExposureAdaptation();
		RenderToImage(renderer, scene, camera);
		CHECK(renderer.GetStats().PreExposure == 1.0f);
		CHECK_FALSE(renderer.GetStats().PreExposureSettled);
		CHECK(RenderUntilPreExposureSettled(renderer, scene, camera, 8) <= 3);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The temporal history follows a pre-exposure step without a flash")
	{
		// The history is brought to the frame's pre-exposure (exactly: a power of two), so a renderer whose pre-exposure steps
		// shows what one whose pre-exposure stays shows, frame by frame.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		MaterialProperties light;
		light.BaseColor = glm::vec4(0.9f, 0.8f, 0.7f, 1.0f);
		MaterialProperties dark;
		dark.BaseColor = glm::vec4(0.05f, 0.1f, 0.2f, 1.0f);
		Scene scene;
		PostProcessComponent& postProcess = AddNeutralPostProcess(scene);
		postProcess.AntiAliasing = true;
		postProcess.AntiAliasingMethod = AntiAliasingMode::TAA;
		AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(dark), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(10.0f);
		const AssetHandle lightMaterial = assets.AddMaterial(light);
		for (int index = 0; index < 5; index++)
			AddMesh(scene, BuiltinAssets::CubeMesh, lightMaterial, glm::vec3(static_cast<float>(index) - 2.0f, 0.5f, static_cast<float>(index % 2)), "Box");
		AddSun(scene, glm::vec3(0.5f, -1.0f, -0.4f), 3.0f);
		scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().AmbientColor = glm::vec3(0.3f);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 3.0f, 5.0f), glm::vec3(0.0f, 0.3f, 0.0f));

		SceneRenderer steady;
		SceneRenderer stepping;
		steady.SetViewportSize(c_Size, c_Size);
		stepping.SetViewportSize(c_Size, c_Size);
		for (uint32_t frame = 0; frame < 12; frame++)
		{
			CAPTURE(frame);
			const ReadbackImage expected = RenderPreExposed(steady, scene, camera, 1.0f);
			const ReadbackImage actual = RenderPreExposed(stepping, scene, camera, frame < 6 ? 1.0f : 1.0f / 16.0f);
			REQUIRE(stepping.GetStats().TemporalFrames == frame + 1);
			CHECK(MaxDifference(expected, actual) == 0);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
