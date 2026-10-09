// Logging at every level with every loggable value type, and exceptions thrown by scripts.

#include "SharedScripts.h"

#include <stdexcept>
#include <string>

using namespace Strata;
using namespace FeatureTest;

// The runner expects exactly these messages (c_ExpectedLogMessages in StrataTests/src/FeatureTest/FeatureTestUtils.cpp);
// keep both in sync.
class LogFeatures : public FeatureScript
{
public:
	void OnCreate() override
	{
		Journal(*this, "LogFeatures", "OnCreate");
		Log::Trace("FeatureTest trace: ", 1);
		Log::Info("FeatureTest info: ", std::string("text"), ' ', 2.5f, " ", false);
		Log::Warn("FeatureTest expected warning: ", true, ", ", int32_t(7), ", ", 0.5f, ", ", glm::vec2(1.0f, 2.0f), ", ", glm::vec3(1.0f, 2.0f, 3.0f), ", ",
			glm::vec4(1.0f, 2.0f, 3.0f, 4.0f), ", ", glm::quat(1.0f, 0.0f, 0.0f, 0.0f), ", ", Entity(42), ", ", AssetHandle(43));
		Log::Error("FeatureTest expected error: ", std::string("scripts report errors"));

		// The journal is a script-written log too (long enough by now to take the SDK's path for long strings).
		const std::string journal = Scene::FindEntityByName("Journal").GetProperty<std::string>("Text", "Text").value_or(std::string());
		Expect(journal.find("LogFeatures.OnCreate@Log Features;") != std::string::npos, "the journal holds this script's OnCreate");
		Completed = true;
	}
};

ST_SCRIPT_CLASS(LogFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
}

void FeatureTest::ExceptionProbe::OnCreate()
{
	Journal(*this, "ExceptionProbe", "OnCreate");
}

void FeatureTest::ExceptionProbe::OnUpdate(float)
{
	if (!Thrown)
	{
		// The SDK catches this, reports the message to the engine (logged as an error the runner expects) and the engine
		// disables this instance. The hot reload re-enables it with its fields.
		Thrown = true;
		throw std::runtime_error("FeatureTest: deliberate exception");
	}

	Expect(Reloaded, "a script that threw only updates again after a reload");
	UpdatesAfterReload++;
	if (UpdatesAfterReload == 3)
		Completed = true;
}

void FeatureTest::ExceptionProbe::OnReload()
{
	Journal(*this, "ExceptionProbe", "OnReload");
	Expect(Thrown, "fields of a disabled script survive a reload");
	Reloaded = true;
}

ST_SCRIPT_CLASS(ExceptionProbe)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(Thrown);
	ST_SCRIPT_FIELD(Reloaded);
	ST_SCRIPT_FIELD(UpdatesAfterReload);
}

// Next to the ExceptionProbe: a script that threw stays on its entity but is unreachable until it is re-enabled.
class ExceptionObserver : public FeatureScript
{
public:
	bool SawDisabled = false;

	void OnCreate() override
	{
		Journal(*this, "ExceptionObserver", "OnCreate");
	}

	void OnUpdate(float) override
	{
		Entity self = GetEntity();
		Expect(self.HasScript("ExceptionProbe"), "a script that threw stays on its entity");
		ExceptionProbe* probe = self.GetScript<ExceptionProbe>();
		if (!probe)
		{
			SawDisabled = true;
			return;
		}
		if (probe->Reloaded)
		{
			Expect(SawDisabled, "the probe was disabled after it threw");
			Completed = true;
		}
	}
};

ST_SCRIPT_CLASS(ExceptionObserver)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(SawDisabled);
}
