#include <doctest/doctest.h>

#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/FontAtlas.h"
#include "Strata/Renderer/TextLayout.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

using namespace Strata;

TEST_SUITE("Renderer.Text")
{
	TEST_CASE("UTF-8 decodes to code points and malformed input to U+FFFD")
	{
		std::vector<uint32_t> codepoints;
		DecodeUTF8("A\xC3\xA9\xE2\x9C\x93\xF0\x9F\x98\x80", codepoints); // A, e acute, check mark, emoji
		CHECK(codepoints == std::vector<uint32_t> { 0x41, 0xE9, 0x2713, 0x1F600 });

		DecodeUTF8("", codepoints);
		CHECK(codepoints.empty());

		const std::vector<uint32_t> replaced = { 0xFFFD };
		for (const char* malformed : { "\xFF", "\x80", "\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80" }) // Invalid lead, stray continuation, overlong, surrogate, beyond U+10FFFF
		{
			CAPTURE(std::string(malformed).size());
			DecodeUTF8(malformed, codepoints);
			CHECK(codepoints == replaced);
		}
		// A truncated sequence replaces only its own bytes.
		DecodeUTF8("\xE2\x9C" "A", codepoints);
		CHECK(codepoints == std::vector<uint32_t> { 0xFFFD, 0x41 });
	}

	TEST_CASE("The default font builds a distance field atlas on demand")
	{
		const Ref<Font>& font = Font::GetDefault();
		REQUIRE(font);
		CHECK(&Font::GetDefault() == &font); // Created once
		std::string error;
		Scope<FontAtlas> atlas = FontAtlas::Create(font, &error);
		REQUIRE_MESSAGE(atlas, error);
		CHECK_FALSE(FontAtlas::Create(nullptr, &error));
		CHECK_FALSE(error.empty());

		const FontMetrics& metrics = atlas->GetMetrics();
		CHECK(metrics.Ascent > 0.5f);
		CHECK(metrics.Descent < 0.0f);
		CHECK(metrics.LineHeight >= metrics.Ascent - metrics.Descent);

		const GlyphInfo& letter = atlas->GetGlyph('A');
		CHECK(letter.Visible);
		CHECK(letter.Advance > 0.3f);
		CHECK(letter.PlaneMax.x > letter.PlaneMin.x);
		CHECK(letter.PlaneMax.y > 0.5f);      // Capital letters rise well above the baseline
		CHECK(letter.PlaneMin.y < 0.0f);      // The distance field padding reaches below it
		CHECK(letter.AtlasSize.x > 0);
		const GlyphInfo& space = atlas->GetGlyph(' ');
		CHECK_FALSE(space.Visible);
		CHECK(space.Advance > 0.1f);
		CHECK(&atlas->GetGlyph('A') == &letter); // Rasterized once

		// Characters the font lacks share its missing glyph.
		const GlyphInfo& missing = atlas->GetGlyph(0xE000);
		CHECK(missing.GlyphIndex == 0);
		CHECK(&atlas->GetGlyph(0xE001) == &missing);
		CHECK(&atlas->GetGlyph(0x7FFFFFFF) == &missing);

		// The texels inside a glyph are above the on-edge value, the border of its rectangle far below.
		const std::vector<uint8_t>& pixels = atlas->GetPixels();
		const glm::uvec2 size = atlas->GetSize();
		uint8_t brightest = 0;
		for (uint32_t y = letter.AtlasPosition.y; y < letter.AtlasPosition.y + letter.AtlasSize.y; y++)
		{
			for (uint32_t x = letter.AtlasPosition.x; x < letter.AtlasPosition.x + letter.AtlasSize.x; x++)
				brightest = std::max(brightest, pixels[static_cast<size_t>(y) * size.x + x]);
		}
		CHECK(brightest > 160);
		CHECK(pixels[static_cast<size_t>(letter.AtlasPosition.y) * size.x + letter.AtlasPosition.x] < 40);
	}

	TEST_CASE("The atlas grows to hold many glyphs without overlapping them")
	{
		Scope<FontAtlas> atlas = FontAtlas::Create(Font::GetDefault());
		REQUIRE(atlas);
		const uint32_t initialHeight = atlas->GetSize().y;
		std::vector<const GlyphInfo*> glyphs;
		for (uint32_t codepoint = 0x21; codepoint < 0x500; codepoint++) // Latin, Greek and Cyrillic
		{
			const GlyphInfo& glyph = atlas->GetGlyph(codepoint);
			if (glyph.Visible && std::find(glyphs.begin(), glyphs.end(), &glyph) == glyphs.end())
				glyphs.push_back(&glyph);
		}
		CHECK(glyphs.size() > 500);
		CHECK(atlas->GetSize().y > initialHeight);
		const glm::uvec2 size = atlas->GetSize();
		for (size_t first = 0; first < glyphs.size(); first++)
		{
			const GlyphInfo& a = *glyphs[first];
			REQUIRE(a.AtlasPosition.x + a.AtlasSize.x <= size.x);
			REQUIRE(a.AtlasPosition.y + a.AtlasSize.y <= size.y);
			for (size_t second = first + 1; second < glyphs.size(); second++)
			{
				const GlyphInfo& b = *glyphs[second];
				const bool separate = a.AtlasPosition.x + a.AtlasSize.x <= b.AtlasPosition.x || b.AtlasPosition.x + b.AtlasSize.x <= a.AtlasPosition.x
					|| a.AtlasPosition.y + a.AtlasSize.y <= b.AtlasPosition.y || b.AtlasPosition.y + b.AtlasSize.y <= a.AtlasPosition.y;
				REQUIRE(separate);
			}
		}
	}

	TEST_CASE("Text layout aligns lines around the origin and stacks them downward")
	{
		Scope<FontAtlas> atlas = FontAtlas::Create(Font::GetDefault());
		REQUIRE(atlas);
		const FontMetrics& metrics = atlas->GetMetrics();
		TextLayout layout;

		LayoutText(*atlas, "Hi", TextAlignment::Left, layout);
		REQUIRE(layout.Quads.size() == 2);
		CHECK(layout.LineCount == 1);
		CHECK(layout.Min.x == doctest::Approx(0.0f));
		const float width = layout.Max.x;
		CHECK(width == doctest::Approx(atlas->GetGlyph('H').Advance + atlas->GetKerning(atlas->GetGlyph('H'), atlas->GetGlyph('i')) + atlas->GetGlyph('i').Advance));
		CHECK(layout.Max.y == doctest::Approx(0.0f));
		CHECK(layout.Min.y == doctest::Approx(-metrics.Ascent + metrics.Descent));
		CHECK(layout.Quads[0].Min.x < layout.Quads[1].Min.x);

		LayoutText(*atlas, "Hi", TextAlignment::Center, layout);
		CHECK(layout.Min.x == doctest::Approx(-width * 0.5f));
		CHECK(layout.Max.x == doctest::Approx(width * 0.5f));
		LayoutText(*atlas, "Hi", TextAlignment::Right, layout);
		CHECK(layout.Max.x == doctest::Approx(0.0f));
		CHECK(layout.Quads[1].Max.x <= 0.1f);

		// Lines are aligned one by one; the second sits a line height lower.
		LayoutText(*atlas, "HHHH\nH", TextAlignment::Center, layout);
		REQUIRE(layout.Quads.size() == 5);
		CHECK(layout.LineCount == 2);
		CHECK(layout.Quads[4].Min.y == doctest::Approx(layout.Quads[0].Min.y - metrics.LineHeight));
		CHECK((layout.Quads[4].Min.x + layout.Quads[4].Max.x) * 0.5f == doctest::Approx((layout.Quads[0].Min.x + layout.Quads[3].Max.x) * 0.5f).epsilon(0.05));
		CHECK(layout.Min.y == doctest::Approx(-metrics.Ascent + metrics.Descent - metrics.LineHeight));

		// Tabs advance by four spaces, other control characters are skipped, an empty text is one empty line.
		LayoutText(*atlas, "\tH\x01", TextAlignment::Left, layout);
		REQUIRE(layout.Quads.size() == 1);
		CHECK(layout.Quads[0].Min.x >= 4.0f * atlas->GetGlyph(' ').Advance - 0.2f);
		LayoutText(*atlas, "", TextAlignment::Left, layout);
		CHECK(layout.Quads.empty());
		CHECK(layout.LineCount == 1);
	}
}
