#include <doctest/doctest.h>

#include "Physics/PhysicsScenes.h"
#include "Physics/PhysicsTestUtils.h"
#include "Strata/Physics/PhysicsTypes.h"
#include "Strata/Scene/Scene.h"

#include <algorithm>
#include <map>
#include <string>

using namespace Strata;
using namespace Strata::Tests;

// How the engine's defaults settle the settle scenes (DeveloperGuide "Contact recycling"): deterministic for a build on a
// platform whatever the number of threads, so the bounds are the values measured at the defaults (on Windows, Debug and
// Release alike) times 1.25, rounded up; across compilers the last bits of Strata's own math may differ, which the margin
// absorbs. The bounds were checked against absurd settings: recycling off topples the tower, a 0.7 m recycle distance fails the
// seams and ledges, a sleep threshold of 0 keeps the tower, the pit and the pyramid awake, and contacts of 1 Hz topple the
// tower and the pyramid; the dominoes' held under every setting the sweep tried, so it guards the code rather than the
// tuning, and the platform's fails while kinematic bodies recycle contacts (696 mm).
TEST_SUITE("Physics.Settling")
{
	TEST_CASE("A tower of 20 crates stands and sleeps")
	{
		Scene scene;
		const SettleScene tower = CreateTower(scene, 20);
		const SettlingSummary summary = MeasureSettling(scene, tower, 5.0f);
		MESSAGE("Tower: top drift ", summary.Probes.at("tower.topDriftMm"), " mm, asleep after ", summary.SecondsToAllAsleep, " s");
		CHECK(summary.Probes.at("tower.topDriftMm") <= 10.1);
		CHECK(summary.SecondsToAllAsleep >= 0.0);
		CHECK(summary.SecondsToAllAsleep <= 1.13);
	}

	TEST_CASE("A pit of 500 balls falls asleep")
	{
		Scene scene;
		const SettleScene pit = CreateBallPit(scene, 500);
		const SettlingSummary summary = MeasureSettling(scene, pit, 40.0f);
		MESSAGE("Ball pit: 99% asleep after ", summary.SecondsTo99Asleep, " s, all after ", summary.SecondsToAllAsleep, " s");
		CHECK(summary.Probes.at("pit.ballsInPit") == 500.0);
		CHECK(summary.SecondsTo99Asleep >= 0.0);
		CHECK(summary.SecondsTo99Asleep <= 20.3);
	}

	TEST_CASE("A pyramid of 15 rows stands and sleeps")
	{
		Scene scene;
		const SettleScene pyramid = CreatePyramid(scene, 15);
		const SettlingSummary summary = MeasureSettling(scene, pyramid, 3.0f);
		CHECK(summary.Probes.at("pyramid.topDriftMm") <= 3.7);
		CHECK(summary.SecondsToAllAsleep >= 0.0);
		CHECK(summary.SecondsToAllAsleep <= 1.13);
	}

	TEST_CASE("64 dominoes all fall")
	{
		Scene scene;
		const SettleScene dominoes = CreateDominoRun(scene, 64);
		const SettlingSummary summary = MeasureSettling(scene, dominoes, 15.0f);
		CHECK(summary.Probes.at("dominoes.down") == 64.0);
		CHECK(summary.Probes.at("dominoes.allDownSeconds") >= 0.0);
		CHECK(summary.Probes.at("dominoes.allDownSeconds") <= 13.7);
	}

	TEST_CASE("Recycled contacts add little to bumps at seams and to travel past ledges")
	{
		// Recycled contacts keep their points and normal. Against the same scene with recycling off, the defaults let a body
		// rise at most 5.0 mm higher crossing the seams of tiles (boxes at 2 m/s, over bumps of 44 mm without recycling: the
		// tiles' inner edges) or of a triangle mesh, and travel at most 1.7 cm further past a ledge before it drops (balls at 1
		// m/s); the bounds are those times 1.25.
		const auto play = [](float distance)
		{
			PhysicsSettings settings = PhysicsSystem::GetDefaultSettings();
			settings.ContactRecycleDistance = distance;
			ScopedPhysicsSettings scoped(settings);
			Scene scene;
			const SettleScene probe = CreateSeamProbe(scene);
			return MeasureSettling(scene, probe, 6.0f).Probes;
		};
		const std::map<std::string, double> off = play(0.0f);
		const std::map<std::string, double> defaults = play(c_DefaultContactRecycleDistance);
		REQUIRE(off.size() == defaults.size());
		double bump = 0.0;
		double travel = 0.0;
		for (const auto& [name, value] : defaults)
		{
			if (name.ends_with(".excursionMm"))
				bump = std::max(bump, value - off.at(name));
			else if (name.ends_with(".travel"))
				travel = std::max(travel, value - off.at(name));
		}
		MESSAGE("Recycling adds at most ", bump, " mm at seams and ", travel, " m past ledges");
		CHECK(bump <= 6.3);
		CHECK(travel <= 0.022);
	}

	TEST_CASE("Boxes ride a moving and turning kinematic platform")
	{
		Scene scene;
		const SettleScene platform = CreateKinematicPlatform(scene);
		const SettlingSummary summary = MeasureSettling(scene, platform, 6.0f);
		MESSAGE("Platform: the riders crept ", summary.Probes.at("platform.slipMm"), " mm");
		// Kinematic bodies never recycle contacts: with recycling, the riders crept 696 mm in those four seconds.
		CHECK(summary.Probes.at("platform.slipMm") <= 21.0);
	}
}
