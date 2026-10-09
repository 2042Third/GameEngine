#include "stpch.h"
#include "Strata/Renderer/FontAtlas.h"

#include "Strata/Core/Assert.h"
#include "Strata/Core/Log.h"
#include "Strata/Renderer/Font.h"

#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_GlyphGutter = 1;            // Empty texels between glyphs, so filtering never bleeds
		constexpr unsigned char c_OnEdgeValue = 128;     // Distance field value on the glyph outline
		constexpr uint32_t c_MaxCodepoint = 0x10FFFF;
		constexpr size_t c_PageBytes = static_cast<size_t>(FontAtlas::c_PageSize) * FontAtlas::c_PageSize;

	}

	struct FontAtlas::FontInfo
	{
		stbtt_fontinfo Info = {};
	};

	FontAtlas::~FontAtlas() = default;

	Scope<FontAtlas> FontAtlas::Create(const Ref<Font>& font, std::string* outError, const FontAtlasSpecification& specification)
	{
		auto fail = [&](const char* message)
		{
			if (outError)
				*outError = message;
			return Scope<FontAtlas>();
		};
		if (!font || font->GetData().empty())
			return fail("No font data");
		if (specification.MaxPages == 0)
			return fail("A font atlas needs at least one page");

		// Font::Create validated everything stb_truetype reads below (see ValidateTrueTypeFont).
		const std::vector<uint8_t>& data = font->GetData();
		const int offset = static_cast<int>(font->GetFontOffset());

		Scope<FontAtlas> atlas(new FontAtlas());
		atlas->m_Specification = specification;
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
		return atlas;
	}

	void FontAtlas::BeginFrame()
	{
		m_Frame++;
		// Caches of glyphs that are not in the atlas are cheap to rebuild: keep them from growing with every glyph and code
		// point ever asked for.
		if (m_Glyphs.size() > c_GlyphCacheLimit)
		{
			for (auto it = m_Glyphs.begin(); it != m_Glyphs.end();)
				it = it->second.Info.Visible ? std::next(it) : m_Glyphs.erase(it);
		}
		if (m_CodepointGlyphs.size() > c_GlyphCacheLimit)
			m_CodepointGlyphs.clear();
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
			// Character maps may name glyphs the font does not have: those use the missing glyph too.
			if (codepoint <= c_MaxCodepoint)
			{
				const int found = stbtt_FindGlyphIndex(&m_Info->Info, static_cast<int>(codepoint));
				glyphIndex = found > 0 && static_cast<uint32_t>(found) < m_Font->GetGlyphCount() ? static_cast<uint32_t>(found) : 0u;
			}
			m_CodepointGlyphs.emplace(codepoint, glyphIndex);
		}
		return GetGlyphByIndex(glyphIndex);
	}

	const GlyphInfo& FontAtlas::GetGlyphByIndex(uint32_t glyphIndex)
	{
		if (glyphIndex >= m_Font->GetGlyphCount())
			glyphIndex = 0;
		auto it = m_Glyphs.find(glyphIndex);
		if (it == m_Glyphs.end())
			it = m_Glyphs.emplace(glyphIndex, DescribeGlyph(glyphIndex)).first;

		GlyphEntry& entry = it->second;
		if (entry.Info.Pending)
			Rasterize(entry);
		entry.LastUsed = m_Frame;
		return entry.Info;
	}

	FontAtlas::GlyphEntry FontAtlas::DescribeGlyph(uint32_t glyphIndex) const
	{
		const stbtt_fontinfo* info = &m_Info->Info;
		GlyphEntry entry;
		entry.Info.GlyphIndex = glyphIndex;
		int advance = 0;
		int leftSideBearing = 0;
		stbtt_GetGlyphHMetrics(info, static_cast<int>(glyphIndex), &advance, &leftSideBearing);
		entry.Info.Advance = static_cast<float>(advance) * m_EmScale;

		// The rasterizer allocates and visits every texel of the glyph's box (from the glyph's header, which a malformed font
		// can make huge) once per outline vertex: check both before rasterizing.
		const uint32_t points = m_Font->GetGlyphPointCount(glyphIndex);
		int boxX0 = 0;
		int boxY0 = 0;
		int boxX1 = 0;
		int boxY1 = 0;
		stbtt_GetGlyphBitmapBox(info, static_cast<int>(glyphIndex), m_Scale, m_Scale, &boxX0, &boxY0, &boxX1, &boxY1);
		if (points == 0 || boxX1 <= boxX0 || boxY1 <= boxY0)
			return entry; // Nothing to draw (whitespace)
		const uint64_t width = static_cast<uint64_t>(static_cast<int64_t>(boxX1) - boxX0) + 2 * c_DistancePadding;
		const uint64_t height = static_cast<uint64_t>(static_cast<int64_t>(boxY1) - boxY0) + 2 * c_DistancePadding;
		if (width > c_MaxGlyphTexels || height > c_MaxGlyphTexels || width * height * points > c_MaxGlyphRasterCost)
		{
			ST_CORE_WARN("Font: glyph {} is too large or complex to draw ({}x{} texels, {} points)", glyphIndex, width, height, points);
			return entry;
		}

		// The distance field covers the box plus the padding; its offsets are in texels with +Y down from the pen.
		const float left = static_cast<float>(boxX0 - c_DistancePadding);
		const float top = static_cast<float>(boxY0 - c_DistancePadding);
		entry.Info.Pending = true;
		entry.Info.PlaneMin = glm::vec2(left, -(top + static_cast<float>(height))) / c_GlyphEmSize;
		entry.Info.PlaneMax = glm::vec2(left + static_cast<float>(width), -top) / c_GlyphEmSize;
		entry.Size = glm::uvec2(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
		entry.Cost = width * height * points;
		return entry;
	}

	void FontAtlas::Rasterize(GlyphEntry& entry)
	{
		if (m_Budget.Glyphs == 0 || m_Budget.Cost == 0)
			return;
		const glm::uvec2 cells = (entry.Size + glm::uvec2(c_GlyphGutter + c_CellSize - 1)) / c_CellSize;
		CellBlock block;
		if (!Allocate(cells, block))
			return;
		m_Budget.Glyphs--;
		m_Budget.Cost = m_Budget.Cost > entry.Cost ? m_Budget.Cost - entry.Cost : 0;

		// The distance rises by c_OnEdgeValue / c_DistancePadding per texel toward the inside: 0 at the padding's edge.
		const uint32_t glyphIndex = entry.Info.GlyphIndex;
		int width = 0;
		int height = 0;
		int offsetX = 0;
		int offsetY = 0;
		unsigned char* distances = stbtt_GetGlyphSDF(&m_Info->Info, m_Scale, static_cast<int>(glyphIndex), c_DistancePadding, c_OnEdgeValue,
			static_cast<float>(c_OnEdgeValue) / static_cast<float>(c_DistancePadding), &width, &height, &offsetX, &offsetY);
		entry.Info.Pending = false;
		if (!distances || width != static_cast<int>(entry.Size.x) || height != static_cast<int>(entry.Size.y))
		{
			// Not drawn; the cells stay free.
			ST_CORE_WARN("Font: glyph {} could not be rasterized", glyphIndex);
			if (distances)
				stbtt_FreeSDF(distances, nullptr);
			return;
		}

		Page& page = m_Pages[block.Page];
		const glm::uvec2 position = block.Cell * c_CellSize;
		for (uint32_t row = 0; row < entry.Size.y; row++)
		{
			std::memcpy(page.Pixels.data() + (static_cast<size_t>(position.y) + row) * c_PageSize + position.x,
				distances + static_cast<size_t>(row) * entry.Size.x, entry.Size.x);
		}
		stbtt_FreeSDF(distances, nullptr);
		for (uint32_t y = block.Cell.y; y < block.Cell.y + block.Cells.y; y++)
		{
			for (uint32_t x = block.Cell.x; x < block.Cell.x + block.Cells.x; x++)
				page.Cells[y * c_CellsPerRow + x] = glyphIndex;
		}
		page.FreeCells -= block.Cells.x * block.Cells.y;
		MarkDirty(page, position.y, entry.Size.y);

		entry.Info.Visible = true;
		entry.Info.Page = block.Page;
		entry.Info.AtlasPosition = position;
		entry.Info.AtlasSize = entry.Size;
		m_Stats.RasterizedGlyphs++;
	}

	bool FontAtlas::Allocate(const glm::uvec2& cells, CellBlock& outBlock)
	{
		if (cells.x > c_CellsPerRow || cells.y > c_CellsPerRow)
			return false; // Larger than a page (glyph sizes are limited well below)
		outBlock.Cells = cells;
		const uint32_t blockCells = cells.x * cells.y;
		auto isFree = [&](const Page& page, uint32_t cellX, uint32_t cellY)
		{
			for (uint32_t y = cellY; y < cellY + cells.y; y++)
			{
				for (uint32_t x = cellX; x < cellX + cells.x; x++)
				{
					if (page.Cells[y * c_CellsPerRow + x] != c_FreeCell)
						return false;
				}
			}
			return true;
		};

		// Free cells first.
		for (uint32_t pageIndex = 0; pageIndex < m_Pages.size(); pageIndex++)
		{
			const Page& page = m_Pages[pageIndex];
			if (page.FreeCells < blockCells)
				continue;
			for (uint32_t y = 0; y + cells.y <= c_CellsPerRow; y++)
			{
				for (uint32_t x = 0; x + cells.x <= c_CellsPerRow; x++)
				{
					if (isFree(page, x, y))
					{
						outBlock.Page = pageIndex;
						outBlock.Cell = glm::uvec2(x, y);
						return true;
					}
				}
			}
		}

		// Then a new page.
		if (m_Pages.size() < m_Specification.MaxPages)
		{
			Page& page = m_Pages.emplace_back();
			page.Pixels.assign(c_PageBytes, 0);
			page.Cells.assign(static_cast<size_t>(c_CellsPerRow) * c_CellsPerRow, c_FreeCell);
			page.FreeCells = c_CellsPerRow * c_CellsPerRow;
			outBlock.Page = static_cast<uint32_t>(m_Pages.size() - 1);
			outBlock.Cell = glm::uvec2(0);
			return true;
		}

		// Then the block whose glyphs were used longest ago; glyphs of this frame stay (their quads are laid out already).
		// A block's age is that of its most recently used glyph.
		bool found = false;
		uint64_t bestAge = 0;
		for (uint32_t pageIndex = 0; pageIndex < m_Pages.size(); pageIndex++)
		{
			const Page& page = m_Pages[pageIndex];
			for (uint32_t cellY = 0; cellY + cells.y <= c_CellsPerRow; cellY++)
			{
				for (uint32_t cellX = 0; cellX + cells.x <= c_CellsPerRow; cellX++)
				{
					uint64_t age = 0;
					bool evictable = true;
					for (uint32_t y = cellY; y < cellY + cells.y && evictable; y++)
					{
						for (uint32_t x = cellX; x < cellX + cells.x && evictable; x++)
						{
							const uint32_t glyph = page.Cells[y * c_CellsPerRow + x];
							if (glyph == c_FreeCell)
								continue;
							auto entry = m_Glyphs.find(glyph);
							ST_CORE_ASSERT(entry != m_Glyphs.end(), "Font atlas cell holds unknown glyph {}", glyph);
							evictable = entry->second.LastUsed < m_Frame;
							age = std::max(age, entry->second.LastUsed + 1);
						}
					}
					if (evictable && (!found || age < bestAge))
					{
						found = true;
						bestAge = age;
						outBlock.Page = pageIndex;
						outBlock.Cell = glm::uvec2(cellX, cellY);
					}
				}
			}
		}
		if (!found)
		{
			if (!m_ReportedFull)
				ST_CORE_WARN("Font atlas is full: a frame uses more glyphs than its {} pages hold; the rest are not drawn", m_Pages.size());
			m_ReportedFull = true;
			return false;
		}

		const Page& page = m_Pages[outBlock.Page];
		for (uint32_t y = outBlock.Cell.y; y < outBlock.Cell.y + cells.y; y++)
		{
			for (uint32_t x = outBlock.Cell.x; x < outBlock.Cell.x + cells.x; x++)
			{
				const uint32_t glyph = page.Cells[y * c_CellsPerRow + x];
				if (glyph != c_FreeCell)
					Evict(glyph);
			}
		}
		return true;
	}

	void FontAtlas::Evict(uint32_t glyphIndex)
	{
		auto it = m_Glyphs.find(glyphIndex);
		ST_CORE_ASSERT(it != m_Glyphs.end() && it->second.Info.Visible, "Font atlas evicts glyph {}, which it does not hold", glyphIndex);
		const GlyphEntry& entry = it->second;
		Page& page = m_Pages[entry.Info.Page];
		const glm::uvec2 firstCell = entry.Info.AtlasPosition / c_CellSize;
		const glm::uvec2 cells = (entry.Size + glm::uvec2(c_GlyphGutter + c_CellSize - 1)) / c_CellSize;
		for (uint32_t y = firstCell.y; y < firstCell.y + cells.y; y++)
		{
			for (uint32_t x = firstCell.x; x < firstCell.x + cells.x; x++)
				page.Cells[y * c_CellsPerRow + x] = c_FreeCell;
		}
		page.FreeCells += cells.x * cells.y;

		// Cleared texels keep the gutters of later glyphs in these cells empty.
		for (uint32_t row = 0; row < entry.Size.y; row++)
			std::memset(page.Pixels.data() + (static_cast<size_t>(entry.Info.AtlasPosition.y) + row) * c_PageSize + entry.Info.AtlasPosition.x, 0, entry.Size.x);
		MarkDirty(page, entry.Info.AtlasPosition.y, entry.Size.y);
		m_Glyphs.erase(it);
		m_Stats.EvictedGlyphs++;
	}

	void FontAtlas::MarkDirty(Page& page, uint32_t firstRow, uint32_t rowCount)
	{
		if (page.DirtyEnd > page.DirtyBegin)
		{
			page.DirtyBegin = std::min(page.DirtyBegin, firstRow);
			page.DirtyEnd = std::max(page.DirtyEnd, firstRow + rowCount);
		}
		else
		{
			page.DirtyBegin = firstRow;
			page.DirtyEnd = firstRow + rowCount;
		}
	}

	float FontAtlas::GetKerning(const GlyphInfo& left, const GlyphInfo& right) const
	{
		if (!m_Font->HasUsableKerning())
			return 0.0f;
		return static_cast<float>(stbtt_GetGlyphKernAdvance(&m_Info->Info, static_cast<int>(left.GlyphIndex), static_cast<int>(right.GlyphIndex))) * m_EmScale;
	}

	const std::vector<uint8_t>& FontAtlas::GetPagePixels(uint32_t page) const
	{
		ST_CORE_ASSERT(page < m_Pages.size(), "Font atlas page {} does not exist", page);
		return m_Pages[page].Pixels;
	}

	bool FontAtlas::Upload(nvrhi::IDevice* device, nvrhi::ICommandList* commandList)
	{
		const uint32_t pageCount = std::max(GetPageCount(), 1u);
		if (!m_Texture || m_Texture->getDesc().arraySize < pageCount)
		{
			// Capacity grows in powers of two (up to the page limit); a new texture receives every page.
			uint32_t layers = 1;
			while (layers < pageCount)
				layers *= 2;
			nvrhi::TextureDesc desc;
			desc.dimension = nvrhi::TextureDimension::Texture2DArray;
			desc.width = c_PageSize;
			desc.height = c_PageSize;
			desc.arraySize = std::min(layers, m_Specification.MaxPages); // At least pageCount: pages never exceed the limit
			desc.format = nvrhi::Format::R8_UNORM;
			desc.debugName = "FontAtlas";
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			nvrhi::TextureHandle texture = device->createTexture(desc);
			if (!texture)
				return false;
			m_Texture = texture;
			for (Page& page : m_Pages)
				page.FullyDirty = true;
		}

		for (uint32_t index = 0; index < m_Pages.size(); index++)
		{
			Page& page = m_Pages[index];
			if (page.FullyDirty)
			{
				commandList->writeTexture(m_Texture, index, 0, page.Pixels.data(), c_PageSize);
				m_Stats.UploadedBytes += c_PageBytes;
			}
			else if (page.DirtyEnd > page.DirtyBegin)
			{
				// NVRHI writes whole subresources: changed rows go through a band texture, then into the page.
				if (!m_UploadBand)
				{
					nvrhi::TextureDesc desc;
					desc.width = c_PageSize;
					desc.height = c_UploadBandRows;
					desc.format = nvrhi::Format::R8_UNORM;
					desc.debugName = "FontAtlasUploadBand";
					desc.initialState = nvrhi::ResourceStates::CopySource;
					desc.keepInitialState = true;
					m_UploadBand = device->createTexture(desc);
					if (!m_UploadBand)
						return false;
				}
				for (uint32_t row = page.DirtyBegin; row < page.DirtyEnd; row += c_UploadBandRows)
				{
					const uint32_t top = std::min(row, c_PageSize - c_UploadBandRows);
					commandList->writeTexture(m_UploadBand, 0, 0, page.Pixels.data() + static_cast<size_t>(top) * c_PageSize, c_PageSize);
					commandList->copyTexture(m_Texture, nvrhi::TextureSlice().setOrigin(0, top, 0).setSize(c_PageSize, c_UploadBandRows, 1).setArraySlice(index),
						m_UploadBand, nvrhi::TextureSlice().setSize(c_PageSize, c_UploadBandRows, 1));
					m_Stats.UploadedBytes += static_cast<uint64_t>(c_PageSize) * c_UploadBandRows;
				}
			}
			page.FullyDirty = false;
			page.DirtyBegin = 0;
			page.DirtyEnd = 0;
		}
		return true;
	}

}
