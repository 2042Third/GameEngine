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

}
