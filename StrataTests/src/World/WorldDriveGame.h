#pragma once

#include <cstdint>
#include <filesystem>

namespace Strata::Tests
{

	// Writes the game StrataRuntime.World drives into `directory` (StrataTests --strata-test-helper=write-world-game): a 6 x 6
	// world of 32 m Ground cells (`entitiesPerCell` entities each, `bodiesPerCell` of them static boxes, on a floor; 2,500 and
	// 500 in full size, a quarter for Debug builds) under a Props layer, packed with
	// a persistent scene whose "Driver", the world's streaming source, runs WorldDrive (StrataTests/Scripts/World) to visit
	// every cell; the script module is copied next to the manifest, World.stgame. Returns the helper's exit code: 0 when
	// written, 1 with the reason on stderr otherwise.
	int WriteWorldDriveGame(const std::filesystem::path& directory, uint32_t entitiesPerCell = 2500, uint32_t bodiesPerCell = 500);

}
