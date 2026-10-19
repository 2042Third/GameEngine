#include "Perf/WorldGameMeasurement.h"

#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Process.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Runtime/GameRuntime.h>
#include <Strata/Scene/Scene.h>

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <chrono>
#include <cstdio>

namespace Strata::Tests
{

	namespace
	{

		constexpr std::chrono::minutes c_HelperTimeout { 10 };
		constexpr double c_Megabyte = 1024.0 * 1024.0;

		nlohmann::json ToJson(const WorldGameMeasurement& measurement)
		{
			return { { "StartMs", measurement.StartMs }, { "Entities", measurement.Entities }, { "PeakPrivateMB", measurement.PeakPrivateMB },
				{ "PrivateAfterMB", measurement.PrivateAfterMB } };
		}

		std::optional<WorldGameMeasurement> FromJson(const nlohmann::json& json)
		{
			WorldGameMeasurement measurement;
			const auto read = [&json](const char* key, auto& value)
			{
				const nlohmann::json* entry = json.is_object() ? JsonUtils::Find(json, key) : nullptr;
				if (!entry || !entry->is_number())
					return false;
				value = entry->get<std::remove_reference_t<decltype(value)>>();
				return true;
			};
			if (!read("StartMs", measurement.StartMs) || !read("Entities", measurement.Entities) || !read("PeakPrivateMB", measurement.PeakPrivateMB)
				|| !read("PrivateAfterMB", measurement.PrivateAfterMB))
				return std::nullopt;
			return measurement;
		}

	}

	std::optional<WorldGameMeasurement> MeasureWorldGameStart(const std::filesystem::path& manifest, std::string& error)
	{
		WorldGameMeasurement measurement;
		const auto start = std::chrono::steady_clock::now();
		Scope<GameRuntime> game = GameRuntime::Create(manifest, &error);
		if (!game)
			return std::nullopt;
		if (!game->WaitForStart(std::chrono::minutes(5)))
		{
			error = game->GetStartError().value_or("The game did not start within five minutes");
			return std::nullopt;
		}
		measurement.StartMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		measurement.Entities = game->GetScene() ? game->GetScene()->GetEntityCount() : 0;
		for (int frame = 0; frame < 60; frame++)
			game->Update(Timestep(1.0f / 60.0f));
		const std::optional<ProcessMemoryInfo> memory = Platform::GetProcessMemory();
		if (!memory)
		{
			error = "The system does not report the process's memory";
			return std::nullopt;
		}
		measurement.PeakPrivateMB = static_cast<double>(memory->PeakPrivateBytes) / c_Megabyte;
		measurement.PrivateAfterMB = static_cast<double>(memory->PrivateBytes) / c_Megabyte;
		return measurement;
	}

	std::optional<WorldGameMeasurement> MeasureWorldGameStartInHelper(const std::filesystem::path& manifest, std::string& error)
	{
		const std::filesystem::path resultFile = CreateTemporaryDirectory("WorldGameResult") / "Result.json";
		ProcessSpecification specification;
		specification.Executable = GetTestExecutablePath();
		specification.Arguments = { "--strata-test-helper=start-world-game", FileSystem::ToUTF8(manifest), FileSystem::ToUTF8(resultFile) };
		const Process::RunResult run = Process::Run(specification, c_HelperTimeout);
		if (!run.Started || run.TimedOut || run.ExitCode != 0)
		{
			error = fmt::format("The start-world-game helper {} (exit code {}): {}{}", run.TimedOut ? "timed out" : "failed", run.ExitCode, run.Error, run.Output);
			return std::nullopt;
		}
		const std::optional<std::string> text = FileSystem::ReadText(resultFile);
		const std::optional<nlohmann::json> json = text ? JsonUtils::Parse(*text, &error) : std::nullopt;
		std::optional<WorldGameMeasurement> measurement = json ? FromJson(*json) : std::nullopt;
		if (!measurement)
			error = "The start-world-game helper's result is unreadable: " + error;
		return measurement;
	}

	std::optional<int> RunWorldGameHelper(std::string_view mode, int argc, char** argv)
	{
		if (mode != "start-world-game")
			return std::nullopt;
		if (argc != 4)
		{
			std::fprintf(stderr, "Usage: --strata-test-helper=start-world-game <manifest> <result file>\n");
			return 2;
		}
		LogSpecification logSpecification;
		logSpecification.Level = LogLevel::Warn;
		Log::Init(logSpecification);
		// A measurement runs at full speed with the application's pools, like the game it stands for.
		Platform::DisableExecutionSpeedThrottling();
		JobSystem::Init();
		std::string error;
		const std::optional<WorldGameMeasurement> measurement = MeasureWorldGameStart(FileSystem::FromUTF8(argv[2]), error);
		JobSystem::Shutdown();
		Log::Shutdown();
		if (!measurement)
		{
			std::fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		if (!FileSystem::WriteText(FileSystem::FromUTF8(argv[3]), JsonUtils::Dump(ToJson(*measurement))))
		{
			std::fprintf(stderr, "Could not write '%s'\n", argv[3]);
			return 1;
		}
		return 0;
	}

}
