#include "stpch.h"
#include "Strata/Renderer/FontAtlasCache.h"

#include "Strata/Core/Log.h"
#include "Strata/Renderer/Font.h"

namespace Strata
{

	void FontAtlasCache::BeginFrame(const GlyphRasterBudget& budget)
	{
		// Atlases live as long as their font asset: once only the cache still holds a font nobody drew with in the previous
		// frame, it is gone from the scene's asset manager (unloaded or replaced by a reload) and its atlas is dropped.
		const Font* defaultFont = Font::GetDefault().get();
		for (auto it = m_Atlases.begin(); it != m_Atlases.end();)
		{
			const Entry& entry = it->second;
			const bool orphaned = !entry.Used && (!entry.Atlas || entry.Atlas->GetFont().use_count() == 1) && it->first != defaultFont;
			it = orphaned ? m_Atlases.erase(it) : std::next(it);
		}
		for (auto& [font, entry] : m_Atlases)
		{
			entry.Used = false;
			if (entry.Atlas)
				entry.Atlas->BeginFrame();
		}
		m_Budget = budget;
	}

	FontAtlas* FontAtlasCache::Get(const Ref<Font>& font)
	{
		if (!font)
			return nullptr;
		auto it = m_Atlases.find(font.get());
		if (it == m_Atlases.end())
		{
			std::string error;
			Entry entry;
			entry.Atlas = FontAtlas::Create(font, &error);
			if (!entry.Atlas)
				ST_CORE_ERROR("A font cannot be drawn with: {}", error); // Kept as unusable, so not retried every frame
			else
				entry.Atlas->BeginFrame();
			it = m_Atlases.emplace(font.get(), std::move(entry)).first;
		}
		it->second.Used = true;
		return it->second.Atlas.get();
	}

	size_t FontAtlasCache::GetAtlasCount() const
	{
		size_t count = 0;
		for (const auto& [font, entry] : m_Atlases)
			count += entry.Atlas ? 1 : 0;
		return count;
	}

}
