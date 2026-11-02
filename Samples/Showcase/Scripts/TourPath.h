#pragma once

// The camera tour's shots and timeline. No engine calls: Outpost reads where the tour is at a time and moves the camera.
// Each shot dollies through its key along a Catmull-Rom curve over the markers Shot<N>.From, Shot<N>.Key and Shot<N>.To,
// looking at Shot<N>.Look; it fades in from black and out to black over FadeSeconds at both ends, and the tour cuts and
// changes its time of day in the black between two shots.

#include "Stations.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace Showcase
{

	struct TourShot
	{
		int32_t Number;
		float Seconds;
		float Fov; // Vertical field of view (degrees)
		StationId Station;
		const char* Caption;
	};

	constexpr std::array<TourShot, 6> c_Shots = { {
		{ 1, 14.0f, 50.0f, StationId::Dawn, "dawn vista" },
		{ 2, 14.0f, 40.0f, StationId::Golden, "the bus" },
		{ 3, 12.0f, 45.0f, StationId::Golden, "the oil tank" },
		{ 4, 14.0f, 55.0f, StationId::Golden, "the wet apron" },
		{ 5, 10.0f, 70.0f, StationId::Golden, "the hut" },
		{ 6, 14.0f, 50.0f, StationId::Dusk, "dusk" },
	} };

	constexpr int32_t c_ShotCount = static_cast<int32_t>(c_Shots.size());

	inline const TourShot& GetShot(int32_t number)
	{
		return c_Shots[static_cast<size_t>(std::clamp(number, 1, c_ShotCount) - 1)];
	}

	// Seconds of tour time from the tour's start to the start of a shot (1-6).
	inline float GetShotStart(int32_t number)
	{
		float start = 0.0f;
		for (int32_t index = 1; index < std::clamp(number, 1, c_ShotCount); index++)
			start += GetShot(index).Seconds;
		return start;
	}

	inline float GetTourSeconds()
	{
		return GetShotStart(c_ShotCount) + GetShot(c_ShotCount).Seconds;
	}

	inline glm::vec3 CatmullRom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, float t)
	{
		const float t2 = t * t;
		const float t3 = t2 * t;
		return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
	}

	inline float EaseInOut(float t)
	{
		const float x = std::clamp(t, 0.0f, 1.0f);
		return x * x * (3.0f - 2.0f * x);
	}

	// The camera on a shot's dolly at progress t (0-1, eased): From to Key to To, the ends repeated.
	inline glm::vec3 DollyPosition(const glm::vec3& from, const glm::vec3& key, const glm::vec3& to, float t)
	{
		const float u = EaseInOut(t) * 2.0f;
		if (u < 1.0f)
			return CatmullRom(from, from, key, to, u);
		return CatmullRom(from, key, to, to, std::min(u - 1.0f, 1.0f));
	}

	struct TourMoment
	{
		int32_t Shot;   // 1-6
		float Progress; // 0-1 through the shot
		float Fade;     // 0 clear, 1 black
	};

	// Where the tour is after tourTime seconds (wrapped into one tour).
	inline TourMoment GetTourMoment(float tourTime, float fadeSeconds)
	{
		const float total = GetTourSeconds();
		float time = tourTime - total * static_cast<float>(static_cast<int64_t>(std::max(tourTime, 0.0f) / total));
		time = std::clamp(time, 0.0f, total);
		TourMoment moment { c_ShotCount, 1.0f, 1.0f };
		for (const TourShot& shot : c_Shots)
		{
			if (time < shot.Seconds || shot.Number == c_ShotCount)
			{
				const float local = std::min(time, shot.Seconds);
				const float fade = std::max(fadeSeconds, 1e-3f);
				moment.Shot = shot.Number;
				moment.Progress = local / shot.Seconds;
				moment.Fade = std::clamp(std::max(1.0f - local / fade, 1.0f - (shot.Seconds - local) / fade), 0.0f, 1.0f);
				return moment;
			}
			time -= shot.Seconds;
		}
		return moment;
	}

}
