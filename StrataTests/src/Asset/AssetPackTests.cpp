#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Strata/Asset/AssetPack.h"
#include "Strata/Asset/AssetPackWriter.h"
#include "Strata/Core/FileReader.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Hash.h"
#include "Strata/Core/JobSystem.h"
#include "TestHelpers.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	using namespace AssetPackFormat;

	AssetMetadata MakeMetadata(uint64_t handle, AssetType type, std::string path, std::string name = {})
	{
		AssetMetadata metadata;
		metadata.Handle = UUID(handle);
		metadata.Type = type;
		metadata.Path = std::move(path);
		metadata.Name = name.empty() ? metadata.Path : std::move(name);
		return metadata;
	}

	// Deterministic content of the kinds packs hold: text-like data that compresses well, noise that does not, and
	// both mixed.
	enum class ContentKind : uint8_t
	{
		Compressible,
		Incompressible,
		Mixed
	};

	std::vector<uint8_t> MakeContent(size_t size, ContentKind kind, uint32_t seed)
	{
		std::vector<uint8_t> data(size);
		uint32_t state = seed * 2654435761u + 1u;
		auto next = [&state]()
		{
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;
			return state;
		};
		static constexpr char c_Words[] = "\"Translation\": [0.0, 1.5, -2.25], \"Material\": \"00000000DEADBEEF\", \"Visible\": true, ";
		for (size_t index = 0; index < size; index++)
		{
			const bool noise = kind == ContentKind::Incompressible || (kind == ContentKind::Mixed && (index / 4096) % 2 == 1);
			data[index] = noise ? static_cast<uint8_t>(next() >> 24) : static_cast<uint8_t>(c_Words[(index + seed) % (sizeof(c_Words) - 1)] + (index / 65536) % 3);
		}
		return data;
	}

	struct PackSource
	{
		AssetPackSource Source;
		std::vector<uint8_t> Data;
	};

	// Writes a pack of the sources (in their order) and returns its path.
	std::filesystem::path WritePack(const std::filesystem::path& path, const std::vector<PackSource>& sources, const AssetPackWriteOptions& options = {},
		AssetPackWriteStats* outStats = nullptr)
	{
		std::vector<AssetPackSource> assets;
		std::map<uint64_t, const std::vector<uint8_t>*> data;
		for (const PackSource& source : sources)
		{
			assets.push_back(source.Source);
			data[static_cast<uint64_t>(source.Source.Metadata.Handle)] = &source.Data;
		}
		std::string error;
		const bool written = AssetPackWriter::Write(path, assets, [&data](const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string*)
		{
			outData = *data.at(static_cast<uint64_t>(metadata.Handle));
			return true;
		}, options, outStats, &error);
		REQUIRE_MESSAGE(written, error);
		return path;
	}

	PackSource MakeSource(uint64_t handle, std::vector<uint8_t> data, AssetType type = AssetType::Texture)
	{
		PackSource source;
		source.Source.Metadata = MakeMetadata(handle, type, "Assets/" + std::to_string(handle) + ".bin", "Asset " + std::to_string(handle));
		source.Source.Metadata.StoredSize = data.size();
		source.Data = std::move(data);
		return source;
	}

	// A version 2 pack taken apart, to write back changed with checksums that match, so that tests reach the validation of
	// what the checksums protect.
	struct PackImage
	{
		Header PackHeader;
		std::vector<uint8_t> Data; // Everything before the tables (the header included)
		std::vector<EntryRecord> Entries;
		std::vector<StringRecord> Strings;
		std::vector<uint8_t> StringBytes;
		std::vector<BlockRecord> Blocks;
		std::vector<uint64_t> Dependencies;

		static PackImage Read(const std::filesystem::path& path)
		{
			const std::optional<std::vector<uint8_t>> file = FileSystem::ReadBytes(path);
			REQUIRE(file);
			PackImage image;
			REQUIRE(file->size() >= sizeof(Header));
			std::memcpy(&image.PackHeader, file->data(), sizeof(Header));
			const Header& header = image.PackHeader;
			REQUIRE(header.Version == 2);
			image.Data.assign(file->begin(), file->begin() + static_cast<std::ptrdiff_t>(header.TablesOffset));
			const uint8_t* cursor = file->data() + header.TablesOffset;
			auto take = [&cursor](auto& vector, size_t count)
			{
				vector.resize(count);
				std::memcpy(vector.data(), cursor, count * sizeof(vector[0]));
				cursor += count * sizeof(vector[0]);
			};
			take(image.Entries, header.EntryCount);
			take(image.Strings, header.StringCount);
			take(image.StringBytes, static_cast<size_t>(header.StringBytes));
			take(image.Blocks, header.BlockCount);
			take(image.Dependencies, header.DependencyCount);
			REQUIRE(cursor == file->data() + file->size());
			return image;
		}

		// The pack's bytes, with the counts of the tables and both checksums made to match them.
		std::vector<uint8_t> Write() const
		{
			Header header = PackHeader;
			header.EntryCount = static_cast<uint32_t>(Entries.size());
			header.StringCount = static_cast<uint32_t>(Strings.size());
			header.StringBytes = StringBytes.size();
			header.BlockCount = static_cast<uint32_t>(Blocks.size());
			header.DependencyCount = static_cast<uint32_t>(Dependencies.size());
			header.TablesOffset = Data.size();
			std::vector<uint8_t> tables;
			auto append = [&tables](const auto& vector)
			{
				const uint8_t* bytes = reinterpret_cast<const uint8_t*>(vector.data());
				tables.insert(tables.end(), bytes, bytes + vector.size() * sizeof(vector[0]));
			};
			append(Entries);
			append(Strings);
			append(StringBytes);
			append(Blocks);
			append(Dependencies);
			header.TablesChecksum = Hash::CRC32C(tables);
			std::vector<uint8_t> bytes = Data;
			bytes.insert(bytes.end(), tables.begin(), tables.end());
			std::memcpy(bytes.data(), &header, sizeof(header));
			Reseal(bytes);
			return bytes;
		}

		// Makes the header checksum match the header's bytes.
		static void Reseal(std::vector<uint8_t>& bytes)
		{
			const uint32_t checksum = Hash::CRC32C(std::span<const uint8_t>(bytes.data(), offsetof(Header, HeaderChecksum)));
			std::memcpy(bytes.data() + offsetof(Header, HeaderChecksum), &checksum, sizeof(checksum));
		}
	};

	// Opening `bytes` as a pack fails with an error (and does not crash).
	void CheckRejected(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, std::string_view expected = {})
	{
		REQUIRE(FileSystem::WriteBytes(path, bytes));
		std::string error;
		const Scope<AssetPack> pack = AssetPack::Open(path, &error);
		CHECK_FALSE(pack);
		CHECK_FALSE(error.empty());
		if (!expected.empty())
		{
			INFO(error);
			CHECK(error.find(expected) != std::string::npos);
		}
	}

	// Opens the pack, and if it opens, reads every entry: whatever the bytes, there is an answer and never a crash.
	void OpenAndReadEverything(const std::filesystem::path& path)
	{
		std::string error;
		const Scope<AssetPack> pack = AssetPack::Open(path, &error);
		if (!pack)
		{
			CHECK_FALSE(error.empty());
			return;
		}
		std::vector<uint8_t> data;
		for (const AssetPackEntry& entry : pack->GetEntries())
		{
			error.clear();
			if (!pack->ReadData(entry, data, &error))
				CHECK_FALSE(error.empty());
		}
	}

}

