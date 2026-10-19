#pragma once

#include "Strata/Asset/AssetTypes.h"

#include <nlohmann/json_fwd.hpp>

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Strata
{

	// What ImportRecordStore::Load found, for logs and tests.
	struct ImportRecordLoadStats
	{
		size_t Lines = 0;         // Entries read (the header left out)
		size_t SkippedLines = 0;  // Entries whose checksum or JSON was bad (a torn last line, corruption): ignored
		size_t MigratedRecords = 0; // "<handle>.import" files taken over
		size_t MigratedPaths = 0;   // Paths taken over from "AssetIndex.json"
		bool Discarded = false;   // The file had an unknown header: it is rewritten from scratch
	};

	// The import records of a project's assets, kept in one append-only file of the cache directory
	// (<cache>/ImportRecords.log) instead of one file per asset: what EditorAssetManager knows of each asset's last import (or,
	// for assets stored as their source, what they reference) and the path each handle belongs to (which resolves duplicated
	// .meta files the same way on every scan).
	//
	// The file is a header line, {"Format":"ImportRecords","Version":1}, then one entry per line: the CRC-32C of the entry's
	// JSON text as 8 hexadecimal digits, a space and the compact JSON, one of {"Handle", "Record": {...}}, {"Handle", "Path"}
	// and {"Handle", "Removed": true}. The latest record and the latest path of a handle win; Removed drops both.
	//
	// Crash-safe: entries are appended as whole lines by one writer at a time (an I/O job; callers only queue them), flushed
	// per batch. Load skips lines whose checksum or JSON is bad (a line torn by a crash, corruption): their assets are imported
	// or searched again, never wrong. Callers rename cache files into place before they set the record that names them, so a
	// record never names a missing cache file (and readers check anyway). When dead entries outnumber live ones and the file
	// exceeds c_CompactMinBytes, the writer rewrites it from memory (atomically, AtomicFileWriter).
	//
	// Load migrates the earlier layout: "<handle>.import" files of handles without a record and "AssetIndex.json" are read once,
	// appended, and removed once the store wrote them, so an upgraded project neither imports again nor keeps two sources of
	// truth.
	//
	// Thread-safe. Reads answer from memory.
	class ImportRecordStore
	{
	public:
		static constexpr std::string_view c_FileName = "ImportRecords.log";
		static constexpr uint32_t c_Version = 1;
		static constexpr uint64_t c_CompactMinBytes = 1024 * 1024;

		explicit ImportRecordStore(std::filesystem::path cacheDirectory);
		// Writes what is queued (Flush).
		~ImportRecordStore();

		ImportRecordStore(const ImportRecordStore&) = delete;
		ImportRecordStore& operator=(const ImportRecordStore&) = delete;

		// Reads the file into memory (and migrates the earlier layout), once: blocking, on any thread (the editor's scan does it
		// on a job); later calls do nothing and return empty statistics. Every other function loads first when nothing did yet,
		// so that an entry never truncates a file that was not read. A file that ends in a torn line is cut back to its last
		// whole line, so that appends start on a line of their own.
		ImportRecordLoadStats Load();

		// The latest record of a handle, or nothing (none, or removed).
		std::optional<nlohmann::json> FindRecord(AssetHandle handle) const;
		// The latest path of a handle, or nothing.
		std::optional<std::string> FindPath(AssetHandle handle) const;
		// Every handle's path.
		std::unordered_map<AssetHandle, std::string> GetPaths() const;
		// Every handle that has a record.
		std::vector<AssetHandle> GetRecordedHandles() const;

		// Queue an entry, which the writer appends soon after (the in-memory state changes at once).
		void SetRecord(AssetHandle handle, const nlohmann::json& record);
		// Nothing is written when the path is the one the store has.
		void SetPath(AssetHandle handle, const std::string& path);
		// Drops the handle's record and path; nothing is written when it has neither.
		void Remove(AssetHandle handle);

		// Waits until everything queued so far is in the file. False, with the reason, when a write failed since the last call
		// (the entries it lost are imported or searched again later).
		bool Flush(std::string* outError = nullptr);

		// Live entries (records and paths) and dead ones (superseded lines in the file), and the file's size.
		size_t GetLiveCount() const;
		size_t GetDeadCount() const;
		uint64_t GetFileSize() const;
		// Rewrites of the file from memory so far (compactions, and the rewrite of a discarded file).
		uint64_t GetRewriteCount() const;

		const std::filesystem::path& GetPath() const;

		// The line an entry is written as (without its line feed): the checksum, a space and the JSON. For tests and tools.
		static std::string FormatLine(std::string_view json);
	private:
		struct Shared;

		void EnsureLoaded() const;
		ImportRecordLoadStats LoadFile() const;
	private:
		std::shared_ptr<Shared> m_Shared;
	};

}
