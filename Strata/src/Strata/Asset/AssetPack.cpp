#include "stpch.h"
#include "Strata/Asset/AssetPack.h"

#include "Strata/Core/BinaryStream.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Hash.h"
#include "Strata/Core/JobSystem.h"

#include <zstd.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <numeric>
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
		// Reads of consecutive blocks are merged up to this many stored bytes (few system calls for many blocks, without
		// holding the stored form of a large entry in memory at once). A read's blocks decode on the job system's workers,
		// in parallel once there are c_ParallelDecodeBlocks of them, while the next read runs: one reading I/O thread alone
		// would decompress far slower than the file is read.
		constexpr uint64_t c_ReadChunkBytes = 8ull << 20;
		constexpr uint32_t c_ParallelDecodeBlocks = 4;

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
		// The stored bytes of the blocks being read, and a block decoded for a range that covers only part of it.
		thread_local std::vector<uint8_t> t_StoredScratch[2]; // One being read while the other decodes
		thread_local bool t_StoredScratchInUse = false; // A read of this thread uses them (reads nest, see ReadBlocks)
		thread_local std::vector<uint8_t> t_BlockScratch;

		std::string DescribeEntry(const AssetPackEntry& entry)
		{
			return entry.Metadata.Path.empty() ? entry.Metadata.Name : entry.Metadata.Path;
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

	uint64_t AssetPack::HashContent(std::span<const uint8_t> data)
	{
		uint64_t hash = Hash::FNV1a(std::string_view());
		for (size_t offset = 0; offset < data.size(); offset += c_BlockSize)
			hash = Hash::Combine(hash, Hash::FNV1a(data.subspan(offset, std::min<size_t>(c_BlockSize, data.size() - offset))));
		return Hash::Combine(hash, data.size());
	}

	Scope<AssetPack> AssetPack::Open(const std::filesystem::path& path, std::string* outError)
	{
		auto fail = [outError, &path](const std::string& message) -> Scope<AssetPack>
		{
			if (outError)
				*outError = fmt::format("{}: {}", FileSystem::ToUTF8(path), message);
			return nullptr;
		};

		Scope<AssetPack> pack(new AssetPack());
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

		std::vector<std::string> strings;
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

			// The blocks cover the stored form exactly: full blocks, then the rest, consecutive in the file.
			const uint64_t expectedBlocks = (record.Size + header.BlockSize - 1) / header.BlockSize;
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
		if (size > std::numeric_limits<size_t>::max())
			return fail("the range is too large");
		try
		{
			outData.resize(static_cast<size_t>(size));
		}
		catch (const std::bad_alloc&)
		{
			return fail("out of memory");
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

		// The reads: runs of consecutive blocks of up to c_ReadChunkBytes stored bytes (at least one block each).
		struct Chunk
		{
			uint32_t First = 0; // Block index within the entry
			uint32_t Count = 0;
			uint64_t Bytes = 0;
		};
		const uint64_t end = offset + size;
		const uint32_t last = static_cast<uint32_t>((end - 1) / m_BlockSize);
		std::vector<Chunk> chunks;
		std::mutex errorMutex;
		std::string firstError; // Of the decoding; the first failure is the one reported
		auto report = [&](std::string error)
		{
			std::scoped_lock<std::mutex> lock(errorMutex);
			if (firstError.empty())
				firstError = std::move(error);
		};
		auto failed = [&]()
		{
			std::scoped_lock<std::mutex> lock(errorMutex);
			return !firstError.empty();
		};

		// A chunk's blocks write disjoint parts of the destination: they are decoded in parallel on the workers once there
		// are enough of them.
		auto decodeChunk = [&](const Chunk& chunk, const std::vector<uint8_t>& stored)
		{
			try
			{
				std::vector<uint64_t> starts(chunk.Count);
				for (uint32_t index = 1; index < chunk.Count; index++)
					starts[index] = starts[index - 1] + m_Blocks[entry.FirstBlock + chunk.First + index - 1].StoredSize;
				auto decode = [&](uint32_t index)
				{
					const AssetPackBlock& block = m_Blocks[entry.FirstBlock + chunk.First + index];
					std::string error;
					if (!DecodeBlock(entry, chunk.First + index, std::span<const uint8_t>(stored.data() + starts[index], block.StoredSize), offset, end,
						destination, error))
						report(std::move(error));
				};
				if (chunk.Count < c_ParallelDecodeBlocks)
				{
					for (uint32_t index = 0; index < chunk.Count; index++)
						decode(index);
				}
				else
				{
					JobSystem::ParallelFor(chunk.Count, 1, [&](uint32_t begin, uint32_t endIndex)
					{
						for (uint32_t index = begin; index < endIndex; index++)
							decode(index);
					});
				}
			}
			catch (const std::bad_alloc&)
			{
				report("out of memory");
			}
		};

		// A thread that waits for decoding helps run queued jobs, which may read packs too: only the outermost read of a
		// thread uses its buffers.
		std::vector<uint8_t> ownBuffers[2];
		std::vector<uint8_t>* buffers = ownBuffers;
		struct ClaimBuffers
		{
			bool Claimed = false;

			~ClaimBuffers()
			{
				if (Claimed)
					t_StoredScratchInUse = false;
			}
		} claimBuffers;
		if (!t_StoredScratchInUse)
		{
			t_StoredScratchInUse = true;
			claimBuffers.Claimed = true;
			buffers = t_StoredScratch;
		}

		// A chunk decodes while the next one is read, in the other buffer. The decoding job uses this function's locals, so
		// every way out of it waits for the job first.
		JobHandle decoding;
		struct WaitForDecoding
		{
			JobHandle& Handle;
			~WaitForDecoding() { Handle.Wait(); }
		} waitForDecoding { decoding };
		try
		{
			for (uint32_t block = static_cast<uint32_t>(offset / m_BlockSize); block <= last;)
			{
				Chunk chunk { block, 0, 0 };
				while (block <= last && (chunk.Count == 0 || chunk.Bytes + m_Blocks[entry.FirstBlock + block].StoredSize <= c_ReadChunkBytes))
				{
					chunk.Bytes += m_Blocks[entry.FirstBlock + block].StoredSize;
					chunk.Count++;
					block++;
				}
				chunks.push_back(chunk);
			}

			for (size_t index = 0; index < chunks.size(); index++)
			{
				const Chunk& chunk = chunks[index];
				std::vector<uint8_t>& stored = buffers[index % 2];
				stored.resize(static_cast<size_t>(chunk.Bytes));
				if (!m_Reader->ReadAt(m_Blocks[entry.FirstBlock + chunk.First].Offset, stored))
				{
					outError = "the file could not be read";
					return false;
				}
				m_FileBytesRead.fetch_add(chunk.Bytes, std::memory_order_relaxed);

				// The previous chunk is decoded, so its buffer can take the chunk after this one.
				decoding.Wait();
				decoding = {};
				if (failed())
					break;
				if (index + 1 == chunks.size())
					decodeChunk(chunk, stored); // Nothing left to read meanwhile
				else
					decoding = JobSystem::Submit([&decodeChunk, &chunk, &stored]() { decodeChunk(chunk, stored); }, JobPriority::High); // The read waits for it
			}
		}
		catch (const std::exception& exception)
		{
			outError = fmt::format("reading failed: {}", exception.what());
			return false;
		}
		decoding.Wait();
		if (failed())
		{
			outError = firstError;
			return false;
		}
		return true;
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

		// The part of the block the range covers: decoded in place when it is the whole block.
		const uint64_t blockStart = static_cast<uint64_t>(block) * m_BlockSize;
		const uint64_t copyStart = std::max(blockStart, offset);
		const uint64_t copyEnd = std::min(blockStart + current.Size, end);
		const bool whole = copyStart == blockStart && copyEnd == blockStart + current.Size;
		uint8_t* decoded = nullptr;
		if (whole)
		{
			decoded = destination + (blockStart - offset);
		}
		else
		{
			try
			{
				t_BlockScratch.resize(current.Size);
			}
			catch (const std::bad_alloc&)
			{
				outError = "out of memory";
				return false;
			}
			decoded = t_BlockScratch.data();
		}

		if (current.Codec == AssetPackCodec::Stored)
		{
			std::memcpy(decoded, stored.data(), current.Size);
		}
		else
		{
			if (!t_Decompression.Context)
			{
				outError = "out of memory (decompression context)";
				return false;
			}
			const size_t result = ZSTD_decompressDCtx(t_Decompression.Context, decoded, current.Size, stored.data(), stored.size());
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

	AssetPackStats AssetPack::GetStats() const
	{
		AssetPackStats stats;
		stats.Reads = m_Reads.load(std::memory_order_relaxed);
		stats.FileBytesRead = m_FileBytesRead.load(std::memory_order_relaxed);
		stats.BlocksDecoded = m_BlocksDecoded.load(std::memory_order_relaxed);
		stats.BlocksDecompressed = m_BlocksDecompressed.load(std::memory_order_relaxed);
		stats.BytesDecompressed = m_BytesDecompressed.load(std::memory_order_relaxed);
		return stats;
	}

}
