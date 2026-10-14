#include <doctest/doctest.h>

#include "Editor/CanvasEdit.h"
#include "Renderer/FontTestUtils.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Renderer/TextureStreamingTestUtils.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/GameUI/GameUIComponents.h"
#include "Strata/GameUI/GameUIScene.h"
#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/TextureStreamer.h"
#include "Strata/Runtime/GameRenderer.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneSerializer.h"
#include "TestHelpers.h"

#include <limits>
#include <string>
#include <utility>
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

	// The smallest box holding every pixel whose color passes `select` (min inclusive, max exclusive); empty: min > max.
	template<typename Select>
	std::pair<glm::ivec2, glm::ivec2> FindPixels(const ReadbackImage& image, Select select)
	{
		glm::ivec2 min(std::numeric_limits<int>::max());
		glm::ivec2 max(std::numeric_limits<int>::min());
		for (uint32_t y = 0; y < image.Height; y++)
		{
			for (uint32_t x = 0; x < image.Width; x++)
			{
				if (!select(GetPixelRGBA8(image, x, y)))
					continue;
				min = glm::min(min, glm::ivec2(static_cast<int>(x), static_cast<int>(y)));
				max = glm::max(max, glm::ivec2(static_cast<int>(x) + 1, static_cast<int>(y) + 1));
			}
		}
		return { min, max };
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

	TEST_CASE("A screen view zooms and pans the canvases and the HUD text without laying them out again")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		UIScene ui;
		ui.AddImage(ui.Canvas, "Panel", { 100.0f, 50.0f }, { 200.0f, 100.0f }, { 1.0f, 0.0f, 0.0f, 1.0f });
		TextComponent& text = ui.Target.CreateEntity("HUD").AddComponent<TextComponent>();
		text.Text = "HUD";
		text.ScreenSpace = true;
		text.ScreenAnchor = { 0.0f, 0.0f };
		text.ScreenOffset = { 300.0f, 200.0f };
		text.FontSize = 40.0f;
		text.Color = { 0.0f, 1.0f, 0.0f, 1.0f };
		auto green = [](const glm::u8vec4& pixel) { return pixel.g > 128 && pixel.r < 64; };

		// The game's screen as an image of its own.
		SceneRenderer renderer(ExactSpecification());
		const ReadbackImage plain = RenderUI(renderer, ui.Target, { 640, 360 });
		const auto [plainMin, plainMax] = FindPixels(plain, green);
		REQUIRE(plainMin.x < plainMax.x);
		GameUIScene::Get(ui.Target).TakeStats();

		// The same screen twice as large, its top-left corner at (-40, 20) of a 1280x720 image.
		SceneRenderOptions options;
		options.ScreenView.ScreenSize = { 640.0f, 360.0f };
		options.ScreenView.Zoom = 2.0f;
		options.ScreenView.Offset = { -40.0f, 20.0f };
		const ReadbackImage zoomed = RenderUI(renderer, ui.Target, { 1280, 720 }, nullptr, options);
		CHECK(GameUIScene::Get(ui.Target).TakeStats().ElementsLaidOut == 0); // The screen's layout, magnified
		// The panel's (100, 50)-(300, 150) on the screen.
		CHECK(Pixel(zoomed, 160.0f, 120.0f) == c_Red);
		CHECK(Pixel(zoomed, 159.0f, 120.0f) == c_Black);
		CHECK(Pixel(zoomed, 160.0f, 119.0f) == c_Black);
		CHECK(Pixel(zoomed, 559.0f, 319.0f) == c_Red);
		CHECK(Pixel(zoomed, 560.0f, 319.0f) == c_Black);
		CHECK(Pixel(zoomed, 559.0f, 320.0f) == c_Black);
		// The HUD text's glyphs, twice as large from the same place (their distance fields' edges may round differently).
		const auto [zoomedMin, zoomedMax] = FindPixels(zoomed, green);
		const glm::ivec2 offset(-40, 20);
		CHECK(std::abs(zoomedMin.x - (offset.x + 2 * plainMin.x)) <= 3);
		CHECK(std::abs(zoomedMin.y - (offset.y + 2 * plainMin.y)) <= 3);
		CHECK(std::abs(zoomedMax.x - (offset.x + 2 * plainMax.x)) <= 3);
		CHECK(std::abs(zoomedMax.y - (offset.y + 2 * plainMax.y)) <= 3);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The canvas mode's cropped camera renders the visible part of the whole view, pixel for pixel")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene("Crop");
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(UnlitColor({ 1.0f, 0.0f, 0.0f, 1.0f })), { -1.5f, 0.5f, 0.0f }, "Red");
		AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(UnlitColor({ 0.0f, 1.0f, 0.0f, 1.0f })), { 1.0f, -0.5f, -1.0f }, "Green");
		AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(UnlitColor({ 0.0f, 0.0f, 1.0f, 1.0f })), { 0.3f, 1.2f, 1.0f }, "Blue");
		SceneRenderer renderer(ExactSpecification());
		for (const bool orthographic : { false, true })
		{
			CAPTURE(orthographic);
			SceneCamera camera = orthographic ? OrthographicLookAt({ 0.0f, 1.0f, 6.0f }, { 0.0f, 0.0f, 0.0f }, 5.0f) : LookAt({ 0.0f, 1.0f, 6.0f }, { 0.0f, 0.0f, 0.0f }, 2.0f);
			if (orthographic)
				camera.Projection = Math::OrthographicReverseZ(-5.0f, 5.0f, -2.5f, 2.5f, 0.1f, 100.0f);
			camera.ClearColor = { 0.0f, 0.0f, 0.0f, 1.0f };
			renderer.SetViewportSize(800, 400);
			const ReadbackImage whole = RenderToImage(renderer, scene, camera);

			const UIBox display { { 0.0f, 0.0f }, { 800.0f, 400.0f } };
			const UIBox visible { { 200.0f, 100.0f }, { 400.0f, 200.0f } };
			renderer.SetViewportSize(400, 200);
			const ReadbackImage part = RenderToImage(renderer, scene, CanvasEdit::CropCamera(camera, display, visible));
			uint32_t mismatches = 0;
			uint32_t colored = 0;
			for (uint32_t y = 0; y < part.Height; y++)
			{
				for (uint32_t x = 0; x < part.Width; x++)
				{
					const glm::ivec4 a(GetPixelRGBA8(part, x, y));
					const glm::ivec4 b(GetPixelRGBA8(whole, x + 200, y + 100));
					const glm::ivec4 difference = glm::abs(a - b);
					if (std::max({ difference.r, difference.g, difference.b }) > 8)
						mismatches++;
					if (a.r + a.g + a.b > 0)
						colored++;
				}
			}
			CHECK(colored > part.Width * part.Height / 10); // The cubes are in view
			CHECK(mismatches <= part.Width * part.Height / 200); // Only edges may round differently
		}
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

	TEST_CASE("The Tetris HUD covers the same fraction of the screen at 1280x720 and 2560x1440")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		std::string error;
		const std::filesystem::path project = CopySampleProject("Tetris", &error);
		REQUIRE_MESSAGE(!project.empty(), error);
		const Ref<Scene> scene = SceneSerializer::LoadFromFile(project / "Assets" / "Scenes" / "Main.stscene", &error);
		REQUIRE_MESSAGE(scene, error);
		// The HUD alone, over black: every lit pixel is the HUD's.
		const Entity hud = scene->FindEntityByName("HUD");
		REQUIRE(hud.IsValid());
		std::vector<Entity> others;
		for (const UUID& root : scene->GetRootEntities())
		{
			if (root != hud.GetUUID())
				others.push_back(scene->GetEntityByUUID(root));
		}
		scene->DestroyEntities(others);
		AddNeutralPostProcess(*scene);

		SceneRenderer renderer(ExactSpecification());
		glm::vec4 fractions[2];
		int index = 0;
		for (const glm::uvec2 size : { glm::uvec2(1280, 720), glm::uvec2(2560, 1440) })
		{
			CAPTURE(size);
			const ReadbackImage image = RenderUI(renderer, *scene, size);
			glm::uvec2 min(size);
			glm::uvec2 max(0);
			for (uint32_t y = 0; y < image.Height; y++)
			{
				for (uint32_t x = 0; x < image.Width; x++)
				{
					const glm::u8vec4 pixel = GetPixelRGBA8(image, x, y);
					if (pixel.r + pixel.g + pixel.b < 24)
						continue;
					min = glm::min(min, glm::uvec2(x, y));
					max = glm::max(max, glm::uvec2(x + 1, y + 1));
				}
			}
			REQUIRE(max.x > min.x);
			fractions[index++] = glm::vec4(glm::vec2(min) / glm::vec2(size), glm::vec2(max) / glm::vec2(size));
		}
		// On a canvas the HUD scales with the screen: the same box within 1% of the screen (a HUD of fixed pixel sizes covers
		// half the fraction at the larger size).
		for (int component = 0; component < 4; component++)
		{
			CAPTURE(component);
			CHECK(std::abs(fractions[0][component] - fractions[1][component]) <= 0.01f);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
