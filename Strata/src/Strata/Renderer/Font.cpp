#include "stpch.h"
#include "Strata/Renderer/Font.h"

#include "Strata/Core/Assert.h"
#include "Strata/Core/EmbeddedFiles.h"
#include "Strata/Core/Log.h"
#include "Strata/Renderer/FontValidation.h"

namespace Strata
{

	Ref<Font> Font::Create(std::vector<uint8_t> fontData, std::string* outError)
	{
		TrueTypeFontFacts facts;
		std::string error;
		if (!ValidateTrueTypeFont(fontData, facts, error))
		{
			if (outError)
				*outError = error;
			return nullptr;
		}
		if (!facts.KerningUsable)
			ST_CORE_WARN("Font: kerning disabled ({})", facts.KerningIssue);
		if (facts.SearchFix)
		{
			// stb_truetype's character lookup trusts these; the validator checked that the subtable holds them.
			auto write = [&](uint64_t offset, uint16_t value)
			{
				fontData[offset] = static_cast<uint8_t>(value >> 8);
				fontData[offset + 1] = static_cast<uint8_t>(value);
			};
			const uint64_t subtable = facts.SearchFix->Offset;
			write(subtable + 8, facts.SearchFix->SearchRange);
			write(subtable + 10, facts.SearchFix->EntrySelector);
			write(subtable + 12, facts.SearchFix->RangeShift);
		}

		Ref<Font> font(new Font());
		font->m_Data = std::move(fontData);
		font->m_FontOffset = facts.FontOffset;
		font->m_KerningUsable = facts.KerningUsable;
		font->m_GlyphShapes = std::move(facts.GlyphShapes);
		return font;
	}

	const GlyphShapeCost& Font::GetGlyphShape(uint32_t glyph) const
	{
		static const GlyphShapeCost s_None;
		return glyph < m_GlyphShapes.size() ? m_GlyphShapes[glyph] : s_None;
	}

	const Ref<Font>& Font::GetDefault()
	{
		static const Ref<Font> s_Default = []()
		{
			const std::span<const uint8_t> data = EmbeddedFiles::GetDefaultFont();
			std::string error;
			Ref<Font> font = Create(std::vector<uint8_t>(data.begin(), data.end()), &error);
			ST_CORE_VERIFY(font, "The embedded default font is invalid: {}", error);
			return font;
		}();
		return s_Default;
	}

}
