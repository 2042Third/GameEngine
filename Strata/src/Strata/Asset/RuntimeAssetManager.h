#pragma once

#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/AssetPack.h"

#include <filesystem>

namespace Strata
{

	// Asset manager of shipped games: every asset comes from an asset pack built by the editor. Assets load
	// asynchronously like in the editor, but nothing is imported and nothing changes at runtime.
	class RuntimeAssetManager final : public AssetManagerBase
	{
	public:
		// Returns null (with an error) when the pack cannot be opened.
		static Ref<RuntimeAssetManager> Create(const std::filesystem::path& packPath, std::string* outError = nullptr);
		~RuntimeAssetManager() override;

		const AssetPack& GetPack() const { return *m_Pack; }
	protected:
		bool ReadAssetData(const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string* outError) override;
	private:
		explicit RuntimeAssetManager(Scope<AssetPack> pack);
	private:
		Scope<AssetPack> m_Pack;
		std::unordered_map<AssetHandle, size_t> m_EntryIndices; // Immutable after construction
	};

}
