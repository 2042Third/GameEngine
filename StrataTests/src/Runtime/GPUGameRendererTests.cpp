#include "Renderer/SceneRendererTestUtils.h"

#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Runtime/GameRenderer.h"

#include <functional>
#include <sstream>
#include <string>
#include <vector>

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

		explicit WindowTarget(nvrhi::IDevice* device, const glm::uvec2& size = glm::uvec2(c_Size))
		{
			nvrhi::TextureDesc desc;
			desc.width = size.x;
			desc.height = size.y;
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
		CHECK(GetDrawnCounts(renderer.GetSceneRenderer()).Instances == 1);

		// A deactivated camera no longer counts.
		camera.SetActive(false);
		REQUIRE(renderer.Render(scene, target.Framebuffer, size));
		CHECK(renderer.IsShowingMessage());
		camera.SetActive(true);
		REQUIRE(renderer.Render(scene, target.Framebuffer, size));
		CHECK_FALSE(renderer.IsShowingMessage());
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A game's temporal anti-aliasing starts again with each scene it shows and when its camera returns")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		WindowTarget target(gpu.GetNvrhiDevice());
		const auto makeLevel = [](const char* name)
		{
			Ref<Scene> level = CreateRef<Scene>(name);
			PostProcessComponent& postProcess = AddNeutralPostProcess(*level);
			postProcess.AntiAliasing = true;
			postProcess.AntiAliasingMethod = AntiAliasingMode::TAA;
			AddMesh(*level, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f), "Cube");
			Entity camera = level->CreateEntity("Camera");
			camera.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 3.0f);
			camera.AddComponent<CameraComponent>();
			return level;
		};
		const Ref<Scene> first = makeLevel("First");
		const Ref<Scene> second = makeLevel("Second");
		GameRenderer renderer;
		const glm::uvec2 size(c_Size, c_Size);
		const SceneRendererStats& stats = renderer.GetSceneRenderer().GetStats();
		for (int frame = 0; frame < 3; frame++)
			REQUIRE(renderer.Render(first, target.Framebuffer, size));
		CHECK(stats.TemporalFrames == 3);
		CHECK(stats.HistoryResets == 1);
		// A level load.
		REQUIRE(renderer.Render(second, target.Framebuffer, size));
		CHECK(stats.TemporalFrames == 1);
		CHECK(stats.HistoryResets == 2);
		REQUIRE(renderer.Render(second, target.Framebuffer, size));
		CHECK(stats.TemporalFrames == 2);
		// The camera goes (the message shows) and comes back.
		Entity camera = second->FindEntityByName("Camera");
		camera.SetActive(false);
		REQUIRE(renderer.Render(second, target.Framebuffer, size));
		CHECK(renderer.IsShowingMessage());
		camera.SetActive(true);
		REQUIRE(renderer.Render(second, target.Framebuffer, size));
		CHECK(stats.TemporalFrames == 1);
		CHECK(stats.HistoryResets == 3);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The perf log's Render columns hold what the game's frame drew, with the GPU's counts")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		WindowTarget target(gpu.GetNvrhiDevice());
		Ref<Scene> scene = CreateRef<Scene>("Level");
		AddNeutralPostProcess(*scene);
		for (int index = 0; index < 3; index++)
			AddMesh(*scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(static_cast<float>(index) - 1.0f, 0.0f, 0.0f), "Cube");
		// And a thousand out of view behind the camera, so that the first frame's upload shows in megabytes.
		for (int index = 0; index < 1000; index++)
			AddMesh(*scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(static_cast<float>(index % 40), static_cast<float>(index / 40), 40.0f), "Behind");
		Entity camera = scene->CreateEntity("Camera");
		camera.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 6.0f);
		camera.AddComponent<CameraComponent>();
		// A shadowed lamp above the cubes: its six views are drawn in the first frame.
		Entity lamp = AddPointLight(*scene, glm::vec3(0.0f, 2.0f, 1.0f), glm::vec3(1.0f), 10.0f, 5.0f);
		lamp.GetComponent<PointLightComponent>().CastShadows = true;

		const auto writeRow = [](const FramePerfColumnGroup& group)
		{
			std::string line;
			FramePerfRow row(line);
			group.Write(row);
			CHECK(row.GetCount() == group.Names.size());
			return line;
		};
		// Without a renderer (a headless game), or before its first frame: empty fields.
		const FramePerfColumnGroup headless = GameRenderer::MakePerfLogColumns(nullptr);
		CHECK(headless.Names == std::vector<std::string> { "DrawCalls", "Instances", "Triangles", "VisibleProxies", "OccludedProxies", "InstanceUploadMB",
			"LocalShadowLights", "LocalShadowViewsDrawn" });
		CHECK(writeRow(headless) == ",,,,,,,,");
		GameRenderer renderer;
		const FramePerfColumnGroup columns = GameRenderer::MakePerfLogColumns(&renderer);
		CHECK(writeRow(columns) == ",,,,,,,,");

		REQUIRE(renderer.Render(scene, target.Framebuffer, glm::uvec2(c_Size)));
		renderer.GetSceneRenderer().ResolveGPUStats(true);
		const DrawnCounts drawn = GetDrawnCounts(renderer.GetSceneRenderer());
		REQUIRE(drawn.Instances == 3);
		std::vector<double> values;
		std::stringstream fields(writeRow(columns).substr(1));
		for (std::string field; std::getline(fields, field, ',');)
			values.push_back(std::stod(field));
		REQUIRE(values.size() == 8);
		CHECK(values[0] == static_cast<double>(drawn.Draws));
		CHECK(values[1] == 3.0);
		CHECK(values[2] == static_cast<double>(drawn.Triangles));
		CHECK(values[3] == static_cast<double>(drawn.VisibleProxies));
		CHECK(values[4] == 0.0);
		CHECK(values[5] > 0.0); // The first frame uploads the instances
		CHECK(values[6] == 1.0);
		CHECK(values[7] == 6.0);
		// The next frame keeps the lamp's views: nothing reaching them changed.
		REQUIRE(renderer.Render(scene, target.Framebuffer, glm::uvec2(c_Size)));
		std::stringstream next(writeRow(columns).substr(1));
		values.clear();
		for (std::string field; std::getline(next, field, ',');)
			values.push_back(std::stod(field));
		REQUIRE(values.size() == 8);
		CHECK(values[6] == 1.0);
		CHECK(values[7] == 0.0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Games render without an entity-ID buffer, and without an output texture until they show text")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		WindowTarget target(gpu.GetNvrhiDevice());
		Ref<Scene> scene = CreateRef<Scene>("Level");
		AddNeutralPostProcess(*scene);
		AddMesh(*scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f), "Cube");
		scene->CreateEntity("Sun").AddComponent<DirectionalLightComponent>().Intensity = 2.0f;
		Entity camera = scene->CreateEntity("Camera");
		camera.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 3.0f);
		camera.AddComponent<CameraComponent>().ClearColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);

		// The image goes straight into the window: the targets a game does not use are never made.
		GameRenderer renderer;
		const glm::uvec2 size(c_Size, c_Size);
		REQUIRE(renderer.Render(scene, target.Framebuffer, size));
		CHECK(GetPixelRGBA8(target.Read(), c_Size / 2, c_Size / 2).r > 150); // The lit cube
		CHECK(renderer.GetSceneRenderer().GetEntityIDTexture() == nullptr);
		CHECK(renderer.GetSceneRenderer().GetOutputTexture() == nullptr);
		CHECK(renderer.GetSceneRenderer().GetDepthTexture() != nullptr);

		// A HUD is composed in the output texture first.
		TextComponent& hud = scene->CreateEntity("Hud").AddComponent<TextComponent>();
		hud.Text = "HUD";
		hud.FontSize = 12.0f;
		REQUIRE(renderer.Render(scene, target.Framebuffer, size));
		CHECK(renderer.GetStats().Texts == 1);
		CHECK(renderer.GetSceneRenderer().GetOutputTexture() != nullptr);
		CHECK(renderer.GetSceneRenderer().GetEntityIDTexture() == nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Resizing the window right after a frame keeps the frame's render targets until the GPU is done")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		// The message frame draws no mesh, so its frames only clear the prepass targets, which NVRHI's Vulkan backend does
		// not keep alive for the GPU; the renderer recreates its targets at every new window size.
		Ref<Scene> scene = CreateRef<Scene>("Empty");
		const glm::uvec2 sizes[] = { { 64, 48 }, { 80, 64 }, { 48, 72 } };
		std::vector<WindowTarget> targets;
		for (const glm::uvec2& size : sizes)
			targets.emplace_back(gpu.GetNvrhiDevice(), size);

		GameRenderer renderer;
		for (int frame = 0; frame < 18; frame++)
		{
			const size_t index = static_cast<size_t>(frame) % targets.size();
			REQUIRE(renderer.Render(scene, targets[index].Framebuffer, sizes[index]));
			CHECK(renderer.IsShowingMessage());
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
