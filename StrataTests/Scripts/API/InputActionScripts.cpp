// Scripts of the Scripting.InputActions tests (StrataTests/src/Scripting/ScriptInputActionTests.cpp), which give the project the
// map Gameplay (Jump: a Button on Space and gamepad A; Steer: an Axis on A and D; Move: a Vector2 on W, S, A, D and the left
// stick; Left: a Button on Q, sharing its bare name with UI/Left) and the map Vehicle (Horn: a Button on H), disabled when the
// game starts.

#include "TestScripts.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;
using namespace ScriptTests;

namespace
{

	std::string JoinBindings(const std::vector<std::string>& bindings)
	{
		std::string text;
		for (const std::string& binding : bindings)
		{
			text += binding;
			text += ';';
		}
		return text;
	}

	// "Key/J;Gamepad/B" as a list; empty text is no binding.
	std::vector<std::string> SplitBindings(std::string_view text)
	{
		std::vector<std::string> bindings;
		size_t start = 0;
		while (start < text.size())
		{
			const size_t end = text.find(';', start);
			const size_t stop = end == std::string_view::npos ? text.size() : end;
			bindings.emplace_back(text.substr(start, stop - start));
			start = stop + 1;
		}
		return bindings;
	}

}

// Reads an action and a map every update into fields, after doing what the test asked through Request (then cleared):
// 1 sets the action's bindings to NewBindings (';'-separated, empty unbinds it), 2 resets the action's bindings, 3 resets
// every action's, 4 enables the map, 5 disables it, 6 saves the players' bindings. Accepted is the request's result.
class ActionReader : public Script
{
public:
	std::string Action = "Gameplay/Jump";
	std::string Map = "Vehicle";
	int32_t Request = 0;
	std::string NewBindings;
	bool Accepted = false;

	bool Down = false;
	bool Pressed = false;
	bool Released = false;
	glm::vec2 Value = glm::vec2(0.0f);
	float Axis = 0.0f;
	std::string Bindings;
	int32_t MapEnabled = -1; // IsActionMapEnabled: 1, 0, or -1 for an unknown map

	void OnUpdate(float) override
	{
		switch (Request)
		{
			case 1: Accepted = Input::SetActionBindings(Action, SplitBindings(NewBindings)); break;
			case 2: Accepted = Input::ResetActionBindings(Action); break;
			case 3: Accepted = Input::ResetActionBindings(); break;
			case 4: Accepted = Input::SetActionMapEnabled(Map, true); break;
			case 5: Accepted = Input::SetActionMapEnabled(Map, false); break;
			case 6: Accepted = Input::SaveBindings(); break;
			default: break;
		}
		Request = 0;

		Down = Input::IsActionDown(Action);
		Pressed = Input::IsActionPressed(Action);
		Released = Input::IsActionReleased(Action);
		Value = Input::GetActionValue(Action);
		Axis = Input::GetActionAxis(Action);
		Bindings = JoinBindings(Input::GetActionBindings(Action));
		const std::optional<bool> enabled = Input::IsActionMapEnabled(Map);
		MapEnabled = enabled ? (*enabled ? 1 : 0) : -1;
	}
};

ST_SCRIPT_CLASS(ActionReader)
{
	ST_SCRIPT_FIELD(Action);
	ST_SCRIPT_FIELD(Map);
	ST_SCRIPT_FIELD(Request);
	ST_SCRIPT_FIELD(NewBindings);
	ST_SCRIPT_FIELD(Accepted);
	ST_SCRIPT_FIELD(Down);
	ST_SCRIPT_FIELD(Pressed);
	ST_SCRIPT_FIELD(Released);
	ST_SCRIPT_FIELD(Value);
	ST_SCRIPT_FIELD(Axis);
	ST_SCRIPT_FIELD(Bindings);
	ST_SCRIPT_FIELD(MapEnabled);
}

// A rebinding menu's capture, driven by the test through Request (then cleared): 1 begins one on Devices (the ABI's bits),
// 2 cancels it. Accepted is the request's result. Every update polls it: Binding is the last binding a poll returned and
// Polls counts them; with SmallBuffer a raw poll into a 2-byte buffer comes first, whose answer is SmallBufferSize. Down
// reads Gameplay/Jump.
class BindingCapturer : public Script
{
public:
	int32_t Request = 0;
	int32_t Devices = StrataScriptInputDevices_Keyboard;
	bool Accepted = false;
	bool SmallBuffer = false;
	int32_t SmallBufferSize = 0;
	std::string Binding;
	int32_t Polls = 0;
	bool Down = false;

	void OnUpdate(float) override
	{
		switch (Request)
		{
			case 1: Accepted = Input::BeginBindingCapture(static_cast<InputDevices>(static_cast<uint32_t>(Devices))); break;
			case 2: Accepted = Input::CancelBindingCapture(); break;
			default: break;
		}
		Request = 0;

		if (SmallBuffer)
		{
			char start[2] = {};
			SmallBufferSize = static_cast<int32_t>(Detail::GetHost()->InputPollBindingCapture(Detail::GetContext(), start, sizeof(start)));
		}
		if (const std::optional<std::string> binding = Input::PollBindingCapture())
		{
			Binding = *binding;
			Polls++;
		}
		Down = Input::IsActionDown("Jump");
	}
};

ST_SCRIPT_CLASS(BindingCapturer)
{
	ST_SCRIPT_FIELD(Request);
	ST_SCRIPT_FIELD(Devices);
	ST_SCRIPT_FIELD(Accepted);
	ST_SCRIPT_FIELD(SmallBuffer);
	ST_SCRIPT_FIELD(SmallBufferSize);
	ST_SCRIPT_FIELD(Binding);
	ST_SCRIPT_FIELD(Polls);
	ST_SCRIPT_FIELD(Down);
}

