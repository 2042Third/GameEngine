#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Renderer/FontValidation.h"

#include <glm/glm.hpp>

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

	// What a glyph measures and what its distance field takes (FontAtlas), known without rasterizing it.
	struct FontGlyphMetrics
	{
		float Advance = 0.0f; // Pen advance in em units
		// Drawable: it has a shape that fits the rasterizer's limits (whitespace and glyphs too large or costly have none).
		bool Drawable = false;
		glm::vec2 PlaneMin = glm::vec2(0.0f); // Corners of its distance field's quad relative to the pen on the baseline, em, +Y up
		glm::vec2 PlaneMax = glm::vec2(0.0f);
		glm::uvec2 Size = glm::uvec2(0); // Distance field texels (padding included)
		float Scale = 0.0f;              // Font units to distance field texels (reduced for costly glyphs)
		uint64_t Cost = 0;               // Rasterization cost (FontAtlas::GetRasterCost)
	};

	// The glyph metrics of a font, for measuring and laying out text without rasterizing anything: code points to glyphs,
	// advances, kerning, vertical metrics and the plane bounds of each glyph's distance field (the quad FontAtlas draws it
	// with). One per font, made on first use by Font::GetFace and shared by every atlas and text layout of the font; it reads
	// only what Font::Create validated (FontValidation.h). Lookups are cached: glyph metrics for every glyph asked for (at
	// most the font's glyphs), code points and kerning pairs up to c_CacheLimit each. Main thread only.
	class FontFace
	{
	public:
		static constexpr float c_GlyphEmSize = 32.0f; // Distance field texels per em at full resolution
		static constexpr int c_DistancePadding = 4;   // Texels of distance field around each glyph shape
		// Glyphs larger than this (padding included) or costlier than c_MaxGlyphRasterCost are rasterized at half or a quarter of
		// the resolution, and beyond that not drawn (see FontAtlas).
		static constexpr uint64_t c_MaxGlyphTexels = 192;
		static constexpr uint64_t c_MaxGlyphRasterCost = 2 * 1024 * 1024;
		static constexpr uint32_t c_MaxResolutionReduction = 4;
		static constexpr uint64_t c_CurveCostWeight = 2; // Extra cost of a curve vertex over a line vertex
		// The code point and kerning caches are cleared when they reach this many entries.
		static constexpr size_t c_CacheLimit = 4096;

		// The cost of rasterizing a glyph of `shape` into a distance field of `size` texels: about proportional to time, about
		// 4 ns per unit in an optimized build on a desktop CPU, up to twice that for curve-heavy outlines such as emoji.
		static uint64_t GetRasterCost(const glm::uvec2& size, const GlyphShapeCost& shape);

		// Null (with an error) when the font's tables cannot be read. Font::GetFace makes the face of a font.
		static Scope<FontFace> Create(const Font& font, std::string* outError = nullptr);
		~FontFace();

		FontFace(const FontFace&) = delete;
		FontFace& operator=(const FontFace&) = delete;

		const FontMetrics& GetMetrics() const { return m_Metrics; }
		uint32_t GetGlyphCount() const { return m_GlyphCount; }

		// The glyph of a Unicode code point; 0 (the font's missing-glyph shape) for code points it lacks.
		uint32_t GetGlyphIndex(uint32_t codepoint) const;
		// Whether the font has a glyph of its own for the code point.
		bool HasGlyph(uint32_t codepoint) const { return GetGlyphIndex(codepoint) != 0; }
		// A glyph's metrics (out of range: the missing glyph's). Computed on first use; a glyph too large or complex to draw
		// is reported once.
		const FontGlyphMetrics& GetGlyphMetrics(uint32_t glyphIndex) const;
		// Extra advance between two glyphs, in em units; 0 when the font's kerning is unusable (Font::HasUsableKerning).
		// GetKerning caches pairs, ComputeKerning searches the font's tables every time.
		float GetKerning(uint32_t leftGlyph, uint32_t rightGlyph) const;
		float ComputeKerning(uint32_t leftGlyph, uint32_t rightGlyph) const;

		// The distance field of a glyph as GetGlyphMetrics sizes it: Size texels, rows top to bottom, 128 on the outline
		// rising by 128 / c_DistancePadding per texel inside. Empty when it cannot be rasterized.
		std::vector<uint8_t> RasterizeGlyph(uint32_t glyphIndex) const;

		size_t GetCachedCodepointCount() const { return m_CodepointGlyphs.size(); }
		size_t GetCachedKerningCount() const { return m_Kerning.size(); }
	private:
		struct NativeInfo;

		FontFace() = default;
		FontGlyphMetrics DescribeGlyph(uint32_t glyphIndex) const;
	private:
		const Font* m_Font = nullptr; // The font owns the face
		std::unique_ptr<NativeInfo> m_Info; // stb_truetype state (kept out of the header)
		float m_Scale = 1.0f;   // Font units to texels at full resolution
		float m_EmScale = 1.0f; // Font units to em
		uint32_t m_GlyphCount = 0;
		FontMetrics m_Metrics;
		bool m_KerningUsable = false;

		mutable std::unordered_map<uint32_t, uint32_t> m_CodepointGlyphs; // Code point -> glyph index
		mutable std::unordered_map<uint32_t, float> m_Kerning;            // Glyph pair (left << 16 | right) -> kerning
		mutable std::unordered_map<uint32_t, FontGlyphMetrics> m_Glyphs;  // Glyph index -> metrics (node-based: references stay valid)
	};

}
