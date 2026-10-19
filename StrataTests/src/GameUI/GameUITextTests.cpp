#include <doctest/doctest.h>

#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/FontAtlas.h"
#include "Strata/Renderer/FontFace.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/TextLayout.h"

#include <cmath>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;

namespace
{

	const FontFace& GetDefaultFace()
	{
		const FontFace* face = Font::GetDefault()->GetFace();
		REQUIRE(face);
		return *face;
	}

	ShapedText Shape(std::string_view text, const TextShapeOptions& options = {})
	{
		ShapedText shaped;
		ShapeText(GetDefaultFace(), text, options, shaped);
		return shaped;
	}

	// The width of text on one line.
	float Measure(std::string_view text)
	{
		return Shape(text).Width;
	}

	// The glyphs of each line, by their baselines (top to bottom).
	std::vector<std::vector<ShapedGlyph>> SplitLines(const ShapedText& shaped)
	{
		std::vector<std::vector<ShapedGlyph>> lines;
		float baseline = 0.0f;
		for (const ShapedGlyph& glyph : shaped.Glyphs)
		{
			if (lines.empty() || std::abs(glyph.Pen.y - baseline) > 1e-4f)
			{
				lines.emplace_back();
				baseline = glyph.Pen.y;
			}
			lines.back().push_back(glyph);
		}
		return lines;
	}

	uint32_t Glyph(uint32_t codepoint)
	{
		return GetDefaultFace().GetGlyphIndex(codepoint);
	}

}

