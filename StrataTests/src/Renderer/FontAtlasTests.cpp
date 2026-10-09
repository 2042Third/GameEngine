#include <doctest/doctest.h>

#include "Renderer/FontTestUtils.h"
#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/FontAtlas.h"
#include "Strata/Renderer/TextLayout.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	constexpr uint64_t c_Unlimited = std::numeric_limits<uint64_t>::max();

	Ref<Font> LoadFont(const char* path)
	{
		std::string error;
		Ref<Font> font = Font::Create(Tests::ReadSourceFile(path), &error);
		REQUIRE_MESSAGE(font, error);
		return font;
	}

	uint8_t Texel(const FontAtlas& atlas, uint32_t page, uint32_t x, uint32_t y)
	{
		return atlas.GetPagePixels(page)[static_cast<size_t>(y) * FontAtlas::c_PageSize + x];
	}

	// The glyph's distance field equals the reference's, and the texels right of and below it (the gutter) are empty.
	void CheckGlyphTexels(const FontAtlas& atlas, const GlyphInfo& glyph, const FontAtlas* reference, const GlyphInfo* referenceGlyph)
	{
		CAPTURE(glyph.GlyphIndex);
		REQUIRE(glyph.Page < atlas.GetPageCount());
		REQUIRE(glyph.AtlasPosition.x + glyph.AtlasSize.x < FontAtlas::c_PageSize);
		REQUIRE(glyph.AtlasPosition.y + glyph.AtlasSize.y < FontAtlas::c_PageSize);
		bool emptyGutter = true;
		for (uint32_t y = glyph.AtlasPosition.y; y <= glyph.AtlasPosition.y + glyph.AtlasSize.y; y++)
			emptyGutter = emptyGutter && Texel(atlas, glyph.Page, glyph.AtlasPosition.x + glyph.AtlasSize.x, y) == 0;
		for (uint32_t x = glyph.AtlasPosition.x; x <= glyph.AtlasPosition.x + glyph.AtlasSize.x; x++)
			emptyGutter = emptyGutter && Texel(atlas, glyph.Page, x, glyph.AtlasPosition.y + glyph.AtlasSize.y) == 0;
		CHECK(emptyGutter);
		if (!reference)
			return;
		REQUIRE(referenceGlyph->Visible);
		REQUIRE(referenceGlyph->AtlasSize == glyph.AtlasSize);
		bool identical = true;
		for (uint32_t y = 0; y < glyph.AtlasSize.y; y++)
		{
			for (uint32_t x = 0; x < glyph.AtlasSize.x; x++)
			{
				identical = identical && Texel(atlas, glyph.Page, glyph.AtlasPosition.x + x, glyph.AtlasPosition.y + y)
					== Texel(*reference, referenceGlyph->Page, referenceGlyph->AtlasPosition.x + x, referenceGlyph->AtlasPosition.y + y);
			}
		}
		CHECK(identical);
	}

}

