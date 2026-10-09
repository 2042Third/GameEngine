#pragma once

#include "Strata/Core/Base.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class Font;

	// Vertical font metrics in em units (font size 1), +Y up from the baseline.
	struct FontMetrics
	{
		float Ascent = 0.8f;
		float Descent = -0.2f; // Negative: below the baseline
		float LineHeight = 1.2f; // Baseline to baseline
	};

	struct GlyphInfo
	{
		uint32_t GlyphIndex = 0;
		float Advance = 0.0f;    // Pen advance in em units
		bool Visible = false;    // False for whitespace and glyphs that did not fit into the atlas
		glm::vec2 PlaneMin = glm::vec2(0.0f); // Quad corners relative to the pen on the baseline, em units, +Y up
		glm::vec2 PlaneMax = glm::vec2(0.0f);
		glm::uvec2 AtlasPosition = glm::uvec2(0); // Top-left texel of the glyph's rectangle in the atlas
		glm::uvec2 AtlasSize = glm::uvec2(0);
	};

	// Signed distance field glyph atlas of one font, filled on demand. Each glyph is rasterized once at a fixed size as
	// a distance field (crisp when scaled up or down) into a single-channel texture that grows as glyphs are added.
	// Characters the font lacks use its missing-glyph shape. Main thread only.
	class FontAtlas
	{
	public:
		static constexpr float c_GlyphEmSize = 48.0f; // Atlas texels per em
		static constexpr int c_DistancePadding = 6;   // Texels of distance field around each glyph shape
		static constexpr uint32_t c_AtlasWidth = 1024;
		static constexpr uint32_t c_MaxAtlasHeight = 4096;
		// Glyphs larger than this (texels, padding included) or costlier to rasterize than c_MaxGlyphRasterCost (texels
		// times outline points; real glyphs stay far below, a Latin letter costs about 100 thousand) are not drawn: a
		// malformed font cannot make the rasterizer allocate or compute without bound.
		static constexpr uint64_t c_MaxGlyphTexels = 256;
		static constexpr uint64_t c_MaxGlyphRasterCost = 8ull * 1024 * 1024;

		// Null (with an error) when the font data cannot be read.
		static Scope<FontAtlas> Create(const Ref<Font>& font, std::string* outError = nullptr);
		~FontAtlas();

		FontAtlas(const FontAtlas&) = delete;
		FontAtlas& operator=(const FontAtlas&) = delete;

		const Ref<Font>& GetFont() const { return m_Font; }
		const FontMetrics& GetMetrics() const { return m_Metrics; }

		// The glyph of a Unicode code point, rasterized on first use.
		const GlyphInfo& GetGlyph(uint32_t codepoint);
		// Extra advance between two glyphs (kerning), in em units.
		float GetKerning(const GlyphInfo& left, const GlyphInfo& right) const;

		// Creates or updates the GPU texture with the glyphs added since the last upload. False when the texture
		// cannot be created (the atlas then stays dirty).
		bool Upload(nvrhi::IDevice* device, nvrhi::ICommandList* commandList);
		nvrhi::ITexture* GetTexture() const { return m_Texture; }
		glm::uvec2 GetSize() const { return { c_AtlasWidth, m_Height }; }
		uint32_t GetGlyphCount() const { return static_cast<uint32_t>(m_Glyphs.size()); }
		const std::vector<uint8_t>& GetPixels() const { return m_Pixels; }
	private:
		FontAtlas() = default;
		GlyphInfo& AddGlyph(uint32_t glyphIndex);
		// Finds space for a rectangle (shelf packing), growing the atlas when needed. False when it is full.
		bool Allocate(uint32_t width, uint32_t height, glm::uvec2& outPosition);
	private:
		struct FontInfo;

		Ref<Font> m_Font;
		std::unique_ptr<FontInfo> m_Info; // stb_truetype state (kept out of the header)
		float m_Scale = 1.0f;             // Font units to atlas texels
		float m_EmScale = 1.0f;           // Font units to em
		FontMetrics m_Metrics;

		std::unordered_map<uint32_t, uint32_t> m_CodepointGlyphs; // Code point -> glyph index
		std::unordered_map<uint32_t, GlyphInfo> m_Glyphs;         // Glyph index -> glyph

		std::vector<uint8_t> m_Pixels; // c_AtlasWidth x m_Height, one byte per texel
		uint32_t m_Height = 0;
		uint32_t m_ShelfX = 0;
		uint32_t m_ShelfY = 0;
		uint32_t m_ShelfHeight = 0;
		bool m_Dirty = false;
		nvrhi::TextureHandle m_Texture;
	};

}
