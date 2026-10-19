#pragma once

#include "StrataScript/Host.h"

#include <cstdint>

namespace Strata
{

	// Simulation time of the scene the script runs in.
	class Time
	{
	public:
		// Duration of the current frame in seconds (scaled by the time scale).
		static float GetDeltaTime()
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			return host ? host->GetDeltaTime(Detail::GetContext()) : 0.0f;
		}

		// Duration of one fixed update step in seconds.
		static float GetFixedDeltaTime()
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			return host ? host->GetFixedDeltaTime(Detail::GetContext()) : 0.0f;
		}

		// Seconds of simulation since the scene started playing.
		static double GetElapsedTime()
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			return host ? host->GetElapsedTime(Detail::GetContext()) : 0.0;
		}

		// Number of completed frames since the scene started playing.
		static uint64_t GetFrameIndex()
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			return host ? host->GetFrameIndex(Detail::GetContext()) : 0;
		}

		// Speed of the simulation (1 = real time, 0 = frozen).
		static float GetTimeScale()
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			return host ? host->GetTimeScale(Detail::GetContext()) : 1.0f;
		}

		static void SetTimeScale(float timeScale)
		{
			if (const StrataScriptHostAPI* host = Detail::GetHost())
				host->SetTimeScale(Detail::GetContext(), timeScale);
		}

		// Duration of the current frame in seconds before the time scale: what menus, fades and pause screens animate with,
		// since it goes on at time scale 0.
		static float GetUnscaledDeltaTime()
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(GetUnscaledDeltaTime);
			return host ? host->GetUnscaledDeltaTime(Detail::GetContext()) : 0.0f;
		}

		// Seconds since the scene started playing, unaffected by the time scale (the sum of GetUnscaledDeltaTime).
		static double GetRealTime()
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(GetRealTime);
			return host ? host->GetRealTime(Detail::GetContext()) : 0.0;
		}
	};

}
