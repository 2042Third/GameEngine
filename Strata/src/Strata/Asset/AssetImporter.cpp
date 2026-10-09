#include "stpch.h"
#include "Strata/Asset/AssetImporter.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Hash.h"
#include "Strata/Core/StringUtils.h"

namespace Strata
{

	// Defined in Asset/AssetImporters.cpp: creates the built-in importers.
	void CreateBuiltinAssetImporters(std::vector<Scope<AssetImporter>>& importers);

	namespace
	{

		struct ImporterStorage
		{
			std::mutex Mutex;
			std::vector<Scope<AssetImporter>> Importers;
		};

		ImporterStorage& GetImporterStorage()
		{
			static ImporterStorage s_Storage;
			return s_Storage;
		}

		// Built-ins are registered first, before any other registration, so later registrations override them.
		void EnsureBuiltinImporters()
		{
			static std::once_flag s_Once;
			std::call_once(s_Once, []()
			{
				std::vector<Scope<AssetImporter>> builtins;
				CreateBuiltinAssetImporters(builtins);
				ImporterStorage& storage = GetImporterStorage();
				std::scoped_lock<std::mutex> lock(storage.Mutex);
				for (Scope<AssetImporter>& importer : builtins)
					storage.Importers.push_back(std::move(importer));
			});
		}

	}

	nlohmann::json AssetImporter::GetDefaultSettings(const std::filesystem::path&) const
	{
		return nlohmann::json::object();
	}

	void AssetImporterRegistry::Register(Scope<AssetImporter> importer)
	{
		EnsureBuiltinImporters();
		ImporterStorage& storage = GetImporterStorage();
		std::scoped_lock<std::mutex> lock(storage.Mutex);
		storage.Importers.push_back(std::move(importer));
	}

	const AssetImporter* AssetImporterRegistry::FindByExtension(std::string_view extension)
	{
		EnsureBuiltinImporters();
		const std::string lowered = StringUtils::ToLower(extension);
		ImporterStorage& storage = GetImporterStorage();
		std::scoped_lock<std::mutex> lock(storage.Mutex);
		// Later registrations win, so tools and tests can override built-in importers.
		for (auto it = storage.Importers.rbegin(); it != storage.Importers.rend(); ++it)
		{
			for (const std::string& candidate : (*it)->GetExtensions())
			{
				if (candidate == lowered)
					return it->get();
			}
		}
		return nullptr;
	}

	std::vector<const AssetImporter*> AssetImporterRegistry::GetAll()
	{
		EnsureBuiltinImporters();
		ImporterStorage& storage = GetImporterStorage();
		std::scoped_lock<std::mutex> lock(storage.Mutex);
		std::vector<const AssetImporter*> importers;
		for (const Scope<AssetImporter>& importer : storage.Importers)
			importers.push_back(importer.get());
		return importers;
	}

	AssetHandle DeriveSubAssetHandle(AssetHandle parent, std::string_view key)
	{
		uint64_t value = Hash::Combine(Hash::FNV1a(key), static_cast<uint64_t>(parent));
		// Never collide with the null handle or the range reserved for built-in assets.
		if (value <= c_MaxBuiltinAssetHandle)
			value += c_MaxBuiltinAssetHandle + 1;
		return UUID(value);
	}

	std::optional<std::vector<uint8_t>> ReadImportDependency(const ImportContext& context, ImportResult& result, const std::filesystem::path& path)
	{
		const std::filesystem::path normalized = path.lexically_normal();
		const std::filesystem::path assetDirectory = context.AssetDirectory.lexically_normal();
		if (!FileSystem::IsInside(normalized, assetDirectory))
			return std::nullopt;

		ImportDependency dependency;
		dependency.Path = normalized;
		dependency.Exists = FileSystem::IsRegularFile(normalized);

		// The state is taken before reading: a change while reading then shows as a later modification.
		std::optional<std::vector<uint8_t>> bytes;
		if (dependency.Exists)
		{
			dependency.Size = FileSystem::GetFileSize(normalized).value_or(0);
			dependency.Time = FileSystem::GetLastWriteTime(normalized).value_or(0);
			// A link leading out of the asset directory is not read.
			if (FileSystem::IsInsideResolved(normalized, assetDirectory))
				bytes = FileSystem::ReadBytes(normalized);
			if (bytes)
				dependency.Hash = Hash::FNV1a(std::span<const uint8_t>(*bytes));
			else
				dependency.Size = ImportDependency::c_Unreadable; // Never current: retried until the file can be read
		}

		const bool known = std::any_of(result.Dependencies.begin(), result.Dependencies.end(), [&](const ImportDependency& existing) { return existing.Path == normalized; });
		if (!known)
			result.Dependencies.push_back(std::move(dependency));
		return bytes;
	}

}
