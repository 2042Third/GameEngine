#pragma once

#include "StrataScript/Input.h"

#include <cmath>
#include <cstdint>

// Small gameplay helpers that run entirely in the script module: a deterministic random number generator, timers and key
// repeat. They hold plain state, so scripts keep them as members; that state is not a field, so a hot reload starts them
// over (re-create them in OnReload, or keep what matters in fields).

namespace Strata
{

	// A seedable pseudo-random number generator (PCG32, XSH-RR variant). A seed gives the same sequence on every platform,
	// compiler and SDK version, so games can replay a match, share a level seed or test against fixed outcomes. Generators
	// constructed without a seed all produce the same sequence: seed from something that varies (time, a level number) for
	// variety. Not suitable for cryptography.
	class Random
	{
	public:
		explicit Random(uint64_t seed = 0)
		{
			Seed(seed);
		}

		// Restarts the sequence of `seed`.
		void Seed(uint64_t seed)
		{
			m_State = 0;
			NextUInt();
			m_State += seed;
			NextUInt();
		}

		// Uniform over every 32-bit value.
		uint32_t NextUInt()
		{
			const uint64_t state = m_State;
			m_State = state * c_Multiplier + c_Increment;
			const uint32_t xorShifted = static_cast<uint32_t>(((state >> 18u) ^ state) >> 27u);
			const uint32_t rotation = static_cast<uint32_t>(state >> 59u);
			return (xorShifted >> rotation) | (xorShifted << ((32u - rotation) & 31u));
		}

		// Uniform in [min, max], both included (every value equally likely); the bounds may come in either order.
		int32_t Range(int32_t min, int32_t max)
		{
			if (min > max)
			{
				const int32_t swap = min;
				min = max;
				max = swap;
			}
			const uint64_t span = static_cast<uint64_t>(static_cast<int64_t>(max) - static_cast<int64_t>(min)) + 1u;
			if (span > 0xFFFFFFFFull)
				return static_cast<int32_t>(static_cast<int64_t>(min) + static_cast<int64_t>(NextUInt()));
			// Values below the threshold would make the low results more likely than the high ones: draw again.
			const uint32_t bound = static_cast<uint32_t>(span);
			const uint32_t threshold = static_cast<uint32_t>(0x100000000ull % bound);
			for (;;)
			{
				const uint32_t value = NextUInt();
				if (value >= threshold)
					return static_cast<int32_t>(static_cast<int64_t>(min) + static_cast<int64_t>(value % bound));
			}
		}

		// Uniform in [0, 1).
		float NextFloat()
		{
			return static_cast<float>(NextUInt() >> 8u) * (1.0f / 16777216.0f);
		}

		// Uniform in [min, max).
		float Range(float min, float max)
		{
			return min + (max - min) * NextFloat();
		}

		// True with the given probability (0: never, 1: always).
		bool Chance(float probability)
		{
			return NextFloat() < probability;
		}
	private:
		static constexpr uint64_t c_Multiplier = 6364136223846793005ull;
		static constexpr uint64_t c_Increment = 1442695040888963407ull;

		uint64_t m_State = 0;
	};

	// Counts game time down: Update it with the frame's delta time (scaled time, so it stops while the game is paused or
	// slowed down) and it reports when it elapsed. A repeating timer starts over by itself and keeps the overshoot, so its
	// period does not drift with the frame rate.
	class Timer
	{
	public:
		Timer() = default;

		explicit Timer(float duration, bool repeat = false)
		{
			Start(duration, repeat);
		}

		// (Re)starts the timer. Durations are at least a microsecond; an infinite one never elapses.
		void Start(float duration, bool repeat = false)
		{
			m_Duration = duration > c_MinDuration ? duration : c_MinDuration;
			m_Remaining = m_Duration;
			m_Repeat = repeat;
			m_Running = true;
		}

		void Stop()
		{
			m_Running = false;
		}

		// Advances the timer and returns how often it elapsed during this step: 0 or 1, or more for a repeating timer whose
		// period is shorter than the step. A one-shot timer stops when it elapses.
		int32_t Update(float deltaTime)
		{
			if (!m_Running || !(deltaTime > 0.0f) || !std::isfinite(deltaTime))
				return 0;
			m_Remaining -= deltaTime;
			if (m_Remaining > 0.0f)
				return 0;
			if (!m_Repeat)
			{
				m_Remaining = 0.0f;
				m_Running = false;
				return 1;
			}

			// The period that ended, and every further one that fit into the step, elapsed; the next one is partly over.
			const float overshoot = -m_Remaining;
			const float periods = std::floor(overshoot / m_Duration);
			m_Remaining = m_Duration - (overshoot - periods * m_Duration);
			if (!(m_Remaining > 0.0f) || m_Remaining > m_Duration)
				m_Remaining = m_Duration; // Rounding
			const double count = static_cast<double>(periods) + 1.0;
			return count < 2147483647.0 ? static_cast<int32_t>(count) : 2147483647;
		}
		bool IsRunning() const { return m_Running; }
		// Seconds until the timer elapses next (0 once a one-shot timer elapsed).
		float GetRemaining() const { return m_Remaining; }
		// The part of the current period that passed, from 0 to 1.
		float GetProgress() const { return m_Duration > 0.0f && std::isfinite(m_Duration) ? 1.0f - m_Remaining / m_Duration : 0.0f; }
	private:
		static constexpr float c_MinDuration = 1e-6f;

		float m_Duration = 0.0f;
		float m_Remaining = 0.0f;
		bool m_Repeat = false;
		bool m_Running = false;
	};

	// Repeats a held key, the way menus, text fields and falling-block games move: Update is true in the frame the key goes
	// down, again once it has been held for `delay` seconds, then every `interval` seconds while it stays down (at most
	// once per frame). Call Update once per frame, in OnUpdate, with that frame's delta time.
	class KeyRepeat
	{
	public:
		explicit KeyRepeat(KeyCode key, float delay = 0.25f, float interval = 0.05f)
			: m_Key(key), m_Delay(delay > 0.0f ? delay : 0.0f), m_Interval(interval > 0.0f ? interval : 0.0f)
		{
		}

		bool Update(float deltaTime)
		{
			if (!Input::IsKeyDown(m_Key))
			{
				m_Held = false;
				return false;
			}
			if (!m_Held || Input::IsKeyPressed(m_Key))
			{
				m_Held = true;
				m_HeldTime = 0.0f;
				m_NextRepeat = m_Delay;
				return true;
			}
			m_HeldTime += deltaTime > 0.0f ? deltaTime : 0.0f;
			if (m_HeldTime < m_NextRepeat)
				return false;
			m_NextRepeat += m_Interval;
			// A long frame repeats once, then continues from now.
			if (m_NextRepeat < m_HeldTime)
				m_NextRepeat = m_HeldTime + m_Interval;
			return true;
		}
	private:
		KeyCode m_Key;
		float m_Delay;
		float m_Interval;
		float m_HeldTime = 0.0f;
		float m_NextRepeat = 0.0f;
		bool m_Held = false;
	};

}
