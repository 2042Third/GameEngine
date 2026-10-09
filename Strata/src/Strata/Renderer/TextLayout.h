#pragma once

#include "Strata/Scene/Components.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace Strata
{

	class FontAtlas;

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

	// Decodes UTF-8; malformed sequences, overlong encodings and surrogates become U+FFFD.
	void DecodeUTF8(std::string_view text, std::vector<uint32_t>& outCodepoints);

	// Lays out UTF-8 text in em units (font size 1). Lines break at '\n' and each is aligned around x = 0: Left lines start
	// at it, Center lines are centered on it, Right lines end at it. The first line's top (its ascent) is at y = 0 and
	// later lines follow downward (-Y). Tabs advance by four spaces; other control characters are skipped. Glyphs are
	// added to the atlas as needed; pending glyphs (see GlyphInfo::Pending) keep their advance but get no quad.
	void LayoutText(FontAtlas& atlas, std::string_view text, TextAlignment alignment, TextLayout& outLayout);

}
