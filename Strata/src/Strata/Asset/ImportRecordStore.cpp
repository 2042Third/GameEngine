#include "stpch.h"
#include "Strata/Asset/ImportRecordStore.h"

#include "Strata/Core/AtomicFileWriter.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Hash.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Log.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <span>
#include <system_error>
#include <utility>

namespace Strata
{

	namespace
	{

		constexpr std::string_view c_LegacyRecordExtension = ".import";
		constexpr std::string_view c_LegacyIndexFile = "AssetIndex.json";

		std::string MakeHeader()
		{
			return fmt::format("{{\"Format\":\"ImportRecords\",\"Version\":{}}}", ImportRecordStore::c_Version);
		}

		uint32_t Checksum(std::string_view text)
		{
			return Hash::CRC32C(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
		}

		// The JSON of an entry line, when its checksum holds.
		std::optional<std::string_view> CheckLine(std::string_view line)
		{
			constexpr size_t c_ChecksumDigits = 8;
			if (line.size() <= c_ChecksumDigits + 1 || line[c_ChecksumDigits] != ' ')
				return std::nullopt;
			uint32_t expected = 0;
			const std::from_chars_result parsed = std::from_chars(line.data(), line.data() + c_ChecksumDigits, expected, 16);
			if (parsed.ec != std::errc() || parsed.ptr != line.data() + c_ChecksumDigits)
				return std::nullopt;
			const std::string_view json = line.substr(c_ChecksumDigits + 1);
			if (Checksum(json) != expected)
				return std::nullopt;
			return json;
		}

	}

	struct ImportRecordStore::Shared
	{
		std::filesystem::path Directory;
		std::filesystem::path File;

		std::once_flag LoadOnce;
		mutable std::mutex Mutex;
		std::condition_variable Condition;
		// The JSON text of each handle's latest record entry ({"Handle", "Record"}), parsed when asked for; and each handle's path.
		std::unordered_map<AssetHandle, std::string> Records;
		std::unordered_map<AssetHandle, std::string> Paths;

		// Lines waiting for the writer (each with its line feed), and how many entries were queued and written (or lost to a
		// failed write) so far.
		std::string Queue;
		uint64_t Queued = 0;
		uint64_t Written = 0;
		bool WriterActive = false;
		uint64_t WriterGeneration = 0; // Of the writer that runs (or ran last)
		JobHandle Writer;              // Of the writer that runs, once its submitter stored it

		bool HeaderWritten = false; // The file exists with a valid header (else the next append creates it with one)
		bool RewriteNeeded = false; // The file lost entries (a failed write) or was unreadable: the next write rewrites it whole
		size_t FileEntries = 0;     // Entry lines in the file, live or dead
		uint64_t FileBytes = 0;
		uint64_t Rewrites = 0;
		std::string WriteError;     // Since the last Flush

		// Files of the earlier layout, removed once the entries that took them over are written (by their sequence).
		std::vector<std::pair<uint64_t, std::filesystem::path>> PendingRemovals;

		// Requires Mutex.
		void QueueLine(std::string_view json)
		{
			Queue += FormatLine(json);
			Queue += '\n';
			Queued++;
		}

		size_t GetLiveCountLocked() const { return Records.size() + Paths.size(); }

		bool NeedsCompactionLocked() const
		{
			const size_t live = GetLiveCountLocked();
			const size_t dead = FileEntries > live ? FileEntries - live : 0;
			return dead > live && FileBytes > c_CompactMinBytes;
		}

		// The whole file as the in-memory state says it (requires Mutex): the header, then each handle's path and record.
		std::string MakeContentsLocked(size_t& outEntries) const
		{
			std::string contents = MakeHeader();
			contents += '\n';
			outEntries = 0;
			for (const auto& [handle, path] : Paths)
			{
				contents += FormatLine(JsonUtils::Dump(nlohmann::json { { "Handle", handle.ToString() }, { "Path", path } }));
				contents += '\n';
				outEntries++;
			}
			for (const auto& [handle, entry] : Records)
			{
				contents += FormatLine(entry);
				contents += '\n';
				outEntries++;
			}
			return contents;
		}

		// Makes the caller the one that starts the writer, unless one runs (requires Mutex). Returns the writer's generation, or
		// 0 when one runs.
		uint64_t ClaimWriterLocked()
		{
			if (WriterActive)
				return 0;
			WriterActive = true;
			return ++WriterGeneration;
		}

