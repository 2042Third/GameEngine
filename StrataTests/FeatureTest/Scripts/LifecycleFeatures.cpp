// Lifecycle callbacks: order within a frame, fixed and late updates, hot reload and destruction.

#include "FeatureScript.h"

using namespace Strata;
using namespace FeatureTest;

class LifecycleFeatures : public FeatureScript
{
public:
	int32_t Creates = 0;
	int32_t Updates = 0;
	int32_t FixedUpdates = 0;
	int32_t LateUpdates = 0;
	int32_t Reloads = 0;
	int32_t LastUpdateFrame = -1;
	int32_t LastLateFrame = -1;

	void OnCreate() override
	{
		Journal(*this, "LifecycleFeatures", "OnCreate");
		Creates++;
		Expect(Updates == 0 && FixedUpdates == 0 && LateUpdates == 0, "OnCreate runs before every update");
	}

	void OnUpdate(float) override
	{
		const int32_t frame = GetFrame();
		Expect(frame == LastUpdateFrame + 1, "OnUpdate runs once per frame");
		Expect(LastLateFrame == LastUpdateFrame, "the previous frame ended with OnLateUpdate");
		LastUpdateFrame = frame;
		Updates++;
	}

	void OnFixedUpdate(float fixedDeltaTime) override
	{
		Expect(LastUpdateFrame == GetFrame() && LastLateFrame < LastUpdateFrame, "OnFixedUpdate runs between OnUpdate and OnLateUpdate");
		Expect(Near(fixedDeltaTime, Time::GetFixedDeltaTime()), "the fixed update gets the fixed timestep");
		FixedUpdates++;
	}

	void OnLateUpdate(float) override
	{
		Expect(LastUpdateFrame == GetFrame() && LastLateFrame < LastUpdateFrame, "OnLateUpdate follows OnUpdate");
		LastLateFrame = GetFrame();
		LateUpdates++;
		if (Reloads == 1 && Updates >= 60)
		{
			Expect(Creates == 1, "OnCreate runs once, also across a hot reload");
			Expect(LateUpdates == Updates, "one OnLateUpdate per OnUpdate");
			Expect(FixedUpdates > 0, "fixed updates ran");
			Completed = true;
		}
	}

	void OnReload() override
	{
		Journal(*this, "LifecycleFeatures", "OnReload");
		Reloads++;
		Expect(Creates == 1 && Updates > 0, "fields survive a hot reload");
	}

	void OnDestroy() override
	{
		Journal(*this, "LifecycleFeatures", "OnDestroy");
	}
};

ST_SCRIPT_CLASS(LifecycleFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(Creates);
	ST_SCRIPT_FIELD(Updates);
	ST_SCRIPT_FIELD(FixedUpdates);
	ST_SCRIPT_FIELD(LateUpdates);
	ST_SCRIPT_FIELD(Reloads);
	ST_SCRIPT_FIELD(LastUpdateFrame);
	ST_SCRIPT_FIELD(LastLateFrame);
}
