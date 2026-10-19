#include "Asset/ImportRecordTestUtils.h"

#include <Strata/Asset/ImportRecordStore.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/Log.h>

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace Strata::Tests
{

	std::optional<int> RunImportRecordHelper(std::string_view mode, int argc, char** argv)
	{
		if (mode != "append-import-records")
			return std::nullopt;
		uint64_t first = 0;
		const std::string_view firstText = argc == 4 ? std::string_view(argv[3]) : std::string_view();
		if (argc != 4 || std::from_chars(firstText.data(), firstText.data() + firstText.size(), first).ec != std::errc())
			return 2;

		LogSpecification logSpecification;
		logSpecification.Level = LogLevel::Error;
		Log::Init(logSpecification);
		JobSystem::Init();
		const std::filesystem::path directory = FileSystem::FromUTF8(argv[2]);
		FileSystem::CreateDirectories(directory);
		ImportRecordStore store(directory);
		store.Load();
		const std::vector<uint8_t> data(4096, 0x5A);
		for (uint64_t index = first;; index++)
		{
			const AssetHandle handle(0x1000000000000000ull + index);
			const std::string name = handle.ToString() + ".bin";
			const std::filesystem::path staged = directory / FileSystem::FromUTF8(name + ".staged");
			if (!FileSystem::WriteBytes(staged, data) || !FileSystem::Rename(staged, directory / FileSystem::FromUTF8(name)))
				return 1;
			store.SetRecord(handle, nlohmann::json { { "Format", "ImportRecord" }, { "Cache", name }, { "Index", index } });
			if (index == first)
			{
				std::printf("ready\n");
				std::fflush(stdout);
			}
		}
	}

}