TEST_SUITE("Asset.Pack")
{
	TEST_CASE("Asset packs round trip metadata, data, dependencies and bounds")
	{
		AssetMetadata parent = MakeMetadata(0x1000, AssetType::Model, "Models/Robot.gltf", "Robot");
		AssetMetadata child = MakeMetadata(0x1001, AssetType::Mesh, "Models/Robot.gltf", "Body");
		child.Parent = parent.Handle;
		child.SubAssetKey = "Mesh/0";
		AssetMetadata material = MakeMetadata(0x1002, AssetType::Material, "Models/Robot.gltf", "Paint");
		material.Parent = parent.Handle;
		material.SubAssetKey = "Material/0";

		std::vector<PackSource> sources(3);
		sources[0].Source = AssetPackSource { parent, { child.Handle, material.Handle, child.Handle, parent.Handle, UUID::Null() }, std::nullopt };
		sources[0].Data = { 1, 2, 3, 4, 5 };
		sources[1].Source = AssetPackSource { child, { material.Handle }, AABB(glm::vec3(-1.0f, -2.0f, -3.0f), glm::vec3(1.0f, 2.0f, 3.0f)) };
		sources[1].Data = MakeContent(AssetPack::c_BlockSize + 17, ContentKind::Compressible, 1);
		sources[2].Source = AssetPackSource { material, {}, AABB() }; // An empty box records no bounds
		const std::filesystem::path path = WritePack(CreateTemporaryDirectory("PackRoundTrip") / "Game.stpak", sources);

		std::string error;
		Scope<AssetPack> pack = AssetPack::Open(path, &error);
		REQUIRE_MESSAGE(pack, error);
		CHECK(pack->GetVersion() == AssetPack::c_Version);
		REQUIRE(pack->GetEntries().size() == 3);
		const AssetPackEntry& first = pack->GetEntries()[0];
		CHECK(first.Metadata.Handle == parent.Handle);
		CHECK(first.Metadata.Type == AssetType::Model);
		CHECK(first.Metadata.Path == "Models/Robot.gltf");
		CHECK(first.Metadata.Name == "Robot");
		CHECK(first.Metadata.StoredSize == 5);
		CHECK_FALSE(first.Bounds.has_value());
		// Dependencies are kept once each, without the asset itself or null handles.
		const std::span<const AssetHandle> dependencies = pack->GetDependencies(first);
		CHECK(std::vector<AssetHandle>(dependencies.begin(), dependencies.end()) == std::vector<AssetHandle> { child.Handle, material.Handle });

		const AssetPackEntry& second = pack->GetEntries()[1];
		CHECK(second.Metadata.Parent == parent.Handle);
		CHECK(second.Metadata.SubAssetKey == "Mesh/0");
		CHECK(second.Size == sources[1].Data.size());
		REQUIRE(second.Bounds.has_value());
		CHECK(second.Bounds->Min == glm::vec3(-1.0f, -2.0f, -3.0f));
		CHECK(second.Bounds->Max == glm::vec3(1.0f, 2.0f, 3.0f));
		CHECK(second.BlockCount == 2);
		CHECK(pack->GetBlocks()[second.FirstBlock].Offset % AssetPack::c_EntryAlignment == 0); // Entries start 4 KiB-aligned
		CHECK(second.ContentHash == AssetPack::HashContent(sources[1].Data));

		const AssetPackEntry& third = pack->GetEntries()[2];
		CHECK(third.Size == 0);
		CHECK(third.BlockCount == 0);
		CHECK_FALSE(third.Bounds.has_value());

		std::vector<uint8_t> data;
		REQUIRE(pack->ReadData(first, data, &error));
		CHECK(data == sources[0].Data);
		REQUIRE(pack->ReadData(second, data, &error));
		CHECK(data == sources[1].Data);
		REQUIRE(pack->ReadData(third, data, &error));
		CHECK(data.empty());
		CHECK_FALSE(pack->ReadRange(first, 3, 3, data, &error)); // Past its end
		CHECK(error.find("outside") != std::string::npos);

		// A provider failure aborts the write and leaves no file behind, nor does a refused asset list.
		const std::filesystem::path failedPath = path.parent_path() / "Failed.stpak";
		CHECK_FALSE(AssetPackWriter::Write(failedPath, { parent }, [](const AssetMetadata&, std::vector<uint8_t>&, std::string* outError)
		{
			*outError = "missing";
			return false;
		}, &error));
		CHECK(error.find("missing") != std::string::npos);
		CHECK_FALSE(FileSystem::Exists(failedPath));
		CHECK_FALSE(AssetPackWriter::Write(failedPath, { parent, parent }, [](const AssetMetadata&, std::vector<uint8_t>&, std::string*) { return true; }, &error));
		CHECK(error.find("used twice") != std::string::npos);
		CHECK_FALSE(AssetPackWriter::Write(failedPath, { MakeMetadata(0, AssetType::Material, "Null.stmat") }, [](const AssetMetadata&, std::vector<uint8_t>&, std::string*) { return true; },
			&error));
		size_t leftovers = 0;
		for (const std::filesystem::directory_entry& file : std::filesystem::directory_iterator(path.parent_path()))
			leftovers += file.path().filename() != "Game.stpak" ? 1 : 0;
		CHECK(leftovers == 0);
	}

	TEST_CASE("200 entries of 0-3 MB, compressible and not, round trip byte for byte")
	{
		ScopedJobSystem jobs(8, 2);
		std::vector<PackSource> sources;
		uint32_t state = 7;
		for (uint32_t index = 0; index < 200; index++)
		{
			state = state * 1664525u + 1013904223u;
			// Sizes across 0-3 MB, most of them small like a game's, with the block boundaries among them.
			size_t size = 0;
			switch (index % 10)
			{
				case 0: size = std::vector<size_t> { 0, 1, AssetPack::c_BlockSize - 1, AssetPack::c_BlockSize, AssetPack::c_BlockSize + 1, 3u << 20 }[(index / 10) % 6]; break;
				case 1: case 2: size = (state >> 8) % (3u << 20); break;
				default: size = (state >> 8) % (192u << 10); break;
			}
			const ContentKind kind = static_cast<ContentKind>(index % 3);
			sources.push_back(MakeSource(0x10000 + index, MakeContent(size, kind, index)));
		}
		AssetPackWriteStats stats;
		const std::filesystem::path path = WritePack(CreateTemporaryDirectory("PackRoundTrip200") / "Game.stpak", sources, {}, &stats);
		CHECK(stats.Blocks > 200);
		CHECK(stats.CompressedBlocks > 0);
		CHECK(stats.CompressedBlocks < stats.Blocks); // The noise is stored as it is
		CHECK(stats.FileSize < stats.StoredBytes);
		CHECK(FileSystem::GetFileSize(path) == stats.FileSize);

		std::string error;
		Scope<AssetPack> pack = AssetPack::Open(path, &error);
		REQUIRE_MESSAGE(pack, error);
		REQUIRE(pack->GetEntries().size() == sources.size());
		std::vector<uint8_t> data;
		for (size_t index = 0; index < sources.size(); index++)
		{
			CAPTURE(index);
			const AssetPackEntry& entry = pack->GetEntries()[index];
			CHECK(entry.Metadata.Handle == sources[index].Source.Metadata.Handle);
			CHECK(entry.Metadata.Name == sources[index].Source.Metadata.Name);
			REQUIRE(pack->ReadData(entry, data, &error));
			CHECK(data == sources[index].Data);
			CHECK(entry.ContentHash == AssetPack::HashContent(sources[index].Data));
			if (entry.BlockCount > 0)
				CHECK(pack->GetBlocks()[entry.FirstBlock].Offset % AssetPack::c_EntryAlignment == 0);
		}
		const AssetPackStats readStats = pack->GetStats();
		CHECK(readStats.Reads == sources.size());
		CHECK(readStats.BlocksDecoded == stats.Blocks);
		CHECK(readStats.BlocksDecompressed == stats.CompressedBlocks);
	}

	TEST_CASE("Ranges decode only the blocks that cover them and equal the slices of the whole")
	{
		ScopedJobSystem jobs(8, 2);
		std::vector<PackSource> sources;
		sources.push_back(MakeSource(0x2000, MakeContent(5 * AssetPack::c_BlockSize + 1234, ContentKind::Mixed, 3)));
		const std::filesystem::path path = WritePack(CreateTemporaryDirectory("PackRanges") / "Game.stpak", sources);
		std::string error;
		Scope<AssetPack> pack = AssetPack::Open(path, &error);
		REQUIRE_MESSAGE(pack, error);
		const AssetPackEntry& entry = pack->GetEntries()[0];
		std::vector<uint8_t> whole;
		REQUIRE(pack->ReadData(entry, whole, &error));
		REQUIRE(whole == sources[0].Data);

		constexpr uint64_t c_Block = AssetPack::c_BlockSize;
		const uint64_t size = whole.size();
		const std::vector<std::pair<uint64_t, uint64_t>> ranges = {
			{ 0, 0 }, { 0, 1 }, { 0, c_Block }, { c_Block - 1, 2 }, { c_Block, c_Block }, { c_Block - 100, 2 * c_Block + 200 }, { 3 * c_Block + 5, 1 },
			{ size - 1, 1 }, { size - 1234, 1234 }, { size - 1235, 1235 }, { 17, size - 17 }, { 0, size }, { size, 0 } };
		std::vector<uint8_t> range;
		for (const auto& [offset, length] : ranges)
		{
			CAPTURE(offset);
			CAPTURE(length);
			REQUIRE(pack->ReadRange(entry, offset, length, range, &error));
			CHECK(std::equal(range.begin(), range.end(), whole.begin() + static_cast<std::ptrdiff_t>(offset), whole.begin() + static_cast<std::ptrdiff_t>(offset + length)));
			CHECK(range.size() == length);
		}
		CHECK_FALSE(pack->ReadRange(entry, size, 1, range, &error));
		CHECK_FALSE(pack->ReadRange(entry, 1, size, range, &error));
		CHECK_FALSE(pack->ReadRange(entry, UINT64_MAX, 2, range, &error));

		// Two blocks at most for a range across a boundary, one inside a block.
		const uint64_t decodedBefore = pack->GetStats().BlocksDecoded;
		REQUIRE(pack->ReadRange(entry, 2 * c_Block - 10, 20, range, &error));
		CHECK(pack->GetStats().BlocksDecoded - decodedBefore == 2);
		REQUIRE(pack->ReadRange(entry, 2 * c_Block + 10, 20, range, &error));
		CHECK(pack->GetStats().BlocksDecoded - decodedBefore == 3);
	}

	TEST_CASE("A 64 KiB range of a 90 MB entry decompresses at most two blocks")
	{
		ScopedJobSystem jobs(16, 2);
		std::vector<PackSource> sources;
		sources.push_back(MakeSource(0x3000, MakeContent(90u << 20, ContentKind::Compressible, 11)));
		AssetPackWriteOptions options;
		options.CompressionLevel = 1; // The level does not matter here; the lowest keeps a Debug run short
		AssetPackWriteStats stats;
		const std::filesystem::path path = WritePack(CreateTemporaryDirectory("PackLargeEntry") / "Game.stpak", sources, options, &stats);
		CHECK(stats.CompressedBlocks == stats.Blocks);
		CHECK(stats.FileSize < (90u << 20) / 4);

		std::string error;
		Scope<AssetPack> pack = AssetPack::Open(path, &error);
		REQUIRE_MESSAGE(pack, error);
		const AssetPackEntry& entry = pack->GetEntries()[0];
		std::vector<uint8_t> range;
		for (const uint64_t offset : { uint64_t(45) << 20, (uint64_t(45) << 20) - 32768, (uint64_t(90) << 20) - 65536 })
		{
			CAPTURE(offset);
			const AssetPackStats before = pack->GetStats();
			REQUIRE(pack->ReadRange(entry, offset, 64 * 1024, range, &error));
			const AssetPackStats after = pack->GetStats();
			CHECK(after.BlocksDecompressed - before.BlocksDecompressed <= 2);
			CHECK(after.FileBytesRead - before.FileBytesRead <= 2 * AssetPack::c_BlockSize);
			CHECK(std::equal(range.begin(), range.end(), sources[0].Data.begin() + static_cast<std::ptrdiff_t>(offset)));
		}
	}

	TEST_CASE("Eight threads read random ranges of a pack opened once")
	{
		ScopedJobSystem jobs(8, 2);
		std::vector<PackSource> sources;
		for (uint32_t index = 0; index < 24; index++)
			sources.push_back(MakeSource(0x4000 + index, MakeContent(index * 97'000 + 5'000, static_cast<ContentKind>(index % 3), index)));
		const std::filesystem::path path = WritePack(CreateTemporaryDirectory("PackThreads") / "Game.stpak", sources);
		std::vector<uint32_t> expectedChecksums;
		for (const PackSource& source : sources)
			expectedChecksums.push_back(Hash::CRC32C(source.Data));

		const uint64_t opensBefore = FileReader::GetOpenCount();
		std::string error;
		Scope<AssetPack> pack = AssetPack::Open(path, &error);
		REQUIRE_MESSAGE(pack, error);

		std::atomic<uint32_t> failures = 0;
		std::atomic<uint32_t> reads = 0;
		std::vector<std::thread> threads;
		for (uint32_t thread = 0; thread < 8; thread++)
		{
			threads.emplace_back([&, thread]()
			{
				uint32_t state = 0x9E3779B9u * (thread + 1);
				std::vector<uint8_t> range;
				for (int read = 0; read < 1000; read++)
				{
					state = state * 1664525u + 1013904223u;
					const size_t index = state % sources.size();
					const std::vector<uint8_t>& source = sources[index].Data;
					state = state * 1664525u + 1013904223u;
					const uint64_t offset = state % (source.size() + 1);
					state = state * 1664525u + 1013904223u;
					const uint64_t length = std::min<uint64_t>(state % (3 * AssetPack::c_BlockSize), source.size() - offset);
					std::string readError;
					const bool whole = read % 50 == 0;
					const bool success = whole ? pack->ReadData(pack->GetEntries()[index], range, &readError)
						: pack->ReadRange(pack->GetEntries()[index], offset, length, range, &readError);
					// Each thread verifies what it read by its checksum.
					const uint32_t expected = whole ? expectedChecksums[index]
						: Hash::CRC32C(std::span<const uint8_t>(source.data() + offset, static_cast<size_t>(length)));
					if (!success || Hash::CRC32C(range) != expected)
						failures++;
					reads++;
				}
			});
		}
		for (std::thread& thread : threads)
			thread.join();
		CHECK(failures == 0);
		CHECK(reads == 8000);
		CHECK(FileReader::GetOpenCount() - opensBefore == 1); // One handle, opened by Open, for every read
	}

	TEST_CASE("Reads in jobs return their own bytes when a waiting read runs another one")
	{
		// Two workers and more jobs than they run at once, each reading an entry that takes two file reads: a read waiting
		// for the decoding of its first part helps run queued jobs, so reads of other entries run nested on its thread, and
		// must not share its buffers.
		ScopedJobSystem jobs(2, 2);
		std::vector<PackSource> sources;
		for (uint32_t index = 0; index < 6; index++)
			sources.push_back(MakeSource(0x5000 + index, MakeContent((9u << 20) + index * 300'000, ContentKind::Incompressible, 40 + index)));
		AssetPackWriteOptions options;
		options.CompressionLevel = 1; // Noise is stored as it is at any level; the lowest keeps a Debug run short
		const std::filesystem::path path = WritePack(CreateTemporaryDirectory("PackNestedReads") / "Game.stpak", sources, options);
		std::string error;
		Scope<AssetPack> pack = AssetPack::Open(path, &error);
		REQUIRE_MESSAGE(pack, error);
		std::vector<uint8_t> data;
		for (size_t index = 0; index < sources.size(); index++)
		{
			REQUIRE_MESSAGE(pack->ReadData(pack->GetEntries()[index], data, &error), error);
			CHECK(data == sources[index].Data);
		}

		constexpr uint32_t c_Reads = 24;
		std::mutex failureMutex;
		std::vector<std::string> failures;
		std::vector<JobHandle> handles;
		for (uint32_t read = 0; read < c_Reads; read++)
		{
			const size_t index = read % sources.size();
			handles.push_back(JobSystem::Submit([&, index]()
			{
				std::vector<uint8_t> bytes;
				std::string readError;
				const bool success = pack->ReadData(pack->GetEntries()[index], bytes, &readError);
				if (!success || bytes != sources[index].Data)
				{
					std::scoped_lock<std::mutex> lock(failureMutex);
					failures.push_back(success ? fmt::format("entry {} read wrong bytes", index) : readError);
				}
			}));
		}
		JobSystem::WaitAll(handles);
		CHECK_MESSAGE(failures.empty(), failures.size(), " reads failed, the first: ", failures.empty() ? std::string() : failures[0]);
		CHECK(pack->GetStats().Reads == sources.size() + c_Reads);
	}

	TEST_CASE("A version 1 pack written by the retained writer reads like version 2")
	{
		std::vector<PackSource> sources;
		for (uint32_t index = 0; index < 20; index++)
			sources.push_back(MakeSource(0x5000 + index, MakeContent(index * 40'000, static_cast<ContentKind>(index % 3), index)));
		sources[3].Source.Dependencies = { sources[4].Source.Metadata.Handle };
		const std::filesystem::path directory = CreateTemporaryDirectory("PackVersion1");
		AssetPackWriteOptions version1;
		version1.Version = 1;
		const std::filesystem::path oldPath = WritePack(directory / "Old.stpak", sources, version1);
		const std::filesystem::path newPath = WritePack(directory / "New.stpak", sources);

		std::string error;
		Scope<AssetPack> oldPack = AssetPack::Open(oldPath, &error);
		REQUIRE_MESSAGE(oldPack, error);
		Scope<AssetPack> newPack = AssetPack::Open(newPath, &error);
		REQUIRE_MESSAGE(newPack, error);
		CHECK(oldPack->GetVersion() == 1);
		CHECK(oldPack->GetBlocks().empty());
		REQUIRE(oldPack->GetEntries().size() == newPack->GetEntries().size());
		std::vector<uint8_t> oldData;
		std::vector<uint8_t> newData;
		for (size_t index = 0; index < sources.size(); index++)
		{
			CAPTURE(index);
			const AssetPackEntry& oldEntry = oldPack->GetEntries()[index];
			const AssetPackEntry& newEntry = newPack->GetEntries()[index];
			CHECK(oldEntry.Metadata.Handle == newEntry.Metadata.Handle);
			CHECK(oldEntry.Metadata.Path == newEntry.Metadata.Path);
			CHECK(oldEntry.Metadata.StoredSize == newEntry.Metadata.StoredSize);
			REQUIRE(oldPack->ReadData(oldEntry, oldData, &error));
			REQUIRE(newPack->ReadData(newEntry, newData, &error));
			CHECK(oldData == sources[index].Data);
			CHECK(oldData == newData);
			if (oldData.size() > 10)
			{
				REQUIRE(oldPack->ReadRange(oldEntry, 5, oldData.size() - 10, oldData, &error));
				REQUIRE(newPack->ReadRange(newEntry, 5, newData.size() - 10, newData, &error));
				CHECK(oldData == newData);
			}
		}
		// Version 1 has no dependency table.
		CHECK(oldPack->GetDependencies(oldPack->GetEntries()[3]).empty());
		CHECK(newPack->GetDependencies(newPack->GetEntries()[3]).size() == 1);

		AssetPackWriteOptions unknown;
		unknown.Version = 3;
		CHECK_FALSE(AssetPackWriter::Write(directory / "Future.stpak", std::span<const AssetPackSource>(), {}, unknown, nullptr, &error));
		CHECK(error.find("version 3") != std::string::npos);
	}

	TEST_CASE("Writing reports progress and stops when cancelled")
	{
		ScopedJobSystem jobs(4, 2);
		std::vector<PackSource> sources;
		for (uint32_t index = 0; index < 8; index++)
			sources.push_back(MakeSource(0x6000 + index, MakeContent(300'000 + index * 1000, ContentKind::Compressible, index)));
		const std::filesystem::path directory = CreateTemporaryDirectory("PackProgress");

		std::vector<std::pair<uint64_t, uint64_t>> reports;
		AssetPackWriteOptions options;
		options.Progress = [&reports](uint64_t done, uint64_t total) { reports.emplace_back(done, total); };
		WritePack(directory / "Game.stpak", sources, options);
		REQUIRE(reports.size() >= sources.size());
		for (size_t index = 1; index < reports.size(); index++)
			CHECK(reports[index].first >= reports[index - 1].first);
		CHECK(reports.back().first == reports.back().second);

		std::atomic<bool> cancel = false;
		AssetPackWriteOptions cancelling;
		cancelling.Cancel = &cancel;
		cancelling.Progress = [&cancel](uint64_t done, uint64_t total) { cancel = done * 2 >= total; };
		std::vector<AssetPackSource> assets;
		for (const PackSource& source : sources)
			assets.push_back(source.Source);
		std::string error;
		uint32_t provided = 0;
		CHECK_FALSE(AssetPackWriter::Write(directory / "Cancelled.stpak", assets, [&](const AssetMetadata&, std::vector<uint8_t>& outData, std::string*)
		{
			outData = sources[provided++].Data;
			return true;
		}, cancelling, nullptr, &error));
		CHECK(error == "Cancelled");
		CHECK(provided < sources.size());
		CHECK_FALSE(FileSystem::Exists(directory / "Cancelled.stpak"));
	}

	TEST_CASE("Truncated and corrupt packs fail to open or read with an error, never a crash")
	{
		std::vector<PackSource> sources;
		sources.push_back(MakeSource(0x7000, MakeContent(3 * AssetPack::c_BlockSize + 100, ContentKind::Compressible, 1)));
		sources.push_back(MakeSource(0x7001, MakeContent(10'000, ContentKind::Incompressible, 2)));
		sources.push_back(MakeSource(0x7002, MakeContent(5'000, ContentKind::Compressible, 3), AssetType::Material));
		sources[0].Source.Dependencies = { UUID(0x7001), UUID(0x7002) };
		sources[1].Source.Bounds = AABB(glm::vec3(0.0f), glm::vec3(1.0f));
		const std::filesystem::path directory = CreateTemporaryDirectory("PackCorrupt");
		const std::filesystem::path path = WritePack(directory / "Game.stpak", sources);
		const std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(path);
		REQUIRE(bytes);
		const PackImage image = PackImage::Read(path);
		REQUIRE(image.Write() == *bytes); // The image writes the pack back as it was
		const std::filesystem::path corrupt = directory / "Corrupt.stpak";

		std::string error;
		CHECK_FALSE(AssetPack::Open(directory / "Missing.stpak", &error));
		CHECK_FALSE(error.empty());

		// Truncated at every 4 KiB boundary, and at every 8 bytes of the header and the tables.
		std::vector<size_t> lengths;
		for (size_t length = 0; length < bytes->size(); length += 4096)
			lengths.push_back(length);
		for (size_t length = 0; length < sizeof(Header); length += 8)
			lengths.push_back(length);
		for (size_t length = image.Data.size(); length < bytes->size(); length += 8)
			lengths.push_back(length);
		for (size_t length : lengths)
		{
			CAPTURE(length);
			CheckRejected(corrupt, std::vector<uint8_t>(bytes->begin(), bytes->begin() + static_cast<std::ptrdiff_t>(length)));
		}
		std::vector<uint8_t> extended = *bytes;
		extended.push_back(0);
		CheckRejected(corrupt, extended, "do not end the file");

		// The header: magic, version, flags, checksum.
		std::vector<uint8_t> wrongMagic = *bytes;
		wrongMagic[0] = 'X';
		CheckRejected(corrupt, wrongMagic, "not a Strata asset pack");
		std::vector<uint8_t> futureVersion = *bytes;
		futureVersion[4] = 3;
		CheckRejected(corrupt, futureVersion, "unsupported pack version 3");
		std::vector<uint8_t> flipped = *bytes;
		flipped[offsetof(Header, EntryCount)] ^= 0x01;
		CheckRejected(corrupt, flipped, "checksum");
		std::vector<uint8_t> flags = *bytes;
		flags[offsetof(Header, Flags)] = 1;
		PackImage::Reseal(flags);
		CheckRejected(corrupt, flags, "features this engine does not know");
		std::vector<uint8_t> blockSize = *bytes;
		const uint32_t oddBlockSize = 100'000;
		std::memcpy(blockSize.data() + offsetof(Header, BlockSize), &oddBlockSize, sizeof(oddBlockSize));
		PackImage::Reseal(blockSize);
		CheckRejected(corrupt, blockSize, "corrupt pack header");
		// Tables larger than the file, also by more than 4 GiB.
		for (const uint64_t stringBytes : { image.PackHeader.StringBytes + 8, uint64_t(5) << 30, UINT64_MAX - 16 })
		{
			std::vector<uint8_t> huge = *bytes;
			std::memcpy(huge.data() + offsetof(Header, StringBytes), &stringBytes, sizeof(stringBytes));
			PackImage::Reseal(huge);
			CheckRejected(corrupt, huge, "corrupt pack header");
		}
		std::vector<uint8_t> manyBlocks = *bytes;
		const uint32_t blockCount = UINT32_MAX;
		std::memcpy(manyBlocks.data() + offsetof(Header, BlockCount), &blockCount, sizeof(blockCount));
		PackImage::Reseal(manyBlocks);
		CheckRejected(corrupt, manyBlocks, "corrupt pack header");
		// A changed table byte breaks the tables' checksum.
		std::vector<uint8_t> table = *bytes;
		table[image.Data.size() + 3] ^= 0x40;
		CheckRejected(corrupt, table, "checksum");

		// Consistent checksums over inconsistent tables.
		auto rejected = [&](const std::function<void(PackImage&)>& change, std::string_view expected)
		{
			PackImage changed = image;
			change(changed);
			CheckRejected(corrupt, changed.Write(), expected);
		};
		rejected([](PackImage& pack) { pack.Blocks[0].Offset = pack.Data.size(); }, "outside the data section");
		rejected([](PackImage& pack) { pack.Blocks[0].Offset = 8; }, "outside the data section");
		rejected([](PackImage& pack) { pack.Blocks.back().StoredSize = static_cast<uint32_t>(pack.Data.size()); }, "outside the data section");
		rejected([](PackImage& pack) { pack.Blocks[4].Offset = pack.Blocks[0].Offset; }, "overlap");
		rejected([](PackImage& pack) { pack.Blocks[1].Offset += 1; }, "overlap");
		rejected([](PackImage& pack) { pack.Blocks[0].Size = AssetPack::c_BlockSize + 1; }, "invalid size");
		rejected([](PackImage& pack) { pack.Blocks[0].Size = 0; }, "invalid size");
		rejected([](PackImage& pack) { pack.Blocks[0].Codec = 7; }, "unknown codec");
		rejected([](PackImage& pack) { pack.Blocks[4].Codec = static_cast<uint32_t>(AssetPackCodec::Zstd); }, "invalid size"); // Stored, not compressed
		rejected([](PackImage& pack) { pack.Blocks[0].Codec = static_cast<uint32_t>(AssetPackCodec::Stored); }, "invalid size");
		rejected([](PackImage& pack) { pack.Entries[0].Size = uint64_t(5) << 30; }, "blocks for");
		rejected([](PackImage& pack) { pack.Entries[0].Size += 1; }, "wrong size");
		rejected([](PackImage& pack) { pack.Entries[0].BlockCount = 3; }, "blocks for");
		rejected([](PackImage& pack) { pack.Entries[0].FirstBlock = 3; }, "names blocks that do not exist");
		rejected([](PackImage& pack) { pack.Entries[1].FirstBlock = 0; }, "wrong size");
		rejected([](PackImage& pack) { std::swap(pack.Blocks[1], pack.Blocks[2]); }, "not consecutive");
		rejected([](PackImage& pack) { pack.Entries[0].DependencyCount = 3; }, "dependencies that do not exist");
		rejected([](PackImage& pack) { pack.Entries[0].FirstDependency = UINT32_MAX; }, "dependencies that do not exist");
		rejected([](PackImage& pack) { pack.Entries[0].PathString = 99; }, "string that does not exist");
		rejected([](PackImage& pack) { pack.Strings[0].Offset = static_cast<uint32_t>(pack.StringBytes.size()); pack.Strings[0].Size = 1; }, "outside the string table");
		rejected([](PackImage& pack) { pack.Entries[1].Handle = pack.Entries[0].Handle; }, "invalid entry 1");
		rejected([](PackImage& pack) { pack.Entries[1].Handle = 0; }, "invalid entry 1");
		rejected([](PackImage& pack) { pack.Entries[1].Type = 999; }, "invalid entry 1");
		rejected([](PackImage& pack) { pack.Entries[1].Flags = 0x80; }, "unknown flags");
		rejected([](PackImage& pack) { pack.Entries[1].BoundsMin[0] = 2.0f; }, "invalid bounds");
		rejected([](PackImage& pack) { pack.Entries[1].BoundsMax[2] = std::numeric_limits<float>::infinity(); }, "invalid bounds");

		// Block data: a changed stored byte fails the block's checksum, and a block with a matching checksum over bytes zstd
		// cannot decode fails the decompression; reads of other blocks and entries still work.
		auto readFails = [&](const std::function<void(PackImage&)>& change, std::string_view expected)
		{
			PackImage changed = image;
			change(changed);
			REQUIRE(FileSystem::WriteBytes(corrupt, changed.Write()));
			Scope<AssetPack> pack = AssetPack::Open(corrupt, &error);
			REQUIRE_MESSAGE(pack, error);
			std::vector<uint8_t> data;
			error.clear();
			CHECK_FALSE(pack->ReadData(pack->GetEntries()[0], data, &error));
			INFO(error);
			CHECK(error.find(expected) != std::string::npos);
			CHECK(pack->ReadRange(pack->GetEntries()[0], 2 * AssetPack::c_BlockSize, 50, data, &error)); // Another block
			CHECK(pack->ReadData(pack->GetEntries()[1], data, &error));
			CHECK(data == sources[1].Data);
		};
		readFails([](PackImage& pack) { pack.Data[pack.Blocks[0].Offset + 10] ^= 0x01; }, "checksum mismatch");
		readFails([](PackImage& pack)
		{
			uint8_t* stored = pack.Data.data() + pack.Blocks[0].Offset;
			for (uint32_t index = 0; index < pack.Blocks[0].StoredSize; index++)
				stored[index] = static_cast<uint8_t>(index * 7);
			pack.Blocks[0].Checksum = Hash::CRC32C(std::span<const uint8_t>(stored, pack.Blocks[0].StoredSize));
		}, "is corrupt");

		// Any single changed byte: an error or the right answer, never a crash.
		std::vector<PackSource> small;
		small.push_back(MakeSource(0x7100, MakeContent(3000, ContentKind::Compressible, 4), AssetType::Material));
		small.push_back(MakeSource(0x7101, MakeContent(700, ContentKind::Incompressible, 5)));
		small[0].Source.Dependencies = { UUID(0x7101) };
		const std::filesystem::path smallPath = WritePack(directory / "Small.stpak", small);
		const std::optional<std::vector<uint8_t>> smallBytes = FileSystem::ReadBytes(smallPath);
		REQUIRE(smallBytes);
		for (size_t offset = 0; offset < smallBytes->size(); offset += offset < sizeof(Header) || offset >= smallBytes->size() - 400 ? 1 : 97)
		{
			CAPTURE(offset);
			std::vector<uint8_t> changed = *smallBytes;
			changed[offset] ^= 0xA5;
			REQUIRE(FileSystem::WriteBytes(corrupt, changed));
			OpenAndReadEverything(corrupt);
		}
	}

	TEST_CASE("Version 1 packs that are corrupt are still refused")
	{
		std::vector<PackSource> sources;
		sources.push_back(MakeSource(0x7200, MakeContent(2000, ContentKind::Compressible, 6), AssetType::Material));
		const std::filesystem::path directory = CreateTemporaryDirectory("PackCorruptV1");
		AssetPackWriteOptions version1;
		version1.Version = 1;
		const std::filesystem::path path = WritePack(directory / "Old.stpak", sources, version1);
		const std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(path);
		REQUIRE(bytes);
		const std::filesystem::path corrupt = directory / "Corrupt.stpak";
		for (size_t length = 0; length < bytes->size(); length += 3)
		{
			CAPTURE(length);
			CheckRejected(corrupt, std::vector<uint8_t>(bytes->begin(), bytes->begin() + static_cast<std::ptrdiff_t>(length)));
		}
		std::vector<uint8_t> hugeEntryCount = *bytes;
		hugeEntryCount[8] = 0xFF;
		hugeEntryCount[9] = 0xFF;
		CheckRejected(corrupt, hugeEntryCount);
		for (size_t offset = 0; offset < bytes->size(); offset++)
		{
			std::vector<uint8_t> changed = *bytes;
			changed[offset] ^= 0xA5;
			REQUIRE(FileSystem::WriteBytes(corrupt, changed));
			OpenAndReadEverything(corrupt);
		}
	}
}
