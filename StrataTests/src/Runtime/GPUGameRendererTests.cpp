#include "Renderer/SceneRendererTestUtils.h"

#include "Strata/Runtime/GameRenderer.h"

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = c_SceneTestSize;

	// Like a swapchain image: BGRA8 UNORM, read back as RGBA.
	struct WindowTarget
	{
		nvrhi::TextureHandle Texture;
		nvrhi::FramebufferHandle Framebuffer;

		explicit WindowTarget(nvrhi::IDevice* device)
		{
			nvrhi::TextureDesc desc;
			desc.width = c_Size;
			desc.height = c_Size;
			desc.format = nvrhi::Format::BGRA8_UNORM;
			desc.isRenderTarget = true;
			desc.debugName = "WindowTarget";
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			Texture = device->createTexture(desc);
			REQUIRE(Texture);
			Framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(Texture));
			REQUIRE(Framebuffer);
		}

		ReadbackImage Read() const
		{
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(Texture, image));
			for (size_t index = 0; index + 3 < image.Pixels.size(); index += 4)
				std::swap(image.Pixels[index], image.Pixels[index + 2]);
			return image;
		}
	};

	int CountPixels(const ReadbackImage& image, const std::function<bool(const glm::u8vec4&)>& predicate)
	{
		int count = 0;
		for (uint32_t y = 0; y < image.Height; y++)
		{
			for (uint32_t x = 0; x < image.Width; x++)
				count += predicate(GetPixelRGBA8(image, x, y)) ? 1 : 0;
		}
		return count;
	}

}

TEST_SUITE("GPU.Runtime.GameRenderer")
{
	TEST_CASE("Games render from their primary camera with their HUD, or show a message without one")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		WindowTarget target(gpu.GetNvrhiDevice());

		Ref<Scene> scene = CreateRef<Scene>("Level");
		AddNeutralPostProcess(*scene);
		AddMesh(*scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f), "Cube");
		scene->CreateEntity("Sun").AddComponent<DirectionalLightComponent>().Intensity = 2.0f;
		TextComponent& hud = scene->CreateEntity("Hud").AddComponent<TextComponent>();
		hud.Text = "HUD";
		hud.FontSize = 12.0f;
		hud.ScreenAnchor = glm::vec2(0.5f, 0.1f);

		GameRenderer renderer;
		const glm::uvec2 size(c_Size, c_Size);
		CHECK_FALSE(renderer.Render(scene, target.Framebuffer, glm::uvec2(0, 0))); // Minimized
		CHECK_FALSE(renderer.Render(nullptr, target.Framebuffer, size));
		CHECK_FALSE(renderer.Render(scene, nullptr, size));

		// Without a camera: the message frame, a dark red background with light text.
		REQUIRE(renderer.Render(scene, target.Framebuffer, size));
		CHECK(renderer.IsShowingMessage());
		ReadbackImage image = target.Read();
		const glm::u8vec4 corner = GetPixelRGBA8(image, 0, 0);
		CHECK(std::abs(corner.r - 41) <= 2);
		CHECK(std::abs(corner.g - 13) <= 2);
		CHECK(std::abs(corner.b - 13) <= 2);
		CHECK(CountPixels(image, [](const glm::u8vec4& pixel) { return pixel.r > 150 && pixel.g > 120; }) > 10);
		CHECK(renderer.GetStats().Texts == 1);

		// With a primary camera: the scene through it, with its screen-space text.
		Entity camera = scene->CreateEntity("Camera");
		camera.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 3.0f);
		camera.AddComponent<CameraComponent>().ClearColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		REQUIRE(renderer.Render(scene, target.Framebuffer, size));
		CHECK_FALSE(renderer.IsShowingMessage());
		image = target.Read();
		CHECK(GetPixelRGBA8(image, c_Size / 2, c_Size / 2).r > 150); // The lit cube
		CHECK(GetPixelRGBA8(image, 1, c_Size - 2) == glm::u8vec4(0, 0, 0, 255));
		CHECK(renderer.GetStats().Texts == 1);
		CHECK(renderer.GetStats().Instances == 1);

		// A deactivated camera no longer counts.
		camera.SetActive(false);
		REQUIRE(renderer.Render(scene, target.Framebuffer, size));
		CHECK(renderer.IsShowingMessage());
		camera.SetActive(true);
		REQUIRE(renderer.Render(scene, target.Framebuffer, size));
		CHECK_FALSE(renderer.IsShowingMessage());
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
