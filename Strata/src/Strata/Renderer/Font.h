#pragma once

#include "Strata/Asset/Asset.h"

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

		uint64_t GetMemoryUsage() const override { return m_Data.size() + m_GlyphPoints.size() * sizeof(uint32_t); }
		const std::vector<uint8_t>& GetData() const { return m_Data; }

		uint32_t GetFontOffset() const { return m_FontOffset; } // Start of the font in the file (collections)
		uint32_t GetGlyphCount() const { return static_cast<uint32_t>(m_GlyphPoints.size()); }
		// Outline points of a glyph after expanding composite glyphs (0 for empty glyphs and invalid indices).
		uint32_t GetGlyphPointCount(uint32_t glyph) const { return glyph < m_GlyphPoints.size() ? m_GlyphPoints[glyph] : 0; }
		// False when the font's kerning data is malformed: text then uses the glyph advances only.
		bool HasUsableKerning() const { return m_KerningUsable; }
	private:
		Font() = default;
	private:
		std::vector<uint8_t> m_Data;
		uint32_t m_FontOffset = 0;
		bool m_KerningUsable = false;
		std::vector<uint32_t> m_GlyphPoints;
	};

}
