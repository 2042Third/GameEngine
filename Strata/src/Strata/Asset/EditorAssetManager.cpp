#include "stpch.h"
#include "Strata/Asset/EditorAssetManager.h"

#include "Strata/Asset/ImportRecordStore.h"

#include "Strata/Asset/AssetDependencies.h"
#include "Strata/Asset/AssetPackCook.h"
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
#include <system_error>
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

		// The import record of an asset stored as its source: it only caches what the asset references, for as long as the
		// file has this size, time and content hash.
		nlohmann::json MakeNativeRecord(const std::string& relativePath, uint64_t size, int64_t time, uint64_t contentHash,
			const std::vector<AssetHandle>& references)
		{
			nlohmann::json record = nlohmann::json::object();
			record["Format"] = "ImportRecord";
			record["Version"] = c_ImportRecordVersion;
			record["Native"] = true;
			record["Source"] = relativePath;
			record["SourceSize"] = size;
			record["SourceTime"] = GetRecordableTime(time);
			record["SourceHash"] = HashToString(contentHash);
			record["References"] = ReferencesToJson(references);
			return record;
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

		// "Models/Crate/Crate.gltf" -> "Models/Crate"; "" for files at the root.
		std::string GetParentFolder(std::string_view path)
		{
			const size_t slash = path.rfind('/');
			return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
		}

		// Whether a relative path is the folder or lies below it ("" holds everything).
		bool IsInFolder(std::string_view path, std::string_view folder)
		{
			if (folder.empty())
				return true;
			return path == folder || (path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 && path[folder.size()] == '/');
		}

		// A path below `from` moved to below `to`.
		std::string ReplaceFolder(const std::string& path, const std::string& from, const std::string& to)
		{
			return to + path.substr(from.size());
		}

		// The name of a new trash folder: the UTC time, sortable and readable ("20261011-120305").
		std::string MakeTrashTimestamp()
		{
			const auto now = std::chrono::system_clock::now();
			const auto days = std::chrono::floor<std::chrono::days>(now);
			const std::chrono::year_month_day date(days);
			const std::chrono::hh_mm_ss time(std::chrono::floor<std::chrono::seconds>(now - days));
			return fmt::format("{:04}{:02}{:02}-{:02}{:02}{:02}", static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
				static_cast<unsigned>(date.day()), time.hours().count(), time.minutes().count(), time.seconds().count());
		}

		constexpr const char* c_TrashAssetsFolder = "Assets";
		constexpr const char* c_TrashCacheFolder = "Cache";
		constexpr const char* c_TrashManifest = "Trashed.json";
		// A trashed asset's import record, beside its cache files in the trash folder.
		constexpr std::string_view c_TrashRecordExtension = ".import";

		// How many folders above a file the files it reads (a model's "../Textures") may lie when it is copied into a project.
		constexpr int c_MaxExternalFileClimb = 2;

		// Copies files from outside the project into the asset directory (EditorAssetManager::ImportExternal, on an I/O job,
		// and ImportExternalFile): records every file it writes in the item and every folder it creates, so that a failure or
		// a cancellation can take them back. Uses no manager state. `claim` (optional) hears every file's relative path before
		// the file is created, so that the manager's file watcher leaves files that are still being written alone.
		class ExternalCopy
		{
		public:
			ExternalCopy(const std::filesystem::path& assetDirectory, const std::atomic<bool>* cancel, std::vector<std::string>& createdFolders,
				std::function<void(const std::string& relativePath)> claim = {})
				: m_AssetDirectory(assetDirectory), m_Cancel(cancel), m_CreatedFolders(createdFolders), m_Claim(std::move(claim))
			{
			}

			bool IsCancelled() const { return Strata::IsCancelled(m_Cancel); }

			// A file and the files its importer reads beside it.
			ExternalImportItem CopyExternalFile(const std::filesystem::path& file, const std::filesystem::path& targetDirectory)
			{
				ExternalImportItem item;
				item.Source = file;
				const AssetImporter* importer = AssetImporterRegistry::FindByExtension(GetExtension(file));
				if (!FileSystem::IsRegularFile(file))
				{
					item.Error = fmt::format("'{}' is not a file", FileSystem::ToUTF8(file));
					return item;
				}
				if (!importer)
				{
					item.Error = fmt::format("No importer handles '{}' files", FileSystem::ToUTF8(file.extension()));
					return item;
				}

				// The files it reads, and the folder their layout is kept below (the nearest one holding all of them).
				std::vector<std::filesystem::path> companions;
				std::filesystem::path root = file.parent_path();
				if (importer->ReadsExternalFiles())
				{
					const std::optional<std::vector<uint8_t>> data = FileSystem::ReadBytes(file);
					std::vector<std::string> names;
					std::string error;
					if (!data)
						error = fmt::format("Could not read '{}'", FileSystem::ToUTF8(file));
					else if (!importer->FindExternalFiles(*data, names, &error))
						error = fmt::format("{}: {}", FileSystem::ToUTF8(file.filename()), error);
					if (!error.empty())
					{
						item.Error = std::move(error);
						return item;
					}
					for (const std::string& name : names)
					{
						const std::filesystem::path companion = (file.parent_path() / FileSystem::FromUTF8(name)).lexically_normal();
						int climbed = 0;
						while (FileSystem::GetRelativePath(companion, root).empty() && climbed <= c_MaxExternalFileClimb)
						{
							root = root.parent_path();
							climbed++;
						}
						if (climbed > c_MaxExternalFileClimb)
						{
							item.Error = fmt::format("{}: '{}' lies more than {} folders above it; copy them together into one folder first",
								FileSystem::ToUTF8(file.filename()), name, c_MaxExternalFileClimb);
							return item;
						}
						// Missing files are left to the import, which says what it lacks.
						if (FileSystem::IsRegularFile(companion))
							companions.push_back(companion);
					}
				}

				std::filesystem::path destination;
				if (companions.empty())
				{
					destination = FileSystem::GetUniquePath(targetDirectory / file.filename());
				}
				else
				{
					// A new folder named after it, with everything below the root at the same relative place.
					const std::filesystem::path folder = FileSystem::GetUniquePath(targetDirectory / file.stem());
					destination = folder / FileSystem::GetRelativePath(file, root);
					for (const std::filesystem::path& companion : companions)
					{
						if (!Copy(companion, folder / FileSystem::GetRelativePath(companion, root), item))
							return item;
					}
				}
				if (!Copy(file, destination, item))
					return item;
				// The file itself first: ImportExternal registers it before the files it reads.
				std::rotate(item.Files.begin(), item.Files.end() - 1, item.Files.end());
				item.Path = Relative(destination);
				return item;
			}

			// A folder with its asset files, their .meta files and the files models in it read inside it.
			ExternalImportItem CopyExternalFolder(const std::filesystem::path& directory, const std::filesystem::path& targetDirectory)
			{
				ExternalImportItem item;
				item.Source = directory;
				item.Folder = true;
				if (!FileSystem::IsDirectory(directory))
				{
					item.Error = fmt::format("'{}' is not a folder", FileSystem::ToUTF8(directory));
					return item;
				}
				const std::filesystem::path source = FileSystem::RemoveTrailingSeparators(directory);
				const std::filesystem::path folder = FileSystem::GetUniquePath(targetDirectory / source.filename());

				std::vector<std::filesystem::path> files;
				std::error_code error;
				std::filesystem::recursive_directory_iterator iterator(source, std::filesystem::directory_options::skip_permission_denied, error);
				const std::filesystem::recursive_directory_iterator end;
				while (!error && iterator != end)
				{
					const std::filesystem::directory_entry& entry = *iterator;
					std::error_code typeError;
					if (IsHiddenName(entry.path().filename()) || entry.is_symlink(typeError))
					{
						if (entry.is_directory(typeError))
							iterator.disable_recursion_pending();
					}
					else if (entry.is_regular_file(typeError) && AssetImporterRegistry::FindByExtension(GetExtension(entry.path())))
					{
						files.push_back(entry.path());
					}
					iterator.increment(error);
				}
				if (error)
				{
					item.Error = fmt::format("Could not read the folder '{}': {}", FileSystem::ToUTF8(source), error.message());
					return item;
				}
				std::sort(files.begin(), files.end());

				std::unordered_set<std::string> copied;
				auto copyBelow = [&](const std::filesystem::path& file)
				{
					const std::filesystem::path relative = FileSystem::GetRelativePath(file, source);
					if (relative.empty() || !copied.insert(FileSystem::ToUTF8(relative)).second)
						return true;
					return Copy(file, folder / relative, item);
				};
				for (const std::filesystem::path& file : files)
				{
					if (IsCancelled())
						return item;
					if (!copyBelow(file) || (FileSystem::IsRegularFile(GetMetaPath(file)) && !copyBelow(GetMetaPath(file))))
						return item;
					const AssetImporter* importer = AssetImporterRegistry::FindByExtension(GetExtension(file));
					if (!importer->ReadsExternalFiles())
						continue;
					// What a model reads inside the folder comes along; what lies outside it does not (its import says so).
					const std::optional<std::vector<uint8_t>> data = FileSystem::ReadBytes(file);
					std::vector<std::string> names;
					if (!data || !importer->FindExternalFiles(*data, names, nullptr))
						continue;
					for (const std::string& name : names)
					{
						const std::filesystem::path companion = (file.parent_path() / FileSystem::FromUTF8(name)).lexically_normal();
						if (FileSystem::IsRegularFile(companion) && !FileSystem::GetRelativePath(companion, source).empty() && !copyBelow(companion))
							return item;
					}
				}
				item.Path = Relative(folder);
				if (!FileSystem::IsDirectory(folder) && !MakeFolder(folder))
					item.Error = fmt::format("Could not create the folder '{}'", item.Path);
				return item;
			}
		private:
			std::string Relative(const std::filesystem::path& absolute) const
			{
				return FileSystem::ToUTF8(FileSystem::GetRelativePath(absolute, m_AssetDirectory));
			}

			// Creates a folder and the parents it lacks, recording those it made (parents first).
			bool MakeFolder(const std::filesystem::path& absolute)
			{
				std::vector<std::filesystem::path> missing;
				for (std::filesystem::path path = absolute.lexically_normal(); !FileSystem::Exists(path) && path != m_AssetDirectory && path.has_relative_path();
					path = path.parent_path())
					missing.push_back(path);
				for (auto it = missing.rbegin(); it != missing.rend(); ++it)
				{
					if (!FileSystem::CreateDirectories(*it))
						return false;
					m_CreatedFolders.push_back(Relative(*it));
				}
				return true;
			}

			bool Copy(const std::filesystem::path& from, const std::filesystem::path& to, ExternalImportItem& item)
			{
				if (IsCancelled())
				{
					item.Error = "The import was cancelled";
					return false;
				}
				if (!MakeFolder(to.parent_path()))
				{
					item.Error = fmt::format("Could not copy '{}' into the project", FileSystem::ToUTF8(from));
					return false;
				}
				const std::string relative = Relative(to);
				if (m_Claim)
					m_Claim(relative);
				// Never over a file that was created since its name was picked; what a copy that failed otherwise left is no
				// file of the project.
				std::error_code error;
				if (!std::filesystem::copy_file(from, to, std::filesystem::copy_options::none, error) || error)
				{
					if (error != std::errc::file_exists)
					{
						std::error_code removeError;
						std::filesystem::remove(to, removeError);
					}
					// System messages come in the system's code page.
					item.Error = fmt::format("Could not copy '{}' into the project: {}", FileSystem::ToUTF8(from),
						error ? StringUtils::MakeValidUTF8(error.message()) : std::string("the copy failed"));
					return false;
				}
				item.Files.push_back(relative);
				return true;
			}
		private:
			std::filesystem::path m_AssetDirectory;
			const std::atomic<bool>* m_Cancel;
			std::vector<std::string>& m_CreatedFolders;
			std::function<void(const std::string& relativePath)> m_Claim;
		};

	}

	bool ExternalImport::HasFailures() const
	{
		return std::any_of(m_Items.begin(), m_Items.end(), [](const ExternalImportItem& item) { return !item.Error.empty(); });
	}

	// What the manager's background jobs use. A job keeps it alive (a Ref in its closure) and touches nothing else, so the
	// manager closes, and may be destroyed, while a cancelled job still runs to its next cancellation check. Everything here
	// is immutable or guarded by its own locks.
	struct EditorAssetManager::BackgroundWork
	{
		explicit BackgroundWork(const EditorAssetManagerSpecification& specification)
			: Specification(specification), Records(specification.CacheDirectory)
		{
		}

		const EditorAssetManagerSpecification Specification; // The manager's, with absolute directories

		// Every asset's import record and the path of every handle (ImportRecordStore: one append-only file of the cache). A
		// record is set where its cache files were published, under the exclusive file lock, so that an import cancelled by a
		// removal never brings its record back.
		mutable ImportRecordStore Records;

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

		// Documents written in the background (DocumentWriteClaim), by relative path, counted: the file watcher leaves them
		// alone until their writes are published.
		mutable std::mutex ClaimMutex;
		std::unordered_map<std::string, uint32_t> ClaimedPaths;

		void Claim(const std::string& relativePath)
		{
			std::scoped_lock<std::mutex> lock(ClaimMutex);
			ClaimedPaths[relativePath]++;
		}

		void Unclaim(const std::string& relativePath)
		{
			std::scoped_lock<std::mutex> lock(ClaimMutex);
			auto it = ClaimedPaths.find(relativePath);
			if (it != ClaimedPaths.end() && --it->second == 0)
				ClaimedPaths.erase(it);
		}

		bool IsClaimed(const std::string& relativePath) const
		{
			std::scoped_lock<std::mutex> lock(ClaimMutex);
			return ClaimedPaths.contains(relativePath);
		}

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

		// Whether an asset of the open manager has the handle (a closed one has none). Any thread.
		bool IsHandleTaken(AssetHandle handle) const
		{
			std::scoped_lock<std::mutex> lock(OwnerMutex);
			return Owner && Owner->IsHandleValid(handle);
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
		// Deletes cached data and import records of assets that are not `valid` and leftovers of interrupted writes (a scan's
		// job, before any import of the manager runs).
		void RemoveUnusedCacheFiles(const std::unordered_set<AssetHandle>& valid);
	};

	// A source file as a scan job prepared it: everything its registration needs, read from the disk off the main thread.
	struct EditorAssetManager::PreparedSource
	{
		std::filesystem::path Absolute;
		std::string Path; // Relative, '/' separators
		const AssetImporter* Importer = nullptr;
		AssetHandle Handle = UUID::Null();
		nlohmann::json Settings = nlohmann::json::object();
		uint64_t SourceSize = 0;
		int64_t SourceTime = 0;
		bool Skip = false; // No importer, or an invalid .meta (never overwritten: it may hold a merge conflict)
		bool NewHandle = false; // Its .meta was missing, or holds a handle another file keeps
		// Of an imported asset: its cached import, when it is current.
		std::optional<ImportOutcome> Current;
		// Of an asset stored as its source: what it references (from its record, or scanned).
		std::optional<NativeReferences> References;
		bool Inserted = false; // Registered by InsertPrepared (Handle is its handle then)
	};

	struct ExternalImport::CopyTask
	{
		std::atomic<bool> Cancel = false;
		std::mutex Mutex;
		bool Done = false; // Guarded by Mutex, like what follows
		std::vector<ExternalImportItem> Items;
		std::vector<std::string> CreatedFolders;
		// Every file the copy writes (relative to the asset directory), named before it is created: the file watcher leaves
		// them alone until the import registers them, so it never imports a file that is still being written (a large copy
		// can look settled to it).
		std::unordered_set<std::string> Claimed;
		// The asset files it copied, prepared for registration by the copy job (the main thread only inserts them).
		std::vector<EditorAssetManager::PreparedSource> Prepared;
	};

	// What the scan job shares with the manager, which never waits for it: the job writes its results, then sets Finished.
	struct EditorAssetManager::ScanShared
	{
		std::atomic<bool> Cancel = false;
		std::atomic<uint8_t> Phase = static_cast<uint8_t>(AssetScanPhase::Walking);
		std::atomic<uint64_t> Done = 0;  // Walking: files found; Applying: files inserted
		std::atomic<uint64_t> Total = 0;
		// Registering: files whose .meta was read, and whose cached state was checked (each half of the step's work).
		std::atomic<uint64_t> Read = 0;
		std::atomic<uint64_t> Checked = 0;
		std::atomic<bool> Finished = false;

		std::vector<PreparedSource> Prepared;
		std::vector<std::string> Folders;
		ImportRecordLoadStats Records;
		std::string WalkError;
	};

	struct EditorAssetManager::ScanState
	{
		ScanImports Imports = ScanImports::Wait;
		Ref<ScanShared> Shared;
		JobHandle Job;
		std::chrono::steady_clock::time_point Started;
		bool Received = false; // The job's results were taken over (folders added)
		size_t Next = 0;       // The next prepared source to insert
		std::vector<ImportRequest> Stale; // Imported assets whose cached import is not current
		uint32_t Failed = 0;
	};

	EditorAssetManager::EditorAssetManager(const EditorAssetManagerSpecification& specification)
		: m_Specification(specification)
	{
		// The file watcher reports absolute paths, so the directories must be absolute too.
		std::error_code error;
		m_Specification.AssetDirectory = std::filesystem::absolute(m_Specification.AssetDirectory, error).lexically_normal();
		m_Specification.CacheDirectory = std::filesystem::absolute(m_Specification.CacheDirectory, error).lexically_normal();
		if (!m_Specification.TrashDirectory.empty())
			m_Specification.TrashDirectory = std::filesystem::absolute(m_Specification.TrashDirectory, error).lexically_normal();
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
		// What was recorded is in the file before a manager of the project opens again.
		std::string error;
		if (!m_Work->Records.Flush(&error))
			ST_CORE_WARN("The import records of '{}' were not all written: {}", FileSystem::ToUTF8(m_Specification.AssetDirectory), error);
	}

	void EditorAssetManager::Close()
	{
		if (m_Closed)
			return;
		m_Closed = true;
		m_Watcher.Stop();
		m_SearchesDue.clear();
		// A scan in progress ends: its job stops at its next check, and nothing it prepared is inserted.
		if (m_Scan)
		{
			m_Scan->Shared->Cancel.store(true);
			m_Scan.reset();
		}

		// Imports of external files end here: a copy still running takes back what it copied, and imports of what was copied
		// run again when the project next opens (the files are its assets now).
		for (const Ref<ExternalImport>& import : m_ExternalImports)
		{
			import->m_Task->Cancel.store(true);
			import->m_Error = "The project was closed before the import finished";
			import->m_State = ExternalImportState::Finished;
		}
		m_ExternalImports.clear();

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
					RemoveFromFolderIndex(handle, moved.Path);
					moved.Path = relativePath;
					moved.PathGeneration++;
					m_SourcesByPath[relativePath] = handle;
					AddToFolderIndex(handle, relativePath);
					m_Work->Records.SetPath(handle, relativePath);
					MarkChanged();
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
		AddToFolderIndex(handle, relativePath);
		m_Work->Records.SetPath(handle, relativePath);
		MarkChanged();
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
			RemoveFromFolderIndex(handle, it->second.Path);
			m_Sources.erase(it);
		}
		MarkChanged();

		for (AssetHandle subAsset : subAssets)
		{
			ForgetReferences(subAsset);
			UnregisterAsset(subAsset);
		}
		ForgetReferences(handle);
		UnregisterAsset(handle);

		if (deleteCache)
		{
			std::unique_lock<std::shared_mutex> fileLock(m_Work->FileMutex);
			for (AssetHandle subAsset : subAssets)
				FileSystem::Remove(GetCachePath(subAsset));
			FileSystem::Remove(GetCachePath(handle));
			m_Work->Records.Remove(handle);
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

	////////////////////////////////////////////////////////////////////////////////
	// Scanning: the asset directory registered by background jobs and inserted in slices
	////////////////////////////////////////////////////////////////////////////////

	// The scan's preparation of every file (on its job): reads .meta files, resolves handles, writes the .meta files that are
	// missing or need a new handle, checks cached imports, finds what documents reference, and removes cached data nothing
	// uses. Uses only what it is given.
	void EditorAssetManager::PrepareScan(BackgroundWork& work, ScanShared& shared, uint32_t batch, JobPriority priority)
	{
		std::vector<PreparedSource>& prepared = shared.Prepared;
		const auto cancelled = [&shared]() { return shared.Cancel.load(std::memory_order_relaxed); };

		// What each file is: its importer, .meta and state.
		JobSystem::ParallelFor(static_cast<uint32_t>(prepared.size()), std::max<uint32_t>(batch, 1), [&](uint32_t begin, uint32_t end)
		{
			for (uint32_t index = begin; index < end && !cancelled(); index++)
			{
				PreparedSource& source = prepared[index];
				shared.Read.fetch_add(1, std::memory_order_relaxed);
				source.Path = work.ToRelative(source.Absolute);
				source.Importer = AssetImporterRegistry::FindByExtension(GetExtension(source.Absolute));
				if (source.Path.empty() || !source.Importer)
				{
					source.Skip = true;
					continue;
				}
				const std::filesystem::path metaPath = GetMetaPath(source.Absolute);
				const AssetMeta meta = ReadAssetMeta(metaPath);
				if (meta.Status == AssetMetaStatus::Invalid)
				{
					ST_CORE_ERROR("'{}' is not a valid asset .meta file ({}); '{}' is ignored until the .meta is fixed or deleted",
						FileSystem::ToUTF8(metaPath), meta.Error, source.Path);
					source.Skip = true;
					continue;
				}
				source.Settings = source.Importer->GetDefaultSettings(source.Absolute);
				MergeSettings(source.Settings, meta.Settings);
				source.Handle = meta.Status == AssetMetaStatus::Valid ? meta.Handle : UUID::Null();
				source.NewHandle = meta.Status == AssetMetaStatus::Missing;
				source.SourceSize = FileSystem::GetFileSize(source.Absolute).value_or(0);
				source.SourceTime = FileSystem::GetLastWriteTime(source.Absolute).value_or(0);
			}
		}, priority);
		if (cancelled())
			return;

		// Handles that several files claim, and built-in ones, are given anew (the file the records say keeps its own).
		std::vector<AssetHandle> handles(prepared.size(), UUID::Null());
		std::vector<std::string> paths(prepared.size());
		std::unordered_set<AssetHandle> taken;
		for (size_t index = 0; index < prepared.size(); index++)
		{
			if (prepared[index].Skip)
				continue;
			handles[index] = prepared[index].Handle;
			paths[index] = prepared[index].Path;
			taken.insert(prepared[index].Handle);
		}
		for (const DuplicateHandle& duplicate : FindDuplicateHandles(handles, paths, work.Records.GetPaths()))
		{
			ST_CORE_WARN("'{}' has the asset handle of '{}' (copied .meta file?); assigning a new handle", paths[duplicate.File], paths[duplicate.Keeper]);
			prepared[duplicate.File].NewHandle = true;
		}
		for (PreparedSource& source : prepared)
		{
			if (source.Skip)
				continue;
			if (!source.NewHandle && BuiltinAssets::IsBuiltin(source.Handle))
			{
				ST_CORE_WARN("'{}' has the asset handle of a built-in asset (copied .meta file?); assigning a new handle", source.Path);
				source.NewHandle = true;
			}
			if (!source.NewHandle)
				continue;
			AssetHandle handle;
			do
				handle = UUID();
			while (BuiltinAssets::IsBuiltin(handle) || !handle.IsValid() || taken.contains(handle));
			taken.insert(handle);
			source.Handle = handle;
		}

		// The .meta files that change, and every file's cached state, in parallel.
		shared.Phase.store(static_cast<uint8_t>(AssetScanPhase::Registering));
		JobSystem::ParallelFor(static_cast<uint32_t>(prepared.size()), std::max<uint32_t>(batch, 1), [&](uint32_t begin, uint32_t end)
		{
			for (uint32_t index = begin; index < end && !cancelled(); index++)
			{
				PreparedSource& source = prepared[index];
				shared.Checked.fetch_add(1, std::memory_order_relaxed);
				if (source.Skip)
					continue;
				if (source.NewHandle && !WriteAssetMeta(GetMetaPath(source.Absolute), source.Handle, source.Importer->GetType(), source.Settings))
					ST_CORE_ERROR("Could not write '{}{}'; the asset's handle will change on the next scan", source.Path, EditorAssetManager::c_MetaExtension);
				try
				{
					if (source.Importer->StoresSourceDirectly())
					{
						source.References = work.FindNativeReferences(source.Handle, source.Importer->GetType(), source.Path);
					}
					else
					{
						ImportOutcome outcome;
						if (work.LoadCurrentImport(ImportRequest { source.Handle, source.Path, source.Importer, source.Settings }, outcome))
							source.Current = std::move(outcome);
					}
				}
				catch (const std::exception& exception)
				{
					ST_CORE_ERROR("Checking the cached state of '{}' failed: {}", source.Path, exception.what());
				}
			}
		}, priority);
		if (cancelled())
			return;

		// Cached data and records of what no asset has any more (deleted while the editor was closed, or leftovers of
		// interrupted writes). No import of this manager runs yet: its imports are queued once the scan is applied.
		std::unordered_set<AssetHandle> valid;
		for (const PreparedSource& source : prepared)
		{
			if (source.Skip)
				continue;
			valid.insert(source.Handle);
			if (source.Current)
			{
				for (const ImportedSubAsset& subAsset : source.Current->SubAssets)
					valid.insert(DeriveSubAssetHandle(source.Handle, subAsset.Key));
			}
		}
		work.RemoveUnusedCacheFiles(valid);
	}

	void EditorAssetManager::BackgroundWork::RemoveUnusedCacheFiles(const std::unordered_set<AssetHandle>& valid)
	{
		// Cache files are named "<handle>.<kind>[.<temporary suffix>]". No import of this manager runs during a scan, so
		// temporary files are leftovers of interrupted writes, or of a cancelled import of an earlier manager of the project
		// that is still running: that one never publishes them (and removes them itself where this removal fails).
		std::vector<std::filesystem::path> unused;
		std::error_code error;
		for (std::filesystem::directory_iterator it(Specification.CacheDirectory, error), end; !error && it != end; it.increment(error))
		{
			const std::string name = FileSystem::ToUTF8(it->path().filename());
			const size_t dot = name.find('.');
			const std::optional<UUID> handle = dot != std::string::npos ? UUID::FromString(std::string_view(name).substr(0, dot)) : std::nullopt;
			std::error_code typeError;
			if (!handle || !it->is_regular_file(typeError))
				continue;
			const std::string_view kind = std::string_view(name).substr(dot);
			if (kind != ".bin" || !valid.contains(*handle))
				unused.push_back(it->path());
		}
		{
			std::unique_lock<std::shared_mutex> fileLock(FileMutex);
			for (const std::filesystem::path& path : unused)
				FileSystem::Remove(path);
		}
		// Records and paths of handles no asset has any more.
		std::unordered_set<AssetHandle> recorded;
		for (const AssetHandle handle : Records.GetRecordedHandles())
			recorded.insert(handle);
		for (const auto& [handle, path] : Records.GetPaths())
			recorded.insert(handle);
		for (const AssetHandle handle : recorded)
		{
			if (!valid.contains(handle))
				Records.Remove(handle);
		}
	}

	void EditorAssetManager::PrepareAddedFiles(BackgroundWork& work, std::vector<PreparedSource>& sources, const std::atomic<bool>* cancel)
	{
		std::unordered_set<AssetHandle> taken;
		for (PreparedSource& source : sources)
		{
			if (IsCancelled(cancel))
				return;
			source.Path = work.ToRelative(source.Absolute);
			source.Importer = AssetImporterRegistry::FindByExtension(GetExtension(source.Absolute));
			if (source.Path.empty() || !source.Importer || !FileSystem::IsRegularFile(source.Absolute))
			{
				source.Skip = true;
				continue;
			}
			const std::filesystem::path metaPath = GetMetaPath(source.Absolute);
			const AssetMeta meta = ReadAssetMeta(metaPath);
			if (meta.Status == AssetMetaStatus::Invalid)
			{
				ST_CORE_ERROR("'{}' is not a valid asset .meta file ({}); '{}' is ignored until the .meta is fixed or deleted",
					FileSystem::ToUTF8(metaPath), meta.Error, source.Path);
				source.Skip = true;
				continue;
			}
			source.Settings = source.Importer->GetDefaultSettings(source.Absolute);
			MergeSettings(source.Settings, meta.Settings);
			source.SourceSize = FileSystem::GetFileSize(source.Absolute).value_or(0);
			source.SourceTime = FileSystem::GetLastWriteTime(source.Absolute).value_or(0);
			// A copied .meta keeps its handle unless the project has it (copied from the project, or twice): then a new one.
			source.Handle = meta.Status == AssetMetaStatus::Valid ? meta.Handle : UUID::Null();
			source.NewHandle = !source.Handle.IsValid() || BuiltinAssets::IsBuiltin(source.Handle) || taken.contains(source.Handle)
				|| work.IsHandleTaken(source.Handle);
			if (source.NewHandle)
			{
				if (meta.Status == AssetMetaStatus::Valid)
					ST_CORE_WARN("'{}' has the asset handle of another asset (copied .meta file?); assigning a new handle", source.Path);
				do
					source.Handle = UUID();
				while (!source.Handle.IsValid() || BuiltinAssets::IsBuiltin(source.Handle) || taken.contains(source.Handle) || work.IsHandleTaken(source.Handle));
				if (!WriteAssetMeta(metaPath, source.Handle, source.Importer->GetType(), source.Settings))
					ST_CORE_ERROR("Could not write '{}'; the asset's handle will change on the next scan", FileSystem::ToUTF8(metaPath));
			}
			taken.insert(source.Handle);
			try
			{
				if (source.Importer->StoresSourceDirectly())
				{
					source.References = work.FindNativeReferences(source.Handle, source.Importer->GetType(), source.Path);
				}
				else
				{
					// An import cached with the file (it came back with it) is current: taken at once, nothing is imported anew.
					ImportOutcome outcome;
					if (work.LoadCurrentImport(ImportRequest { source.Handle, source.Path, source.Importer, source.Settings }, outcome))
						source.Current = std::move(outcome);
				}
			}
			catch (const std::exception& exception)
			{
				ST_CORE_ERROR("Checking the cached state of '{}' failed: {}", source.Path, exception.what());
			}
		}
	}

	void EditorAssetManager::BeginScan(ScanImports imports)
	{
		ST_PROFILE_FUNCTION();
		ST_CORE_ASSERT(!m_Scan && !m_Scanned, "EditorAssetManager scans once, after construction");
		if (m_Scan || m_Scanned || m_Closed)
			return;
		m_Scan = CreateScope<ScanState>();
		m_Scan->Imports = imports;
		m_Scan->Shared = CreateRef<ScanShared>();
		m_Scan->Started = std::chrono::steady_clock::now();
		// Watched from before the walk, so that a file added while the scan runs is never missed: its report waits until the
		// scan is applied (ProcessFileChanges), and finds the file registered when the walk saw it. Folders too: one made or
		// removed outside the editor shows in (or leaves) the folder index without a file in it.
		FileWatcherSettings watchSettings;
		watchSettings.ReportDirectories = true;
		if (m_Specification.WatchFiles && !m_Watcher.IsRunning() && !m_Watcher.Start(m_Specification.AssetDirectory, watchSettings))
			ST_CORE_WARN("Asset hot reload is unavailable: cannot watch '{}'", FileSystem::ToUTF8(m_Specification.AssetDirectory));
		// The editor's scan is work no frame waits for; a blocking one (tools, tests) is waited for.
		const JobPriority priority = imports == ScanImports::InBackground ? JobPriority::Background : JobPriority::Normal;
		auto job = [work = m_Work, shared = m_Scan->Shared, batch = m_RegistrationOptions.PrepareBatch, priority]()
		{
			try
			{
				shared->Records = work->Records.Load();
				AssetDirectoryListing listing = WalkAssetDirectory(work->Specification.AssetDirectory, &shared->Cancel, &shared->Done);
				shared->WalkError = std::move(listing.Error);
				shared->Folders = std::move(listing.Folders);
				shared->Prepared.resize(listing.Files.size());
				for (size_t index = 0; index < listing.Files.size(); index++)
					shared->Prepared[index].Absolute = std::move(listing.Files[index]);
				shared->Done.store(0);
				shared->Total.store(shared->Prepared.size());
				shared->Phase.store(static_cast<uint8_t>(AssetScanPhase::Registering));
				PrepareScan(*work, *shared, batch, priority);
			}
			catch (const std::exception& exception)
			{
				ST_CORE_ERROR("The scan of '{}' failed: {}", FileSystem::ToUTF8(work->Specification.AssetDirectory), exception.what());
				shared->Prepared.clear();
			}
			shared->Done.store(0);
			shared->Total.store(shared->Prepared.size());
			shared->Phase.store(static_cast<uint8_t>(AssetScanPhase::Applying));
			shared->Finished.store(true, std::memory_order_release);
		};
		try
		{
			m_Scan->Job = JobSystem::Submit(std::move(job), priority);
		}
		catch (const std::exception& exception)
		{
			ST_CORE_ERROR("Could not start the scan of '{}': {}", FileSystem::ToUTF8(m_Specification.AssetDirectory), exception.what());
			m_Scan.reset();
			m_Scanned = true;
		}
	}

	void EditorAssetManager::Scan(ScanImports imports)
	{
		ST_PROFILE_FUNCTION();
		if (m_Scanned && !m_Scan)
		{
			RegisterNewFiles(imports);
			return;
		}
		if (!m_Scan)
			BeginScan(imports);
		if (!m_Scan)
			return;
		JobSystem::Wait(m_Scan->Job);
		AdvanceScan(true);
	}

	void EditorAssetManager::RegisterNewFiles(ScanImports imports)
	{
		if (m_Closed)
			return;
		const AssetDirectoryListing listing = WalkAssetDirectory(m_Specification.AssetDirectory);
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			for (const std::string& folder : listing.Folders)
				AddFolderLocked(folder);
		}
		std::vector<AssetHandle> added;
		for (const std::filesystem::path& file : listing.Files)
		{
			{
				std::scoped_lock<std::mutex> lock(m_SourceMutex);
				if (m_SourcesByPath.contains(ToRelative(file)))
					continue;
			}
			const AssetHandle handle = RegisterSourceFile(file);
			if (handle.IsValid())
				added.push_back(handle);
		}
		MarkChanged();

		std::vector<ImportRequest> requests;
		for (const AssetHandle handle : added)
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_Sources.find(handle);
			if (it != m_Sources.end() && !it->second.Importer->StoresSourceDirectly() && !it->second.ImportJob)
				requests.push_back(MakeImportRequest(it->second));
		}
		if (imports == ScanImports::Wait)
		{
			std::vector<ImportOutcome> outcomes(requests.size());
			JobSystem::ParallelFor(static_cast<uint32_t>(requests.size()), 1, [&](uint32_t begin, uint32_t end)
			{
				for (uint32_t index = begin; index < end; index++)
				{
					if (!m_Work->LoadCurrentImport(requests[index], outcomes[index]))
						outcomes[index] = m_Work->RunImport(requests[index]);
				}
			}, JobPriority::Background);
			for (ImportOutcome& outcome : outcomes)
				ApplyImportOutcome(std::move(outcome), false);
		}
		else
		{
			for (const ImportRequest& request : requests)
				QueueImport(request.Handle);
		}
		for (const AssetHandle handle : added)
			RefreshNativeReferences(handle, {}, nullptr);
	}

	std::optional<AssetScanProgress> EditorAssetManager::GetScanProgress() const
	{
		if (!m_Scan)
			return std::nullopt;
		const ScanShared& shared = *m_Scan->Shared;
		AssetScanProgress progress;
		progress.Phase = static_cast<AssetScanPhase>(shared.Phase.load());
		progress.Done = progress.Phase == AssetScanPhase::Registering
			? (shared.Read.load(std::memory_order_relaxed) + shared.Checked.load(std::memory_order_relaxed)) / 2 : shared.Done.load(std::memory_order_relaxed);
		progress.Total = shared.Total.load(std::memory_order_relaxed);
		return progress;
	}

	void EditorAssetManager::SetRegistrationOptions(AssetRegistrationOptions options)
	{
		m_RegistrationOptions = std::move(options);
	}

	void EditorAssetManager::AdvanceScan(bool unlimited)
	{
		if (!m_Scan || !m_Scan->Shared->Finished.load(std::memory_order_acquire))
			return;
		ST_PROFILE_FUNCTION();
		ScanState& scan = *m_Scan;
		ScanShared& shared = *scan.Shared;
		if (!scan.Received)
		{
			scan.Received = true;
			const ImportRecordLoadStats& records = shared.Records;
			if (records.SkippedLines > 0 || records.Discarded)
				ST_CORE_WARN("The import records of '{}': {} unreadable entries skipped{} (their assets are imported again when needed)",
					FileSystem::ToUTF8(m_Specification.AssetDirectory), records.SkippedLines, records.Discarded ? ", an unknown file started anew" : "");
			if (records.MigratedRecords + records.MigratedPaths > 0)
				ST_CORE_INFO("Took over {} import records and {} asset paths into '{}'", records.MigratedRecords, records.MigratedPaths,
					FileSystem::ToUTF8(m_Work->Records.GetPath()));
			if (!shared.WalkError.empty())
				ST_CORE_ERROR("Asset scan of '{}' incomplete: {}", FileSystem::ToUTF8(m_Specification.AssetDirectory), shared.WalkError);
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			for (const std::string& folder : shared.Folders)
				AddFolderLocked(folder);
			MarkChanged();
		}

		RegistrationSlice slice(m_RegistrationOptions, unlimited);
		scan.Next = InsertPrepared(shared.Prepared, scan.Next, slice, scan.Stale, scan.Failed);
		shared.Done.store(scan.Next, std::memory_order_relaxed);
		if (scan.Next < shared.Prepared.size())
			return;
		FinishScan();
	}

	size_t EditorAssetManager::InsertPrepared(std::vector<PreparedSource>& sources, size_t next, RegistrationSlice& slice, std::vector<ImportRequest>& outStale,
		uint32_t& outFailed)
	{
		ST_PROFILE_FUNCTION();
		// Inserted in batches (one registry lock each), then their cached state is applied, until the slice's time is up.
		std::vector<AssetMetadata> batch;
		std::vector<size_t> batchSources;
		const size_t batchSize = std::max<uint32_t>(m_RegistrationOptions.EntriesPerCheck, 1);
		bool more = true;
		while (more && next < sources.size())
		{
			batch.clear();
			batchSources.clear();
			{
				std::scoped_lock<std::mutex> lock(m_SourceMutex);
				while (next < sources.size() && batchSources.size() < batchSize)
				{
					const size_t index = next++;
					PreparedSource& prepared = sources[index];
					if (prepared.Skip)
						continue;
					if (auto existing = m_SourcesByPath.find(prepared.Path); existing != m_SourcesByPath.end())
					{
						// Registered already (by an earlier scan of the file): it keeps its handle.
						prepared.Handle = existing->second;
						prepared.Inserted = true;
						continue;
					}
					if (m_Sources.contains(prepared.Handle) || m_Reservations.contains(prepared.Handle) || BuiltinAssets::IsBuiltin(prepared.Handle))
					{
						// Taken meanwhile (a background save published one): a new handle, written by an I/O job.
						ST_CORE_WARN("'{}' has the asset handle of another asset; assigning a new handle", prepared.Path);
						do
							prepared.Handle = UUID();
						while (BuiltinAssets::IsBuiltin(prepared.Handle) || m_Sources.contains(prepared.Handle) || m_Reservations.contains(prepared.Handle));
						prepared.Current.reset();
						if (prepared.References)
							prepared.References->Handle = prepared.Handle;
						auto write = [path = GetMetaPath(prepared.Absolute), handle = prepared.Handle, type = prepared.Importer->GetType(), settings = prepared.Settings]()
						{
							if (!WriteAssetMeta(path, handle, type, settings))
								ST_CORE_ERROR("Could not write '{}'", FileSystem::ToUTF8(path));
						};
						try
						{
							JobSystem::SubmitIO(std::move(write), JobPriority::Normal);
						}
						catch (const std::exception& exception)
						{
							ST_CORE_ERROR("Could not queue the write of '{}{}': {}", prepared.Path, c_MetaExtension, exception.what());
						}
					}
					SourceAsset source;
					source.Handle = prepared.Handle;
					source.Path = prepared.Path;
					source.Importer = prepared.Importer;
					source.Type = prepared.Importer->GetType();
					source.Settings = prepared.Settings;
					source.SourceSize = prepared.SourceSize;
					source.SourceTime = prepared.SourceTime;
					source.Import.Imported = prepared.Importer->StoresSourceDirectly(); // Read as-is: nothing to import

					AssetMetadata metadata;
					metadata.Handle = source.Handle;
					metadata.Type = source.Type;
					metadata.Path = source.Path;
					metadata.Name = FileSystem::ToUTF8(prepared.Absolute.stem());
					// Imported assets are read from the cache: its size comes with the cached import (or the import, later).
					metadata.StoredSize = source.Import.Imported ? source.SourceSize : (prepared.Current ? prepared.Current->StoredSize : 0);
					batch.push_back(std::move(metadata));
					batchSources.push_back(index);

					m_SourcesByPath[source.Path] = source.Handle;
					const std::filesystem::path folder = FileSystem::FromUTF8(source.Path).parent_path();
					if (!folder.empty())
						AddFolderLocked(FileSystem::ToUTF8(folder));
					AddToFolderIndex(source.Handle, source.Path);
					m_Work->Records.SetPath(source.Handle, source.Path);
					m_Sources.emplace(source.Handle, std::move(source));
					prepared.Inserted = true;
				}
			}
			RegisterAssets(batch);
			for (const size_t index : batchSources)
			{
				PreparedSource& prepared = sources[index];
				if (prepared.References)
					ApplyNativeReferences(*prepared.References);
				if (prepared.Current)
				{
					outFailed += prepared.Current->Success ? 0 : 1;
					ApplyImportOutcome(std::move(*prepared.Current), false);
					prepared.Current.reset();
				}
				else if (!prepared.Importer->StoresSourceDirectly())
				{
					outStale.push_back(ImportRequest { prepared.Handle, prepared.Path, prepared.Importer, prepared.Settings });
				}
				more = slice.HasTime();
			}
			MarkChanged();
		}
		return next;
	}

	void EditorAssetManager::FinishScan()
	{
		ScanState& scan = *m_Scan;
		const size_t assets = m_Sources.size();
		uint32_t importedCount = 0;
		if (scan.Imports == ScanImports::Wait)
		{
			// Imported here, in parallel on the background threads, before the scan returns.
			std::vector<ImportOutcome> outcomes(scan.Stale.size());
			JobSystem::ParallelFor(static_cast<uint32_t>(scan.Stale.size()), 1, [&](uint32_t begin, uint32_t end)
			{
				for (uint32_t index = begin; index < end; index++)
				{
					try
					{
						outcomes[index] = m_Work->RunImport(scan.Stale[index]);
					}
					catch (const std::exception& exception)
					{
						outcomes[index] = ImportOutcome();
						outcomes[index].Handle = scan.Stale[index].Handle;
						outcomes[index].Error = fmt::format("Import failed: {}", exception.what());
					}
				}
			}, JobPriority::Background);
			for (ImportOutcome& outcome : outcomes)
			{
				importedCount++;
				scan.Failed += outcome.Success ? 0 : 1;
				ApplyImportOutcome(std::move(outcome), false);
			}
		}
		else
		{
			for (const ImportRequest& request : scan.Stale)
				QueueImport(request.Handle);
		}

		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - scan.Started);
		if (scan.Imports == ScanImports::Wait || scan.Stale.empty())
			ST_CORE_INFO("Asset scan: {} assets, {} imported, {} failed ({} ms)", assets, importedCount, scan.Failed, elapsed.count());
		else
			ST_CORE_INFO("Asset scan: {} assets, {} importing in the background, {} failed ({} ms)", assets, scan.Stale.size(), scan.Failed, elapsed.count());
		m_Scan.reset();
		m_Scanned = true;
		MarkChanged();
	}

	////////////////////////////////////////////////////////////////////////////////
	// Importing
	////////////////////////////////////////////////////////////////////////////////

	EditorAssetManager::ImportRequest EditorAssetManager::MakeImportRequest(const SourceAsset& source) const
	{
		return ImportRequest { source.Handle, source.Path, source.Importer, source.Settings };
	}

	bool EditorAssetManager::BackgroundWork::LoadCurrentImport(const ImportRequest& request, ImportOutcome& outOutcome, const std::atomic<bool>* cancel) const
	{
		if (IsCancelled(cancel))
			return false;
		std::optional<nlohmann::json> record = Records.FindRecord(request.Handle);
		if (!record || !record->is_object())
			return false;
		// The source and its dependencies may be hashed: read under the shared file lock, like every read of project files
		// off the main thread (a move or delete of a file an open reader holds fails on Windows).
		std::shared_lock<std::shared_mutex> fileLock(FileMutex);

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

		const std::optional<uint64_t> storedSize = FileSystem::IsRegularFile(GetCachePath(request.Handle)) ? FileSystem::GetFileSize(GetCachePath(request.Handle))
			: std::nullopt;
		if (!storedSize)
			return false;
		outcome.StoredSize = *storedSize;
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
				const std::filesystem::path cachePath = GetCachePath(DeriveSubAssetHandle(request.Handle, entry.Key));
				const std::optional<uint64_t> subAssetSize = FileSystem::IsRegularFile(cachePath) ? FileSystem::GetFileSize(cachePath) : std::nullopt;
				if (!subAssetSize)
					return false;
				outcome.SubAssets.push_back(std::move(entry));
				outcome.SubAssetStoredSizes.push_back(*subAssetSize);
			}
		}

		fileLock.unlock();

		if (recordChanged)
		{
			// Like an import's publication: under the exclusive lock, and not once the import is cancelled (its asset may be
			// gone, with the record it had).
			std::unique_lock<std::shared_mutex> writeLock(FileMutex);
			if (!IsCancelled(cancel))
				Records.SetRecord(request.Handle, *record);
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
		// Messages of third-party decoders can quote bytes of the file: logs, records and results get valid UTF-8.
		outcome.Warnings = std::move(result.Warnings);
		for (std::string& warning : outcome.Warnings)
			warning = StringUtils::MakeValidUTF8(warning);
		error = StringUtils::MakeValidUTF8(error);

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
			auto stage = [&](AssetHandle handle, std::vector<uint8_t>& data, uint64_t& outSize)
			{
				outSize = data.size();
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
			staged = stage(request.Handle, result.Data, outcome.StoredSize);
			outcome.SubAssetStoredSizes.resize(result.SubAssets.size());
			for (size_t index = 0; index < result.SubAssets.size(); index++)
			{
				ImportedSubAsset& subAsset = result.SubAssets[index];
				// Frees every sub-asset's data
				staged = stage(DeriveSubAssetHandle(request.Handle, subAsset.Key), subAsset.Data, outcome.SubAssetStoredSizes[index]) && staged;
			}
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
		// After the cache files are in place (the record never names one that is missing), still under the exclusive lock.
		Records.SetRecord(request.Handle, record);
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

		MarkChanged();
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

		SetStoredSize(outcome.Handle, outcome.StoredSize);
		RecordReferences(outcome.Handle, outcome.References, outcome.Bounds);
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
			metadata.StoredSize = index < outcome.SubAssetStoredSizes.size() ? outcome.SubAssetStoredSizes[index] : 0;
			RegisterAsset(metadata);
			RecordReferences(metadata.Handle, subAsset.References, subAsset.Bounds);
		}

		// Sub-assets the new import no longer produces disappear, their cached data on an I/O job.
		std::vector<AssetHandle> dropped;
		for (AssetHandle previous : previousSubAssets)
		{
			if (std::find(subAssetHandles.begin(), subAssetHandles.end(), previous) != subAssetHandles.end())
				continue;
			ForgetReferences(previous);
			UnregisterAsset(previous);
			dropped.push_back(previous);
		}
		if (!dropped.empty())
			RemoveCacheFilesLater(std::move(dropped));

		if (reload && !outcome.FromCache)
		{
			ReloadAsset(outcome.Handle);
			for (AssetHandle subAsset : subAssetHandles)
				ReloadAsset(subAsset);
		}
		// Reloads restart waiting loads themselves; these are the others (an import found current, or applied without reload).
		ResumeLoadsAwaitingImport(awaited);
	}

	void EditorAssetManager::RemoveCacheFilesLater(std::vector<AssetHandle> handles)
	{
		auto remove = [work = m_Work, handles = std::move(handles)]()
		{
			// A handle registered again meanwhile (an import that brought the sub-asset back) keeps its file. Lock order: the
			// owner before the file lock.
			std::scoped_lock<std::mutex> ownerLock(work->OwnerMutex);
			std::unique_lock<std::shared_mutex> fileLock(work->FileMutex);
			for (const AssetHandle handle : handles)
			{
				if (!work->Owner || !work->Owner->IsHandleValid(handle))
					FileSystem::Remove(work->GetCachePath(handle));
			}
		};
		try
		{
			JobSystem::SubmitIO(std::move(remove), JobPriority::Background);
		}
		catch (const std::exception& exception)
		{
			// The next scan removes what nothing uses.
			ST_CORE_WARN("Could not queue the removal of cached data: {}", exception.what());
		}
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
		MarkChanged();
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
		MarkChanged();
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
				if (current)
					it->second.SearchJob.reset();
			}
			if (!current)
				continue;
			ApplyNativeReferences(found);
			if (found.Record)
				m_Work->Records.SetRecord(found.Handle, *found.Record);
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
		// A scan in progress first: it queues imports once it is applied. Searches that wait for their document to settle start
		// now.
		if (m_Scan)
		{
			JobSystem::Wait(m_Scan->Job);
			AdvanceScan(true);
		}
		StartDueReferenceSearches(true);
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

	void EditorAssetManager::WaitForImportProgress(std::chrono::milliseconds timeout)
	{
		{
			// Condition is notified whenever a job leaves Jobs, which a finished job does after handing its result over.
			std::unique_lock<std::mutex> lock(m_Work->Mutex);
			m_Work->Condition.wait_for(lock, timeout, [this]()
			{
				return m_Work->Jobs.empty() || !m_Work->CompletedImports.empty() || !m_Work->CompletedReferences.empty();
			});
		}
		ProcessCompletedImports();
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
		try
		{
			// The record, when the file is what it was when the record was written. The file is hashed under the shared file lock:
			// a save replaces it under the exclusive one, which an open reader would make fail on Windows.
			if (data.empty())
			{
				std::optional<nlohmann::json> record = Records.FindRecord(handle);
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
							Records.SetRecord(handle, *record);
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

			nlohmann::json record = MakeNativeRecord(relativePath, data.size(), sourceTime, Hash::FNV1a(data), result.References);
			if (deferRecord)
				result.Record = std::move(record);
			else
				Records.SetRecord(handle, record);
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
			RecordReferences(references.Handle, references.References, std::nullopt);
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
			// A search queued before would find the previous contents: it is no longer wanted.
			StopReferenceSearch(it->second);
		}
		m_SearchesDue.erase(handle);
		ApplyNativeReferences(m_Work->FindNativeReferences(handle, type, path, data, references));
	}

	void EditorAssetManager::QueueNativeReferences(AssetHandle handle)
	{
		if (m_Closed)
			return;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_Sources.find(handle);
			if (it == m_Sources.end() || !it->second.Importer->StoresSourceDirectly() || !AssetDependencies::CanReference(it->second.Type))
				return;
			// A search of the previous contents is no longer wanted: one search per document at a time.
			StopReferenceSearch(it->second);
		}
		// It starts once the document has not changed for a while (a file written over seconds is searched once).
		m_SearchesDue[handle] = std::chrono::steady_clock::now() + c_ReferenceSearchDelay;
	}

	void EditorAssetManager::StopReferenceSearch(SourceAsset& source)
	{
		source.ReferencesGeneration++;
		if (source.SearchJob)
		{
			m_Work->Cancel(source.SearchJob);
			source.SearchJob.reset();
		}
	}

	void EditorAssetManager::StartDueReferenceSearches(bool all)
	{
		if (m_SearchesDue.empty())
			return;
		const auto now = std::chrono::steady_clock::now();
		for (auto due = m_SearchesDue.begin(); due != m_SearchesDue.end();)
		{
			if (!all && due->second > now)
			{
				++due;
				continue;
			}
			const AssetHandle handle = due->first;
			due = m_SearchesDue.erase(due);
			AssetType type = AssetType::None;
			std::string path;
			uint64_t generation = 0;
			const Ref<BackgroundJob> job = CreateRef<BackgroundJob>();
			job->Search = true;
			job->Asset = handle;
			{
				std::scoped_lock<std::mutex> lock(m_SourceMutex);
				auto it = m_Sources.find(handle);
				if (it == m_Sources.end())
					continue;
				type = it->second.Type;
				path = it->second.Path;
				generation = ++it->second.ReferencesGeneration;
				it->second.SearchJob = job;
			}

			// A job like an import: WaitForImports counts it, Close cancels it.
			m_Work->Add(job);
			m_ReferenceSearches.fetch_add(1, std::memory_order_relaxed);
			auto search = [work = m_Work, job, handle, type, path, generation]()
			{
				if (job->Cancel.load())
					return;
				job->Started.store(true);
				// Its record is written by Update, and only while this is the latest search (FindNativeReferences catches what
				// it may throw).
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
	}

	bool EditorAssetManager::HasPendingReferenceSearches() const
	{
		if (!m_SearchesDue.empty())
			return true;
		std::scoped_lock<std::mutex> lock(m_Work->Mutex);
		return !m_Work->CompletedReferences.empty()
			|| std::any_of(m_Work->Jobs.begin(), m_Work->Jobs.end(), [](const Ref<BackgroundJob>& job) { return job->Search; });
	}

	std::vector<AssetHandle> EditorAssetManager::GetPendingReferenceSearches() const
	{
		std::unordered_set<AssetHandle> pending;
		for (const auto& [handle, due] : m_SearchesDue)
			pending.insert(handle);
		{
			std::scoped_lock<std::mutex> lock(m_Work->Mutex);
			for (const Ref<BackgroundJob>& job : m_Work->Jobs)
			{
				if (job->Search)
					pending.insert(job->Asset);
			}
			for (const NativeReferences& references : m_Work->CompletedReferences)
				pending.insert(references.Handle);
		}
		std::vector<AssetHandle> handles(pending.begin(), pending.end());
		std::sort(handles.begin(), handles.end());
		return handles;
	}

	std::optional<std::vector<AssetHandle>> EditorAssetManager::FindDocumentReferences(AssetHandle document) const
	{
		AssetType type = AssetType::None;
		std::string path;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_Sources.find(document);
			if (it == m_Sources.end() || !it->second.Importer->StoresSourceDirectly())
				return std::nullopt;
			type = it->second.Type;
			path = it->second.Path;
		}
		NativeReferences references = m_Work->FindNativeReferences(document, type, path, {}, nullptr, true);
		if (!references.Scanned)
			return std::nullopt;
		return std::move(references.References);
	}

	void EditorAssetManager::RecordScannedReferences(AssetHandle document, std::span<const AssetHandle> references)
	{
		if (IsHandleValid(document))
			RecordReferences(document, references, std::nullopt);
	}

	////////////////////////////////////////////////////////////////////////////////
	// Hot reload
	////////////////////////////////////////////////////////////////////////////////

	void EditorAssetManager::Update()
	{
		ST_PROFILE_FUNCTION();
		// A scan inserts what its jobs prepared, a slice per update.
		AdvanceScan(false);
		ST_PROFILE_STAGE("Assets.Update");
		ProcessFileChanges();
		StartDueReferenceSearches(false);
		ProcessCompletedImports();
		if (!m_ExternalImports.empty())
			UpdateExternalImports();
		AssetManagerBase::Update();
	}

	void EditorAssetManager::ProcessFileChanges()
	{
		// What changed while a scan runs waits for it (the watcher keeps the reports).
		if (!m_Watcher.IsRunning() || m_Scan)
			return;

		// Removals last: a move arrives as a removal plus an addition, and the addition must find the asset at its old
		// place to keep its handle and cached data; a file replaced by a "safe save" exists again by then.
		std::vector<FileChange> changes = m_Watcher.PollChanges();
		if (changes.empty())
			return;
		std::stable_partition(changes.begin(), changes.end(), [](const FileChange& change) { return change.Type != FileChangeType::Removed; });

		// Files that imports of external files are still copying belong to those imports, which register them once they are
		// complete (UpdateExternalImports, later in this update): a file that is still being written is never imported.
		std::unordered_set<std::string> copying;
		for (const Ref<ExternalImport>& import : m_ExternalImports)
		{
			if (import->m_State != ExternalImportState::Copying)
				continue;
			std::scoped_lock<std::mutex> lock(import->m_Task->Mutex);
			copying.insert(import->m_Task->Claimed.begin(), import->m_Task->Claimed.end());
		}

		for (const FileChange& change : changes)
		{
			const std::filesystem::path path = change.Path.lexically_normal();
			if (!copying.empty() && copying.contains(ToRelative(path)))
				continue;
			// Documents written in the background (and the .meta of a new one) are published by their writer, which records
			// their state: what the watcher saw of the write is no news then.
			if (!change.Directory)
			{
				std::filesystem::path claimed = path;
				if (GetExtension(path) == c_MetaExtension)
					claimed.replace_extension();
				if (m_Work->IsClaimed(ToRelative(claimed)))
					continue;
			}
			if (change.Directory)
			{
				ProcessFolderChange(path, change.Type);
				continue;
			}
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

	uint64_t EditorAssetManager::GetContentStamp(AssetHandle handle) const
	{
		if (std::optional<AssetMetadata> metadata = GetMetadata(handle); metadata && metadata->IsSubAsset())
			handle = metadata->Parent;
		std::scoped_lock<std::mutex> lock(m_SourceMutex);
		auto it = m_Sources.find(handle);
		if (it == m_Sources.end())
			return 0;
		const SourceAsset& source = it->second;
		const std::string state = fmt::format("{}|{}|{}|{}|{}|{}", source.SourceSize, source.SourceTime, source.Importer ? source.Importer->GetVersion() : 0u,
			source.Import.Imported, source.Import.Error.empty(), JsonUtils::Dump(source.Settings));
		const uint64_t stamp = Hash::FNV1a(state);
		return stamp != 0 ? stamp : 1;
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

		std::filesystem::path directory = ToAbsolute(targetDirectory);
		if (!directory.has_filename())
			directory = directory.parent_path(); // Trailing separator ("UI/") or the asset root ("")
		if (directory != m_Specification.AssetDirectory && ToRelative(directory).empty())
			return fail(fmt::format("'{}' is outside the asset directory", targetDirectory));

		std::vector<std::string> createdFolders;
		ExternalCopy copy(m_Specification.AssetDirectory, nullptr, createdFolders);
		const ExternalImportItem item = copy.CopyExternalFile(sourceFile, directory);
		if (!item.Error.empty())
		{
			RollBackExternalFiles(item.Files, {});
			return fail(item.Error);
		}

		// The file first, then the files it reads that are assets themselves (its import reads them as they are, so they are
		// imported for their own use only).
		AssetHandle handle = UUID::Null();
		for (const std::string& file : item.Files)
		{
			const AssetHandle registered = RegisterSourceFile(ToAbsolute(file));
			if (!registered.IsValid())
				continue;
			if (!handle.IsValid())
				handle = registered;
			// A failed import is logged by ApplyImportOutcome and kept in the asset's import state.
			ReimportAsset(registered);
		}
		if (!handle.IsValid())
			return fail("Registering the copied file failed");
		return handle;
	}

	Ref<ExternalImport> EditorAssetManager::ImportExternal(const ExternalImportRequest& request, std::string* outError)
	{
		auto fail = [outError](std::string message) -> Ref<ExternalImport>
		{
			if (outError)
				*outError = std::move(message);
			return nullptr;
		};

		if (m_Closed)
			return fail("The project's assets are closed: nothing is imported any more");
		const std::string target = NormalizeIncludePath(request.TargetDirectory);
		const std::filesystem::path directory = target.empty() ? m_Specification.AssetDirectory : ToAbsolute(target);
		const std::string relative = ToRelative(directory);
		if (!target.empty() && (relative.empty() || relative == "."))
			return fail(fmt::format("'{}' is outside the asset directory", request.TargetDirectory));
		if (!target.empty() && IsHiddenRelativePath(FileSystem::FromUTF8(relative)))
			return fail(fmt::format("'{}' is hidden (a name starts with '.'), so it could hold no assets", relative));
		if (FileSystem::Exists(directory) && !FileSystem::IsDirectory(directory))
			return fail(fmt::format("'{}' is a file, not a folder", relative));
		if (request.Files.empty() && request.Directories.empty())
			return fail("Nothing to import");

		Ref<ExternalImport> import = CreateRef<ExternalImport>();
		import->m_Request = request;
		import->m_TargetDirectory = directory;
		import->m_Task = CreateRef<ExternalImport::CopyTask>();
		// One copy at a time (StartNextExternalCopy): a copy picks free names for what it writes, and two copies beside each
		// other could pick the same one. A copy that cannot start at once fails the call; one that waits for another starts
		// with a later update.
		const bool waits = std::any_of(m_ExternalImports.begin(), m_ExternalImports.end(), [](const Ref<ExternalImport>& other)
		{
			return other->m_State == ExternalImportState::Copying;
		});
		if (!waits)
		{
			std::string error;
			if (!StartExternalCopy(*import, &error))
				return fail(std::move(error));
		}
		m_ExternalImports.push_back(import);
		MarkChanged();
		return import;
	}

	bool EditorAssetManager::StartExternalCopy(ExternalImport& import, std::string* outError)
	{
		// The job uses only what it shares with the import: the manager may close while it copies.
		auto copy = [work = m_Work, task = import.m_Task, request = import.m_Request, assetDirectory = m_Specification.AssetDirectory, directory = import.m_TargetDirectory]()
		{
			std::vector<ExternalImportItem> items;
			std::vector<std::string> createdFolders;
			auto claim = [&task](const std::string& relativePath)
			{
				std::scoped_lock<std::mutex> lock(task->Mutex);
				task->Claimed.insert(relativePath);
			};
			ExternalCopy copier(assetDirectory, &task->Cancel, createdFolders, claim);
			for (const std::filesystem::path& file : request.Files)
				items.push_back(copier.CopyExternalFile(file, directory));
			for (const std::filesystem::path& folder : request.Directories)
				items.push_back(copier.CopyExternalFolder(folder, directory));
			if (copier.IsCancelled())
			{
				// Nobody registers what was copied: it goes again, so the next scan finds no half import.
				for (const ExternalImportItem& item : items)
				{
					for (const std::string& file : item.Files)
						FileSystem::Remove(assetDirectory / FileSystem::FromUTF8(file));
				}
				for (auto it = createdFolders.rbegin(); it != createdFolders.rend(); ++it)
				{
					std::error_code error;
					std::filesystem::remove(assetDirectory / FileSystem::FromUTF8(*it), error); // Only while empty
				}
			}
			// What it copied is prepared for registration here, so the main thread only inserts it (its own asset first, then the
			// files it reads that are assets themselves, in the order each item lists them).
			std::vector<PreparedSource> prepared;
			if (!copier.IsCancelled())
			{
				std::unordered_set<std::string> seen;
				for (const ExternalImportItem& item : items)
				{
					if (!item.Error.empty())
						continue;
					for (const std::string& file : item.Files)
					{
						if (GetExtension(FileSystem::FromUTF8(file)) == c_MetaExtension || !seen.insert(file).second)
							continue;
						PreparedSource source;
						source.Absolute = (assetDirectory / FileSystem::FromUTF8(file)).lexically_normal();
						prepared.push_back(std::move(source));
					}
				}
				PrepareAddedFiles(*work, prepared, &task->Cancel);
			}
			std::scoped_lock<std::mutex> lock(task->Mutex);
			task->Items = std::move(items);
			task->CreatedFolders = std::move(createdFolders);
			task->Prepared = std::move(prepared);
			task->Done = true;
		};
		try
		{
			JobSystem::SubmitIO(std::move(copy), JobPriority::Normal);
		}
		catch (const std::exception& exception)
		{
			if (outError)
				*outError = fmt::format("Could not start copying: {}", exception.what());
			return false;
		}
		import.m_CopyStarted = true;
		return true;
	}

	void EditorAssetManager::StartNextExternalCopy()
	{
		for (const Ref<ExternalImport>& import : m_ExternalImports)
		{
			if (import->m_State != ExternalImportState::Copying)
				continue;
			if (import->m_CopyStarted)
				return; // Copying: the others wait for it
			std::string error;
			if (StartExternalCopy(*import, &error))
				return;
			// It ends with the error (UpdateExternalImports drops it), and the next one tries.
			import->m_Error = std::move(error);
			import->m_State = ExternalImportState::Finished;
			MarkChanged();
		}
	}

	void EditorAssetManager::UpdateExternalImports()
	{
		StartNextExternalCopy();
		bool copied = false;
		for (size_t index = 0; index < m_ExternalImports.size();)
		{
			ExternalImport& import = *m_ExternalImports[index];
			if (import.m_State == ExternalImportState::Finished)
			{
				m_ExternalImports.erase(m_ExternalImports.begin() + static_cast<std::ptrdiff_t>(index));
				continue;
			}
			if (import.m_State == ExternalImportState::Copying)
			{
				if (!import.m_CopyStarted)
				{
					index++;
					continue;
				}
				std::scoped_lock<std::mutex> lock(import.m_Task->Mutex);
				if (!import.m_Task->Done)
				{
					index++;
					continue;
				}
				copied = true;
				import.m_Items = std::move(import.m_Task->Items);
				import.m_CreatedFolders = std::move(import.m_Task->CreatedFolders);
				import.m_State = ExternalImportState::Importing;

				// What was copied becomes assets (with their .meta files and handles), imported in the background.
				{
					std::scoped_lock<std::mutex> sourceLock(m_SourceMutex);
					for (const std::string& folder : import.m_CreatedFolders)
						AddFolderLocked(folder);
				}
				// The copy job prepared them: inserting them touches no file. Imports whose cached data came along are current.
				std::vector<ImportRequest> stale;
				uint32_t failed = 0;
				RegistrationSlice unlimited(m_RegistrationOptions, true);
				InsertPrepared(import.m_Task->Prepared, 0, unlimited, stale, failed);
				for (const ImportRequest& request : stale)
					QueueImport(request.Handle);
				std::unordered_map<std::string, AssetHandle> registered;
				for (const PreparedSource& prepared : import.m_Task->Prepared)
				{
					if (prepared.Inserted)
						registered.emplace(prepared.Path, prepared.Handle);
				}
				import.m_Task->Prepared.clear();
				for (ExternalImportItem& item : import.m_Items)
				{
					if (!item.Error.empty())
						continue;
					for (const std::string& file : item.Files)
					{
						const auto it = registered.find(file);
						if (it == registered.end())
							continue;
						if (std::find(item.Assets.begin(), item.Assets.end(), it->second) == item.Assets.end())
							item.Assets.push_back(it->second);
					}
					// The import's caller reports the failures FinishExternalImport finds (a file's own asset, every asset of a
					// folder): they are no imports nobody waited for (TakeImportFailures). A file the imported one reads that
					// fails alone is.
					{
						std::scoped_lock<std::mutex> sourceLock(m_SourceMutex);
						for (size_t asset = 0; asset < item.Assets.size() && (item.Folder || asset == 0); asset++)
						{
							auto source = m_Sources.find(item.Assets[asset]);
							if (source != m_Sources.end() && !source->second.Importer->StoresSourceDirectly())
								source->second.ImportAwaited = true;
						}
					}
					if (!item.Folder && item.Assets.empty())
						item.Error = fmt::format("'{}' could not be registered as an asset", item.Path);
				}
				MarkChanged();
			}

			// Done once every asset it brought is imported (or failed).
			const bool importing = std::any_of(import.m_Items.begin(), import.m_Items.end(), [this](const ExternalImportItem& item)
			{
				return std::any_of(item.Assets.begin(), item.Assets.end(), [this](AssetHandle handle) { return IsImportPending(handle); });
			});
			if (importing)
			{
				index++;
				continue;
			}
			FinishExternalImport(import);
			m_ExternalImports.erase(m_ExternalImports.begin() + static_cast<std::ptrdiff_t>(index));
			MarkChanged();
		}
		// The next copy starts as soon as one is done, rather than a frame later.
		if (copied)
			StartNextExternalCopy();
	}

	void EditorAssetManager::FinishExternalImport(ExternalImport& import)
	{
		for (ExternalImportItem& item : import.m_Items)
		{
			// A copy that failed takes back what it copied. Logged here, once, as failed imports are when they are applied.
			if (!item.Error.empty())
			{
				ST_CORE_ERROR("Import of '{}' failed: {}", FileSystem::ToUTF8(item.Source), item.Error);
				if (!item.Files.empty())
				{
					RollBackExternalFiles(item.Files, item.Assets);
					item.Assets.clear();
					item.RolledBack = true;
				}
				continue;
			}

			// The assets whose import failed: for a file its own (the files it reads may fail alone: a model then lacks a texture
			// and says so in its warnings), for a folder each.
			std::vector<AssetHandle> failed;
			std::string firstError;
			for (size_t index = 0; index < item.Assets.size(); index++)
			{
				const AssetImportInfo info = GetImportInfo(item.Assets[index]);
				const bool removed = !IsHandleValid(item.Assets[index]);
				if (info.Error.empty() && !removed)
					continue;
				if (!item.Folder && index > 0)
					continue;
				failed.push_back(item.Assets[index]);
				if (firstError.empty())
					firstError = removed ? std::string("the asset was removed while it imported") : info.Error;
			}
			if (failed.empty())
				continue;
			item.Error = item.Folder ? fmt::format("{} of {} assets failed to import; the first: {}", failed.size(), item.Assets.size(), firstError) : firstError;
			if (import.m_Request.KeepOnFailure)
				continue;

			if (item.Folder)
			{
				// Only the assets that failed go (with their .meta files): the rest of the folder imported.
				std::vector<std::string> files;
				for (AssetHandle handle : failed)
				{
					if (const std::optional<AssetMetadata> metadata = GetMetadata(handle))
					{
						files.push_back(metadata->Path);
						files.push_back(metadata->Path + std::string(c_MetaExtension));
					}
				}
				RollBackExternalFiles(files, failed);
				std::erase_if(item.Assets, [&failed](AssetHandle handle) { return std::find(failed.begin(), failed.end(), handle) != failed.end(); });
			}
			else
			{
				RollBackExternalFiles(item.Files, item.Assets);
				item.Assets.clear();
			}
			item.RolledBack = true;
		}

		// Folders the copy made that nothing stayed in (only a roll-back empties one: an import that kept everything touches
		// no file here).
		const bool rolledBack = std::any_of(import.m_Items.begin(), import.m_Items.end(), [](const ExternalImportItem& item) { return item.RolledBack; });
		for (auto it = import.m_CreatedFolders.rbegin(); rolledBack && it != import.m_CreatedFolders.rend(); ++it)
		{
			const std::filesystem::path folder = ToAbsolute(*it);
			std::error_code error;
			if (FileSystem::IsDirectory(folder) && std::filesystem::directory_iterator(folder, error) == std::filesystem::directory_iterator() && !error
				&& std::filesystem::remove(folder, error))
			{
				std::scoped_lock<std::mutex> lock(m_SourceMutex);
				m_Folders.erase(*it);
			}
		}
		import.m_State = ExternalImportState::Finished;
	}

	void EditorAssetManager::RollBackExternalFiles(const std::vector<std::string>& files, const std::vector<AssetHandle>& assets)
	{
		for (AssetHandle handle : assets)
		{
			std::string error;
			if (IsHandleValid(handle) && !DeleteAsset(handle, &error))
				ST_CORE_WARN("Could not remove an asset whose import failed: {}", error);
		}
		std::unique_lock<std::shared_mutex> fileLock(m_Work->FileMutex);
		for (const std::string& file : files)
		{
			const std::filesystem::path path = ToAbsolute(file);
			if (FileSystem::Exists(path) && !FileSystem::Remove(path))
				ST_CORE_WARN("Could not remove '{}' after its import failed", file);
		}
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
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			if (m_ReservedPaths.contains(ToRelative(path)))
				return fail(fmt::format("'{}' is being written", relativePath));
		}

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

	AssetHandle EditorAssetManager::ReserveNativeAsset(const std::string& relativePath, AssetHandle handle, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return UUID::Null();
		};

		const std::filesystem::path path = ToAbsolute(relativePath);
		const std::string relative = ToRelative(path);
		if (relative.empty())
			return fail(fmt::format("'{}' is outside the asset directory", relativePath));
		if (IsHiddenRelativePath(FileSystem::FromUTF8(relative)))
			return fail(fmt::format("'{}' is in a hidden folder", relative));
		const AssetImporter* importer = AssetImporterRegistry::FindByExtension(GetExtension(path));
		if (!importer || !importer->StoresSourceDirectly() || GetNativeAssetExtension(importer->GetType()).empty())
			return fail(fmt::format("'{}' is not an engine asset file (.stscene, .stprefab, .stmat)", relative));
		if (FileSystem::Exists(path))
			return fail(fmt::format("'{}' already exists", relative));
		if (handle.IsValid() && BuiltinAssets::IsBuiltin(handle))
			return fail(fmt::format("{} is the handle of a built-in asset", handle.ToString()));
		// The registry of handles is asked before the source lock is taken (its own lock comes first elsewhere).
		if (handle.IsValid() && IsHandleValid(handle))
			return fail(fmt::format("The handle {} is taken", handle.ToString()));
		if (!handle.IsValid())
		{
			do
				handle = UUID();
			while (BuiltinAssets::IsBuiltin(handle) || IsHandleValid(handle));
		}

		std::scoped_lock<std::mutex> lock(m_SourceMutex);
		if (m_SourcesByPath.contains(relative))
			return fail(fmt::format("'{}' is an asset already", relative));
		if (m_ReservedPaths.contains(relative))
			return fail(fmt::format("'{}' is being written already", relative));
		if (m_Sources.contains(handle) || m_Reservations.contains(handle))
			return fail(fmt::format("The handle {} is taken", handle.ToString()));
		m_Reservations.emplace(handle, Reservation { relative, importer->GetType() });
		m_ReservedPaths.emplace(relative, handle);
		return handle;
	}

	void EditorAssetManager::ReleaseNativeAssetReservation(AssetHandle handle)
	{
		std::scoped_lock<std::mutex> lock(m_SourceMutex);
		auto it = m_Reservations.find(handle);
		if (it == m_Reservations.end())
			return;
		m_ReservedPaths.erase(it->second.Path);
		m_Reservations.erase(it);
	}

	std::string EditorAssetManager::GetReservedPath(AssetHandle handle) const
	{
		std::scoped_lock<std::mutex> lock(m_SourceMutex);
		auto it = m_Reservations.find(handle);
		return it != m_Reservations.end() ? it->second.Path : std::string();
	}

	Ref<EditorAssetManager::DocumentWriteClaim> EditorAssetManager::ClaimDocumentWrite(AssetHandle handle, std::string* outError)
	{
		Ref<DocumentWriteClaim> claim(new DocumentWriteClaim());
		claim->m_Work = m_Work;
		claim->m_Asset = handle;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			if (auto source = m_Sources.find(handle); source != m_Sources.end() && source->second.Importer->StoresSourceDirectly())
			{
				claim->m_Type = source->second.Type;
				claim->m_Path = source->second.Path;
			}
			else if (auto reservation = m_Reservations.find(handle); reservation != m_Reservations.end())
			{
				claim->m_Type = reservation->second.Type;
				claim->m_Path = reservation->second.Path;
				claim->m_New = true;
			}
			else
			{
				if (outError)
					*outError = fmt::format("{} is no engine asset of the project", handle.ToString());
				return nullptr;
			}
		}
		claim->m_AbsolutePath = ToAbsolute(claim->m_Path);
		if (claim->m_New)
		{
			// The sidecar the asset would get when the editor registered its file (WriteMetaFile).
			const AssetImporter* importer = AssetImporterRegistry::FindByExtension(GetExtension(claim->m_AbsolutePath));
			nlohmann::json meta = nlohmann::json::object();
			meta["Strata"] = { { "Format", "AssetMeta" }, { "Version", c_MetaVersion } };
			meta["Handle"] = UUIDToJson(handle);
			meta["Type"] = AssetTypeToString(claim->m_Type);
			meta["ImportSettings"] = importer ? importer->GetDefaultSettings(claim->m_AbsolutePath) : nlohmann::json::object();
			claim->m_MetaText = JsonUtils::Dump(meta, 1, '\t') + "\n";
		}
		m_Work->Claim(claim->m_Path);
		return claim;
	}

	EditorAssetManager::DocumentWriteClaim::~DocumentWriteClaim()
	{
		Release();
	}

	void EditorAssetManager::DocumentWriteClaim::Release()
	{
		if (m_Released || !m_Work)
			return;
		m_Released = true;
		m_Work->Unclaim(m_Path);
	}

	bool EditorAssetManager::DocumentWriteClaim::Commit(AtomicFileWriter& file, uint64_t contentHash, std::vector<AssetHandle> references,
		WrittenDocument& outWritten, std::string* outError)
	{
		auto fail = [&](std::string message)
		{
			file.Abort();
			if (outError)
				*outError = std::move(message);
			return false;
		};

		std::string error;
		if (!file.Finish(&error))
			return fail(error);
		{
			// Readers of project files hold the shared lock: none has the file open while it is replaced.
			std::unique_lock<std::shared_mutex> fileLock(m_Work->FileMutex);
			if (m_New)
			{
				// Something appeared at the reserved place meanwhile: it is never overwritten.
				if (FileSystem::Exists(m_AbsolutePath))
					return fail(fmt::format("'{}' exists already: the new asset was not written", m_Path));
				if (!FileSystem::WriteText(GetMetaPath(m_AbsolutePath), m_MetaText))
					return fail(fmt::format("Could not write '{}{}'", m_Path, c_MetaExtension));
			}
			if (!file.Commit(&error))
			{
				if (m_New)
					FileSystem::Remove(GetMetaPath(m_AbsolutePath));
				return fail(error);
			}
		}

		outWritten.Size = FileSystem::GetFileSize(m_AbsolutePath).value_or(file.GetBytesWritten());
		outWritten.Time = FileSystem::GetLastWriteTime(m_AbsolutePath).value_or(0);
		// The record is a cache: when it cannot be written, the next scan searches the document again.
		m_Work->Records.SetRecord(m_Asset, MakeNativeRecord(m_Path, outWritten.Size, outWritten.Time, contentHash, references));
		outWritten.References = std::move(references);
		return true;
	}

	void EditorAssetManager::PublishWrittenDocument(const WrittenDocument& written)
	{
		if (!written.Claim)
			return;
		const DocumentWriteClaim& claim = *written.Claim;
		const AssetHandle handle = claim.GetAsset();
		bool registered = false;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			if (claim.IsNewAsset())
			{
				auto reservation = m_Reservations.find(handle);
				if (reservation != m_Reservations.end())
				{
					m_ReservedPaths.erase(reservation->second.Path);
					m_Reservations.erase(reservation);
				}
				if (!m_Sources.contains(handle) && !m_SourcesByPath.contains(claim.GetPath()))
				{
					SourceAsset source;
					source.Handle = handle;
					source.Path = claim.GetPath();
					source.Importer = AssetImporterRegistry::FindByExtension(GetExtension(claim.GetAbsolutePath()));
					source.Type = claim.GetType();
					source.Settings = source.Importer ? source.Importer->GetDefaultSettings(claim.GetAbsolutePath()) : nlohmann::json::object();
					source.Import.Imported = true; // Read as-is: nothing to import
					source.SourceSize = written.Size;
					source.SourceTime = written.Time;
					m_SourcesByPath[source.Path] = handle;
					const std::filesystem::path folder = FileSystem::FromUTF8(source.Path).parent_path();
					if (!folder.empty())
						AddFolderLocked(FileSystem::ToUTF8(folder));
					AddToFolderIndex(handle, source.Path);
					m_Work->Records.SetPath(handle, source.Path);
					m_Sources.emplace(handle, std::move(source));
					registered = true;
				}
			}
			else if (auto it = m_Sources.find(handle); it != m_Sources.end())
			{
				it->second.SourceSize = written.Size;
				it->second.SourceTime = written.Time;
				it->second.Import.Error.clear();
				it->second.ReferencesGeneration++; // A search queued before found the previous contents
			}
		}
		if (registered)
		{
			AssetMetadata metadata;
			metadata.Handle = handle;
			metadata.Type = claim.GetType();
			metadata.Path = claim.GetPath();
			metadata.Name = FileSystem::ToUTF8(claim.GetAbsolutePath().stem());
			metadata.StoredSize = written.Size;
			RegisterAsset(metadata);
		}
		else
		{
			SetStoredSize(handle, written.Size);
		}
		MarkChanged();
		RecordReferences(handle, written.References, std::nullopt);
		if (written.Reload && !registered)
			ReloadAsset(handle);
		written.Claim->Release();
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

		std::unique_lock<std::mutex> lock(m_SourceMutex);
		auto it = m_Sources.find(handle);
		if (it == m_Sources.end())
			return fail("Not a source asset of the project");
		if (destinationRelative == it->second.Path)
			return true; // Where it is already
		const std::filesystem::path source = ToAbsolute(it->second.Path);
		// A name that differs only in case names the asset's own file where the file system ignores case: renamed in place.
		if (FileSystem::Exists(destination) && !FileSystem::IsSameFile(source, destination))
			return fail(fmt::format("'{}' already exists", destinationRelative));
		if (AssetImporterRegistry::FindByExtension(GetExtension(destination)) != it->second.Importer)
			return fail("Moving cannot change the file type");

		// An import of the asset reads the file at its old place: it is cancelled before the file moves, so it stores nothing
		// it read there, and imports again once the move is done (at the old place if the move fails). Other imports go on.
		const Ref<BackgroundJob> stopped = StopImport(it->second);
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
			RemoveFromFolderIndex(handle, it->second.Path);
			it->second.Path = destinationRelative;
			it->second.PathGeneration++;
			m_SourcesByPath[destinationRelative] = handle;
			AddToFolderIndex(handle, destinationRelative);
			m_Work->Records.SetPath(handle, destinationRelative);
			MarkChanged();
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

	std::optional<nlohmann::json> EditorAssetManager::GetImportRecord(AssetHandle handle) const
	{
		return m_Work->Records.FindRecord(handle);
	}

	bool EditorAssetManager::FlushImportRecords(std::string* outError)
	{
		return m_Work->Records.Flush(outError);
	}

	bool EditorAssetManager::HasPendingImports() const
	{
		// A scan queues the imports of what it finds once it is applied. Searches of what documents reference are not imports
		// (HasPendingReferenceSearches).
		if (m_Scan)
			return true;
		std::scoped_lock<std::mutex> lock(m_Work->Mutex);
		return !m_Work->CompletedImports.empty()
			|| std::any_of(m_Work->Jobs.begin(), m_Work->Jobs.end(), [](const Ref<BackgroundJob>& job) { return !job->Search; });
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
		// Documents of entities are cooked (and scenes adjusted) by the same read that finds their references.
		AssetPackCookOptions cook;
		cook.Cook = options.CookDocuments;
		cook.AdjustSceneSettings = options.AdjustSceneSettings;
		return AssetPackWriter::Write(packPath, plan.Assets, [this, &planned, &describe, &cook](const AssetMetadata& metadata, std::vector<uint8_t>& outData,
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
			const bool entityDocument = metadata.Type == AssetType::Scene || metadata.Type == AssetType::Prefab || metadata.Type == AssetType::Model;
			const bool rewrite = entityDocument && (cook.Cook || (metadata.Type == AssetType::Scene && cook.AdjustSceneSettings));
			if (!rewrite && (!AssetDependencies::CanReference(metadata.Type) || source == planned.end()))
				return true;

			std::vector<AssetHandle> references;
			std::string scanError;
			const bool read = rewrite ? CookForPack(metadata, outData, cook, references, &scanError) : AssetDependencies::Scan(metadata.Type, outData, references, &scanError);
			if (!read)
				return fail(fmt::format("it cannot be read as a document ({}); fix it and export again", scanError));
			if (source == planned.end())
				return true;
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

	////////////////////////////////////////////////////////////////////////////////
	// References
	////////////////////////////////////////////////////////////////////////////////

	void EditorAssetManager::RecordReferences(AssetHandle handle, std::span<const AssetHandle> references, std::optional<AABB> bounds)
	{
		SetAssetReferences(handle, references, bounds);
		std::vector<AssetHandle> normalized = NormalizeReferences(handle, references);
		std::scoped_lock<std::mutex> lock(m_ReferenceMutex);
		if (auto previous = m_RecordedReferences.find(handle); previous != m_RecordedReferences.end())
		{
			for (AssetHandle reference : previous->second)
			{
				auto referencers = m_Referencers.find(reference);
				if (referencers == m_Referencers.end())
					continue;
				referencers->second.erase(handle);
				if (referencers->second.empty())
					m_Referencers.erase(referencers);
			}
			m_RecordedReferences.erase(previous);
		}
		for (AssetHandle reference : normalized)
			m_Referencers[reference].insert(handle);
		if (!normalized.empty())
			m_RecordedReferences.emplace(handle, std::move(normalized));
	}

	void EditorAssetManager::ForgetReferences(AssetHandle handle)
	{
		std::scoped_lock<std::mutex> lock(m_ReferenceMutex);
		auto previous = m_RecordedReferences.find(handle);
		if (previous == m_RecordedReferences.end())
			return;
		for (AssetHandle reference : previous->second)
		{
			auto referencers = m_Referencers.find(reference);
			if (referencers == m_Referencers.end())
				continue;
			referencers->second.erase(handle);
			if (referencers->second.empty())
				m_Referencers.erase(referencers);
		}
		m_RecordedReferences.erase(previous);
	}

	std::vector<AssetHandle> EditorAssetManager::GetReferencers(AssetHandle handle) const
	{
		std::vector<AssetHandle> referencers;
		{
			std::scoped_lock<std::mutex> lock(m_ReferenceMutex);
			auto it = m_Referencers.find(handle);
			if (it != m_Referencers.end())
				referencers.assign(it->second.begin(), it->second.end());
		}
		std::sort(referencers.begin(), referencers.end());
		return referencers;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Folders
	////////////////////////////////////////////////////////////////////////////////

	void EditorAssetManager::AddFolderLocked(const std::string& relativePath)
	{
		// Parents are indexed with their children, so the walk up stops at the first folder known already.
		std::string folder = relativePath;
		while (!folder.empty() && folder != "." && m_Folders.insert(folder).second)
			folder = GetParentFolder(folder);
	}

	void EditorAssetManager::AddToFolderIndex(AssetHandle handle, const std::string& path)
	{
		const std::string folder = GetParentFolder(path);
		m_SourcesByFolder[folder].insert(handle);
		AddFolderLocked(folder);
	}

	void EditorAssetManager::RemoveFromFolderIndex(AssetHandle handle, const std::string& path)
	{
		auto it = m_SourcesByFolder.find(GetParentFolder(path));
		if (it == m_SourcesByFolder.end())
			return;
		it->second.erase(handle);
		if (it->second.empty())
			m_SourcesByFolder.erase(it);
	}

	void EditorAssetManager::IndexFoldersBelowLocked(const std::filesystem::path& directory)
	{
		std::error_code error;
		std::filesystem::recursive_directory_iterator iterator(directory, std::filesystem::directory_options::skip_permission_denied, error);
		const std::filesystem::recursive_directory_iterator end;
		while (!error && iterator != end)
		{
			const std::filesystem::directory_entry& entry = *iterator;
			if (entry.is_directory(error))
			{
				if (IsHiddenName(entry.path().filename()))
					iterator.disable_recursion_pending();
				else
					AddFolderLocked(ToRelative(entry.path()));
			}
			iterator.increment(error);
		}
	}

	std::vector<std::string> EditorAssetManager::GetFolders() const
	{
		std::scoped_lock<std::mutex> lock(m_SourceMutex);
		return std::vector<std::string>(m_Folders.begin(), m_Folders.end());
	}

	bool EditorAssetManager::HasFolder(std::string_view relativePath) const
	{
		if (relativePath.empty())
			return true;
		std::scoped_lock<std::mutex> lock(m_SourceMutex);
		return m_Folders.contains(std::string(relativePath));
	}

	std::vector<AssetHandle> EditorAssetManager::GetAssetsInFolder(std::string_view relativePath) const
	{
		std::vector<std::pair<std::string, AssetHandle>> assets;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_SourcesByFolder.find(std::string(relativePath));
			if (it == m_SourcesByFolder.end())
				return {};
			assets.reserve(it->second.size());
			for (AssetHandle handle : it->second)
				assets.emplace_back(m_Sources.at(handle).Path, handle);
		}
		std::sort(assets.begin(), assets.end());
		std::vector<AssetHandle> handles;
		handles.reserve(assets.size());
		for (const auto& [path, handle] : assets)
			handles.push_back(handle);
		return handles;
	}

	std::vector<AssetHandle> EditorAssetManager::GetAssetsBelowFolder(std::string_view relativePath) const
	{
		std::vector<std::pair<std::string, AssetHandle>> assets;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			for (const auto& [folder, handles] : m_SourcesByFolder)
			{
				if (!IsInFolder(folder, relativePath))
					continue;
				for (AssetHandle handle : handles)
					assets.emplace_back(m_Sources.at(handle).Path, handle);
			}
		}
		std::sort(assets.begin(), assets.end());
		std::vector<AssetHandle> handles;
		handles.reserve(assets.size());
		for (const auto& [path, handle] : assets)
			handles.push_back(handle);
		return handles;
	}

	std::vector<AssetListing> EditorAssetManager::ListAssets(std::string_view relativePath, bool recursive) const
	{
		std::vector<AssetListing> listings;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto add = [&](const std::unordered_set<AssetHandle>& handles)
			{
				for (AssetHandle handle : handles)
				{
					const SourceAsset& source = m_Sources.at(handle);
					listings.push_back({ handle, source.Type, source.Path, source.Import.Error, source.Import.Warnings.size(), source.Import.SubAssets,
						source.ImportJob != nullptr });
				}
			};
			if (!recursive)
			{
				auto it = m_SourcesByFolder.find(std::string(relativePath));
				if (it != m_SourcesByFolder.end())
					add(it->second);
			}
			else
			{
				for (const auto& [folder, handles] : m_SourcesByFolder)
				{
					if (IsInFolder(folder, relativePath))
						add(handles);
				}
			}
		}
		std::sort(listings.begin(), listings.end(), [](const AssetListing& a, const AssetListing& b) { return a.Path < b.Path; });
		return listings;
	}

	void EditorAssetManager::ProcessFolderChange(const std::filesystem::path& path, FileChangeType type)
	{
		const std::string relative = ToRelative(path);
		if (relative.empty() || relative == "." || IsHiddenRelativePath(FileSystem::FromUTF8(relative)))
			return;
		if (type == FileChangeType::Removed)
		{
			// Gone (not moved back meanwhile): its folders leave the index, and so do its assets whose files went with it
			// (a folder deleted in a file manager may be reported without its files). The editor's own moves and deletes
			// updated the index already, so they find nothing here.
			if (FileSystem::Exists(path))
				return;
			for (AssetHandle handle : GetAssetsBelowFolder(relative))
			{
				const std::optional<AssetMetadata> metadata = GetMetadata(handle);
				if (metadata && !FileSystem::Exists(ToAbsolute(metadata->Path)))
				{
					UnregisterSource(handle, false);
					ST_CORE_INFO("Asset '{}' removed", metadata->Path);
				}
			}
			bool removed = false;
			{
				std::scoped_lock<std::mutex> lock(m_SourceMutex);
				removed = std::erase_if(m_Folders, [&relative](const std::string& folder) { return IsInFolder(folder, relative); }) > 0;
			}
			if (removed)
				MarkChanged();
			return;
		}
		if (!FileSystem::IsDirectory(path))
			return;
		bool added = false;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			if (!m_Folders.contains(relative))
			{
				AddFolderLocked(relative);
				IndexFoldersBelowLocked(path);
				added = true;
			}
		}
		if (added)
			MarkChanged();
	}

	bool EditorAssetManager::CreateFolder(const std::string& relativePath, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		const std::filesystem::path absolute = ToAbsolute(NormalizeIncludePath(relativePath));
		const std::string relative = ToRelative(absolute);
		if (relative.empty() || relative == ".")
			return fail(fmt::format("'{}' is no folder inside the asset directory", relativePath));
		if (IsHiddenRelativePath(FileSystem::FromUTF8(relative)))
			return fail(fmt::format("'{}' is hidden (a name starts with '.'), so it could hold no assets", relative));
		if (FileSystem::Exists(absolute))
			return fail(fmt::format("'{}' exists already", relative));
		if (!FileSystem::CreateDirectories(absolute))
			return fail(fmt::format("Could not create the folder '{}' (is the name valid?)", relative));
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			AddFolderLocked(relative);
		}
		MarkChanged();
		return true;
	}

	bool EditorAssetManager::RemoveEmptyFolder(const std::string& relativePath, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		const std::filesystem::path absolute = ToAbsolute(NormalizeIncludePath(relativePath));
		const std::string relative = ToRelative(absolute);
		if (relative.empty() || relative == "." || !FileSystem::IsDirectory(absolute))
			return fail(fmt::format("'{}' is no folder of the asset directory", relativePath));
		std::error_code error;
		if (std::filesystem::directory_iterator(absolute, error) != std::filesystem::directory_iterator() || error)
			return fail(fmt::format("'{}' is not empty", relative));
		if (!std::filesystem::remove(absolute, error) || error)
			return fail(fmt::format("Could not remove the folder '{}'", relative));
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			m_Folders.erase(relative);
		}
		MarkChanged();
		return true;
	}

	bool EditorAssetManager::MoveFolder(const std::string& from, const std::string& to, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		const std::filesystem::path source = ToAbsolute(NormalizeIncludePath(from));
		const std::filesystem::path destination = ToAbsolute(NormalizeIncludePath(to));
		const std::string sourceRelative = ToRelative(source);
		const std::string destinationRelative = ToRelative(destination);
		if (sourceRelative.empty() || sourceRelative == "." || !FileSystem::IsDirectory(source))
			return fail(fmt::format("'{}' is no folder of the asset directory", from));
		if (destinationRelative.empty() || destinationRelative == ".")
			return fail(fmt::format("'{}' is outside the asset directory", to));
		if (IsHiddenRelativePath(FileSystem::FromUTF8(destinationRelative)))
			return fail(fmt::format("'{}' is hidden (a name starts with '.'), so it could hold no assets", destinationRelative));
		if (destinationRelative == sourceRelative)
			return true; // Where it is already
		if (IsInFolder(destinationRelative, sourceRelative))
			return fail(fmt::format("'{}' cannot move into itself ('{}')", sourceRelative, destinationRelative));
		// A name that differs only in case names the folder itself where the file system ignores case: renamed in place.
		if (FileSystem::Exists(destination) && !FileSystem::IsSameFile(source, destination))
			return fail(fmt::format("'{}' exists already", destinationRelative));

		struct MovedSource
		{
			AssetHandle Handle;
			std::string To;
			std::vector<AssetHandle> SubAssets;
			Ref<BackgroundJob> Stopped; // Its import, cancelled before the move: it runs again at the new place
			bool ReadsOtherFiles = false; // Its import read files beside it, which it now finds at other paths
		};
		std::vector<MovedSource> moved;
		std::vector<AssetHandle> dependents; // Imports of assets outside the folder that read files in it
		std::string failure;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			for (auto& [handle, asset] : m_Sources)
			{
				if (IsInFolder(asset.Path, sourceRelative))
					moved.push_back({ handle, ReplaceFolder(asset.Path, sourceRelative, destinationRelative), asset.Import.SubAssets, StopImport(asset), !asset.Dependencies.empty() });
			}
			const std::string sourceKey = GetDependencyKey(sourceRelative);
			for (const auto& [key, handles] : m_DependentsByPath)
			{
				if (IsInFolder(key, sourceKey))
					dependents.insert(dependents.end(), handles.begin(), handles.end());
			}

			{
				// Readers of the files (loads, imports) hold the shared lock: the folder moves while nobody reads in it.
				std::unique_lock<std::shared_mutex> fileLock(m_Work->FileMutex);
				if (!FileSystem::CreateDirectories(destination.parent_path()) || !FileSystem::Rename(source, destination))
					failure = fmt::format("Could not move '{}' to '{}'", sourceRelative, destinationRelative);
			}
			if (failure.empty())
			{
				for (const MovedSource& entry : moved)
				{
					SourceAsset& asset = m_Sources.at(entry.Handle);
					m_SourcesByPath.erase(asset.Path);
					RemoveFromFolderIndex(entry.Handle, asset.Path);
					asset.Path = entry.To;
					asset.PathGeneration++;
					m_SourcesByPath[entry.To] = entry.Handle;
					AddToFolderIndex(entry.Handle, entry.To);
					m_Work->Records.SetPath(entry.Handle, entry.To);
				}
				std::vector<std::string> folders;
				for (auto it = m_Folders.begin(); it != m_Folders.end();)
				{
					if (IsInFolder(*it, sourceRelative))
					{
						folders.push_back(*it);
						it = m_Folders.erase(it);
					}
					else
					{
						++it;
					}
				}
				for (const std::string& folder : folders)
					AddFolderLocked(ReplaceFolder(folder, sourceRelative, destinationRelative));
				AddFolderLocked(destinationRelative);
			}
		}
		MarkChanged();

		// Cancelled imports run again: at the new place, or at the old one when nothing moved.
		for (const MovedSource& entry : moved)
		{
			if (entry.Stopped)
				QueueImport(entry.Handle, entry.Stopped->Force, true);
		}
		if (!failure.empty())
			return fail(std::move(failure));

		for (const MovedSource& entry : moved)
		{
			if (std::optional<AssetMetadata> metadata = GetMetadata(entry.Handle))
			{
				metadata->Path = entry.To;
				UpdateAssetMetadata(*metadata);
			}
			for (AssetHandle subAsset : entry.SubAssets)
			{
				if (std::optional<AssetMetadata> metadata = GetMetadata(subAsset))
				{
					metadata->Path = entry.To;
					UpdateAssetMetadata(*metadata);
				}
			}
			// A model's record names the files it read by their old paths: imported again, it records the new ones (and its
			// cached result stays loaded meanwhile).
			if (entry.ReadsOtherFiles && !entry.Stopped)
				QueueImport(entry.Handle);
		}
		for (AssetHandle dependent : dependents)
			QueueImport(dependent);
		return true;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Trash
	////////////////////////////////////////////////////////////////////////////////

	AssetHandle EditorAssetManager::RegisterAddedFile(const std::filesystem::path& absolutePath, bool queueImport)
	{
		const AssetHandle handle = RegisterSourceFile(absolutePath);
		if (!handle.IsValid())
			return handle;

		ImportRequest request;
		bool direct = false;
		bool imported = false;
		AssetType type = AssetType::None;
		std::string path;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_Sources.find(handle);
			if (it == m_Sources.end())
				return UUID::Null();
			direct = it->second.Importer->StoresSourceDirectly();
			imported = it->second.Import.Imported || it->second.ImportJob != nullptr;
			type = it->second.Type;
			path = it->second.Path;
			if (!direct)
				request = MakeImportRequest(it->second);
		}
		if (direct)
		{
			// What it references: from its record, which came back with it, or found again (small documents).
			ApplyNativeReferences(m_Work->FindNativeReferences(handle, type, path));
			return handle;
		}
		if (imported)
			return handle;
		// An import cached with the file (it came back with it) is current: taken at once, nothing is imported anew.
		ImportOutcome outcome;
		if (m_Work->LoadCurrentImport(request, outcome))
			ApplyImportOutcome(std::move(outcome), false);
		else if (queueImport)
			QueueImport(handle);
		return handle;
	}

	bool EditorAssetManager::MoveToTrash(const std::string& relativePath, TrashedAssets& outTrashed, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		if (m_Specification.TrashDirectory.empty())
			return fail("The project has no trash folder");
		const std::filesystem::path absolute = ToAbsolute(NormalizeIncludePath(relativePath));
		const std::string relative = ToRelative(absolute);
		if (relative.empty() || relative == ".")
			return fail(fmt::format("'{}' is no asset or folder inside the asset directory", relativePath));
		const bool folder = FileSystem::IsDirectory(absolute);
		std::vector<AssetHandle> handles;
		if (folder)
		{
			handles = GetAssetsBelowFolder(relative);
		}
		else
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			auto it = m_SourcesByPath.find(relative);
			if (it == m_SourcesByPath.end())
				return fail(fmt::format("'{}' is no asset of the project", relative));
			handles.push_back(it->second);
		}

		// A new folder of the trash, named after the time (made unique within a second).
		const std::string timestamp = MakeTrashTimestamp();
		std::filesystem::path entry = m_Specification.TrashDirectory / FileSystem::FromUTF8(timestamp);
		for (uint32_t attempt = 1; FileSystem::Exists(entry); attempt++)
			entry = m_Specification.TrashDirectory / FileSystem::FromUTF8(fmt::format("{}-{}", timestamp, attempt));
		const std::filesystem::path stored = entry / c_TrashAssetsFolder / FileSystem::FromUTF8(relative);
		const std::filesystem::path cache = entry / c_TrashCacheFolder;
		if (!FileSystem::CreateDirectories(stored.parent_path()) || !FileSystem::CreateDirectories(cache))
			return fail(fmt::format("Could not create the trash folder '{}'", FileSystem::ToUTF8(entry)));

		// The cached imports go with the assets, so that a restore needs no import. Imports that run are cancelled first: they
		// would publish into the cache after the move.
		std::vector<std::pair<AssetHandle, Ref<BackgroundJob>>> stopped;
		std::vector<std::filesystem::path> cacheFiles;
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			for (AssetHandle handle : handles)
			{
				auto it = m_Sources.find(handle);
				if (it == m_Sources.end())
					continue;
				if (Ref<BackgroundJob> job = StopImport(it->second))
					stopped.emplace_back(handle, job);
				cacheFiles.push_back(GetCachePath(handle));
				for (AssetHandle subAsset : it->second.Import.SubAssets)
					cacheFiles.push_back(GetCachePath(subAsset));
			}
		}

		std::string failure;
		{
			std::unique_lock<std::shared_mutex> fileLock(m_Work->FileMutex);
			if (!FileSystem::Rename(absolute, stored))
			{
				failure = fmt::format("Could not move '{}' to the trash (is a file in it open in another program?)", relative);
			}
			else if (!folder && FileSystem::Exists(GetMetaPath(absolute)) && !FileSystem::Rename(GetMetaPath(absolute), GetMetaPath(stored)))
			{
				FileSystem::Rename(stored, absolute);
				failure = fmt::format("Could not move the .meta file of '{}' to the trash", relative);
			}
			else
			{
				// Derived data: a cached import that stays behind is imported again on a restore. The records go with them, as a
				// file each, and leave the store.
				for (const std::filesystem::path& file : cacheFiles)
				{
					if (FileSystem::Exists(file) && !FileSystem::Rename(file, cache / file.filename()))
						FileSystem::Remove(file);
				}
				for (AssetHandle handle : handles)
				{
					if (const std::optional<nlohmann::json> record = m_Work->Records.FindRecord(handle))
						FileSystem::WriteText(cache / FileSystem::FromUTF8(handle.ToString() + std::string(c_TrashRecordExtension)), JsonUtils::Dump(*record));
					m_Work->Records.Remove(handle);
				}
			}
		}
		if (!failure.empty())
		{
			FileSystem::Remove(entry);
			// Nothing moved: the imports that were cancelled run again.
			for (const auto& [handle, job] : stopped)
				QueueImport(handle, job->Force, true);
			return fail(std::move(failure));
		}

		for (AssetHandle handle : handles)
			UnregisterSource(handle, false);
		if (folder)
		{
			std::scoped_lock<std::mutex> lock(m_SourceMutex);
			std::erase_if(m_Folders, [&relative](const std::string& path) { return IsInFolder(path, relative); });
		}
		MarkChanged();

		nlohmann::json handleList = nlohmann::json::array();
		for (AssetHandle handle : handles)
			handleList.push_back(UUIDToJson(handle));
		const nlohmann::json manifest = { { "Format", "Trashed" }, { "Version", 1 }, { "Path", relative }, { "Folder", folder }, { "Handles", std::move(handleList) },
			{ "Time", timestamp } };
		if (!FileSystem::WriteText(entry / c_TrashManifest, JsonUtils::Dump(manifest, 1, '\t')))
			ST_CORE_WARN("Could not describe the trash folder '{}'", FileSystem::ToUTF8(entry));
		ST_CORE_INFO("Moved '{}' to the trash ({})", relative, FileSystem::ToUTF8(entry));

		outTrashed = TrashedAssets { relative, folder, entry, std::move(handles) };
		return true;
	}

	bool EditorAssetManager::RestoreFromTrash(const TrashedAssets& trashed, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		const std::filesystem::path destination = ToAbsolute(trashed.Path);
		const std::filesystem::path stored = trashed.Entry / c_TrashAssetsFolder / FileSystem::FromUTF8(trashed.Path);
		if (!FileSystem::Exists(stored))
			return fail(fmt::format("The trash no longer holds '{}' ({} is gone)", trashed.Path, FileSystem::ToUTF8(trashed.Entry)));
		if (FileSystem::Exists(destination))
			return fail(fmt::format("Something else is at '{}' now; move or delete it first", trashed.Path));

		{
			std::unique_lock<std::shared_mutex> fileLock(m_Work->FileMutex);
			if (!FileSystem::CreateDirectories(destination.parent_path()) || !FileSystem::Rename(stored, destination))
				return fail(fmt::format("Could not move '{}' back from the trash", trashed.Path));
			if (!trashed.Folder && FileSystem::Exists(GetMetaPath(stored)) && !FileSystem::Rename(GetMetaPath(stored), GetMetaPath(destination)))
			{
				FileSystem::Rename(destination, stored);
				return fail(fmt::format("Could not move the .meta file of '{}' back from the trash", trashed.Path));
			}
			// The cached imports come back with them, unless an import wrote new ones meanwhile; their records go into the store.
			std::error_code error;
			for (std::filesystem::directory_iterator it(trashed.Entry / c_TrashCacheFolder, error), end; !error && it != end; it.increment(error))
			{
				const std::filesystem::path& file = it->path();
				if (FileSystem::ToUTF8(file.extension()) == c_TrashRecordExtension)
				{
					const std::optional<UUID> handle = UUID::FromString(FileSystem::ToUTF8(file.stem()));
					const std::optional<std::string> text = FileSystem::ReadText(file);
					const std::optional<nlohmann::json> record = text ? JsonUtils::Parse(*text) : std::nullopt;
					if (handle && record && record->is_object() && !m_Work->Records.FindRecord(*handle))
						m_Work->Records.SetRecord(*handle, *record);
					continue;
				}
				const std::filesystem::path target = m_Specification.CacheDirectory / file.filename();
				if (!FileSystem::Exists(target))
					FileSystem::Rename(file, target);
			}
		}

		if (trashed.Folder)
		{
			std::vector<std::filesystem::path> files;
			std::error_code error;
			std::filesystem::recursive_directory_iterator iterator(destination, std::filesystem::directory_options::skip_permission_denied, error);
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
			std::sort(files.begin(), files.end());
			{
				std::scoped_lock<std::mutex> lock(m_SourceMutex);
				AddFolderLocked(trashed.Path);
				IndexFoldersBelowLocked(destination);
			}
			for (const std::filesystem::path& file : files)
				RegisterAddedFile(file, true);
		}
		else
		{
			RegisterAddedFile(destination, true);
		}
		FileSystem::Remove(trashed.Entry);
		MarkChanged();
		ST_CORE_INFO("Restored '{}' from the trash", trashed.Path);
		return true;
	}

	void EditorAssetManager::PurgeTrash(std::chrono::hours maxAge)
	{
		if (m_Specification.TrashDirectory.empty() || !FileSystem::IsDirectory(m_Specification.TrashDirectory))
			return;
		auto purge = [directory = m_Specification.TrashDirectory, maxAge]()
		{
			using Clock = std::filesystem::file_time_type::clock;
			const int64_t now = Clock::now().time_since_epoch().count();
			const int64_t limit = std::chrono::duration_cast<Clock::duration>(maxAge).count();
			std::error_code error;
			for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
			{
				std::error_code typeError;
				if (!it->is_directory(typeError))
					continue;
				const std::optional<int64_t> time = FileSystem::GetLastWriteTime(it->path() / c_TrashManifest);
				if (time && now - *time > limit && FileSystem::Remove(it->path()))
					ST_CORE_INFO("Removed '{}' from the trash: it is older than {} days", FileSystem::ToUTF8(it->path().filename()), maxAge.count() / 24);
			}
		};
		try
		{
			JobSystem::SubmitIO(std::move(purge), JobPriority::Low);
		}
		catch (const std::exception& exception)
		{
			ST_CORE_WARN("Could not queue the cleanup of the trash: {}", exception.what());
		}
	}

}
