#include "stpch.h"
#include "Strata/Renderer/TextLayout.h"

#include "Strata/Renderer/FontAtlas.h"

#include <algorithm>
#include <limits>

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_ReplacementCharacter = 0xFFFD;
		constexpr uint32_t c_TabWidth = 4; // In spaces

	}

	void DecodeUTF8(std::string_view text, std::vector<uint32_t>& outCodepoints)
	{
		outCodepoints.clear();
		size_t index = 0;
		while (index < text.size())
		{
			const uint8_t lead = static_cast<uint8_t>(text[index]);
			uint32_t length = 0;
			uint32_t codepoint = 0;
			uint32_t minimum = 0;
			if (lead < 0x80)
			{
				outCodepoints.push_back(lead);
				index++;
				continue;
			}
			if ((lead & 0xE0) == 0xC0)
			{
				length = 2;
				codepoint = lead & 0x1F;
				minimum = 0x80;
			}
			else if ((lead & 0xF0) == 0xE0)
			{
				length = 3;
				codepoint = lead & 0x0F;
				minimum = 0x800;
			}
			else if ((lead & 0xF8) == 0xF0)
			{
				length = 4;
				codepoint = lead & 0x07;
				minimum = 0x10000;
			}
			else
			{
				outCodepoints.push_back(c_ReplacementCharacter); // Continuation byte or invalid lead
				index++;
				continue;
			}

			// A truncated sequence replaces only the bytes that belong to it.
			uint32_t consumed = 1;
			bool valid = true;
			for (; consumed < length; consumed++)
			{
				if (index + consumed >= text.size() || (static_cast<uint8_t>(text[index + consumed]) & 0xC0) != 0x80)
				{
					valid = false;
					break;
				}
				codepoint = (codepoint << 6) | (static_cast<uint8_t>(text[index + consumed]) & 0x3F);
			}
			if (!valid || codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
				codepoint = c_ReplacementCharacter;
			outCodepoints.push_back(codepoint);
			index += consumed;
		}
	}

	void LayoutText(FontAtlas& atlas, std::string_view text, TextAlignment alignment, TextLayout& outLayout)
	{
		outLayout.Quads.clear();
		outLayout.Min = glm::vec2(0.0f);
		outLayout.Max = glm::vec2(0.0f);
		outLayout.LineCount = 0;
		outLayout.PendingGlyphs = 0;

		std::vector<uint32_t> codepoints;
		DecodeUTF8(text, codepoints);
		const FontMetrics& metrics = atlas.GetMetrics();
		const float spaceAdvance = atlas.GetGlyph(' ').Advance;

		float left = std::numeric_limits<float>::max();
		float right = std::numeric_limits<float>::lowest();
		size_t lineStart = 0;
		while (lineStart <= codepoints.size())
		{
			size_t lineEnd = lineStart;
			while (lineEnd < codepoints.size() && codepoints[lineEnd] != '\n')
				lineEnd++;

			// Pen positions along the line, then shifted by the alignment once its width is known.
			const size_t firstQuad = outLayout.Quads.size();
			const float baseline = -metrics.Ascent - static_cast<float>(outLayout.LineCount) * metrics.LineHeight;
			float pen = 0.0f;
			const GlyphInfo* previous = nullptr;
			for (size_t index = lineStart; index < lineEnd; index++)
			{
				const uint32_t codepoint = codepoints[index];
				if (codepoint == '\t')
				{
					pen += spaceAdvance * static_cast<float>(c_TabWidth);
					previous = nullptr;
					continue;
				}
				if (codepoint < 0x20 || codepoint == 0x7F)
					continue;

				const GlyphInfo& glyph = atlas.GetGlyph(codepoint);
				if (previous)
					pen += atlas.GetKerning(*previous, glyph);
				if (glyph.Visible)
				{
					TextGlyphQuad quad;
					quad.Min = glm::vec2(pen, baseline) + glyph.PlaneMin;
					quad.Max = glm::vec2(pen, baseline) + glyph.PlaneMax;
					quad.Page = glyph.Page;
					quad.AtlasPosition = glyph.AtlasPosition;
					quad.AtlasSize = glyph.AtlasSize;
					outLayout.Quads.push_back(quad);
				}
				else if (glyph.Pending)
				{
					outLayout.PendingGlyphs++;
				}
				pen += glyph.Advance;
				previous = &glyph;
			}

			const float width = pen;
			const float shift = alignment == TextAlignment::Left ? 0.0f : (alignment == TextAlignment::Center ? -width * 0.5f : -width);
			for (size_t quad = firstQuad; quad < outLayout.Quads.size(); quad++)
			{
				outLayout.Quads[quad].Min.x += shift;
				outLayout.Quads[quad].Max.x += shift;
			}
			left = std::min(left, shift);
			right = std::max(right, shift + width);
			outLayout.LineCount++;
			lineStart = lineEnd + 1;
		}

		outLayout.Min = glm::vec2(left, -metrics.Ascent + metrics.Descent - static_cast<float>(outLayout.LineCount - 1) * metrics.LineHeight);
		outLayout.Max = glm::vec2(right, 0.0f);
	}

}
