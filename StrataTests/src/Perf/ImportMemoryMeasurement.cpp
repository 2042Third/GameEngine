#include "Perf/ImportMemoryMeasurement.h"

#include "TestHelpers.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Process.h>

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <charconv>
#include <chrono>
#include <cstdio>
#include <thread>

namespace Strata::Tests
{

	namespace
	{

		constexpr double c_Megabyte = 1024.0 * 1024.0;
		constexpr std::chrono::minutes c_Timeout { 30 };

		nlohmann::json ToJson(const ImportMemoryMeasurement& measurement)
		{
			return { { "PrivateMBBefore", measurement.PrivateMBBefore }, { "PeakPrivateIncreaseMB", measurement.PeakPrivateIncreaseMB },
				{ "Seconds", measurement.Seconds }, { "Imported", measurement.Imported } };
		}

		std::optional<ImportMemoryMeasurement> FromJson(const nlohmann::json& json)
		{
			ImportMemoryMeasurement measurement;
			const auto read = [&json](const char* key, auto& value)
			{
				const nlohmann::json* entry = json.is_object() ? JsonUtils::Find(json, key) : nullptr;
				if (!entry || !entry->is_number())
					return false;
				value = entry->get<std::remove_reference_t<decltype(value)>>();
				return true;
			};
			if (!read("PrivateMBBefore", measurement.PrivateMBBefore) || !read("PeakPrivateIncreaseMB", measurement.PeakPrivateIncreaseMB)
				|| !read("Seconds", measurement.Seconds) || !read("Imported", measurement.Imported))
				return std::nullopt;
			return measurement;
		}

	}

	std::optional<ImportMemoryMeasurement> MeasureFolderImport(const std::filesystem::path& projectDirectory, const std::filesystem::path& folder,
		uint64_t budgetBytes, std::string& error)
	{
		EditorAssetManagerSpecification specification;
		specification.AssetDirectory = projectDirectory / "Assets";
		specification.CacheDirectory = projectDirectory / ".strata" / "Cache";
		specification.WatchFiles = false;
		if (!FileSystem::CreateDirectories(specification.AssetDirectory))
		{
			error = "The project's asset directory cannot be made";
			return std::nullopt;
		}
		EditorAssetManager manager(specification);
		manager.Scan();
		manager.SetImportMemoryBudget(budgetBytes);
		const std::optional<ProcessMemoryInfo> before = Platform::GetProcessMemory();
		if (!before)
		{
			error = "The system does not report the process's memory";
			return std::nullopt;
		}

		ImportMemoryMeasurement measurement;
		measurement.PrivateMBBefore = static_cast<double>(before->PrivateBytes) / c_Megabyte;
		const auto start = std::chrono::steady_clock::now();
		ExternalImportRequest request;
		request.Directories = { folder };
		const Ref<ExternalImport> import = manager.ImportExternal(request, &error);
		if (!import)
			return std::nullopt;
		while (!import->IsFinished() || manager.HasPendingImports())
		{
			if (std::chrono::steady_clock::now() - start > c_Timeout)
			{
				error = "The import did not finish in time";
				return std::nullopt;
			}
			manager.Update();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		measurement.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		if (import->HasFailures() || import->GetItems().empty())
		{
			error = "The import failed";
			return std::nullopt;
		}
		for (const AssetHandle handle : import->GetItems().front().Assets)
			measurement.Imported += manager.GetImportInfo(handle).Imported ? 1 : 0;
		const std::optional<ProcessMemoryInfo> after = Platform::GetProcessMemory();
		measurement.PeakPrivateIncreaseMB = after && after->PeakPrivateBytes > before->PrivateBytes
			? static_cast<double>(after->PeakPrivateBytes - before->PrivateBytes) / c_Megabyte : 0.0;
		return measurement;
	}

	std::optional<ImportMemoryMeasurement> MeasureFolderImportInHelper(const std::filesystem::path& projectDirectory, const std::filesystem::path& folder,
		uint64_t budgetBytes, std::string& error)
	{
		const std::filesystem::path resultFile = CreateTemporaryDirectory("ImportMemoryResult") / "Result.json";
		ProcessSpecification specification;
		specification.Executable = GetTestExecutablePath();
		specification.Arguments = { "--strata-test-helper=import-folder", FileSystem::ToUTF8(projectDirectory), FileSystem::ToUTF8(folder),
			std::to_string(budgetBytes >> 20), FileSystem::ToUTF8(resultFile) };
		const Process::RunResult run = Process::Run(specification, c_Timeout + std::chrono::minutes(1));
		if (!run.Started || run.TimedOut || run.ExitCode != 0)
		{
			error = fmt::format("The import-folder helper {} (exit code {}): {}{}", run.TimedOut ? "timed out" : "failed", run.ExitCode, run.Error, run.Output);
			return std::nullopt;
		}
		const std::optional<std::string> text = FileSystem::ReadText(resultFile);
		const std::optional<nlohmann::json> json = text ? JsonUtils::Parse(*text, &error) : std::nullopt;
		std::optional<ImportMemoryMeasurement> measurement = json ? FromJson(*json) : std::nullopt;
		if (!measurement)
			error = "The import-folder helper's result is unreadable: " + error;
		return measurement;
	}

	std::optional<int> RunImportMemoryHelper(std::string_view mode, int argc, char** argv)
	{
		if (mode != "import-folder")
			return std::nullopt;
		uint64_t budgetMB = 0;
		const std::string_view budgetText = argc == 6 ? std::string_view(argv[4]) : std::string_view();
		if (argc != 6 || std::from_chars(budgetText.data(), budgetText.data() + budgetText.size(), budgetMB).ec != std::errc() || budgetMB == 0)
		{
			std::fprintf(stderr, "Usage: --strata-test-helper=import-folder <project directory> <folder> <budget MB> <result file>\n");
			return 2;
		}
		LogSpecification logSpecification;
		logSpecification.Level = LogLevel::Warn;
		Log::Init(logSpecification);
		Platform::DisableExecutionSpeedThrottling();
		JobSystem::Init();
		std::string error;
		const std::optional<ImportMemoryMeasurement> measurement = MeasureFolderImport(FileSystem::FromUTF8(argv[2]), FileSystem::FromUTF8(argv[3]),
			budgetMB << 20, error);
		JobSystem::Shutdown();
		Log::Shutdown();
		if (!measurement)
		{
			std::fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		if (!FileSystem::WriteText(FileSystem::FromUTF8(argv[5]), JsonUtils::Dump(ToJson(*measurement))))
		{
			std::fprintf(stderr, "Could not write '%s'\n", argv[5]);
			return 1;
		}
		return 0;
	}

}