		static void Submit(const std::shared_ptr<Shared>& shared, uint64_t generation)
		{
			JobHandle handle;
			try
			{
				handle = JobSystem::SubmitIO([shared]() { Drain(*shared); }, JobPriority::Normal);
			}
			catch (const std::exception& exception)
			{
				ST_CORE_ERROR("Could not queue the write of the import records: {}", exception.what());
				Drain(*shared);
				return;
			}
			std::scoped_lock<std::mutex> lock(shared->Mutex);
			// Only while that writer runs (it may have ended already, and another may have started since).
			if (shared->WriterActive && shared->WriterGeneration == generation)
				shared->Writer = std::move(handle);
			shared->Condition.notify_all();
		}

		// Requires Mutex: the writer stops.
		void StopWriterLocked()
		{
			WriterActive = false;
			Writer = JobHandle();
			Condition.notify_all();
		}

		// The writer: appends what is queued, batch after batch, rewrites the file when that is due and removes the files of
		// the earlier layout whose entries are written, until nothing is left.
		static void Drain(Shared& shared)
		{
			for (;;)
			{
				std::string lines;
				std::string contents;
				size_t contentEntries = 0;
				uint64_t batchEnd = 0;
				bool rewrite = false;
				std::vector<std::pair<uint64_t, std::filesystem::path>> removals;
				{
					std::scoped_lock<std::mutex> lock(shared.Mutex);
					rewrite = shared.RewriteNeeded || shared.NeedsCompactionLocked();
					batchEnd = shared.Queued;
					// Every entry queued so far is in this batch: the files they took over may go once it is written.
					removals.swap(shared.PendingRemovals);
					if (shared.Queue.empty() && !rewrite && removals.empty())
					{
						shared.StopWriterLocked();
						return;
					}
					lines.swap(shared.Queue);
					// The in-memory state holds every queued entry: a rewrite needs none of the lines.
					if (rewrite)
						contents = shared.MakeContentsLocked(contentEntries);
				}

				std::string error;
				uint64_t bytes = 0;
				if (rewrite)
				{
					AtomicFileWriter file(shared.File);
					if (!file.Write(contents) || !file.Commit(&error))
						error = error.empty() ? file.GetError() : error;
					bytes = contents.size();
				}
				else if (!lines.empty())
				{
					// The first entries create the file, after its header.
					if (!shared.HeaderWritten)
						lines.insert(0, MakeHeader() + "\n");
					std::ofstream stream(shared.File, std::ios::binary | (shared.HeaderWritten ? std::ios::app : std::ios::trunc));
					stream.write(lines.data(), static_cast<std::streamsize>(lines.size()));
					stream.flush();
					if (!stream)
						error = fmt::format("Could not append to '{}'", FileSystem::ToUTF8(shared.File));
					bytes = lines.size();
				}
				if (error.empty())
				{
					for (const auto& [sequence, file] : removals)
						FileSystem::Remove(file);
				}

				{
					std::scoped_lock<std::mutex> lock(shared.Mutex);
					shared.Written = batchEnd;
					if (error.empty())
					{
						if (rewrite)
						{
							shared.FileEntries = contentEntries;
							shared.FileBytes = bytes;
							shared.Rewrites++;
							shared.RewriteNeeded = false;
						}
						else if (!lines.empty())
						{
							shared.FileEntries += static_cast<size_t>(std::count(lines.begin(), lines.end(), '\n')) - (shared.HeaderWritten ? 0 : 1);
							shared.FileBytes = shared.HeaderWritten ? shared.FileBytes + bytes : bytes;
						}
						shared.HeaderWritten = shared.HeaderWritten || rewrite || !lines.empty();
					}
					else
					{
						// The entries are lost: their assets are imported or searched again later, never wrong. The next write
						// rewrites the whole file, which then holds every entry again (and lets the taken-over files go).
						shared.WriteError = error;
						shared.RewriteNeeded = true;
						shared.PendingRemovals.insert(shared.PendingRemovals.end(), std::make_move_iterator(removals.begin()),
							std::make_move_iterator(removals.end()));
						ST_CORE_WARN("The import records could not be written ({}); the assets concerned are imported again when needed", error);
						// The next round would fail the same way: the writer stops until more is queued.
						if (shared.Queue.empty())
						{
							shared.StopWriterLocked();
							return;
						}
					}
					shared.Condition.notify_all();
				}
			}
		}
	};

