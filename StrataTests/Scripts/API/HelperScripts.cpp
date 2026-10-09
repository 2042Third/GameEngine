// Scripts of the Scripting.Helpers tests (StrataTests/src/Scripting/ScriptHelperTests.cpp): the SDK's gameplay helpers
// (Random, Timer, KeyRepeat in StrataScript/Gameplay.h).

#include "TestScripts.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string>

using namespace Strata;
using namespace ScriptTests;

// Random and Timer are plain computations: checked in OnCreate.
class HelperChecks : public CheckingScript
{
public:
	void OnCreate() override
	{
		CheckRandom();
		CheckTimer();
	}
private:
	void CheckRandom()
	{
		Random first(42);
		Random second(42);
		Random other(43);
		bool same = true;
		bool different = false;
		for (int index = 0; index < 100; index++)
		{
			const uint32_t value = first.NextUInt();
			same &= value == second.NextUInt();
			different |= value != other.NextUInt();
		}
		Expect(same, "a seed fixes the sequence");
		Expect(different, "other seeds give other sequences");

		// Saved seeds (replays, shared levels) keep their meaning: the sequence of a seed never changes.
		Random pinned(12345);
		Expect(pinned.NextUInt() == 1411482639u && pinned.NextUInt() == 3165192603u && pinned.NextUInt() == 3360792183u, "the sequence of seed 12345");
		first.Seed(42);
		Expect(first.NextUInt() == 3270867926u && first.NextUInt() == 1795671209u, "Seed restarts the sequence of seed 42");
		Random defaulted;
		Random zero(0);
		Expect(defaulted.NextUInt() == zero.NextUInt(), "the default seed is 0");

		Random dice(7);
		std::array<int32_t, 6> counts = {};
		bool inRange = true;
		for (int index = 0; index < 6000; index++)
		{
			const int32_t value = dice.Range(1, 6);
			if (value < 1 || value > 6)
				inRange = false;
			else
				counts[static_cast<size_t>(value - 1)]++;
		}
		bool uniform = true;
		for (const int32_t count : counts)
			uniform &= count > 850 && count < 1150;
		Expect(inRange, "Range(int) includes both bounds and nothing else");
		Expect(uniform, "Range(int) gives every value as often");
		Expect(dice.Range(5, 5) == 5, "a range of one value");
		const int32_t swapped = dice.Range(6, 1);
		Expect(swapped >= 1 && swapped <= 6, "the bounds may come in either order");
		bool negative = false;
		bool positive = false;
		for (int index = 0; index < 64; index++)
		{
			const int32_t value = dice.Range(std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max());
			negative |= value < 0;
			positive |= value > 0;
		}
		Expect(negative && positive, "the full integer range");

		float lowest = 1.0f;
		float highest = 0.0f;
		bool floatsInRange = true;
		for (int index = 0; index < 10000; index++)
		{
			const float value = dice.NextFloat();
			lowest = value < lowest ? value : lowest;
			highest = value > highest ? value : highest;
			const float ranged = dice.Range(-2.0f, 3.0f);
			floatsInRange &= value >= 0.0f && value < 1.0f && ranged >= -2.0f && ranged < 3.0f;
		}
		Expect(floatsInRange, "NextFloat lies in [0, 1), Range(float) in [min, max)");
		Expect(lowest < 0.01f && highest > 0.99f, "NextFloat covers its range");

		int32_t never = 0;
		int32_t always = 0;
		int32_t quarter = 0;
		for (int index = 0; index < 4000; index++)
		{
			never += dice.Chance(0.0f) ? 1 : 0;
			always += dice.Chance(1.0f) ? 1 : 0;
			quarter += dice.Chance(0.25f) ? 1 : 0;
		}
		Expect(never == 0 && always == 4000 && quarter > 850 && quarter < 1150, "Chance");
	}

	void CheckTimer()
	{
		Timer idle;
		Expect(!idle.IsRunning() && idle.Update(1.0f) == 0, "a timer that never started");

		Timer once(1.0f);
		Expect(once.IsRunning() && once.GetRemaining() == 1.0f && once.GetProgress() == 0.0f, "a started timer");
		Expect(once.Update(0.5f) == 0 && Near(once.GetProgress(), 0.5f), "half way");
		Expect(once.Update(0.75f) == 1 && !once.IsRunning() && once.GetRemaining() == 0.0f && once.GetProgress() == 1.0f, "a one-shot timer elapses once and stops");
		Expect(once.Update(5.0f) == 0, "a stopped timer does not elapse");

		Timer repeating(0.25f, true);
		Expect(repeating.Update(0.375f) == 1 && repeating.IsRunning() && Near(repeating.GetRemaining(), 0.125f), "a repeating timer keeps the overshoot");
		Expect(repeating.Update(1.0f) == 4 && Near(repeating.GetRemaining(), 0.125f), "every period of a long step elapses");
		Expect(repeating.Update(std::numeric_limits<float>::quiet_NaN()) == 0 && repeating.Update(-1.0f) == 0
			&& repeating.Update(std::numeric_limits<float>::infinity()) == 0 && Near(repeating.GetRemaining(), 0.125f), "invalid steps change nothing");
		repeating.Stop();
		Expect(!repeating.IsRunning() && repeating.Update(1.0f) == 0, "Stop");
		repeating.Start(2.0f);
		Expect(repeating.IsRunning() && repeating.GetRemaining() == 2.0f && repeating.Update(2.0f) == 1 && !repeating.IsRunning(), "Start restarts, as a one-shot timer");

		Timer tiny(0.0f, true);
		const int32_t elapsed = tiny.Update(1.0f);
		Expect(elapsed > 900000 && elapsed < 1100000 && tiny.IsRunning(), "huge counts are computed, not looped");
		Timer endless(std::numeric_limits<float>::infinity());
		Expect(endless.Update(1e30f) == 0 && endless.IsRunning() && endless.GetProgress() == 0.0f, "an infinite timer never elapses");
	}
};

ST_SCRIPT_CLASS(HelperChecks)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
}

// Records the frames in which KeyRepeat (Space, a delay of 0.125 s, an interval of 0.0625 s) fires, as "<frame>," text.
class KeyRepeatProbe : public Script
{
public:
	std::string Fires;

	void OnUpdate(float deltaTime) override
	{
		if (m_Repeat.Update(deltaTime))
			Fires += std::to_string(Time::GetFrameIndex()) + ",";
	}
private:
	KeyRepeat m_Repeat { Key::Space, 0.125f, 0.0625f };
};

ST_SCRIPT_CLASS(KeyRepeatProbe)
{
	ST_SCRIPT_FIELD(Fires);
}
