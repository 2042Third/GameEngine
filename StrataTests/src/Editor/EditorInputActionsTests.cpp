#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/ProjectSettings.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Input/Input.h>
#include <Strata/Input/InputActions.h>
#include <Strata/Input/InputActionSet.h>
#include <Strata/Project/Project.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	EditorContextSpecification MakeSpecification()
	{
		EditorContextSpecification specification;
		specification.WatchAssetFiles = false;
		return specification;
	}

	// Runs commands against an editor context without UI, with a project of its own (input actions are the project's).
	struct ActionCommands
	{
		EditorContext Context { MakeSpecification() };
		EditorCommandRegistry Commands;

		explicit ActionCommands(const std::string& name)
		{
			Input::Reset();
			const std::filesystem::path directory = CreateTemporaryDirectory(name) / "Game";
			Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });
		}

		~ActionCommands()
		{
			Context.Stop();
			Input::Reset();
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return RunCommandToCompletion(Context, Commands, name, parameters);
		}

		// A request a command refuses as invalid, changing nothing and recording no step; returns the reason.
		std::string Refuse(std::string_view command, const nlohmann::json& parameters)
		{
			INFO(parameters.dump());
			const nlohmann::json section = Context.GetProject()->GetSettings().GetStoredEntry(InputActionSet::c_SectionId);
			const size_t position = Context.GetUndoStack().GetPosition();
			const EditorCommandResult result = Commands.Execute(Context, command, parameters);
			CHECK_FALSE(result.Success);
			CHECK(result.ErrorKind == EditorCommandError::InvalidParameters);
			CHECK(Context.GetProject()->GetSettings().GetStoredEntry(InputActionSet::c_SectionId) == section);
			CHECK(Context.GetUndoStack().GetPosition() == position);
			return result.Error;
		}

		// The project's actions as its settings hold them.
		InputActionSet Actions()
		{
			const nlohmann::json* value = Context.GetProject()->GetSettings().FindSection(InputActionSet::c_SectionId);
			if (!value)
				return InputActionSet::MakeDefault();
			std::optional<InputActionSet> set = InputActionSet::FromJson(*value);
			REQUIRE(set);
			return *set;
		}

		std::vector<std::string> Bindings(std::string_view action)
		{
			const InputActionSet set = Actions();
			const std::optional<InputActionLocation> location = set.FindAction(action);
			REQUIRE(location);
			std::vector<std::string> texts;
			for (const InputBinding& binding : set.GetAction(*location).Bindings)
				texts.push_back(binding.ToString());
			return texts;
		}

		size_t Position() { return Context.GetUndoStack().GetPosition(); }

		// A frame as the editor application runs one: an input frame, the actions, then the editor's update.
		void Frame()
		{
			Input::BeginFrame();
			InputActions::Update();
			Context.Update(Timestep(1.0f / 60.0f));
		}
	};

}

