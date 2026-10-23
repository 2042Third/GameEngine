#include <doctest/doctest.h>

#include "Scripting/ScriptTestUtils.h"
#include "Strata/Core/EngineServices.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SaveFiles.h"
#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Input/Input.h"
#include "Strata/Input/InputActions.h"
#include "Strata/Input/InputActionSet.h"
#include "TestHelpers.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// The project the API test module's ActionReader expects (StrataTests/Scripts/API/InputActionScripts.cpp), provided while
	// it lives (validated through a local registry, as Core.InputActions does), with the input state reset around it.
	class ScriptActionsProject
	{
	public:
		ScriptActionsProject()
		{
			REQUIRE(m_Registry.Register(InputActionSet::MakeSection()));
			const nlohmann::json actions = { { "Maps", nlohmann::json::array({
				{ { "Name", "Gameplay" }, { "Actions", nlohmann::json::array({
					{ { "Name", "Jump" }, { "Kind", "Button" }, { "Bindings", { "Key/Space", "Gamepad/A" } } },
					{ { "Name", "Steer" }, { "Kind", "Axis" }, { "Bindings", { "1D:Key/A,Key/D" } } },
					{ { "Name", "Move" }, { "Kind", "Vector2" }, { "Bindings", { "2D:Key/W,Key/S,Key/A,Key/D", "Gamepad/LeftStick" } } },
					{ { "Name", "Left" }, { "Bindings", { "Key/Q" } } } }) } },
				{ { "Name", "Vehicle" }, { "Enabled", false }, { "Actions", nlohmann::json::array({
					{ { "Name", "Horn" }, { "Bindings", { "Key/H" } } } }) } } }) } };
			std::string error;
			REQUIRE_MESSAGE(m_Values.Set(InputActionSet::c_SectionId, actions, m_Registry, &error), error);
			Input::Reset();
		}

		~ScriptActionsProject()
		{
			Input::Reset();
		}

		ScriptActionsProject(const ScriptActionsProject&) = delete;
		ScriptActionsProject& operator=(const ScriptActionsProject&) = delete;
	private:
		SettingsRegistry m_Registry;
		ProjectSettingsValues m_Values;
		EngineServices::ScopedProvider<IProjectSettings> m_Provider { m_Values };
	};

	// A frame as the program running a game has it: the frame's input (device events, here), then the actions, then the scene.
	void Step(Scene& scene, std::initializer_list<std::pair<KeyCode, bool>> keys = {})
	{
		Input::BeginFrame();
		for (const auto& [key, down] : keys)
			Input::ProcessKey(key, down);
		InputActions::Update();
		scene.OnUpdateRuntime(1.0f / 60.0f);
	}

	Entity AddReader(Scene& scene, const std::string& name, const std::string& action, const std::string& map = "Vehicle")
	{
		Entity entity = scene.CreateEntity(name);
		ScriptEntry& entry = AddScriptEntry(entity, "ActionReader");
		AddFieldOverride(entry, "Action", PropertyType::String, PropertyValue(action));
		AddFieldOverride(entry, "Map", PropertyType::String, PropertyValue(map));
		return entity;
	}

	// Asks a reader to do something in its next update (ActionReader's Request).
	void Ask(Scene& scene, Entity reader, int32_t request, const std::string& bindings = {})
	{
		ScriptSystem& system = GetScriptSystem(scene);
		REQUIRE(system.SetFieldValue(reader, "ActionReader", "Request", PropertyValue(request)));
		REQUIRE(system.SetFieldValue(reader, "ActionReader", "NewBindings", PropertyValue(bindings)));
	}

	constexpr int32_t c_SetBindings = 1;
	constexpr int32_t c_ResetBindings = 2;
	constexpr int32_t c_ResetAllBindings = 3;
	constexpr int32_t c_EnableMap = 4;
	constexpr int32_t c_DisableMap = 5;
	constexpr int32_t c_SaveBindings = 6;

	struct ReaderState
	{
		bool Down = false;
		bool Pressed = false;
		bool Released = false;
	};

	ReaderState GetState(Scene& scene, Entity reader)
	{
		const ScriptSystem& system = GetScriptSystem(scene);
		return { GetField<bool>(system, reader, "ActionReader", "Down"), GetField<bool>(system, reader, "ActionReader", "Pressed"),
			GetField<bool>(system, reader, "ActionReader", "Released") };
	}

	bool IsState(Scene& scene, Entity reader, bool down, bool pressed, bool released)
	{
		const ReaderState state = GetState(scene, reader);
		return state.Down == down && state.Pressed == pressed && state.Released == released;
	}

	std::string GetBindings(Scene& scene, Entity reader)
	{
		return GetField<std::string>(GetScriptSystem(scene), reader, "ActionReader", "Bindings");
	}

	bool WasAccepted(Scene& scene, Entity reader)
	{
		return GetField<bool>(GetScriptSystem(scene), reader, "ActionReader", "Accepted");
	}

	size_t CountLogged(const std::vector<LogEntry>& entries, std::string_view text)
	{
		return static_cast<size_t>(std::count_if(entries.begin(), entries.end(), [&](const LogEntry& entry) { return entry.Message.find(text) != std::string::npos; }));
	}

}

