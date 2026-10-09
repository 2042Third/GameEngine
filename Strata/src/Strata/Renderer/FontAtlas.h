#pragma once

#include "Strata/Core/Base.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <limits>
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
		bool Visible = false;    // In the atlas, ready to draw
		// Has a shape but is not in the atlas yet: it waits for the rasterization budget of a later frame, or for atlas
		// space. Glyphs that are neither visible nor pending have nothing to draw (whitespace, or too large to draw).
		bool Pending = false;
		glm::vec2 PlaneMin = glm::vec2(0.0f); // Quad corners relative to the pen on the baseline, em units, +Y up
		glm::vec2 PlaneMax = glm::vec2(0.0f);
		uint32_t Page = 0;                        // Atlas page (texture array layer) of a visible glyph
		glm::uvec2 AtlasPosition = glm::uvec2(0); // Top-left texel of the glyph's rectangle in its page
		glm::uvec2 AtlasSize = glm::uvec2(0);
	};

	// Glyph rasterization a frame may still do (see FontAtlas::SetRasterBudget). A glyph is rasterized while both counts
	// are above zero; its cost may overdraw the remaining cost.
	struct GlyphRasterBudget
	{
		uint32_t Glyphs = std::numeric_limits<uint32_t>::max();
		uint64_t Cost = std::numeric_limits<uint64_t>::max(); // Texels times outline points: the rasterization time is about proportional
	};

	struct FontAtlasSpecification
	{
		uint32_t MaxPages = 8; // Of FontAtlas::c_PageSize squared texels (256 KB each, on the CPU and the GPU)
	};

	struct FontAtlasStats
	{
		uint64_t RasterizedGlyphs = 0; // Totals since the atlas was created
		uint64_t EvictedGlyphs = 0;
		uint64_t UploadedBytes = 0;
	};

	// Signed distance field glyph atlas of one font, filled on demand. Each glyph is rasterized once at a fixed size as a
	// distance field (crisp when scaled up or down) into a page of a single-channel texture array. Characters the font
	// lacks use its missing-glyph shape. Main thread only.
	//
	// Pages are grids of square cells; a glyph takes one cell, or a block of cells when it is larger. Memory stays bounded:
	// when no cells are free and no page can be added, the least recently used glyphs that were not used in the current
	// frame (see BeginFrame) are evicted and rasterized again when next needed. Rasterization can be limited per frame
	// (SetRasterBudget); glyphs over the budget stay pending and are drawn on a later frame. Only the rows of a page that
	// changed are uploaded to the GPU.
	class FontAtlas
	{
	public:
		static constexpr float c_GlyphEmSize = 32.0f; // Atlas texels per em
		static constexpr int c_DistancePadding = 4;   // Texels of distance field around each glyph shape
		static constexpr uint32_t c_PageSize = 512;
		// Cells hold glyphs up to one texel smaller (the empty gutter keeps filtering from bleeding between glyphs): shapes
		// of about one em, such as Latin letters and CJK ideographs, take one cell.
		static constexpr uint32_t c_CellSize = 42;
		static constexpr uint32_t c_CellsPerRow = c_PageSize / c_CellSize;
		static constexpr uint32_t c_UploadBandRows = 64; // Changed rows are uploaded in bands of this height
		// Glyphs larger than this (texels, padding included) or costlier to rasterize than c_MaxGlyphRasterCost (texels
		// times outline points; real glyphs stay far below, a Latin letter costs about 20 thousand) are not drawn: a
		// malformed font cannot make the rasterizer allocate or compute without bound.
		static constexpr uint64_t c_MaxGlyphTexels = 192;
		static constexpr uint64_t c_MaxGlyphRasterCost = 8ull * 1024 * 1024;
		// BeginFrame drops cached glyphs that are not in the atlas, and the code point cache, beyond this many entries; the
		// kerning cache is cleared when it reaches it.
		static constexpr size_t c_GlyphCacheLimit = 4096;

		// Null (with an error) when the font data cannot be read.
		static Scope<FontAtlas> Create(const Ref<Font>& font, std::string* outError = nullptr, const FontAtlasSpecification& specification = {});
		~FontAtlas();

		FontAtlas(const FontAtlas&) = delete;
		FontAtlas& operator=(const FontAtlas&) = delete;

		const Ref<Font>& GetFont() const { return m_Font; }
		const FontMetrics& GetMetrics() const { return m_Metrics; }

		// Starts a frame: glyphs returned from now on are not evicted until the next BeginFrame, and the references to them
		// stay valid until then.
		void BeginFrame();
		// The rasterization this frame may still do; GetRasterBudget returns what is left. Unlimited by default.
		void SetRasterBudget(const GlyphRasterBudget& budget) { m_Budget = budget; }
		const GlyphRasterBudget& GetRasterBudget() const { return m_Budget; }

		// The glyph of a Unicode code point, rasterized on first use (budget and atlas space permitting).
		const GlyphInfo& GetGlyph(uint32_t codepoint);
		// A glyph by its index in the font (out of range: the missing glyph).
		const GlyphInfo& GetGlyphByIndex(uint32_t glyphIndex);
		// Extra advance between two glyphs (kerning), in em units. Cached per glyph pair: stb_truetype searches the font's
		// kerning tables on every call.
		float GetKerning(const GlyphInfo& left, const GlyphInfo& right);

		// Creates the GPU texture array or uploads what changed since the last upload. False when a texture cannot be
		// created (the changes then stay pending).
		bool Upload(nvrhi::IDevice* device, nvrhi::ICommandList* commandList);
		nvrhi::ITexture* GetTexture() const { return m_Texture; }

		uint32_t GetPageCount() const { return static_cast<uint32_t>(m_Pages.size()); }
		const std::vector<uint8_t>& GetPagePixels(uint32_t page) const; // c_PageSize squared texels, rows top to bottom
		size_t GetCachedGlyphCount() const { return m_Glyphs.size(); }
		size_t GetCachedKerningCount() const { return m_Kerning.size(); }
		const FontAtlasStats& GetStats() const { return m_Stats; }
	private:
		static constexpr uint32_t c_FreeCell = std::numeric_limits<uint32_t>::max();

		struct GlyphEntry
		{
			GlyphInfo Info;
			glm::uvec2 Size = glm::uvec2(0); // Distance field texels of a glyph with a shape
			uint64_t Cost = 0;
			uint64_t LastUsed = 0;           // Frame
		};

		struct Page
		{
			std::vector<uint8_t> Pixels;
			std::vector<uint32_t> Cells; // Glyph index per cell, row by row (c_FreeCell when free)
			uint32_t FreeCells = 0;
			uint32_t DirtyBegin = 0;     // Rows changed since the last upload
			uint32_t DirtyEnd = 0;
			bool FullyDirty = true;      // The whole page needs uploading
		};

		// A block of cells: the top-left cell and the size in cells.
		struct CellBlock
		{
			uint32_t Page = 0;
			glm::uvec2 Cell = glm::uvec2(0);
			glm::uvec2 Cells = glm::uvec2(1);
		};

		FontAtlas() = default;
		GlyphEntry DescribeGlyph(uint32_t glyphIndex) const;
		void Rasterize(GlyphEntry& entry);
		// Finds cells for a glyph: free ones, a new page, or the least recently used glyphs not used in this frame.
		bool Allocate(const glm::uvec2& cells, CellBlock& outBlock);
		void Evict(uint32_t glyphIndex);
		void MarkDirty(Page& page, uint32_t firstRow, uint32_t rowCount);
	private:
		struct FontInfo;

		Ref<Font> m_Font;
		FontAtlasSpecification m_Specification;
		std::unique_ptr<FontInfo> m_Info; // stb_truetype state (kept out of the header)
		float m_Scale = 1.0f;             // Font units to atlas texels
		float m_EmScale = 1.0f;           // Font units to em
		FontMetrics m_Metrics;

		std::unordered_map<uint32_t, uint32_t> m_CodepointGlyphs; // Code point -> glyph index
		std::unordered_map<uint32_t, float> m_Kerning;            // Glyph pair (left << 16 | right) -> kerning
		std::unordered_map<uint32_t, GlyphEntry> m_Glyphs;        // Glyph index -> glyph
		std::vector<Page> m_Pages;
		uint64_t m_Frame = 0;
		GlyphRasterBudget m_Budget;
		bool m_ReportedFull = false;
		FontAtlasStats m_Stats;

		nvrhi::TextureHandle m_Texture;    // Texture2DArray, one layer per page (capacity grows in powers of two)
		nvrhi::TextureHandle m_UploadBand; // c_PageSize x c_UploadBandRows staging for partial page uploads
	};

}
