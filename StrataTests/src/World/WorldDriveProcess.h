#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Strata::Tests
{

	struct GeneratedWorld;

	// What a world drive measured (RunWorldDriveProcess): the frames' work (the game's update, loading included) and the
	// process's peak private memory, which a helper process measures alone.
	struct WorldDriveResult
	{
		uint32_t Frames = 0;
		double FrameP95Ms = 0.0;
		double FrameMaxMs = 0.0;
		double PeakPrivateMB = 0.0;
		uint64_t LoadsStarted = 0;
		uint32_t MostCellsLoaded = 0;
		uint32_t MostBodiesWaiting = 0;
		std::string Error; // Empty when the drive ran
	};

	// Writes the world Perf.World.DrivePeakPrivate and DriveFrameP95 drive into `directory` (its World.stgame): `side` x `side`
	// Ground cells of 32 m, each of `entitiesPerCell` entities with `bodiesPerCell` static boxes, generated as they are packed
	// and packed cooked (world-io's binary form), under a persistent scene whose "Driver" is the world's streaming source.
	// The manifest's path (and in `outWorld`, when given, what was written), or nullopt with the reason.
	std::optional<std::filesystem::path> WriteWorldDriveWorld(const std::filesystem::path& directory, uint32_t side, uint32_t entitiesPerCell,
		uint32_t bodiesPerCell, std::string* outError = nullptr, GeneratedWorld* outWorld = nullptr);

	// --strata-test-helper=world-drive <manifest> <frames> <result file>: plays the game headless (GameRuntime) and drives its
	// "Driver" along the world's diagonal at 40 m/s, a frame every 1/60 s as a game paces them, for `frames` frames, then
	// writes the WorldDriveResult as JSON. Returns the helper's exit code (0 once the result is written).
	int RunWorldDriveProcess(int argc, char** argv);
	// Reads a result RunWorldDriveProcess wrote; nullopt with the reason.
	std::optional<WorldDriveResult> ReadWorldDriveResult(const std::filesystem::path& file, std::string* outError = nullptr);

}
