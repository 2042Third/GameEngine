#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Renderer/FontValidation.h"

#include <string>
#include <vector>

namespace Strata
{

	// Font file with TrueType outlines (a .ttf, or the first font of a .ttc collection); glyph atlases are built by the
	// text renderer on demand. Font files are untrusted input: Create validates everything the glyph rasterizer reads (see
	// ValidateTrueTypeFont) and rejects malformed fonts and fonts with CFF outlines.
	class Font : public Asset
	{
	public:
		static AssetType GetStaticType() { return AssetType::Font; }
		AssetType GetType() const override { return GetStaticType(); }

		static Ref<Font> Create(std::vector<uint8_t> fontData, std::string* outError = nullptr);
		// The engine's built-in font (Roboto Medium), used by text without a font of its own. Created on first use.
		static const Ref<Font>& GetDefault();

		uint64_t GetMemoryUsage() const override { return m_Data.size() + m_GlyphShapes.size() * sizeof(GlyphShapeCost); }
		// The font file as stb_truetype reads it: inconsistent format 4 character map search parameters are corrected.
		const std::vector<uint8_t>& GetData() const { return m_Data; }

		uint32_t GetFontOffset() const { return m_FontOffset; } // Start of the font in the file (collections)
		uint32_t GetGlyphCount() const { return static_cast<uint32_t>(m_GlyphShapes.size()); }
		// The outline stb_truetype builds for a glyph (no vertices for empty glyphs and invalid indices).
		const GlyphShapeCost& GetGlyphShape(uint32_t glyph) const;
		// False when the font's kerning data is malformed or too costly to search: text then uses the glyph advances only.
		bool HasUsableKerning() const { return m_KerningUsable; }
	private:
		Font() = default;
	private:
		std::vector<uint8_t> m_Data;
		uint32_t m_FontOffset = 0;
		bool m_KerningUsable = false;
		std::vector<GlyphShapeCost> m_GlyphShapes;
	};

}
