#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Asset/ImportRecordStore.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Process.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	nlohmann::json MakeRecord(uint64_t value)
	{
		return nlohmann::json { { "Format", "ImportRecord" }, { "Value", value } };
	}

	uint64_t GetValue(const ImportRecordStore& store, AssetHandle handle)
	{
		const std::optional<nlohmann::json> record = store.FindRecord(handle);
		return record ? JsonUtils::GetUInt(*record, "Value", 0) : 0;
	}

	// The byte offset at which each line of the file starts (the header's first).
	std::vector<size_t> GetLineStarts(const std::string& text)
	{
		std::vector<size_t> starts { 0 };
		for (size_t index = 0; index + 1 < text.size(); index++)
		{
			if (text[index] == '\n')
				starts.push_back(index + 1);
		}
		return starts;
	}

}

TEST_SUITE("Asset.ImportRecords")
{
	TEST_CASE("Records and paths are appended, survive a reopen, the latest entry wins and a removal drops both")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("ImportRecords");
		{
			ImportRecordStore store(directory);
			const ImportRecordLoadStats stats = store.Load();
			CHECK(stats.Lines == 0);
			store.SetRecord(UUID(1), MakeRecord(1));
			store.SetRecord(UUID(2), MakeRecord(2));
			store.SetPath(UUID(1), "Textures/Brick.png");
			store.SetPath(UUID(2), "Textures/Moss.png");
			store.SetRecord(UUID(1), MakeRecord(11)); // Latest wins
			store.SetPath(UUID(1), "Textures/Brick.png"); // Unchanged: nothing written
			store.SetRecord(UUID(3), MakeRecord(3));
			store.Remove(UUID(3));
			store.Remove(UUID(4)); // Unknown: nothing written
			CHECK(GetValue(store, UUID(1)) == 11);
			CHECK_FALSE(store.FindRecord(UUID(3)).has_value());
			REQUIRE(store.Flush());
			CHECK(store.GetLiveCount() == 4);
			CHECK(store.GetDeadCount() == 3); // The first record of 1, and 3's record and removal
		}
		const std::optional<std::string> text = FileSystem::ReadText(directory / ImportRecordStore::c_FileName);
		REQUIRE(text.has_value());
		CHECK(text->rfind("{\"Format\":\"ImportRecords\",\"Version\":1}\n", 0) == 0);
		CHECK(std::count(text->begin(), text->end(), '\n') == 8); // The header and seven entries

		ImportRecordStore reopened(directory);
		const ImportRecordLoadStats stats = reopened.Load();
		CHECK(stats.Lines == 7);
		CHECK(stats.SkippedLines == 0);
		CHECK(GetValue(reopened, UUID(1)) == 11);
		CHECK(GetValue(reopened, UUID(2)) == 2);
		CHECK_FALSE(reopened.FindRecord(UUID(3)).has_value());
		CHECK(reopened.FindPath(UUID(1)) == std::optional<std::string>("Textures/Brick.png"));
		CHECK(reopened.GetPaths().size() == 2);
		CHECK(reopened.GetRecordedHandles().size() == 2);
	}

	TEST_CASE("A file whose dead entries outnumber the live ones is rewritten from memory once it is large")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("ImportRecordsCompaction");
		const std::string padding(400, 'x');
		{
			ImportRecordStore store(directory);
			store.Load();
			// 10 assets imported again and again: about 2 MB of entries, of which 10 live.
			for (uint64_t round = 1; round <= 500; round++)
			{
				for (uint64_t asset = 1; asset <= 10; asset++)
				{
					nlohmann::json record = MakeRecord(round);
					record["Padding"] = padding;
					store.SetRecord(UUID(asset), record);
				}
			}
			REQUIRE(store.Flush());
			CHECK(store.GetRewriteCount() >= 2); // The first write (no file yet) and at least one compaction
			CHECK(store.GetFileSize() < 2 * ImportRecordStore::c_CompactMinBytes);
			CHECK(store.GetDeadCount() <= store.GetLiveCount() + 5000);
		}
		ImportRecordStore reopened(directory);
		reopened.Load();
		for (uint64_t asset = 1; asset <= 10; asset++)
			CHECK(GetValue(reopened, UUID(asset)) == 500);
		CHECK(reopened.GetFileSize() == FileSystem::GetFileSize(directory / ImportRecordStore::c_FileName).value_or(0));
	}

	TEST_CASE("The file survives a crash: cut at every byte of its last three entries, it opens with every whole entry and none of the torn one")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("ImportRecordsTorn");
		{
			ImportRecordStore store(directory);
			store.Load();
			for (uint64_t asset = 1; asset <= 6; asset++)
				store.SetRecord(UUID(asset), MakeRecord(asset));
			// The last three entries change what the first three had.
			for (uint64_t asset = 1; asset <= 3; asset++)
				store.SetRecord(UUID(asset), MakeRecord(100 + asset));
			REQUIRE(store.Flush());
		}
		const std::filesystem::path file = directory / ImportRecordStore::c_FileName;
		const std::optional<std::string> text = FileSystem::ReadText(file);
		REQUIRE(text.has_value());
		const std::vector<size_t> starts = GetLineStarts(*text);
		REQUIRE(starts.size() == 10); // The header and nine entries
		const size_t firstCut = starts[7];
		for (size_t cut = firstCut; cut <= text->size(); cut++)
		{
			CAPTURE(cut);
			const std::filesystem::path copy = CreateTemporaryDirectory("ImportRecordsCut");
			REQUIRE(FileSystem::WriteText(copy / ImportRecordStore::c_FileName, std::string_view(*text).substr(0, cut)));
			ImportRecordStore store(copy);
			store.Load();
			// Entries 7, 8 and 9 (the new values of assets 1 to 3) count only when their line is whole, its line feed included.
			for (uint64_t asset = 1; asset <= 3; asset++)
			{
				const size_t lineEnd = asset < 3 ? starts[7 + asset] : text->size();
				CHECK(GetValue(store, UUID(asset)) == (cut >= lineEnd ? 100 + asset : asset));
			}
			for (uint64_t asset = 4; asset <= 6; asset++)
				CHECK(GetValue(store, UUID(asset)) == asset);
			// Appends start on a line of their own: what follows the torn end is read back.
			store.SetRecord(UUID(9), MakeRecord(9));
			REQUIRE(store.Flush());
			ImportRecordStore reopened(copy);
			const ImportRecordLoadStats stats = reopened.Load();
			CHECK(stats.SkippedLines == 0);
			CHECK(GetValue(reopened, UUID(9)) == 9);
		}
	}

	TEST_CASE("Entries whose checksum or JSON is bad are skipped, and an unknown header makes the file start anew")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("ImportRecordsCorrupt");
		const std::string good = ImportRecordStore::FormatLine(R"({"Handle":"0000000000000001","Record":{"Value":1}})");
		std::string flipped = ImportRecordStore::FormatLine(R"({"Handle":"0000000000000002","Record":{"Value":2}})");
		flipped[flipped.size() - 3] = '7'; // The checksum no longer holds
		const std::string notJson = ImportRecordStore::FormatLine("{\"Handle\":");
		const std::string noHandle = ImportRecordStore::FormatLine(R"({"Record":{"Value":3}})");
		REQUIRE(FileSystem::WriteText(directory / ImportRecordStore::c_FileName,
			"{\"Format\":\"ImportRecords\",\"Version\":1}\n" + good + "\n" + flipped + "\nnot a line\n" + notJson + "\n" + noHandle + "\n"));
		{
			ImportRecordStore store(directory);
			const ImportRecordLoadStats stats = store.Load();
			CHECK(stats.Lines == 5);
			CHECK(stats.SkippedLines == 4);
			CHECK_FALSE(stats.Discarded);
			CHECK(GetValue(store, UUID(1)) == 1);
			CHECK_FALSE(store.FindRecord(UUID(2)).has_value());
		}

		REQUIRE(FileSystem::WriteText(directory / ImportRecordStore::c_FileName, "{\"Format\":\"ImportRecords\",\"Version\":99}\n" + good + "\n"));
		{
			ImportRecordStore store(directory);
			const ImportRecordLoadStats stats = store.Load();
			CHECK(stats.Discarded);
			CHECK_FALSE(store.FindRecord(UUID(1)).has_value());
			REQUIRE(store.Flush());
		}
		const std::optional<std::string> text = FileSystem::ReadText(directory / ImportRecordStore::c_FileName);
		REQUIRE(text.has_value());
		CHECK(*text == "{\"Format\":\"ImportRecords\",\"Version\":1}\n");
	}

	TEST_CASE("The earlier layout is taken over once: record files and the asset index, removed after the store wrote them")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("ImportRecordsMigration");
		REQUIRE(FileSystem::WriteText(directory / "0000000000000001.import", JsonUtils::Dump(MakeRecord(1), 1, '\t')));
		REQUIRE(FileSystem::WriteText(directory / "0000000000000002.import", JsonUtils::Dump(MakeRecord(2), 1, '\t')));
		REQUIRE(FileSystem::WriteText(directory / "0000000000000003.import", "not json"));
		REQUIRE(FileSystem::WriteText(directory / "AssetIndex.json",
			R"({"Format":"AssetIndex","Version":1,"Assets":{"0000000000000001":"Textures/Brick.png","0000000000000002":"Textures/Moss.png"}})"));
		REQUIRE(FileSystem::WriteBytes(directory / "0000000000000001.bin", std::vector<uint8_t>(16, 1)));
		{
			ImportRecordStore store(directory);
			const ImportRecordLoadStats stats = store.Load();
			CHECK(stats.MigratedRecords == 2);
			CHECK(stats.MigratedPaths == 2);
			CHECK(GetValue(store, UUID(1)) == 1);
			CHECK(GetValue(store, UUID(2)) == 2);
			CHECK_FALSE(store.FindRecord(UUID(3)).has_value());
			CHECK(store.FindPath(UUID(2)) == std::optional<std::string>("Textures/Moss.png"));
			REQUIRE(store.Flush());
		}
		// Taken over and gone (an unreadable one too: its asset is imported again); cache files stay.
		CHECK_FALSE(FileSystem::Exists(directory / "0000000000000001.import"));
		CHECK_FALSE(FileSystem::Exists(directory / "0000000000000002.import"));
		CHECK_FALSE(FileSystem::Exists(directory / "0000000000000003.import"));
		CHECK_FALSE(FileSystem::Exists(directory / "AssetIndex.json"));
		CHECK(FileSystem::Exists(directory / "0000000000000001.bin"));

		// Once only: a record file that turns up again (an older editor wrote it) loses to the store's record and goes.
		REQUIRE(FileSystem::WriteText(directory / "0000000000000001.import", JsonUtils::Dump(MakeRecord(7))));
		ImportRecordStore reopened(directory);
		const ImportRecordLoadStats stats = reopened.Load();
		CHECK(stats.MigratedRecords == 0);
		CHECK(GetValue(reopened, UUID(1)) == 1);
		REQUIRE(reopened.Flush());
		CHECK_FALSE(FileSystem::Exists(directory / "0000000000000001.import"));
	}

	TEST_CASE("A process killed while it appends leaves a store that opens and whose every record names a cache file that exists")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("ImportRecordsKilled");
		std::mt19937 random(12345);
		size_t records = 0;
		for (int run = 0; run < 5; run++)
		{
			ProcessSpecification specification;
			specification.Executable = GetTestExecutablePath();
			specification.Arguments = { "--strata-test-helper=append-import-records", FileSystem::ToUTF8(directory), std::to_string(run * 1'000'000) };
			Process process;
			REQUIRE(process.Start(specification));
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			std::string output;
			while (output.find("ready") == std::string::npos && std::chrono::steady_clock::now() < deadline && process.IsRunning())
			{
				output += process.TakeOutput();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			REQUIRE_MESSAGE(output.find("ready") != std::string::npos, output);
			// Killed somewhere in its appends.
			std::this_thread::sleep_for(std::chrono::milliseconds(std::uniform_int_distribution<int>(5, 120)(random)));
			REQUIRE(process.Terminate());

			ImportRecordStore store(directory);
			const ImportRecordLoadStats stats = store.Load();
			CHECK_FALSE(stats.Discarded);
			CAPTURE(run);
			const std::vector<AssetHandle> handles = store.GetRecordedHandles();
			CHECK(handles.size() >= records); // What earlier runs wrote stays
			records = handles.size();
			for (const AssetHandle handle : handles)
			{
				const std::optional<nlohmann::json> record = store.FindRecord(handle);
				REQUIRE(record.has_value());
				CHECK(FileSystem::IsRegularFile(directory / FileSystem::FromUTF8(JsonUtils::GetString(*record, "Cache"))));
			}
			REQUIRE(store.Flush());
		}
		CHECK(records > 0);
	}
}