// The binding text protocol through the raw host functions, the SDK's own refusals and an engine without the input action
// functions, in OnCreate (Gameplay/Jump ends with the project's bindings).
class ActionAPIChecks : public CheckingScript
{
public:
	bool Done = false;

	void OnCreate() override
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		const auto bindings = []() { return JoinBindings(Input::GetActionBindings("Jump")); };
		const auto setText = [host](std::string_view text) { return host->InputSetActionBindings(Detail::GetContext(), Detail::ToABIString("Jump"), Detail::ToABIString(text)); };

		Expect(bindings() == "Key/Space;Gamepad/A;", "the project's bindings");
		// The text buffer protocol: the full size whatever the capacity.
		char start[4] = {};
		Expect(host->InputGetActionBindings(Detail::GetContext(), Detail::ToABIString("Jump"), start, sizeof(start)) == 20, "the bindings' size (\"Key/Space\\nGamepad/A\\n\")");
		Expect(std::string_view(start, sizeof(start)) == "Key/", "a buffer too small takes the start");

		// Each binding is followed by a line break, the last one's optional; an empty line is no binding.
		Expect(setText("Key/J\n") && bindings() == "Key/J;", "a final line break");
		Expect(setText("Key/K\nGamepad/B") && bindings() == "Key/K;Gamepad/B;", "no final line break");
		Expect(!setText("Key/J\n\n") && !setText("\n") && !setText("Key/J\n\nGamepad/B"), "empty lines are no bindings");
		Expect(!setText("Key/Nope") && bindings() == "Key/K;Gamepad/B;", "text that is no binding changes nothing");
		Expect(!host->InputSetActionBindings(Detail::GetContext(), Detail::ToABIString("Jump"), StrataScriptString { nullptr, 5 })
			&& bindings() == "Key/K;Gamepad/B;", "bindings text that cannot be read");
		Expect(!Input::SetActionBindings("Jump", { "Key/J\nKey/L" }) && bindings() == "Key/K;Gamepad/B;", "the SDK refuses a line break within a binding");
		const std::vector<std::string> nine = { "Key/A", "Key/B", "Key/C", "Key/D", "Key/E", "Key/F", "Key/G", "Key/H", "Key/I" };
		Expect(!Input::SetActionBindings("Jump", nine) && bindings() == "Key/K;Gamepad/B;", "at most 8 bindings");
		Expect(Input::SetActionBindings("Jump", {}) && bindings().empty() && Input::GetActionBindings("Jump").empty(), "no bindings unbind the action");

		// A name that cannot be read never resets every action.
		Expect(Input::SetActionBindings("Jump", { "Key/J" }), "SetActionBindings");
		Expect(!host->InputResetActionBindings(Detail::GetContext(), StrataScriptString { nullptr, 4 }) && bindings() == "Key/J;", "an unreadable name resets nothing");
		Expect(Input::ResetActionBindings("Jump") && bindings() == "Key/Space;Gamepad/A;", "ResetActionBindings");

		// A null output pointer fails; GetActionValue's output is zeroed on failure.
		Expect(!host->InputGetActionValue(Detail::GetContext(), Detail::ToABIString("Jump"), nullptr), "a null output pointer");
		float value[2] = { 3.0f, 4.0f };
		Expect(!host->InputGetActionValue(Detail::GetContext(), Detail::ToABIString(""), value) && value[0] == 0.0f && value[1] == 0.0f,
			"an empty name has no value");

		// An engine without the input action functions: everything fails, nothing changes.
		StrataScriptHostAPI older = *host;
		older.StructSize = static_cast<uint32_t>(offsetof(StrataScriptHostAPI, InputIsActionDown));
		{
			const ScopedHost scope(&older);
			Expect(!Input::IsActionDown("Jump") && !Input::IsActionPressed("Jump") && !Input::IsActionReleased("Jump"), "older engine: states");
			Expect(Input::GetActionValue("Move") == glm::vec2(0.0f) && Input::GetActionAxis("Steer") == 0.0f, "older engine: values");
			Expect(!Input::SetActionMapEnabled("Vehicle", true) && !Input::IsActionMapEnabled("Vehicle").has_value(), "older engine: maps");
			Expect(Input::GetActionBindings("Jump").empty() && !Input::SetActionBindings("Jump", { "Key/J" }), "older engine: bindings");
			Expect(!Input::ResetActionBindings("Jump") && !Input::ResetActionBindings() && !Input::SaveBindings(), "older engine: reset and save");
			Expect(!Input::BeginBindingCapture() && !Input::PollBindingCapture() && !Input::CancelBindingCapture(), "older engine: capture");
		}
		// An engine with the input actions but not yet the binding capture.
		older = *host;
		older.StructSize = static_cast<uint32_t>(offsetof(StrataScriptHostAPI, InputBeginBindingCapture));
		{
			const ScopedHost scope(&older);
			Expect(Input::GetActionBindings("Jump").size() == 2, "an engine without the capture reads bindings");
			Expect(!Input::BeginBindingCapture(InputDevices::KeyboardAndMouse) && !Input::PollBindingCapture() && !Input::CancelBindingCapture(),
				"an engine without the capture");
		}
		Expect(bindings() == "Key/Space;Gamepad/A;" && Input::IsActionMapEnabled("Vehicle") == false, "the older engine changed nothing");
		Done = true;
	}
};

ST_SCRIPT_CLASS(ActionAPIChecks)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Done);
}