	ImportRecordStore::ImportRecordStore(std::filesystem::path cacheDirectory)
		: m_Shared(std::make_shared<Shared>())
	{
		m_Shared->Directory = std::move(cacheDirectory);
		m_Shared->File = m_Shared->Directory / c_FileName;
	}

	ImportRecordStore::~ImportRecordStore()
	{
		Flush();
	}

	std::string ImportRecordStore::FormatLine(std::string_view json)
	{
		return fmt::format("{:08x} {}", Checksum(json), json);
	}

	const std::filesystem::path& ImportRecordStore::GetPath() const
	{
		return m_Shared->File;
	}

	ImportRecordLoadStats ImportRecordStore::Load()
	{
		ImportRecordLoadStats stats;
		std::call_once(m_Shared->LoadOnce, [this, &stats]() { stats = LoadFile(); });
		return stats;
	}

	void ImportRecordStore::EnsureLoaded() const
	{
		std::call_once(m_Shared->LoadOnce, [this]() { LoadFile(); });
	}

	ImportRecordLoadStats ImportRecordStore::LoadFile() const
	{
		ImportRecordLoadStats stats;
		Shared& shared = *m_Shared;
		const std::optional<std::string> text = FileSystem::Exists(shared.File) ? FileSystem::ReadText(shared.File) : std::nullopt;

		std::unordered_map<AssetHandle, std::string> records;
		std::unordered_map<AssetHandle, std::string> paths;
		bool headerValid = false;
		size_t entries = 0;
		uint64_t fileBytes = 0;
		if (text && !text->empty())
		{
			const std::string_view all = *text;
			// A last line without its line feed was torn by a crash: it goes, so that appends start on a line of their own.
			const size_t lastFeed = all.rfind('\n');
			const size_t whole = lastFeed == std::string_view::npos ? 0 : lastFeed + 1;
			if (whole < all.size())
			{
				std::error_code error;
				std::filesystem::resize_file(shared.File, whole, error);
				if (error)
					ST_CORE_WARN("Could not cut the torn end of '{}': {}", FileSystem::ToUTF8(shared.File), error.message());
				else
					stats.SkippedLines++;
			}
			fileBytes = whole;

			size_t position = 0;
			bool first = true;
			while (position < whole)
			{
				const size_t end = all.find('\n', position);
				const std::string_view line = all.substr(position, end - position);
				position = end + 1;
				if (first)
				{
					first = false;
					const std::optional<nlohmann::json> header = JsonUtils::Parse(line);
					headerValid = header && header->is_object() && JsonUtils::GetString(*header, "Format") == "ImportRecords"
						&& JsonUtils::GetUInt(*header, "Version", 0) == c_Version;
					if (!headerValid)
						break;
					continue;
				}
				entries++;
				stats.Lines++;
				const std::optional<std::string_view> json = CheckLine(line);
				std::optional<nlohmann::json> entry = json ? JsonUtils::Parse(*json) : std::nullopt;
				const std::optional<UUID> handle = entry && entry->is_object() ? UUID::FromString(JsonUtils::GetString(*entry, "Handle")) : std::nullopt;
				if (!handle || !handle->IsValid())
				{
					stats.SkippedLines++;
					continue;
				}
				if (const nlohmann::json* record = JsonUtils::Find(*entry, "Record"); record && record->is_object())
					records[*handle] = std::string(*json);
				else if (const nlohmann::json* path = JsonUtils::Find(*entry, "Path"); path && path->is_string())
					paths[*handle] = path->get<std::string>();
				else if (JsonUtils::GetBool(*entry, "Removed", false))
				{
					records.erase(*handle);
					paths.erase(*handle);
				}
				else
					stats.SkippedLines++;
			}
			// A file cut back to nothing is simply empty.
			if (!headerValid && whole > 0)
			{
				stats.Discarded = true;
				records.clear();
				paths.clear();
				entries = 0;
				ST_CORE_WARN("'{}' is not an import record file this version reads; it is written anew", FileSystem::ToUTF8(shared.File));
			}
		}

		uint64_t submit = 0;
		{
			std::scoped_lock<std::mutex> lock(shared.Mutex);
			shared.Records = std::move(records);
			shared.Paths = std::move(paths);
			shared.HeaderWritten = headerValid;
			shared.RewriteNeeded = stats.Discarded;
			shared.FileEntries = entries;
			shared.FileBytes = fileBytes;

			// The earlier layout: a record file per asset and the asset index, taken over where the store has nothing.
			std::error_code error;
			for (std::filesystem::directory_iterator it(shared.Directory, error), end; !error && it != end; it.increment(error))
			{
				const std::filesystem::path& file = it->path();
				if (FileSystem::ToUTF8(file.extension()) != c_LegacyRecordExtension)
					continue;
				const std::optional<UUID> handle = UUID::FromString(FileSystem::ToUTF8(file.stem()));
				if (!handle || !handle->IsValid())
					continue;
				if (!shared.Records.contains(*handle))
				{
					const std::optional<std::string> recordText = FileSystem::ReadText(file);
					const std::optional<nlohmann::json> record = recordText ? JsonUtils::Parse(*recordText) : std::nullopt;
					if (record && record->is_object())
					{
						const std::string json = JsonUtils::Dump(nlohmann::json { { "Handle", handle->ToString() }, { "Record", *record } });
						shared.Records[*handle] = json;
						shared.QueueLine(json);
						stats.MigratedRecords++;
					}
				}
				shared.PendingRemovals.emplace_back(shared.Queued, file);
			}
			const std::filesystem::path index = shared.Directory / FileSystem::FromUTF8(c_LegacyIndexFile);
			if (FileSystem::Exists(index))
			{
				const std::optional<std::string> indexText = FileSystem::ReadText(index);
				const std::optional<nlohmann::json> document = indexText ? JsonUtils::Parse(*indexText) : std::nullopt;
				const nlohmann::json* assets = document ? JsonUtils::Find(*document, "Assets") : nullptr;
				if (assets && assets->is_object())
				{
					for (const auto& [handleText, path] : assets->items())
					{
						const std::optional<UUID> handle = UUID::FromString(handleText);
						if (!handle || !handle->IsValid() || !path.is_string() || shared.Paths.contains(*handle))
							continue;
						shared.Paths[*handle] = path.get<std::string>();
						shared.QueueLine(JsonUtils::Dump(nlohmann::json { { "Handle", handle->ToString() }, { "Path", path } }));
						stats.MigratedPaths++;
					}
				}
				shared.PendingRemovals.emplace_back(shared.Queued, index);
			}
			// A discarded file is rewritten, and taken-over files are removed once their entries are written.
			if (!shared.Queue.empty() || !shared.PendingRemovals.empty() || stats.Discarded)
				submit = shared.ClaimWriterLocked();
		}
		if (submit != 0)
			Shared::Submit(m_Shared, submit);
		return stats;
	}

