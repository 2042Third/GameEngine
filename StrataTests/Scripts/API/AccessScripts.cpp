// Script-to-script access, input, time and scene query test scripts.

#include "TestScripts.h"

using namespace Strata;
using namespace ScriptTests;

class Listener : public Script
{
public:
	int32_t Received = 0;
	std::string LastMessage;

	void Receive(const std::string& message)
	{
		Received++;
		LastMessage = message;
	}
};

ST_SCRIPT_CLASS(Listener)
{
	ST_SCRIPT_FIELD(Received);
	ST_SCRIPT_FIELD(LastMessage);
}

// Calls the Listener on the entity named "ListenerEntity" every update.
class Talker : public CheckingScript
{
public:
	int32_t Talks = 0;

	void OnUpdate(float) override
	{
		const Entity target = Scene::FindEntityByName("ListenerEntity");
		Listener* listener = target.GetScript<Listener>();
		Expect(listener != nullptr, "GetScript finds another entity's script");
		if (listener)
			listener->Receive("hello " + std::to_string(Talks));
		Talks++;

		Expect(target.GetScript<Talker>() == nullptr, "GetScript of a class the entity does not have");
		Expect(GetEntity().GetScript<Talker>() == this, "GetScript returns the instance itself");
		Expect(target.HasScript("Listener") && !target.HasScript("Talker"), "HasScript");
		Expect(Entity().GetScript<Listener>() == nullptr && Entity(0x4444).GetScript<Listener>() == nullptr, "GetScript on missing entities");
	}
};

ST_SCRIPT_CLASS(Talker)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Talks);
}

// Samples the input state every update.
class InputProbe : public Script
{
public:
	bool WDown = false;
	bool WPressed = false;
	bool WReleased = false;
	bool LeftDown = false;
	bool LeftPressed = false;
	bool LeftReleased = false;
	bool InvalidKeyDown = true;
	bool InvalidButtonDown = true;
	glm::vec2 MousePosition = glm::vec2(0.0f);
	glm::vec2 MouseDelta = glm::vec2(0.0f);
	glm::vec2 ScrollDelta = glm::vec2(0.0f);

	void OnUpdate(float) override
	{
		WDown = Input::IsKeyDown(Key::W);
		WPressed = Input::IsKeyPressed(Key::W);
		WReleased = Input::IsKeyReleased(Key::W);
		LeftDown = Input::IsMouseButtonDown(Mouse::ButtonLeft);
		LeftPressed = Input::IsMouseButtonPressed(Mouse::ButtonLeft);
		LeftReleased = Input::IsMouseButtonReleased(Mouse::ButtonLeft);
		InvalidKeyDown = Input::IsKeyDown(static_cast<KeyCode>(60000));
		InvalidButtonDown = Input::IsMouseButtonDown(static_cast<MouseCode>(200));
		MousePosition = Input::GetMousePosition();
		MouseDelta = Input::GetMouseDelta();
		ScrollDelta = Input::GetScrollDelta();
	}
};

ST_SCRIPT_CLASS(InputProbe)
{
	ST_SCRIPT_FIELD(WDown);
	ST_SCRIPT_FIELD(WPressed);
	ST_SCRIPT_FIELD(WReleased);
	ST_SCRIPT_FIELD(LeftDown);
	ST_SCRIPT_FIELD(LeftPressed);
	ST_SCRIPT_FIELD(LeftReleased);
	ST_SCRIPT_FIELD(InvalidKeyDown);
	ST_SCRIPT_FIELD(InvalidButtonDown);
	ST_SCRIPT_FIELD(MousePosition);
	ST_SCRIPT_FIELD(MouseDelta);
	ST_SCRIPT_FIELD(ScrollDelta);
}

// Samples the time queries and applies SetTimeScaleTo (when non-negative) once.
class TimeProbe : public Script
{
public:
	float UpdateArgument = 0.0f;
	float LateArgument = 0.0f;
	float FixedArgument = 0.0f;
	float DeltaTime = 0.0f;
	float FixedDeltaTime = 0.0f;
	float ElapsedTime = -1.0f;
	int32_t FrameIndex = -1;
	float TimeScale = 0.0f;
	int32_t FixedSteps = 0;
	float SetTimeScaleTo = -1.0f;

	void OnUpdate(float deltaTime) override
	{
		UpdateArgument = deltaTime;
		DeltaTime = Time::GetDeltaTime();
		ElapsedTime = static_cast<float>(Time::GetElapsedTime());
		FrameIndex = static_cast<int32_t>(Time::GetFrameIndex());
		TimeScale = Time::GetTimeScale();
		if (SetTimeScaleTo >= 0.0f)
		{
			Time::SetTimeScale(SetTimeScaleTo);
			SetTimeScaleTo = -1.0f;
		}
	}

	void OnFixedUpdate(float fixedDeltaTime) override
	{
		FixedArgument = fixedDeltaTime;
		FixedDeltaTime = Time::GetFixedDeltaTime();
		FixedSteps++;
	}

	void OnLateUpdate(float deltaTime) override
	{
		LateArgument = deltaTime;
	}
};

ST_SCRIPT_CLASS(TimeProbe)
{
	ST_SCRIPT_FIELD(UpdateArgument);
	ST_SCRIPT_FIELD(LateArgument);
	ST_SCRIPT_FIELD(FixedArgument);
	ST_SCRIPT_FIELD(DeltaTime);
	ST_SCRIPT_FIELD(FixedDeltaTime);
	ST_SCRIPT_FIELD(ElapsedTime);
	ST_SCRIPT_FIELD(FrameIndex);
	ST_SCRIPT_FIELD(TimeScale);
	ST_SCRIPT_FIELD(FixedSteps);
	ST_SCRIPT_FIELD(SetTimeScaleTo);
}

// Samples scene queries every update.
class SceneProbe : public Script
{
public:
	Entity PrimaryCamera;
	int32_t EnemyCount = 0;
	int32_t RootCount = 0;
	Entity FirstEnemy;

	void OnUpdate(float) override
	{
		PrimaryCamera = Scene::GetPrimaryCamera();
		const std::vector<Entity> enemies = Scene::FindEntitiesByTag("Enemy");
		EnemyCount = static_cast<int32_t>(enemies.size());
		FirstEnemy = enemies.empty() ? Entity() : enemies.front();
		RootCount = static_cast<int32_t>(Scene::GetRootEntities().size());
		Log::Info("SceneProbe sees ", EnemyCount, " enemies; camera ", PrimaryCamera, ", first enemy at ", FirstEnemy.GetTransform().GetWorldPosition());
	}
};

ST_SCRIPT_CLASS(SceneProbe)
{
	ST_SCRIPT_FIELD(PrimaryCamera);
	ST_SCRIPT_FIELD(EnemyCount);
	ST_SCRIPT_FIELD(RootCount);
	ST_SCRIPT_FIELD(FirstEnemy);
}
