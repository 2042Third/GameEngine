// Scripts of the Scripting.Input and Scripting.Game tests (StrataTests/src/Scripting/ScriptShellTests.cpp): gamepads, the
// cursor, the game's window and save files.

#include "TestScripts.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace Strata;
using namespace ScriptTests;

// Samples a gamepad's queries every update into its fields.
class GamepadProbe : public Script
{
public:
	int32_t Gamepad = 0;
	bool Connected = false;
	bool ADown = false;
	bool APressed = false;
	bool AReleased = false;
	bool StartDown = false;
	float LeftX = 0.0f;         // Without a dead zone
	float LeftXDeadZone = 0.0f; // With the default dead zone
	float LeftY = 0.0f;
	glm::vec2 LeftStick = glm::vec2(0.0f);
	float RightTrigger = 0.0f;
	bool InvalidButtonDown = true;
	bool InvalidGamepadConnected = true;
	float InvalidAxis = 1.0f;

	void OnUpdate(float) override
	{
		const uint32_t gamepad = static_cast<uint32_t>(Gamepad);
		Connected = Input::IsGamepadConnected(gamepad);
		ADown = Input::IsGamepadButtonDown(GamepadButton::A, gamepad);
		APressed = Input::IsGamepadButtonPressed(GamepadButton::A, gamepad);
		AReleased = Input::IsGamepadButtonReleased(GamepadButton::A, gamepad);
		StartDown = Input::IsGamepadButtonDown(GamepadButton::Start, gamepad);
		LeftX = Input::GetGamepadAxis(GamepadAxis::LeftX, gamepad, 0.0f);
		LeftXDeadZone = Input::GetGamepadAxis(GamepadAxis::LeftX, gamepad);
		LeftY = Input::GetGamepadAxis(GamepadAxis::LeftY, gamepad, 0.0f);
		LeftStick = Input::GetGamepadStick(GamepadStick::Left, gamepad);
		RightTrigger = Input::GetGamepadAxis(GamepadAxis::RightTrigger, gamepad, 0.0f);
		InvalidButtonDown = Input::IsGamepadButtonDown(static_cast<GamepadButton>(99), gamepad);
		InvalidGamepadConnected = Input::IsGamepadConnected(9);
		InvalidAxis = Input::GetGamepadAxis(static_cast<GamepadAxis>(42), gamepad, 0.0f);
	}
};

ST_SCRIPT_CLASS(GamepadProbe)
{
	ST_SCRIPT_FIELD(Gamepad);
	ST_SCRIPT_FIELD(Connected);
	ST_SCRIPT_FIELD(ADown);
	ST_SCRIPT_FIELD(APressed);
	ST_SCRIPT_FIELD(AReleased);
	ST_SCRIPT_FIELD(StartDown);
	ST_SCRIPT_FIELD(LeftX);
	ST_SCRIPT_FIELD(LeftXDeadZone);
	ST_SCRIPT_FIELD(LeftY);
	ST_SCRIPT_FIELD(LeftStick);
	ST_SCRIPT_FIELD(RightTrigger);
	ST_SCRIPT_FIELD(InvalidButtonDown);
	ST_SCRIPT_FIELD(InvalidGamepadConnected);
	ST_SCRIPT_FIELD(InvalidAxis);
}

// Sets the cursor mode Mode (0 normal, 1 hidden, 2 locked) when it starts, and reports what the engine answers.
class CursorProbe : public CheckingScript
{
public:
	int32_t Mode = 2;
	int32_t Reported = -1;

	void OnCreate() override
	{
		Input::SetCursorMode(static_cast<CursorMode>(Mode));
		Reported = static_cast<int32_t>(Input::GetCursorMode());
		Expect(Reported == Mode, "GetCursorMode reports the mode set");

		// An unknown mode is reported and changes nothing.
		const StrataScriptHostAPI* host = Detail::GetHost();
		host->SetCursorMode(Detail::GetContext(), 7);
		Expect(Input::GetCursorMode() == static_cast<CursorMode>(Mode), "an unknown cursor mode changes nothing");
	}
};

ST_SCRIPT_CLASS(CursorProbe)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Mode);
	ST_SCRIPT_FIELD(Reported);
}

// Asks for a window mode, a resolution and vsync on its first update, and checks that refused requests are refused. Size is
// what Game::GetWindowSize reports then; Accepted whether the engine took the requests (false without a window service).
class WindowRequests : public CheckingScript
{
public:
	int32_t Mode = 1;
	int32_t Width = 1280;
	int32_t Height = 720;
	bool VSync = false;
	bool Accepted = false;
	glm::vec2 Size = glm::vec2(-1.0f);
	bool Done = false;

	void OnUpdate(float) override
	{
		if (Done)
			return;
		Done = true;

		const bool mode = Game::SetWindowMode(static_cast<WindowMode>(Mode));
		const bool resolution = Game::SetResolution(static_cast<uint32_t>(Width), static_cast<uint32_t>(Height));
		const bool vsync = Game::SetVSync(VSync);
		Accepted = mode && resolution && vsync;
		Size = glm::vec2(Game::GetWindowSize());

		Expect(!Game::SetResolution(0, 600), "a zero width is refused");
		Expect(!Game::SetResolution(800, 16385), "a height above 16384 is refused");
		const StrataScriptHostAPI* host = Detail::GetHost();
		Expect(!host->SetWindowMode(Detail::GetContext(), 3), "an unknown window mode is refused");
		Expect(!host->GetWindowSize(Detail::GetContext(), nullptr), "a null output is refused");

		// An engine without the functions (a host table that ends before them) accepts nothing and reports no size.
		StrataScriptHostAPI older = *host;
		older.StructSize = static_cast<uint32_t>(offsetof(StrataScriptHostAPI, SetWindowMode));
		Detail::s_Host = &older;
		const bool olderMode = Game::SetWindowMode(WindowMode::Borderless);
		const glm::uvec2 olderSize = Game::GetWindowSize();
		Detail::s_Host = host;
		Expect(!olderMode && olderSize == glm::uvec2(0), "an engine without the functions changes nothing");
	}
};

