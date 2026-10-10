#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

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

	// Keeps a loop at or below a frame rate by sleeping out the rest of each frame's time slot, e.g. for headless
	// applications, which have no vsync to pace them. Frames are scheduled at fixed intervals, so a frame that overslept
	// is followed by a shorter wait. A frame that ends after the next slot has begun lets the next frame start at once,
	// and the schedule continues from there (late frames are not made up with a burst). Precision is that of the
	// operating system's sleep.
	class FramePacer
	{
	public:
		// 0: unlimited (WaitForNextFrame returns at once).
		explicit FramePacer(uint32_t maxFrameRate = 0)
		{
			SetMaxFrameRate(maxFrameRate);
		}

		// Another rate starts a new schedule at the next wait; setting the current rate again changes nothing, so a loop
		// may set its rate every frame.
		void SetMaxFrameRate(uint32_t maxFrameRate)
		{
			if (maxFrameRate == m_MaxFrameRate)
				return;
			m_MaxFrameRate = maxFrameRate;
			m_Period = maxFrameRate > 0 ? std::chrono::nanoseconds(1'000'000'000 / maxFrameRate) : std::chrono::nanoseconds(0);
			m_Scheduled = false;
		}

		uint32_t GetMaxFrameRate() const { return m_MaxFrameRate; }

		// Call once per frame, when the frame's work is done: returns when the next frame may start.
		void WaitForNextFrame()
		{
			if (m_MaxFrameRate == 0)
				return;

			const auto now = std::chrono::steady_clock::now();
			if (m_Scheduled)
			{
				m_NextFrame += m_Period;
				if (m_NextFrame < now)
					m_NextFrame = now;
			}
			else
			{
				m_NextFrame = now + m_Period;
				m_Scheduled = true;
			}
			std::this_thread::sleep_until(m_NextFrame);
		}
	private:
		uint32_t m_MaxFrameRate = 0;
		std::chrono::nanoseconds m_Period { 0 };
		std::chrono::steady_clock::time_point m_NextFrame;
		bool m_Scheduled = false;
	};

}
