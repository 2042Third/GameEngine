#include "stpch.h"
#include "Strata/Asset/EditorAssetManager.h"

#include "Strata/Asset/AssetPack.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Hash.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/StringUtils.h"
#include "Strata/Reflection/PropertyJson.h"

#include <cctype>

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_MetaVersion = 1;
		constexpr uint32_t c_ImportRecordVersion = 1;

		std::string GetExtension(const std::filesystem::path& path)
		{
			return StringUtils::ToLower(FileSystem::ToUTF8(path.extension()));
		}

		std::filesystem::path GetMetaPath(const std::filesystem::path& sourcePath)
		{
			std::filesystem::path metaPath = sourcePath;
			metaPath += EditorAssetManager::c_MetaExtension;
			return metaPath;
		}

		std::string HashToString(uint64_t hash)
		{
			return fmt::format("{:016X}", hash);
		}

		uint64_t HashSettings(const nlohmann::json& settings)
		{
			// nlohmann::json objects keep their keys sorted, so equal settings always dump identically.
			return Hash::FNV1a(JsonUtils::Dump(settings));
		}

		std::optional<uint64_t> HashFile(const std::filesystem::path& path)
		{
			std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(path);
			if (!bytes)
				return std::nullopt;
			return Hash::FNV1a(std::span<const uint8_t>(*bytes));
		}

		// Directories and files starting with '.' (version control, caches, editor and OS metadata such as macOS
		// "._" files) are never assets.
		bool IsHiddenName(const std::filesystem::path& name)
		{
			const std::string text = FileSystem::ToUTF8(name);
			return !text.empty() && text[0] == '.';
		}

		bool IsHiddenRelativePath(const std::filesystem::path& relativePath)
		{
			for (const std::filesystem::path& component : relativePath)
			{
				if (IsHiddenName(component))
					return true;
			}
			return false;
		}

		enum class MetaStatus : uint8_t
		{
			Missing,
			Invalid,
			Valid
		};

		struct MetaContents
		{
			MetaStatus Status = MetaStatus::Missing;
			AssetHandle Handle = UUID::Null();
			nlohmann::json Settings = nlohmann::json::object();
			std::string Error;
		};

		MetaContents ReadMetaFile(const std::filesystem::path& metaPath)
		{
			MetaContents contents;
			if (!FileSystem::Exists(metaPath))
				return contents;

			contents.Status = MetaStatus::Invalid;
			std::optional<std::string> text = FileSystem::ReadText(metaPath);
			if (!text)
			{
				contents.Error = "cannot be read";
				return contents;
			}
			std::optional<nlohmann::json> meta = JsonUtils::Parse(*text, &contents.Error);
			if (!meta || !meta->is_object())
			{
				if (contents.Error.empty())
					contents.Error = "not a JSON object";
				return contents;
			}
			const nlohmann::json* handle = JsonUtils::Find(*meta, "Handle");
			std::optional<UUID> parsedHandle = handle ? UUIDFromJson(*handle) : std::nullopt;
			if (!parsedHandle || !parsedHandle->IsValid())
			{
				contents.Error = "missing or invalid \"Handle\"";
				return contents;
			}
			contents.Status = MetaStatus::Valid;
			contents.Handle = *parsedHandle;
			if (const nlohmann::json* settings = JsonUtils::Find(*meta, "ImportSettings"); settings && settings->is_object())
				contents.Settings = *settings;
			return contents;
		}

		// Key of a file in the dependency map. The default file systems of Windows and macOS ignore case, so a model
		// may name "textures/Wood.png" for "Textures/wood.png", while the file watcher reports the spelling on disk.
		std::string GetDependencyKey(const std::string& relativePath)
		{
#if defined(ST_PLATFORM_WINDOWS) || defined(ST_PLATFORM_MACOS)
			return StringUtils::ToLower(relativePath);
#else
			return relativePath;
#endif
		}

		void MergeSettings(nlohmann::json& settings, const nlohmann::json& overrides)
		{
			if (!overrides.is_object())
				return;
			if (!settings.is_object())
				settings = nlohmann::json::object();
			for (const auto& [key, value] : overrides.items())
				settings[key] = value;
		}

	}

	EditorAssetManager::EditorAssetManager(const EditorAssetManagerSpecification& specification)
		: m_Specification(specification)
	{
		// The file watcher reports absolute paths, so the directories must be absolute too.
		std::error_code error;
		m_Specification.AssetDirectory = std::filesystem::absolute(m_Specification.AssetDirectory, error).lexically_normal();
		m_Specification.CacheDirectory = std::filesystem::absolute(m_Specification.CacheDirectory, error).lexically_normal();
		FileSystem::CreateDirectories(m_Specification.AssetDirectory);
		FileSystem::CreateDirectories(m_Specification.CacheDirectory);
	}

	EditorAssetManager::~EditorAssetManager()
	{
		m_Watcher.Stop();
		{
			// Import jobs reference this manager.
			std::unique_lock<std::mutex> lock(m_ImportMutex);
			m_ImportCondition.wait(lock, [this]() { return m_ImportsInFlight == 0; });
		}
		WaitForInFlightLoads();
		SaveAssetIndex();
	}

	////////////////////////////////////////////////////////////////////////////////
	// Paths
	////////////////////////////////////////////////////////////////////////////////

	std::filesystem::path EditorAssetManager::ToAbsolute(std::string_view relativePath) const
	{
		return (m_Specification.AssetDirectory / FileSystem::FromUTF8(relativePath)).lexically_normal();
	}

	std::string EditorAssetManager::ToRelative(const std::filesystem::path& absolutePath) const
	{
		const std::filesystem::path relative = FileSystem::GetRelativePath(absolutePath, m_Specification.AssetDirectory);
		return relative.empty() ? std::string() : FileSystem::ToUTF8(relative);
	}

	std::filesystem::path EditorAssetManager::GetCachePath(AssetHandle handle) const
	{
		return m_Specification.CacheDirectory / (handle.ToString() + ".bin");
	}

	std::filesystem::path EditorAssetManager::GetImportRecordPath(AssetHandle handle) const
	{
		return m_Specification.CacheDirectory / (handle.ToString() + ".import");
	}

	////////////////////////////////////////////////////////////////////////////////
	// Registration
	////////////////////////////////////////////////////////////////////////////////

	bool EditorAssetManager::WriteMetaFile(const SourceAsset& source) const
	{
		nlohmann::json meta = nlohmann::json::object();
		meta["Strata"] = { { "Format", "AssetMeta" }, { "Version", c_MetaVersion } };
		meta["Handle"] = UUIDToJson(source.Handle);
		meta["Type"] = AssetTypeToString(source.Type);
		meta["ImportSettings"] = source.Settings;
		return FileSystem::WriteText(GetMetaPath(ToAbsolute(source.Path)), JsonUtils::Dump(meta, 1, '\t') + "\n");
	}

	AssetHandle EditorAssetManager::RegisterSourceFile(const std::filesystem::path& absolutePath, bool assignNewHandle)
	{
		const std::filesystem::path path = absolutePath.lexically_normal();
		const std::string relativePath = ToRelative(path);
		if (relativePath.empty() || IsHiddenRelativePath(FileSystem::FromUTF8(relativePath)) || !FileSystem::IsRegularFile(path))
			return UUID::Null();

		const AssetImporter* importer = AssetImporterRegistry::FindByExtension(GetExtension(path));
		if (!importer)
			return UUID::Null();

		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_SourcesByPath.find(relativePath);
			if (it != m_SourcesByPath.end())
				return it->second;
		}

		SourceAsset source;
		source.Path = relativePath;
		source.Importer = importer;
		source.Type = importer->GetType();
		source.Settings = importer->GetDefaultSettings(path);
		source.SourceSize = FileSystem::GetFileSize(path).value_or(0);
		source.SourceTime = FileSystem::GetLastWriteTime(path).value_or(0);

		const std::filesystem::path metaPath = GetMetaPath(path);
		const MetaContents meta = ReadMetaFile(metaPath);
		if (meta.Status == MetaStatus::Invalid)
		{
			// Never overwrite it: it may hold a merge conflict, and the handle in it is what other assets reference.
			ST_CORE_ERROR("'{}' is not a valid asset .meta file ({}); '{}' is ignored until the .meta is fixed or deleted",
				FileSystem::ToUTF8(metaPath), meta.Error, relativePath);
			return UUID::Null();
		}
		MergeSettings(source.Settings, meta.Settings);
		AssetHandle handle = meta.Status == MetaStatus::Valid && !assignNewHandle ? meta.Handle : UUID::Null();
		bool writeMeta = meta.Status == MetaStatus::Missing || assignNewHandle;
		if (source.Importer->StoresSourceDirectly())
			source.Import.Imported = true; // Read as-is: nothing to import

		if (handle.IsValid())
		{
			// The handle may already be in use: a file moved together with its .meta outside the editor (a move), or a
			// file copied with its .meta (a duplicate, which needs a new handle).
			std::unique_lock<std::mutex> lock(m_SourceMutex);
			auto existing = m_Sources.find(handle);
			if (existing != m_Sources.end())
			{
				SourceAsset& moved = existing->second;
				if (!FileSystem::Exists(ToAbsolute(moved.Path)))
				{
					m_SourcesByPath.erase(moved.Path);
					moved.Path = relativePath;
					moved.PathGeneration++;
					m_SourcesByPath[relativePath] = handle;
					m_AssetIndexDirty = true;
					const std::vector<AssetHandle> subAssets = moved.Import.SubAssets;
					lock.unlock();

					for (AssetHandle assetHandle : subAssets)
					{
						if (std::optional<AssetMetadata> metadata = GetMetadata(assetHandle))
						{
							metadata->Path = relativePath;
							UpdateAssetMetadata(*metadata);
						}
					}
					if (std::optional<AssetMetadata> metadata = GetMetadata(handle))
					{
						metadata->Path = relativePath;
						metadata->Name = FileSystem::ToUTF8(path.stem());
						UpdateAssetMetadata(*metadata);
					}
					ST_CORE_INFO("Asset '{}' moved to '{}'", FileSystem::ToUTF8(path.filename()), relativePath);
					return handle;
				}
			}
			lock.unlock();

			if (BuiltinAssets::IsBuiltin(handle) || IsHandleValid(handle))
			{
				ST_CORE_WARN("'{}' has the asset handle of another asset (copied .meta file?); assigning a new handle", relativePath);
				handle = UUID::Null();
				writeMeta = true;
			}
		}

		if (!handle.IsValid())
		{
			do
				handle = UUID();
			while (BuiltinAssets::IsBuiltin(handle) || IsHandleValid(handle));
		}
		source.Handle = handle;

		if (writeMeta && !WriteMetaFile(source))
			ST_CORE_ERROR("Could not write '{}'; the asset's handle will change on the next scan", FileSystem::ToUTF8(metaPath));

		AssetMetadata metadata;
		metadata.Handle = handle;
		metadata.Type = source.Type;
		metadata.Path = relativePath;
		metadata.Name = FileSystem::ToUTF8(path.stem());
		// Imported assets are read from the cache (known once imported, see ApplyImportOutcome).
		metadata.StoredSize = source.Importer->StoresSourceDirectly() ? source.SourceSize : FileSystem::GetFileSize(GetCachePath(handle)).value_or(0);
		RegisterAsset(metadata);

		std::scoped_lock<std::mutex> lock(m_SourceMutex);
		m_SourcesByPath[relativePath] = handle;
		m_Sources.emplace(handle, std::move(source));
		m_AssetIndexDirty = true;
		return handle;
	}

	void EditorAssetManager::UnregisterSource(AssetHandle handle, bool deleteCache)
	{
		std::vector<AssetHandle> subAssets;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_Sources.find(handle);
			if (it == m_Sources.end())
				return;
			subAssets = it->second.Import.SubAssets;
			SetDependencies(it->second, {});
			m_SourcesByPath.erase(it->second.Path);
			m_Sources.erase(it);
			m_AssetIndexDirty = true;
		}

		for (AssetHandle subAsset : subAssets)
			UnregisterAsset(subAsset);
		UnregisterAsset(handle);

		if (deleteCache)
		{
			std::unique_lock<std::shared_mutex> fileLock(m_FileMutex);
			for (AssetHandle subAsset : subAssets)
				FileSystem::Remove(GetCachePath(subAsset));
			FileSystem::Remove(GetCachePath(handle));
			FileSystem::Remove(GetImportRecordPath(handle));
		}
	}

	void EditorAssetManager::SetDependencies(SourceAsset& source, std::vector<std::string> dependencies)
	{
		for (const std::string& path : source.Dependencies)
		{
			auto it = m_DependentsByPath.find(GetDependencyKey(path));
			if (it == m_DependentsByPath.end())
				continue;
			std::erase(it->second, source.Handle);
			if (it->second.empty())
				m_DependentsByPath.erase(it);
		}
		source.Dependencies = std::move(dependencies);
		for (const std::string& path : source.Dependencies)
		{
			std::vector<AssetHandle>& dependents = m_DependentsByPath[GetDependencyKey(path)];
			if (std::find(dependents.begin(), dependents.end(), source.Handle) == dependents.end())
				dependents.push_back(source.Handle);
		}
	}

	std::vector<AssetHandle> EditorAssetManager::GetDependents(const std::filesystem::path& absolutePath) const
	{
		const std::string relativePath = ToRelative(absolutePath.lexically_normal());
		if (relativePath.empty())
			return {};
		std::scoped_lock<std::mutex> lock(m_SourceMutex);
		auto it = m_DependentsByPath.find(GetDependencyKey(relativePath));
		return it != m_DependentsByPath.end() ? it->second : std::vector<AssetHandle>();
	}

	void EditorAssetManager::Scan()
	{
		ST_PROFILE_FUNCTION();
		const auto startTime = std::chrono::steady_clock::now();

		std::vector<std::filesystem::path> files;
		std::error_code error;
		std::filesystem::recursive_directory_iterator iterator(m_Specification.AssetDirectory, std::filesystem::directory_options::skip_permission_denied, error);
		const std::filesystem::recursive_directory_iterator end;
		while (!error && iterator != end)
		{
			const std::filesystem::directory_entry& entry = *iterator;
			if (IsHiddenName(entry.path().filename()))
			{
				if (entry.is_directory(error))
					iterator.disable_recursion_pending();
			}
			else if (entry.is_regular_file(error) && GetExtension(entry.path()) != c_MetaExtension)
			{
				files.push_back(entry.path());
			}
			iterator.increment(error);
		}
		if (error)
			ST_CORE_ERROR("Asset scan of '{}' incomplete: {}", FileSystem::ToUTF8(m_Specification.AssetDirectory), error.message());

		std::sort(files.begin(), files.end());

		// Files copied together with their .meta share a handle. The file the persisted index assigns the handle to
		// keeps it; without an index (fresh checkout) the shortest path wins, which is the original for the usual
		// "Name - Copy" / "Name copy" / "Name (1)" naming of file managers. The others get new handles.
		const std::unordered_map<uint64_t, std::string> index = LoadAssetIndex();
		std::unordered_map<uint64_t, std::vector<size_t>> filesByHandle;
		for (size_t fileIndex = 0; fileIndex < files.size(); fileIndex++)
		{
			const MetaContents meta = ReadMetaFile(GetMetaPath(files[fileIndex]));
			if (meta.Status == MetaStatus::Valid)
				filesByHandle[static_cast<uint64_t>(meta.Handle)].push_back(fileIndex);
		}

		std::vector<bool> needsNewHandle(files.size(), false);
		for (const auto& [handle, fileIndices] : filesByHandle)
		{
			if (fileIndices.size() < 2)
				continue;
			auto indexed = index.find(handle);
			size_t winner = fileIndices.front();
			bool winnerFromIndex = false;
			for (size_t fileIndex : fileIndices)
			{
				if (indexed != index.end() && ToRelative(files[fileIndex]) == indexed->second)
				{
					winner = fileIndex;
					winnerFromIndex = true;
					break;
				}
			}
			if (!winnerFromIndex)
			{
				for (size_t fileIndex : fileIndices)
				{
					if (ToRelative(files[fileIndex]).size() < ToRelative(files[winner]).size())
						winner = fileIndex;
				}
			}
			for (size_t fileIndex : fileIndices)
			{
				if (fileIndex != winner)
				{
					needsNewHandle[fileIndex] = true;
					ST_CORE_WARN("'{}' has the asset handle of '{}' (copied .meta file?); assigning a new handle", ToRelative(files[fileIndex]), ToRelative(files[winner]));
				}
			}
		}

		for (size_t fileIndex = 0; fileIndex < files.size(); fileIndex++)
		{
			if (!needsNewHandle[fileIndex])
				RegisterSourceFile(files[fileIndex]);
		}
		for (size_t fileIndex = 0; fileIndex < files.size(); fileIndex++)
		{
			if (needsNewHandle[fileIndex])
				RegisterSourceFile(files[fileIndex], true);
		}

		std::vector<ImportRequest> requests;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			for (auto& [handle, source] : m_Sources)
			{
				if (source.Importer->StoresSourceDirectly())
					source.Import.Imported = true;
				else if (!source.Import.Imported)
					requests.push_back(MakeImportRequest(source));
			}
		}

		std::vector<ImportOutcome> outcomes(requests.size());
		std::atomic<uint32_t> importedCount = 0;
		JobSystem::ParallelFor(static_cast<uint32_t>(requests.size()), 1, [&](uint32_t begin, uint32_t endIndex)
		{
			for (uint32_t requestIndex = begin; requestIndex < endIndex; requestIndex++)
			{
				try
				{
					if (!LoadCurrentImport(requests[requestIndex], outcomes[requestIndex]))
					{
						outcomes[requestIndex] = RunImport(requests[requestIndex]);
						importedCount++;
					}
				}
				catch (const std::exception& exception)
				{
					outcomes[requestIndex] = ImportOutcome();
					outcomes[requestIndex].Handle = requests[requestIndex].Handle;
					outcomes[requestIndex].Error = fmt::format("Import failed: {}", exception.what());
				}
			}
		});

		uint32_t failedCount = 0;
		for (ImportOutcome& outcome : outcomes)
		{
			failedCount += outcome.Success ? 0 : 1;
			ApplyImportOutcome(std::move(outcome), false);
		}
		RemoveUnusedCacheFiles();

		if (m_Specification.WatchFiles && !m_Watcher.IsRunning() && !m_Watcher.Start(m_Specification.AssetDirectory))
			ST_CORE_WARN("Asset hot reload is unavailable: cannot watch '{}'", FileSystem::ToUTF8(m_Specification.AssetDirectory));

		SaveAssetIndex();
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime);
		ST_CORE_INFO("Asset scan: {} assets, {} imported, {} failed ({} ms)", files.size(), importedCount.load(), failedCount, elapsed.count());
	}

	////////////////////////////////////////////////////////////////////////////////
	// Importing
	////////////////////////////////////////////////////////////////////////////////

	EditorAssetManager::ImportRequest EditorAssetManager::MakeImportRequest(const SourceAsset& source) const
	{
		return ImportRequest { source.Handle, source.Path, source.Importer, source.Settings };
	}

	void EditorAssetManager::RemoveUnusedCacheFiles()
	{
		ST_CORE_ASSERT(!m_Watcher.IsRunning(), "Cache files are cleaned up during the scan, before background imports can start");
		// Cache files are named "<handle>.<kind>[.<temporary suffix>]". No import runs during a scan, so temporary
		// files are leftovers of interrupted writes.
		std::vector<std::filesystem::path> unused;
		std::error_code error;
		for (std::filesystem::directory_iterator it(m_Specification.CacheDirectory, error), end; !error && it != end; it.increment(error))
		{
			const std::string name = FileSystem::ToUTF8(it->path().filename());
			const size_t dot = name.find('.');
			const std::optional<UUID> handle = dot != std::string::npos ? UUID::FromString(std::string_view(name).substr(0, dot)) : std::nullopt;
			std::error_code typeError;
			if (!handle || !it->is_regular_file(typeError))
				continue;
			const std::string_view kind = std::string_view(name).substr(dot);
			if ((kind != ".bin" && kind != ".import") || !IsHandleValid(*handle))
				unused.push_back(it->path());
		}

		std::unique_lock<std::shared_mutex> fileLock(m_FileMutex);
		for (const std::filesystem::path& path : unused)
			FileSystem::Remove(path);
	}

	std::unordered_map<uint64_t, std::string> EditorAssetManager::LoadAssetIndex() const
	{
		std::unordered_map<uint64_t, std::string> index;
		std::optional<std::string> text = FileSystem::ReadText(m_Specification.CacheDirectory / "AssetIndex.json");
		std::optional<nlohmann::json> document = text ? JsonUtils::Parse(*text) : std::nullopt;
		const nlohmann::json* assets = document ? JsonUtils::Find(*document, "Assets") : nullptr;
		if (!assets || !assets->is_object())
			return index;
		for (const auto& [handleText, path] : assets->items())
		{
			std::optional<UUID> handle = UUID::FromString(handleText);
			if (handle && path.is_string())
				index.emplace(static_cast<uint64_t>(*handle), path.get<std::string>());
		}
		return index;
	}

	void EditorAssetManager::SaveAssetIndex()
	{
		nlohmann::json assets = nlohmann::json::object();
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			if (!m_AssetIndexDirty)
				return;
			for (const auto& [handle, source] : m_Sources)
				assets[handle.ToString()] = source.Path;
			m_AssetIndexDirty = false;
		}
		nlohmann::json document = { { "Format", "AssetIndex" }, { "Version", 1 }, { "Assets", std::move(assets) } };
		if (!FileSystem::WriteText(m_Specification.CacheDirectory / "AssetIndex.json", JsonUtils::Dump(document, 1, '\t')))
			ST_CORE_WARN("Could not write the asset index in '{}'", FileSystem::ToUTF8(m_Specification.CacheDirectory));
	}

	bool EditorAssetManager::LoadCurrentImport(const ImportRequest& request, ImportOutcome& outOutcome) const
	{
		const std::filesystem::path recordPath = GetImportRecordPath(request.Handle);
		std::optional<std::string> recordText = FileSystem::ReadText(recordPath);
		if (!recordText)
			return false;
		std::optional<nlohmann::json> record = JsonUtils::Parse(*recordText);
		if (!record || !record->is_object())
			return false;

		if (JsonUtils::GetUInt(*record, "Version", 0) != c_ImportRecordVersion
			|| JsonUtils::GetUInt(*record, "ImporterVersion", 0) != request.Importer->GetVersion()
			|| JsonUtils::GetString(*record, "SettingsHash") != HashToString(HashSettings(request.Settings))
			|| !JsonUtils::GetBool(*record, "Success", false))
			return false;

		// A file is current when its size and time match, or when only its time changed (checkout, copy) but not its
		// contents. Refreshed times are written back so the contents are compared once.
		bool recordChanged = false;
		auto isCurrent = [&recordChanged](const std::filesystem::path& path, nlohmann::json& state, std::string_view sizeKey, std::string_view timeKey, std::string_view hashKey)
		{
			const std::optional<uint64_t> size = FileSystem::GetFileSize(path);
			const int64_t time = FileSystem::GetLastWriteTime(path).value_or(0);
			if (!size || *size != JsonUtils::GetUInt(state, sizeKey, UINT64_MAX))
				return false;
			if (time == JsonUtils::GetInt(state, timeKey, 0))
				return true;
			const std::optional<uint64_t> hash = HashFile(path);
			if (!hash || HashToString(*hash) != JsonUtils::GetString(state, hashKey))
				return false;
			state[std::string(timeKey)] = time;
			recordChanged = true;
			return true;
		};

		const std::filesystem::path sourcePath = ToAbsolute(request.Path);
		if (!isCurrent(sourcePath, *record, "SourceSize", "SourceTime", "SourceHash"))
			return false;

		ImportOutcome outcome;
		outcome.Handle = request.Handle;
		outcome.Success = true;
		outcome.FromCache = true;
		outcome.SourceSize = JsonUtils::GetUInt(*record, "SourceSize", 0);
		outcome.SourceTime = JsonUtils::GetInt(*record, "SourceTime", 0);

		if (auto dependencies = record->find("Dependencies"); dependencies != record->end() && dependencies->is_array())
		{
			// Relative references resolve against the source's place: a moved model reads different files.
			if (!dependencies->empty() && JsonUtils::GetString(*record, "Source") != request.Path)
				return false;
			for (nlohmann::json& dependency : *dependencies)
			{
				const std::string path = JsonUtils::GetString(dependency, "Path");
				if (path.empty() || !dependency.is_object())
					return false;
				const std::filesystem::path dependencyPath = ToAbsolute(path);
				const bool exists = FileSystem::IsRegularFile(dependencyPath);
				if (exists != JsonUtils::GetBool(dependency, "Exists", false) || (exists && !isCurrent(dependencyPath, dependency, "Size", "Time", "Hash")))
					return false;
				outcome.Dependencies.push_back(path);
			}
		}
		if (const nlohmann::json* warnings = JsonUtils::Find(*record, "Warnings"); warnings && warnings->is_array())
		{
			for (const nlohmann::json& warning : *warnings)
			{
				if (warning.is_string())
					outcome.Warnings.push_back(warning.get<std::string>());
			}
		}

		if (!FileSystem::IsRegularFile(GetCachePath(request.Handle)))
			return false;
		if (const nlohmann::json* subAssets = JsonUtils::Find(*record, "SubAssets"); subAssets && subAssets->is_array())
		{
			for (const nlohmann::json& subAsset : *subAssets)
			{
				ImportedSubAsset entry;
				entry.Key = JsonUtils::GetString(subAsset, "Key");
				entry.Name = JsonUtils::GetString(subAsset, "Name");
				entry.Type = AssetTypeFromString(JsonUtils::GetString(subAsset, "Type")).value_or(AssetType::None);
				if (entry.Key.empty() || entry.Type == AssetType::None)
					return false;
				if (!FileSystem::IsRegularFile(GetCachePath(DeriveSubAssetHandle(request.Handle, entry.Key))))
					return false;
				outcome.SubAssets.push_back(std::move(entry));
			}
		}

		if (recordChanged)
			FileSystem::WriteText(recordPath, JsonUtils::Dump(*record, 1, '\t'));
		outOutcome = std::move(outcome);
		return true;
	}

	EditorAssetManager::ImportOutcome EditorAssetManager::RunImport(const ImportRequest& request) const
	{
		ST_PROFILE_FUNCTION();
		const std::filesystem::path sourcePath = ToAbsolute(request.Path);

		ImportOutcome outcome;
		outcome.Handle = request.Handle;
		// The file state before importing: a change during the import is then still detected as a modification.
		outcome.SourceSize = FileSystem::GetFileSize(sourcePath).value_or(0);
		outcome.SourceTime = FileSystem::GetLastWriteTime(sourcePath).value_or(0);
		const std::optional<uint64_t> sourceHash = HashFile(sourcePath);

		ImportContext context;
		context.Handle = request.Handle;
		context.SourcePath = sourcePath;
		context.AssetDirectory = m_Specification.AssetDirectory;
		context.Settings = request.Settings;
		context.ResolveAsset = [this](const std::filesystem::path& path) { return ResolveProjectFile(path); };

		// Large sources import one at a time: decoded data is many times the file size.
		static std::mutex s_LargeImportMutex;
		std::unique_lock<std::mutex> largeImportLock(s_LargeImportMutex, std::defer_lock);
		if (outcome.SourceSize > c_LargeImportSize)
			largeImportLock.lock();

		ImportResult result;
		std::string error;
		try
		{
			outcome.Success = request.Importer->Import(context, result, &error);
		}
		catch (const std::exception& exception)
		{
			outcome.Success = false;
			error = fmt::format("the importer failed: {}", exception.what());
			result = ImportResult();
		}
		catch (...)
		{
			outcome.Success = false;
			error = "the importer failed with an unknown exception";
			result = ImportResult();
		}
		largeImportLock = {};
		outcome.Warnings = std::move(result.Warnings);

		// Dependencies count for failed imports too: creating or fixing a missing file must retry the import.
		nlohmann::json dependencies = nlohmann::json::array();
		for (const ImportDependency& dependency : result.Dependencies)
		{
			const std::string relativePath = ToRelative(dependency.Path);
			if (relativePath.empty() || relativePath == request.Path)
				continue;
			dependencies.push_back({ { "Path", relativePath }, { "Exists", dependency.Exists }, { "Size", dependency.Size }, { "Time", dependency.Time },
				{ "Hash", HashToString(dependency.Hash) } });
			outcome.Dependencies.push_back(relativePath);
		}

		if (!outcome.Success)
		{
			outcome.Error = error.empty() ? std::string("Import failed") : error;
		}
		else
		{
			// Cache files are written under temporary names first: only replacing them takes the file lock, so loads
			// are not blocked while a large import writes.
			std::vector<std::pair<std::filesystem::path, std::filesystem::path>> stagedFiles;
			auto stage = [&](AssetHandle handle, std::vector<uint8_t>& data)
			{
				std::filesystem::path stagedPath = GetCachePath(handle);
				stagedPath += ".staged";
				const bool written = FileSystem::WriteBytes(stagedPath, data);
				stagedFiles.emplace_back(stagedPath, GetCachePath(handle));
				std::vector<uint8_t>().swap(data); // Free the memory now: imports can be large
				return written;
			};
			bool written = stage(request.Handle, result.Data);
			for (ImportedSubAsset& subAsset : result.SubAssets)
				written = stage(DeriveSubAssetHandle(request.Handle, subAsset.Key), subAsset.Data) && written; // Frees every sub-asset's data

			if (written)
			{
				std::unique_lock<std::shared_mutex> fileLock(m_FileMutex);
				for (const auto& [stagedPath, cachePath] : stagedFiles)
					written = written && FileSystem::Rename(stagedPath, cachePath);
			}
			if (!written)
			{
				for (const auto& [stagedPath, cachePath] : stagedFiles)
					FileSystem::Remove(stagedPath);
				outcome.Success = false;
				outcome.Error = fmt::format("Could not write imported data to '{}'", FileSystem::ToUTF8(m_Specification.CacheDirectory));
			}
			outcome.SubAssets = std::move(result.SubAssets);
		}

		nlohmann::json record = nlohmann::json::object();
		record["Format"] = "ImportRecord";
		record["Version"] = c_ImportRecordVersion;
		record["Source"] = request.Path;
		record["ImporterVersion"] = request.Importer->GetVersion();
		record["SettingsHash"] = HashToString(HashSettings(request.Settings));
		record["SourceSize"] = outcome.SourceSize;
		record["SourceTime"] = outcome.SourceTime;
		record["SourceHash"] = sourceHash ? HashToString(*sourceHash) : std::string();
		record["Success"] = outcome.Success;
		record["Error"] = outcome.Error;
		record["Warnings"] = outcome.Warnings;
		nlohmann::json subAssets = nlohmann::json::array();
		for (const ImportedSubAsset& subAsset : outcome.SubAssets)
			subAssets.push_back({ { "Key", subAsset.Key }, { "Name", subAsset.Name }, { "Type", AssetTypeToString(subAsset.Type) } });
		record["SubAssets"] = std::move(subAssets);
		record["Dependencies"] = std::move(dependencies);
		if (!FileSystem::WriteText(GetImportRecordPath(request.Handle), JsonUtils::Dump(record, 1, '\t')))
			ST_CORE_WARN("Could not write the import record of '{}'", request.Path);
		return outcome;
	}

	void EditorAssetManager::ApplyImportOutcome(ImportOutcome outcome, bool reload)
	{
		std::vector<AssetHandle> previousSubAssets;
		std::vector<AssetHandle> subAssetHandles;
		std::string path;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_Sources.find(outcome.Handle);
			if (it == m_Sources.end())
				return; // Deleted while importing

			SourceAsset& source = it->second;
			source.Import.Imported = true;
			source.Import.Error = outcome.Error;
			source.Import.Warnings = outcome.Warnings;
			source.SourceSize = outcome.SourceSize;
			source.SourceTime = outcome.SourceTime;
			SetDependencies(source, std::move(outcome.Dependencies));
			path = source.Path;
			// A failed import keeps the previous sub-assets registered, so references to them survive until the source
			// is fixed (loading them reports the import error meanwhile).
			if (outcome.Success)
			{
				previousSubAssets = std::move(source.Import.SubAssets);
				for (const ImportedSubAsset& subAsset : outcome.SubAssets)
					subAssetHandles.push_back(DeriveSubAssetHandle(source.Handle, subAsset.Key));
				source.Import.SubAssets = subAssetHandles;
			}
		}

		for (const std::string& warning : outcome.Warnings)
			ST_CORE_WARN("Import '{}': {}", path, warning);
		if (!outcome.Success)
		{
			ST_CORE_ERROR("Import of '{}' failed: {}", path, outcome.Error);
			return;
		}

		SetStoredSize(outcome.Handle, FileSystem::GetFileSize(GetCachePath(outcome.Handle)).value_or(0));
		for (size_t index = 0; index < outcome.SubAssets.size(); index++)
		{
			const ImportedSubAsset& subAsset = outcome.SubAssets[index];
			AssetMetadata metadata;
			metadata.Handle = subAssetHandles[index];
			metadata.Type = subAsset.Type;
			metadata.Path = path;
			metadata.Parent = outcome.Handle;
			metadata.SubAssetKey = subAsset.Key;
			metadata.Name = subAsset.Name.empty() ? subAsset.Key : subAsset.Name;
			metadata.StoredSize = FileSystem::GetFileSize(GetCachePath(metadata.Handle)).value_or(0);
			RegisterAsset(metadata);
		}

		// Sub-assets the new import no longer produces disappear.
		for (AssetHandle previous : previousSubAssets)
		{
			if (std::find(subAssetHandles.begin(), subAssetHandles.end(), previous) != subAssetHandles.end())
				continue;
			UnregisterAsset(previous);
			std::unique_lock<std::shared_mutex> fileLock(m_FileMutex);
			FileSystem::Remove(GetCachePath(previous));
		}

		if (reload && !outcome.FromCache)
		{
			ReloadAsset(outcome.Handle);
			for (AssetHandle subAsset : subAssetHandles)
				ReloadAsset(subAsset);
		}
	}

	void EditorAssetManager::QueueImport(AssetHandle handle)
	{
		ImportRequest request;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_Sources.find(handle);
			if (it == m_Sources.end() || it->second.Importer->StoresSourceDirectly())
				return;
			if (it->second.ImportInFlight)
			{
				it->second.ImportQueuedAgain = true;
				return;
			}
			it->second.ImportInFlight = true;
			request = MakeImportRequest(it->second);
		}

		{
			std::scoped_lock<std::mutex> lock(m_ImportMutex);
			m_ImportsInFlight++;
		}
		auto import = [this, request]()
		{
			ImportOutcome outcome;
			try
			{
				if (!LoadCurrentImport(request, outcome))
					outcome = RunImport(request);
			}
			catch (const std::exception& exception)
			{
				outcome = ImportOutcome();
				outcome.Handle = request.Handle;
				outcome.Error = fmt::format("Import failed: {}", exception.what());
			}

			// Always completes, so WaitForImports and the destructor never wait forever.
			std::scoped_lock<std::mutex> lock(m_ImportMutex);
			m_ImportsInFlight--;
			try
			{
				m_CompletedImports.push_back(std::move(outcome));
			}
			catch (...)
			{
				ST_CORE_ERROR("Out of memory while completing the import of {}", request.Path);
			}
			m_ImportCondition.notify_all();
		};
		try
		{
			JobSystem::SubmitIO(import, JobPriority::Normal);
		}
		catch (const std::exception& exception)
		{
			ST_CORE_ERROR("Could not queue the import of '{}': {}", request.Path, exception.what());
			{
				std::scoped_lock<std::mutex> lock(m_SourceMutex);
				auto it = m_Sources.find(handle);
				if (it != m_Sources.end())
				{
					it->second.ImportInFlight = false;
					it->second.ImportQueuedAgain = false;
				}
			}
			std::scoped_lock<std::mutex> lock(m_ImportMutex);
			m_ImportsInFlight--;
			m_ImportCondition.notify_all();
		}
	}

	void EditorAssetManager::ProcessCompletedImports()
	{
		std::vector<ImportOutcome> completed;
		{
			std::scoped_lock<std::mutex> lock(m_ImportMutex);
			completed.swap(m_CompletedImports);
		}

		for (ImportOutcome& outcome : completed)
		{
			const AssetHandle handle = outcome.Handle;
			bool importAgain = false;
			{
				std::scoped_lock<std::mutex> lock(m_SourceMutex);
				auto it = m_Sources.find(handle);
				if (it != m_Sources.end())
				{
					importAgain = it->second.ImportQueuedAgain;
					it->second.ImportInFlight = false;
					it->second.ImportQueuedAgain = false;
				}
			}
			ApplyImportOutcome(std::move(outcome), true);
			if (importAgain)
				QueueImport(handle);
		}
	}

	void EditorAssetManager::WaitForImports()
	{
		while (true)
		{
			{
				std::unique_lock<std::mutex> lock(m_ImportMutex);
				m_ImportCondition.wait(lock, [this]() { return m_ImportsInFlight == 0; });
			}
			ProcessCompletedImports();

			std::scoped_lock<std::mutex> lock(m_ImportMutex);
			if (m_ImportsInFlight == 0 && m_CompletedImports.empty())
				return;
		}
	}

	AssetHandle EditorAssetManager::ResolveProjectFile(const std::filesystem::path& absolutePath) const
	{
		return FindAssetByAbsolutePath(absolutePath);
	}

	////////////////////////////////////////////////////////////////////////////////
	// Hot reload
	////////////////////////////////////////////////////////////////////////////////

	void EditorAssetManager::Update()
	{
		ST_PROFILE_FUNCTION();
		ProcessFileChanges();
		ProcessCompletedImports();
		SaveAssetIndex();
		AssetManagerBase::Update();
	}

	void EditorAssetManager::ProcessFileChanges()
	{
		if (!m_Watcher.IsRunning())
			return;

		// Removals last: a move arrives as a removal plus an addition, and the addition must find the asset at its old
		// place to keep its handle and cached data; a file replaced by a "safe save" exists again by then.
		std::vector<FileChange> changes = m_Watcher.PollChanges();
		std::stable_partition(changes.begin(), changes.end(), [](const FileChange& change) { return change.Type != FileChangeType::Removed; });

		for (const FileChange& change : changes)
		{
			const std::filesystem::path path = change.Path.lexically_normal();
			if (GetExtension(path) == c_MetaExtension)
			{
				std::filesystem::path sourcePath = path;
				sourcePath.replace_extension();
				const AssetHandle handle = FindAssetByAbsolutePath(sourcePath);
				if (!handle.IsValid())
				{
					if (change.Type != FileChangeType::Removed && FileSystem::IsRegularFile(sourcePath))
						QueueImport(RegisterSourceFile(sourcePath));
					continue;
				}

				std::optional<nlohmann::json> meta;
				if (change.Type != FileChangeType::Removed)
				{
					std::optional<std::string> text = FileSystem::ReadText(path);
					meta = text ? JsonUtils::Parse(*text) : std::nullopt;
					if (!meta || !meta->is_object())
						continue;
				}

				bool settingsChanged = false;
				{
					std::scoped_lock<std::mutex> lock(m_SourceMutex);
					auto it = m_Sources.find(handle);
					if (it == m_Sources.end())
						continue;
					if (!meta)
					{
						// The sidecar holds the handle other assets reference: restore it while the asset exists.
						if (FileSystem::IsRegularFile(sourcePath) && !WriteMetaFile(it->second))
							ST_CORE_ERROR("Could not restore '{}'", FileSystem::ToUTF8(path));
						continue;
					}

					const nlohmann::json* handleJson = JsonUtils::Find(*meta, "Handle");
					const std::optional<UUID> metaHandle = handleJson ? UUIDFromJson(*handleJson) : std::nullopt;
					if (metaHandle && *metaHandle != handle)
					{
						ST_CORE_WARN("The asset handle of '{}' changed on disk; restart the editor to apply it", it->second.Path);
						continue;
					}

					nlohmann::json settings = it->second.Importer->GetDefaultSettings(sourcePath);
					if (const nlohmann::json* importSettings = JsonUtils::Find(*meta, "ImportSettings"))
						MergeSettings(settings, *importSettings);
					settingsChanged = settings != it->second.Settings;
					if (settingsChanged)
						it->second.Settings = std::move(settings);
				}
				if (settingsChanged)
					QueueImport(handle);
				continue;
			}

			// Other files imports read (a model's external buffers and images) redo those imports. Imports whose cached
			// result is still current (the file was only touched) are not redone.
			for (AssetHandle dependent : GetDependents(path))
				QueueImport(dependent);

			if (!AssetImporterRegistry::FindByExtension(GetExtension(path)))
				continue;

			AssetHandle handle = FindAssetByAbsolutePath(path);
			if (change.Type == FileChangeType::Removed)
			{
				// The .meta and the cached import stay: the file may come back (checkout, an addition processed later)
				// or the .meta may follow it to a new place. The next scan removes cached data nothing uses.
				if (handle.IsValid() && !FileSystem::Exists(path))
				{
					UnregisterSource(handle, false);
					ST_CORE_INFO("Asset '{}' removed", ToRelative(path));
				}
				continue;
			}

			if (!FileSystem::IsRegularFile(path))
				continue;

			if (!handle.IsValid())
			{
				handle = RegisterSourceFile(path);
				if (handle.IsValid())
				{
					ST_CORE_INFO("Asset '{}' added", ToRelative(path));
					QueueImport(handle);
				}
				continue;
			}

			// Our own writes (saves, moves) were recorded already; anything else is an external modification.
			const uint64_t size = FileSystem::GetFileSize(path).value_or(0);
			const int64_t time = FileSystem::GetLastWriteTime(path).value_or(0);
			bool direct = false;
			{
				std::scoped_lock<std::mutex> lock(m_SourceMutex);
				auto it = m_Sources.find(handle);
				if (it == m_Sources.end() || (it->second.SourceSize == size && it->second.SourceTime == time))
					continue;
				direct = it->second.Importer->StoresSourceDirectly();
				if (direct)
				{
					it->second.SourceSize = size;
					it->second.SourceTime = time;
				}
			}

			ST_CORE_INFO("Asset '{}' changed; reloading", ToRelative(path));
			if (direct)
			{
				SetStoredSize(handle, size);
				ReloadAsset(handle);
			}
			else
			{
				QueueImport(handle);
			}
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Queries and file operations
	////////////////////////////////////////////////////////////////////////////////

	bool EditorAssetManager::ReadAssetData(const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		const AssetHandle sourceHandle = metadata.IsSubAsset() ? metadata.Parent : metadata.Handle;
		// The source lock is never held while waiting for the file lock (readers of large files would stall the main
		// thread). A file moved between looking up its path and reading it is read again at its new place.
		std::string sourcePath;
		bool direct = false;
		uint64_t pathGeneration = 0;
		for (int attempt = 0;; attempt++)
		{
			{
				std::scoped_lock<std::mutex> lock(m_SourceMutex);
				auto it = m_Sources.find(sourceHandle);
				if (it == m_Sources.end())
					return fail("The asset is not part of the project");
				if (!it->second.Import.Error.empty())
					return fail(fmt::format("Import failed: {}", it->second.Import.Error));
				if (attempt > 0 && it->second.PathGeneration == pathGeneration)
					break; // Not moved: the read failed for another reason
				sourcePath = it->second.Path;
				direct = it->second.Importer->StoresSourceDirectly();
				pathGeneration = it->second.PathGeneration;
			}

			std::optional<std::vector<uint8_t>> data;
			{
				std::shared_lock<std::shared_mutex> fileLock(m_FileMutex);
				data = FileSystem::ReadBytes(direct ? ToAbsolute(sourcePath) : GetCachePath(metadata.Handle));
			}
			if (data)
			{
				outData = std::move(*data);
				return true;
			}
			if (attempt == 2)
				break;
		}
		return fail(direct ? fmt::format("Could not read '{}'", sourcePath) : fmt::format("The imported data of '{}' is missing; re-import the asset", sourcePath));
	}

	std::filesystem::path EditorAssetManager::GetAbsolutePath(AssetHandle handle) const
	{
		std::optional<AssetMetadata> metadata = GetMetadata(handle);
		if (!metadata)
			return {};
		const AssetHandle sourceHandle = metadata->IsSubAsset() ? metadata->Parent : handle;

		std::scoped_lock<std::mutex> lock(m_SourceMutex);
		auto it = m_Sources.find(sourceHandle);
		return it != m_Sources.end() ? ToAbsolute(it->second.Path) : std::filesystem::path();
	}

	AssetHandle EditorAssetManager::FindAssetByAbsolutePath(const std::filesystem::path& path) const
	{
		const std::string relativePath = ToRelative(path.lexically_normal());
		if (relativePath.empty())
			return UUID::Null();

		std::scoped_lock<std::mutex> lock(m_SourceMutex);
		auto it = m_SourcesByPath.find(relativePath);
		return it != m_SourcesByPath.end() ? it->second : UUID::Null();
	}

	AssetImportInfo EditorAssetManager::GetImportInfo(AssetHandle handle) const
	{
		if (std::optional<AssetMetadata> metadata = GetMetadata(handle); metadata && metadata->IsSubAsset())
			handle = metadata->Parent;
		std::scoped_lock<std::mutex> lock(m_SourceMutex);
		auto it = m_Sources.find(handle);
		return it != m_Sources.end() ? it->second.Import : AssetImportInfo();
	}

	nlohmann::json EditorAssetManager::GetImportSettings(AssetHandle handle) const
	{
		std::scoped_lock<std::mutex> lock(m_SourceMutex);
		auto it = m_Sources.find(handle);
		return it != m_Sources.end() ? it->second.Settings : nlohmann::json();
	}

	bool EditorAssetManager::SetImportSettings(AssetHandle handle, const nlohmann::json& settings, std::string* outError)
	{
		if (!settings.is_object())
		{
			if (outError)
				*outError = "Import settings must be a JSON object";
			return false;
		}

		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_Sources.find(handle);
			if (it == m_Sources.end())
			{
				if (outError)
					*outError = "Not a source asset of the project";
				return false;
			}
			// An invalid .meta on disk is never overwritten: it may hold a merge conflict.
			const MetaContents meta = ReadMetaFile(GetMetaPath(ToAbsolute(it->second.Path)));
			if (meta.Status == MetaStatus::Invalid)
			{
				if (outError)
					*outError = fmt::format("The .meta file of '{}' is invalid ({}); fix or delete it first", it->second.Path, meta.Error);
				return false;
			}

			nlohmann::json previousSettings = it->second.Settings;
			MergeSettings(it->second.Settings, settings);
			if (!WriteMetaFile(it->second))
			{
				it->second.Settings = std::move(previousSettings);
				if (outError)
					*outError = fmt::format("Could not write the .meta file of '{}'", it->second.Path);
				return false;
			}
		}
		return ReimportAsset(handle, outError);
	}

	bool EditorAssetManager::ReimportAsset(AssetHandle handle, std::string* outError)
	{
		// Finish background imports first: their results would otherwise overwrite this one.
		WaitForImports();

		ImportRequest request;
		bool direct = false;
		uint64_t sourceSize = 0;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_Sources.find(handle);
			if (it == m_Sources.end())
			{
				if (outError)
					*outError = "Not a source asset of the project";
				return false;
			}
			direct = it->second.Importer->StoresSourceDirectly();
			if (direct)
			{
				const std::filesystem::path path = ToAbsolute(it->second.Path);
				it->second.SourceSize = FileSystem::GetFileSize(path).value_or(0);
				it->second.SourceTime = FileSystem::GetLastWriteTime(path).value_or(0);
				it->second.Import.Imported = true;
				it->second.Import.Error.clear();
				sourceSize = it->second.SourceSize;
			}
			else
			{
				request = MakeImportRequest(it->second);
			}
		}

		if (direct)
		{
			SetStoredSize(handle, sourceSize);
			ReloadAsset(handle);
			return true;
		}

		ImportOutcome outcome = RunImport(request);
		const bool success = outcome.Success;
		if (!success && outError)
			*outError = outcome.Error;
		ApplyImportOutcome(std::move(outcome), true);
		return success;
	}

	AssetHandle EditorAssetManager::ImportExternalFile(const std::filesystem::path& sourceFile, const std::string& targetDirectory, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return UUID::Null();
		};

		if (!FileSystem::IsRegularFile(sourceFile))
			return fail(fmt::format("'{}' is not a file", FileSystem::ToUTF8(sourceFile)));
		if (!AssetImporterRegistry::FindByExtension(GetExtension(sourceFile)))
			return fail(fmt::format("No importer handles '{}' files", FileSystem::ToUTF8(sourceFile.extension())));

		std::filesystem::path directory = ToAbsolute(targetDirectory);
		if (!directory.has_filename())
			directory = directory.parent_path(); // Trailing separator ("UI/") or the asset root ("")
		if (directory != m_Specification.AssetDirectory && ToRelative(directory).empty())
			return fail(fmt::format("'{}' is outside the asset directory", targetDirectory));

		const std::filesystem::path destination = FileSystem::GetUniquePath(directory / sourceFile.filename());
		if (!FileSystem::CreateDirectories(directory) || !FileSystem::Copy(sourceFile, destination, false))
			return fail(fmt::format("Could not copy '{}' into the project", FileSystem::ToUTF8(sourceFile)));

		const AssetHandle handle = RegisterSourceFile(destination);
		if (!handle.IsValid())
			return fail("Registering the copied file failed");

		std::string error;
		if (!ReimportAsset(handle, &error))
			ST_CORE_ERROR("Import of '{}' failed: {}", ToRelative(destination), error);
		return handle;
	}

	AssetHandle EditorAssetManager::CreateNativeAsset(const std::string& relativePath, std::span<const uint8_t> data, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return UUID::Null();
		};

		const std::filesystem::path path = ToAbsolute(relativePath);
		if (ToRelative(path).empty())
			return fail(fmt::format("'{}' is outside the asset directory", relativePath));
		const AssetImporter* importer = AssetImporterRegistry::FindByExtension(GetExtension(path));
		if (!importer || !importer->StoresSourceDirectly() || GetNativeAssetExtension(importer->GetType()).empty())
			return fail(fmt::format("'{}' is not an engine asset file (.stscene, .stprefab, .stmat)", relativePath));
		if (FileSystem::Exists(path))
			return fail(fmt::format("'{}' already exists", relativePath));

		const AssetLoadFunction* loader = AssetLoaderRegistry::Find(importer->GetType());
		std::string error;
		AssetLoadData document(data);
		if (!loader || !(*loader)(AssetMetadata { UUID::Null(), importer->GetType(), relativePath }, document, &error))
			return fail(fmt::format("Invalid {} data: {}", AssetTypeToString(importer->GetType()), error));

		if (!FileSystem::WriteBytes(path, data))
			return fail(fmt::format("Could not write '{}'", relativePath));

		const AssetHandle handle = RegisterSourceFile(path);
		if (!handle.IsValid())
			return fail(fmt::format("Registering '{}' failed", relativePath));

		std::scoped_lock<std::mutex> lock(m_SourceMutex);
		auto it = m_Sources.find(handle);
		if (it != m_Sources.end())
			it->second.Import.Imported = true;
		return handle;
	}

	bool EditorAssetManager::SaveNativeAsset(AssetHandle handle, std::span<const uint8_t> data, bool reload, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		AssetType type = AssetType::None;
		std::filesystem::path path;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_Sources.find(handle);
			if (it == m_Sources.end() || !it->second.Importer->StoresSourceDirectly())
				return fail("Not an engine asset of the project");
			type = it->second.Type;
			path = ToAbsolute(it->second.Path);
		}

		const AssetLoadFunction* loader = AssetLoaderRegistry::Find(type);
		std::string error;
		AssetLoadData document(data);
		if (!loader || !(*loader)(AssetMetadata { handle, type, ToRelative(path) }, document, &error))
			return fail(fmt::format("Invalid {} data: {}", AssetTypeToString(type), error));
		bool written = false;
		{
			std::unique_lock<std::shared_mutex> fileLock(m_FileMutex);
			written = FileSystem::WriteBytes(path, data);
		}
		if (!written)
			return fail(fmt::format("Could not write '{}'", FileSystem::ToUTF8(path)));

		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_Sources.find(handle);
			if (it != m_Sources.end())
			{
				it->second.SourceSize = FileSystem::GetFileSize(path).value_or(0);
				it->second.SourceTime = FileSystem::GetLastWriteTime(path).value_or(0);
				it->second.Import.Error.clear();
			}
		}
		SetStoredSize(handle, data.size());
		if (reload)
			ReloadAsset(handle);
		return true;
	}

	bool EditorAssetManager::MoveAsset(AssetHandle handle, const std::string& newRelativePath, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		// An import still reading the old path would fail (and record the failure): finish imports first.
		WaitForImports();

		const std::filesystem::path destination = ToAbsolute(newRelativePath);
		const std::string destinationRelative = ToRelative(destination);
		if (destinationRelative.empty())
			return fail(fmt::format("'{}' is outside the asset directory", newRelativePath));
		if (IsHiddenRelativePath(FileSystem::FromUTF8(destinationRelative)))
			return fail(fmt::format("'{}' is in a hidden directory", destinationRelative));
		if (FileSystem::Exists(destination))
			return fail(fmt::format("'{}' already exists", destinationRelative));

		std::unique_lock<std::mutex> lock(m_SourceMutex);
		auto it = m_Sources.find(handle);
		if (it == m_Sources.end())
			return fail("Not a source asset of the project");
		if (AssetImporterRegistry::FindByExtension(GetExtension(destination)) != it->second.Importer)
			return fail("Moving cannot change the file type");

		const std::filesystem::path source = ToAbsolute(it->second.Path);
		{
			std::unique_lock<std::shared_mutex> fileLock(m_FileMutex);
			if (!FileSystem::CreateDirectories(destination.parent_path()) || !FileSystem::Rename(source, destination))
				return fail(fmt::format("Could not move '{}' to '{}'", it->second.Path, destinationRelative));
			if (FileSystem::Exists(GetMetaPath(source)) && !FileSystem::Rename(GetMetaPath(source), GetMetaPath(destination)))
			{
				FileSystem::Rename(destination, source);
				return fail(fmt::format("Could not move the .meta file of '{}'", it->second.Path));
			}
		}

		m_SourcesByPath.erase(it->second.Path);
		it->second.Path = destinationRelative;
		it->second.PathGeneration++;
		m_SourcesByPath[destinationRelative] = handle;
		m_AssetIndexDirty = true;
		if (!FileSystem::Exists(GetMetaPath(destination)))
			WriteMetaFile(it->second);
		const std::vector<AssetHandle> subAssets = it->second.Import.SubAssets;
		lock.unlock();

		if (std::optional<AssetMetadata> metadata = GetMetadata(handle))
		{
			metadata->Path = destinationRelative;
			metadata->Name = FileSystem::ToUTF8(destination.stem());
			UpdateAssetMetadata(*metadata);
		}
		for (AssetHandle subAsset : subAssets)
		{
			if (std::optional<AssetMetadata> metadata = GetMetadata(subAsset))
			{
				metadata->Path = destinationRelative;
				UpdateAssetMetadata(*metadata);
			}
		}
		return true;
	}

	bool EditorAssetManager::BuildAssetPack(const std::filesystem::path& packPath, std::string* outError)
	{
		ST_PROFILE_FUNCTION();
		WaitForImports();

		std::vector<AssetMetadata> assets;
		for (AssetMetadata& metadata : GetAllMetadata())
		{
			if (!metadata.IsBuiltin() && !BuiltinAssets::IsBuiltin(metadata.Handle))
				assets.push_back(std::move(metadata));
		}

		return AssetPack::Write(packPath, assets, [this](const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string* outReadError)
		{
			return ReadAssetData(metadata, outData, outReadError);
		}, outError);
	}

	bool EditorAssetManager::DeleteAsset(AssetHandle handle, std::string* outError)
	{
		// A running import would write cache files after they were deleted.
		WaitForImports();

		std::filesystem::path path;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_Sources.find(handle);
			if (it == m_Sources.end())
			{
				if (outError)
					*outError = "Not a source asset of the project";
				return false;
			}
			path = ToAbsolute(it->second.Path);
		}

		{
			std::unique_lock<std::shared_mutex> fileLock(m_FileMutex);
			if (FileSystem::Exists(path) && !FileSystem::Remove(path))
			{
				if (outError)
					*outError = fmt::format("Could not delete '{}'", FileSystem::ToUTF8(path));
				return false;
			}
			FileSystem::Remove(GetMetaPath(path));
		}
		UnregisterSource(handle, true);
		return true;
	}

}
