#pragma once

#include <chrono>

namespace Strata
{

	// Monotonic stopwatch.
	class Timer
	{
	public:
		Timer()
		{
			Reset();
		}

		void Reset()
		{
			m_Start = std::chrono::steady_clock::now();
		}

		// Elapsed time in seconds.
		float Elapsed() const
		{
			return std::chrono::duration<float>(std::chrono::steady_clock::now() - m_Start).count();
		}

		float ElapsedMillis() const
		{
			return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - m_Start).count();
		}
	private:
		std::chrono::steady_clock::time_point m_Start;
	};

	class Time
	{
	public:
		// Seconds elapsed since the process started (monotonic, double precision).
		static double GetTime()
		{
			static const std::chrono::steady_clock::time_point s_Start = std::chrono::steady_clock::now();
			return std::chrono::duration<double>(std::chrono::steady_clock::now() - s_Start).count();
		}
	};

}
