#include "stpch.h"
#include "Strata/Renderer/FontFace.h"

#include "Strata/Core/Log.h"
#include "Strata/Renderer/Font.h"

#include <stb_truetype.h>

#include <cmath>
#include <cstring>

namespace Strata
{

	namespace
	{

		constexpr unsigned char c_OnEdgeValue = 128; // Distance field value on the glyph outline
		constexpr uint32_t c_MaxCodepoint = 0x10FFFF;

	}

	struct FontFace::NativeInfo
	{
		stbtt_fontinfo Info = {};
	};

	FontFace::~FontFace() = default;

	uint64_t FontFace::GetRasterCost(const glm::uvec2& size, const GlyphShapeCost& shape)
	{
		// Every texel measures its distance to every vertex (curves cost more), after composite glyphs were assembled.
		const uint64_t vertexWork = static_cast<uint64_t>(shape.Vertices) + c_CurveCostWeight * static_cast<uint64_t>(shape.Curves);
		return static_cast<uint64_t>(size.x) * size.y * vertexWork + shape.CompositeCopies;
	}

	Scope<FontFace> FontFace::Create(const Font& font, std::string* outError)
	{
		auto fail = [&](const char* message)
		{
			if (outError)
				*outError = message;
			return Scope<FontFace>();
		};
		const std::vector<uint8_t>& data = font.GetData();
		if (data.empty())
			return fail("No font data");

		// Font::Create validated everything stb_truetype reads (see ValidateTrueTypeFont).
		Scope<FontFace> face(new FontFace());
		face->m_Font = &font;
		face->m_Info = std::make_unique<NativeInfo>();
		if (!stbtt_InitFont(&face->m_Info->Info, data.data(), static_cast<int>(font.GetFontOffset())))
			return fail("The font's tables cannot be read");
		face->m_Scale = stbtt_ScaleForMappingEmToPixels(&face->m_Info->Info, c_GlyphEmSize);
		face->m_EmScale = stbtt_ScaleForMappingEmToPixels(&face->m_Info->Info, 1.0f);
		if (!(face->m_EmScale > 0.0f) || !std::isfinite(face->m_EmScale))
			return fail("The font has an invalid em size");
		face->m_GlyphCount = font.GetGlyphCount();
		face->m_KerningUsable = font.HasUsableKerning();

		int ascent = 0;
		int descent = 0;
		int lineGap = 0;
		stbtt_GetFontVMetrics(&face->m_Info->Info, &ascent, &descent, &lineGap);
		const float lineHeight = static_cast<float>(ascent - descent + lineGap) * face->m_EmScale;
		if (ascent > descent && lineHeight > 0.0f)
		{
			face->m_Metrics.Ascent = static_cast<float>(ascent) * face->m_EmScale;
			face->m_Metrics.Descent = static_cast<float>(descent) * face->m_EmScale;
			face->m_Metrics.LineHeight = lineHeight;
		}
		return face;
	}

	uint32_t FontFace::GetGlyphIndex(uint32_t codepoint) const
	{
		auto mapped = m_CodepointGlyphs.find(codepoint);
		if (mapped != m_CodepointGlyphs.end())
			return mapped->second;

		// Glyph 0 is the font's missing-glyph shape. Character maps may name glyphs the font does not have: those use the
		// missing glyph too.
		uint32_t glyphIndex = 0;
		if (codepoint <= c_MaxCodepoint)
		{
			const int found = stbtt_FindGlyphIndex(&m_Info->Info, static_cast<int>(codepoint));
			glyphIndex = found > 0 && static_cast<uint32_t>(found) < m_GlyphCount ? static_cast<uint32_t>(found) : 0u;
		}
		if (m_CodepointGlyphs.size() >= c_CacheLimit)
			m_CodepointGlyphs.clear(); // Bounded: code points are cheap to look up again
		m_CodepointGlyphs.emplace(codepoint, glyphIndex);
		return glyphIndex;
	}

	const FontGlyphMetrics& FontFace::GetGlyphMetrics(uint32_t glyphIndex) const
	{
		if (glyphIndex >= m_GlyphCount)
			glyphIndex = 0;
		auto it = m_Glyphs.find(glyphIndex);
		if (it == m_Glyphs.end())
			it = m_Glyphs.emplace(glyphIndex, DescribeGlyph(glyphIndex)).first;
		return it->second;
	}