TEST_SUITE("Renderer.FontAtlas")
{
	TEST_CASE("Glyph rasterization keeps to the frame budget and pending glyphs follow on later frames")
	{
		const std::string text = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"; // 62 distinct glyphs
		Scope<FontAtlas> complete = FontAtlas::Create(Font::GetDefault());
		REQUIRE(complete);
		TextLayout expected;
		LayoutText(*complete, text, TextAlignment::Center, expected);
		REQUIRE(expected.Quads.size() == text.size());
		CHECK(expected.PendingGlyphs == 0);

		// At most ten glyphs a frame: the text completes on the seventh frame. Pending glyphs keep their advance, so the
		// layout's extent and the glyphs already drawn stay in place.
		Scope<FontAtlas> atlas = FontAtlas::Create(Font::GetDefault());
		REQUIRE(atlas);
		TextLayout layout;
		uint64_t rasterized = 0;
		uint32_t frames = 0;
		do
		{
			atlas->BeginFrame();
			atlas->SetRasterBudget({ 10, c_Unlimited });
			LayoutText(*atlas, text, TextAlignment::Center, layout);
			const uint64_t added = atlas->GetStats().RasterizedGlyphs - rasterized;
			rasterized = atlas->GetStats().RasterizedGlyphs;
			CHECK(added <= 10);
			CHECK(atlas->GetRasterBudget().Glyphs == 10 - added);
			CHECK(layout.Quads.size() == rasterized);
			CHECK(layout.Quads.size() + layout.PendingGlyphs == text.size());
			CHECK(layout.Min.x == doctest::Approx(expected.Min.x));
			CHECK(layout.Max.x == doctest::Approx(expected.Max.x));
			frames++;
		} while (layout.PendingGlyphs > 0 && frames < 20);
		CHECK(frames == 7);
		REQUIRE(layout.Quads.size() == expected.Quads.size());
		for (size_t index = 0; index < layout.Quads.size(); index++)
			CHECK(layout.Quads[index].Min.x == doctest::Approx(expected.Quads[index].Min.x));

		// The cost budget may be overdrawn by one glyph: a budget of 1 still rasterizes one glyph a frame.
		Scope<FontAtlas> costly = FontAtlas::Create(Font::GetDefault());
		REQUIRE(costly);
		for (uint32_t frame = 1; frame <= 3; frame++)
		{
			costly->BeginFrame();
			costly->SetRasterBudget({ 100, 1 });
			LayoutText(*costly, "XYZ", TextAlignment::Left, layout);
			CHECK(layout.Quads.size() == frame);
			CHECK(costly->GetRasterBudget().Cost == 0);
		}

		// Without budget glyphs are only looked up: whitespace is not pending, shapes are.
		costly->BeginFrame();
		costly->SetRasterBudget({ 0, c_Unlimited });
		const GlyphInfo& pending = costly->GetGlyph('Q');
		CHECK(pending.Pending);
		CHECK_FALSE(pending.Visible);
		CHECK(pending.GlyphIndex > 0);
		CHECK(pending.Advance > 0.3f);
		CHECK_FALSE(costly->GetGlyph(' ').Pending);
		CHECK(costly->GetGlyph('X').Visible);
	}

	TEST_CASE("Least recently used glyphs are evicted, so thousands of glyphs pass through a bounded atlas")
	{
		const Ref<Font> font = LoadFont("Strata/vendor/tracy/profiler/src/font/FiraCode-Retina.ttf");
		const uint32_t glyphCount = font->GetGlyphCount();
		REQUIRE(glyphCount > 2000);
		FontAtlasSpecification specification;
		specification.MaxPages = 2;
		Scope<FontAtlas> atlas = FontAtlas::Create(font, nullptr, specification);
		REQUIRE(atlas);
		Scope<FontAtlas> reference = FontAtlas::Create(font);
		REQUIRE(reference);

		// Two passes over every glyph of the font, a screenful of 96 glyphs at a time, each shown until it is complete.
		constexpr uint32_t c_Screen = 96;
		uint32_t drawn = 0;
		for (uint32_t pass = 0; pass < 2; pass++)
		{
			for (uint32_t first = 0; first < glyphCount; first += c_Screen)
			{
				CAPTURE(first);
				const uint32_t last = std::min(first + c_Screen, glyphCount);
				bool complete = false;
				for (uint32_t frame = 0; frame < 32 && !complete; frame++)
				{
					atlas->BeginFrame();
					atlas->SetRasterBudget({ 32, c_Unlimited });
					uint32_t pending = 0;
					for (uint32_t glyph = first; glyph < last; glyph++)
						pending += atlas->GetGlyphByIndex(glyph).Pending ? 1 : 0;
					complete = pending == 0;
				}
				REQUIRE(complete);

				// In the frame that completed the screen, every glyph is in the atlas with its own distance field.
				for (uint32_t glyph = first; glyph < last; glyph++)
				{
					const GlyphInfo& info = atlas->GetGlyphByIndex(glyph);
					if (!info.Visible)
						continue;
					drawn++;
					const bool compare = glyph % 13 == 0;
					CheckGlyphTexels(*atlas, info, compare ? reference.get() : nullptr, compare ? &reference->GetGlyphByIndex(glyph) : nullptr);
				}
				CHECK(atlas->GetPageCount() <= specification.MaxPages);
				CHECK(atlas->GetCachedGlyphCount() <= FontAtlas::c_GlyphCacheLimit + c_Screen);
			}
		}
		CHECK(drawn > 3000);
		CHECK(atlas->GetStats().EvictedGlyphs > 1000);
		CHECK(atlas->GetStats().RasterizedGlyphs >= drawn);
	}

	TEST_CASE("Glyphs used in the current frame are never evicted")
	{
		// More glyphs in one frame than a single page holds: the page fills, the rest stays pending, and every glyph drawn
		// this frame keeps its place.
		const Ref<Font> font = LoadFont("Strata/vendor/tracy/profiler/src/font/FiraCode-Retina.ttf");
		FontAtlasSpecification specification;
		specification.MaxPages = 1;
		Scope<FontAtlas> atlas = FontAtlas::Create(font, nullptr, specification);
		REQUIRE(atlas);
		atlas->BeginFrame();
		std::vector<GlyphInfo> drawn;
		std::vector<uint32_t> pending;
		for (uint32_t glyph = 0; glyph < 800; glyph++)
		{
			const GlyphInfo& info = atlas->GetGlyphByIndex(glyph);
			if (info.Visible)
				drawn.push_back(info);
			if (info.Pending)
				pending.push_back(glyph);
		}
		CHECK(drawn.size() > 100);
		CHECK(pending.size() > 100);
		CHECK(atlas->GetPageCount() == 1);
		CHECK(atlas->GetStats().EvictedGlyphs == 0);
		for (const GlyphInfo& before : drawn)
		{
			const GlyphInfo& after = atlas->GetGlyphByIndex(before.GlyphIndex);
			CHECK(after.Visible);
			CHECK(after.AtlasPosition == before.AtlasPosition);
		}
		CHECK(atlas->GetGlyphByIndex(pending.front()).Pending); // Still no room

		// In the next frame a glyph that waited replaces the least recently used one (all were used in the same frame: the
		// first in the page goes). That glyph is rasterized again when it is needed, replacing another one, but never the
		// glyph that took its place, which is in use in this frame.
		atlas->BeginFrame();
		const uint64_t rasterized = atlas->GetStats().RasterizedGlyphs;
		const GlyphInfo& replacement = atlas->GetGlyphByIndex(pending.front());
		CHECK(replacement.Visible);
		CHECK(replacement.AtlasPosition == drawn.front().AtlasPosition);
		CHECK(atlas->GetStats().EvictedGlyphs >= 1);
		const GlyphInfo& again = atlas->GetGlyphByIndex(drawn.front().GlyphIndex);
		CHECK(again.Visible);
		CHECK(again.AtlasPosition != drawn.front().AtlasPosition);
		CHECK(atlas->GetStats().RasterizedGlyphs == rasterized + 2);
		CHECK(atlas->GetGlyphByIndex(pending.front()).Visible);
		CHECK(atlas->GetStats().RasterizedGlyphs == rasterized + 2); // Not evicted, so not rasterized again
	}

	TEST_CASE("Kerning is looked up once per glyph pair, in a bounded cache")
	{
		Scope<FontAtlas> atlas = FontAtlas::Create(Font::GetDefault());
		REQUIRE(atlas);
		TextLayout layout;
		LayoutText(*atlas, "AVAVAV", TextAlignment::Left, layout);
		CHECK(atlas->GetCachedKerningCount() == 2); // AV and VA
		const float kerning = atlas->GetKerning(atlas->GetGlyph('A'), atlas->GetGlyph('V'));
		CHECK(kerning < 0.0f);
		CHECK(atlas->GetCachedKerningCount() == 2);

		// More pairs than the cache holds: it is cleared rather than grown, and answers stay the same.
		GlyphInfo left;
		GlyphInfo right;
		for (left.GlyphIndex = 0; left.GlyphIndex < 100; left.GlyphIndex++)
		{
			for (right.GlyphIndex = 0; right.GlyphIndex < 100; right.GlyphIndex++)
				atlas->GetKerning(left, right);
		}
		CHECK(atlas->GetCachedKerningCount() <= FontAtlas::c_GlyphCacheLimit);
		CHECK(atlas->GetKerning(atlas->GetGlyph('A'), atlas->GetGlyph('V')) == kerning);
	}

	TEST_CASE("Glyphs costlier than the ceiling are rasterized at reduced resolution, within it")
	{
		// The resolution a glyph was rasterized at: its quad's size in atlas texels at full resolution over its texels.
		auto reduction = [](const GlyphInfo& glyph) { return (glyph.PlaneMax.x - glyph.PlaneMin.x) * FontAtlas::c_GlyphEmSize / static_cast<float>(glyph.AtlasSize.x); };

		// Text glyphs keep the full resolution.
		const Ref<Font>& roboto = Font::GetDefault();
		Scope<FontAtlas> atlas = FontAtlas::Create(roboto, nullptr, FontAtlasSpecification { 64 });
		REQUIRE(atlas);
		uint32_t drawn = 0;
		for (uint32_t index = 0; index < roboto->GetGlyphCount(); index++)
		{
			const GlyphInfo& glyph = atlas->GetGlyphByIndex(index);
			if (!glyph.Visible)
				continue;
			CAPTURE(index);
			drawn++;
			CHECK(reduction(glyph) == doctest::Approx(1.0f));
			CHECK(FontAtlas::GetRasterCost(glyph.AtlasSize, roboto->GetGlyphShape(index)) <= FontAtlas::c_MaxGlyphRasterCost);
		}
		CHECK(drawn > 1000);

		// Emoji with hundreds of curves would take 10 to 20 ms each at full resolution (one with 2675 vertices even more):
		// they are drawn at half or a quarter of it, each within the ceiling.
		std::string error;
		const Ref<Font> emoji = Font::Create(Tests::ReadSourceFile("Strata/vendor/tracy/profiler/src/font/NotoEmoji-Regular.ttf"), &error);
		REQUIRE_MESSAGE(emoji, error);
		Scope<FontAtlas> emojiAtlas = FontAtlas::Create(emoji);
		REQUIRE(emojiAtlas);
		for (uint32_t index : { 261u, 483u, 715u, 1088u })
		{
			CAPTURE(index);
			const GlyphInfo& glyph = emojiAtlas->GetGlyphByIndex(index);
			REQUIRE(glyph.Visible);
			CHECK(FontAtlas::GetRasterCost(glyph.AtlasSize, emoji->GetGlyphShape(index)) <= FontAtlas::c_MaxGlyphRasterCost);
			const float reduced = reduction(glyph);
			CHECK((reduced == doctest::Approx(2.0f) || reduced == doctest::Approx(4.0f)));
			if (index == 715)
				CHECK(reduced == doctest::Approx(4.0f));
		}
	}
}
