#pragma once

#include "Strata/Asset/Asset.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	struct ImportedSubAsset
	{
		std::string Key;  // Stable identifier within the parent asset ("Mesh/0"); determines the sub-asset handle
		std::string Name; // Display name
		AssetType Type = AssetType::None;
		std::vector<uint8_t> Data; // Stored (cooked) bytes
	};

	// A file an import read besides its source, with its state before reading. A change to the file, or its creation
	// when it was missing, redoes the import like a change to the source itself.
	struct ImportDependency
	{
		std::filesystem::path Path; // Absolute, inside the asset directory
		bool Exists = false;
		uint64_t Size = 0; // c_Unreadable when the file exists but could not be read
		int64_t Time = 0;
		uint64_t Hash = 0; // Of the bytes read

		static constexpr uint64_t c_Unreadable = UINT64_MAX;
	};

	struct ImportResult
	{
		std::vector<uint8_t> Data; // Stored (cooked) bytes of the asset itself
		std::vector<ImportedSubAsset> SubAssets;
		std::vector<std::string> Warnings;
		std::vector<ImportDependency> Dependencies; // Filled by ReadImportDependency
	};

	struct ImportContext
	{
		AssetHandle Handle = UUID::Null();    // The asset being imported; sub-asset handles derive from it
		std::filesystem::path SourcePath;     // Absolute path of the source file
		std::filesystem::path AssetDirectory; // Project asset root
		nlohmann::json Settings;              // Import settings (importer defaults merged with the asset's .meta)
		// Resolves another file of the project (e.g. a texture referenced by a model) to the handle of its registered
		// asset. Returns null for unregistered files, files outside the asset directory and unknown types.
		std::function<AssetHandle(const std::filesystem::path& absolutePath)> ResolveAsset;
	};

	// Converts a source file into the stored form the asset loaders read. Importers run on I/O threads and must not
	// touch engine state other than through the ImportContext.
	class AssetImporter
	{
	public:
		virtual ~AssetImporter() = default;

		virtual AssetType GetType() const = 0;
		virtual std::vector<std::string> GetExtensions() const = 0; // Lower case, with the dot (".png")
		// Increment whenever the output format changes; stale cached imports are then redone automatically.
		virtual uint32_t GetVersion() const = 0;
		virtual nlohmann::json GetDefaultSettings(const std::filesystem::path& sourcePath) const;
		// True when the stored form is the source file itself (JSON assets): nothing is cached.
		virtual bool StoresSourceDirectly() const { return false; }

		virtual bool Import(const ImportContext& context, ImportResult& result, std::string* outError) const = 0;
	};

	class AssetImporterRegistry
	{
	public:
		static void Register(Scope<AssetImporter> importer);
		static const AssetImporter* FindByExtension(std::string_view extension); // Case-insensitive
		static std::vector<const AssetImporter*> GetAll();
	};

	// Deterministic handle of a sub-asset: stable across re-imports and machines for the same parent and key.
	AssetHandle DeriveSubAssetHandle(AssetHandle parent, std::string_view key);

	// Reads a file an import uses besides its source (external buffers, images) and records it in the result's
	// dependencies, also when it is missing. Only regular files inside the asset directory, also after resolving
	// symbolic links, are read; returns null for anything else.
	std::optional<std::vector<uint8_t>> ReadImportDependency(const ImportContext& context, ImportResult& result, const std::filesystem::path& path);

}
