// Scripts working with other scripts: adding, finding, calling and removing script instances.

#include "SharedScripts.h"

#include <string>

using namespace Strata;
using namespace FeatureTest;

void FeatureTest::Receiver::OnCreate()
{
	Journal(*this, "Receiver", "OnCreate");
}

void FeatureTest::Receiver::Receive(const std::string& message)
{
	Received++;
	LastMessage = message;
}

ST_SCRIPT_CLASS(Receiver)
{
	ST_SCRIPT_FIELD(Received);
	ST_SCRIPT_FIELD(LastMessage);
}

// Added and removed by name by the Messenger.
class Helper : public Script
{
public:
	void OnCreate() override
	{
		Journal(*this, "Helper", "OnCreate");
	}

	void OnDestroy() override
	{
		Journal(*this, "Helper", "OnDestroy");
	}
};

ST_SCRIPT_CLASS(Helper)
{
}

// On "Script Features"; talks to the Receiver on "Receiver Host" every frame.
class Messenger : public FeatureScript
{
public:
	static constexpr int32_t c_RemoveHelperFrame = 2;

	int32_t Sent = 0;

	void OnCreate() override
	{
		Journal(*this, "Messenger", "OnCreate");
		Entity self = GetEntity();
		Expect(self.GetScript<Messenger>() == this && self.HasScript("Messenger"), "GetScript returns the instance itself");

		Expect(!self.HasScript("Receiver") && self.GetScript<Receiver>() == nullptr, "no Receiver on the messenger yet");
		Receiver* added = self.AddScript<Receiver>();
		Expect(added != nullptr && self.HasScript("Receiver") && self.GetScript<Receiver>() == added, "AddScript<T> creates the instance right away");
		if (added)
			added->Receive("to myself");

		Expect(self.AddScript("Helper") && self.HasScript("Helper"), "AddScript by class name");
	}

	void OnUpdate(float) override
	{
		const int32_t frame = GetFrame();
		Entity host = Scene::FindEntityByName("Receiver Host");
		Receiver* receiver = host.GetScript<Receiver>();
		Expect(receiver != nullptr && host.GetScript<Messenger>() == nullptr, "GetScript finds another entity's script (and only its own classes)");
		if (receiver)
		{
			receiver->Receive("message " + std::to_string(Sent));
			Sent++;
			Expect(receiver->Received == Sent && receiver->LastMessage == "message " + std::to_string(Sent - 1), "calls reach the other script");
		}

		Receiver* own = GetEntity().GetScript<Receiver>();
		Expect(own != nullptr && own->Received == 1 && own->LastMessage == "to myself", "the added Receiver kept its state");

		if (frame == c_RemoveHelperFrame)
		{
			Expect(GetEntity().RemoveScript("Helper") && !GetEntity().HasScript("Helper"), "RemoveScript");
		}
		else if (frame == c_RemoveHelperFrame + 1)
		{
			Expect(!GetEntity().RemoveScript("Helper"), "removing a script that is gone fails");
			Completed = true;
		}
	}
};

ST_SCRIPT_CLASS(Messenger)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(Sent);
}