TEST_SUITE("Scripting.InputActions")
{
	TEST_CASE("Scripts read actions through every host function")
	{
		ScriptActionsProject project;
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		const Entity jump = AddReader(scene, "Jump", "Gameplay/Jump");
		const Entity steer = AddReader(scene, "Steer", "Steer");
		const Entity move = AddReader(scene, "Move", "Move");
		const Entity horn = AddReader(scene, "Horn", "Vehicle/Horn");
		LogCapture log;
		scene.OnRuntimeStart();
		const ScriptSystem& system = GetScriptSystem(scene);

		Step(scene);
		CHECK(IsState(scene, jump, false, false, false));
		CHECK(GetField<glm::vec2>(system, move, "ActionReader", "Value") == glm::vec2(0.0f));
		CHECK(GetBindings(scene, jump) == "Key/Space;Gamepad/A;");
		CHECK(GetBindings(scene, move) == "2D:Key/W,Key/S,Key/A,Key/D;Gamepad/LeftStick;");
		CHECK(GetField<int32_t>(system, horn, "ActionReader", "MapEnabled") == 0); // Disabled when the game starts

		// Space, D and H go down: the Vehicle map is disabled, so the horn stays up until a script enables the map.
		Step(scene, { { Key::Space, true }, { Key::D, true }, { Key::H, true } });
		CHECK(IsState(scene, jump, true, true, false));
		CHECK(GetField<glm::vec2>(system, jump, "ActionReader", "Value") == glm::vec2(1.0f, 0.0f));
		CHECK(IsState(scene, steer, true, true, false));
		CHECK(GetField<float>(system, steer, "ActionReader", "Axis") == doctest::Approx(1.0f));
		CHECK(GetField<glm::vec2>(system, steer, "ActionReader", "Value") == glm::vec2(1.0f, 0.0f));
		CHECK(GetField<glm::vec2>(system, move, "ActionReader", "Value") == glm::vec2(1.0f, 0.0f));
		CHECK(GetField<float>(system, move, "ActionReader", "Axis") == doctest::Approx(1.0f));
		CHECK(IsState(scene, horn, false, false, false));
		Ask(scene, horn, c_EnableMap);
		Step(scene);
		CHECK(WasAccepted(scene, horn));
		CHECK(GetField<int32_t>(system, horn, "ActionReader", "MapEnabled") == 1);
		CHECK(IsState(scene, jump, true, false, false));

		// The map is enabled from the next input frame on; a rebinding applies from then too.
		Ask(scene, jump, c_SetBindings, "Key/J;Gamepad/B");
		Step(scene);
		CHECK(IsState(scene, horn, true, true, false));
		CHECK(WasAccepted(scene, jump));
		CHECK(GetBindings(scene, jump) == "Key/J;Gamepad/B;");
		CHECK(IsState(scene, jump, true, false, false));

		// Space no longer jumps; the horn's map is disabled again while H holds the horn, which the script still reads as down
		// in this frame: the map lets go of it in the next input frame.
		Ask(scene, horn, c_DisableMap);
		Step(scene, { { Key::D, false } });
		CHECK(IsState(scene, jump, false, false, true));
		CHECK(IsState(scene, steer, false, false, true));
		CHECK(GetField<float>(system, steer, "ActionReader", "Axis") == 0.0f);
		CHECK(GetField<int32_t>(system, horn, "ActionReader", "MapEnabled") == 0);
		CHECK(IsState(scene, horn, true, false, false));
		// H is still held, but the map is disabled: the horn reports its release once, as a key let go would, then reads as up.
		Step(scene, { { Key::J, true } });
		CHECK(IsState(scene, jump, true, true, false));
		CHECK(IsState(scene, horn, false, false, true));

		// Resetting gives the action the project's bindings back from the next frame on, where Space, still held, presses it.
		Ask(scene, jump, c_ResetBindings);
		Step(scene, { { Key::J, false } });
		CHECK(IsState(scene, horn, false, false, false));
		CHECK(WasAccepted(scene, jump));
		CHECK(GetBindings(scene, jump) == "Key/Space;Gamepad/A;");
		CHECK(IsState(scene, jump, false, false, true));
		Step(scene);
		CHECK(IsState(scene, jump, true, true, false));

		// A player may unbind an action; resetting every action restores it.
		Ask(scene, steer, c_SetBindings, "");
		Step(scene);
		CHECK(WasAccepted(scene, steer));
		CHECK(GetBindings(scene, steer).empty());
		Ask(scene, jump, c_ResetAllBindings);
		Step(scene);
		Step(scene);
		CHECK(WasAccepted(scene, jump));
		CHECK(GetBindings(scene, steer) == "1D:Key/A,Key/D;");
		CHECK(InputActions::GetPlayerBindings().Actions.empty());

		// Nothing above was misuse.
		CHECK(log.GetEntries().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Unknown names fail and are reported once")
	{
		ScriptActionsProject project;
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		const Entity reader = AddReader(scene, "Unknown", "Fly", "Boat");
		LogCapture log;
		scene.OnRuntimeStart();
		const ScriptSystem& system = GetScriptSystem(scene);
		for (int frame = 0; frame < 3; frame++)
			Step(scene, { { Key::Space, true } });
		CHECK(IsState(scene, reader, false, false, false));
		CHECK(GetField<glm::vec2>(system, reader, "ActionReader", "Value") == glm::vec2(0.0f));
		CHECK(GetBindings(scene, reader).empty());
		// An unknown map is an answer, not misuse.
		CHECK(GetField<int32_t>(system, reader, "ActionReader", "MapEnabled") == -1);

		for (const int32_t request : { c_SetBindings, c_ResetBindings, c_EnableMap })
		{
			Ask(scene, reader, request, "Key/J");
			Step(scene);
			CHECK_FALSE(WasAccepted(scene, reader));
		}
		Step(scene);

		// Every function that read the name every frame reported it once.
		const std::vector<LogEntry> entries = log.GetEntries();
		for (const char* function : { "InputIsActionDown", "InputIsActionPressed", "InputIsActionReleased", "InputGetActionValue",
			"InputGetActionBindings", "InputSetActionBindings", "InputResetActionBindings" })
		{
			INFO("Host function ", function);
			CHECK(CountLogged(entries, std::string(function) + " (called by script 'ActionReader' on entity") == 1);
		}
		CHECK(CountLogged(entries, "There is no action 'Fly'") == 7);
		CHECK(CountLogged(entries, "InputSetActionMapEnabled (called by script 'ActionReader' on entity") == 1);
		CHECK(CountLogged(entries, "There is no action map 'Boat'") == 1);
		CHECK(CountLogged(entries, "InputIsActionMapEnabled") == 0);
		CHECK(entries.size() == 8);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Qualified names resolve and an ambiguous bare name is reported")
	{
		ScriptActionsProject project;
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		const Entity gameplayLeft = AddReader(scene, "Gameplay Left", "Gameplay/Left");
		const Entity uiLeft = AddReader(scene, "UI Left", "ui/LEFT"); // Names ignore case
		const Entity ambiguous = AddReader(scene, "Ambiguous", "Left");
		const Entity bare = AddReader(scene, "Bare", "jump");
		LogCapture log;
		scene.OnRuntimeStart();

		// Q is the game's Left, the arrow the UI map's: each qualified name reads its own.
		Step(scene, { { Key::Q, true } });
		CHECK(IsState(scene, gameplayLeft, true, true, false));
		CHECK(IsState(scene, uiLeft, false, false, false));
		CHECK(IsState(scene, ambiguous, false, false, false));
		Step(scene, { { Key::Q, false }, { Key::Left, true }, { Key::Space, true } });
		CHECK(IsState(scene, gameplayLeft, false, false, true));
		CHECK(IsState(scene, uiLeft, true, true, false));
		CHECK(IsState(scene, ambiguous, false, false, false));
		CHECK(GetBindings(scene, ambiguous).empty());
		// A bare name that only one map has needs no map.
		CHECK(IsState(scene, bare, true, true, false));
		CHECK(GetBindings(scene, bare) == "Key/Space;Gamepad/A;");

		// The ambiguous name is refused naming both actions, once per function.
		const std::vector<LogEntry> entries = log.GetEntries();
		CHECK(CountLogged(entries, "'Left' is ambiguous: Gameplay/Left, UI/Left") == 5);
		CHECK(CountLogged(entries, "InputIsActionDown (called by script 'ActionReader' on entity") == 1);
		CHECK(entries.size() == 5);
		scene.OnRuntimeStop();
	}

	TEST_CASE("SetActionBindings validates by kind")
	{
		ScriptActionsProject project;
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		const Entity jump = AddReader(scene, "Jump", "Jump");
		const Entity steer = AddReader(scene, "Steer", "Steer");
		const Entity move = AddReader(scene, "Move", "Move");
		const Entity checks = scene.CreateEntity("Checks");
		AddScriptEntry(checks, "ActionAPIChecks");
		LogCapture log;
		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		CheckScriptChecks(system, checks, "ActionAPIChecks", 20);
		CHECK(GetField<bool>(system, checks, "ActionAPIChecks", "Done"));
		Step(scene);

		const auto rebind = [&](Entity reader, const std::string& bindings)
		{
			Ask(scene, reader, c_SetBindings, bindings);
			Step(scene);
			return WasAccepted(scene, reader);
		};
		// A Button takes button-like controls only: the whole list is refused, changing nothing.
		CHECK_FALSE(rebind(jump, "Key/J;Gamepad/LeftStick"));
		CHECK_FALSE(rebind(jump, "Gamepad/LeftX"));
		CHECK(GetBindings(scene, jump) == "Key/Space;Gamepad/A;");
		CHECK(rebind(jump, "Gamepad/RightTrigger+;Mouse/Left"));
		// An Axis takes those, whole axes and 1D composites; a Vector2 sticks and 2D composites.
		CHECK(rebind(steer, "Gamepad/LeftX;1D:Key/Z,Key/X;Key/C"));
		CHECK_FALSE(rebind(steer, "2D:Key/W,Key/S,Key/A,Key/D"));
		CHECK(GetBindings(scene, steer) == "Gamepad/LeftX;1D:Key/Z,Key/X;Key/C;");
		CHECK_FALSE(rebind(move, "Key/W"));
		CHECK_FALSE(rebind(move, "Key/Nope"));
		CHECK(rebind(move, "Gamepad/RightStick"));
		CHECK(GetBindings(scene, move) == "Gamepad/RightStick;");

		const std::vector<LogEntry> entries = log.GetEntries();
		CHECK(CountLogged(entries, "Gameplay/Jump is a Button action, which cannot use 'Gamepad/LeftStick'") == 1);
		CHECK(CountLogged(entries, "Gameplay/Jump is a Button action, which cannot use 'Gamepad/LeftX'") == 1);
		CHECK(CountLogged(entries, "Gameplay/Steer is an Axis action, which cannot use '2D:Key/W,Key/S,Key/A,Key/D'") == 1);
		CHECK(CountLogged(entries, "Gameplay/Move is a Vector2 action, which cannot use 'Key/W'") == 1);
		CHECK(CountLogged(entries, "Gameplay/Move: 'Key/Nope' is no binding") == 1);
		// ActionAPIChecks' refusals: empty lines, text that is no binding, unreadable text, nine bindings, an unreadable name,
		// a null output pointer and an empty name.
		CHECK(CountLogged(entries, "Gameplay/Jump: '' is no binding: a binding is empty") == 1);
		CHECK(CountLogged(entries, "Gameplay/Jump: 'Key/Nope' is no binding") == 1);
		CHECK(CountLogged(entries, "the bindings text is not readable") == 1);
		CHECK(CountLogged(entries, "An action has at most 8 bindings (9 given)") == 1);
		CHECK(CountLogged(entries, "the action name is not readable") == 1);
		CHECK(CountLogged(entries, "the output pointer is null") == 1);
		CHECK(CountLogged(entries, "An action name is empty") == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("SaveBindings writes through IGameSaves and a new session reads it")
	{
		ScriptActionsProject project;
		const std::filesystem::path directory = CreateTemporaryDirectory("ScriptInputBindings") / "Saves";
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		{
			SaveDirectory saves(directory);
			EngineServices::ScopedProvider<IGameSaves> provider(saves);
			Scene scene;
			const Entity jump = AddReader(scene, "Jump", "Jump");
			LogCapture log;
			scene.OnRuntimeStart();
			Step(scene);
			Ask(scene, jump, c_SetBindings, "Key/J");
			Step(scene);
			Ask(scene, jump, c_SaveBindings);
			Step(scene);
			CHECK(WasAccepted(scene, jump));
			CHECK(log.GetEntries().empty());
			scene.OnRuntimeStop();
		}

		// The engine's file, in the save directory, outside the game's save files.
		const std::filesystem::path file = directory / InputPlayerBindings::c_FileName;
		REQUIRE(FileSystem::Exists(file));
		const std::optional<std::string> text = FileSystem::ReadText(file);
		REQUIRE(text);
		CHECK(text->find("\"Gameplay/Jump\"") != std::string::npos);
		CHECK(text->find("\"Key/J\"") != std::string::npos);
		CHECK(SaveFiles::List(directory).empty());

		// The session ends; the next one starts with the project's bindings until the program running the game loads the
		// players'.
		InputActions::ResetSession();
		CHECK(InputActions::GetPlayerBindings().Actions.empty());
		const std::optional<std::vector<InputBinding>> projectBindings = InputActions::GetBindings("Jump");
		REQUIRE(projectBindings);
		REQUIRE(projectBindings->size() == 2);
		CHECK(projectBindings->front().ToString() == "Key/Space");
		{
			SaveDirectory saves(directory);
			EngineServices::ScopedProvider<IGameSaves> provider(saves);
			InputActions::LoadBindings();
			Scene scene;
			const Entity jump = AddReader(scene, "Jump", "Jump");
			scene.OnRuntimeStart();
			Step(scene, { { Key::J, true } });
			CHECK(GetBindings(scene, jump) == "Key/J;");
			CHECK(IsState(scene, jump, true, true, false));
			scene.OnRuntimeStop();
		}

		// Without a save directory there is nowhere to keep them: refused and reported.
		SaveDirectory none;
		EngineServices::ScopedProvider<IGameSaves> provider(none);
		Scene scene;
		const Entity jump = AddReader(scene, "Jump", "Jump");
		LogCapture log;
		scene.OnRuntimeStart();
		Ask(scene, jump, c_SaveBindings);
		Step(scene);
		CHECK_FALSE(WasAccepted(scene, jump));
		CHECK(CountLogged(log.GetEntries(), "InputSaveBindings (called by script 'ActionReader' on entity") == 1);
		CHECK(CountLogged(log.GetEntries(), "no save directory") == 1);
		scene.OnRuntimeStop();
	}
}
