#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Renderer/FontAtlas.h"

#include <cstdint>
#include <unordered_map>

namespace Strata
{

	class Font;

	// The glyph atlases a SceneRenderer draws text with: one per font, shared by every pass of the renderer that draws glyphs
	// (the world and screen text of TextRenderer, the game UI's overlay pass), with one glyph rasterization budget per frame
	// that the passes draw on in turn. Main thread only.
	//
	// An atlas lives while its font is used: BeginFrame, at the start of each frame, drops the atlases no pass asked for in
	// the previous frame whose font only the cache still holds (the font was unloaded or replaced by a reload), never the
	// default font's. Trimming at the start of a frame rather than after the first pass keeps the atlases later passes use.
	class FontAtlasCache
	{
	public:
		FontAtlasCache() = default;
		FontAtlasCache(const FontAtlasCache&) = delete;
		FontAtlasCache& operator=(const FontAtlasCache&) = delete;

		// Starts a frame: drops unused atlases (see above), starts a frame in every atlas (FontAtlas::BeginFrame) and gives the
		// frame `budget` of glyph rasterization.
		void BeginFrame(const GlyphRasterBudget& budget);
		// The font's atlas, created on first use and marked used this frame; null (logged once) when the font cannot be used.
		FontAtlas* Get(const Ref<Font>& font);

		// The frame's rasterization budget is lent to an atlas for a pass's glyph requests and given back afterwards:
		// atlas.SetRasterBudget(GetBudget()), requests, then SetBudget(atlas.GetRasterBudget()).
		const GlyphRasterBudget& GetBudget() const { return m_Budget; }
		void SetBudget(const GlyphRasterBudget& budget) { m_Budget = budget; }

		size_t GetAtlasCount() const;
	private:
		struct Entry
		{
			Scope<FontAtlas> Atlas; // Null for a font that cannot be used (not retried every frame)
			bool Used = false;      // Asked for since the latest BeginFrame
		};

		std::unordered_map<const Font*, Entry> m_Atlases;
		GlyphRasterBudget m_Budget;
	};

}