TEST_SUITE("GameUI.Text")
{
	TEST_CASE("Shaping places glyphs where the glyph atlas's text layout does, for every alignment")
	{
		Scope<FontAtlas> atlas = FontAtlas::Create(Font::GetDefault());
		REQUIRE(atlas);
		const std::string text = "Hello, AVATAR!\n\tWorld of Text\x01 Way";
		for (TextAlignment alignment : { TextAlignment::Left, TextAlignment::Center, TextAlignment::Right })
		{
			CAPTURE(static_cast<int>(alignment));
			TextLayout layout;
			LayoutText(*atlas, text, alignment, layout);
			REQUIRE(layout.PendingGlyphs == 0);
			TextShapeOptions options;
			options.Alignment = alignment;
			const ShapedText shaped = Shape(text, options);
			REQUIRE(shaped.Glyphs.size() == layout.Quads.size());
			CHECK(shaped.LineCount == layout.LineCount);
			CHECK(shaped.Min.x == doctest::Approx(layout.Min.x));
			CHECK(shaped.Max.x == doctest::Approx(layout.Max.x));
			CHECK(shaped.Min.y == doctest::Approx(layout.Min.y));
			CHECK(shaped.Max.y == doctest::Approx(layout.Max.y));
			for (size_t index = 0; index < shaped.Glyphs.size(); index++)
			{
				const FontGlyphMetrics& metrics = GetDefaultFace().GetGlyphMetrics(shaped.Glyphs[index].GlyphIndex);
				CHECK(shaped.Glyphs[index].Pen.x + metrics.PlaneMin.x == doctest::Approx(layout.Quads[index].Min.x));
				CHECK(shaped.Glyphs[index].Pen.y + metrics.PlaneMin.y == doctest::Approx(layout.Quads[index].Min.y));
				CHECK(shaped.Glyphs[index].Pen.x + metrics.PlaneMax.x == doctest::Approx(layout.Quads[index].Max.x));
			}
		}
	}

	TEST_CASE("Shaping measures text without rasterizing a glyph")
	{
		Scope<FontAtlas> atlas = FontAtlas::Create(Font::GetDefault());
		REQUIRE(atlas);
		const FontAtlasStats before = atlas->GetStats();
		ShapedText shaped;
		ShapeText(atlas->GetFace(), "Measured, never rasterized: 0123456789 \xE2\x9C\x93 \xE6\x96\x87", {}, shaped);
		CHECK_FALSE(shaped.Glyphs.empty());
		CHECK(atlas->GetStats().RasterizedGlyphs == before.RasterizedGlyphs);
		CHECK(atlas->GetCachedGlyphCount() == 0);
		CHECK(atlas->GetPageCount() == 0);

		// The width is the advances and kerning of the glyphs.
		const FontFace& face = GetDefaultFace();
		const float expected = face.GetGlyphMetrics(Glyph('A')).Advance + face.GetKerning(Glyph('A'), Glyph('V')) + face.GetGlyphMetrics(Glyph('V')).Advance;
		CHECK(Measure("AV") == doctest::Approx(expected));
		CHECK(face.GetKerning(Glyph('A'), Glyph('V')) < 0.0f);
	}

	TEST_CASE("Lines wrap between words, after hyphens and inside words longer than a line")
	{
		TextShapeOptions options;
		options.WrapWidth = Measure("one two") + 0.01f;

		// The space at the break is dropped: the second line starts at the margin, the first line's width has no space.
		ShapedText shaped = Shape("one two three", options);
		CHECK(shaped.LineCount == 2);
		std::vector<std::vector<ShapedGlyph>> lines = SplitLines(shaped);
		REQUIRE(lines.size() == 2);
		CHECK(lines[0].size() == 6);
		CHECK(lines[1].size() == 5);
		CHECK(lines[1].front().GlyphIndex == Glyph('t'));
		CHECK(lines[1].front().Pen.x == doctest::Approx(0.0f));
		CHECK(shaped.Width == doctest::Approx(Measure("one two")));
		CHECK_FALSE(shaped.Truncated);

		// Runs of spaces hang at the end of the line; the next starts after them.
		shaped = Shape("one two     three", options);
		lines = SplitLines(shaped);
		REQUIRE(lines.size() == 2);
		CHECK(lines[1].front().Pen.x == doctest::Approx(0.0f));

		// A hyphen stays on the line it ends.
		options.WrapWidth = Measure("well-do") + 0.01f;
		lines = SplitLines(Shape("well-done", options));
		REQUIRE(lines.size() == 2);
		CHECK(lines[0].size() == 5);
		CHECK(lines[0].back().GlyphIndex == Glyph('-'));

		// A word longer than the line is broken where it reaches the width.
		options.WrapWidth = Measure("abcd") + 0.01f;
		shaped = Shape("abcdabcdabcdab", options);
		CHECK(shaped.LineCount == 4);
		lines = SplitLines(shaped);
		REQUIRE(lines.size() == 4);
		CHECK(lines[0].size() == 4);
		CHECK(lines[3].size() == 2);

		// A glyph wider than the line still gets a line of its own.
		options.WrapWidth = 0.01f;
		CHECK(Shape("WW", options).LineCount == 2);

		// '\n' always breaks; a trailing one adds an empty line, and empty text is one empty line.
		options.WrapWidth = 0.0f;
		CHECK(Shape("a\nb", options).LineCount == 2);
		CHECK(Shape("a\n", options).LineCount == 2);
		CHECK(Shape("", options).LineCount == 1);
	}

	TEST_CASE("Lines align horizontally and the block vertically in its box")
	{
		const FontMetrics& metrics = GetDefaultFace().GetMetrics();
		const float lineBlock = metrics.Ascent - metrics.Descent;
		TextShapeOptions options;
		options.Alignment = TextAlignment::Center;
		ShapedText shaped = Shape("Wide line\nshort", options);
		CHECK(shaped.Min.x == doctest::Approx(-shaped.Width * 0.5f));
		CHECK(shaped.Max.x == doctest::Approx(shaped.Width * 0.5f));
		options.Alignment = TextAlignment::Right;
		shaped = Shape("Wide line\nshort", options);
		CHECK(shaped.Max.x == doctest::Approx(0.0f));
		const std::vector<std::vector<ShapedGlyph>> lines = SplitLines(shaped);
		REQUIRE(lines.size() == 2);
		const float lastAdvance = GetDefaultFace().GetGlyphMetrics(Glyph('t')).Advance;
		CHECK(lines[1].back().Pen.x + lastAdvance == doctest::Approx(0.0f).epsilon(0.001));

		// Vertical alignment in a box 10 em high, and without a box around y = 0.
		options.Alignment = TextAlignment::Left;
		options.BoxHeight = 10.0f;
		options.VerticalAlignment = TextVerticalAlignment::Top;
		shaped = Shape("Two\nlines", options);
		const float height = lineBlock + metrics.LineHeight;
		CHECK(shaped.Height == doctest::Approx(height));
		CHECK(shaped.Max.y == doctest::Approx(0.0f));
		options.VerticalAlignment = TextVerticalAlignment::Middle;
		shaped = Shape("Two\nlines", options);
		CHECK(shaped.Max.y == doctest::Approx(-(10.0f - height) * 0.5f));
		CHECK(shaped.Min.y == doctest::Approx(-(10.0f + height) * 0.5f));
		options.VerticalAlignment = TextVerticalAlignment::Bottom;
		shaped = Shape("Two\nlines", options);
		CHECK(shaped.Min.y == doctest::Approx(-10.0f));
		options.BoxHeight = 0.0f;
		shaped = Shape("Two\nlines", options);
		CHECK(shaped.Min.y == doctest::Approx(0.0f));
		options.VerticalAlignment = TextVerticalAlignment::Middle;
		shaped = Shape("Two\nlines", options);
		CHECK(shaped.Max.y == doctest::Approx(height * 0.5f));

		// Line spacing moves later lines; the first baseline stays a line's ascent below the top.
		options.VerticalAlignment = TextVerticalAlignment::Top;
		options.LineSpacing = 2.0f;
		shaped = Shape("A\nB", options);
		REQUIRE(shaped.Glyphs.size() == 2);
		CHECK(shaped.Glyphs[0].Pen.y == doctest::Approx(-metrics.Ascent));
		CHECK(shaped.Glyphs[1].Pen.y - shaped.Glyphs[0].Pen.y == doctest::Approx(-2.0f * metrics.LineHeight));
		CHECK(shaped.Height == doctest::Approx(lineBlock + 2.0f * metrics.LineHeight));
	}

	TEST_CASE("Truncation keeps the lines the box holds, and an ellipsis ends the last one shown")
	{
		const FontMetrics& metrics = GetDefaultFace().GetMetrics();
		const float twoLines = metrics.Ascent - metrics.Descent + metrics.LineHeight;
		TextShapeOptions options;
		options.BoxHeight = twoLines + 0.01f;

		// Overflow keeps everything.
		ShapedText shaped = Shape("1\n2\n3\n4", options);
		CHECK(shaped.LineCount == 4);
		CHECK_FALSE(shaped.Truncated);

		options.Overflow = TextOverflowMode::Truncate;
		shaped = Shape("1\n2\n3\n4", options);
		CHECK(shaped.LineCount == 2);
		CHECK(shaped.Truncated);
		REQUIRE(shaped.Glyphs.size() == 2);
		CHECK(shaped.Glyphs[1].GlyphIndex == Glyph('2'));

		options.Overflow = TextOverflowMode::Ellipsis;
		shaped = Shape("1\n2\n3\n4", options);
		CHECK(shaped.LineCount == 2);
		REQUIRE(GetDefaultFace().HasGlyph(0x2026));
		REQUIRE(shaped.Glyphs.size() == 3);
		CHECK(shaped.Glyphs[1].GlyphIndex == Glyph('2'));
		CHECK(shaped.Glyphs[2].GlyphIndex == Glyph(0x2026));
		CHECK(shaped.Glyphs[2].Pen.y == doctest::Approx(shaped.Glyphs[1].Pen.y));

		// Text that fits is left alone; a box lower than one line shows nothing.
		shaped = Shape("1\n2", options);
		CHECK_FALSE(shaped.Truncated);
		CHECK(shaped.Glyphs.size() == 2);
		options.BoxHeight = (metrics.Ascent - metrics.Descent) * 0.5f;
		shaped = Shape("1\n2", options);
		CHECK(shaped.LineCount == 0);
		CHECK(shaped.Glyphs.empty());
		CHECK(shaped.Truncated);

		// Wrapped text: the last line shown ends with the ellipsis within the width.
		options.BoxHeight = twoLines + 0.01f;
		options.WrapWidth = Measure("word word") + 0.01f;
		shaped = Shape("word word word word word word", options);
		CHECK(shaped.LineCount == 2);
		CHECK(shaped.Glyphs.back().GlyphIndex == Glyph(0x2026));
		CHECK(shaped.Width <= options.WrapWidth + 1e-3f);

		// A single line cut at a maximum width (labels that do not wrap).
		options = {};
		options.Overflow = TextOverflowMode::Ellipsis;
		options.MaxWidth = Measure("abcdefgh");
		shaped = Shape("abcdefghijklmnop", options);
		CHECK(shaped.Truncated);
		CHECK(shaped.Width <= options.MaxWidth + 1e-3f);
		CHECK(shaped.Glyphs.back().GlyphIndex == Glyph(0x2026));
		CHECK(shaped.Glyphs.size() < 9);
		options.Overflow = TextOverflowMode::Truncate;
		shaped = Shape("abcdefghijklmnop", options);
		CHECK(shaped.Glyphs.size() == 8);
		// Trailing spaces never make a line too wide.
		options.MaxWidth = Measure("abc");
		shaped = Shape("abc   ", options);
		CHECK_FALSE(shaped.Truncated);
	}

	TEST_CASE("Caret stops stand before each character and after the last, where the shaped line puts them")
	{
		const std::string text = "AV b\xC3\xA9\tx";
		std::vector<TextCaretStop> stops;
		const float width = MeasureCaretStops(GetDefaultFace(), text, stops);
		// A, V, space, b, é (two bytes), tab, x, and the end.
		REQUIRE(stops.size() == 8);
		const uint32_t bytes[] = { 0, 1, 2, 3, 4, 6, 7, 8 };
		for (size_t index = 0; index < stops.size(); index++)
		{
			CAPTURE(index);
			CHECK(stops[index].Byte == bytes[index]);
			if (index > 0)
				CHECK(stops[index].X >= stops[index - 1].X);
		}
		CHECK(stops.front().X == 0.0f);
		CHECK(stops.back().X == doctest::Approx(width));
		// The same width the shaper gives the line, and each stop where the line up to it ends.
		CHECK(width == doctest::Approx(Measure(text)));
		CHECK(stops[2].X == doctest::Approx(Measure("AV")));
		CHECK(stops[5].X == doctest::Approx(Measure("AV b\xC3\xA9")));
		// The tab is four spaces.
		const float space = GetDefaultFace().GetGlyphMetrics(Glyph(' ')).Advance;
		CHECK(stops[6].X - stops[5].X == doctest::Approx(4.0f * space));

		// An empty text has one stop.
		CHECK(MeasureCaretStops(GetDefaultFace(), "", stops) == 0.0f);
		REQUIRE(stops.size() == 1);
		CHECK(stops[0].Byte == 0);
	}
}
