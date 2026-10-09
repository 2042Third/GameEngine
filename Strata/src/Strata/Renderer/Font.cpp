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

		Ref<Font> font(new Font());
		font->m_Data = std::move(fontData);
		font->m_FontOffset = facts.FontOffset;
		font->m_KerningUsable = facts.KerningUsable;
		font->m_GlyphPoints = std::move(facts.GlyphPoints);
		return font;
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
