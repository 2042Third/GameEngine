#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// The start of an exported world game measured as a player meets it (the Perf.Runtime.World1M* metrics,
// Editor/WorldGamePerfTests.cpp): GameRuntime opens the game's manifest and its start scene loads; then 60 frames play.
// The game's peak private memory is the kernel's peak of the process, so the measurement runs in a helper process of its
// own, whose memory is the game's alone.
namespace Strata::Tests
{

	struct WorldGameMeasurement
	{
		double StartMs = 0.0;        // From opening the game to its start scene playing (GameRuntime::WaitForStart)
		uint64_t Entities = 0;       // Of the start scene
		double PeakPrivateMB = 0.0;  // The process's peak private memory, 60 frames after the start
		double PrivateAfterMB = 0.0; // The private memory 60 frames after the start
	};

	// Measures the game's start in this process. nullopt with the reason when the game does not open or start.
	std::optional<WorldGameMeasurement> MeasureWorldGameStart(const std::filesystem::path& manifest, std::string& error);
	// MeasureWorldGameStart in a helper process: --strata-test-helper=start-world-game <manifest> <result file>
	std::optional<WorldGameMeasurement> MeasureWorldGameStartInHelper(const std::filesystem::path& manifest, std::string& error);
	// The helper mode above; nullopt for other modes. Exit code 0 with the result file written, 1 when the measurement failed
	// and 2 for wrong arguments.
	std::optional<int> RunWorldGameHelper(std::string_view mode, int argc, char** argv);

}
