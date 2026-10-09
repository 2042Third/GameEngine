#pragma once

#include "Strata/Asset/Asset.h"

#include <string>
#include <vector>

namespace Strata
{

	// Raw font file (TrueType/OpenType); glyph atlases are built by the text renderer on demand.
	class Font : public Asset
	{
	public:
		static AssetType GetStaticType() { return AssetType::Font; }
		AssetType GetType() const override { return GetStaticType(); }

		static Ref<Font> Create(std::vector<uint8_t> fontData, std::string* outError = nullptr);
		// The engine's built-in font (Roboto Medium), used by text without a font of its own. Created on first use.
		static const Ref<Font>& GetDefault();

		uint64_t GetMemoryUsage() const override { return m_Data.size(); }
		const std::vector<uint8_t>& GetData() const { return m_Data; }
	private:
		Font() = default;
	private:
		std::vector<uint8_t> m_Data;
	};

}
