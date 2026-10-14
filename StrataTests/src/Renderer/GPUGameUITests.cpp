#include <doctest/doctest.h>

#include "Renderer/FontTestUtils.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Renderer/TextureStreamingTestUtils.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/GameUI/GameUIComponents.h"
#include "Strata/GameUI/GameUIScene.h"
#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/TextureStreamer.h"
#include "Strata/Runtime/GameRenderer.h"
#include "Strata/Scene/Entity.h"

#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A scene with neutral post-processing over black, and a canvas whose unit is one pixel (unless a test changes it).
	struct UIScene
	{
		Scene Target { "UI" };
		Entity Canvas;

		UIScene()
		{
			AddNeutralPostProcess(Target);
			Canvas = Target.CreateEntity("Canvas");
			Canvas.AddComponent<UIRectComponent>();
			Canvas.AddComponent<UICanvasComponent>().ScaleMode = UICanvasScaleMode::ConstantPixelSize;
		}

		Entity AddImage(Entity parent, const std::string& name, const glm::vec2& position, const glm::vec2& size, const glm::vec4& color)
		{
			Entity entity = Target.CreateChildEntity(parent, name);
			UIRectComponent& rect = entity.AddComponent<UIRectComponent>();
			rect.AnchorMin = { 0.0f, 0.0f };
			rect.AnchorMax = { 0.0f, 0.0f };
			rect.Pivot = { 0.0f, 0.0f };
			rect.Position = position;
			rect.Size = size;
			entity.AddComponent<UIImageComponent>().Color = color;
			return entity;
		}
	};

	SceneCamera BlackCamera()
	{
		SceneCamera camera = LookAt({ 0.0f, 0.0f, 5.0f }, { 0.0f, 0.0f, 0.0f });
		camera.ClearColor = { 0.0f, 0.0f, 0.0f, 1.0f };
		return camera;
	}

	// Renders until the UI waits for no glyph or asset (glyphs are rasterized within a budget per frame).
	ReadbackImage RenderUI(SceneRenderer& renderer, Scene& scene, const glm::uvec2& size, AssetManagerBase* assets = nullptr, const SceneRenderOptions& options = {})
	{
		renderer.SetViewportSize(size.x, size.y);
		SceneCamera camera = BlackCamera();
		camera.Projection = Math::PerspectiveReverseZ(camera.VerticalFOV, static_cast<float>(size.x) / static_cast<float>(size.y), camera.Near, camera.Far);
		for (int frame = 0; frame < 200; frame++)
		{
			if (assets)
				assets->Update();
			REQUIRE(renderer.Render(scene, camera, nullptr, options));
			if (renderer.GetStats().Overlays.PendingGlyphs == 0 && renderer.GetStats().PendingAssets == 0)
				break;
		}
		CHECK(renderer.GetStats().Overlays.PendingGlyphs == 0);
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
		return image;
	}

	glm::u8vec4 Pixel(const ReadbackImage& image, float x, float y)
	{
		return GetPixelRGBA8(image, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
	}

	SceneRendererSpecification ExactSpecification()
	{
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		return specification;
	}

	const glm::u8vec4 c_Black(0, 0, 0, 255);
	const glm::u8vec4 c_Red(255, 0, 0, 255);
	const glm::u8vec4 c_Green(0, 255, 0, 255);
	const glm::u8vec4 c_Blue(0, 0, 255, 255);
	const glm::u8vec4 c_White(255, 255, 255, 255);

}

