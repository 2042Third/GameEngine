#include "stpch.h"
#include "Strata/Asset/AssetPackWriter.h"

#include "Strata/Core/BinaryStream.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Hash.h"
#include "Strata/Core/JobSystem.h"

#include <zstd.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace Strata
{

	namespace
	{

		using namespace AssetPackFormat;

		// Blocks compressed together (in parallel) before they are written: 16 MiB of a large asset at a time, which bounds
		// the memory a write holds and lets it report progress within large assets.
		constexpr uint32_t c_BlocksPerBatch = 64;

		// One compression context per thread: the job system's workers reuse theirs for every block.
		struct CompressionContext
		{
			ZSTD_CCtx* Context = ZSTD_createCCtx();

			~CompressionContext()
			{
				ZSTD_freeCCtx(Context);
			}
		};

		thread_local CompressionContext t_Compression;

		bool IsKnownAssetType(AssetType type)
		{
			return type != AssetType::None && AssetTypeFromString(AssetTypeToString(type)) == type;
		}

		// A block on its way into the file.
		struct EncodedBlock
		{
			std::vector<uint8_t> Compressed; // Empty: stored as it is
			uint32_t Checksum = 0;
			uint64_t ContentHash = 0;        // FNV-1a of the block's decoded bytes (AssetPack::HashContent)
		};

		// The temporary file a pack is written into, with what the write reports.
		class PackFile
		{
		public:
			PackFile(const std::filesystem::path& path, const AssetPackWriteOptions& options, uint64_t totalWeight)
				: m_Stream(path, std::ios::binary | std::ios::trunc), m_Options(options), m_TotalWeight(totalWeight)
			{
			}

			bool IsOpen() const { return static_cast<bool>(m_Stream); }
			bool IsGood() const { return static_cast<bool>(m_Stream); }
			uint64_t GetPosition() const { return m_Position; }

			bool IsCancelled() const { return m_Options.Cancel && m_Options.Cancel->load(std::memory_order_relaxed); }

			void Write(const void* data, size_t size)
			{
				if (size == 0)
					return;
				m_Stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
				m_Position += size;
			}

			void PadTo(uint64_t alignment)
			{
				static constexpr uint8_t c_Zeros[AssetPack::c_EntryAlignment] = {};
				const uint64_t padding = (alignment - m_Position % alignment) % alignment;
				Write(c_Zeros, static_cast<size_t>(padding));
			}

			void WriteAt(uint64_t position, const void* data, size_t size)
			{
				m_Stream.seekp(static_cast<std::streamoff>(position));
				m_Stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
				m_Stream.seekp(static_cast<std::streamoff>(m_Position));
			}

			bool Finish()
			{
				m_Stream.flush();
				const bool good = static_cast<bool>(m_Stream);
				m_Stream.close();
				return good;
			}

			// The asset being written has `done` of its `size` bytes in the file.
			void ReportAsset(uint64_t weight, uint64_t done, uint64_t size)
			{
				if (!m_Options.Progress)
					return;
				const uint64_t share = size == 0 ? weight : static_cast<uint64_t>(static_cast<double>(weight) * static_cast<double>(done) / static_cast<double>(size));
				m_Options.Progress(std::min(m_DoneWeight + share, m_TotalWeight), m_TotalWeight);
			}

			void FinishAsset(uint64_t weight)
			{
				m_DoneWeight += weight;
				if (m_Options.Progress)
					m_Options.Progress(std::min(m_DoneWeight, m_TotalWeight), m_TotalWeight);
			}
		private:
			std::ofstream m_Stream;
			const AssetPackWriteOptions& m_Options;
			uint64_t m_Position = 0;
			uint64_t m_TotalWeight = 0;
			uint64_t m_DoneWeight = 0;
		};

		uint64_t GetWeight(const AssetMetadata& metadata)
		{
			return std::max<uint64_t>(metadata.StoredSize, 1);
		}

		// Compresses the blocks [first, first + count) of `data` in parallel.
		void EncodeBlocks(std::span<const uint8_t> data, uint32_t first, uint32_t count, int level, std::vector<EncodedBlock>& outBlocks)
		{
			outBlocks.resize(count);
			JobSystem::ParallelFor(count, 1, [&](uint32_t begin, uint32_t end)
			{
				for (uint32_t index = begin; index < end; index++)
				{
					const size_t offset = static_cast<size_t>(first + index) * AssetPack::c_BlockSize;
					const std::span<const uint8_t> raw = data.subspan(offset, std::min<size_t>(AssetPack::c_BlockSize, data.size() - offset));
					EncodedBlock& block = outBlocks[index];
					block.Compressed.clear();
					block.ContentHash = Hash::FNV1a(raw);

					// Compressed only when that saves at least an eighth (c_MinCompressionSaving): zstd stops once the output
					// would not fit in seven eighths. Without memory for the output the block is stored as it is: a valid pack,
					// only larger.
					const size_t capacity = raw.size() - raw.size() / AssetPack::c_MinCompressionSaving - 1;
					if (t_Compression.Context && capacity > 0)
					{
						try
						{
							block.Compressed.resize(capacity);
							const size_t result = ZSTD_compressCCtx(t_Compression.Context, block.Compressed.data(), capacity, raw.data(), raw.size(), level);
							if (ZSTD_isError(result) || result == 0 || result >= raw.size())
								block.Compressed.clear();
							else
								block.Compressed.resize(result);
						}
						catch (const std::bad_alloc&)
						{
							block.Compressed.clear();
						}
					}
					block.Checksum = Hash::CRC32C(block.Compressed.empty() ? raw : std::span<const uint8_t>(block.Compressed));
				}
			});
		}

		bool WriteVersion2(PackFile& file, std::span<const AssetPackSource> assets, const AssetPackWriter::DataProvider& provider, const AssetPackWriteOptions& options,
			AssetPackWriteStats& stats, std::string& outError)
		{
			Header header;
			file.Write(&header, sizeof(header)); // Written again once the tables are known

			std::vector<EntryRecord> entries;
			entries.reserve(assets.size());
			std::vector<BlockRecord> blocks;
			std::vector<uint64_t> dependencies;
			std::vector<StringRecord> strings;
			std::vector<uint8_t> stringBytes;
			std::unordered_map<std::string, uint32_t> stringIndices;
			auto addString = [&](const std::string& text) -> std::optional<uint32_t>
			{
				auto it = stringIndices.find(text);
				if (it != stringIndices.end())
					return it->second;
				if (stringBytes.size() + text.size() > UINT32_MAX || strings.size() >= UINT32_MAX)
					return std::nullopt;
				const uint32_t index = static_cast<uint32_t>(strings.size());
				strings.push_back(StringRecord { static_cast<uint32_t>(stringBytes.size()), static_cast<uint32_t>(text.size()) });
				stringBytes.insert(stringBytes.end(), text.begin(), text.end());
				stringIndices.emplace(text, index);
				return index;
			};

			std::vector<uint8_t> data;
			std::vector<EncodedBlock> encoded;
			uint64_t nameBytes = 0; // What the entries name, counted per entry (see AssetPackFormat::GetMaxNameBytes)
			for (const AssetPackSource& source : assets)
			{
				const AssetMetadata& metadata = source.Metadata;
				if (file.IsCancelled())
				{
					outError = "Cancelled";
					return false;
				}

				data.clear();
				std::string assetError;
				if (!provider(metadata, data, &assetError))
				{
					outError = fmt::format("'{}' ({}): {}", metadata.Path.empty() ? metadata.Name : metadata.Path, metadata.Handle.ToString(), assetError);
					return false;
				}

				const uint64_t blockCount = (data.size() + AssetPack::c_BlockSize - 1) / AssetPack::c_BlockSize;
				if (blocks.size() + blockCount > UINT32_MAX)
				{
					outError = "Too many blocks for one pack";
					return false;
				}

				EntryRecord entry;
				entry.Handle = static_cast<uint64_t>(metadata.Handle);
				entry.Parent = static_cast<uint64_t>(metadata.Parent);
				entry.Size = data.size();
				entry.Type = static_cast<uint16_t>(metadata.Type);
				const std::optional<uint32_t> path = addString(metadata.Path);
				const std::optional<uint32_t> key = addString(metadata.SubAssetKey);
				const std::optional<uint32_t> name = addString(metadata.Name);
				if (!path || !key || !name)
				{
					outError = "Too many names for one pack";
					return false;
				}
				entry.PathString = *path;
				entry.SubAssetKeyString = *key;
				entry.NameString = *name;
				nameBytes += metadata.Path.size() + metadata.SubAssetKey.size() + metadata.Name.size();
				entry.FirstBlock = static_cast<uint32_t>(blocks.size());
				entry.BlockCount = static_cast<uint32_t>(blockCount);

				// Dependencies (once each, never itself or the null handle).
				entry.FirstDependency = static_cast<uint32_t>(dependencies.size());
				std::unordered_set<uint64_t> seen;
				for (AssetHandle dependency : source.Dependencies)
				{
					const uint64_t value = static_cast<uint64_t>(dependency);
					if (value != 0 && dependency != metadata.Handle && seen.insert(value).second)
						dependencies.push_back(value);
				}
				if (dependencies.size() > UINT32_MAX)
				{
					outError = "Too many dependencies for one pack";
					return false;
				}
				entry.DependencyCount = static_cast<uint32_t>(dependencies.size() - entry.FirstDependency);

				if (source.Bounds && source.Bounds->IsValid())
				{
					bool finite = true;
					for (int axis = 0; axis < 3; axis++)
						finite = finite && std::isfinite(source.Bounds->Min[axis]) && std::isfinite(source.Bounds->Max[axis]);
					if (finite)
					{
						entry.Flags |= c_EntryHasBounds;
						for (int axis = 0; axis < 3; axis++)
						{
							entry.BoundsMin[axis] = source.Bounds->Min[axis];
							entry.BoundsMax[axis] = source.Bounds->Max[axis];
						}
					}
				}

				// The stored form, block by block, from a 4 KiB boundary on.
				if (!data.empty())
					file.PadTo(AssetPack::c_EntryAlignment);
				uint64_t contentHash = Hash::FNV1a(std::string_view());
				const uint64_t weight = GetWeight(metadata);
				for (uint32_t first = 0; first < blockCount; first += c_BlocksPerBatch)
				{
					if (file.IsCancelled())
					{
						outError = "Cancelled";
						return false;
					}
					const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(c_BlocksPerBatch, blockCount - first));
					EncodeBlocks(data, first, count, options.CompressionLevel, encoded);
					for (uint32_t index = 0; index < count; index++)
					{
						const EncodedBlock& block = encoded[index];
						const size_t offset = static_cast<size_t>(first + index) * AssetPack::c_BlockSize;
						const uint32_t size = static_cast<uint32_t>(std::min<size_t>(AssetPack::c_BlockSize, data.size() - offset));
						BlockRecord record;
						record.Offset = file.GetPosition();
						record.Size = size;
						record.Checksum = block.Checksum;
						if (block.Compressed.empty())
						{
							record.StoredSize = size;
							record.Codec = static_cast<uint32_t>(AssetPackCodec::Stored);
							file.Write(data.data() + offset, size);
						}
						else
						{
							record.StoredSize = static_cast<uint32_t>(block.Compressed.size());
							record.Codec = static_cast<uint32_t>(AssetPackCodec::Zstd);
							file.Write(block.Compressed.data(), block.Compressed.size());
							stats.CompressedBlocks++;
						}
						blocks.push_back(record);
						contentHash = Hash::Combine(contentHash, block.ContentHash);
					}
					file.ReportAsset(weight, std::min<uint64_t>(static_cast<uint64_t>(first + count) * AssetPack::c_BlockSize, data.size()), data.size());
				}
				entry.ContentHash = Hash::Combine(contentHash, data.size());
				entries.push_back(entry);
				stats.StoredBytes += data.size();
				file.FinishAsset(weight);
				if (!file.IsGood())
				{
					outError = "Writing failed (disk full?)";
					return false;
				}
			}

			header.Magic = AssetPack::c_Magic;
			header.Version = 2;
			header.HeaderSize = sizeof(Header);
			header.BlockSize = AssetPack::c_BlockSize;
			header.EntryCount = static_cast<uint32_t>(entries.size());
			header.StringCount = static_cast<uint32_t>(strings.size());
			header.BlockCount = static_cast<uint32_t>(blocks.size());
			header.DependencyCount = static_cast<uint32_t>(dependencies.size());
			header.StringBytes = stringBytes.size();
			header.TablesOffset = file.GetPosition();

			BinaryWriter tables;
			tables.WriteBytes(entries.data(), entries.size() * sizeof(EntryRecord));
			tables.WriteBytes(strings.data(), strings.size() * sizeof(StringRecord));
			tables.WriteBytes(stringBytes.data(), stringBytes.size());
			tables.WriteBytes(blocks.data(), blocks.size() * sizeof(BlockRecord));
			tables.WriteBytes(dependencies.data(), dependencies.size() * sizeof(uint64_t));
			// Readers refuse packs whose entries name more than this (a pack of many empty entries with long shared names).
			const uint64_t fileSize = file.GetPosition() + tables.GetSize();
			if (nameBytes > GetMaxNameBytes(fileSize))
			{
				outError = fmt::format("The assets' paths and names take {} bytes, more than a pack of {} bytes may name ({})", nameBytes, fileSize,
					GetMaxNameBytes(fileSize));
				return false;
			}
			header.TablesChecksum = Hash::CRC32C(tables.GetData());
			header.HeaderChecksum = Hash::CRC32C(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&header), offsetof(Header, HeaderChecksum)));
			file.Write(tables.GetData().data(), tables.GetSize());
			file.WriteAt(0, &header, sizeof(header));

			stats.Blocks = static_cast<uint32_t>(blocks.size());
			stats.FileSize = file.GetPosition();
			return true;
		}

		// The version 1 layout: a header, the data blobs back to back, then the entry table.
		bool WriteVersion1(PackFile& file, std::span<const AssetPackSource> assets, const AssetPackWriter::DataProvider& provider, AssetPackWriteStats& stats,
			std::string& outError)
		{
			HeaderV1 header;
			file.Write(&header, sizeof(header));

			BinaryWriter table;
			std::vector<uint8_t> data;
			for (const AssetPackSource& source : assets)
			{
				const AssetMetadata& metadata = source.Metadata;
				if (file.IsCancelled())
				{
					outError = "Cancelled";
					return false;
				}
				data.clear();
				std::string assetError;
				if (!provider(metadata, data, &assetError))
				{
					outError = fmt::format("'{}' ({}): {}", metadata.Path.empty() ? metadata.Name : metadata.Path, metadata.Handle.ToString(), assetError);
					return false;
				}

				table.Write(static_cast<uint64_t>(metadata.Handle));
				table.Write(static_cast<uint16_t>(metadata.Type));
				table.Write(static_cast<uint16_t>(0));
				table.Write(static_cast<uint64_t>(metadata.Parent));
				table.Write(file.GetPosition());
				table.Write(static_cast<uint64_t>(data.size()));
				table.WriteString(metadata.Path);
				table.WriteString(metadata.SubAssetKey);
				table.WriteString(metadata.Name);
				file.Write(data.data(), data.size());
				stats.StoredBytes += data.size();
				file.FinishAsset(GetWeight(metadata));
				if (!file.IsGood())
				{
					outError = "Writing failed (disk full?)";
					return false;
				}
			}

			header.Magic = AssetPack::c_Magic;
			header.Version = 1;
			header.EntryCount = static_cast<uint32_t>(assets.size());
			header.TableOffset = file.GetPosition();
			header.TableSize = table.GetSize();
			file.Write(table.GetData().data(), table.GetSize());
			file.WriteAt(0, &header, sizeof(header));
			stats.FileSize = file.GetPosition();
			return true;
		}

	}

	bool AssetPackWriter::Write(const std::filesystem::path& path, std::span<const AssetPackSource> assets, const DataProvider& provider, const AssetPackWriteOptions& options,
		AssetPackWriteStats* outStats, std::string* outError)
	{
		ST_PROFILE_FUNCTION();
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		if (options.Version != 1 && options.Version != AssetPack::c_Version)
			return fail(fmt::format("Cannot write asset pack version {} (versions 1 and {} can be written)", options.Version, AssetPack::c_Version));
		if (assets.size() > UINT32_MAX)
			return fail("Too many assets for one pack");
		if (options.CompressionLevel < ZSTD_minCLevel() || options.CompressionLevel > ZSTD_maxCLevel())
			return fail(fmt::format("Invalid compression level {} ({} to {})", options.CompressionLevel, ZSTD_minCLevel(), ZSTD_maxCLevel()));
		std::unordered_set<uint64_t> handles;
		uint64_t totalWeight = 0;
		for (const AssetPackSource& source : assets)
		{
			if (!source.Metadata.Handle.IsValid() || !IsKnownAssetType(source.Metadata.Type) || !handles.insert(static_cast<uint64_t>(source.Metadata.Handle)).second)
				return fail(fmt::format("'{}' cannot be packed: its handle {} is null or used twice, or its type is unknown", source.Metadata.Path, source.Metadata.Handle.ToString()));
			totalWeight += GetWeight(source.Metadata);
		}

		// A unique temporary name: concurrent builds of the same pack never write into one file.
		std::filesystem::path temporaryPath = path;
		temporaryPath += "." + UUID().ToString() + ".tmp";
		if (path.has_parent_path() && !FileSystem::CreateDirectories(path.parent_path()))
			return fail(fmt::format("Could not create '{}'", FileSystem::ToUTF8(path.parent_path())));

		AssetPackWriteStats stats;
		std::string error;
		bool success = false;
		{
			PackFile file(temporaryPath, options, totalWeight);
			if (!file.IsOpen())
				return fail(fmt::format("Could not create '{}'", FileSystem::ToUTF8(temporaryPath)));
			try
			{
				success = options.Version == 1 ? WriteVersion1(file, assets, provider, stats, error) : WriteVersion2(file, assets, provider, options, stats, error);
			}
			catch (const std::bad_alloc&)
			{
				success = false;
				error = "Out of memory";
			}
			if (!file.Finish() && success)
			{
				success = false;
				error = fmt::format("Writing '{}' failed (disk full?)", FileSystem::ToUTF8(temporaryPath));
			}
		}

		if (!success)
		{
			FileSystem::Remove(temporaryPath);
			return fail(error);
		}
		if (!FileSystem::Rename(temporaryPath, path))
		{
			FileSystem::Remove(temporaryPath);
			return fail(fmt::format("Could not replace '{}'", FileSystem::ToUTF8(path)));
		}
		if (outStats)
			*outStats = stats;
		return true;
	}

	bool AssetPackWriter::Write(const std::filesystem::path& path, const std::vector<AssetMetadata>& assets, const DataProvider& provider, std::string* outError)
	{
		std::vector<AssetPackSource> sources;
		sources.reserve(assets.size());
		for (const AssetMetadata& metadata : assets)
			sources.push_back(AssetPackSource { metadata, {}, std::nullopt });
		return Write(path, sources, provider, AssetPackWriteOptions(), nullptr, outError);
	}

}