ST_SCRIPT_CLASS(WindowRequests)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Mode);
	ST_SCRIPT_FIELD(Width);
	ST_SCRIPT_FIELD(Height);
	ST_SCRIPT_FIELD(VSync);
	ST_SCRIPT_FIELD(Accepted);
	ST_SCRIPT_FIELD(Size);
	ST_SCRIPT_FIELD(Done);
}

// Writes, reads, lists and deletes save files on its first update and leaves "Kept" (Text) behind for the engine side to
// find. Names that would leave the save directory are refused.
class SaveRoundTrip : public CheckingScript
{
public:
	std::string Text = "level=3;score=1200";
	std::string Listed;
	bool Done = false;

	void OnUpdate(float) override
	{
		if (Done)
			return;
		Done = true;

		Expect(Game::WriteSaveFile("Kept", Text), "WriteSaveFile(text)");
		const std::optional<std::string> read = Game::ReadSaveText("Kept");
		Expect(read && *read == Text, "ReadSaveText returns what was written");

		// Bytes, larger than the SDK's first read buffer.
		std::vector<uint8_t> bytes(1000);
		for (size_t index = 0; index < bytes.size(); index++)
			bytes[index] = static_cast<uint8_t>(index * 7);
		Expect(Game::WriteSaveFile("Binary.bin", std::span<const uint8_t>(bytes)), "WriteSaveFile(bytes)");
		const std::optional<std::vector<uint8_t>> readBytes = Game::ReadSaveFile("Binary.bin");
		Expect(readBytes && *readBytes == bytes, "ReadSaveFile returns the bytes written");
		Expect(Game::WriteSaveFile("Empty", std::string_view()), "an empty save");
		const std::optional<std::string> empty = Game::ReadSaveText("Empty");
		Expect(empty && empty->empty(), "an empty save reads as empty");

		const std::vector<std::string> names = Game::ListSaveFiles();
		for (const std::string& name : names)
			Listed += name + ";";
		Expect(names == std::vector<std::string> { "Binary.bin", "Empty", "Kept" }, "ListSaveFiles lists the saves, sorted");

		Expect(Game::DeleteSaveFile("Binary.bin"), "DeleteSaveFile");
		Expect(!Game::DeleteSaveFile("Binary.bin"), "deleting a missing save");
		Expect(!Game::ReadSaveFile("Binary.bin"), "a deleted save reads as nothing");
		Expect(Game::DeleteSaveFile("Empty"), "DeleteSaveFile(Empty)");

		// Names that would leave the directory, or name no file everywhere, are refused.
		Expect(!Game::WriteSaveFile("../Escape", Text), "a name with a separator and ..");
		Expect(!Game::WriteSaveFile("Sub/Slot", Text), "a name with a separator");
		Expect(!Game::WriteSaveFile("..", Text), "the name ..");
		Expect(!Game::WriteSaveFile("", Text), "an empty name");
		Expect(!Game::WriteSaveFile("NUL", Text), "a device name");
		Expect(!Game::ReadSaveText("..\\Kept"), "reading through ..");
		Expect(!Game::DeleteSaveFile("../Kept"), "deleting through ..");

		const StrataScriptHostAPI* host = Detail::GetHost();
		StrataScriptContext* context = Detail::GetContext();
		Expect(!host->WriteSaveFile(context, Detail::ToABIString("Null"), nullptr, 4), "a null data pointer with a size");
		Expect(!host->ReadSaveFile(context, Detail::ToABIString("Kept"), nullptr, 0, nullptr), "a null size pointer");
		uint64_t size = 0;
		Expect(host->ReadSaveFile(context, Detail::ToABIString("Kept"), nullptr, 0, &size) && size == Text.size(), "the size alone");

		// An engine without the functions saves nothing.
		StrataScriptHostAPI older = *host;
		older.StructSize = static_cast<uint32_t>(offsetof(StrataScriptHostAPI, WriteSaveFile));
		Detail::s_Host = &older;
		const bool written = Game::WriteSaveFile("Older", Text);
		const bool olderRead = Game::ReadSaveText("Kept").has_value();
		const bool olderList = Game::ListSaveFiles().empty();
		Detail::s_Host = host;
		Expect(!written && !olderRead && olderList, "an engine without the functions saves nothing");
	}
};

ST_SCRIPT_CLASS(SaveRoundTrip)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Text);
	ST_SCRIPT_FIELD(Listed);
	ST_SCRIPT_FIELD(Done);
}

// Reads the save "Kept" when it starts: Loaded is its text, or empty without one.
class SaveLoader : public Script
{
public:
	std::string Loaded;
	bool Found = false;

	void OnCreate() override
	{
		const std::optional<std::string> text = Game::ReadSaveText("Kept");
		Found = text.has_value();
		Loaded = text.value_or(std::string());
	}
};

ST_SCRIPT_CLASS(SaveLoader)
{
	ST_SCRIPT_FIELD(Loaded);
	ST_SCRIPT_FIELD(Found);
}
