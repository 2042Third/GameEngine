#include "stpch.h"
#include "Strata/Renderer/FontAtlas.h"

#include "Strata/Renderer/Font.h"

#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_InitialAtlasHeight = 256;
		constexpr uint32_t c_GlyphGutter = 1;            // Empty texels between glyphs, so filtering never bleeds
		constexpr unsigned char c_OnEdgeValue = 128;     // Distance field value on the glyph outline
		constexpr uint32_t c_MaxCodepoint = 0x10FFFF;

	}

	struct FontAtlas::FontInfo
	{
		stbtt_fontinfo Info = {};
	};

	FontAtlas::~FontAtlas() = default;

	Scope<FontAtlas> FontAtlas::Create(const Ref<Font>& font, std::string* outError)
	{
		auto fail = [&](const char* message)
		{
			if (outError)
				*outError = message;
			return Scope<FontAtlas>();
		};
		if (!font || font->GetData().empty())
			return fail("No font data");

		const std::vector<uint8_t>& data = font->GetData();
		const int offset = stbtt_GetFontOffsetForIndex(data.data(), 0);
		if (offset < 0)
			return fail("The font file holds no font");

		Scope<FontAtlas> atlas(new FontAtlas());
		atlas->m_Info = std::make_unique<FontInfo>();
		if (!stbtt_InitFont(&atlas->m_Info->Info, data.data(), offset))
			return fail("The font's tables cannot be read");
		atlas->m_Font = font;
		atlas->m_Scale = stbtt_ScaleForMappingEmToPixels(&atlas->m_Info->Info, c_GlyphEmSize);
		atlas->m_EmScale = stbtt_ScaleForMappingEmToPixels(&atlas->m_Info->Info, 1.0f);
		if (!(atlas->m_EmScale > 0.0f) || !std::isfinite(atlas->m_EmScale))
			return fail("The font has an invalid em size");

		int ascent = 0;
		int descent = 0;
		int lineGap = 0;
		stbtt_GetFontVMetrics(&atlas->m_Info->Info, &ascent, &descent, &lineGap);
		const float lineHeight = static_cast<float>(ascent - descent + lineGap) * atlas->m_EmScale;
		if (ascent > descent && lineHeight > 0.0f)
		{
			atlas->m_Metrics.Ascent = static_cast<float>(ascent) * atlas->m_EmScale;
			atlas->m_Metrics.Descent = static_cast<float>(descent) * atlas->m_EmScale;
			atlas->m_Metrics.LineHeight = lineHeight;
		}

		atlas->m_Height = c_InitialAtlasHeight;
		atlas->m_Pixels.assign(static_cast<size_t>(c_AtlasWidth) * atlas->m_Height, 0);
		return atlas;
	}

	const GlyphInfo& FontAtlas::GetGlyph(uint32_t codepoint)
	{
		uint32_t glyphIndex = 0;
		auto mapped = m_CodepointGlyphs.find(codepoint);
		if (mapped != m_CodepointGlyphs.end())
		{
			glyphIndex = mapped->second;
		}
		else
		{
			// Glyph 0 is the font's missing-glyph shape.
			if (codepoint <= c_MaxCodepoint)
				glyphIndex = static_cast<uint32_t>(std::max(stbtt_FindGlyphIndex(&m_Info->Info, static_cast<int>(codepoint)), 0));
			m_CodepointGlyphs.emplace(codepoint, glyphIndex);
		}

		auto glyph = m_Glyphs.find(glyphIndex);
		if (glyph != m_Glyphs.end())
			return glyph->second;
		return AddGlyph(glyphIndex);
	}

	GlyphInfo& FontAtlas::AddGlyph(uint32_t glyphIndex)
	{
		const stbtt_fontinfo* info = &m_Info->Info;
		GlyphInfo glyph;
		glyph.GlyphIndex = glyphIndex;
		int advance = 0;
		int leftSideBearing = 0;
		stbtt_GetGlyphHMetrics(info, static_cast<int>(glyphIndex), &advance, &leftSideBearing);
		glyph.Advance = static_cast<float>(advance) * m_EmScale;

		// The distance rises by c_OnEdgeValue / c_DistancePadding per texel toward the inside: 0 at the padding's edge.
		int width = 0;
		int height = 0;
		int offsetX = 0;
		int offsetY = 0;
		unsigned char* distances = stbtt_GetGlyphSDF(info, m_Scale, static_cast<int>(glyphIndex), c_DistancePadding, c_OnEdgeValue,
			static_cast<float>(c_OnEdgeValue) / static_cast<float>(c_DistancePadding), &width, &height, &offsetX, &offsetY);
		if (distances && width > 0 && height > 0)
		{
			glm::uvec2 position(0);
			if (Allocate(static_cast<uint32_t>(width) + c_GlyphGutter, static_cast<uint32_t>(height) + c_GlyphGutter, position))
			{
				for (int row = 0; row < height; row++)
				{
					std::memcpy(m_Pixels.data() + (static_cast<size_t>(position.y) + static_cast<size_t>(row)) * c_AtlasWidth + position.x,
						distances + static_cast<size_t>(row) * static_cast<size_t>(width), static_cast<size_t>(width));
				}
				glyph.Visible = true;
				glyph.AtlasPosition = position;
				glyph.AtlasSize = glm::uvec2(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
				// Bitmap offsets are in texels with +Y down from the pen.
				glyph.PlaneMin = glm::vec2(static_cast<float>(offsetX), -static_cast<float>(offsetY + height)) / c_GlyphEmSize;
				glyph.PlaneMax = glm::vec2(static_cast<float>(offsetX + width), -static_cast<float>(offsetY)) / c_GlyphEmSize;
				m_Dirty = true;
			}
			else
			{
				ST_CORE_WARN("Font atlas is full ({} glyphs); glyph {} is not drawn", m_Glyphs.size(), glyphIndex);
			}
		}
		if (distances)
			stbtt_FreeSDF(distances, nullptr);
		return m_Glyphs.emplace(glyphIndex, glyph).first->second;
	}

	float FontAtlas::GetKerning(const GlyphInfo& left, const GlyphInfo& right) const
	{
		return static_cast<float>(stbtt_GetGlyphKernAdvance(&m_Info->Info, static_cast<int>(left.GlyphIndex), static_cast<int>(right.GlyphIndex))) * m_EmScale;
	}

	bool FontAtlas::Allocate(uint32_t width, uint32_t height, glm::uvec2& outPosition)
	{
		if (width > c_AtlasWidth || height > c_MaxAtlasHeight)
			return false;
		if (m_ShelfX + width > c_AtlasWidth)
		{
			m_ShelfY += m_ShelfHeight;
			m_ShelfX = 0;
			m_ShelfHeight = 0;
		}
		if (m_ShelfY + height > m_Height)
		{
			// Growing adds rows below: existing glyphs keep their texel positions.
			uint32_t newHeight = m_Height;
			while (m_ShelfY + height > newHeight && newHeight < c_MaxAtlasHeight)
				newHeight *= 2;
			if (m_ShelfY + height > newHeight)
				return false;
			m_Pixels.resize(static_cast<size_t>(c_AtlasWidth) * newHeight, 0);
			m_Height = newHeight;
		}
		outPosition = glm::uvec2(m_ShelfX, m_ShelfY);
		m_ShelfX += width;
		m_ShelfHeight = std::max(m_ShelfHeight, height);
		return true;
	}

	bool FontAtlas::Upload(nvrhi::IDevice* device, nvrhi::ICommandList* commandList)
	{
		const bool resized = !m_Texture || m_Texture->getDesc().height != m_Height;
		if (!m_Dirty && !resized)
			return true;

		if (resized)
		{
			nvrhi::TextureDesc desc;
			desc.width = c_AtlasWidth;
			desc.height = m_Height;
			desc.format = nvrhi::Format::R8_UNORM;
			desc.debugName = "FontAtlas";
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			nvrhi::TextureHandle texture = device->createTexture(desc);
			if (!texture)
				return false;
			m_Texture = texture;
		}
		commandList->writeTexture(m_Texture, 0, 0, m_Pixels.data(), c_AtlasWidth);
		m_Dirty = false;
		return true;
	}

}
