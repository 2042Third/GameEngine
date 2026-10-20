#include "stpch.h"
#include "Strata/Scene/SceneWorkBudget.h"

#include "Strata/Scene/Scene.h"

#include <algorithm>

namespace Strata
{

	namespace
	{

		std::chrono::steady_clock::duration ToDuration(double milliseconds)
		{
			return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(std::max(milliseconds, 0.0)));
		}

	}

	std::chrono::microseconds SceneWorkBudget::GetRemainingTime() const
	{
		const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
		return now < Deadline ? std::chrono::duration_cast<std::chrono::microseconds>(Deadline - now) : std::chrono::microseconds(0);
	}

	std::chrono::steady_clock::time_point SceneWorkBudget::GetDeadlineWithin(double milliseconds) const
	{
		return std::min(Deadline, std::chrono::steady_clock::now() + ToDuration(milliseconds));
	}

	SceneWorkBudget SceneWorkBudget::ForFrame(const SceneSettings& settings, double milliseconds)
	{
		SceneWorkBudget budget;
		budget.Deadline = std::chrono::steady_clock::now() + ToDuration(milliseconds);
		budget.EntitiesToCreate = std::clamp<uint32_t>(settings.MaxEntitiesInstantiatedPerFrame, 1, SceneSettings::c_MaxEntitiesInstantiatedLimit);
		budget.EntitiesToDestroy = std::clamp<uint32_t>(settings.MaxEntitiesDestroyedPerFrame, 1, SceneSettings::c_MaxEntitiesDestroyedLimit);
		budget.SubSceneMilliseconds = Scene::c_MaxSubSceneMillisecondsPerFrame;
		return budget;
	}

	size_t SceneWorkBudget::FitChunk(std::chrono::nanoseconds remaining, double nanosecondsPerEntity, size_t smallest, size_t largest)
	{
		smallest = std::min(smallest, largest);
		if (remaining.count() <= 0)
			return smallest;
		const double fitting = static_cast<double>(remaining.count()) / std::max(nanosecondsPerEntity, 1.0);
		if (fitting >= static_cast<double>(largest))
			return largest;
		return std::max(smallest, static_cast<size_t>(fitting));
	}

	void SceneWorkBudget::RecordChunkCost(double& nanosecondsPerEntity, std::chrono::nanoseconds elapsed, size_t entities)
	{
		if (entities == 0)
			return;
		const double measured = static_cast<double>(elapsed.count()) / static_cast<double>(entities);
		nanosecondsPerEntity = std::max(measured, 0.5 * (nanosecondsPerEntity + measured));
	}

}
