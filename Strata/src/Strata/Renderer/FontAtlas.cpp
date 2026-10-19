#include "stpch.h"
#include "Strata/Renderer/FontAtlas.h"

#include "Strata/Core/Assert.h"
#include "Strata/Core/Log.h"
#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/TextLayout.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_GlyphGutter = 1; // Empty texels between glyphs, so filtering never bleeds
		constexpr size_t c_PageBytes = static_cast<size_t>(FontAtlas::c_PageSize) * FontAtlas::c_PageSize;
		std::atomic<uint64_t> s_NextAtlasId = 1;

	}

	FontAtlas::~FontAtlas() = default;

	Scope<FontAtlas> FontAtlas::Create(const Ref<Font>& font, std::string* outError, const FontAtlasSpecification& specification)
	{
		auto fail = [&](const std::string& message)
		{
			if (outError)
				*outError = message;
			return Scope<FontAtlas>();
		};
		if (!font || font->GetData().empty())
			return fail("No font data");
		if (specification.MaxPages == 0)
			return fail("A font atlas needs at least one page");

		std::string error;
		const FontFace* face = font->GetFace(&error);
		if (!face)
			return fail(error);

		Scope<FontAtlas> atlas(new FontAtlas());
		atlas->m_Id = s_NextAtlasId.fetch_add(1, std::memory_order_relaxed);
		atlas->m_Specification = specification;
		atlas->m_Font = font;
		atlas->m_Face = face;
		return atlas;
	}

	void FontAtlas::BeginFrame()
	{
		m_Frame++;
		// Entries of glyphs that are not in the atlas are cheap to rebuild from the face: keep them from growing with every
		// glyph ever asked for.
		if (m_Glyphs.size() > c_GlyphCacheLimit)
		{
			for (auto it = m_Glyphs.begin(); it != m_Glyphs.end();)
				it = it->second.Info.Visible ? std::next(it) : m_Glyphs.erase(it);
		}
	}

	const GlyphInfo& FontAtlas::GetGlyph(uint32_t codepoint)
	{
		return GetGlyphByIndex(m_Face->GetGlyphIndex(codepoint));
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
		// The face checked the glyph's size and cost against the rasterizer's limits (FontFace::GetGlyphMetrics).
		const FontGlyphMetrics& metrics = m_Face->GetGlyphMetrics(glyphIndex);
		GlyphEntry entry;
		entry.Info.GlyphIndex = glyphIndex;
		entry.Info.Advance = metrics.Advance;
		if (!metrics.Drawable)
			return entry; // Nothing to draw: whitespace, or too large or complex
		entry.Info.Pending = true;
		entry.Info.PlaneMin = metrics.PlaneMin;
		entry.Info.PlaneMax = metrics.PlaneMax;
		entry.Size = metrics.Size;
		entry.Cost = metrics.Cost;
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

		const uint32_t glyphIndex = entry.Info.GlyphIndex;
		const std::vector<uint8_t> distances = m_Face->RasterizeGlyph(glyphIndex);
		entry.Info.Pending = false;
		if (distances.size() != static_cast<size_t>(entry.Size.x) * entry.Size.y)
		{
			// Not drawn; the cells stay free.
			ST_CORE_WARN("Font: glyph {} could not be rasterized", glyphIndex);
			return;
		}

		Page& page = m_Pages[block.Page];
		const glm::uvec2 position = block.Cell * c_CellSize;
		for (uint32_t row = 0; row < entry.Size.y; row++)
		{
			std::memcpy(page.Pixels.data() + (static_cast<size_t>(position.y) + row) * c_PageSize + position.x,
				distances.data() + static_cast<size_t>(row) * entry.Size.x, entry.Size.x);
		}
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
		m_PlacementVersion++;
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
		m_PlacementVersion++;
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

	float FontAtlas::GetKerning(uint32_t leftGlyph, uint32_t rightGlyph)
	{
		if (!m_Font->HasUsableKerning())
			return 0.0f;
		// Glyph indices are below 65536 (the font's glyph count is 16-bit).
		const uint32_t pair = (leftGlyph << 16) | rightGlyph;
		auto cached = m_Kerning.find(pair);
		if (cached != m_Kerning.end())
			return cached->second;
		const float kerning = m_Face->ComputeKerning(leftGlyph, rightGlyph);
		if (m_Kerning.size() >= c_GlyphCacheLimit)
			m_Kerning.clear(); // Bounded: pairs are cheap to look up again
		m_Kerning.emplace(pair, kerning);
		return kerning;
	}

	void FontAtlas::Resolve(const ShapedText& text, TextLayout& outLayout)
	{
		outLayout.Quads.clear();
		outLayout.PendingGlyphs = 0;
		outLayout.Min = text.Min;
		outLayout.Max = text.Max;
		outLayout.LineCount = text.LineCount;
		for (const ShapedGlyph& shaped : text.Glyphs)
		{
			const GlyphInfo& glyph = GetGlyphByIndex(shaped.GlyphIndex);
			if (glyph.Visible)
			{
				TextGlyphQuad quad;
				quad.Min = shaped.Pen + glyph.PlaneMin;
				quad.Max = shaped.Pen + glyph.PlaneMax;
				quad.Page = glyph.Page;
				quad.AtlasPosition = glyph.AtlasPosition;
				quad.AtlasSize = glyph.AtlasSize;
				outLayout.Quads.push_back(quad);
			}
			else if (glyph.Pending)
			{
				outLayout.PendingGlyphs++;
			}
		}
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
