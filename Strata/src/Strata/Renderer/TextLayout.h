#pragma once

#include "Strata/Scene/Components.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace Strata
{

	class FontAtlas;
	class FontFace;

	struct TextGlyphQuad
	{
		glm::vec2 Min = glm::vec2(0.0f); // Corners in em units, +Y up
		glm::vec2 Max = glm::vec2(0.0f);
		uint32_t Page = 0; // Of the atlas
		glm::uvec2 AtlasPosition = glm::uvec2(0);
		glm::uvec2 AtlasSize = glm::uvec2(0);
	};

	struct TextLayout
	{
		std::vector<TextGlyphQuad> Quads;
		// The text block in em units: from the first line's ascent (y = 0) down to the last line's descent, and from the
		// left end of the widest line to its right end.
		glm::vec2 Min = glm::vec2(0.0f);
		glm::vec2 Max = glm::vec2(0.0f);
		uint32_t LineCount = 0;
		uint32_t PendingGlyphs = 0; // Glyphs left out because they are not in the atlas yet (see GlyphInfo::Pending)
	};

	enum class TextVerticalAlignment : uint8_t
	{
		Top = 0,
		Middle,
		Bottom
	};

	enum class TextOverflowMode : uint8_t
	{
		Overflow = 0, // Lines go on beyond MaxWidth and BoxHeight
		Truncate,     // Lines below BoxHeight are left out, and glyphs crossing MaxWidth
		Ellipsis      // Like Truncate, with an ellipsis ending the last line kept, or a line that was cut
	};

	// How ShapeText lays text out, in em units (font size 1).
	struct TextShapeOptions
	{
		TextAlignment Alignment = TextAlignment::Left;
		TextVerticalAlignment VerticalAlignment = TextVerticalAlignment::Top;
		// Above 0: lines wrap at this width, between words (after spaces, which a break drops, and after hyphens), and inside
		// words longer than a line.
		float WrapWidth = 0.0f;
		// Above 0, with Truncate or Ellipsis: lines are cut at this width (usually the wrap width of unwrapped text).
		float MaxWidth = 0.0f;
		// Above 0: the height of the box vertical alignment places the block in (from y = 0 down), and with Truncate or
		// Ellipsis the height lines must fit in (a line fits when its descent does).
		float BoxHeight = 0.0f;
		float LineSpacing = 1.0f; // Times the font's line height
		TextOverflowMode Overflow = TextOverflowMode::Overflow;
	};

	// A glyph with a shape, placed: its pen position on the baseline (em, +Y up). Its quad is the pen plus its plane bounds
	// (FontGlyphMetrics, GlyphInfo).
	struct ShapedGlyph
	{
		uint32_t GlyphIndex = 0;
		glm::vec2 Pen = glm::vec2(0.0f);
	};

	// Text measured and laid out without a glyph atlas (ShapeText): the glyphs with shapes and their pen positions.
	struct ShapedText
	{
		std::vector<ShapedGlyph> Glyphs;
		// The block: x around the alignment's origin (Left lines start at 0, Center lines are centered on it, Right lines end
		// at it), y from the box's top (0) down, both in em. Min.x and Max.x span the widest line, Min.y and Max.y the lines'
		// ascents and descents placed by the vertical alignment.
		glm::vec2 Min = glm::vec2(0.0f);
		glm::vec2 Max = glm::vec2(0.0f);
		float Width = 0.0f;  // Of the widest line, including its spaces
		float Height = 0.0f; // Of the block: the first line's ascent to the last line's descent
		uint32_t LineCount = 0;
		bool Truncated = false; // Lines or glyphs were left out (Truncate, Ellipsis)
	};

	// Decodes UTF-8; malformed sequences, overlong encodings and surrogates become U+FFFD.
	void DecodeUTF8(std::string_view text, std::vector<uint32_t>& outCodepoints);

	// Lays out UTF-8 text from the font's glyph metrics alone, rasterizing nothing (so layout can measure text without a
	// GPU, e.g. to fit a UI rect to it). Lines break at '\n' and, with a wrap width, between words; tabs advance by four
	// spaces; other control characters are skipped. An empty text is one empty line.
	void ShapeText(const FontFace& face, std::string_view text, const TextShapeOptions& options, ShapedText& outText);

	// Lays out UTF-8 text in em units (font size 1). Lines break at '\n' and each is aligned around x = 0: Left lines start
	// at it, Center lines are centered on it, Right lines end at it. The first line's top (its ascent) is at y = 0 and
	// later lines follow downward (-Y). Tabs advance by four spaces; other control characters are skipped. Glyphs are
	// added to the atlas as needed; pending glyphs (see GlyphInfo::Pending) keep their advance but get no quad. ShapeText
	// with the atlas's kerning cache, then FontAtlas::Resolve.
	void LayoutText(FontAtlas& atlas, std::string_view text, TextAlignment alignment, TextLayout& outLayout);

}