	std::optional<nlohmann::json> ImportRecordStore::FindRecord(AssetHandle handle) const
	{
		EnsureLoaded();
		std::string entry;
		{
			std::scoped_lock<std::mutex> lock(m_Shared->Mutex);
			auto it = m_Shared->Records.find(handle);
			if (it == m_Shared->Records.end())
				return std::nullopt;
			entry = it->second;
		}
		std::optional<nlohmann::json> parsed = JsonUtils::Parse(entry);
		if (!parsed || !parsed->is_object() || !parsed->contains("Record"))
			return std::nullopt;
		return std::move((*parsed)["Record"]);
	}

	std::optional<std::string> ImportRecordStore::FindPath(AssetHandle handle) const
	{
		EnsureLoaded();
		std::scoped_lock<std::mutex> lock(m_Shared->Mutex);
		auto it = m_Shared->Paths.find(handle);
		if (it == m_Shared->Paths.end())
			return std::nullopt;
		return it->second;
	}

	std::unordered_map<AssetHandle, std::string> ImportRecordStore::GetPaths() const
	{
		EnsureLoaded();
		std::scoped_lock<std::mutex> lock(m_Shared->Mutex);
		return m_Shared->Paths;
	}

	std::vector<AssetHandle> ImportRecordStore::GetRecordedHandles() const
	{
		EnsureLoaded();
		std::scoped_lock<std::mutex> lock(m_Shared->Mutex);
		std::vector<AssetHandle> handles;
		handles.reserve(m_Shared->Records.size());
		for (const auto& [handle, entry] : m_Shared->Records)
			handles.push_back(handle);
		return handles;
	}

