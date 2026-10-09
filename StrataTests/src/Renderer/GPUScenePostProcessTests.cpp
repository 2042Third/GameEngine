#include "Renderer/SceneRendererTestUtils.h"

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
		const int adapting = renderCenter(false); // Still exposed for the dim wall
		CHECK(adapting > 200);
		CHECK(std::abs(renderCenter(true) - c_MiddleGray) <= 4);

		// Exposure compensation shifts the result by stops.
		postProcess.Exposure = 1.0f;
		CHECK(std::abs(renderCenter(true) - 161) <= 5); // 0.36 encoded as sRGB
		postProcess.Exposure = 0.0f;

		// Scenes darker than the allowed range stay dark.
		wall.GetComponent<MeshRendererComponent>().Material = veryDark;
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
		REQUIRE(occlusion.BytesPerPixel == sizeof(float));
		CHECK(ReadFloatTexel(occlusion, creasePixel) < 0.85f);
		CHECK(ReadFloatTexel(occlusion, openFloor) > 0.95f);
		CHECK(ReadFloatTexel(occlusion, boxTop) > 0.95f);
		CHECK(ReadFloatTexel(occlusion, glm::uvec2(size / 2, 1)) == 1.0f); // Background

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
		CHECK(ReadFloatTexel(occlusion, creasePixel) < 0.85f); // The crease behind the pane is occluded
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
			CHECK(ReadFloatTexel(occlusion, ProjectToPixel(camera, crease.CreasePoint(), size.x, size.y)) < 0.9f);
			CHECK(ReadFloatTexel(occlusion, ProjectToPixel(camera, crease.OpenFloorPoint(), size.x, size.y)) > 0.95f);

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
}