TEST_SUITE("Editor.InputActions")
{
	TEST_CASE("Maps and actions are added, renamed, moved and removed with one undo step each")
	{
		ActionCommands harness("InputActionsEdit");
		CHECK_FALSE(harness.Context.GetProject()->GetSettings().IsStored(InputActionSet::c_SectionId));

		size_t position = harness.Position();
		nlohmann::json result = harness.Run("inputActions.setMap", { { "map", "Gameplay" } });
		CHECK(result["created"] == true);
		CHECK(result["map"]["name"] == "Gameplay");
		CHECK(result["map"]["enabled"] == true);
		CHECK(harness.Position() == ++position);
		CHECK(harness.Context.GetUndoStack().GetUndoName() == "Add Input Map");
		CHECK_FALSE(harness.Context.IsSceneModified());
		harness.Run("inputActions.setMap", { { "map", "Vehicle" }, { "enabled", false } });
		CHECK(harness.Position() == ++position);
		CHECK(harness.Actions().Maps.size() == 3);
		CHECK_FALSE(harness.Actions().Maps[2].Enabled);

		result = harness.Run("inputActions.setAction", { { "action", "Gameplay/Jump" }, { "bindings", { "Key/Space", "Gamepad/A" } } });
		CHECK(result["created"] == true);
		CHECK(result["action"]["name"] == "Gameplay/Jump");
		CHECK(result["action"]["kind"] == "Button");
		CHECK(result["action"]["bindings"] == nlohmann::json::array({ "Key/Space", "Gamepad/A" }));
		CHECK(harness.Position() == ++position);
		result = harness.Run("inputActions.setAction", { { "action", "Steer" }, { "map", "Gameplay" }, { "kind", "Axis" }, { "deadZone", 0.2 },
			{ "bindings", { "1D:Key/A,Key/D", "Gamepad/LeftX" } } });
		CHECK(result["action"]["name"] == "Gameplay/Steer");
		CHECK(result["action"]["deadZone"] == 0.2);
		CHECK(harness.Position() == ++position);
		CHECK(harness.Bindings("Steer") == std::vector<std::string> { "1D:Key/A,Key/D", "Gamepad/LeftX" });

		// Renamed, changed, moved, removed.
		result = harness.Run("inputActions.setAction", { { "action", "Jump" }, { "rename", "Leap" } });
		CHECK(result["action"]["name"] == "Gameplay/Leap");
		CHECK(harness.Context.GetUndoStack().GetUndoName() == "Rename Input Action");
		CHECK(harness.Position() == ++position);
		harness.Run("inputActions.setAction", { { "action", "Gameplay/Leap" }, { "map", "Vehicle" } });
		CHECK(harness.Context.GetUndoStack().GetUndoName() == "Move Input Action");
		CHECK(harness.Position() == ++position);
		CHECK(harness.Actions().FindAction("Vehicle/Leap"));
		CHECK_FALSE(harness.Actions().FindAction("Gameplay/Leap"));
		harness.Run("inputActions.setMap", { { "map", "Vehicle" }, { "rename", "Car" }, { "enabled", true } });
		CHECK(harness.Position() == ++position);
		CHECK(harness.Actions().FindAction("Car/Leap"));
		CHECK(harness.Actions().Maps[2].Enabled);
		CHECK(harness.Run("inputActions.setAction", { { "action", "Car/Leap" }, { "remove", true } })["removed"] == "Car/Leap");
		CHECK(harness.Position() == ++position);
		CHECK(harness.Run("inputActions.setMap", { { "map", "car" }, { "remove", true } })["removed"] == "Car");
		CHECK(harness.Position() == ++position);
		CHECK(harness.Actions().Maps.size() == 2);

		// The project file holds the set once its write is done.
		harness.Run("project.save");
		const std::optional<std::string> text = FileSystem::ReadText(harness.Context.GetProject()->GetProjectFile());
		REQUIRE(text);
		const std::optional<nlohmann::json> file = JsonUtils::Parse(*text);
		REQUIRE(file);
		CHECK((*file)["Settings"]["Input.Actions"]["Value"] == harness.Actions().ToJson());

		// Every step undoes and redoes; undoing them all leaves no section.
		harness.Run("edit.undo");
		CHECK(harness.Actions().FindMap("Car"));
		harness.Run("edit.redo");
		CHECK_FALSE(harness.Actions().FindMap("Car"));
		while (harness.Position() > 0)
			harness.Run("edit.undo");
		CHECK_FALSE(harness.Context.GetProject()->GetSettings().IsStored(InputActionSet::c_SectionId));
		CHECK(harness.Actions() == InputActionSet::MakeDefault());
	}

	TEST_CASE("The UI map's actions refuse renames and removal")
	{
		ActionCommands harness("InputActionsReserved");
		harness.Run("inputActions.setMap", { { "map", "Gameplay" } });
		CHECK(harness.Refuse("inputActions.setAction", { { "action", "UI/Submit" }, { "rename", "Confirm" } }).find("is the game UI's") != std::string::npos);
		CHECK(harness.Refuse("inputActions.setAction", { { "action", "UI/Cancel" }, { "remove", true } }).find("cannot be removed") != std::string::npos);
		CHECK(harness.Refuse("inputActions.setAction", { { "action", "UI/Up" }, { "kind", "Axis" }, { "bindings", nlohmann::json::array() } })
			.find("is a Button") != std::string::npos);
		CHECK(harness.Refuse("inputActions.setAction", { { "action", "UI/Left" }, { "map", "Gameplay" } }).find("stays in the map UI") != std::string::npos);
		CHECK(harness.Refuse("inputActions.setMap", { { "map", "UI" }, { "rename", "Menus" } }).find("reserved") != std::string::npos);
		CHECK(harness.Refuse("inputActions.setMap", { { "map", "UI" }, { "remove", true } }).find("reserved") != std::string::npos);

		// Their bindings change, and the map can hold a game's own actions.
		harness.Run("inputActions.setAction", { { "action", "UI/Submit" }, { "bindings", { "Key/J" } } });
		CHECK(harness.Bindings("UI/Submit") == std::vector<std::string> { "Key/J" });
		harness.Run("inputActions.setAction", { { "action", "UI/NextTab" }, { "bindings", { "Key/Tab" } } });
		CHECK(harness.Actions().FindAction("UI/NextTab"));
		harness.Run("inputActions.setAction", { { "action", "UI/NextTab" }, { "remove", true } });

		// A gameplay Left beside UI/Left: a bare name that both maps have is ambiguous.
		harness.Run("inputActions.setAction", { { "action", "Gameplay/Left" }, { "bindings", { "Key/A" } } });
		CHECK(harness.Refuse("inputActions.setAction", { { "action", "Left" }, { "bindings", { "Key/Q" } } }) == "Parameter 'action': 'Left' is ambiguous: "
			"Gameplay/Left, UI/Left");
	}

	TEST_CASE("Bindings are validated by kind")
	{
		ActionCommands harness("InputActionsBindings");
		harness.Run("inputActions.setMap", { { "map", "Gameplay" } });
		harness.Run("inputActions.setAction", { { "action", "Gameplay/Move" }, { "kind", "Vector2" }, { "bindings", { "Gamepad/LeftStick" } } });
		CHECK(harness.Refuse("inputActions.setAction", { { "action", "Move" }, { "bindings", { "Key/W" } } })
			.find("a Vector2 action cannot use 'Key/W'") != std::string::npos);
		CHECK(harness.Refuse("inputActions.setAction", { { "action", "Gameplay/Jump" }, { "bindings", { "Gamepad/LeftX" } } })
			.find("a Button action cannot use 'Gamepad/LeftX'") != std::string::npos);
		CHECK(harness.Refuse("inputActions.setAction", { { "action", "Move" }, { "bindings", { "Key/Nope" } } }).find("'Nope' is no key") != std::string::npos);
		CHECK(harness.Refuse("inputActions.setAction", { { "action", "Move" }, { "bindings", { "2D:Key/W,Key/S,Key/A" } } }).find("four parts")
			!= std::string::npos);
		// A kind change keeps only bindings that fit it, or takes new ones.
		CHECK(harness.Refuse("inputActions.setAction", { { "action", "Move" }, { "kind", "Button" } }).find("give 'bindings'") != std::string::npos);
		harness.Run("inputActions.setAction", { { "action", "Move" }, { "kind", "Axis" }, { "bindings", { "Gamepad/LeftX", "1D:Key/A,Key/D" } } });
		CHECK(harness.Bindings("Move") == std::vector<std::string> { "Gamepad/LeftX", "1D:Key/A,Key/D" });
		// Other refusals, each naming what is wrong.
		CHECK(harness.Refuse("inputActions.setAction", { { "action", "Move" }, { "deadZone", 1.5 } }).find("deadZone") != std::string::npos);
		// The largest dead zone, as agents write it and as the page's slider sends it (a float).
		CHECK(harness.Run("inputActions.setAction", { { "action", "Move" }, { "deadZone", 0.9 } })["action"]["deadZone"] == 0.9);
		harness.Run("inputActions.setAction", { { "action", "Move" }, { "deadZone", 0.2 } });
		CHECK(harness.Run("inputActions.setAction", { { "action", "Move" }, { "deadZone", 0.9f } })["action"]["deadZone"] == 0.9);
		CHECK(harness.Refuse("inputActions.setAction", { { "action", "Move" }, { "kind", "Trigger" } }).find("kind") != std::string::npos);
		CHECK(harness.Refuse("inputActions.setAction", { { "action", "Fly" } }).find("name the map") != std::string::npos);
		CHECK(harness.Refuse("inputActions.setAction", { { "action", "Air/Fly" } }).find("no map 'Air'") != std::string::npos);
		CHECK(harness.Refuse("inputActions.setAction", { { "action", "Gameplay/Fly!" } }).find("only letters") != std::string::npos);
		CHECK(harness.Refuse("inputActions.setAction", { { "action", "Move" }, { "remove", true }, { "kind", "Button" } }).find("no other change")
			!= std::string::npos);
		CHECK(harness.Refuse("inputActions.setMap", { { "map", "Ghost" }, { "remove", true } }).find("no map 'Ghost'") != std::string::npos);
		harness.Run("inputActions.setAction", { { "action", "Gameplay/Jump" } });
		CHECK(harness.Refuse("inputActions.setAction", { { "action", "Jump" }, { "rename", "move" } }).find("has an action named 'Move'") != std::string::npos);
		const std::vector<std::string> nine = { "Key/A", "Key/B", "Key/C", "Key/D", "Key/E", "Key/F", "Key/G", "Key/H", "Key/I" };
		CHECK(harness.Refuse("inputActions.setAction", { { "action", "Jump" }, { "bindings", nine } }).find("at most 8") != std::string::npos);

		// Without a project there are no actions to edit.
		harness.Run("project.close", { { "force", true } });
		const EditorCommandResult closed = harness.Commands.Execute(harness.Context, "inputActions.setMap", { { "map", "Gameplay" } });
		CHECK_FALSE(closed.Success);
		CHECK(closed.Error.find("No project is open") != std::string::npos);
	}

	TEST_CASE("An edit while playing changes the running game's actions in the next frame")
	{
		ActionCommands harness("InputActionsPlaying");
		harness.Run("inputActions.setMap", { { "map", "Gameplay" } });
		harness.Run("inputActions.setAction", { { "action", "Gameplay/Jump" }, { "bindings", { "Key/Space" } } });
		harness.Run("play.start", { { "build", false } });
		Input::SimulateKey(Key::J, true);
		harness.Frame();
		CHECK_FALSE(InputActions::IsDown("Jump"));

		const size_t position = harness.Position();
		const nlohmann::json result = harness.Run("inputActions.setAction", { { "action", "Jump" }, { "bindings", { "Key/Space", "Key/J" } } });
		CHECK_FALSE(result.contains("warning")); // A project change, not one of the running copy
		CHECK(harness.Position() == position + 1);
		harness.Frame();
		CHECK(InputActions::IsPressed("Gameplay/Jump"));
		// A map disabled at start applies when the next session starts.
		harness.Run("inputActions.setMap", { { "map", "Gameplay" }, { "enabled", false } });
		harness.Frame();
		CHECK(InputActions::IsMapEnabled("Gameplay") == std::optional<bool>(true));

		// The change stays after play mode.
		harness.Run("play.stop");
		CHECK(harness.Bindings("Jump") == std::vector<std::string> { "Key/Space", "Key/J" });
		harness.Run("play.start", { { "build", false } });
		harness.Frame();
		CHECK(InputActions::IsMapEnabled("Gameplay") == std::optional<bool>(false));
		harness.Run("play.stop");
	}

	TEST_CASE("Player bindings saved in play are shown and can be reset")
	{
		ActionCommands harness("InputActionsPlayerBindings");
		harness.Run("inputActions.setMap", { { "map", "Gameplay" } });
		harness.Run("inputActions.setAction", { { "action", "Gameplay/Jump" }, { "bindings", { "Key/Space" } } });
		harness.Run("inputActions.setAction", { { "action", "Gameplay/Fire" }, { "bindings", { "Mouse/Left" } } });
		CHECK(harness.Context.GetPlayerBindings().Actions.empty());
		// Nothing to reset is no failure.
		CHECK(harness.Run("inputActions.resetPlayerBindings")["reset"] == nlohmann::json::array());

		// A game's rebinding menu rebinds Jump and keeps it; while playing the marks follow the running game.
		harness.Run("play.start", { { "build", false } });
		std::string error;
		REQUIRE(InputActions::SetBindings("Gameplay/Jump", std::vector<InputBinding> { *InputBinding::Parse("Key/J") }));
		CHECK(harness.Context.GetPlayerBindings().Actions.count("Gameplay/Jump") == 1);
		REQUIRE_MESSAGE(InputActions::SaveBindings(&error), error);
		harness.Run("play.stop");
		const std::filesystem::path file = *harness.Context.GetGameSaveDirectory() / InputPlayerBindings::c_FileName;
		REQUIRE(FileSystem::Exists(file));

		// After play the file shows: edits of Jump's bindings would not reach the next session.
		const InputPlayerBindings& shown = harness.Context.GetPlayerBindings();
		REQUIRE(shown.Actions.size() == 1);
		CHECK(shown.Actions.at("Gameplay/Jump") == std::vector<InputBinding> { *InputBinding::Parse("Key/J") });
		harness.Run("inputActions.setAction", { { "action", "Jump" }, { "bindings", { "Key/Space", "Key/W" } } });
		harness.Run("play.start", { { "build", false } });
		CHECK(InputActions::GetBindings("Jump") == std::vector<InputBinding> { *InputBinding::Parse("Key/J") });

		// Reset while playing: the file goes and the running game gets the project's bindings back.
		const size_t position = harness.Position();
		CHECK(harness.Run("inputActions.resetPlayerBindings")["reset"] == nlohmann::json::array({ "Gameplay/Jump" }));
		CHECK(harness.Position() == position); // Not undoable, like the game's saves
		CHECK_FALSE(FileSystem::Exists(file));
		CHECK(harness.Context.GetPlayerBindings().Actions.empty());
		CHECK(InputActions::GetBindings("Jump") == std::vector<InputBinding> { *InputBinding::Parse("Key/Space"), *InputBinding::Parse("Key/W") });
		harness.Run("play.stop");
		CHECK(harness.Context.GetPlayerBindings().Actions.empty());

		// Saved again and reset in Edit mode: the next session starts with the project's bindings.
		harness.Run("play.start", { { "build", false } });
		REQUIRE(InputActions::SetBindings("Gameplay/Fire", std::vector<InputBinding> { *InputBinding::Parse("Key/F") }));
		REQUIRE_MESSAGE(InputActions::SaveBindings(&error), error);
		harness.Run("play.stop");
		CHECK(harness.Context.GetPlayerBindings().Actions.count("Gameplay/Fire") == 1);
		CHECK(harness.Run("inputActions.resetPlayerBindings")["reset"] == nlohmann::json::array({ "Gameplay/Fire" }));
		CHECK(harness.Context.GetPlayerBindings().Actions.empty());
		harness.Run("play.start", { { "build", false } });
		CHECK(InputActions::GetBindings("Fire") == std::vector<InputBinding> { *InputBinding::Parse("Mouse/Left") });
		harness.Run("play.stop");

		// Without a project there are no players' bindings, nor any to reset.
		harness.Run("play.start", { { "build", false } });
		REQUIRE(InputActions::SetBindings("Gameplay/Fire", std::vector<InputBinding> { *InputBinding::Parse("Key/F") }));
		REQUIRE_MESSAGE(InputActions::SaveBindings(&error), error);
		harness.Run("play.stop");
		REQUIRE(harness.Context.GetPlayerBindings().Actions.size() == 1);
		harness.Run("project.close");
		CHECK(harness.Context.GetPlayerBindings().Actions.empty());
		const EditorCommandResult refused = harness.Commands.Execute(harness.Context, "inputActions.resetPlayerBindings", nlohmann::json::object());
		CHECK_FALSE(refused.Success);
	}
}
