#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Perf/WorldFixture.h"
#include "Perf/WorldGameMeasurement.h"
#include "TestHelpers.h"

#include <Strata/Core/Timestep.h>
#include <Strata/Runtime/GameRuntime.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

// Perf tests of exported world games (CTest StrataTests.Perf: Release and Dist only). The realistic 1M world
// (Perf/WorldFixture.h) exported as a game twice, as exports write it (cooked scenes) and with its JSON: its start, measured
// in a helper process (Perf/WorldGameMeasurement.h) for its memory, and quitting while the start scene loads.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr const char* c_World1MStart = "Perf.Runtime.World1MStart";
	constexpr const char* c_World1MTransientMemory = "Perf.Runtime.World1MTransientMemory";
	constexpr const char* c_QuitDuringLoad1M = "Perf.Runtime.QuitDuringLoad1M";

	// The game opens, plays two frames (its start scene still loading) and quits: the milliseconds from the end of its last
	// frame until it is gone.
	double TimeQuitDuringLoad(const std::filesystem::path& manifest)
	{
		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(manifest, &error);
		REQUIRE_MESSAGE(game, error);
		for (int frame = 0; frame < 2; frame++)
			game->Update(Timestep(1.0f / 60.0f));
		CHECK_FALSE(game->GetStartError().has_value());
		const auto start = std::chrono::steady_clock::now();
		game.reset();
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

}

TEST_SUITE("Perf.Runtime")
{
	TEST_CASE("The exported realistic 1M world starts quickly with little transient memory, and quits at once while it loads")
	{
		const Perf::ScopedApplicationJobSystem jobs;
		const std::filesystem::path root = CreateTemporaryDirectory("PerfWorldGame1M");
		std::string error;
		const std::optional<WorldProject> project = WriteWorldProject(root / "World", WorldProjectSpec { { c_World1M } }, error);
		REQUIRE_MESSAGE(project.has_value(), error);
		WorldGameOptions options;
		options.StartScene = project->Scenes.front().Handle;
		const std::optional<WorldGame> cooked = WriteWorldGame(*project, root / "Game", error, options);
		REQUIRE_MESSAGE(cooked.has_value(), error);
		options.CookDocuments = false;
		const std::optional<WorldGame> json = WriteWorldGame(*project, root / "JsonGame", error, options);
		REQUIRE_MESSAGE(json.has_value(), error);

		// The start is one run's time; the transient memory is the load's peak (a maximum by nature) over what the game holds
		// once it plays.
		Perf::CheckBudgetsConfirmed({ c_World1MStart, c_World1MTransientMemory }, [&]()
		{
			std::string measureError;
			const std::optional<WorldGameMeasurement> start = MeasureWorldGameStartInHelper(cooked->Manifest, measureError);
			REQUIRE_MESSAGE(start.has_value(), measureError);
			CHECK(start->Entities == GetWorldEntityCount(c_World1M));
			MESSAGE(fmt::format("The cooked 1M world started in {:.0f} ms, peak {:.0f} MB, {:.0f} MB 60 frames later", start->StartMs, start->PeakPrivateMB,
				start->PrivateAfterMB));
			return Perf::MetricValues { { c_World1MStart, start->StartMs }, { c_World1MTransientMemory, start->PeakPrivateMB - start->PrivateAfterMB } };
		});
		std::string jsonError;
		if (const std::optional<WorldGameMeasurement> jsonStart = MeasureWorldGameStartInHelper(json->Manifest, jsonError))
			MESSAGE(fmt::format("With its JSON the 1M world started in {:.0f} ms, peak {:.0f} MB", jsonStart->StartMs, jsonStart->PeakPrivateMB));

		// The larger of the two games' quits: neither waits for its read.
		Perf::CheckBudgetConfirmed(c_QuitDuringLoad1M, [&]()
		{
			const double cookedQuit = TimeQuitDuringLoad(cooked->Manifest);
			const double jsonQuit = TimeQuitDuringLoad(json->Manifest);
			MESSAGE(fmt::format("Quitting while the 1M world loads: {:.1f} ms cooked, {:.1f} ms JSON", cookedQuit, jsonQuit));
			return std::max(cookedQuit, jsonQuit);
		});
	}
}
