#include "Renderer/FontTestUtils.h"
#include "Renderer/SceneRendererTestUtils.h"

#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/FontAtlas.h"
#include "Strata/Renderer/TextLayout.h"
#include "Strata/Renderer/TextRenderer.h"

#include <map>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Width = 128;
	constexpr uint32_t c_Height = 64;

	struct Coverage
	{
		uint32_t Pixels = 0; // Pixels the text touches
		glm::uvec2 Min = glm::uvec2(UINT32_MAX);
		glm::uvec2 Max = glm::uvec2(0);
		glm::vec2 Centroid = glm::vec2(0.0f);
	};

	// Pixels differing from the background color.
	Coverage MeasureCoverage(const ReadbackImage& image, const glm::u8vec4& background)
	{
		Coverage coverage;
		glm::vec2 sum(0.0f);
		for (uint32_t y = 0; y < image.Height; y++)
		{
			for (uint32_t x = 0; x < image.Width; x++)
			{
				const glm::ivec4 difference = glm::abs(glm::ivec4(GetPixelRGBA8(image, x, y)) - glm::ivec4(background));
				if (std::max({ difference.r, difference.g, difference.b }) <= 8)
					continue;
				coverage.Pixels++;
				coverage.Min = glm::min(coverage.Min, glm::uvec2(x, y));
				coverage.Max = glm::max(coverage.Max, glm::uvec2(x, y));
				sum += glm::vec2(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
			}
		}
		if (coverage.Pixels > 0)
			coverage.Centroid = sum / static_cast<float>(coverage.Pixels);
		return coverage;
	}

	TextComponent& AddText(Scene& scene, const std::string& content, float fontSize, const std::string& name = "Text")
	{
		TextComponent& text = scene.CreateEntity(name).AddComponent<TextComponent>();
		text.Text = content;
		text.FontSize = fontSize;
		return text;
	}

	ReadbackImage Render(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera)
	{
		REQUIRE(renderer.Render(scene, camera));
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
		return image;
	}

}

