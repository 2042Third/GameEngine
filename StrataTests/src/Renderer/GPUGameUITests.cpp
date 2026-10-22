#include <doctest/doctest.h>

#include "Renderer/FontTestUtils.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Renderer/TextureStreamingTestUtils.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Core/FrameProfiler.h"
#include "Strata/GameUI/GameUIComponents.h"
#include "Strata/GameUI/GameUIHitTest.h"
#include "Strata/GameUI/GameUIRenderer.h"
#include "Strata/GameUI/GameUIScene.h"
#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/FontAtlas.h"
#include "Strata/Renderer/FontAtlasCache.h"
#include "Strata/Renderer/GPUProfiler.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/SceneOverlay.h"
#include "Strata/Renderer/SceneRenderer.h"
#include "Strata/Renderer/TextureStreamer.h"
#include "Strata/Runtime/GameRenderer.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneSerializer.h"
#include "TestHelpers.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include <optional>
#include <span>
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

	TEST_CASE("The UI pass is timed on the GPU as GPU.UI, in a scene's frames and in a game's scene without a camera")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		// Registered with the module (RegisterGameUIModule), so a perf log has its column from its first row.
		const FrameStageID stage = FrameProfiler::Get().FindStage("GPU.UI");
		REQUIRE(stage != c_InvalidFrameStage);
		CHECK(FrameProfiler::Get().GetStageKind(stage) == FrameStageKind::GPU);
		if (!Renderer::GetGPUProfiler().IsEnabled())
		{
			MESSAGE("The device writes no timestamps: nothing to time");
			CHECK(gpu.GetNewErrorCount() == 0);
			return;
		}
		// The stage's time of a renderer's latest timed frame; -1 when the frame has none.
		const auto stageMs = [stage](const SceneRendererGPUStats& stats)
		{
			for (const GPUStageTime& time : stats.StageMs)
			{
				if (time.Stage == stage)
					return time.Milliseconds;
			}
			return -1.0f;
		};
		// Frames as an application runs them, so the profiler resolves their timestamps.
		const auto timedFrames = [&gpu](const std::function<void()>& render)
		{
			for (int32_t frame = 0; frame < 2; frame++)
			{
				REQUIRE(gpu.GetDevice().BeginFrame());
				Renderer::BeginFrame();
				render();
				gpu.GetDevice().EndFrame();
			}
		};

		// A scene with a camera: its canvas over the image.
		UIScene ui;
		ui.AddImage(ui.Canvas, "Panel", { 8.0f, 8.0f }, { 48.0f, 48.0f }, { 0.0f, 0.0f, 1.0f, 1.0f });
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(64, 64);
		timedFrames([&]() { REQUIRE(renderer.Render(ui.Target, BlackCamera())); });
		CHECK(renderer.GetStats().Overlays.DrawCalls == 1);
		renderer.ResolveGPUStats(true);
		REQUIRE(renderer.GetGPUStats().HasTimes);
		CHECK(stageMs(renderer.GetGPUStats()) >= 0.0f);
		CHECK(stageMs(renderer.GetGPUStats()) <= renderer.GetGPUStats().BusyMs);

		// A game whose scene has no camera draws its menu through the same pass.
		Ref<Scene> menu = CreateRef<Scene>("Menu");
		Entity canvas = menu->CreateEntity("Canvas");
		canvas.AddComponent<UIRectComponent>();
		canvas.AddComponent<UICanvasComponent>().ScaleMode = UICanvasScaleMode::ConstantPixelSize;
		Entity panel = menu->CreateChildEntity(canvas, "Panel");
		panel.AddComponent<UIRectComponent>().Size = { 32.0f, 32.0f };
		panel.AddComponent<UIImageComponent>().Color = { 0.0f, 1.0f, 0.0f, 1.0f };
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
		timedFrames([&]() { REQUIRE(game.Render(menu, framebuffer, { 64, 64 })); });
		CHECK_FALSE(game.IsShowingMessage());
		SceneRenderer& gameRenderer = game.GetSceneRenderer();
		gameRenderer.ResolveGPUStats(true);
		REQUIRE(gameRenderer.GetGPUStats().HasTimes);
		CHECK(stageMs(gameRenderer.GetGPUStats()) >= 0.0f);
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

		// A canvas placed in the scene is no screen content: without a camera nothing shows it, so the message does.
		canvas.GetComponent<UICanvasComponent>().RenderMode = UICanvasRenderMode::WorldSpace;
		canvas.MarkModified<UICanvasComponent>();
		REQUIRE(game.Render(scene, framebuffer, { 64, 64 }));
		CHECK(game.IsShowingMessage());

		// Without the canvas it is the message again.
		scene->DestroyEntity(canvas);
		REQUIRE(game.Render(scene, framebuffer, { 64, 64 }));
		CHECK(game.IsShowingMessage());
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Masks clip their children's pixels, with soft edges, in one draw that an idle frame does not upload again")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		UIScene ui;
		// A red panel masking a green child that spills out of it on every side.
		Entity panel = ui.AddImage(ui.Canvas, "Panel", { 100.0f, 50.0f }, { 200.0f, 100.0f }, { 1.0f, 0.0f, 0.0f, 1.0f });
		panel.AddComponent<UIMaskComponent>().Padding = { 10.0f, 0.0f, 0.0f, 0.0f };
		ui.AddImage(panel, "Spill", { -50.0f, -50.0f }, { 300.0f, 200.0f }, { 0.0f, 1.0f, 0.0f, 1.0f });
		// A soft mask beside it: its child fades over 20 pixels inside the edges.
		Entity soft = ui.AddImage(ui.Canvas, "Soft", { 400.0f, 50.0f }, { 100.0f, 100.0f }, { 0.0f, 0.0f, 0.0f, 0.0f });
		soft.AddComponent<UIMaskComponent>().Softness = 20.0f;
		ui.AddImage(soft, "Faded", { -10.0f, -10.0f }, { 120.0f, 120.0f }, { 0.0f, 0.0f, 1.0f, 1.0f });

		SceneRenderer renderer(ExactSpecification());
		const ReadbackImage image = RenderUI(renderer, ui.Target, { 640, 360 });
		// The child shows only inside (110, 50)-(300, 150); the panel itself is not clipped by its own mask.
		CHECK(Pixel(image, 110.0f, 50.0f) == c_Green);
		CHECK(Pixel(image, 299.0f, 149.0f) == c_Green);
		CHECK(Pixel(image, 105.0f, 100.0f) == c_Red);
		CHECK(Pixel(image, 300.0f, 100.0f) == c_Black);
		CHECK(Pixel(image, 200.0f, 150.0f) == c_Black);
		CHECK(Pixel(image, 200.0f, 49.0f) == c_Black);
		// The soft edge: transparent outside, half at 10 pixels in, whole from 20 in.
		CHECK(Pixel(image, 399.0f, 100.0f) == c_Black);
		const glm::u8vec4 edge = Pixel(image, 400.0f, 100.0f);
		const glm::u8vec4 half = Pixel(image, 410.0f, 100.0f);
		CHECK(edge.b < 16);
		CHECK(std::abs(static_cast<int>(half.b) - 134) <= 3); // 10.5 / 20 of full blue
		CHECK(Pixel(image, 450.0f, 100.0f) == c_Blue);
		const SceneOverlayStats& stats = renderer.GetStats().Overlays;
		CHECK(stats.DrawCalls == 1);

		// Nothing changed: nothing uploaded. A mask's padding uploads its clip rects alone.
		REQUIRE(renderer.Render(ui.Target, BlackCamera(), nullptr, {}));
		CHECK(renderer.GetStats().Overlays.UploadedBytes == 0);
		panel.GetComponent<UIMaskComponent>().Padding = { 0.0f, 0.0f, 0.0f, 0.0f };
		panel.MarkModified<UIMaskComponent>();
		REQUIRE(renderer.Render(ui.Target, BlackCamera(), nullptr, {}));
		CHECK(renderer.GetStats().Overlays.UploadedBytes == 2 * sizeof(RenderData::UIClipRect));
		ReadbackImage after;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), after));
		CHECK(Pixel(after, 100.0f, 50.0f) == c_Green);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("An edited input field draws its selection and caret, its long line scrolled to the caret and clipped by its mask")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		UIScene ui;
		Entity field = ui.AddImage(ui.Canvas, "Field", { 100.0f, 100.0f }, { 300.0f, 40.0f }, { 0.0f, 0.0f, 0.0f, 0.0f });
		field.AddComponent<UIMaskComponent>();
		Entity shown = ui.Target.CreateChildEntity(field, "Shown");
		UIRectComponent inset;
		inset.AnchorMin = glm::vec2(0.0f);
		inset.AnchorMax = glm::vec2(1.0f);
		inset.Size = glm::vec2(-16.0f); // (108, 108) to (392, 132)
		shown.AddComponent<UIRectComponent>(inset);
		UITextComponent& text = shown.AddComponent<UITextComponent>();
		text.Alignment = TextAlignment::Left;
		text.FontSize = 20.0f;
		text.Color = { 1.0f, 1.0f, 1.0f, 1.0f };
		UIInputFieldComponent& component = field.AddComponent<UIInputFieldComponent>();
		component.TextElement = shown.GetUUID();
		component.Text = "    "; // Spaces: the selection shows without glyphs over it
		component.CaretColor = { 1.0f, 0.0f, 0.0f, 1.0f };
		component.SelectionColor = { 0.0f, 1.0f, 0.0f, 1.0f };
		GameUIScene& scene = GameUIScene::Get(ui.Target);
		scene.SetInputFieldState(field.GetHandle(), UIInputFieldState { 2, 0, true }); // The first two spaces selected

		SceneRenderer renderer(ExactSpecification());
		const ReadbackImage selected = RenderUI(renderer, ui.Target, { 640, 360 });
		const auto [selectionMin, selectionMax] = FindPixels(selected, [](const glm::u8vec4& pixel) { return pixel == c_Green; });
		const auto [caretMin, caretMax] = FindPixels(selected, [](const glm::u8vec4& pixel) { return pixel == c_Red; });
		REQUIRE(selectionMin.x < selectionMax.x);
		REQUIRE(caretMin.x < caretMax.x);
		CHECK(selectionMin.x == 108); // From the line's start
		CHECK(std::abs(caretMin.x - selectionMax.x) <= 1); // The caret at the selection's end
		CHECK(caretMax.x - caretMin.x <= 2);
		CHECK(caretMin.y >= 108);
		CHECK(caretMax.y <= 132);

		// A line longer than the field: scrolled so that the caret at its end shows at the right edge; nothing spills.
		field.GetComponent<UIInputFieldComponent>().Text = std::string(60, 'W');
		field.MarkModified<UIInputFieldComponent>();
		scene.SetInputFieldState(field.GetHandle(), UIInputFieldState { 60, 60, true });
		const ReadbackImage scrolled = RenderUI(renderer, ui.Target, { 640, 360 });
		const auto [endMin, endMax] = FindPixels(scrolled, [](const glm::u8vec4& pixel) { return pixel == c_Red; });
		REQUIRE(endMin.x < endMax.x);
		CHECK(endMax.x <= 392);
		CHECK(endMin.x >= 385);
		const auto [glyphsMin, glyphsMax] = FindPixels(scrolled, [](const glm::u8vec4& pixel) { return pixel.g > 128 && pixel.r > 128; });
		CHECK(glyphsMin.x >= 100); // Clipped at the field's edges by its mask
		CHECK(glyphsMax.x <= 400);
		CHECK(glyphsMin.x <= 102); // The scrolled line runs past the left edge

		// Not edited: the line shows from its start.
		scene.ClearInputFieldState(field.GetHandle());
		const ReadbackImage unedited = RenderUI(renderer, ui.Target, { 640, 360 });
		const auto [startMin, startMax] = FindPixels(unedited, [](const glm::u8vec4& pixel) { return pixel.g > 128 && pixel.r > 128; });
		CHECK(startMin.x >= 108);
		CHECK(startMin.x <= 112);
		CHECK(FindPixels(unedited, [](const glm::u8vec4& pixel) { return pixel == c_Red; }).first.x > 640); // No caret
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A world-space canvas draws in perspective where the camera sees it, behind nearer geometry, with the HUD hidden")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		UIScene ui;
		// A HUD square in the screen's top-left corner, and a 4 x 2 m sign at the origin: green, a blue square on its left,
		// a white "I" on its right.
		ui.AddImage(ui.Canvas, "HUD", { 0.0f, 0.0f }, { 20.0f, 20.0f }, { 0.0f, 0.0f, 1.0f, 1.0f });
		Entity sign = ui.Target.CreateEntity("Sign");
		sign.AddComponent<UIRectComponent>();
		UICanvasComponent& canvas = sign.AddComponent<UICanvasComponent>();
		canvas.RenderMode = UICanvasRenderMode::WorldSpace;
		canvas.ReferenceResolution = { 400.0f, 200.0f };
		canvas.WorldUnitsPerUnit = 0.01f;
		ui.AddImage(sign, "Panel", { 0.0f, 0.0f }, { 400.0f, 200.0f }, { 0.0f, 1.0f, 0.0f, 1.0f });
		ui.AddImage(sign, "Square", { 40.0f, 60.0f }, { 80.0f, 80.0f }, { 0.0f, 0.0f, 1.0f, 1.0f });
		Entity label = ui.Target.CreateChildEntity(sign, "Label");
		UIRectComponent& labelRect = label.AddComponent<UIRectComponent>();
		labelRect.AnchorMin = { 0.0f, 0.0f };
		labelRect.AnchorMax = { 0.0f, 0.0f };
		labelRect.Pivot = { 0.0f, 0.0f };
		labelRect.Position = { 260.0f, 20.0f };
		labelRect.Size = { 120.0f, 160.0f };
		UITextComponent& text = label.AddComponent<UITextComponent>();
		text.Text = "I";
		text.FontSize = 150.0f;
		text.Color = { 1.0f, 1.0f, 1.0f, 1.0f };

		const glm::uvec2 size(400, 200);
		SceneCamera camera = BlackCamera();
		camera.Projection = Math::PerspectiveReverseZ(camera.VerticalFOV, 2.0f, camera.Near, camera.Far);
		const glm::mat4 viewProjection = camera.Projection * camera.View;
		GameUIScene& scene = GameUIScene::Get(ui.Target);
		auto projectedBox = [&]()
		{
			const std::span<const UICanvasView> views = scene.EnsureLayout({ glm::vec2(size), 1.0f });
			const auto world = std::find_if(views.begin(), views.end(), [](const UICanvasView& view) { return view.WorldSpace; });
			REQUIRE(world != views.end());
			const glm::mat4 toWorld = scene.GetCanvasWorldMatrix(*world);
			glm::vec2 min(std::numeric_limits<float>::max());
			glm::vec2 max(std::numeric_limits<float>::lowest());
			for (const glm::vec2 corner : { glm::vec2(0.0f, 0.0f), glm::vec2(400.0f, 0.0f), glm::vec2(0.0f, 200.0f), glm::vec2(400.0f, 200.0f) })
			{
				const std::optional<glm::vec2> pixel = GameUIPointer::ProjectCanvasPoint(viewProjection, toWorld, glm::vec2(size), corner);
				REQUIRE(pixel.has_value());
				min = glm::min(min, *pixel);
				max = glm::max(max, *pixel);
			}
			return std::pair<glm::vec2, glm::vec2>(min, max);
		};
		auto isGreen = [](const glm::u8vec4& pixel) { return pixel.g > 128 && pixel.r < 64 && pixel.b < 64; };
		auto checkBox = [&](const ReadbackImage& image)
		{
			const auto [expectedMin, expectedMax] = projectedBox();
			const auto [min, max] = FindPixels(image, isGreen);
			CHECK(std::abs(static_cast<float>(min.x) - expectedMin.x) <= 1.0f);
			CHECK(std::abs(static_cast<float>(min.y) - expectedMin.y) <= 1.0f);
			CHECK(std::abs(static_cast<float>(max.x) - expectedMax.x) <= 1.0f);
			CHECK(std::abs(static_cast<float>(max.y) - expectedMax.y) <= 1.0f);
		};

		SceneRenderer renderer(ExactSpecification());
		{
			const ReadbackImage image = RenderUI(renderer, ui.Target, size);
			checkBox(image);
			CHECK(Pixel(image, 5.0f, 5.0f) == c_Blue); // The HUD
			CHECK(Pixel(image, 200.0f, 100.0f) == c_Green);
			// The blue square and the white glyph, in their halves of the sign.
			CHECK(Pixel(image, 200.0f - 120.0f * 138.6f / 400.0f, 100.0f) == c_Blue); // The sign is about 138.6 pixels wide
			CHECK(Pixel(image, 200.0f - 40.0f * 138.6f / 400.0f, 100.0f) == c_Green);
			const auto [whiteMin, whiteMax] = FindPixels(image, [](const glm::u8vec4& pixel) { return pixel == c_White; });
			CHECK(whiteMin.x > 200);
			CHECK(whiteMax.x < 300);
			CHECK(whiteMax.y - whiteMin.y > 30); // 1.5 m at 5 m: about 50 pixels
			const SceneOverlayStats& stats = renderer.GetStats().Overlays;
			CHECK(stats.Canvases == 2);
			CHECK(stats.DrawCalls == 2); // One for the scene's canvases, one for the screen's
		}

		// Turned 40 degrees about Y it is a trapezoid, still where the camera sees its corners.
		sign.GetComponent<TransformComponent>().Rotation = glm::quat(glm::vec3(0.0f, glm::radians(40.0f), 0.0f));
		sign.MarkModified<TransformComponent>();
		{
			const ReadbackImage image = RenderUI(renderer, ui.Target, size);
			checkBox(image);
			const auto [min, max] = FindPixels(image, isGreen);
			const auto columnHeight = [&](int x)
			{
				int height = 0;
				for (uint32_t y = 0; y < image.Height; y++)
					height += isGreen(GetPixelRGBA8(image, static_cast<uint32_t>(x), y)) ? 1 : 0;
				return height;
			};
			// The edge turned towards the camera (+X turns to -Z, away from it) is taller.
			CHECK(columnHeight(min.x + 2) > columnHeight(max.x - 3) + 4);
		}
		sign.GetComponent<TransformComponent>().Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		sign.MarkModified<TransformComponent>();

		// A red cube between the camera and the sign hides its middle; behind it, the sign hides the cube.
		Entity cube = AddMesh(ui.Target, BuiltinAssets::CubeMesh, assets.AddMaterial(UnlitColor({ 1.0f, 0.0f, 0.0f, 1.0f })), { 0.0f, 0.0f, 1.5f }, "Cube");
		{
			const ReadbackImage image = RenderUI(renderer, ui.Target, size);
			CHECK(Pixel(image, 200.0f, 100.0f) == c_Red);
			const auto [expectedMin, expectedMax] = projectedBox();
			CHECK(Pixel(image, expectedMin.x + 3.0f, expectedMin.y + 3.0f) == c_Green);
		}
		cube.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, -1.5f };
		cube.MarkModified<TransformComponent>();
		{
			const ReadbackImage image = RenderUI(renderer, ui.Target, size);
			CHECK(Pixel(image, 200.0f, 100.0f) == c_Green);
			CHECK(FindPixels(image, [](const glm::u8vec4& pixel) { return pixel == c_Red; }).first.x == std::numeric_limits<int>::max());
		}

		// Hiding the screen-space UI hides the HUD, not the sign.
		SceneRenderOptions hidden;
		hidden.DrawScreenSpaceText = false;
		{
			const ReadbackImage image = RenderUI(renderer, ui.Target, size, nullptr, hidden);
			CHECK(Pixel(image, 5.0f, 5.0f) == c_Black);
			checkBox(image);
			CHECK(renderer.GetStats().Overlays.Canvases == 1);
			CHECK(renderer.GetStats().Overlays.HiddenCanvases == 1);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A world-space canvas 100 km from the origin draws as it does at the origin, behind the same geometry")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const AssetHandle red = assets.AddMaterial(UnlitColor({ 1.0f, 0.0f, 0.0f, 1.0f }));
		// A 2 x 1 m sign with a checker of 10 cm squares, a cube turned through it (the sign's depth test against the scene
		// decides the line where they meet), seen from a meter and a half: a pixel spans about 7 mm there, what 32-bit world
		// positions resolve 100 km from the origin. Everything at `offset` (exact as floats far from the origin too).
		const auto renderAt = [&](const glm::vec3& offset)
		{
			UIScene ui;
			Entity sign = ui.Target.CreateEntity("Sign");
			sign.GetComponent<TransformComponent>().Translation = offset;
			sign.AddComponent<UIRectComponent>();
			UICanvasComponent& canvas = sign.AddComponent<UICanvasComponent>();
			canvas.RenderMode = UICanvasRenderMode::WorldSpace;
			canvas.ReferenceResolution = { 200.0f, 100.0f };
			canvas.WorldUnitsPerUnit = 0.01f;
			ui.AddImage(sign, "Panel", { 0.0f, 0.0f }, { 200.0f, 100.0f }, { 0.0f, 1.0f, 0.0f, 1.0f });
			for (int y = 0; y < 10; y++)
			{
				for (int x = (y % 2); x < 20; x += 2)
					ui.AddImage(sign, "Square", { static_cast<float>(x) * 10.0f, static_cast<float>(y) * 10.0f }, { 10.0f, 10.0f }, { 0.0f, 0.0f, 1.0f, 1.0f });
			}
			Entity cube = AddMesh(ui.Target, BuiltinAssets::CubeMesh, red, offset + glm::vec3(0.25f, 0.0f, 0.0f), "Cube");
			cube.GetComponent<TransformComponent>().Rotation = glm::quat(glm::vec3(0.3f, 0.7f, 0.0f));
			cube.GetComponent<TransformComponent>().Scale = glm::vec3(0.5f);

			const glm::uvec2 size(256, 256);
			SceneCamera camera = LookAt(offset + glm::vec3(0.125f, 0.25f, 1.5f), offset + glm::vec3(0.0f, 0.0f, 0.0f));
			camera.Projection = Math::PerspectiveReverseZ(glm::radians(60.0f), 1.0f, 0.1f, 500.0f);
			camera.ClearColor = { 0.0f, 0.0f, 0.0f, 1.0f };
			SceneRenderer renderer(ExactSpecification());
			renderer.SetViewportSize(size.x, size.y);
			ReadbackImage image;
			for (int frame = 0; frame < 3; frame++)
				image = RenderToImage(renderer, ui.Target, camera);
			CHECK(renderer.GetStats().Overlays.Canvases == 2); // The sign, and UIScene's empty screen canvas
			return image;
		};
		const ReadbackImage origin = renderAt(glm::vec3(0.0f));
		const ReadbackImage distant = renderAt(glm::vec3(100'000.0f, 0.0f, 100'000.0f));
		size_t differing = 0;
		for (size_t index = 0; index < origin.Pixels.size(); index += 4)
		{
			for (size_t channel = 0; channel < 3; channel++)
			{
				if (std::abs(int(origin.Pixels[index + channel]) - int(distant.Pixels[index + channel])) > 2)
				{
					differing++;
					break;
				}
			}
		}
		MESSAGE(differing, " of ", origin.Pixels.size() / 4, " pixels differ 100 km away");
		CHECK(static_cast<double>(differing) <= 0.001 * static_cast<double>(origin.Pixels.size() / 4));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("With TAA a world-space canvas on a wall covers the same pixels in every jitter phase")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::QuadMesh));
		UIScene ui;
		PostProcessComponent& postProcess = ui.Target.GetRegistry().get<PostProcessComponent>(ui.Target.GetRegistry().view<PostProcessComponent>().front());
		postProcess.AntiAliasing = true;
		postProcess.AntiAliasingMethod = AntiAliasingMode::TAA;
		// A green 2 x 1 m sign 2 mm in front of a black wall, both seen at a grazing angle (a steep depth slope).
		const glm::quat rotation = glm::angleAxis(glm::radians(70.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		Entity wall = AddMesh(ui.Target, BuiltinAssets::QuadMesh, assets.AddMaterial(UnlitColor({ 0.0f, 0.0f, 0.0f, 1.0f })), glm::vec3(0.0f), "Wall");
		wall.GetComponent<TransformComponent>().Rotation = rotation;
		wall.GetComponent<TransformComponent>().Scale = glm::vec3(4.0f);
		Entity sign = ui.Target.CreateEntity("Sign");
		sign.GetComponent<TransformComponent>().Translation = rotation * glm::vec3(0.0f, 0.0f, 0.002f);
		sign.GetComponent<TransformComponent>().Rotation = rotation;
		sign.AddComponent<UIRectComponent>();
		UICanvasComponent& canvas = sign.AddComponent<UICanvasComponent>();
		canvas.RenderMode = UICanvasRenderMode::WorldSpace;
		canvas.ReferenceResolution = { 200.0f, 100.0f };
		canvas.WorldUnitsPerUnit = 0.01f;
		ui.AddImage(sign, "Panel", { 0.0f, 0.0f }, { 200.0f, 100.0f }, { 0.0f, 1.0f, 0.0f, 1.0f });

		const glm::uvec2 size(64, 64);
		SceneCamera camera = LookAt(glm::vec3(1.0f, 0.4f, 2.5f), glm::vec3(0.0f));
		camera.ClearColor = { 0.0f, 0.0f, 0.0f, 1.0f };
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(size.x, size.y);
		for (int frame = 0; frame < 3; frame++)
			REQUIRE(renderer.Render(ui.Target, camera));
		std::vector<bool> first;
		for (uint32_t phase = 0; phase < SceneRenderer::c_TemporalJitterPhases; phase++)
		{
			CAPTURE(phase);
			const ReadbackImage image = RenderToImage(renderer, ui.Target, camera);
			REQUIRE(renderer.GetStats().TemporalAntiAliasing);
			std::vector<bool> covered;
			for (size_t index = 0; index < image.Pixels.size(); index += 4)
				covered.push_back(image.Pixels[index + 1] > 0);
			if (phase == 0)
			{
				first = covered;
				CHECK(std::count(first.begin(), first.end(), true) > 100);
				continue;
			}
			size_t moved = 0;
			for (size_t pixel = 0; pixel < covered.size(); pixel++)
				moved += covered[pixel] != first[pixel] ? 1u : 0u;
			CHECK(moved == 0);
		}
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

	TEST_CASE("Textures of a world-space canvas stream to the pixels its units project to")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		constexpr AssetHandle c_Texture = AssetHandle(0xC0FFEF);
		const std::filesystem::path pack = WriteStreamingPack({ { c_Texture, AssetType::Texture, "Textures/Sign.png", CookSolidTexture(512, { 50, 100, 200, 255 }) } });
		// A 512 x 512 unit sign of 1 cm units with the texture over it, 5 m from the camera and 50 m away. No canvas on the
		// screen: its scale would count for every texture.
		uint32_t levels[2] = { ~0u, ~0u };
		for (int distance = 0; distance < 2; distance++)
		{
			CAPTURE(distance);
			std::string error;
			Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(pack, &error);
			REQUIRE_MESSAGE(manager, error);
			REQUIRE(TextureStreamer::Find(*manager));
			AssetManager::SetActive(manager);
			Scene scene("Sign");
			AddNeutralPostProcess(scene);
			Entity sign = scene.CreateEntity("Sign");
			sign.AddComponent<UIRectComponent>();
			UICanvasComponent& canvas = sign.AddComponent<UICanvasComponent>();
			canvas.RenderMode = UICanvasRenderMode::WorldSpace;
			canvas.ReferenceResolution = { 512.0f, 512.0f };
			canvas.WorldUnitsPerUnit = 0.01f;
			sign.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, distance == 0 ? 0.0f : -45.0f };
			Entity image = scene.CreateChildEntity(sign, "Image");
			UIRectComponent& rect = image.AddComponent<UIRectComponent>();
			rect.AnchorMin = glm::vec2(0.0f);
			rect.AnchorMax = glm::vec2(1.0f);
			image.AddComponent<UIImageComponent>().Texture = c_Texture;
			SceneRenderer renderer;
			renderer.SetViewportSize(512, 512);
			SceneCamera camera = BlackCamera();
			camera.Projection = Math::PerspectiveReverseZ(camera.VerticalFOV, 1.0f, camera.Near, camera.Far);
			for (int frame = 0; frame < 200; frame++)
			{
				manager->Update();
				REQUIRE(renderer.Render(scene, camera));
				const Ref<Texture> texture = manager->PeekLoadedAsset<Texture>(c_Texture);
				const AssetManagerStats stats = manager->GetStats();
				if (texture && frame > 10 && stats.Detail.Reads == 0 && stats.Detail.Changes == 0)
				{
					levels[distance] = texture->GetResidentLevel();
					break;
				}
			}
			CHECK(renderer.GetStats().Overlays.Canvases == 1);
			AssetManager::SetActive(nullptr);
		}
		// Near, a canvas unit (a texel) covers more than a pixel: the whole texture. Ten times as far, a pixel covers about
		// eight texels: level 3, within one of the projection's rounding.
		CHECK(levels[0] == 0);
		CHECK(levels[1] >= 2);
		CHECK(levels[1] <= 4);
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

	TEST_CASE("The UI pass lets go of atlas textures and buffers once frames stop drawing its UI")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();
		UIScene ui;
		Entity label = ui.AddImage(ui.Canvas, "Label", { 10.0f, 10.0f }, { 200.0f, 30.0f }, { 0.1f, 0.1f, 0.1f, 1.0f });
		label.AddComponent<UITextComponent>().Text = "Atlas";
		Scene withoutUI("Without UI");

		// The pass as a SceneRenderer drives it, over a target of its own.
		FontAtlasCache atlases;
		GameUIRenderer pass(SceneOverlayPassContext { "UIPassRelease" });
		nvrhi::TextureDesc desc;
		desc.width = 256;
		desc.height = 256;
		desc.format = nvrhi::Format::RGBA8_UNORM;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		nvrhi::TextureHandle target = device->createTexture(desc);
		REQUIRE(target);
		nvrhi::FramebufferHandle framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));
		nvrhi::CommandListHandle commandList = device->createCommandList();
		std::vector<VisibleTextureUse> visibleTextures;
		SceneOverlayStats stats;
		auto renderFrame = [&](Scene& scene, bool drawScreenSpace)
		{
			atlases.BeginFrame(GlyphRasterBudget {});
			commandList->open();
			stats = {};
			SceneOverlayFrame frame;
			frame.OverlayScene = &scene;
			frame.ViewportSize = { 256, 256 };
			frame.DrawScreenSpace = drawScreenSpace;
			frame.CommandList = commandList;
			frame.Atlases = &atlases;
			frame.VisibleTextures = &visibleTextures;
			frame.Stats = &stats;
			const bool draws = pass.Prepare(frame);
			if (draws)
				pass.Draw(commandList, framebuffer);
			commandList->close();
			REQUIRE(gpu.ExecuteAndWait(commandList));
			visibleTextures.clear();
			return draws;
		};

		REQUIRE(renderFrame(ui.Target, true));
		CHECK(stats.PendingGlyphs == 0);
		// The default font's atlas (never dropped by the cache): how many hold its texture, this handle included.
		FontAtlas* atlas = atlases.Get(Font::GetDefault());
		REQUIRE(atlas);
		nvrhi::TextureHandle texture = atlas->GetTexture();
		REQUIRE(texture);
		auto countReferences = [&texture]()
		{
			texture->AddRef();
			return texture->Release();
		};
		const unsigned long whileDrawn = countReferences();

		// A scene without canvases (a game's next level after its menu): the pass holds neither the atlas nor its buffers.
		CHECK_FALSE(renderFrame(withoutUI, true));
		CHECK(countReferences() < whileDrawn);
		// Drawn again, everything is uploaded again into new buffers.
		REQUIRE(renderFrame(ui.Target, true));
		CHECK(stats.UploadedBytes >= stats.Quads * sizeof(RenderData::UIQuad));
		CHECK(countReferences() == whileDrawn);
		REQUIRE(renderFrame(ui.Target, true));
		CHECK(stats.UploadedBytes == 0);

		// Screen-space canvases hidden (an editor view without the game UI): the atlas is let go the frame after.
		CHECK_FALSE(renderFrame(ui.Target, false));
		CHECK_FALSE(renderFrame(ui.Target, false));
		CHECK(countReferences() < whileDrawn);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
