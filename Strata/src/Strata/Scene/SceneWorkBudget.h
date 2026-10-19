#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace Strata
{

	struct SceneSettings;

	// The main-thread work one frame may spend loading and unloading scenes: creating and destroying the entities of
	// sub-scenes (Scene::UpdateSubScenes), preparing loaded scenes for their first frame (SceneLoadOptions::Prepare and the
	// scene systems' preparation hooks) and streaming a world's cells. Whoever owns the scenes (GameRuntime, the editor) makes
	// one per frame and hands it to everything that loads, so that their work together stays within one frame's share; each
	// draws from it what it does (its entity counts) and stops at its deadline. Main thread.
	struct SceneWorkBudget
	{
		// The milliseconds of a frame's loading work by default: with a 60 Hz frame's other work, a frame that loads stays
		// within its 16.7 ms.
		static constexpr double c_DefaultLoadWorkMilliseconds = 4.0;

		std::chrono::steady_clock::time_point Deadline;
		uint32_t EntitiesToCreate = 0;
		uint32_t EntitiesToDestroy = 0;
		// The longest an update of a scene may spend creating and destroying sub-scenes' entities within the deadline
		// (Scene::c_MaxSubSceneMillisecondsPerFrame by ForFrame); tests that count slices lift it.
		double SubSceneMilliseconds = 0.0;

		bool HasTime() const { return std::chrono::steady_clock::now() < Deadline; }
		// The time left until the deadline (zero once it passed).
		std::chrono::microseconds GetRemainingTime() const;
		// The deadline of work that may take at most `milliseconds` from now: never later than this budget's.
		std::chrono::steady_clock::time_point GetDeadlineWithin(double milliseconds) const;

		// The budget of a frame that starts now: `milliseconds` of work, and the entities the scene's settings let sub-scenes
		// create and destroy per frame (SceneSettings::MaxEntitiesInstantiatedPerFrame, MaxEntitiesDestroyedPerFrame).
		static SceneWorkBudget ForFrame(const SceneSettings& settings, double milliseconds = c_DefaultLoadWorkMilliseconds);

		// Work done in chunks between looks at the clock (sub-scenes' entities): the entities of the next chunk, those that fit
		// in the `remaining` time at the estimated cost per entity, from `smallest` to `largest` (`smallest` at most
		// `largest`; `smallest` once no time is left).
		static size_t FitChunk(std::chrono::nanoseconds remaining, double nanosecondsPerEntity, size_t smallest, size_t largest);
		// Folds a chunk's cost into an estimate of the cost per entity: a dearer chunk raises it at once (the next chunk must
		// not overrun), a cheaper one lowers it halfway. A chunk of no entities changes nothing.
		static void RecordChunkCost(double& nanosecondsPerEntity, std::chrono::nanoseconds elapsed, size_t entities);
	};

}
