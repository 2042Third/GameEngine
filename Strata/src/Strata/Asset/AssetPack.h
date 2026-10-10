#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Core/FileReader.h"
#include "Strata/Math/AABB.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	// An asset of an asset pack (AssetPack::GetEntries).
	struct AssetPackEntry
	{
		AssetMetadata Metadata;   // StoredSize is Size
		uint64_t Size = 0;        // Bytes of the asset's stored form (uncompressed)
		uint64_t ContentHash = 0; // A 64-bit hash of the stored form (AssetPack::HashContent); 0 in version 1 packs
		std::optional<AABB> Bounds; // Local bounds of meshes, recorded when the asset was imported

		// Where the stored form lies: a run of blocks (version 2), or one range of the file (version 1).
		uint32_t FirstBlock = 0;
		uint32_t BlockCount = 0;
		uint64_t Offset = 0; // Version 1 only

		// The entry's direct dependencies (AssetPack::GetDependencies).
		uint32_t FirstDependency = 0;
		uint32_t DependencyCount = 0;
	};

	// How a block of a version 2 pack is stored.
	enum class AssetPackCodec : uint32_t
	{
		Stored = 0, // As it is (compression saved less than 5%)
		Zstd = 1
	};

	// A block of a version 2 pack: up to BlockSize bytes of an entry's stored form, compressed on their own.
	struct AssetPackBlock
	{
		uint64_t Offset = 0;     // In the file
		uint32_t StoredSize = 0; // Bytes in the file
		uint32_t Size = 0;       // Bytes once decoded
		uint32_t Checksum = 0;   // CRC-32C of the bytes in the file (Hash::CRC32C)
		AssetPackCodec Codec = AssetPackCodec::Stored;
	};

	// What reads of a pack did since it was opened (AssetPack::GetStats).
	struct AssetPackStats
	{
		uint64_t Reads = 0;            // ReadRange and ReadData calls that succeeded
		uint64_t FileBytesRead = 0;    // Bytes read from the file for them
		uint64_t BlocksDecoded = 0;    // Blocks checked and decoded (stored or decompressed)
		uint64_t BlocksDecompressed = 0;
		uint64_t BytesDecompressed = 0;
	};

	// Asset pack (".stpak"): every asset of a game in its stored form, in one file the runtime streams from by byte range.
	// Written by AssetPackWriter (the editor's export, EditorAssetManager::BuildAssetPack), read by RuntimeAssetManager.
	//
	// Version 2 layout (little endian):
	//   header (64 bytes): "STPK", version, flags (none defined; packs with unknown flags are refused), header size, block
	//     size, the counts of entries, strings, blocks and dependencies, the size of the string bytes, where the tables
	//     start, a CRC-32C of the tables and one of the header
	//   entry data: every entry starts at a 4 KiB boundary and is a run of consecutive blocks: its stored form cut into
	//     pieces of BlockSize (256 KiB; the last one shorter), each compressed with zstd, or stored as it is when compression
	//     saves less than 5%. Entries are ordered by first use (see EditorAssetManager::BuildAssetPack), so loading a scene
	//     reads the file front to back.
	//   tables (they end the file, so a pack is written in one pass, and are read with one read):
	//     entries (88 bytes each): handle, parent, stored size, content hash, type, flags (bit 0: has bounds), the string
	//       indices of path, sub-asset key and name, first block and block count, first dependency and dependency count,
	//       bounds minimum and maximum
	//     strings: per string its offset and size in the string bytes, then the string bytes (shared paths are stored once)
	//     blocks (24 bytes each): file offset, stored size, decoded size, CRC-32C of the stored bytes, codec
	//     dependencies: asset handles, 8 bytes each; an entry names its range
	// Open validates the header and every table completely (sizes, ranges, overlapping blocks, checksums) and reads; reads
	// check each block's checksum before decoding it, so corrupt files fail with an error, never a crash. Packs are untrusted
	// input whose tables may point at shared targets, so Open also bounds what it builds by the file's size: entries may not
	// name more blocks or dependencies than the tables hold (so no entry claims more bytes than its blocks decode to), and
	// the strings they name, counted per entry, stay within AssetPackFormat::GetMaxNameBytes; running out of memory fails
	// with an error too. Version 1 packs (header, uncompressed data, an entry table; read whole) still load.
	class AssetPack
	{
	public:
		static constexpr uint32_t c_Magic = 0x4B505453; // "STPK"
		static constexpr uint32_t c_Version = 2;
		static constexpr uint32_t c_BlockSize = 256 * 1024;
		static constexpr uint64_t c_EntryAlignment = 4096;

		// Reads and validates the header and the tables. The file stays open (one handle, shared by every read) until the
		// pack is destroyed.
		static Scope<AssetPack> Open(const std::filesystem::path& path, std::string* outError = nullptr);

		uint32_t GetVersion() const { return m_Version; }
		const std::vector<AssetPackEntry>& GetEntries() const { return m_Entries; }
		const std::filesystem::path& GetPath() const { return m_Path; }
		uint64_t GetFileSize() const { return m_Reader->GetSize(); }
		// The blocks of version 2 packs (empty for version 1).
		const std::vector<AssetPackBlock>& GetBlocks() const { return m_Blocks; }
		std::span<const AssetHandle> GetDependencies(const AssetPackEntry& entry) const;

		// Reads `size` bytes of the entry's stored form from `offset`, decoding only the blocks that cover the range.
		// Thread-safe. False with an error when the range lies outside the entry or the file is unreadable or corrupt.
		bool ReadRange(const AssetPackEntry& entry, uint64_t offset, uint64_t size, std::vector<uint8_t>& outData, std::string* outError = nullptr) const;
		// Reads the entry's whole stored form. Thread-safe.
		bool ReadData(const AssetPackEntry& entry, std::vector<uint8_t>& outData, std::string* outError = nullptr) const;

		AssetPackStats GetStats() const;

		// The content hash of a stored form, as packs record it: FNV-1a of each BlockSize piece, combined in order.
		static uint64_t HashContent(std::span<const uint8_t> data);
	private:
		AssetPack() = default;

		// Open without the handling of allocation failures.
		static Scope<AssetPack> Load(const std::filesystem::path& path, std::string* outError);
		bool ReadBlocks(const AssetPackEntry& entry, uint64_t offset, uint64_t size, uint8_t* destination, std::string& outError) const;
		// Checks and decodes one block of an entry (its index within the entry) into the part of `destination` (which holds
		// the entry's bytes from `offset` to `end`) it covers. Any thread.
		bool DecodeBlock(const AssetPackEntry& entry, uint32_t block, std::span<const uint8_t> stored, uint64_t offset, uint64_t end, uint8_t* destination,
			std::string& outError) const;
	private:
		std::filesystem::path m_Path;
		Scope<FileReader> m_Reader;
		uint32_t m_Version = 0;
		uint32_t m_BlockSize = c_BlockSize;
		std::vector<AssetPackEntry> m_Entries;
		std::vector<AssetPackBlock> m_Blocks;
		std::vector<AssetHandle> m_Dependencies;

		mutable std::atomic<uint64_t> m_Reads = 0;
		mutable std::atomic<uint64_t> m_FileBytesRead = 0;
		mutable std::atomic<uint64_t> m_BlocksDecoded = 0;
		mutable std::atomic<uint64_t> m_BlocksDecompressed = 0;
		mutable std::atomic<uint64_t> m_BytesDecompressed = 0;
	};

	// The binary layout of version 2 packs, shared by the reader and AssetPackWriter.
	namespace AssetPackFormat
	{

		struct Header
		{
			uint32_t Magic = 0;
			uint32_t Version = 0;
			uint32_t Flags = 0;
			uint32_t HeaderSize = 0;
			uint32_t BlockSize = 0;
			uint32_t EntryCount = 0;
			uint32_t StringCount = 0;
			uint32_t BlockCount = 0;
			uint32_t DependencyCount = 0;
			uint32_t Reserved = 0;
			uint64_t StringBytes = 0;
			uint64_t TablesOffset = 0;
			uint32_t TablesChecksum = 0; // CRC-32C of everything from TablesOffset to the end of the file
			uint32_t HeaderChecksum = 0; // CRC-32C of the header's bytes before this field
		};
		static_assert(sizeof(Header) == 64, "The header layout is part of the pack format");

		constexpr uint16_t c_EntryHasBounds = 1;

		struct EntryRecord
		{
			uint64_t Handle = 0;
			uint64_t Parent = 0;
			uint64_t Size = 0;
			uint64_t ContentHash = 0;
			uint16_t Type = 0;
			uint16_t Flags = 0;
			uint32_t PathString = 0;
			uint32_t SubAssetKeyString = 0;
			uint32_t NameString = 0;
			uint32_t FirstBlock = 0;
			uint32_t BlockCount = 0;
			uint32_t FirstDependency = 0;
			uint32_t DependencyCount = 0;
			float BoundsMin[3] = {};
			float BoundsMax[3] = {};
		};
		static_assert(sizeof(EntryRecord) == 88, "The entry layout is part of the pack format");

		struct StringRecord
		{
			uint32_t Offset = 0;
			uint32_t Size = 0;
		};
		static_assert(sizeof(StringRecord) == 8, "The string layout is part of the pack format");

		struct BlockRecord
		{
			uint64_t Offset = 0;
			uint32_t StoredSize = 0;
			uint32_t Size = 0;
			uint32_t Checksum = 0;
			uint32_t Codec = 0;
		};
		static_assert(sizeof(BlockRecord) == 24, "The block layout is part of the pack format");

		// The size of the tables of a header's counts; nullopt when it overflows.
		std::optional<uint64_t> GetTablesSize(const Header& header);

		// Entries share strings (sub-assets share their file's path), so what opening a pack copies into its entries' names
		// could be far more than the string table holds: a pack's entries may name at most this many bytes in all (path,
		// sub-asset key and name, counted per entry) for its file size. AssetPackWriter refuses to write more.
		constexpr uint64_t c_NameBytesPerFileByte = 16;
		constexpr uint64_t c_NameBytesAllowance = 1ull << 20;
		uint64_t GetMaxNameBytes(uint64_t fileSize);

		// Version 1: a 32-byte header, the data blobs back to back, then the entry table (handle, type, padding, parent,
		// data offset, data size, path, sub-asset key, name per entry).
		struct HeaderV1
		{
			uint32_t Magic = 0;
			uint32_t Version = 0;
			uint32_t EntryCount = 0;
			uint32_t Reserved = 0;
			uint64_t TableOffset = 0;
			uint64_t TableSize = 0;
		};
		static_assert(sizeof(HeaderV1) == 32, "The version 1 header layout is part of the pack format");

	}

}
