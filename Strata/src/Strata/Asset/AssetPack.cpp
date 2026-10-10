#include "stpch.h"
#include "Strata/Asset/AssetPack.h"

#include "Strata/Core/BinaryStream.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Hash.h"
#include "Strata/Core/JobSystem.h"

#include <zstd.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace Strata
{

	namespace
	{

		using namespace AssetPackFormat;

		// Tables larger than this are refused (a corrupt header must not make Open allocate gigabytes).
		constexpr uint64_t c_MaxTablesSize = 1ull << 30;
		// Version 1: fixed part of a table entry (handle, type, padding, parent, offset, size, three string lengths).
		constexpr uint64_t c_MinimumEntrySizeV1 = 8 + 2 + 2 + 8 + 8 + 8 + 3 * 4;
		// Block sizes a pack may declare.
		constexpr uint32_t c_MinBlockSize = 4096;
		constexpr uint32_t c_MaxBlockSize = 16u << 20;

		bool IsKnownAssetType(uint16_t value)
		{
			const AssetType type = static_cast<AssetType>(value);
			return type != AssetType::None && AssetTypeFromString(AssetTypeToString(type)) == type;
		}

		// One decompression context per thread: reads of every pack reuse it.
		struct DecompressionContext
		{
			ZSTD_DCtx* Context = ZSTD_createDCtx();

			~DecompressionContext()
			{
				ZSTD_freeDCtx(Context);
			}
		};

		thread_local DecompressionContext t_Decompression;

		std::string DescribeEntry(const AssetPackEntry& entry)
		{
			return entry.Metadata.Path.empty() ? entry.Metadata.Name : entry.Metadata.Path;
		}

	}

	// The blocks of one read on their way to its destination. The reading thread publishes them chunk by chunk as it reads
	// them, and it and the worker jobs it asks for help claim them one at a time and decode them. The jobs keep this alive:
	// one that starts after the read returned finds nothing left to claim, and the read returns only once every block it
	// published is finished, so the pointers below are never used after it.
	struct AssetPack::BlockDecoding
	{
		const AssetPack* Pack = nullptr;
		const AssetPackEntry* Entry = nullptr;
		uint64_t Offset = 0;
		uint64_t End = 0;
		uint8_t* Destination = nullptr;      // Holds the entry's bytes from Offset to End
		uint32_t FirstBlock = 0;             // The entry's block the read starts at
		std::vector<const uint8_t*> Stored;  // Per block of the read: its stored bytes, once published
		std::vector<uint32_t> ChunkOf;       // Per block of the read: the chunk it was read with
		std::unique_ptr<std::atomic<uint32_t>[]> ChunkUnfinished; // Per chunk: blocks not finished yet

		std::atomic<uint32_t> Published = 0; // Blocks of the read (from its first on) whose stored bytes are in memory
		std::atomic<uint32_t> Next = 0;      // The next block to claim
		std::atomic<uint32_t> Finished = 0;  // Blocks decoded, or skipped after a failure
		std::atomic<uint32_t> Helpers = 0;   // Jobs asked for that have not ended
		std::atomic<bool> Failed = false;

		std::mutex Mutex;
		std::condition_variable BlockFinished;
		std::string Error; // The first failure's reason (Mutex); empty when allocating it failed

		// Claims the next published block and decodes it (or skips it after a failure). False when none is left to claim.
		// Never throws.
		bool DecodeNext();
		// Claims and decodes blocks until `done` holds, waiting for the blocks others claimed when none is left.
		template<typename Predicate>
		void HelpUntil(const Predicate& done);
	};

	bool AssetPack::BlockDecoding::DecodeNext()
	{
		uint32_t index = Next.load(std::memory_order_relaxed);
		do
		{
			if (index >= Published.load(std::memory_order_acquire))
				return false;
		} while (!Next.compare_exchange_weak(index, index + 1, std::memory_order_acq_rel, std::memory_order_relaxed));

		// After a failure the read fails: the rest is only counted.
		if (!Failed.load(std::memory_order_acquire))
		{
			try
			{
				const uint32_t block = FirstBlock + index;
				const AssetPackBlock& stored = Pack->m_Blocks[Entry->FirstBlock + block];
				std::string error;
				if (!Pack->DecodeBlock(*Entry, block, std::span<const uint8_t>(Stored[index], stored.StoredSize), Offset, End, Destination, error))
				{
					std::scoped_lock<std::mutex> lock(Mutex);
					if (!Failed.exchange(true))
						Error = std::move(error);
				}
			}
			catch (const std::exception&)
			{
				// Out of memory for an error message: the read reports that.
				Failed = true;
			}
		}
		ChunkUnfinished[ChunkOf[index]].fetch_sub(1, std::memory_order_acq_rel);
		Finished.fetch_add(1, std::memory_order_acq_rel);
		{
			// Taken so that a waiter between its check and its wait cannot miss the notification.
			std::scoped_lock<std::mutex> lock(Mutex);
		}
		BlockFinished.notify_all();
		return true;
	}

	template<typename Predicate>
	void AssetPack::BlockDecoding::HelpUntil(const Predicate& done)
	{
		while (!done())
		{
			if (DecodeNext())
				continue;
			// Nothing left to claim: what remains is being decoded by the helpers that claimed it.
			std::unique_lock<std::mutex> lock(Mutex);
			BlockFinished.wait(lock, done);
		}
	}

	std::optional<uint64_t> AssetPackFormat::GetTablesSize(const Header& header)
	{
		// Counts are 32-bit, so their products fit in 64 bits; only the string bytes can overflow the sum.
		const uint64_t fixed = static_cast<uint64_t>(header.EntryCount) * sizeof(EntryRecord) + static_cast<uint64_t>(header.StringCount) * sizeof(StringRecord)
			+ static_cast<uint64_t>(header.BlockCount) * sizeof(BlockRecord) + static_cast<uint64_t>(header.DependencyCount) * sizeof(uint64_t);
		if (header.StringBytes > UINT64_MAX - fixed)
			return std::nullopt;
		return fixed + header.StringBytes;
	}

	uint64_t AssetPackFormat::GetMaxNameBytes(uint64_t fileSize)
	{
		if (fileSize > (UINT64_MAX - c_NameBytesAllowance) / c_NameBytesPerFileByte)
			return UINT64_MAX;
		return fileSize * c_NameBytesPerFileByte + c_NameBytesAllowance;
	}

	uint64_t AssetPack::HashContent(std::span<const uint8_t> data)
	{
		uint64_t hash = Hash::FNV1a(std::string_view());
		for (size_t offset = 0; offset < data.size(); offset += c_BlockSize)
			hash = Hash::Combine(hash, Hash::FNV1a(data.subspan(offset, std::min<size_t>(c_BlockSize, data.size() - offset))));
		return Hash::Combine(hash, data.size());
	}

	Scope<AssetPack> AssetPack::Open(const std::filesystem::path& path, std::string* outError)
	{
		// What Load allocates is bounded by the file's size (see GetMaxNameBytes), but the memory may still not be there.
		try
		{
			return Load(path, outError);
		}
		catch (const std::bad_alloc&)
		{
		}
		catch (const std::length_error&)
		{
		}
		if (outError)
			*outError = fmt::format("{}: not enough memory to open the asset pack", FileSystem::ToUTF8(path));
		return nullptr;
	}

	Scope<AssetPack> AssetPack::Load(const std::filesystem::path& path, std::string* outError)
	{
		auto fail = [outError, &path](const std::string& message) -> Scope<AssetPack>
		{
			if (outError)
				*outError = fmt::format("{}: {}", FileSystem::ToUTF8(path), message);
			return nullptr;
		};

		Scope<AssetPack> pack(new AssetPack());
		pack->m_FreeBuffers.reserve(c_MaxFreeReadBuffers);
		pack->m_Path = path;
		pack->m_Reader = FileReader::Open(path);
		if (!pack->m_Reader)
			return fail("cannot open the asset pack");
		const FileReader& reader = *pack->m_Reader;
		const uint64_t fileSize = reader.GetSize();

		uint32_t prefix[2] = {};
		if (fileSize < sizeof(prefix) || !reader.ReadAt(0, std::span<uint8_t>(reinterpret_cast<uint8_t*>(prefix), sizeof(prefix))))
			return fail("cannot read the pack header");
		if (prefix[0] != c_Magic)
			return fail("not a Strata asset pack");
		pack->m_Version = prefix[1];

		if (pack->m_Version == 1)
		{
			HeaderV1 header;
			if (fileSize < sizeof(header) || !reader.ReadAt(0, std::span<uint8_t>(reinterpret_cast<uint8_t*>(&header), sizeof(header))))
				return fail("cannot read the pack header");
			if (header.TableOffset < sizeof(HeaderV1) || header.TableSize > c_MaxTablesSize || header.TableOffset > fileSize
				|| header.TableSize > fileSize - header.TableOffset || header.EntryCount > header.TableSize / c_MinimumEntrySizeV1)
				return fail("corrupt pack header");

			std::vector<uint8_t> tableData(static_cast<size_t>(header.TableSize));
			if (!reader.ReadAt(header.TableOffset, tableData))
				return fail("cannot read the entry table");

			pack->m_Entries.reserve(header.EntryCount);
			std::unordered_set<uint64_t> handles;
			BinaryReader table(tableData);
			for (uint32_t index = 0; index < header.EntryCount; index++)
			{
				AssetPackEntry entry;
				const uint64_t handle = table.Read<uint64_t>();
				const uint16_t type = table.Read<uint16_t>();
				table.Read<uint16_t>();
				const uint64_t parent = table.Read<uint64_t>();
				entry.Offset = table.Read<uint64_t>();
				entry.Size = table.Read<uint64_t>();
				entry.Metadata.Path = table.ReadString();
				entry.Metadata.SubAssetKey = table.ReadString();
				entry.Metadata.Name = table.ReadString();
				if (!table.IsValid())
					return fail("truncated entry table");
				if (handle == 0 || !IsKnownAssetType(type) || !handles.insert(handle).second)
					return fail(fmt::format("invalid entry {} (handle {:016X})", index, handle));
				if (entry.Offset < sizeof(HeaderV1) || entry.Offset > header.TableOffset || entry.Size > header.TableOffset - entry.Offset)
					return fail(fmt::format("entry {} points outside the data section", index));

				entry.Metadata.Handle = UUID(handle);
				entry.Metadata.Type = static_cast<AssetType>(type);
				entry.Metadata.Parent = UUID(parent);
				entry.Metadata.StoredSize = entry.Size;
				pack->m_Entries.push_back(std::move(entry));
			}
			if (table.GetRemaining() != 0)
				return fail("unexpected data after the entry table");
			return pack;
		}

		if (pack->m_Version != c_Version)
			return fail(fmt::format("unsupported pack version {} (this engine reads versions 1 and {})", pack->m_Version, c_Version));

		Header header;
		if (fileSize < sizeof(header) || !reader.ReadAt(0, std::span<uint8_t>(reinterpret_cast<uint8_t*>(&header), sizeof(header))))
			return fail("cannot read the pack header");
		const std::span<const uint8_t> headerBytes(reinterpret_cast<const uint8_t*>(&header), offsetof(Header, HeaderChecksum));
		if (Hash::CRC32C(headerBytes) != header.HeaderChecksum)
			return fail("corrupt pack header (checksum mismatch)");
		if (header.Flags != 0)
			return fail(fmt::format("the pack uses features this engine does not know (flags {:08X})", header.Flags));
		if (header.HeaderSize != sizeof(Header) || header.BlockSize < c_MinBlockSize || header.BlockSize > c_MaxBlockSize
			|| (header.BlockSize & (header.BlockSize - 1)) != 0 || header.Reserved != 0)
			return fail("corrupt pack header");
		const std::optional<uint64_t> tablesSize = GetTablesSize(header);
		if (!tablesSize || *tablesSize > c_MaxTablesSize || header.TablesOffset < sizeof(Header) || header.TablesOffset > fileSize
			|| *tablesSize != fileSize - header.TablesOffset)
			return fail("corrupt pack header (the tables do not end the file)");
		pack->m_BlockSize = header.BlockSize;

		std::vector<uint8_t> tables(static_cast<size_t>(*tablesSize));
		if (!reader.ReadAt(header.TablesOffset, tables))
			return fail("cannot read the tables");
		if (Hash::CRC32C(tables) != header.TablesChecksum)
			return fail("corrupt tables (checksum mismatch)");

		const uint8_t* cursor = tables.data();
		auto take = [&cursor](void* destination, size_t size)
		{
			if (size > 0)
				std::memcpy(destination, cursor, size);
			cursor += size;
		};

		std::vector<EntryRecord> entryRecords(header.EntryCount);
		take(entryRecords.data(), entryRecords.size() * sizeof(EntryRecord));
		std::vector<StringRecord> stringRecords(header.StringCount);
		take(stringRecords.data(), stringRecords.size() * sizeof(StringRecord));
		const uint8_t* stringBytes = cursor;
		cursor += header.StringBytes;
		std::vector<BlockRecord> blockRecords(header.BlockCount);
		take(blockRecords.data(), blockRecords.size() * sizeof(BlockRecord));
		std::vector<uint64_t> dependencies(header.DependencyCount);
		take(dependencies.data(), dependencies.size() * sizeof(uint64_t));

		// Views into the tables: strings are copied only into the entries that name them (within GetMaxNameBytes).
		std::vector<std::string_view> strings;
		strings.reserve(stringRecords.size());
		for (size_t index = 0; index < stringRecords.size(); index++)
		{
			const StringRecord& record = stringRecords[index];
			if (record.Offset > header.StringBytes || record.Size > header.StringBytes - record.Offset)
				return fail(fmt::format("string {} lies outside the string table", index));
			strings.emplace_back(reinterpret_cast<const char*>(stringBytes) + record.Offset, record.Size);
		}

		// Blocks lie between the header and the tables, never overlap, and decode to at most a block.
		pack->m_Blocks.reserve(blockRecords.size());
		for (size_t index = 0; index < blockRecords.size(); index++)
		{
			const BlockRecord& record = blockRecords[index];
			const bool knownCodec = record.Codec == static_cast<uint32_t>(AssetPackCodec::Stored) || record.Codec == static_cast<uint32_t>(AssetPackCodec::Zstd);
			if (!knownCodec)
				return fail(fmt::format("block {} uses an unknown codec ({})", index, record.Codec));
			if (record.Offset < sizeof(Header) || record.Offset > header.TablesOffset || record.StoredSize > header.TablesOffset - record.Offset)
				return fail(fmt::format("block {} lies outside the data section", index));
			if (record.Size == 0 || record.Size > header.BlockSize || record.StoredSize == 0)
				return fail(fmt::format("block {} has an invalid size", index));
			const bool stored = record.Codec == static_cast<uint32_t>(AssetPackCodec::Stored);
			if ((stored && record.StoredSize != record.Size) || (!stored && record.StoredSize >= record.Size))
				return fail(fmt::format("block {} has an invalid size", index));
			pack->m_Blocks.push_back(AssetPackBlock { record.Offset, record.StoredSize, record.Size, record.Checksum, static_cast<AssetPackCodec>(record.Codec) });
		}
		std::vector<uint32_t> byOffset(pack->m_Blocks.size());
		std::iota(byOffset.begin(), byOffset.end(), 0u);
		std::sort(byOffset.begin(), byOffset.end(), [&pack](uint32_t a, uint32_t b) { return pack->m_Blocks[a].Offset < pack->m_Blocks[b].Offset; });
		for (size_t index = 1; index < byOffset.size(); index++)
		{
			const AssetPackBlock& previous = pack->m_Blocks[byOffset[index - 1]];
			if (previous.Offset + previous.StoredSize > pack->m_Blocks[byOffset[index]].Offset)
				return fail(fmt::format("blocks {} and {} overlap", byOffset[index - 1], byOffset[index]));
		}

		pack->m_Dependencies.reserve(dependencies.size());
		for (uint64_t dependency : dependencies)
			pack->m_Dependencies.push_back(UUID(dependency));

		// Tables may share what they point to, so what entries name is counted with repeats: what the writer stores once each
		// (blocks, dependencies) at most once in all, so that sizes and lists derived from the entries stay within what the
		// file holds, and names within GetMaxNameBytes.
		const uint64_t maxNameBytes = GetMaxNameBytes(fileSize);
		uint64_t nameBytes = 0;
		uint64_t entryBlocks = 0;
		uint64_t entryDependencies = 0;
		std::unordered_set<uint64_t> handles;
		pack->m_Entries.reserve(entryRecords.size());
		for (size_t index = 0; index < entryRecords.size(); index++)
		{
			const EntryRecord& record = entryRecords[index];
			if (record.Handle == 0 || !IsKnownAssetType(record.Type) || !handles.insert(record.Handle).second)
				return fail(fmt::format("invalid entry {} (handle {:016X})", index, record.Handle));
			if ((record.Flags & ~c_EntryHasBounds) != 0)
				return fail(fmt::format("entry {} has unknown flags", index));
			if (record.PathString >= strings.size() || record.SubAssetKeyString >= strings.size() || record.NameString >= strings.size())
				return fail(fmt::format("entry {} names a string that does not exist", index));
			if (record.FirstBlock > pack->m_Blocks.size() || record.BlockCount > pack->m_Blocks.size() - record.FirstBlock)
				return fail(fmt::format("entry {} names blocks that do not exist", index));
			if (record.FirstDependency > pack->m_Dependencies.size() || record.DependencyCount > pack->m_Dependencies.size() - record.FirstDependency)
				return fail(fmt::format("entry {} names dependencies that do not exist", index));
			// Each string is at most 4 GiB, so the sum of three cannot overflow before it passes the limit.
			nameBytes += static_cast<uint64_t>(strings[record.PathString].size()) + strings[record.SubAssetKeyString].size() + strings[record.NameString].size();
			if (nameBytes > maxNameBytes)
				return fail(fmt::format("the entries name more than {} bytes of strings, too many for a pack of {} bytes", maxNameBytes, fileSize));
			entryBlocks += record.BlockCount;
			entryDependencies += record.DependencyCount;
			if (entryBlocks > pack->m_Blocks.size())
				return fail(fmt::format("entry {} shares blocks with other entries", index));
			if (entryDependencies > pack->m_Dependencies.size())
				return fail(fmt::format("entry {} shares dependencies with other entries", index));

			// The blocks cover the stored form exactly: full blocks, then the rest, consecutive in the file. (Counted without
			// rounding up, which would overflow for sizes near 2^64.)
			const uint64_t expectedBlocks = record.Size / header.BlockSize + (record.Size % header.BlockSize != 0 ? 1 : 0);
			if (record.BlockCount != expectedBlocks)
				return fail(fmt::format("entry {} has {} blocks for {} bytes", index, record.BlockCount, record.Size));
			for (uint32_t block = 0; block < record.BlockCount; block++)
			{
				const AssetPackBlock& current = pack->m_Blocks[record.FirstBlock + block];
				const uint64_t expectedSize = block + 1 < record.BlockCount ? header.BlockSize : record.Size - static_cast<uint64_t>(block) * header.BlockSize;
				if (current.Size != expectedSize)
					return fail(fmt::format("entry {} has a block of the wrong size", index));
				if (block > 0)
				{
					const AssetPackBlock& previous = pack->m_Blocks[record.FirstBlock + block - 1];
					if (previous.Offset + previous.StoredSize != current.Offset)
						return fail(fmt::format("the blocks of entry {} are not consecutive", index));
				}
			}

			AssetPackEntry entry;
			entry.Metadata.Handle = UUID(record.Handle);
			entry.Metadata.Type = static_cast<AssetType>(record.Type);
			entry.Metadata.Parent = UUID(record.Parent);
			entry.Metadata.Path = strings[record.PathString];
			entry.Metadata.SubAssetKey = strings[record.SubAssetKeyString];
			entry.Metadata.Name = strings[record.NameString];
			entry.Metadata.StoredSize = record.Size;
			entry.Size = record.Size;
			entry.ContentHash = record.ContentHash;
			entry.FirstBlock = record.FirstBlock;
			entry.BlockCount = record.BlockCount;
			entry.FirstDependency = record.FirstDependency;
			entry.DependencyCount = record.DependencyCount;
			if (record.Flags & c_EntryHasBounds)
			{
				const glm::vec3 minimum(record.BoundsMin[0], record.BoundsMin[1], record.BoundsMin[2]);
				const glm::vec3 maximum(record.BoundsMax[0], record.BoundsMax[1], record.BoundsMax[2]);
				bool finite = true;
				for (int axis = 0; axis < 3; axis++)
					finite = finite && std::isfinite(minimum[axis]) && std::isfinite(maximum[axis]);
				if (!finite || !AABB(minimum, maximum).IsValid())
					return fail(fmt::format("entry {} has invalid bounds", index));
				entry.Bounds = AABB(minimum, maximum);
			}
			pack->m_Entries.push_back(std::move(entry));
		}
		return pack;
	}

	std::span<const AssetHandle> AssetPack::GetDependencies(const AssetPackEntry& entry) const
	{
		// Ranges were validated when the pack was opened (entries of other packs name none).
		if (entry.FirstDependency > m_Dependencies.size() || entry.DependencyCount > m_Dependencies.size() - entry.FirstDependency)
			return {};
		return std::span<const AssetHandle>(m_Dependencies).subspan(entry.FirstDependency, entry.DependencyCount);
	}

	bool AssetPack::ReadData(const AssetPackEntry& entry, std::vector<uint8_t>& outData, std::string* outError) const
	{
		return ReadRange(entry, 0, entry.Size, outData, outError);
	}

	bool AssetPack::ReadRange(const AssetPackEntry& entry, uint64_t offset, uint64_t size, std::vector<uint8_t>& outData, std::string* outError) const
	{
		auto fail = [&](std::string_view reason)
		{
			if (outError)
				*outError = fmt::format("Could not read '{}' from '{}': {}", DescribeEntry(entry), FileSystem::ToUTF8(m_Path), reason);
			return false;
		};

		if (offset > entry.Size || size > entry.Size - offset)
			return fail(fmt::format("the range {}+{} lies outside its {} bytes", offset, size, entry.Size));
		if (size > std::numeric_limits<size_t>::max() || static_cast<size_t>(size) > outData.max_size())
			return fail("the range is too large");
		try
		{
			outData.resize(static_cast<size_t>(size));
		}
		catch (const std::bad_alloc&)
		{
			return fail("out of memory");
		}
		catch (const std::length_error&)
		{
			return fail("the range is too large");
		}
		if (size == 0)
		{
			m_Reads.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		if (m_Version == 1)
		{
			if (!m_Reader->ReadAt(entry.Offset + offset, outData))
				return fail("the file could not be read");
			m_FileBytesRead.fetch_add(size, std::memory_order_relaxed);
			m_Reads.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		std::string error;
		if (!ReadBlocks(entry, offset, size, outData.data(), error))
			return fail(error);
		m_Reads.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	bool AssetPack::ReadBlocks(const AssetPackEntry& entry, uint64_t offset, uint64_t size, uint8_t* destination, std::string& outError) const
	{
		if (entry.FirstBlock > m_Blocks.size() || entry.BlockCount > m_Blocks.size() - entry.FirstBlock)
		{
			outError = "the entry is not part of this pack";
			return false;
		}

		// Allocating fails only when memory runs out; blocks published by then are finished before the read returns.
		try
		{
			// The reads: runs of consecutive blocks of up to c_ReadChunkBytes stored bytes (at least one block each).
			struct Chunk
			{
				uint32_t First = 0; // Block index within the read
				uint32_t Count = 0;
				uint64_t Bytes = 0;
			};
			const uint64_t end = offset + size;
			const uint32_t firstBlock = static_cast<uint32_t>(offset / m_BlockSize);
			const uint32_t blockCount = static_cast<uint32_t>((end - 1) / m_BlockSize) - firstBlock + 1;
			std::vector<Chunk> chunks;
			Ref<BlockDecoding> work = CreateRef<BlockDecoding>();
			work->Pack = this;
			work->Entry = &entry;
			work->Offset = offset;
			work->End = end;
			work->Destination = destination;
			work->FirstBlock = firstBlock;
			work->Stored.resize(blockCount);
			work->ChunkOf.resize(blockCount);
			for (uint32_t block = 0; block < blockCount;)
			{
				Chunk chunk { block, 0, 0 };
				while (block < blockCount && (chunk.Count == 0 || chunk.Bytes + m_Blocks[entry.FirstBlock + firstBlock + block].StoredSize <= c_ReadChunkBytes))
				{
					chunk.Bytes += m_Blocks[entry.FirstBlock + firstBlock + block].StoredSize;
					work->ChunkOf[block] = static_cast<uint32_t>(chunks.size());
					chunk.Count++;
					block++;
				}
				chunks.push_back(chunk);
			}
			work->ChunkUnfinished = std::make_unique<std::atomic<uint32_t>[]>(chunks.size());
			for (size_t index = 0; index < chunks.size(); index++)
				work->ChunkUnfinished[index].store(chunks[index].Count, std::memory_order_relaxed);

			// Each chunk is read into a buffer the pack lends (up to c_ReadBuffersPerRead chunks ahead of the decoding). Every way
			// out finishes the blocks published so far first: their decoders use the buffers and the destination.
			const size_t bufferCount = std::min(chunks.size(), c_ReadBuffersPerRead);
			std::vector<std::vector<uint8_t>> buffers;
			std::vector<size_t> lentCapacities;
			buffers.reserve(bufferCount);
			lentCapacities.reserve(bufferCount);
			struct FinishReading
			{
				const AssetPack& Pack;
				BlockDecoding& Work;
				std::vector<std::vector<uint8_t>>& Buffers;
				std::vector<size_t>& LentCapacities;

				~FinishReading()
				{
					Work.HelpUntil([this]() { return Work.Finished.load(std::memory_order_acquire) == Work.Published.load(std::memory_order_acquire); });
					for (size_t index = 0; index < Buffers.size(); index++)
						Pack.ReturnBuffer(std::move(Buffers[index]), LentCapacities[index]);
				}
			} finishReading { *this, *work, buffers, lentCapacities };
			for (size_t index = 0; index < bufferCount; index++)
			{
				buffers.push_back(TakeBuffer());
				lentCapacities.push_back(buffers.back().capacity());
			}

			// Blocks decode on the workers while the next chunk is read. The thread that reads never runs other jobs while it
			// waits (a texture's decoding would stall the reads behind it): it decodes blocks of its own read, or sleeps until
			// the blocks its helpers claimed are finished.
			const uint32_t maxHelpers = JobSystem::GetWorkerThreadCount();
			auto requestHelpers = [&work, maxHelpers]()
			{
				const uint32_t published = work->Published.load(std::memory_order_acquire);
				const uint32_t next = work->Next.load(std::memory_order_acquire);
				const uint32_t wanted = std::min(published > next ? published - next : 0u, maxHelpers);
				while (work->Helpers.load(std::memory_order_acquire) < wanted)
				{
					work->Helpers.fetch_add(1, std::memory_order_acq_rel);
					Ref<BlockDecoding> shared = work;
					try
					{
						JobSystem::Submit([shared]()
						{
							while (shared->DecodeNext())
							{
							}
							shared->Helpers.fetch_sub(1, std::memory_order_acq_rel);
						}, JobPriority::High);
					}
					catch (const std::exception&)
					{
						// No helper (out of memory for the job): the reading thread decodes the blocks itself.
						work->Helpers.fetch_sub(1, std::memory_order_acq_rel);
						return;
					}
				}
			};

			for (size_t index = 0; index < chunks.size(); index++)
			{
				const Chunk& chunk = chunks[index];
				// The buffer's previous chunk must be decoded before it is read over.
				if (index >= bufferCount)
				{
					const size_t previous = index - bufferCount;
					work->HelpUntil([&work, previous]() { return work->ChunkUnfinished[previous].load(std::memory_order_acquire) == 0; });
				}
				if (work->Failed.load(std::memory_order_acquire))
					break;

				std::vector<uint8_t>& buffer = buffers[index % bufferCount];
				buffer.resize(static_cast<size_t>(chunk.Bytes));
				if (!m_Reader->ReadAt(m_Blocks[entry.FirstBlock + firstBlock + chunk.First].Offset, buffer))
				{
					outError = "the file could not be read";
					work->Failed = true;
					return false;
				}
				m_FileBytesRead.fetch_add(chunk.Bytes, std::memory_order_relaxed);

				uint64_t position = 0;
				for (uint32_t block = chunk.First; block < chunk.First + chunk.Count; block++)
				{
					work->Stored[block] = buffer.data() + position;
					position += m_Blocks[entry.FirstBlock + firstBlock + block].StoredSize;
				}
				work->Published.store(chunk.First + chunk.Count, std::memory_order_release);
				requestHelpers();
			}

			// The last chunks: decoded here and by the helpers.
			work->HelpUntil([&work]() { return work->Finished.load(std::memory_order_acquire) == work->Published.load(std::memory_order_acquire); });
			if (work->Failed.load(std::memory_order_acquire))
			{
				std::scoped_lock<std::mutex> lock(work->Mutex);
				outError = work->Error.empty() ? std::string("out of memory") : work->Error;
				return false;
			}
			return true;
		}
		catch (const std::exception&)
		{
			outError = "out of memory";
			return false;
		}
	}

	bool AssetPack::DecodeBlock(const AssetPackEntry& entry, uint32_t block, std::span<const uint8_t> stored, uint64_t offset, uint64_t end, uint8_t* destination,
		std::string& outError) const
	{
		const AssetPackBlock& current = m_Blocks[entry.FirstBlock + block];
		if (Hash::CRC32C(stored) != current.Checksum)
		{
			outError = fmt::format("block {} is corrupt (checksum mismatch)", entry.FirstBlock + block);
			return false;
		}

		// The part of the block the range covers: decoded in place when it is the whole block, else through a block of its
		// own (only the first and the last block of a range can be partial; stored blocks are copied from directly).
		const uint64_t blockStart = static_cast<uint64_t>(block) * m_BlockSize;
		const uint64_t copyStart = std::max(blockStart, offset);
		const uint64_t copyEnd = std::min(blockStart + current.Size, end);
		const bool whole = copyStart == blockStart && copyEnd == blockStart + current.Size;
		std::vector<uint8_t> partial;
		const uint8_t* decoded = nullptr;
		if (current.Codec == AssetPackCodec::Stored)
		{
			decoded = stored.data();
			if (whole)
				std::memcpy(destination + (blockStart - offset), stored.data(), current.Size);
		}
		else
		{
			uint8_t* output = destination + (blockStart - offset);
			if (!whole)
			{
				try
				{
					partial.resize(current.Size);
				}
				catch (const std::bad_alloc&)
				{
					outError = "out of memory";
					return false;
				}
				output = partial.data();
			}
			decoded = output;
			if (!t_Decompression.Context)
			{
				outError = "out of memory (decompression context)";
				return false;
			}
			const size_t result = ZSTD_decompressDCtx(t_Decompression.Context, output, current.Size, stored.data(), stored.size());
			if (ZSTD_isError(result) || result != current.Size)
			{
				outError = fmt::format("block {} is corrupt ({})", entry.FirstBlock + block, ZSTD_isError(result) ? ZSTD_getErrorName(result) : "wrong decompressed size");
				return false;
			}
			m_BlocksDecompressed.fetch_add(1, std::memory_order_relaxed);
			m_BytesDecompressed.fetch_add(current.Size, std::memory_order_relaxed);
		}
		m_BlocksDecoded.fetch_add(1, std::memory_order_relaxed);

		if (!whole)
			std::memcpy(destination + (copyStart - offset), decoded + (copyStart - blockStart), static_cast<size_t>(copyEnd - copyStart));
		return true;
	}

	std::vector<uint8_t> AssetPack::TakeBuffer() const
	{
		std::scoped_lock<std::mutex> lock(m_BufferMutex);
		if (m_FreeBuffers.empty())
			return {};
		std::vector<uint8_t> buffer = std::move(m_FreeBuffers.back());
		m_FreeBuffers.pop_back();
		return buffer;
	}

	void AssetPack::ReturnBuffer(std::vector<uint8_t> buffer, size_t lentCapacity) const
	{
		std::scoped_lock<std::mutex> lock(m_BufferMutex);
		// Room for c_MaxFreeReadBuffers was reserved when the pack opened, so keeping one never allocates.
		if (m_FreeBuffers.size() < c_MaxFreeReadBuffers)
		{
			m_BufferBytes += buffer.capacity() - lentCapacity;
			m_FreeBuffers.push_back(std::move(buffer));
		}
		else
		{
			m_BufferBytes -= lentCapacity; // Freed with `buffer`
		}
	}

	AssetPackStats AssetPack::GetStats() const
	{
		AssetPackStats stats;
		stats.Reads = m_Reads.load(std::memory_order_relaxed);
		stats.FileBytesRead = m_FileBytesRead.load(std::memory_order_relaxed);
		stats.BlocksDecoded = m_BlocksDecoded.load(std::memory_order_relaxed);
		stats.BlocksDecompressed = m_BlocksDecompressed.load(std::memory_order_relaxed);
		stats.BytesDecompressed = m_BytesDecompressed.load(std::memory_order_relaxed);
		std::scoped_lock<std::mutex> lock(m_BufferMutex);
		stats.BufferBytes = m_BufferBytes;
		return stats;
	}

}
