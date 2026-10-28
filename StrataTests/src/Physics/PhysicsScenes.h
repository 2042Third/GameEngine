#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Physics/PhysicsTypes.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{
	class Scene;
}

namespace Strata::Tests
{

	// The settle scenes (DeveloperGuide "Settling and the sweep"): the engine's representative ways of bodies coming to rest,
	// playing in a scene that is not running yet. Each creator fills the scene and returns what the scene drives while it plays
	// (initial velocities, walking characters, a moving platform) and what it probes. Deterministic: the same build plays them
	// the same with any number of threads.
	struct SettleScene
	{
		// Installed while the scene plays: the data of its mesh colliders (null: the default provider).
		Ref<PhysicsMeshProvider> MeshProvider;
		// Before every fixed step, with the step's index from 0.
		std::function<void(Scene& scene, uint32_t step)> BeforeStep;
		// After every fixed step: the scene's probes, by name (see each creator).
		std::function<void(Scene& scene, uint32_t step, std::map<std::string, double>& probes)> AfterStep;
	};

	// A walled pit (inner half width 4.5 m, the Physics Playground's, up to 600 balls; 6 m beyond) of `count` balls of 0.25 m
	// radius (the Playground's prefab scaled as it spawns them) in a lattice 0.6 m apart with fixed offsets, a quarter each of
	// the Playground's four ball materials. Probe "pit.ballsInPit".
	SettleScene CreateBallPit(Scene& scene, uint32_t count);
	// `height` 1 m crates (the Playground's: 20 kg, friction 0.8) placed touching on the ground. Probe "tower.topDriftMm", the
	// top crate's horizontal drift.
	SettleScene CreateTower(Scene& scene, uint32_t height);
	// A pyramid of 1 m boxes, `base` in its lowest row, 5 cm apart in a row and touching the row below. Probe
	// "pyramid.topDriftMm".
	SettleScene CreatePyramid(Scene& scene, uint32_t base);
	// `count` dominoes (the Playground's: 1 m tall, 0.5 m wide, 0.15 m thick, 2 kg) 0.55 m apart along a serpentine, the first
	// pushed at its top at the start. Probes "dominoes.down" (tipped past 45 degrees) and "dominoes.allDownSeconds" (-1 until
	// all are down).
	SettleScene CreateDominoRun(Scene& scene, uint32_t count);
	// Ghost collisions: boxes (friction 0) sliding and balls rolling at 0.25, 0.5, 1, 2, 4 and 8 m/s along a lane of 1 m static
	// box tiles and along a static triangle mesh of a 1 m grid, each top 1 m above the ground; and the same boxes and balls
	// leaving a 1 m high ledge at 0.25, 0.5 and 1 m/s. Probes per lane: "seam.<box|ball>.<tiles|mesh>.<speed>.excursionMm" (the
	// range of the center's height along the lane once the contact settled: bumps at seams) and "...upwardSpeed" (the largest,
	// m/s), and "ledge.<box|ball>.<speed>.travel" (how far past the edge the center travelled before it dropped 5 cm, m).
	SettleScene CreateSeamProbe(Scene& scene);
	// A character walking at 2 m/s into a 1 m crate of 5 kg over tiles for 2 s, and a 0.4 m crate dropped on the capsule of a
	// standing character that walks at 0.5 m/s after a second. Probes "crate.distance" (m the pushed crate travelled in the
	// 2 s), "pusher.distance" (m the pusher walked), "headCrate.onTopStanding" (1 when it rests on the standing
	// character), "headCrate.slip" (m it moved horizontally relative to the character during the walk) and "headCrate.onTop" (1
	// when it is still above the character's shoulders afterwards).
	SettleScene CreateCratePush(Scene& scene);
	// Four boxes resting on a kinematic platform that starts after a second to move at 2 m/s and turn at 30 degrees/s. Probe
	// "platform.slipMm": the most a box moved horizontally in the platform's frame.
	SettleScene CreateKinematicPlatform(Scene& scene);
	// `count` character controllers ("Character 0"...) in a grid walking along +X at 2 m/s from the first step on, over a ground
	// crossed by 0.3 m steps every 4 m (Perf.Physics.Characters1000Step's crowd).
	SettleScene CreateCharacterCrowd(Scene& scene, uint32_t count);
	// Perf::CreateBoxStacks of `count` boxes; with trackFloor a listener of watched entities counts the floor's contacts, the
	// floor watched (as a script's contact callbacks on it do).
	SettleScene CreateFallingBoxes(Scene& scene, uint32_t count, bool trackFloor);

	// The scenes by the sweep's names (GetSettleSceneNames); nullopt for an unknown name.
	std::optional<SettleScene> CreateSettleScene(Scene& scene, std::string_view name);
	std::vector<std::string> GetSettleSceneNames();

	// What MeasureSettling saw.
	struct SettlingSummary
	{
		double Seconds = 0.0;             // Simulated
		uint64_t Steps = 0;
		uint32_t MovingBodies = 0;        // At the end (PhysicsStats::MovingBodyCount)
		// When 99% of the moving bodies and all of them were first asleep, in simulated seconds; -1 if not within the run.
		double SecondsTo99Asleep = -1.0;
		double SecondsToAllAsleep = -1.0;
		// Wall time of the steps and of the library's part over the whole run, and the steps' wall time over the first 10
		// simulated seconds (SettleCpuMs: it rewards sleeping sooner as well as cheaper steps).
		double StepMs = 0.0;
		double LibraryMs = 0.0;
		double SettleCpuMs = 0.0;
		// The busy phase as the perf lab measures it (the medians of steps 5 to 44): Strata's and the library's step, box3d's
		// collide stage, and the recycled share of the awake contacts over those steps.
		double BusyStrataMs = 0.0;
		double BusyLibraryMs = 0.0;
		double BusyCollideMs = 0.0;
		double BusyRecycledShare = 0.0;
		// Recycled contacts over awake contacts, summed over the whole run.
		double MeanRecycledShare = 0.0;
		double MaxSpeedAt5s = 0.0;        // m/s, the fastest body written back in the step at 5 s
		double RestingJitterMm = 0.0;     // The mean of the steps' resting jitter between 5 and 10 s (steps that wrote bodies)
		std::map<std::string, double> Probes;

		nlohmann::json ToJson() const;
	};

	// Plays a scene that is not running yet for `seconds` of fixed steps with box3d's counters read every step, and stops it.
	SettlingSummary MeasureSettling(Scene& scene, const SettleScene& settle, float seconds);

	// The helper process "physics-settle" (TestMain.cpp): <scene> <seconds> <result.json> [--recycle-distance m]
	// [--sleep-threshold m/s] [--contact-hertz hz] [--contact-damping ratio] [--contact-push-speed m/s] [--workers n]
	// [--collision-steps n]. Plays a settle scene with those settings (PhysicsSystem::SetDefaultSettings; the workers through
	// PhysicsTaskSystem::SetWorkerLimit) on a job system as applications run, without a project (the settings are the process
	// defaults), and writes MeasureSettling's summary with the settings used. Returns 0, or 1 with the reason on stderr.
	int RunPhysicsSettleProcess(int argc, char** argv);

}
