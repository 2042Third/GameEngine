// Scripts of the Scripting.Helpers tests (StrataTests/src/Scripting/ScriptHelperTests.cpp): the SDK's gameplay helpers
// (Random, Timer, KeyRepeat and Motion in StrataScript/Gameplay.h).

#include "TestScripts.h"

#include <array>
#include <cmath>
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

		// Seed 6789416 starts with the largest draw, where min + (max - min) * unit rounds up to max.
		Random edge(6789416);
		Expect(edge.NextFloat() == 16777215.0f / 16777216.0f, "seed 6789416 starts with the largest NextFloat");
		edge.Seed(6789416);
		const float belowOneAndAHalf = edge.Range(1.0f, 1.5f);
		edge.Seed(6789416);
		const float belowHundred = edge.Range(100.0f, 100.25f);
		Expect(belowOneAndAHalf < 1.5f && belowOneAndAHalf > 1.4999f && belowHundred < 100.25f && belowHundred > 100.2499f,
			"Range(float) never returns max");
		edge.Seed(6789416);
		const float swappedFloat = edge.Range(1.5f, 1.0f);
		Expect(swappedFloat >= 1.0f && swappedFloat < 1.5f, "float bounds may come in either order");
		Expect(dice.Range(2.0f, 2.0f) == 2.0f, "equal float bounds give that value");
		bool finite = true;
		for (int index = 0; index < 64; index++)
			finite &= std::isfinite(dice.Range(-std::numeric_limits<float>::max(), std::numeric_limits<float>::max()));
		Expect(finite, "Range(float) of the extreme bounds stays finite");

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

		Timer zero(0.0f, true);
		Timer negative(-1.0f);
		Timer notANumber(std::numeric_limits<float>::quiet_NaN(), true);
		Expect(!zero.IsRunning() && zero.Update(1.0f) == 0 && !negative.IsRunning() && negative.Update(1.0f) == 0 && !notANumber.IsRunning()
			&& notANumber.Update(1.0f) == 0, "timers without a positive duration do not run");
		Timer restarted(1.0f, true);
		restarted.Start(0.0f, true);
		Expect(!restarted.IsRunning() && restarted.Update(5.0f) == 0 && restarted.GetRemaining() == 0.0f, "starting without a duration stops a timer");

		Timer tiny(1e-6f, true);
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

// Motion's LookAt and SmoothDamp are plain computations: checked in OnCreate.
class MotionChecks : public CheckingScript
{
public:
	void OnCreate() override
	{
		CheckLookAt();
		CheckSmoothDamp();
	}
private:
	void CheckLookAt()
	{
		const glm::vec3 forward(0.0f, 0.0f, -1.0f);
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		const glm::quat ahead = Motion::LookAt(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(1.0f, 2.0f, -7.0f));
		Expect(Near(ahead, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)), "looking along -Z is no rotation");
		const glm::quat right = Motion::LookAt(glm::vec3(0.0f), glm::vec3(5.0f, 0.0f, 0.0f));
		Expect(Near(right * forward, glm::vec3(1.0f, 0.0f, 0.0f)) && Near(right * up, up), "LookAt turns -Z towards the target and keeps +Y up");
		const glm::vec3 target = glm::normalize(glm::vec3(2.0f, -1.0f, 3.0f));
		const glm::quat slanted = Motion::LookAt(glm::vec3(0.0f), target * 4.0f);
		Expect(Near(slanted * forward, target) && (slanted * up).y > 0.0f && Near(glm::dot(slanted * glm::vec3(1.0f, 0.0f, 0.0f), up), 0.0f),
			"a slanted target: no roll");
		const glm::quat straightUp = Motion::LookAt(glm::vec3(0.0f), glm::vec3(0.0f, 3.0f, 0.0f));
		Expect(Near(straightUp * forward, up) && Near(glm::length(straightUp), 1.0f), "a target along up");
		Expect(Near(Motion::LookAt(glm::vec3(2.0f), glm::vec3(2.0f)), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)), "the same point: no rotation");
		const glm::quat sideways = Motion::LookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
		Expect(Near(sideways * up, glm::vec3(1.0f, 0.0f, 0.0f)), "another up rolls the rotation");
	}

	void CheckSmoothDamp()
	{
		constexpr float c_Step = 1.0f / 60.0f;
		float value = 0.0f;
		float velocity = 0.0f;
		bool rising = true;
		bool below = true;
		for (int frame = 0; frame < 120; frame++)
		{
			const float next = Motion::SmoothDamp(value, 10.0f, velocity, 0.3f, c_Step);
			rising &= next >= value;
			below &= next <= 10.0f;
			value = next;
		}
		Expect(rising && below, "SmoothDamp approaches without overshooting");
		Expect(Near(value, 10.0f, 0.01f), "SmoothDamp arrives in a few smooth times");
		Expect(velocity >= 0.0f && velocity < 0.1f, "the velocity settles");

		float limited = 0.0f;
		float limitedVelocity = 0.0f;
		for (int frame = 0; frame < 60; frame++)
			limited = Motion::SmoothDamp(limited, 100.0f, limitedVelocity, 0.5f, c_Step, 2.0f);
		Expect(limited > 0.5f && limited <= 2.0f + 1e-3f, "maxSpeed limits the speed");

		float still = 3.0f;
		float stillVelocity = 1.0f;
		Expect(Motion::SmoothDamp(still, 5.0f, stillVelocity, 0.2f, 0.0f) == 3.0f && stillVelocity == 1.0f, "a zero delta changes nothing");
		float pushed = 9.9f;
		float pushedVelocity = 50.0f;
		pushed = Motion::SmoothDamp(pushed, 10.0f, pushedVelocity, 0.2f, 0.1f);
		Expect(pushed == 10.0f && pushedVelocity == 0.0f, "a spring pushed past the target stops there");

		glm::vec3 position(0.0f);
		glm::vec3 positionVelocity(0.0f);
		const glm::vec3 goal(3.0f, -4.0f, 12.0f);
		bool closer = true;
		for (int frame = 0; frame < 120; frame++)
		{
			const glm::vec3 next = Motion::SmoothDamp(position, goal, positionVelocity, 0.25f, c_Step);
			closer &= glm::length(goal - next) <= glm::length(goal - position) + 1e-5f;
			position = next;
		}
		Expect(closer && Near(position, goal, 0.02f), "SmoothDamp of a vec3");
		glm::vec2 point(10.0f, 0.0f);
		glm::vec2 pointVelocity(0.0f);
		for (int frame = 0; frame < 120; frame++)
			point = Motion::SmoothDamp(point, glm::vec2(0.0f, 5.0f), pointVelocity, 0.25f, c_Step, 1000.0f);
		Expect(Near(point, glm::vec2(0.0f, 5.0f), 0.02f), "SmoothDamp of a vec2");

		float again = 0.0f;
		float againVelocity = 0.0f;
		float repeat = 0.0f;
		float repeatVelocity = 0.0f;
		for (int frame = 0; frame < 30; frame++)
		{
			again = Motion::SmoothDamp(again, 1.0f, againVelocity, 0.3f, c_Step);
			repeat = Motion::SmoothDamp(repeat, 1.0f, repeatVelocity, 0.3f, c_Step);
		}
		Expect(again == repeat && againVelocity == repeatVelocity, "the same steps give the same values");
	}
};

ST_SCRIPT_CLASS(MotionChecks)
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