	FontGlyphMetrics FontFace::DescribeGlyph(uint32_t glyphIndex) const
	{
		const stbtt_fontinfo* info = &m_Info->Info;
		FontGlyphMetrics metrics;
		int advance = 0;
		int leftSideBearing = 0;
		stbtt_GetGlyphHMetrics(info, static_cast<int>(glyphIndex), &advance, &leftSideBearing);
		metrics.Advance = static_cast<float>(advance) * m_EmScale;

		// The rasterizer allocates and visits every texel of the glyph's box (from the glyph's header, which a malformed font
		// can make huge) for every outline vertex: check both before rasterizing. Glyphs too large or costly at the full
		// resolution are rasterized at half or a quarter of it (as sharp at small sizes, softer when shown large); beyond that
		// they are not drawn.
		const GlyphShapeCost& shape = m_Font->GetGlyphShape(glyphIndex);
		if (shape.Vertices == 0)
			return metrics; // Nothing to draw (whitespace)
		uint64_t width = 0;
		uint64_t height = 0;
		for (uint32_t reduction = 1; reduction <= c_MaxResolutionReduction; reduction *= 2)
		{
			const float scale = m_Scale / static_cast<float>(reduction);
			int boxX0 = 0;
			int boxY0 = 0;
			int boxX1 = 0;
			int boxY1 = 0;
			stbtt_GetGlyphBitmapBox(info, static_cast<int>(glyphIndex), scale, scale, &boxX0, &boxY0, &boxX1, &boxY1);
			if (boxX1 <= boxX0 || boxY1 <= boxY0)
				return metrics; // An empty box: nothing to draw
			width = static_cast<uint64_t>(static_cast<int64_t>(boxX1) - boxX0) + 2 * c_DistancePadding;
			height = static_cast<uint64_t>(static_cast<int64_t>(boxY1) - boxY0) + 2 * c_DistancePadding;
			if (width > c_MaxGlyphTexels || height > c_MaxGlyphTexels)
				continue;
			const uint64_t cost = GetRasterCost(glm::uvec2(static_cast<uint32_t>(width), static_cast<uint32_t>(height)), shape);
			if (cost > c_MaxGlyphRasterCost)
				continue;

			// The distance field covers the box plus the padding; its offsets are in texels with +Y down from the pen.
			const float texelsPerEm = c_GlyphEmSize / static_cast<float>(reduction);
			const float left = static_cast<float>(boxX0 - c_DistancePadding);
			const float top = static_cast<float>(boxY0 - c_DistancePadding);
			metrics.Drawable = true;
			metrics.PlaneMin = glm::vec2(left, -(top + static_cast<float>(height))) / texelsPerEm;
			metrics.PlaneMax = glm::vec2(left + static_cast<float>(width), -top) / texelsPerEm;
			metrics.Size = glm::uvec2(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
			metrics.Scale = scale;
			metrics.Cost = cost;
			return metrics;
		}
		// Described once per face (the metrics are cached), so reported once.
		ST_CORE_WARN("Font: glyph {} is too large or complex to draw ({} vertices, {} composite copies, {}x{} texels at a quarter of the resolution)",
			glyphIndex, shape.Vertices, shape.CompositeCopies, width, height);
		return metrics;
	}

	float FontFace::GetKerning(uint32_t leftGlyph, uint32_t rightGlyph) const
	{
		if (!m_KerningUsable)
			return 0.0f;
		// Glyph indices are below 65536 (the font's glyph count is 16-bit).
		const uint32_t pair = (leftGlyph << 16) | rightGlyph;
		auto cached = m_Kerning.find(pair);
		if (cached != m_Kerning.end())
			return cached->second;
		const float kerning = ComputeKerning(leftGlyph, rightGlyph);
		if (m_Kerning.size() >= c_CacheLimit)
			m_Kerning.clear(); // Bounded: pairs are cheap to look up again
		m_Kerning.emplace(pair, kerning);
		return kerning;
	}

	float FontFace::ComputeKerning(uint32_t leftGlyph, uint32_t rightGlyph) const
	{
		if (!m_KerningUsable)
			return 0.0f;
		return static_cast<float>(stbtt_GetGlyphKernAdvance(&m_Info->Info, static_cast<int>(leftGlyph), static_cast<int>(rightGlyph))) * m_EmScale;
	}

	std::vector<uint8_t> FontFace::RasterizeGlyph(uint32_t glyphIndex) const
	{
		const FontGlyphMetrics& metrics = GetGlyphMetrics(glyphIndex);
		if (!metrics.Drawable)
			return {};
		// The distance rises by c_OnEdgeValue / c_DistancePadding per texel toward the inside: 0 at the padding's edge.
		int width = 0;
		int height = 0;
		int offsetX = 0;
		int offsetY = 0;
		unsigned char* distances = stbtt_GetGlyphSDF(&m_Info->Info, metrics.Scale, static_cast<int>(glyphIndex >= m_GlyphCount ? 0 : glyphIndex),
			c_DistancePadding, c_OnEdgeValue, static_cast<float>(c_OnEdgeValue) / static_cast<float>(c_DistancePadding), &width, &height, &offsetX, &offsetY);
		std::vector<uint8_t> pixels;
		if (distances && width == static_cast<int>(metrics.Size.x) && height == static_cast<int>(metrics.Size.y))
		{
			pixels.resize(static_cast<size_t>(metrics.Size.x) * metrics.Size.y);
			std::memcpy(pixels.data(), distances, pixels.size());
		}
		if (distances)
			stbtt_FreeSDF(distances, nullptr);
		return pixels;
	}

}
