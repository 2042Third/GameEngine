// Time: delta and fixed delta times, elapsed time, frame index and time scale.

#include "FeatureScript.h"

using namespace Strata;
using namespace FeatureTest;

// The runner advances every frame by 1/60 s. The scene stores a 0.02 s step; the project's Scene.Timing sets 60 steps/s,
// which wins. For one frame (SlowFrame) the time scale is 0.5.
class TimeFeatures : public FeatureScript
{
public:
	static constexpr float c_FrameTime = 1.0f / 60.0f;
	static constexpr int32_t c_SlowFrame = 20;

	int32_t LastFrame = -1;
	float Elapsed = 0.0f; // Sum of the delta times of completed frames
	int32_t FixedSteps = 0;

	void OnCreate() override
	{
		Journal(*this, "TimeFeatures", "OnCreate");
		Expect(Time::GetFrameIndex() == 0 && Time::GetElapsedTime() == 0.0, "time starts at zero");
		Expect(Time::GetTimeScale() == 1.0f, "the time scale starts at 1");
		Expect(Near(Time::GetFixedDeltaTime(), 1.0f / 60.0f), "the project's fixed rate (Scene.Timing) overrides the scene's 0.02 s step");
	}

	void OnUpdate(float deltaTime) override
	{
		const int32_t frame = GetFrame();
		Expect(frame == LastFrame + 1, "GetFrameIndex counts frames");
		Expect(Near(static_cast<float>(Time::GetElapsedTime()), Elapsed, 1e-4f), "GetElapsedTime is the sum of the previous delta times");
		Expect(deltaTime == Time::GetDeltaTime(), "OnUpdate receives GetDeltaTime");

		const float scale = frame == c_SlowFrame + 1 ? 0.5f : 1.0f;
		Expect(Time::GetTimeScale() == scale, "GetTimeScale");
		Expect(Near(deltaTime, c_FrameTime * scale), "delta times are scaled by the time scale");

		if (frame == c_SlowFrame)
			Time::SetTimeScale(0.5f);
		else if (frame == c_SlowFrame + 1)
			Time::SetTimeScale(1.0f);

		LastFrame = frame;
		Elapsed += deltaTime;
		if (frame == c_SlowFrame + 3)
			Completed = FixedSteps > 0;
	}

	void OnFixedUpdate(float fixedDeltaTime) override
	{
		Expect(Near(fixedDeltaTime, 1.0f / 60.0f) && fixedDeltaTime == Time::GetFixedDeltaTime(), "OnFixedUpdate receives the project's fixed timestep");
		FixedSteps++;
	}

	void OnLateUpdate(float deltaTime) override
	{
		Expect(deltaTime == Time::GetDeltaTime(), "OnLateUpdate receives the frame's delta time");
	}
};

ST_SCRIPT_CLASS(TimeFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(LastFrame);
	ST_SCRIPT_FIELD(Elapsed);
	ST_SCRIPT_FIELD(FixedSteps);
}