TEST_SUITE("GPU.SceneRenderer.Text")
{
	TEST_CASE("Screen-space text is drawn where its anchor and alignment put it")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		TextComponent& text = AddText(scene, "HELLO", 24.0f);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f), static_cast<float>(c_Width) / static_cast<float>(c_Height));
		const glm::u8vec4 black(0, 0, 0, 255);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Width, c_Height);
		Scope<FontAtlas> atlas = FontAtlas::Create(Font::GetDefault());
		REQUIRE(atlas);

		// Centered on the viewport: the glyphs fill the layout's box around the center.
		ReadbackImage image = Render(renderer, scene, camera);
		CHECK(renderer.GetStats().Texts == 1);
		CHECK(renderer.GetStats().TextGlyphs == 5);
		Coverage coverage = MeasureCoverage(image, black);
		REQUIRE(coverage.Pixels > 50);
		TextLayout layout;
		LayoutText(*atlas, "HELLO", TextAlignment::Center, layout);
		float inkLeft = std::numeric_limits<float>::max();
		float inkRight = std::numeric_limits<float>::lowest();
		for (const TextGlyphQuad& quad : layout.Quads)
		{
			// Quads include the distance field padding around the ink.
			const float padding = static_cast<float>(FontAtlas::c_DistancePadding) / FontAtlas::c_GlyphEmSize;
			inkLeft = std::min(inkLeft, quad.Min.x + padding);
			inkRight = std::max(inkRight, quad.Max.x - padding);
		}
		CHECK(std::abs(static_cast<float>(coverage.Min.x) - (64.0f + inkLeft * 24.0f)) <= 2.0f);
		CHECK(std::abs(static_cast<float>(coverage.Max.x) + 1.0f - (64.0f + inkRight * 24.0f)) <= 2.0f);
		CHECK(std::abs(coverage.Centroid.x - 64.0f) <= 2.0f);
		// Capital letters span from the baseline up to about 0.7 em: the block (ascent to descent) is centered.
		const float blockTop = 32.0f - (layout.Max.y - layout.Min.y) * 24.0f * 0.5f;
		const float baseline = blockTop + atlas->GetMetrics().Ascent * 24.0f;
		CHECK(std::abs(static_cast<float>(coverage.Max.y) + 1.0f - baseline) <= 2.0f);
		CHECK(coverage.Min.y > blockTop);

		// Top-left anchor, left-aligned, with a pixel offset: the text hangs from the offset point.
		text.ScreenAnchor = glm::vec2(0.0f, 0.0f);
		text.ScreenOffset = glm::vec2(6.0f, 4.0f);
		text.Alignment = TextAlignment::Left;
		coverage = MeasureCoverage(Render(renderer, scene, camera), black);
		CHECK(coverage.Min.x >= 6);
		CHECK(coverage.Min.x <= 6 + 4);
		CHECK(coverage.Min.y >= 4);
		CHECK(coverage.Min.y <= 4 + 10);

		// Bottom-right anchor, right-aligned, offset inward: the text ends at the offset point.
		text.ScreenAnchor = glm::vec2(1.0f, 1.0f);
		text.ScreenOffset = glm::vec2(-6.0f, -4.0f);
		text.Alignment = TextAlignment::Right;
		coverage = MeasureCoverage(Render(renderer, scene, camera), black);
		CHECK(coverage.Max.x < c_Width - 6);
		CHECK(coverage.Max.x >= c_Width - 6 - 4);
		CHECK(coverage.Max.y < c_Height - 4);

		// Two lines stack downward.
		text.ScreenAnchor = glm::vec2(0.5f);
		text.ScreenOffset = glm::vec2(0.0f);
		text.Alignment = TextAlignment::Center;
		text.Text = "I\nI";
		coverage = MeasureCoverage(Render(renderer, scene, camera), black);
		CHECK(coverage.Max.y - coverage.Min.y > static_cast<uint32_t>(atlas->GetMetrics().LineHeight * 24.0f));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Text colors blend over the image and larger font sizes draw larger glyphs")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(UnlitColor({ 0.5f, 0.5f, 0.5f, 1.0f })), glm::vec3(0.0f), "Background")
			.GetComponent<TransformComponent>().Scale = glm::vec3(20.0f);
		TextComponent& text = AddText(scene, "M", 48.0f);
		text.Color = glm::vec4(1.0f, 0.0f, 0.0f, 0.5f);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f), static_cast<float>(c_Width) / static_cast<float>(c_Height));

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Width, c_Height);
		const ReadbackImage image = Render(renderer, scene, camera);
		const glm::u8vec4 background = GetPixelRGBA8(image, 2, 2);
		REQUIRE(std::abs(background.r - 188) <= 1); // 0.5 linear, sRGB-encoded
		// Inside a stroke the text covers fully: half red over the background, mixed in display space.
		std::map<int, int> reds;
		for (uint32_t y = 0; y < c_Height; y++)
		{
			for (uint32_t x = 0; x < c_Width; x++)
			{
				const glm::u8vec4 pixel = GetPixelRGBA8(image, x, y);
				if (pixel.g < background.g - 40)
					reds[pixel.r]++;
			}
		}
		REQUIRE_FALSE(reds.empty());
		const auto mostCommon = std::max_element(reds.begin(), reds.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
		CHECK(std::abs(mostCommon->first - (255 + 188) / 2) <= 2);
		const glm::u8vec4 stroke = GetPixelRGBA8(image, c_Width / 2, c_Height / 2);
		CHECK(stroke.b == stroke.g);

		const uint32_t smallPixels = MeasureCoverage([&]() { text.FontSize = 16.0f; return Render(renderer, scene, camera); }(), background).Pixels;
		const uint32_t largePixels = MeasureCoverage([&]() { text.FontSize = 48.0f; return Render(renderer, scene, camera); }(), background).Pixels;
		CHECK(largePixels > smallPixels * 5); // Area grows with the square of the size
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("World-space text lies in its entity's plane and is hidden behind geometry")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		Entity sign = scene.CreateEntity("Sign");
		TextComponent& text = sign.AddComponent<TextComponent>();
		text.Text = "WORLD";
		text.FontSize = 0.4f; // World units per em
		text.ScreenSpace = false;
		text.Color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f), static_cast<float>(c_Width) / static_cast<float>(c_Height));
		const glm::u8vec4 black(0, 0, 0, 255);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Width, c_Height);
		Coverage coverage = MeasureCoverage(Render(renderer, scene, camera), black);
		REQUIRE(coverage.Pixels > 30);
		// Centered on the entity: around the image center.
		CHECK(std::abs(coverage.Centroid.x - 64.0f) <= 3.0f);
		CHECK(std::abs(coverage.Centroid.y - 32.0f) <= 4.0f);
		const uint32_t widthAtThree = coverage.Max.x - coverage.Min.x;

		// It is part of the world: closer means larger, and the entity's transform moves it.
		sign.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 1.5f);
		sign.MarkModified<TransformComponent>();
		coverage = MeasureCoverage(Render(renderer, scene, camera), black);
		CHECK(coverage.Max.x - coverage.Min.x > widthAtThree * 3 / 2);
		sign.GetComponent<TransformComponent>().Translation = glm::vec3(-0.8f, 0.0f, 0.0f);
		sign.MarkModified<TransformComponent>();
		coverage = MeasureCoverage(Render(renderer, scene, camera), black);
		CHECK(coverage.Centroid.x < 64.0f - 10.0f);

		// A wall in front hides it.
		sign.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f);
		sign.MarkModified<TransformComponent>();
		AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(UnlitColor({ 0.0f, 0.0f, 1.0f, 1.0f })), glm::vec3(0.0f, 0.0f, 1.0f), "Wall")
			.GetComponent<TransformComponent>().Scale = glm::vec3(10.0f);
		coverage = MeasureCoverage(Render(renderer, scene, camera), glm::u8vec4(0, 0, 255, 255));
		CHECK(coverage.Pixels == 0);
		CHECK(renderer.GetStats().Texts == 1);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Text uses its font asset, the default font meanwhile, and renders into external framebuffers")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		TextComponent& text = AddText(scene, "Font 42", 20.0f);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f), static_cast<float>(c_Width) / static_cast<float>(c_Height));

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Width, c_Height);
		const ReadbackImage withDefault = Render(renderer, scene, camera);
		CHECK(renderer.GetStats().PendingAssets == 0);
		REQUIRE(MeasureCoverage(withDefault, glm::u8vec4(0, 0, 0, 255)).Pixels > 50);

		// A font asset of its own draws its glyphs (Cousine, monospaced, unlike the default Roboto).
		const std::vector<uint8_t> cousineData = ReadSourceFile("Strata/vendor/imgui/misc/fonts/Cousine-Regular.ttf");
		std::string error;
		const Ref<Font> cousine = Font::Create(cousineData, &error);
		REQUIRE_MESSAGE(cousine, error);
		text.Font = assets.Add(cousine, "Cousine");
		const ReadbackImage withCousine = Render(renderer, scene, camera);
		CHECK(renderer.GetStats().PendingTextGlyphs == 0);
		CHECK(renderer.GetStats().TextGlyphs == 6);
		CHECK(MeasureCoverage(withCousine, glm::u8vec4(0, 0, 0, 255)).Pixels > 50);
		CHECK(MaxDifference(withCousine, withDefault) > 128);

		// A font that is still loading counts as pending; the text shows in the default font meanwhile, then in its own.
		const std::filesystem::path path = CreateTemporaryDirectory("TextFonts") / "Fonts.stpak";
		AssetMetadata metadata;
		metadata.Handle = UUID(0xF0F0);
		metadata.Type = AssetType::Font;
		metadata.Path = "Fonts/Packed.ttf";
		REQUIRE(AssetPack::Write(path, { metadata }, [&cousineData](const AssetMetadata&, std::vector<uint8_t>& outData, std::string*)
		{
			outData = cousineData;
			return true;
		}));
		Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(path);
		REQUIRE(manager);
		AssetManager::SetActive(manager);
		text.Font = UUID(0xF0F0);
		CHECK(MaxDifference(Render(renderer, scene, camera), withDefault) == 0);
		CHECK(renderer.GetStats().PendingAssets == 1);
		REQUIRE(manager->WaitForPendingLoads());
		CHECK(MaxDifference(Render(renderer, scene, camera), withCousine) == 0);
		CHECK(renderer.GetStats().PendingAssets == 0);
		AssetManager::SetActive(assets.Manager);

		// Text alone also goes through the external-target path.
		nvrhi::TextureDesc desc;
		desc.width = c_Width;
		desc.height = c_Height;
		desc.format = nvrhi::Format::RGBA8_UNORM;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		nvrhi::TextureHandle target = gpu.GetNvrhiDevice()->createTexture(desc);
		REQUIRE(target);
		nvrhi::FramebufferHandle framebuffer = gpu.GetNvrhiDevice()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));
		text.Font = UUID::Null();
		REQUIRE(renderer.Render(scene, camera, framebuffer));
		ReadbackImage external;
		REQUIRE(Renderer::ReadTexture(target, external));
		CHECK(MaxDifference(external, withDefault) == 0);

		// No text, nothing drawn.
		text.Text.clear();
		CHECK(MeasureCoverage(Render(renderer, scene, camera), glm::u8vec4(0, 0, 0, 255)).Pixels == 0);
		CHECK(renderer.GetStats().Texts == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Text with many new glyphs completes over frames within the rasterization budget")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		// 62 distinct glyphs in two fonts: more than one frame's budget, which the fonts share.
		const std::string letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
		AddText(scene, letters.substr(0, 31), 6.0f, "Default").ScreenAnchor = glm::vec2(0.5f, 0.3f);
		std::string error;
		const Ref<Font> cousine = Font::Create(ReadSourceFile("Strata/vendor/imgui/misc/fonts/Cousine-Regular.ttf"), &error);
		REQUIRE_MESSAGE(cousine, error);
		TextComponent& second = AddText(scene, letters.substr(31), 6.0f, "Cousine");
		second.Font = assets.Add(cousine, "Cousine");
		second.ScreenAnchor = glm::vec2(0.5f, 0.7f);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f), static_cast<float>(c_Width) / static_cast<float>(c_Height));

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Width, c_Height);
		ReadbackImage first;
		ReadbackImage image;
		uint32_t frames = 0;
		uint32_t previousGlyphs = 0;
		do
		{
			image = Render(renderer, scene, camera);
			if (frames == 0)
				first = image;
			const SceneRendererStats& stats = renderer.GetStats();
			CHECK(stats.RasterizedTextGlyphs <= TextRenderer::c_FrameRasterBudget.Glyphs);
			CHECK(stats.TextGlyphs == previousGlyphs + stats.RasterizedTextGlyphs); // Every glyph appears once
			CHECK(stats.TextGlyphs + stats.PendingTextGlyphs == letters.size());
			previousGlyphs = stats.TextGlyphs;
			frames++;
		} while (renderer.GetStats().PendingTextGlyphs > 0 && frames < 64);
		CHECK(renderer.GetStats().PendingTextGlyphs == 0);
		CHECK(frames > 1);
		CHECK(MaxDifference(first, image) > 0); // Glyphs were missing at first

		// Complete text renders the same in every later frame, and the same as in another renderer.
		CHECK(MaxDifference(Render(renderer, scene, camera), image) == 0);
		CHECK(renderer.GetStats().RasterizedTextGlyphs == 0);
		SceneRenderer other;
		other.SetViewportSize(c_Width, c_Height);
		ReadbackImage otherImage;
		for (uint32_t frame = 0; frame < frames; frame++)
			otherImage = Render(other, scene, camera);
		CHECK(other.GetStats().PendingTextGlyphs == 0);
		CHECK(MaxDifference(otherImage, image) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