TEST_SUITE("GPU.GameUI")
{
	TEST_CASE("A canvas places its rects at the same fraction of every screen, edges on whole pixels")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		UIScene ui;
		UICanvasComponent& canvas = ui.Canvas.GetComponent<UICanvasComponent>();
		canvas.ScaleMode = UICanvasScaleMode::ScaleWithScreen;
		canvas.ReferenceResolution = { 1280.0f, 720.0f };
		Entity panel = ui.AddImage(ui.Canvas, "Panel", { 0.0f, 0.0f }, { 320.0f, 180.0f }, { 1.0f, 0.0f, 0.0f, 1.0f });
		UIRectComponent& rect = panel.GetComponent<UIRectComponent>();
		rect.AnchorMin = rect.AnchorMax = { 0.25f, 0.5f };

		SceneRenderer renderer(ExactSpecification());
		for (const glm::uvec2 size : { glm::uvec2(1280, 720), glm::uvec2(2560, 1440) })
		{
			CAPTURE(size);
			const ReadbackImage image = RenderUI(renderer, ui.Target, size);
			const float x0 = static_cast<float>(size.x) * 0.25f;
			const float y0 = static_cast<float>(size.y) * 0.5f;
			const float x1 = x0 + static_cast<float>(size.x) * 0.25f;
			const float y1 = y0 + static_cast<float>(size.y) * 0.25f;
			CHECK(Pixel(image, x0, y0) == c_Red);
			CHECK(Pixel(image, x0 - 1.0f, y0) == c_Black);
			CHECK(Pixel(image, x0, y0 - 1.0f) == c_Black);
			CHECK(Pixel(image, x1 - 1.0f, y1 - 1.0f) == c_Red);
			CHECK(Pixel(image, x1, y1 - 1.0f) == c_Black);
			CHECK(Pixel(image, x1 - 1.0f, y1) == c_Black);
			const SceneOverlayStats& stats = renderer.GetStats().Overlays;
			CHECK(stats.Canvases == 1);
			CHECK(stats.DrawCalls == 1);
			CHECK(stats.Elements == 2);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Images draw solid, filled, rounded with borders and nine-sliced, in exact display colors")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		UIScene ui;
		// Rounded corners and a border: the corner is cut, the band inside the edge is the border's color.
		Entity rounded = ui.AddImage(ui.Canvas, "Rounded", { 16.0f, 16.0f }, { 64.0f, 64.0f }, { 0.0f, 1.0f, 0.0f, 1.0f });
		UIImageComponent& roundedImage = rounded.GetComponent<UIImageComponent>();
		roundedImage.CornerRadius = 16.0f;
		roundedImage.BorderWidth = 4.0f;
		roundedImage.BorderColor = { 0.0f, 0.0f, 1.0f, 1.0f };
		// A quarter filled from the left.
		Entity filled = ui.AddImage(ui.Canvas, "Filled", { 100.0f, 16.0f }, { 80.0f, 20.0f }, { 1.0f, 0.0f, 0.0f, 1.0f });
		filled.GetComponent<UIImageComponent>().Type = UIImageType::Filled;
		filled.GetComponent<UIImageComponent>().FillAmount = 0.25f;

		// A 16x16 texture with a red 4-texel border around green, sliced at its border at 2 canvas units per texel.
		std::vector<uint8_t> pixels;
		for (uint32_t y = 0; y < 16; y++)
		{
			for (uint32_t x = 0; x < 16; x++)
			{
				const bool border = x < 4 || y < 4 || x >= 12 || y >= 12;
				AppendPixel(pixels, border ? 255 : 0, border ? 0 : 255, 0, 255);
			}
		}
		const AssetHandle frame = assets.AddTexture(16, 16, pixels, TextureFormat::RGBA8SRGB);
		auto addSliced = [&](const glm::vec2& position, const glm::vec2& size)
		{
			Entity sliced = ui.AddImage(ui.Canvas, "Sliced", position, size, { 1.0f, 1.0f, 1.0f, 1.0f });
			UIImageComponent& image = sliced.GetComponent<UIImageComponent>();
			image.Texture = frame;
			image.Type = UIImageType::Sliced;
			image.SliceBorders = glm::vec4(4.0f);
			image.SliceScale = 2.0f;
			return sliced;
		};
		addSliced({ 16.0f, 100.0f }, { 100.0f, 60.0f });
		addSliced({ 16.0f, 180.0f }, { 220.0f, 60.0f });

		SceneRenderer renderer(ExactSpecification());
		const ReadbackImage image = RenderUI(renderer, ui.Target, { 256, 256 }, assets.Manager.get());
		CHECK(Pixel(image, 48.0f, 48.0f) == c_Green);
		CHECK(Pixel(image, 17.0f, 48.0f) == c_Blue);
		CHECK(Pixel(image, 48.0f, 78.0f) == c_Blue);
		CHECK(Pixel(image, 17.0f, 17.0f) == c_Black); // Cut by the rounded corner
		CHECK(Pixel(image, 101.0f, 26.0f) == c_Red);
		CHECK(Pixel(image, 119.0f, 26.0f) == c_Red);
		CHECK(Pixel(image, 120.0f, 26.0f) == c_Black);
		// The borders keep their 8 units however wide the rect is; the center stretches.
		for (const float top : { 100.0f, 180.0f })
		{
			const float right = top == 100.0f ? 116.0f : 236.0f;
			CAPTURE(top);
			CHECK(Pixel(image, 19.0f, top + 3.0f) == c_Red);  // Corner
			CHECK(Pixel(image, 19.0f, top + 30.0f) == c_Red); // Left edge
			CHECK(Pixel(image, 40.0f, top + 30.0f) == c_Green); // Center (away from the texels it blends with at its edge)
			CHECK(Pixel(image, right - 4.0f, top + 30.0f) == c_Red); // Right edge
			CHECK(Pixel(image, right - 24.0f, top + 30.0f) == c_Green);
			CHECK(Pixel(image, 60.0f, top + 3.0f) == c_Red); // Top edge
			CHECK(Pixel(image, 60.0f, top + 12.0f) == c_Green);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Text draws its glyphs from distance fields with an outline, in exact display colors")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		UIScene ui;
		Entity label = ui.Target.CreateChildEntity(ui.Canvas, "Label");
		UIRectComponent& rect = label.AddComponent<UIRectComponent>();
		rect.AnchorMin = { 0.0f, 0.0f };
		rect.AnchorMax = { 1.0f, 1.0f };
		rect.Size = { 0.0f, 0.0f };
		UITextComponent& text = label.AddComponent<UITextComponent>();
		text.Text = "I";
		text.FontSize = 160.0f;
		text.Color = { 1.0f, 1.0f, 1.0f, 1.0f };
		text.OutlineWidth = 0.08f;
		text.OutlineColor = { 1.0f, 0.0f, 0.0f, 1.0f };

		SceneRenderer renderer(ExactSpecification());
		const ReadbackImage image = RenderUI(renderer, ui.Target, { 256, 256 });
		// Across the stem in the middle row: black, the outline's red, the glyph's white, red again, black.
		CHECK(Pixel(image, 128.0f, 128.0f) == c_White);
		CHECK(Pixel(image, 4.0f, 128.0f) == c_Black);
		int red = 0;
		int white = 0;
		for (uint32_t x = 0; x < image.Width; x++)
		{
			const glm::u8vec4 pixel = GetPixelRGBA8(image, x, 128);
			red += pixel == c_Red ? 1 : 0;
			white += pixel == c_White ? 1 : 0;
		}
		CHECK(red >= 16);  // Two bands of the outline (12.8 pixels each)
		CHECK(white >= 10);
		const SceneOverlayStats& stats = renderer.GetStats().Overlays;
		CHECK(stats.DrawCalls == 1);
		CHECK(stats.PendingGlyphs == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("An idle frame uploads nothing, and a canvas with up to four fonts is one draw call")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		UIScene ui;
		const char* const fontFiles[] = { "Strata/vendor/tracy/profiler/src/font/FiraCode-Retina.ttf", "Strata/vendor/tracy/profiler/src/font/NotoEmoji-Regular.ttf",
			"StrataTests/FeatureTest/Assets/Fonts/FeatureBlocks.ttf", "Strata/vendor/imgui/misc/fonts/Cousine-Regular.ttf" };
		std::vector<AssetHandle> fonts = { UUID::Null() }; // The default font
		for (const char* file : fontFiles)
		{
			std::string error;
			Ref<Font> font = Font::Create(ReadSourceFile(file), &error);
			REQUIRE_MESSAGE(font, error);
			fonts.push_back(assets.Add(font, file));
		}
		std::vector<Entity> labels;
		for (size_t index = 0; index < 4; index++)
		{
			Entity label = ui.AddImage(ui.Canvas, "Label", { 10.0f, 10.0f + 30.0f * static_cast<float>(index) }, { 200.0f, 30.0f }, { 0.1f, 0.1f, 0.1f, 1.0f });
			UITextComponent& text = label.AddComponent<UITextComponent>();
			text.Text = "AB12";
			text.Font = fonts[index];
			text.FontSize = 20.0f;
			labels.push_back(label);
		}

		SceneRenderer renderer(ExactSpecification());
		RenderUI(renderer, ui.Target, { 256, 256 }, assets.Manager.get());
		CHECK(renderer.GetStats().Overlays.DrawCalls == 1);
		CHECK(renderer.GetStats().Overlays.Quads >= 4 * 4 + 4);

		// Nothing changed: nothing is uploaded.
		RenderUI(renderer, ui.Target, { 256, 256 }, assets.Manager.get());
		CHECK(renderer.GetStats().Overlays.UploadedBytes == 0);

		// A label's color changes: its quads alone (a slot of a few quads).
		labels[2].GetComponent<UITextComponent>().Color = { 1.0f, 1.0f, 0.0f, 1.0f };
		labels[2].MarkModified<UITextComponent>();
		RenderUI(renderer, ui.Target, { 256, 256 }, assets.Manager.get());
		CHECK(renderer.GetStats().Overlays.UploadedBytes > 0);
		CHECK(renderer.GetStats().Overlays.UploadedBytes <= 16 * sizeof(RenderData::UIQuad));

		// A fifth font: two draws.
		Entity fifth = ui.AddImage(ui.Canvas, "Fifth", { 10.0f, 200.0f }, { 200.0f, 30.0f }, { 0.1f, 0.1f, 0.1f, 1.0f });
		UITextComponent& fifthText = fifth.AddComponent<UITextComponent>();
		fifthText.Text = "C";
		fifthText.Font = fonts[4];
		RenderUI(renderer, ui.Target, { 256, 256 }, assets.Manager.get());
		CHECK(renderer.GetStats().Overlays.DrawCalls == 2);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Screen-space canvases hide with the HUD, and a scene without a camera shows its UI instead of a message")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Ref<Scene> scene = CreateRef<Scene>("Menu");
		Entity canvas = scene->CreateEntity("Canvas");
		canvas.AddComponent<UIRectComponent>();
		canvas.AddComponent<UICanvasComponent>().ScaleMode = UICanvasScaleMode::ConstantPixelSize;
		Entity panel = scene->CreateChildEntity(canvas, "Panel");
		UIRectComponent& rect = panel.AddComponent<UIRectComponent>();
		rect.AnchorMin = { 0.0f, 0.0f };
		rect.AnchorMax = { 1.0f, 1.0f };
		rect.Size = { -20.0f, -20.0f };
		panel.AddComponent<UIImageComponent>().Color = { 0.0f, 0.0f, 1.0f, 1.0f };

		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(64, 64);
		SceneRenderOptions hidden;
		hidden.DrawScreenSpaceText = false;
		REQUIRE(renderer.Render(*scene, BlackCamera(), nullptr, hidden));
		CHECK(renderer.GetStats().Overlays.Canvases == 0);
		CHECK(renderer.GetStats().Overlays.HiddenCanvases == 1);
		CHECK(renderer.GetStats().Overlays.Elements == 0);
		CHECK(GameUIScene::Get(*scene).TakeStats().ElementsLaidOut == 0); // Hidden canvases are not laid out

		// A game whose scene has no camera: its menu over black, no message, no error.
		GameRenderer game;
		nvrhi::TextureDesc desc;
		desc.width = 64;
		desc.height = 64;
		desc.format = nvrhi::Format::BGRA8_UNORM;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		nvrhi::TextureHandle target = Renderer::GetDevice()->createTexture(desc);
		nvrhi::FramebufferHandle framebuffer = Renderer::GetDevice()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));
		REQUIRE(framebuffer);
		REQUIRE(game.Render(scene, framebuffer, { 64, 64 }));
		CHECK_FALSE(game.IsShowingMessage());
		CHECK(game.GetStats().Overlays.Canvases == 1);
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(target, image));
		// BGRA: blue is the first byte.
		const glm::u8vec4 center = GetPixelRGBA8(image, 32, 32);
		CHECK(center == glm::u8vec4(255, 0, 0, 255));
		const glm::u8vec4 corner = GetPixelRGBA8(image, 2, 2);
		CHECK(corner == glm::u8vec4(0, 0, 0, 255));

		// Without the canvas it is the message again.
		scene->DestroyEntity(canvas);
		REQUIRE(game.Render(scene, framebuffer, { 64, 64 }));
		CHECK(game.IsShowingMessage());
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("UI textures stream to the level they are drawn at")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		constexpr AssetHandle c_Texture = AssetHandle(0xC0FFEE);
		const std::filesystem::path pack = WriteStreamingPack({ { c_Texture, AssetType::Texture, "Textures/UI.png", CookSolidTexture(512, { 200, 100, 50, 255 }) } });
		for (const float drawnSize : { 512.0f, 64.0f })
		{
			CAPTURE(drawnSize);
			std::string error;
			Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(pack, &error);
			REQUIRE_MESSAGE(manager, error);
			REQUIRE(TextureStreamer::Find(*manager));
			AssetManager::SetActive(manager);
			UIScene ui;
			Entity image = ui.AddImage(ui.Canvas, "Image", { 0.0f, 0.0f }, glm::vec2(drawnSize), { 1.0f, 1.0f, 1.0f, 1.0f });
			image.GetComponent<UIImageComponent>().Texture = c_Texture;
			SceneRenderer renderer;
			renderer.SetViewportSize(512, 512);
			uint32_t level = ~0u;
			for (int frame = 0; frame < 200; frame++)
			{
				manager->Update();
				REQUIRE(renderer.Render(ui.Target, BlackCamera()));
				const Ref<Texture> texture = manager->PeekLoadedAsset<Texture>(c_Texture);
				const AssetManagerStats stats = manager->GetStats();
				if (texture && frame > 10 && stats.Detail.Reads == 0 && stats.Detail.Changes == 0)
				{
					level = texture->GetResidentLevel();
					break;
				}
			}
			// 512 texels over 512 pixels need level 0; over 64 pixels level 3 (64 texels).
			if (drawnSize == 512.0f)
				CHECK(level == 0);
			else
				CHECK(level == 3);
			AssetManager::SetActive(nullptr);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