	void ImportRecordStore::SetRecord(AssetHandle handle, const nlohmann::json& record)
	{
		EnsureLoaded();
		const std::string json = JsonUtils::Dump(nlohmann::json { { "Handle", handle.ToString() }, { "Record", record } });
		uint64_t submit = 0;
		{
			std::scoped_lock<std::mutex> lock(m_Shared->Mutex);
			m_Shared->Records[handle] = json;
			m_Shared->QueueLine(json);
			submit = m_Shared->ClaimWriterLocked();
		}
		if (submit != 0)
			Shared::Submit(m_Shared, submit);
	}

	void ImportRecordStore::SetPath(AssetHandle handle, const std::string& path)
	{
		EnsureLoaded();
		uint64_t submit = 0;
		{
			std::scoped_lock<std::mutex> lock(m_Shared->Mutex);
			auto [it, inserted] = m_Shared->Paths.try_emplace(handle, path);
			if (!inserted && it->second == path)
				return;
			it->second = path;
			m_Shared->QueueLine(JsonUtils::Dump(nlohmann::json { { "Handle", handle.ToString() }, { "Path", path } }));
			submit = m_Shared->ClaimWriterLocked();
		}
		if (submit != 0)
			Shared::Submit(m_Shared, submit);
	}

	void ImportRecordStore::Remove(AssetHandle handle)
	{
		EnsureLoaded();
		uint64_t submit = 0;
		{
			std::scoped_lock<std::mutex> lock(m_Shared->Mutex);
			const bool had = m_Shared->Records.erase(handle) > 0;
			if (m_Shared->Paths.erase(handle) == 0 && !had)
				return;
			m_Shared->QueueLine(JsonUtils::Dump(nlohmann::json { { "Handle", handle.ToString() }, { "Removed", true } }));
			submit = m_Shared->ClaimWriterLocked();
		}
		if (submit != 0)
			Shared::Submit(m_Shared, submit);
	}

	bool ImportRecordStore::Flush(std::string* outError)
	{
		// Nothing is queued before the store loaded.
		Shared& shared = *m_Shared;
		uint64_t target = 0;
		{
			std::scoped_lock<std::mutex> lock(shared.Mutex);
			target = shared.Queued;
		}
		for (;;)
		{
			JobHandle writer;
			{
				std::unique_lock<std::mutex> lock(shared.Mutex);
				if (shared.Written >= target && !shared.WriterActive)
					break;
				if (!shared.WriterActive)
				{
					// None runs (a failed write stopped it with entries left): write here.
					shared.ClaimWriterLocked();
					lock.unlock();
					Shared::Drain(shared);
					continue;
				}
				writer = shared.Writer;
				if (!writer.IsValid())
				{
					// Its submitter has not stored its handle yet: wait until it did, or the writer ended.
					shared.Condition.wait_for(lock, std::chrono::milliseconds(10));
					continue;
				}
			}
			// Runs the writer here when it has not started (what waits may be the I/O thread it is queued on).
			JobSystem::Wait(writer);
		}
		std::scoped_lock<std::mutex> lock(shared.Mutex);
		if (shared.WriteError.empty())
			return true;
		if (outError)
			*outError = shared.WriteError;
		shared.WriteError.clear();
		return false;
	}

	size_t ImportRecordStore::GetLiveCount() const
	{
		EnsureLoaded();
		std::scoped_lock<std::mutex> lock(m_Shared->Mutex);
		return m_Shared->GetLiveCountLocked();
	}

	size_t ImportRecordStore::GetDeadCount() const
	{
		EnsureLoaded();
		std::scoped_lock<std::mutex> lock(m_Shared->Mutex);
		const size_t live = m_Shared->GetLiveCountLocked();
		return m_Shared->FileEntries > live ? m_Shared->FileEntries - live : 0;
	}

	uint64_t ImportRecordStore::GetFileSize() const
	{
		std::scoped_lock<std::mutex> lock(m_Shared->Mutex);
		return m_Shared->FileBytes;
	}

	uint64_t ImportRecordStore::GetRewriteCount() const
	{
		std::scoped_lock<std::mutex> lock(m_Shared->Mutex);
		return m_Shared->Rewrites;
	}

}
