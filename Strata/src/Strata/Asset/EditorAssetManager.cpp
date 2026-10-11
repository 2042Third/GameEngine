#include "stpch.h"
#include "Strata/Asset/EditorAssetManager.h"

#include "Strata/Asset/AssetDependencies.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/FileReader.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Hash.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/StringUtils.h"
#include "Strata/Reflection/PropertyJson.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_MetaVersion = 1;
		// 2: records hold what the assets reference and the bounds of meshes (AssetDependencies).
		constexpr uint32_t c_ImportRecordVersion = 2;

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

		// File systems keep times at a resolution (NTFS 100 ns, exFAT 10 ms, FAT and HFS+ 2 s): a file written again within it
		// keeps its time. A record must not vouch for a time so recent that the file may still change without changing it, or
		// an edit of the same size right after would pass for the recorded contents: such times are recorded as unknown (0),
		// so the next check compares contents and records the time once it is old enough (as git treats racily clean files).
		int64_t GetRecordableTime(int64_t time)
		{
			using Clock = std::filesystem::file_time_type::clock;
			constexpr auto c_TimeResolution = std::chrono::seconds(2);
			const int64_t now = Clock::now().time_since_epoch().count();
			const int64_t window = std::chrono::duration_cast<Clock::duration>(c_TimeResolution).count();
			return now - time < window ? 0 : time;
		}

		// Whether a file is in the state a record noted: same size and time, or only the time changed (a checkout, a copy)
		// but not the contents. A refreshed time is written into the state (outChanged), so contents are compared once (again
		// while the time is too recent to vouch for, see GetRecordableTime).
		bool IsFileCurrent(const std::filesystem::path& path, nlohmann::json& state, std::string_view sizeKey, std::string_view timeKey, std::string_view hashKey,
			bool& outChanged)
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
			state[std::string(timeKey)] = GetRecordableTime(time);
			outChanged = true;
			return true;
		}

		nlohmann::json ReferencesToJson(const std::vector<AssetHandle>& references)
		{
			nlohmann::json array = nlohmann::json::array();
			for (AssetHandle handle : references)
				array.push_back(UUIDToJson(handle));
			return array;
		}

		// nullopt when the record's list is missing or invalid (the record is then stale).
		std::optional<std::vector<AssetHandle>> ReferencesFromJson(const nlohmann::json& record)
		{
			const nlohmann::json* array = JsonUtils::Find(record, "References");
			if (!array || !array->is_array())
				return std::nullopt;
			std::vector<AssetHandle> references;
			for (const nlohmann::json& value : *array)
			{
				const std::optional<UUID> handle = UUIDFromJson(value);
				if (!handle)
					return std::nullopt;
				references.push_back(*handle);
			}
			return references;
		}

		nlohmann::json BoundsToJson(const std::optional<AABB>& bounds)
		{
			if (!bounds || !bounds->IsValid())
				return nullptr;
			return { { "Min", { bounds->Min.x, bounds->Min.y, bounds->Min.z } }, { "Max", { bounds->Max.x, bounds->Max.y, bounds->Max.z } } };
		}

		// False when the record's bounds are invalid (the record is then stale); no bounds (null or absent) is valid.
		bool BoundsFromJson(const nlohmann::json& record, std::optional<AABB>& outBounds)
		{
			outBounds.reset();
			const nlohmann::json* bounds = JsonUtils::Find(record, "Bounds");
			if (!bounds || bounds->is_null())
				return true;
			const nlohmann::json* minimum = JsonUtils::Find(*bounds, "Min");
			const nlohmann::json* maximum = JsonUtils::Find(*bounds, "Max");
			if (!minimum || !maximum || !minimum->is_array() || !maximum->is_array() || minimum->size() != 3 || maximum->size() != 3)
				return false;
			AABB box;
			for (int axis = 0; axis < 3; axis++)
			{
				if (!(*minimum)[axis].is_number() || !(*maximum)[axis].is_number())
					return false;
				box.Min[axis] = (*minimum)[axis].get<float>();
				box.Max[axis] = (*maximum)[axis].get<float>();
			}
			if (!box.IsValid())
				return false;
			outBounds = box;
			return true;
		}

		// Adds what a stored form references to what its importer recorded (AssetDependencies::Scan). Importers write valid
		// documents, so a scan that fails is reported as an import warning.
		void ScanReferences(AssetType type, std::span<const uint8_t> data, std::vector<AssetHandle>& references, std::vector<std::string>& warnings,
			std::string_view name)
		{
			std::string error;
			if (!AssetDependencies::Scan(type, data, references, &error))
				warnings.push_back(fmt::format("The references of '{}' could not be read: {}", name, error));
		}

		// "Prefabs\Enemy.stprefab" -> "Prefabs/Enemy.stprefab", "./Prefabs/" -> "Prefabs": AlwaysInclude entries as asset
		// paths are written.
		std::string NormalizeIncludePath(std::string path)
		{
			std::replace(path.begin(), path.end(), '\\', '/');
			while (path.rfind("./", 0) == 0)
				path.erase(0, 2);
			while (!path.empty() && path.back() == '/')
				path.pop_back();
			return path;
		}

		// Whether a normalized AlwaysInclude entry names the asset: its file, or a folder above it.
		bool IsIncludedByPath(const std::string& includePath, const AssetMetadata& metadata)
		{
			if (metadata.Path == includePath)
				return true;
			return !includePath.empty() && metadata.Path.size() > includePath.size() && metadata.Path.compare(0, includePath.size(), includePath) == 0 &&
				metadata.Path[includePath.size()] == '/';
		}

		std::string DescribeUnmatchedInclude(const std::string& entry)
		{
			return fmt::format("Export.AlwaysInclude names '{}', which is no asset or folder of the project", entry);
		}

		// The assets a pack can hold: every asset of the project but the built-in ones, by path.
		std::vector<AssetMetadata> GetPackableAssets(std::vector<AssetMetadata> all)
		{
			std::erase_if(all, [](const AssetMetadata& metadata) { return metadata.IsBuiltin() || BuiltinAssets::IsBuiltin(metadata.Handle); });
			return all;
		}

		bool IsCancelled(const std::atomic<bool>* cancel)
		{
			return cancel && cancel->load(std::memory_order_relaxed);
		}

	}

	// What the manager's background jobs use. A job keeps it alive (a Ref in its closure) and touches nothing else, so the
	// manager closes, and may be destroyed, while a cancelled job still runs to its next cancellation check. Everything here
	// is immutable or guarded by its own locks.
	struct EditorAssetManager::BackgroundWork
	{
		explicit BackgroundWork(const EditorAssetManagerSpecification& specification)
			: Specification(specification)
		{
		}

		const EditorAssetManagerSpecification Specification; // The manager's, with absolute directories

		// Files read on other threads (cache files, engine-native sources, the files imports read) are replaced, moved or
		// deleted by the main thread, and imports publish their cache files; on Windows a change fails while a reader holds
		// the file open. Reads take the lock shared, changes exclusive.
		mutable std::shared_mutex FileMutex;

		// The manager while it is open, for imports that resolve other files of the project (ImportContext::ResolveAsset).
		// Close clears it, waiting at most for a lookup in progress. Lock order: OwnerMutex before the manager's m_SourceMutex.
		mutable std::mutex OwnerMutex;
		const EditorAssetManager* Owner = nullptr;

		// The wanted jobs (queued or running and not cancelled), and what finished ones handed over for Update to apply.
		// Condition is notified whenever a job leaves Jobs.
		std::mutex Mutex;
		std::condition_variable Condition;
		std::unordered_set<Ref<BackgroundJob>> Jobs;
		std::vector<ImportOutcome> CompletedImports;
		std::vector<NativeReferences> CompletedReferences;

		// Numbers the files imports write before they publish them, so that two imports of an asset never write the same file:
		// a cancelled import may still run beside the one that replaced it.
		mutable std::atomic<uint64_t> NextStagingNumber = 0;

		std::filesystem::path ToAbsolute(std::string_view relativePath) const
		{
			return (Specification.AssetDirectory / FileSystem::FromUTF8(relativePath)).lexically_normal();
		}

		std::string ToRelative(const std::filesystem::path& absolutePath) const
		{
			const std::filesystem::path relative = FileSystem::GetRelativePath(absolutePath, Specification.AssetDirectory);
			return relative.empty() ? std::string() : FileSystem::ToUTF8(relative);
		}

		std::filesystem::path GetCachePath(AssetHandle handle) const
		{
			return Specification.CacheDirectory / (handle.ToString() + ".bin");
		}

		std::filesystem::path GetImportRecordPath(AssetHandle handle) const
		{
			return Specification.CacheDirectory / (handle.ToString() + ".import");
		}

		void Add(const Ref<BackgroundJob>& job)
		{
			std::scoped_lock<std::mutex> lock(Mutex);
			Jobs.insert(job);
		}

		// The job is no longer wanted: it never starts, or stops at its next cancellation check, and hands nothing over.
		void Cancel(const Ref<BackgroundJob>& job)
		{
			{
				std::scoped_lock<std::mutex> lock(Mutex);
				job->Cancel.store(true);
				Jobs.erase(job);
			}
			Condition.notify_all();
		}

		// A job ended: its result is handed over while the job is still wanted (a job is cancelled under the same lock, so
		// a result that arrives after its cancellation is never handed over).
		void Finish(const Ref<BackgroundJob>& job, ImportOutcome outcome)
		{
			const AssetHandle handle = outcome.Handle;
			{
				std::scoped_lock<std::mutex> lock(Mutex);
				if (Jobs.erase(job) > 0 && !outcome.Cancelled)
				{
					outcome.Job = job;
					try
					{
						CompletedImports.push_back(std::move(outcome));
					}
					catch (...)
					{
						ST_CORE_ERROR("Out of memory while completing the import of {}", handle.ToString());
					}
				}
			}
			Condition.notify_all();
		}

		void Finish(const Ref<BackgroundJob>& job, NativeReferences references)
		{
			const AssetHandle handle = references.Handle;
			{
				std::scoped_lock<std::mutex> lock(Mutex);
				if (Jobs.erase(job) > 0)
				{
					try
					{
						CompletedReferences.push_back(std::move(references));
					}
					catch (...)
					{
						ST_CORE_ERROR("Out of memory while recording the references of {}", handle.ToString());
					}
				}
			}
			Condition.notify_all();
		}

		// A file of the project read for an import (ImportContext::ReadFile), under the shared file lock. Nothing is read
		// once the import is cancelled: the file may be on its way to another place.
		std::optional<std::vector<uint8_t>> ReadProjectFile(const std::filesystem::path& path, const std::atomic<bool>* cancel) const
		{
			if (IsCancelled(cancel))
				return std::nullopt;
			std::shared_lock<std::shared_mutex> lock(FileMutex);
			return FileSystem::ReadBytes(path);
		}

		AssetHandle ResolveProjectFile(const std::filesystem::path& absolutePath) const
		{
			std::scoped_lock<std::mutex> lock(OwnerMutex);
			return Owner ? Owner->FindAssetByAbsolutePath(absolutePath) : UUID::Null();
		}

		// True when the cached import matches the source; fills the outcome (sub-assets, warnings) from the record. Reads under
		// the shared file lock; a refreshed record is written under the exclusive one, unless `cancel` is set by then (the
		// asset may be gone, with its record). Any thread.
		bool LoadCurrentImport(const ImportRequest& request, ImportOutcome& outOutcome, const std::atomic<bool>* cancel = nullptr) const;
		// Imports on the calling thread (any thread). Once `cancel` is set the importer stops at its next check and the
		// outcome is Cancelled: nothing is written. Otherwise the cache files and the record are published together under the
		// exclusive file lock, after a last look at `cancel`: whoever cancels an import before it changes files under that
		// lock (a move, a delete) never finds anything the import read before.
		ImportOutcome RunImport(const ImportRequest& request, const std::atomic<bool>* cancel = nullptr) const;
		// References of engine-native assets (AssetDependencies::Scan of the source), cached per content in the asset's import
		// record. Any thread: reads the record, or scans the file (`data` when given, else read under the shared file lock,
		// with its size and time; `references` when the caller knows them) and writes the record, or with deferRecord hands it
		// to the caller in NativeReferences::Record.
		NativeReferences FindNativeReferences(AssetHandle handle, AssetType type, const std::string& relativePath, std::span<const uint8_t> data = {},
			const std::vector<AssetHandle>* references = nullptr, bool deferRecord = false) const;
	};

	EditorAssetManager::EditorAssetManager(const EditorAssetManagerSpecification& specification)
		: m_Specification(specification)
	{
		// The file watcher reports absolute paths, so the directories must be absolute too.
		std::error_code error;
		m_Specification.AssetDirectory = std::filesystem::absolute(m_Specification.AssetDirectory, error).lexically_normal();
		m_Specification.CacheDirectory = std::filesystem::absolute(m_Specification.CacheDirectory, error).lexically_normal();
		FileSystem::CreateDirectories(m_Specification.AssetDirectory);
		FileSystem::CreateDirectories(m_Specification.CacheDirectory);
		m_Work = CreateRef<BackgroundWork>(m_Specification);
		m_Work->Owner = this;
	}

	EditorAssetManager::~EditorAssetManager()
	{
		// Background jobs only use m_Work, which they keep alive: nothing to wait for but the loads in flight.
		Close();
		WaitForInFlightLoads();
		SaveAssetIndex();
	}

	void EditorAssetManager::Close()
	{
		if (m_Closed)
			return;
		m_Closed = true;
		m_Watcher.Stop();

		uint32_t running = 0;
		uint32_t queued = 0;
		{
			std::scoped_lock<std::mutex> sourceLock(m_SourceMutex);
			for (auto& [handle, source] : m_Sources)
				source.ImportJob.reset();
			m_SourcesImporting = 0;

			// Every wanted job: the sources' imports and the searches of references. Results not applied yet go too.
			std::scoped_lock<std::mutex> lock(m_Work->Mutex);
			for (const Ref<BackgroundJob>& job : m_Work->Jobs)
			{
				job->Cancel.store(true);
				(job->Started.load() ? running : queued)++;
			}
			m_Work->Jobs.clear();
			m_Work->CompletedImports.clear();
			m_Work->CompletedReferences.clear();
		}
		m_Work->Condition.notify_all();
		{
			std::scoped_lock<std::mutex> lock(m_Work->OwnerMutex);
			m_Work->Owner = nullptr;
		}

		if (running + queued > 0)
		{
			ST_CORE_INFO("Cancelled the background imports and searches of '{}': {} running, {} queued (imports that did not finish run again when "
				"the project opens)", FileSystem::ToUTF8(m_Specification.AssetDirectory), running, queued);
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Paths
	////////////////////////////////////////////////////////////////////////////////

	std::filesystem::path EditorAssetManager::ToAbsolute(std::string_view relativePath) const
	{
		return m_Work->ToAbsolute(relativePath);
	}

	std::string EditorAssetManager::ToRelative(const std::filesystem::path& absolutePath) const
	{
		return m_Work->ToRelative(absolutePath);
	}

	std::filesystem::path EditorAssetManager::GetCachePath(AssetHandle handle) const
	{
		return m_Work->GetCachePath(handle);
	}

	std::filesystem::path EditorAssetManager::GetImportRecordPath(AssetHandle handle) const
	{
		return m_Work->GetImportRecordPath(handle);
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
			// A removed source is not imported: its import is cancelled before its cached data goes, so it stores nothing.
			StopImport(it->second);
			m_SourcesByPath.erase(it->second.Path);
			m_Sources.erase(it);
			m_AssetIndexDirty = true;
		}

		for (AssetHandle subAsset : subAssets)
			UnregisterAsset(subAsset);
		UnregisterAsset(handle);

		if (deleteCache)
		{
			std::unique_lock<std::shared_mutex> fileLock(m_Work->FileMutex);
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

	void EditorAssetManager::Scan(ScanImports imports)
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

		// Current imports are taken from the cache at once. The others import here (in parallel, on the background threads), or
		// in the background after the scan: then the scan only checks the cache, on the workers like other work the frame
		// waits for.
		const bool wait = imports == ScanImports::Wait;
		std::vector<ImportOutcome> outcomes(requests.size());
		std::vector<uint8_t> done(requests.size(), 0); // Not a vector<bool>: batches write their elements concurrently
		std::atomic<uint32_t> importedCount = 0;
		JobSystem::ParallelFor(static_cast<uint32_t>(requests.size()), 1, [&](uint32_t begin, uint32_t endIndex)
		{
			for (uint32_t requestIndex = begin; requestIndex < endIndex; requestIndex++)
			{
				try
				{
					if (m_Work->LoadCurrentImport(requests[requestIndex], outcomes[requestIndex]))
					{
						done[requestIndex] = 1;
					}
					else if (wait)
					{
						outcomes[requestIndex] = m_Work->RunImport(requests[requestIndex]);
						importedCount++;
						done[requestIndex] = 1;
					}
				}
				catch (const std::exception& exception)
				{
					outcomes[requestIndex] = ImportOutcome();
					outcomes[requestIndex].Handle = requests[requestIndex].Handle;
					outcomes[requestIndex].Error = fmt::format("Import failed: {}", exception.what());
					done[requestIndex] = 1;
				}
			}
		}, wait ? JobPriority::Background : JobPriority::High);

		uint32_t failedCount = 0;
		std::vector<AssetHandle> backgroundImports;
		for (size_t requestIndex = 0; requestIndex < requests.size(); requestIndex++)
		{
			if (!done[requestIndex])
			{
				backgroundImports.push_back(requests[requestIndex].Handle);
				continue;
			}
			failedCount += outcomes[requestIndex].Success ? 0 : 1;
			ApplyImportOutcome(std::move(outcomes[requestIndex]), false);
		}

		// What the engine-native assets reference: from their records where the files did not change, else scanned.
		struct NativeSource
		{
			AssetHandle Handle;
			AssetType Type = AssetType::None;
			std::string Path;
		};
		std::vector<NativeSource> natives;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			for (const auto& [handle, source] : m_Sources)
			{
				if (source.Importer->StoresSourceDirectly() && AssetDependencies::CanReference(source.Type))
					natives.push_back(NativeSource { handle, source.Type, source.Path });
			}
		}
		std::vector<NativeReferences> nativeReferences(natives.size());
		JobSystem::ParallelFor(static_cast<uint32_t>(natives.size()), 4, [&](uint32_t begin, uint32_t endIndex)
		{
			for (uint32_t index = begin; index < endIndex; index++)
				nativeReferences[index] = m_Work->FindNativeReferences(natives[index].Handle, natives[index].Type, natives[index].Path);
		});
		for (const NativeReferences& references : nativeReferences)
			ApplyNativeReferences(references);
		RemoveUnusedCacheFiles();
		// After the cleanup, which takes the temporary files of interrupted imports for leftovers.
		for (const AssetHandle handle : backgroundImports)
			QueueImport(handle);

		if (m_Specification.WatchFiles && !m_Watcher.IsRunning() && !m_Watcher.Start(m_Specification.AssetDirectory))
			ST_CORE_WARN("Asset hot reload is unavailable: cannot watch '{}'", FileSystem::ToUTF8(m_Specification.AssetDirectory));

		SaveAssetIndex();
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime);
		if (backgroundImports.empty())
			ST_CORE_INFO("Asset scan: {} assets, {} imported, {} failed ({} ms)", files.size(), importedCount.load(), failedCount, elapsed.count());
		else
			ST_CORE_INFO("Asset scan: {} assets, {} importing in the background, {} failed ({} ms)", files.size(), backgroundImports.size(), failedCount,
				elapsed.count());
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
		// Cache files are named "<handle>.<kind>[.<temporary suffix>]". No import of this manager runs during a scan, so
		// temporary files are leftovers of interrupted writes, or of a cancelled import of an earlier manager of the project
		// that is still running: that one never publishes them (and removes them itself where this removal fails).
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

		std::unique_lock<std::shared_mutex> fileLock(m_Work->FileMutex);
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

	bool EditorAssetManager::BackgroundWork::LoadCurrentImport(const ImportRequest& request, ImportOutcome& outOutcome, const std::atomic<bool>* cancel) const
	{
		if (IsCancelled(cancel))
			return false;
		const std::filesystem::path recordPath = GetImportRecordPath(request.Handle);
		// The source and its dependencies may be hashed: read under the shared file lock, like every read of project files
		// off the main thread (a move or delete of a file an open reader holds fails on Windows).
		std::shared_lock<std::shared_mutex> fileLock(FileMutex);
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

		// Refreshed times are written back so the contents are compared once (IsFileCurrent).
		bool recordChanged = false;
		const std::filesystem::path sourcePath = ToAbsolute(request.Path);
		if (!IsFileCurrent(sourcePath, *record, "SourceSize", "SourceTime", "SourceHash", recordChanged))
			return false;

		ImportOutcome outcome;
		outcome.Handle = request.Handle;
		outcome.Success = true;
		outcome.FromCache = true;
		outcome.SourceSize = JsonUtils::GetUInt(*record, "SourceSize", 0);
		// The file's own time (the record may hold none for a time too recent to vouch for): the file is current.
		outcome.SourceTime = FileSystem::GetLastWriteTime(sourcePath).value_or(0);

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
				if (exists != JsonUtils::GetBool(dependency, "Exists", false) || (exists && !IsFileCurrent(dependencyPath, dependency, "Size", "Time", "Hash", recordChanged)))
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
		std::optional<std::vector<AssetHandle>> references = ReferencesFromJson(*record);
		if (!references || !BoundsFromJson(*record, outcome.Bounds))
			return false;
		outcome.References = std::move(*references);
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
				std::optional<std::vector<AssetHandle>> subReferences = ReferencesFromJson(subAsset);
				if (!subReferences || !BoundsFromJson(subAsset, entry.Bounds))
					return false;
				entry.References = std::move(*subReferences);
				if (!FileSystem::IsRegularFile(GetCachePath(DeriveSubAssetHandle(request.Handle, entry.Key))))
					return false;
				outcome.SubAssets.push_back(std::move(entry));
			}
		}

		fileLock.unlock();

		if (recordChanged)
		{
			// Like an import's publication: under the exclusive lock, and not once the import is cancelled (its asset may be
			// gone, with the record it had).
			std::unique_lock<std::shared_mutex> writeLock(FileMutex);
			if (!IsCancelled(cancel))
				FileSystem::WriteText(recordPath, JsonUtils::Dump(*record, 1, '\t'));
		}
		outOutcome = std::move(outcome);
		return true;
	}

	EditorAssetManager::ImportOutcome EditorAssetManager::BackgroundWork::RunImport(const ImportRequest& request, const std::atomic<bool>* cancel) const
	{
		ST_PROFILE_FUNCTION();
		const std::filesystem::path sourcePath = ToAbsolute(request.Path);
		auto stopped = [&request]()
		{
			ImportOutcome outcome;
			outcome.Handle = request.Handle;
			outcome.Cancelled = true;
			outcome.Error = "The import was cancelled";
			return outcome;
		};

		// Large sources import one at a time: decoded data is many times the file size. A cancelled import stops waiting.
		static std::timed_mutex s_LargeImportMutex;
		std::unique_lock<std::timed_mutex> largeImportLock(s_LargeImportMutex, std::defer_lock);
		if (FileSystem::GetFileSize(sourcePath).value_or(0) > c_LargeImportSize)
		{
			while (!largeImportLock.try_lock_for(std::chrono::milliseconds(10)))
			{
				if (IsCancelled(cancel))
					return stopped();
			}
		}
		if (IsCancelled(cancel))
			return stopped();

		ImportOutcome outcome;
		outcome.Handle = request.Handle;
		// The file state before importing: a change during the import is then still detected as a modification.
		std::optional<uint64_t> sourceHash;
		{
			std::shared_lock<std::shared_mutex> fileLock(FileMutex);
			outcome.SourceSize = FileSystem::GetFileSize(sourcePath).value_or(0);
			outcome.SourceTime = FileSystem::GetLastWriteTime(sourcePath).value_or(0);
			sourceHash = HashFile(sourcePath);
		}

		ImportContext context;
		context.Handle = request.Handle;
		context.SourcePath = sourcePath;
		context.AssetDirectory = Specification.AssetDirectory;
		context.Settings = request.Settings;
		context.ResolveAsset = [this](const std::filesystem::path& path) { return ResolveProjectFile(path); };
		context.ReadFile = [this, cancel](const std::filesystem::path& path) { return ReadProjectFile(path, cancel); };
		context.Cancel = cancel;

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
		if (largeImportLock.owns_lock())
			largeImportLock.unlock();
		// Whatever the importer returned: it may have failed because it was cancelled, or read a file that moved meanwhile.
		if (IsCancelled(cancel))
			return stopped();
		if (outcome.Success)
		{
			// What the stored forms reference: what the importer recorded, and what their documents show.
			ScanReferences(request.Importer->GetType(), result.Data, result.References, result.Warnings, request.Path);
			for (ImportedSubAsset& subAsset : result.SubAssets)
				ScanReferences(subAsset.Type, subAsset.Data, subAsset.References, result.Warnings, fmt::format("{} ({})", request.Path, subAsset.Key));
			outcome.References = std::move(result.References);
			outcome.Bounds = result.Bounds;
		}
		outcome.Warnings = std::move(result.Warnings);

		// Dependencies count for failed imports too: creating or fixing a missing file must retry the import.
		nlohmann::json dependencies = nlohmann::json::array();
		for (const ImportDependency& dependency : result.Dependencies)
		{
			const std::string relativePath = ToRelative(dependency.Path);
			if (relativePath.empty() || relativePath == request.Path)
				continue;
			dependencies.push_back({ { "Path", relativePath }, { "Exists", dependency.Exists }, { "Size", dependency.Size },
				{ "Time", GetRecordableTime(dependency.Time) }, { "Hash", HashToString(dependency.Hash) } });
			outcome.Dependencies.push_back(relativePath);
		}

		// Cache files are written under temporary names first: only replacing them takes the file lock, so loads are not
		// blocked while a large import writes. The staged files and the record are published together under the exclusive
		// lock, after a last look at the cancellation flag: a move or delete sets it before it changes files under that lock,
		// so an import it cancelled never stores what it read, and nothing of a cancelled import is left.
		std::vector<std::pair<std::filesystem::path, std::filesystem::path>> stagedFiles;
		auto removeStaged = [&stagedFiles]()
		{
			for (const auto& [stagedPath, cachePath] : stagedFiles)
				FileSystem::Remove(stagedPath);
		};
		bool staged = true;
		if (!outcome.Success)
		{
			outcome.Error = error.empty() ? std::string("Import failed") : error;
		}
		else
		{
			const uint64_t stagingNumber = NextStagingNumber.fetch_add(1);
			auto stage = [&](AssetHandle handle, std::vector<uint8_t>& data)
			{
				// Nothing more is written once the import is cancelled.
				bool written = false;
				if (!IsCancelled(cancel))
				{
					std::filesystem::path stagedPath = GetCachePath(handle);
					stagedPath += fmt::format(".staged{}", stagingNumber);
					written = FileSystem::WriteBytes(stagedPath, data);
					stagedFiles.emplace_back(stagedPath, GetCachePath(handle));
				}
				std::vector<uint8_t>().swap(data); // Free the memory now: imports can be large
				return written;
			};
			staged = stage(request.Handle, result.Data);
			for (ImportedSubAsset& subAsset : result.SubAssets)
				staged = stage(DeriveSubAssetHandle(request.Handle, subAsset.Key), subAsset.Data) && staged; // Frees every sub-asset's data
			outcome.SubAssets = std::move(result.SubAssets);
		}

		std::unique_lock<std::shared_mutex> fileLock(FileMutex);
		if (IsCancelled(cancel))
		{
			fileLock.unlock();
			removeStaged();
			return stopped();
		}
		if (outcome.Success)
		{
			bool written = staged;
			for (const auto& [stagedPath, cachePath] : stagedFiles)
				written = written && FileSystem::Rename(stagedPath, cachePath);
			if (!written)
			{
				removeStaged();
				outcome.Success = false;
				outcome.Error = fmt::format("Could not write imported data to '{}'", FileSystem::ToUTF8(Specification.CacheDirectory));
			}
		}

		nlohmann::json record = nlohmann::json::object();
		record["Format"] = "ImportRecord";
		record["Version"] = c_ImportRecordVersion;
		record["Source"] = request.Path;
		record["ImporterVersion"] = request.Importer->GetVersion();
		record["SettingsHash"] = HashToString(HashSettings(request.Settings));
		record["SourceSize"] = outcome.SourceSize;
		record["SourceTime"] = GetRecordableTime(outcome.SourceTime);
		record["SourceHash"] = sourceHash ? HashToString(*sourceHash) : std::string();
		record["Success"] = outcome.Success;
		record["Error"] = outcome.Error;
		record["Warnings"] = outcome.Warnings;
		record["References"] = ReferencesToJson(outcome.References);
		record["Bounds"] = BoundsToJson(outcome.Bounds);
		nlohmann::json subAssets = nlohmann::json::array();
		for (const ImportedSubAsset& subAsset : outcome.SubAssets)
		{
			subAssets.push_back({ { "Key", subAsset.Key }, { "Name", subAsset.Name }, { "Type", AssetTypeToString(subAsset.Type) },
				{ "References", ReferencesToJson(subAsset.References) }, { "Bounds", BoundsToJson(subAsset.Bounds) } });
		}
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
		// Loads of the asset and its sub-assets that waited for a first import (IsAwaitingImport) go on once it is applied:
		// with its data, or failing with its error.
		std::vector<AssetHandle> awaited = { outcome.Handle };
		bool reported = false; // A caller waits for this outcome and reports its failure
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_Sources.find(outcome.Handle);
			if (it == m_Sources.end())
				return; // Deleted while importing

			SourceAsset& source = it->second;
			reported = std::exchange(source.ImportAwaited, false);
			awaited.insert(awaited.end(), source.Import.SubAssets.begin(), source.Import.SubAssets.end());
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
			if (!reported)
			{
				m_ImportFailures.push_back({ outcome.Handle, path, outcome.Error });
				if (m_ImportFailures.size() > c_MaxImportFailures)
					m_ImportFailures.pop_front();
				m_ImportFailureCount++;
			}
			ResumeLoadsAwaitingImport(awaited);
			return;
		}
		awaited.insert(awaited.end(), subAssetHandles.begin(), subAssetHandles.end());

		SetStoredSize(outcome.Handle, FileSystem::GetFileSize(GetCachePath(outcome.Handle)).value_or(0));
		SetAssetReferences(outcome.Handle, outcome.References, outcome.Bounds);
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
			SetAssetReferences(metadata.Handle, subAsset.References, subAsset.Bounds);
		}

		// Sub-assets the new import no longer produces disappear.
		for (AssetHandle previous : previousSubAssets)
		{
			if (std::find(subAssetHandles.begin(), subAssetHandles.end(), previous) != subAssetHandles.end())
				continue;
			UnregisterAsset(previous);
			std::unique_lock<std::shared_mutex> fileLock(m_Work->FileMutex);
			FileSystem::Remove(GetCachePath(previous));
		}

		if (reload && !outcome.FromCache)
		{
			ReloadAsset(outcome.Handle);
			for (AssetHandle subAsset : subAssetHandles)
				ReloadAsset(subAsset);
		}
		// Reloads restart waiting loads themselves; these are the others (an import found current, or applied without reload).
		ResumeLoadsAwaitingImport(awaited);
	}

	void EditorAssetManager::QueueImport(AssetHandle handle, bool force, bool replacing)
	{
		if (m_Closed)
			return;
		const Ref<BackgroundJob> job = CreateRef<BackgroundJob>();
		ImportRequest request;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_Sources.find(handle);
			if (it == m_Sources.end() || it->second.Importer->StoresSourceDirectly())
				return;
			// A queued or running import read an earlier state of the file, its settings or its place: this one replaces it
			// at once, rather than after it (its work would be thrown away).
			const Ref<BackgroundJob> replaced = StopImport(it->second);
			if (replaced)
				force = force || replaced->Force;
			job->Force = force;
			it->second.ImportJob = job;
			m_SourcesImporting++;
			request = MakeImportRequest(it->second);
			request.Force = force;
			request.Replaced = replacing || replaced != nullptr;
			m_Work->Add(job);
		}
		SubmitImport(job, request);
	}

	void EditorAssetManager::SubmitImport(const Ref<BackgroundJob>& job, const ImportRequest& request)
	{
		// The job uses only the work it shares with the manager, so that the manager never waits for it (see BackgroundWork).
		auto import = [work = m_Work, job, request]()
		{
			if (job->Cancel.load())
				return; // Replaced, or the manager closed, before it started
			job->Started.store(true);
			ImportOutcome outcome;
			try
			{
				if (request.Force || !work->LoadCurrentImport(request, outcome, &job->Cancel))
					outcome = work->RunImport(request, &job->Cancel);
				else if (request.Replaced)
					outcome.FromCache = false; // The current cache may hold what the replaced import stored and nothing loaded
			}
			catch (const std::exception& exception)
			{
				outcome = ImportOutcome();
				outcome.Handle = request.Handle;
				outcome.Error = fmt::format("Import failed: {}", exception.what());
			}
			// Always ends the job, so WaitForImports never waits forever; the result goes to Update unless it is out of date.
			work->Finish(job, std::move(outcome));
		};
		try
		{
			// On the background threads: imports decode and compress for seconds, which must neither take the processors from
			// the frame nor hold the I/O threads that stream assets.
			JobSystem::Submit(std::move(import), JobPriority::Background);
		}
		catch (const std::exception& exception)
		{
			ST_CORE_ERROR("Could not queue the import of '{}': {}", request.Path, exception.what());
			bool neverImported = false;
			{
				std::scoped_lock<std::mutex> lock(m_SourceMutex);
				auto it = m_Sources.find(request.Handle);
				if (it != m_Sources.end() && it->second.ImportJob == job)
				{
					StopImport(it->second);
					neverImported = !it->second.Import.Imported;
				}
			}
			// A first import that never runs fails, so that loads waiting for it end (a re-import keeps the data it has).
			if (neverImported)
			{
				ImportOutcome failed;
				failed.Handle = request.Handle;
				failed.Error = fmt::format("Could not queue the import: {}", exception.what());
				ApplyImportOutcome(std::move(failed), false);
			}
		}
	}

	Ref<EditorAssetManager::BackgroundJob> EditorAssetManager::StopImport(SourceAsset& source)
	{
		Ref<BackgroundJob> job = std::exchange(source.ImportJob, nullptr);
		if (!job)
			return nullptr;
		m_SourcesImporting--;
		m_Work->Cancel(job);
		return job;
	}

	void EditorAssetManager::ProcessCompletedImports()
	{
		std::vector<ImportOutcome> completed;
		std::vector<NativeReferences> references;
		{
			std::scoped_lock<std::mutex> lock(m_Work->Mutex);
			completed.swap(m_Work->CompletedImports);
			references.swap(m_Work->CompletedReferences);
		}

		// What engine-native assets reference, unless a later search or a save started meanwhile (its result follows): the
		// record is written here, on the main thread like a save's, so a search that a save overtook never replaces the save's.
		for (const NativeReferences& found : references)
		{
			bool current = false;
			{
				std::scoped_lock<std::mutex> lock(m_SourceMutex);
				auto it = m_Sources.find(found.Handle);
				current = it != m_Sources.end() && it->second.ReferencesGeneration == found.Generation;
			}
			if (!current)
				continue;
			ApplyNativeReferences(found);
			if (found.Record && !FileSystem::WriteText(GetImportRecordPath(found.Handle), JsonUtils::Dump(*found.Record, 1, '\t')))
				ST_CORE_WARN("Could not write the import record of '{}'", JsonUtils::GetString(*found.Record, "Source", ""));
		}

		for (ImportOutcome& outcome : completed)
		{
			{
				std::scoped_lock<std::mutex> lock(m_SourceMutex);
				auto it = m_Sources.find(outcome.Handle);
				// The source went, or another import replaced this one (which finished before it was cancelled): its result is
				// out of date, and the import that replaced it brings the current one.
				if (it == m_Sources.end() || it->second.ImportJob != outcome.Job)
					continue;
				it->second.ImportJob.reset();
				m_SourcesImporting--;
			}
			ApplyImportOutcome(std::move(outcome), true);
		}
	}

	void EditorAssetManager::WaitForImports()
	{
		while (true)
		{
			{
				std::unique_lock<std::mutex> lock(m_Work->Mutex);
				m_Work->Condition.wait(lock, [this]() { return m_Work->Jobs.empty(); });
			}
			ProcessCompletedImports();

			std::scoped_lock<std::mutex> lock(m_Work->Mutex);
			if (m_Work->Jobs.empty() && m_Work->CompletedImports.empty() && m_Work->CompletedReferences.empty())
				return;
		}
	}

	EditorAssetManager::NativeReferences EditorAssetManager::BackgroundWork::FindNativeReferences(AssetHandle handle, AssetType type, const std::string& relativePath,
		std::span<const uint8_t> data, const std::vector<AssetHandle>* references, bool deferRecord) const
	{
		NativeReferences result;
		result.Handle = handle;
		if (!AssetDependencies::CanReference(type))
		{
			result.Scanned = true;
			return result;
		}

		const std::filesystem::path sourcePath = ToAbsolute(relativePath);
		const std::filesystem::path recordPath = GetImportRecordPath(handle);
		try
		{
			// The record, when the file is what it was when the record was written. The file is hashed under the shared file lock:
			// a save replaces it under the exclusive one, which an open reader would make fail on Windows.
			if (data.empty())
			{
				const std::optional<std::string> text = FileSystem::ReadText(recordPath);
				std::optional<nlohmann::json> record = text ? JsonUtils::Parse(*text) : std::nullopt;
				bool recordChanged = false;
				bool current = false;
				if (record && record->is_object() && JsonUtils::GetUInt(*record, "Version", 0) == c_ImportRecordVersion && JsonUtils::GetBool(*record, "Native", false))
				{
					std::shared_lock<std::shared_mutex> fileLock(FileMutex);
					current = IsFileCurrent(sourcePath, *record, "SourceSize", "SourceTime", "SourceHash", recordChanged);
				}
				if (current)
				{
					if (std::optional<std::vector<AssetHandle>> recorded = ReferencesFromJson(*record))
					{
						if (recordChanged && deferRecord)
							result.Record = std::move(*record);
						else if (recordChanged)
							FileSystem::WriteText(recordPath, JsonUtils::Dump(*record, 1, '\t'));
						result.References = std::move(*recorded);
						result.Scanned = true;
						return result;
					}
				}
			}

			// The time the record keeps is the one of the bytes scanned: read with them, under the lock that keeps saves out.
			std::optional<std::vector<uint8_t>> file;
			int64_t sourceTime = 0;
			if (data.empty())
			{
				std::shared_lock<std::shared_mutex> fileLock(FileMutex);
				file = FileSystem::ReadBytes(sourcePath);
				if (!file)
					return result;
				sourceTime = FileSystem::GetLastWriteTime(sourcePath).value_or(0);
				data = *file;
			}
			else
			{
				sourceTime = FileSystem::GetLastWriteTime(sourcePath).value_or(0);
			}
			std::string error;
			if (references)
			{
				result.References = *references;
			}
			else if (!AssetDependencies::Scan(type, data, result.References, &error))
			{
				ST_CORE_WARN("The references of '{}' could not be read: {}", relativePath, error);
				return result;
			}
			result.Scanned = true;

			nlohmann::json record = nlohmann::json::object();
			record["Format"] = "ImportRecord";
			record["Version"] = c_ImportRecordVersion;
			record["Native"] = true; // Stored as its source: the record only caches what it references
			record["Source"] = relativePath;
			record["SourceSize"] = data.size();
			record["SourceTime"] = GetRecordableTime(sourceTime);
			record["SourceHash"] = HashToString(Hash::FNV1a(data));
			record["References"] = ReferencesToJson(result.References);
			if (deferRecord)
				result.Record = std::move(record);
			else if (!FileSystem::WriteText(recordPath, JsonUtils::Dump(record, 1, '\t')))
				ST_CORE_WARN("Could not write the import record of '{}'", relativePath);
		}
		catch (const std::exception& exception)
		{
			ST_CORE_ERROR("Finding the references of '{}' failed: {}", relativePath, exception.what());
			result = NativeReferences { handle, {}, false, 0 };
		}
		return result;
	}

	void EditorAssetManager::ApplyNativeReferences(const NativeReferences& references)
	{
		if (references.Scanned)
			SetAssetReferences(references.Handle, references.References, std::nullopt);
	}

	void EditorAssetManager::RefreshNativeReferences(AssetHandle handle, std::span<const uint8_t> data, const std::vector<AssetHandle>* references)
	{
		AssetType type = AssetType::None;
		std::string path;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_Sources.find(handle);
			if (it == m_Sources.end() || !it->second.Importer->StoresSourceDirectly())
				return;
			type = it->second.Type;
			path = it->second.Path;
			it->second.ReferencesGeneration++; // A search queued before finds the previous contents
		}
		ApplyNativeReferences(m_Work->FindNativeReferences(handle, type, path, data, references));
	}

	void EditorAssetManager::QueueNativeReferences(AssetHandle handle)
	{
		if (m_Closed)
			return;
		AssetType type = AssetType::None;
		std::string path;
		uint64_t generation = 0;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_Sources.find(handle);
			if (it == m_Sources.end() || !it->second.Importer->StoresSourceDirectly() || !AssetDependencies::CanReference(it->second.Type))
				return;
			type = it->second.Type;
			path = it->second.Path;
			generation = ++it->second.ReferencesGeneration;
		}

		// A job like an import: WaitForImports and HasPendingImports count it, Close cancels it.
		const Ref<BackgroundJob> job = CreateRef<BackgroundJob>();
		m_Work->Add(job);
		auto search = [work = m_Work, job, handle, type, path, generation]()
		{
			if (job->Cancel.load())
				return;
			job->Started.store(true);
			// Its record is written by Update, and only while this is the latest search (FindNativeReferences catches what it
			// may throw).
			NativeReferences references = work->FindNativeReferences(handle, type, path, {}, nullptr, true);
			references.Generation = generation;
			work->Finish(job, std::move(references));
		};
		try
		{
			JobSystem::SubmitIO(std::move(search), JobPriority::Normal);
		}
		catch (const std::exception& exception)
		{
			ST_CORE_ERROR("Could not queue the search of what '{}' references: {}", path, exception.what());
			m_Work->Cancel(job);
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Hot reload
	////////////////////////////////////////////////////////////////////////////////

	void EditorAssetManager::Update()
	{
		ST_PROFILE_FUNCTION();
		ST_PROFILE_STAGE("Assets.Update");
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
					QueueNativeReferences(handle);
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
				QueueNativeReferences(handle);
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

	bool EditorAssetManager::ReadStoredFile(const AssetMetadata& metadata, const std::function<bool(const std::filesystem::path& path)>& read, std::string* outError)
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

			bool success = false;
			{
				std::shared_lock<std::shared_mutex> fileLock(m_Work->FileMutex);
				success = read(direct ? ToAbsolute(sourcePath) : GetCachePath(metadata.Handle));
			}
			if (success)
				return true;
			if (attempt == 2)
				break;
		}
		return fail(direct ? fmt::format("Could not read '{}'", sourcePath) : fmt::format("The imported data of '{}' is missing; re-import the asset", sourcePath));
	}

	bool EditorAssetManager::ReadAssetData(const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string* outError)
	{
		return ReadStoredFile(metadata, [&outData](const std::filesystem::path& path)
		{
			std::optional<std::vector<uint8_t>> data = FileSystem::ReadBytes(path);
			if (!data)
				return false;
			outData = std::move(*data);
			return true;
		}, outError);
	}

	bool EditorAssetManager::ReadAssetRange(const AssetMetadata& metadata, uint64_t offset, uint64_t size, std::vector<uint8_t>& outData, std::string* outError)
	{
		bool outside = false;
		uint64_t fileSize = 0;
		const bool read = ReadStoredFile(metadata, [&](const std::filesystem::path& path)
		{
			const Scope<FileReader> reader = FileReader::Open(path);
			if (!reader)
				return false;
			fileSize = reader->GetSize();
			if (offset > fileSize || size > fileSize - offset)
			{
				outside = true;
				return true; // Read, but the range is wrong: reported below
			}
			outData.resize(static_cast<size_t>(size));
			return reader->ReadAt(offset, outData);
		}, outError);
		if (read && outside)
		{
			if (outError)
				*outError = fmt::format("The range {}+{} lies outside the {} bytes of '{}'", offset, size, fileSize, metadata.Path.empty() ? metadata.Name : metadata.Path);
			return false;
		}
		return read;
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

	const AssetImporter* EditorAssetManager::GetImporter(AssetHandle handle) const
	{
		if (std::optional<AssetMetadata> metadata = GetMetadata(handle); metadata && metadata->IsSubAsset())
			handle = metadata->Parent;
		std::scoped_lock<std::mutex> lock(m_SourceMutex);
		auto it = m_Sources.find(handle);
		return it != m_Sources.end() ? it->second.Importer : nullptr;
	}

	bool EditorAssetManager::WriteImportSettings(AssetHandle handle, const nlohmann::json& settings, std::string* outError)
	{
		if (!settings.is_object())
		{
			if (outError)
				*outError = "Import settings must be a JSON object";
			return false;
		}

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
		return true;
	}

	bool EditorAssetManager::SetImportSettings(AssetHandle handle, const nlohmann::json& settings, std::string* outError)
	{
		return WriteImportSettings(handle, settings, outError) && ReimportAsset(handle, outError);
	}

	bool EditorAssetManager::SetImportSettingsInBackground(AssetHandle handle, const nlohmann::json& settings, std::string* outError)
	{
		return WriteImportSettings(handle, settings, outError) && QueueBackgroundImport(handle, false, outError);
	}

	void EditorAssetManager::ReloadDirectSource(AssetHandle handle)
	{
		uint64_t sourceSize = 0;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_Sources.find(handle);
			if (it == m_Sources.end())
				return;
			const std::filesystem::path path = ToAbsolute(it->second.Path);
			it->second.SourceSize = FileSystem::GetFileSize(path).value_or(0);
			it->second.SourceTime = FileSystem::GetLastWriteTime(path).value_or(0);
			it->second.Import.Imported = true;
			it->second.Import.Error.clear();
			sourceSize = it->second.SourceSize;
		}
		SetStoredSize(handle, sourceSize);
		QueueNativeReferences(handle);
		ReloadAsset(handle);
	}

	bool EditorAssetManager::ReimportAssetInBackground(AssetHandle handle, std::string* outError)
	{
		return QueueBackgroundImport(handle, true, outError);
	}

	bool EditorAssetManager::QueueBackgroundImport(AssetHandle handle, bool force, std::string* outError)
	{
		if (m_Closed)
		{
			if (outError)
				*outError = "The project's assets are closed: nothing is imported any more";
			return false;
		}
		bool direct = false;
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
			// The caller waits for this import (IsImportPending) and reports how it went.
			if (!direct)
				it->second.ImportAwaited = true;
		}
		if (direct)
			ReloadDirectSource(handle);
		else
			QueueImport(handle, force);
		return true;
	}

	bool EditorAssetManager::IsAwaitingImport(const AssetMetadata& metadata) const
	{
		const AssetHandle sourceHandle = metadata.IsSubAsset() ? metadata.Parent : metadata.Handle;
		std::scoped_lock<std::mutex> lock(m_SourceMutex);
		auto it = m_Sources.find(sourceHandle);
		return it != m_Sources.end() && !it->second.Importer->StoresSourceDirectly() && !it->second.Import.Imported;
	}

	uint32_t EditorAssetManager::GetPendingImportCount() const
	{
		std::scoped_lock<std::mutex> lock(m_SourceMutex);
		return m_SourcesImporting;
	}

	std::vector<AssetImportFailure> EditorAssetManager::TakeImportFailures(uint64_t* outCount)
	{
		if (outCount)
			*outCount = m_ImportFailureCount;
		std::vector<AssetImportFailure> failures(std::make_move_iterator(m_ImportFailures.begin()), std::make_move_iterator(m_ImportFailures.end()));
		m_ImportFailures.clear();
		m_ImportFailureCount = 0;
		return failures;
	}

	bool EditorAssetManager::IsImportPending(AssetHandle handle) const
	{
		if (std::optional<AssetMetadata> metadata = GetMetadata(handle); metadata && metadata->IsSubAsset())
			handle = metadata->Parent;
		std::scoped_lock<std::mutex> lock(m_SourceMutex);
		auto it = m_Sources.find(handle);
		return it != m_Sources.end() && it->second.ImportJob != nullptr;
	}

	bool EditorAssetManager::ReimportAsset(AssetHandle handle, std::string* outError)
	{
		ImportRequest request;
		bool direct = false;
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
			if (!direct)
			{
				// A background import of the asset read an earlier state, and its result must not follow this one: it is
				// cancelled before this import starts, so it stores nothing once this one could have published. Other imports go
				// on, and nothing waits for them.
				StopImport(it->second);
				request = MakeImportRequest(it->second);
				// The caller gets the outcome (outError).
				it->second.ImportAwaited = true;
			}
		}

		if (direct)
		{
			ReloadDirectSource(handle);
			return true;
		}

		ImportOutcome outcome = m_Work->RunImport(request);
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

	AssetHandle EditorAssetManager::CreateNativeAsset(const std::string& relativePath, std::span<const uint8_t> data, std::string* outError,
		const std::vector<AssetHandle>* references)
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

		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_Sources.find(handle);
			if (it != m_Sources.end())
				it->second.Import.Imported = true;
		}
		RefreshNativeReferences(handle, data, references);
		return handle;
	}

	bool EditorAssetManager::SaveNativeAsset(AssetHandle handle, std::span<const uint8_t> data, bool reload, std::string* outError,
		const std::vector<AssetHandle>* references)
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
			std::unique_lock<std::shared_mutex> fileLock(m_Work->FileMutex);
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
		RefreshNativeReferences(handle, data, references);
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

		// An import of the asset reads the file at its old place: it is cancelled before the file moves, so it stores nothing
		// it read there, and imports again once the move is done (at the old place if the move fails). Other imports go on.
		const Ref<BackgroundJob> stopped = StopImport(it->second);
		const std::filesystem::path source = ToAbsolute(it->second.Path);
		std::string failure;
		{
			std::unique_lock<std::shared_mutex> fileLock(m_Work->FileMutex);
			if (!FileSystem::CreateDirectories(destination.parent_path()) || !FileSystem::Rename(source, destination))
			{
				failure = fmt::format("Could not move '{}' to '{}'", it->second.Path, destinationRelative);
			}
			else if (FileSystem::Exists(GetMetaPath(source)) && !FileSystem::Rename(GetMetaPath(source), GetMetaPath(destination)))
			{
				FileSystem::Rename(destination, source);
				failure = fmt::format("Could not move the .meta file of '{}'", it->second.Path);
			}
		}

		if (failure.empty())
		{
			m_SourcesByPath.erase(it->second.Path);
			it->second.Path = destinationRelative;
			it->second.PathGeneration++;
			m_SourcesByPath[destinationRelative] = handle;
			m_AssetIndexDirty = true;
			if (!FileSystem::Exists(GetMetaPath(destination)))
				WriteMetaFile(it->second);
		}
		const std::vector<AssetHandle> subAssets = it->second.Import.SubAssets;
		lock.unlock();

		if (stopped)
			QueueImport(handle, stopped->Force, true);
		if (!failure.empty())
			return fail(std::move(failure));

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

	bool EditorAssetManager::HasPendingImports() const
	{
		std::scoped_lock<std::mutex> lock(m_Work->Mutex);
		return !m_Work->Jobs.empty() || !m_Work->CompletedImports.empty() || !m_Work->CompletedReferences.empty();
	}

	bool EditorAssetManager::PlanAssetPack(const AssetPackBuildOptions& options, AssetPackBuildPlan& outPlan, std::string* outError) const
	{
		ST_PROFILE_FUNCTION();
		m_PackPlans.fetch_add(1, std::memory_order_relaxed);
		const std::vector<AssetMetadata> all = GetPackableAssets(GetAllMetadata());
		std::unordered_map<AssetHandle, size_t> indices;
		for (size_t index = 0; index < all.size(); index++)
			indices.emplace(all[index].Handle, index);

		// First use: each root's dependency closure, breadth first, appended where it is first reached.
		std::vector<size_t> order;
		std::vector<bool> included(all.size(), false);
		auto addClosure = [&](AssetHandle root)
		{
			for (AssetHandle member : GetDependencyClosure(root))
			{
				auto it = indices.find(member);
				if (it != indices.end() && !included[it->second])
				{
					included[it->second] = true;
					order.push_back(it->second);
				}
			}
		};
		if (options.StartScene.IsValid())
			addClosure(options.StartScene);
		for (const AssetMetadata& metadata : all)
		{
			if (metadata.Type == AssetType::Scene && !metadata.IsSubAsset())
				addClosure(metadata.Handle);
		}

		// What AlwaysInclude names: assets by path (with their sub-assets) and folders.
		for (const std::string& entry : options.AlwaysInclude)
		{
			const std::string path = NormalizeIncludePath(entry);
			bool matched = false;
			for (const AssetMetadata& metadata : all)
			{
				if (IsIncludedByPath(path, metadata))
				{
					matched = true;
					addClosure(metadata.Handle);
				}
			}
			if (!matched && options.Prune)
			{
				if (outError)
					*outError = DescribeUnmatchedInclude(entry);
				return false;
			}
		}

		AssetPackBuildPlan plan;
		for (size_t index = 0; index < all.size(); index++)
		{
			if (included[index])
				continue;
			if (options.Prune)
				plan.Excluded.push_back(all[index]);
			else
				order.push_back(index);
		}
		plan.Assets.reserve(order.size());
		for (size_t index : order)
			plan.Assets.push_back(AssetPackSource { all[index], GetDependencies(all[index].Handle), GetBounds(all[index].Handle) });
		outPlan = std::move(plan);
		return true;
	}

	bool EditorAssetManager::CheckAlwaysInclude(const std::vector<std::string>& alwaysInclude, std::string* outError) const
	{
		ST_PROFILE_FUNCTION();
		if (alwaysInclude.empty())
			return true;
		const std::vector<AssetMetadata> all = GetPackableAssets(GetAllMetadata());
		for (const std::string& entry : alwaysInclude)
		{
			const std::string path = NormalizeIncludePath(entry);
			const bool matched = std::any_of(all.begin(), all.end(), [&path](const AssetMetadata& metadata) { return IsIncludedByPath(path, metadata); });
			if (!matched)
			{
				if (outError)
					*outError = DescribeUnmatchedInclude(entry);
				return false;
			}
		}
		return true;
	}

	bool EditorAssetManager::WriteAssetPack(const std::filesystem::path& packPath, const AssetPackBuildPlan& plan, const AssetPackWriteOptions& options,
		AssetPackWriteStats* outStats, std::string* outError)
	{
		// The plan is a snapshot, the files are not: the editor saves documents and imports files while a pack is written on
		// another thread. A document whose bytes reference what the plan did not record would ship naming an asset the pack
		// may lack, with a dependency table that does not match it: the write fails instead, naming the document. (Changes
		// that keep what a document references, and changes of other stored forms, ship as they are read.)
		std::unordered_map<AssetHandle, const AssetPackSource*> planned;
		for (const AssetPackSource& source : plan.Assets)
			planned.emplace(source.Metadata.Handle, &source);
		auto describe = [this](AssetHandle handle)
		{
			const std::optional<AssetMetadata> metadata = GetMetadata(handle);
			return metadata && !metadata->Path.empty() ? fmt::format("'{}'", metadata->Path) : fmt::format("asset {}", handle.ToString());
		};
		return AssetPackWriter::Write(packPath, plan.Assets, [this, &planned, &describe](const AssetMetadata& metadata, std::vector<uint8_t>& outData,
			std::string* outReadError)
		{
			if (!ReadAssetData(metadata, outData, outReadError))
				return false;
			auto fail = [outReadError](std::string message)
			{
				if (outReadError)
					*outReadError = std::move(message);
				return false;
			};
			const auto source = planned.find(metadata.Handle);
			if (!AssetDependencies::CanReference(metadata.Type) || source == planned.end())
				return true;

			std::vector<AssetHandle> references;
			std::string scanError;
			if (!AssetDependencies::Scan(metadata.Type, outData, references, &scanError))
				return fail(fmt::format("it cannot be read as a document ({}); fix it and export again", scanError));
			// Compared as recorded references are made: a document that names itself (a scene whose script loads it again)
			// depends on nothing for that.
			const std::unordered_set<AssetHandle> recorded(source->second->Dependencies.begin(), source->second->Dependencies.end());
			for (AssetHandle reference : NormalizeReferences(metadata.Handle, references))
			{
				if (!recorded.contains(reference))
					return fail(fmt::format("it changed after the export started and now references {}, which the export did not plan for; export again", describe(reference)));
			}
			return true;
		}, options, outStats, outError);
	}

	bool EditorAssetManager::BuildAssetPack(const std::filesystem::path& packPath, std::string* outError, const AssetPackBuildOptions& options,
		AssetPackBuildPlan* outPlan, AssetPackWriteStats* outStats)
	{
		ST_PROFILE_FUNCTION();
		WaitForImports();
		AssetPackBuildPlan plan;
		if (!PlanAssetPack(options, plan, outError))
			return false;
		if (!WriteAssetPack(packPath, plan, {}, outStats, outError))
			return false;
		if (outPlan)
			*outPlan = std::move(plan);
		return true;
	}

	bool EditorAssetManager::DeleteAsset(AssetHandle handle, std::string* outError)
	{
		std::filesystem::path path;
		Ref<BackgroundJob> stopped;
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
			// Its import is cancelled before the files go, so it stores nothing afterwards (a running one would write cache
			// files after they were deleted). Other imports go on.
			stopped = StopImport(it->second);
		}

		bool removed = true;
		{
			std::unique_lock<std::shared_mutex> fileLock(m_Work->FileMutex);
			removed = !FileSystem::Exists(path) || FileSystem::Remove(path);
			if (removed)
				FileSystem::Remove(GetMetaPath(path));
		}
		if (!removed)
		{
			// The asset stays: so does its import.
			if (stopped)
				QueueImport(handle, stopped->Force, true);
			if (outError)
				*outError = fmt::format("Could not delete '{}'", FileSystem::ToUTF8(path));
			return false;
		}
		UnregisterSource(handle, true);
		return true;
	}

}
