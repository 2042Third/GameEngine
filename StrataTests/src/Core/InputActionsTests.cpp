#include <doctest/doctest.h>

#include "TestHelpers.h"

#include "Strata/Core/EngineServices.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SaveFiles.h"
#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Core/StringUtils.h"
#include "Strata/Input/Input.h"
#include "Strata/Input/InputActions.h"
#include "Strata/Input/InputActionSet.h"
#include "Strata/Input/InputNames.h"
#include "Strata/Project/Project.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;

namespace
{

	// A project's settings with an Input.Actions section, provided while it lives (validated through a local registry, as the
	// process registry has the section only once the composition root registers it).
	class ActionsProject
	{
	public:
		explicit ActionsProject(const nlohmann::json& actions)
		{
			REQUIRE(m_Registry.Register(InputActionSet::MakeSection()));
			Set(actions);
			Input::Reset();
		}

		~ActionsProject()
		{
			Input::Reset();
		}

		void Set(const nlohmann::json& actions)
		{
			std::string error;
			REQUIRE_MESSAGE(m_Values.Set(InputActionSet::c_SectionId, actions, m_Registry, &error), error);
		}
	private:
		SettingsRegistry m_Registry;
		ProjectSettingsValues m_Values;
		EngineServices::ScopedProvider<IProjectSettings> m_Provider { m_Values };
	};

	// Gameplay actions of every kind; Left shares its bare name with the UI map's.
	nlohmann::json MakeGameplayActions()
	{
		return { { "Maps", nlohmann::json::array({ {
			{ "Name", "Gameplay" },
			{ "Actions", nlohmann::json::array({
				{ { "Name", "Jump" }, { "Kind", "Button" }, { "Bindings", { "Key/Space", "Gamepad/A", "Gamepad/RightTrigger+" } } },
				{ { "Name", "Steer" }, { "Kind", "Axis" }, { "Bindings", { "1D:Key/A,Key/D", "Gamepad/LeftX" } } },
				{ { "Name", "Throttle" }, { "Kind", "Axis" }, { "DeadZone", 0.0 }, { "Bindings", { "Gamepad/RightTrigger+", "Key/T" } } },
				{ { "Name", "Move" }, { "Kind", "Vector2" }, { "Bindings", { "2D:Key/W,Key/S,Key/A,Key/D", "Gamepad/LeftStick" } } },
				{ { "Name", "Left" }, { "Bindings", { "Key/Q" } } } }) } } }) } };
	}

	// An input frame as Application runs one: the frame starts (simulated input queued before it applies), then the actions
	// take it.
	void Step()
	{
		Input::BeginFrame();
		InputActions::Update();
	}

	void SetStick(float x, float y, uint32_t gamepad = 0)
	{
		Input::SimulateGamepadAxis(gamepad, GamepadAxis::LeftX, x);
		Input::SimulateGamepadAxis(gamepad, GamepadAxis::LeftY, y);
	}

	std::vector<InputBinding> ParseAll(std::initializer_list<const char*> texts)
	{
		std::vector<InputBinding> bindings;
		for (const char* text : texts)
		{
			std::string error;
			const std::optional<InputBinding> binding = InputBinding::Parse(text, &error);
			REQUIRE_MESSAGE(binding, error);
			bindings.push_back(*binding);
		}
		return bindings;
	}

	// Every binding text Read accepts for some kind: each control and composites of the longest parts.
	std::vector<std::string> MakeEveryBindingText()
	{
		std::vector<std::string> texts;
		for (const char* key : InputNames::GetKeyNames())
			texts.push_back(std::string("Key/") + key);
		for (const char* button : InputNames::GetMouseButtonNames())
			texts.push_back(std::string("Mouse/") + button);
		for (const char* button : InputNames::GetGamepadButtonNames())
			texts.push_back(std::string("Gamepad/") + button);
		for (uint32_t axis = 0; axis < c_GamepadAxisCount; axis++)
		{
			const std::string name = std::string("Gamepad/") + InputNames::GetGamepadAxisName(static_cast<GamepadAxis>(axis));
			texts.push_back(name);
			texts.push_back(name + "+");
			if (GetGamepadAxisMinimum(static_cast<GamepadAxis>(axis)) < 0.0f)
				texts.push_back(name + "-");
		}
		texts.push_back("Gamepad/LeftStick");
		texts.push_back("Gamepad/RightStick");
		texts.push_back("1D:Key/A,Key/D");
		texts.push_back("1D:Gamepad/LeftTrigger+,Mouse/Right");
		texts.push_back("2D:Key/W,Key/S,Key/A,Key/D");
		texts.push_back("2D:Gamepad/DPadUp,Gamepad/DPadDown,Gamepad/LeftX-,Gamepad/LeftX+");
		texts.push_back("2D:Gamepad/RightTrigger+,Gamepad/RightTrigger+,Gamepad/RightTrigger+,Gamepad/RightTrigger+");
		return texts;
	}

	std::string MakeName(char fill, size_t index)
	{
		std::string name(InputActionSet::c_MaxNameLength, fill);
		const std::string digits = std::to_string(index);
		name.replace(name.size() - digits.size(), digits.size(), digits);
		return name;
	}

	// A set at every limit: 32 maps (the UI map's included), 256 actions (the UI map's six included), 8 bindings each, every
	// name 64 characters and every binding as long as bindings get (90 bytes).
	nlohmann::json MakeLimitSet()
	{
		const std::string longest = "2D:Gamepad/RightTrigger+,Gamepad/RightTrigger+,Gamepad/RightTrigger+,Gamepad/RightTrigger+";
		REQUIRE(longest.size() == 90);
		nlohmann::json uiActions = nlohmann::json::array();
		for (const InputAction& action : InputActionSet::GetDefaultUIActions())
		{
			nlohmann::json bindings = nlohmann::json::array();
			for (size_t index = 0; index < InputActionSet::c_MaxBindings; index++)
				bindings.push_back("Gamepad/RightTrigger+");
			uiActions.push_back({ { "Name", action.Name }, { "Bindings", bindings } });
		}
		nlohmann::json maps = nlohmann::json::array({ { { "Name", "UI" }, { "Actions", uiActions } } });
		size_t actions = InputActionSet::GetDefaultUIActions().size();
		for (size_t map = 1; map < InputActionSet::c_MaxMaps; map++)
		{
			nlohmann::json mapActions = nlohmann::json::array();
			const size_t count = (InputActionSet::c_MaxActions - actions) / (InputActionSet::c_MaxMaps - map);
			for (size_t index = 0; index < count; index++)
			{
				nlohmann::json bindings = nlohmann::json::array();
				for (size_t binding = 0; binding < InputActionSet::c_MaxBindings; binding++)
					bindings.push_back(longest);
				mapActions.push_back({ { "Name", MakeName('a', index) }, { "Kind", "Vector2" }, { "DeadZone", 0.25 }, { "Bindings", bindings } });
			}
			actions += count;
			maps.push_back({ { "Name", MakeName('M', map) }, { "Enabled", map % 2 == 0 }, { "Actions", mapActions } });
		}
		REQUIRE(actions == InputActionSet::c_MaxActions);
		return { { "Maps", maps } };
	}

	std::string Refusal(const nlohmann::json& value)
	{
		std::string error;
		const std::optional<InputActionSet> set = InputActionSet::FromJson(value, &error);
		CHECK_FALSE_MESSAGE(set.has_value(), value.dump());
		return error;
	}

	// A set as one map of the given actions.
	nlohmann::json OneMap(const nlohmann::json& actions, const char* name = "Gameplay")
	{
		return { { "Maps", nlohmann::json::array({ { { "Name", name }, { "Actions", actions } } }) } };
	}

	// What every set Read accepts must hold: the UI map with its six Buttons, the limits, bindings that fit their kinds.
	void CheckValidSet(const InputActionSet& set)
	{
		REQUIRE(set.FindMap("UI"));
		CHECK(set.Maps.size() <= InputActionSet::c_MaxMaps);
		CHECK(set.GetActionCount() <= InputActionSet::c_MaxActions);
		for (const InputAction& reserved : InputActionSet::GetDefaultUIActions())
		{
			const std::optional<InputActionLocation> location = set.FindAction("UI/" + reserved.Name);
			REQUIRE(location);
			CHECK(set.GetAction(*location).Kind == InputActionKind::Button);
		}
		for (const InputActionMap& map : set.Maps)
		{
			CHECK_FALSE(InputActionSet::ValidateName(map.Name));
			for (const InputAction& action : map.Actions)
			{
				CHECK_FALSE(InputActionSet::ValidateName(action.Name));
				CHECK(action.Bindings.size() <= InputActionSet::c_MaxBindings);
				CHECK(action.DeadZone >= 0.0f);
				CHECK(action.DeadZone <= InputActionSet::c_MaxDeadZone);
				for (const InputBinding& binding : action.Bindings)
					CHECK(binding.FitsKind(action.Kind));
			}
		}
		const std::optional<InputActionSet> again = InputActionSet::FromJson(set.ToJson());
		REQUIRE(again);
		CHECK(*again == set);
	}

}

TEST_SUITE("Core.InputActions")
{
	TEST_CASE("Binding text round-trips for every control and composite")
	{
		size_t longest = 0;
		for (const std::string& text : MakeEveryBindingText())
		{
			INFO(text);
			std::string error;
			const std::optional<InputBinding> binding = InputBinding::Parse(text, &error);
			REQUIRE_MESSAGE(binding, error);
			CHECK(binding->ToString() == text);
			CHECK(InputBinding::Parse(binding->ToString()) == binding);
			// Names and prefixes ignore case; the text is written in its one spelling.
			CHECK(InputBinding::Parse(StringUtils::ToLower(text)) == binding);
			longest = std::max(longest, text.size());
		}
		CHECK(longest == 90);
		CHECK(longest <= InputBinding::c_MaxTextBytes);

		CHECK(InputBinding::Parse("Mouse/Button0")->ToString() == "Mouse/Left");
		const std::optional<InputBinding> composite = InputBinding::Parse("2d:key/up,KEY/DOWN,Key/Left,gamepad/leftx+");
		REQUIRE(composite);
		CHECK(composite->Type == InputBinding::Shape::Composite2D);
		CHECK(composite->ToString() == "2D:Key/Up,Key/Down,Key/Left,Gamepad/LeftX+");

		const std::pair<const char*, const char*> refused[] = {
			{ "", "empty" },
			{ "Key/Nope", "'Nope' is no key" },
			{ "Mouse/Wheel", "no mouse button" },
			{ "Gamepad/Nope", "no gamepad button" },
			{ "Gamepad/LeftTrigger-", "no - half" },
			{ "Space", "Key/, Mouse/, Gamepad/" },
			{ "1D:Key/A", "two parts" },
			{ "1D:Key/A,Key/B,Key/C", "two parts" },
			{ "2D:Key/W,Key/S,Key/A", "four parts" },
			{ "1D:Gamepad/LeftX,Key/A", "cannot be part of a composite" },
			{ "2D:Gamepad/LeftStick,Key/S,Key/A,Key/D", "cannot be part of a composite" },
			{ "1D:Key/A,", "is no control" },
		};
		for (const auto& [text, reason] : refused)
		{
			INFO(text);
			std::string error;
			CHECK_FALSE(InputBinding::Parse(text, &error));
			CHECK_MESSAGE(error.find(reason) != std::string::npos, error);
		}
		std::string error;
		CHECK_FALSE(InputBinding::Parse(std::string(InputBinding::c_MaxTextBytes + 1, 'K'), &error));
		CHECK(error.find("at most 96 bytes") != std::string::npos);

		// Which bindings fit which kind.
		const InputBinding key = *InputBinding::Parse("Key/A");
		const InputBinding half = *InputBinding::Parse("Gamepad/LeftX+");
		const InputBinding axis = *InputBinding::Parse("Gamepad/LeftX");
		const InputBinding stick = *InputBinding::Parse("Gamepad/LeftStick");
		const InputBinding oneD = *InputBinding::Parse("1D:Key/A,Key/D");
		const InputBinding twoD = *InputBinding::Parse("2D:Key/W,Key/S,Key/A,Key/D");
		CHECK(key.FitsKind(InputActionKind::Button));
		CHECK(half.FitsKind(InputActionKind::Button));
		CHECK_FALSE(axis.FitsKind(InputActionKind::Button));
		CHECK_FALSE(oneD.FitsKind(InputActionKind::Button));
		CHECK(key.FitsKind(InputActionKind::Axis));
		CHECK(half.FitsKind(InputActionKind::Axis));
		CHECK(axis.FitsKind(InputActionKind::Axis));
		CHECK(oneD.FitsKind(InputActionKind::Axis));
		CHECK_FALSE(stick.FitsKind(InputActionKind::Axis));
		CHECK_FALSE(twoD.FitsKind(InputActionKind::Axis));
		CHECK(stick.FitsKind(InputActionKind::Vector2));
		CHECK(twoD.FitsKind(InputActionKind::Vector2));
		CHECK_FALSE(key.FitsKind(InputActionKind::Vector2));
		CHECK_FALSE(oneD.FitsKind(InputActionKind::Vector2));
	}

	TEST_CASE("Read refuses names, kinds and bindings that do not fit, with the reason")
	{
		const nlohmann::json jump = { { "Name", "Jump" }, { "Bindings", { "Key/Space" } } };
		const std::pair<nlohmann::json, const char*> refused[] = {
			{ nlohmann::json::array(), "an object with Maps" },
			{ { { "Maps", 1 } }, "Maps, an array" },
			{ { { "Maps", nlohmann::json::array() }, { "Extra", true } }, "'Extra' is not a member" },
			{ OneMap(nlohmann::json::array({ jump }), ""), "a name is empty" },
			{ OneMap(nlohmann::json::array({ jump }), " Gameplay"), "start or end with a space" },
			{ OneMap(nlohmann::json::array({ jump }), "Game/play"), "only letters, digits" },
			{ OneMap(nlohmann::json::array({ jump }), std::string(65, 'G').c_str()), "at most 64 characters" },
			{ OneMap(nlohmann::json::array({ { { "Name", "Jump!" } } })), "only letters, digits" },
			{ OneMap(nlohmann::json::array({ jump, { { "Name", "JUMP" } } })), "two actions named 'JUMP'" },
			{ { { "Maps", nlohmann::json::array({ { { "Name", "Gameplay" } }, { { "Name", "gameplay" } } }) } }, "Two maps are named 'gameplay'" },
			{ OneMap(nlohmann::json::array({ { { "Name", "Jump" }, { "Kind", "Trigger" } } })), "Kind must be Button, Axis or Vector2" },
			{ OneMap(nlohmann::json::array({ { { "Name", "Jump" }, { "DeadZone", 0.95 } } })), "DeadZone must be a number from 0 to 0.9" },
			{ OneMap(nlohmann::json::array({ { { "Name", "Jump" }, { "DeadZone", -0.1 } } })), "DeadZone must be a number" },
			{ OneMap(nlohmann::json::array({ { { "Name", "Jump" }, { "Speed", 1 } } })), "'Speed' is not a member of an action" },
			{ OneMap(nlohmann::json::array({ { { "Name", "Jump" }, { "Bindings", { "Key/Nope" } } } })), "binding 'Key/Nope': 'Nope' is no key" },
			{ OneMap(nlohmann::json::array({ { { "Name", "Jump" }, { "Bindings", { "Gamepad/LeftStick" } } } })), "a Button action cannot use 'Gamepad/LeftStick'" },
			{ OneMap(nlohmann::json::array({ { { "Name", "Steer" }, { "Kind", "Axis" }, { "Bindings", { "2D:Key/W,Key/S,Key/A,Key/D" } } } })),
				"an Axis action cannot use" },
			{ OneMap(nlohmann::json::array({ { { "Name", "Move" }, { "Kind", "Vector2" }, { "Bindings", { "Key/W" } } } })), "a Vector2 action cannot use 'Key/W'" },
			{ OneMap(nlohmann::json::array({ { { "Name", "Jump" }, { "Bindings", nlohmann::json::array({ "Key/A", "Key/B", "Key/C", "Key/D", "Key/E", "Key/F",
				"Key/G", "Key/H", "Key/I" }) } } })), "9 bindings (at most 8)" },
			{ OneMap(nlohmann::json::array({ { { "Name", "Submit" }, { "Kind", "Axis" } } }), "UI"), "The UI map's action Submit is a Button" },
		};
		for (const auto& [value, reason] : refused)
		{
			const std::string error = Refusal(value);
			CHECK_MESSAGE(error.find(reason) != std::string::npos, error, " for ", value.dump());
		}

		// The limits: maps, actions in the whole set (the UI map's six count), with room for the UI map.
		nlohmann::json tooManyMaps = { { "Maps", nlohmann::json::array() } };
		for (size_t map = 0; map <= InputActionSet::c_MaxMaps; map++)
			tooManyMaps["Maps"].push_back({ { "Name", MakeName('M', map) } });
		CHECK(Refusal(tooManyMaps).find("33 maps (at most 32)") != std::string::npos);
		tooManyMaps["Maps"].erase(tooManyMaps["Maps"].size() - 1);
		CHECK(Refusal(tooManyMaps).find("no room for the reserved map UI") != std::string::npos);

		nlohmann::json actions = nlohmann::json::array();
		for (size_t index = 0; index < InputActionSet::c_MaxActions - 5; index++)
			actions.push_back({ { "Name", MakeName('a', index) } });
		CHECK(Refusal(OneMap(actions)).find("no room for the UI map's action") != std::string::npos);
		while (actions.size() <= InputActionSet::c_MaxActions)
			actions.push_back({ { "Name", MakeName('b', actions.size()) } });
		CHECK(Refusal(OneMap(actions)).find("more than 256 actions") != std::string::npos);
	}

	TEST_CASE("The UI map is added when missing and cannot lose an action")
	{
		// A project without the section, and a set without the UI map, navigate with the UI map's defaults.
		CHECK(InputActionSet::MakeDefault().Maps.size() == 1);
		CheckValidSet(InputActionSet::MakeDefault());
		const std::optional<InputActionSet> gameplay = InputActionSet::FromJson(MakeGameplayActions());
		REQUIRE(gameplay);
		CheckValidSet(*gameplay);
		REQUIRE(gameplay->Maps.size() == 2);
		CHECK(gameplay->Maps[0] == InputActionSet::MakeDefault().Maps[0]);
		CHECK(gameplay->Maps[1].Name == "Gameplay");

		// A UI map that lost an action, renamed one's case or rebound one: the missing action comes back with its defaults,
		// the names are the game UI's, the bindings stay.
		const std::optional<InputActionSet> partial = InputActionSet::FromJson(OneMap(nlohmann::json::array({
			{ { "Name", "submit" }, { "Bindings", { "Key/J" } } },
			{ { "Name", "Inventory" }, { "Bindings", { "Key/I" } } } }), "ui"));
		REQUIRE(partial);
		CheckValidSet(*partial);
		const InputActionMap& ui = partial->Maps[0];
		CHECK(ui.Name == "UI");
		REQUIRE(ui.Actions.size() == 7);
		CHECK(ui.Actions[0].Name == "Submit");
		CHECK(ui.Actions[0].Bindings == ParseAll({ "Key/J" }));
		CHECK(ui.Actions[1].Name == "Inventory");
		CHECK(ui.Actions[2] == InputActionSet::GetDefaultUIActions()[1]);
		CHECK(partial->IsReservedAction({ 0, 0 }));
		CHECK_FALSE(partial->IsReservedAction({ 0, 1 }));
		CHECK(partial->IsReservedAction({ 0, 2 }));

		// The section's Read writes them out, so the stored form always has the UI map.
		SettingsRegistry registry;
		REQUIRE(registry.Register(InputActionSet::MakeSection()));
		const SettingsSection* section = registry.Find(InputActionSet::c_SectionId);
		REQUIRE(section);
		const std::optional<nlohmann::json> read = section->Validate(OneMap(nlohmann::json::array()), 1);
		REQUIRE(read);
		CHECK((*read)["Maps"].size() == 2);
		CHECK((*read)["Maps"][0]["Name"] == "UI");
		CHECK((*read)["Maps"][0]["Actions"].size() == 6);
		CHECK(section->GetDefault() == InputActionSet::MakeDefault().ToJson());
	}

	TEST_CASE("A gameplay action named Left coexists with UI/Left, and an ambiguous bare name is refused naming both")
	{
		const std::optional<InputActionSet> set = InputActionSet::FromJson(MakeGameplayActions());
		REQUIRE(set);
		std::string error;
		CHECK_FALSE(set->FindAction("Left", &error));
		CHECK(error == "'Left' is ambiguous: Gameplay/Left, UI/Left");
		REQUIRE(set->FindAction("Gameplay/Left"));
		CHECK(set->GetQualifiedName(*set->FindAction("gameplay/LEFT")) == "Gameplay/Left");
		CHECK(set->GetQualifiedName(*set->FindAction("UI/Left")) == "UI/Left");
		CHECK(set->GetQualifiedName(*set->FindAction("jump")) == "Gameplay/Jump");
		CHECK_FALSE(set->FindAction("Fly", &error));
		CHECK(error == "There is no action 'Fly'");
		CHECK_FALSE(set->FindAction("UI/Jump", &error));
		CHECK(error == "There is no action 'UI/Jump'");

		// The two Lefts read their own bindings, and so do the queries by name.
		ActionsProject project(MakeGameplayActions());
		Input::SimulateKey(Key::Q, true);
		Step();
		CHECK(InputActions::IsPressed("Gameplay/Left"));
		CHECK_FALSE(InputActions::IsDown("UI/Left"));
		CHECK_FALSE(InputActions::GetState("Left", &error));
		CHECK(error == "'Left' is ambiguous: Gameplay/Left, UI/Left");
		CHECK_FALSE(InputActions::IsDown("Left"));
		Input::SimulateKey(Key::Q, false);
		Input::SimulateKey(Key::Left, true);
		Step();
		CHECK(InputActions::IsReleased("Gameplay/Left"));
		CHECK(InputActions::IsPressed("UI/Left"));
		CHECK(InputActions::GetQualifiedName("steer") == std::optional<std::string>("Gameplay/Steer"));
	}

	TEST_CASE("A set at every limit fits its section bound and round-trips through the project file")
	{
		const nlohmann::json value = MakeLimitSet();
		const std::optional<InputActionSet> set = InputActionSet::FromJson(value);
		REQUIRE(set);
		CheckValidSet(*set);
		CHECK(set->Maps.size() == InputActionSet::c_MaxMaps);
		CHECK(set->GetActionCount() == InputActionSet::c_MaxActions);
		const size_t compact = JsonUtils::Dump(set->ToJson()).size();
		MESSAGE("A set at every limit: ", compact, " bytes compact");
		CHECK(compact <= SettingsSection::c_MaxValueBytes);

		SettingsRegistry registry;
		REQUIRE(registry.Register(InputActionSet::MakeSection()));
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("InputActionsLimits");
		std::string error;
		Ref<Project> project = Project::Create(directory, "Limits", &error);
		REQUIRE_MESSAGE(project, error);
		REQUIRE_MESSAGE(project->GetSettings().Set(InputActionSet::c_SectionId, value, registry, &error), error);
		REQUIRE_MESSAGE(project->Save(&error), error);
		Ref<Project> loaded = Project::Load(project->GetProjectFile(), &error, &registry);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetSettingsProblems().empty());
		const nlohmann::json* stored = loaded->GetSettings().FindSection(InputActionSet::c_SectionId);
		REQUIRE(stored);
		CHECK(InputActionSet::FromJson(*stored) == set);
	}

	TEST_CASE("Button, Axis and Vector2 values combine bindings as specified")
	{
		ActionsProject project(MakeGameplayActions());

		// Button: down while any binding is, on any gamepad; its value is 1 while down.
		Input::SimulateGamepadButton(2, GamepadButton::A, true);
		Step();
		CHECK(InputActions::IsPressed("Jump"));
		CHECK(InputActions::GetValue("Jump") == glm::vec2(1.0f, 0.0f));
		Input::SimulateKey(Key::Space, true);
		Input::SimulateGamepadButton(2, GamepadButton::A, false);
		Step();
		CHECK(InputActions::IsDown("Jump"));
		CHECK_FALSE(InputActions::IsPressed("Jump"));
		CHECK_FALSE(InputActions::IsReleased("Jump"));
		Input::SimulateKey(Key::Space, false);
		Step();
		CHECK(InputActions::IsReleased("Jump"));
		CHECK(InputActions::GetValue("Jump") == glm::vec2(0.0f));
		// A press and release within one input frame reports both.
		Input::SimulateKey(Key::Space, true);
		Input::SimulateKey(Key::Space, false);
		Step();
		const InputActionState tap = *InputActions::GetState("Jump");
		CHECK(tap.Pressed);
		CHECK(tap.Released);
		CHECK_FALSE(tap.Down);

		// Axis: the binding of the largest magnitude; a 1D composite is its positive part less its negative one.
		Input::SimulateKey(Key::D, true);
		Step();
		CHECK(InputActions::GetValue("Steer") == glm::vec2(1.0f, 0.0f));
		CHECK(InputActions::IsPressed("Steer"));
		Input::SimulateKey(Key::A, true);
		Step();
		CHECK(InputActions::GetValue("Steer").x == 0.0f);
		CHECK(InputActions::IsReleased("Steer"));
		Input::SimulateKey(Key::D, false);
		SetStick(0.575f, 0.0f);
		Step();
		CHECK(InputActions::GetValue("Steer").x == -1.0f);
		Input::SimulateKey(Key::A, false);
		Step();
		CHECK(InputActions::GetValue("Steer").x == doctest::Approx(0.5f));
		// A half axis gives its analog value to an Axis action (no dead zone here); a key gives 1.
		Input::SimulateGamepadAxis(1, GamepadAxis::RightTrigger, 0.3f);
		Step();
		CHECK(InputActions::GetValue("Throttle").x == doctest::Approx(0.3f));
		Input::SimulateKey(Key::T, true);
		Step();
		CHECK(InputActions::GetValue("Throttle").x == 1.0f);
		Input::SimulateKey(Key::T, false);

		// Vector2: the largest length; a 2D composite is normalized, so a diagonal of two keys has length 1.
		SetStick(0.0f, 0.0f);
		Input::SimulateKey(Key::W, true);
		Input::SimulateKey(Key::D, true);
		Step();
		const glm::vec2 diagonal = InputActions::GetValue("Move");
		CHECK(diagonal.x == doctest::Approx(std::sqrt(0.5f)));
		CHECK(diagonal.y == doctest::Approx(std::sqrt(0.5f)));
		CHECK(InputActions::IsDown("Move"));
		Input::SimulateKey(Key::W, false);
		Input::SimulateKey(Key::D, false);
		SetStick(-0.3f, 0.95f, 3);
		Step();
		const glm::vec2 stick = InputActions::GetValue("Move");
		CHECK(stick.x < 0.0f);
		CHECK(stick.y > 0.0f); // +Y up, as the stick reports it
		CHECK(glm::length(stick) <= 1.0f);
	}

	TEST_CASE("Dead zones are radial for sticks and rescaled")
	{
		ActionsProject project(MakeGameplayActions());
		// Axis: within the zone reads 0, beyond it the rest is rescaled to reach 1.
		SetStick(0.14f, 0.0f);
		Step();
		CHECK(InputActions::GetValue("Steer").x == 0.0f);
		SetStick(0.575f, 0.0f);
		Step();
		CHECK(InputActions::GetValue("Steer").x == doctest::Approx(0.5f));
		SetStick(-1.0f, 0.0f);
		Step();
		CHECK(InputActions::GetValue("Steer").x == doctest::Approx(-1.0f));

		// Vector2: by the stick's length, so a small diagonal whose axes are each within the zone moves.
		SetStick(0.12f, 0.12f);
		Step();
		const glm::vec2 small = InputActions::GetValue("Move");
		CHECK(glm::length(small) > 0.0f);
		CHECK(small.x == doctest::Approx(small.y));
		SetStick(0.1f, 0.0f);
		Step();
		CHECK(InputActions::GetValue("Move") == glm::vec2(0.0f));
		SetStick(0.3f, 0.4f); // Length 0.5: (0.5 - 0.15) / 0.85 along its direction
		Step();
		const glm::vec2 half = InputActions::GetValue("Move");
		CHECK(glm::length(half) == doctest::Approx(0.35f / 0.85f));
		CHECK(half.x / half.y == doctest::Approx(0.75f));
		SetStick(0.6f, 0.8f);
		Step();
		CHECK(glm::length(InputActions::GetValue("Move")) == doctest::Approx(1.0f));
	}

	TEST_CASE("Half axes press at 0.5 and release at 0.35")
	{
		ActionsProject project(MakeGameplayActions());
		const auto trigger = [](float value)
		{
			Input::SimulateGamepadAxis(0, GamepadAxis::RightTrigger, value);
			Step();
		};
		trigger(0.45f);
		CHECK_FALSE(InputActions::IsDown("Jump"));
		trigger(0.55f);
		CHECK(InputActions::IsPressed("Jump"));
		trigger(0.4f);
		CHECK(InputActions::IsDown("Jump"));
		CHECK_FALSE(InputActions::IsPressed("Jump"));
		trigger(0.3f);
		CHECK(InputActions::IsReleased("Jump"));

		// Evaluated every input frame, not when queried: a press in a frame nobody asked about holds the action in the next
		// frame, below the press threshold.
		trigger(0.6f);
		trigger(0.4f);
		CHECK(InputActions::IsDown("Jump"));

		// Axis and Vector2 actions are down beyond 0.5 of their value (after the dead zone) with the same hysteresis.
		SetStick(0.6f, 0.0f); // 0.53 after the dead zone
		Step();
		CHECK(InputActions::IsPressed("Steer"));
		SetStick(0.5f, 0.0f); // 0.41
		Step();
		CHECK(InputActions::IsDown("Steer"));
		SetStick(0.4f, 0.0f); // 0.29
		Step();
		CHECK(InputActions::IsReleased("Steer"));
	}

	TEST_CASE("Pressed and Released last one input frame and survive suspended frames")
	{
		ActionsProject project(MakeGameplayActions());
		Input::SimulateKey(Key::Space, true);
		Step();
		CHECK(InputActions::IsPressed("Jump"));
		// The game does not update while paused: what it has not seen yet waits, whatever the frames meanwhile.
		Input::SetSuspended(true);
		for (int frame = 0; frame < 3; frame++)
		{
			Step();
			CHECK(InputActions::IsPressed("Jump"));
		}
		Input::SetSuspended(false);
		Step();
		CHECK(InputActions::IsDown("Jump"));
		CHECK_FALSE(InputActions::IsPressed("Jump"));

		// A release while suspended is reported in the game's next update.
		Input::SetSuspended(true);
		Input::SimulateKey(Key::Space, false);
		Step();
		Step();
		CHECK(InputActions::IsDown("Jump"));
		CHECK_FALSE(InputActions::IsReleased("Jump"));
		Input::SetSuspended(false);
		Step();
		CHECK(InputActions::IsReleased("Jump"));
		CHECK_FALSE(InputActions::IsDown("Jump"));
		Step();
		CHECK_FALSE(InputActions::IsReleased("Jump"));

		// Updates without a new input frame change nothing.
		Input::SimulateKey(Key::Space, true);
		Step();
		InputActions::Update();
		InputActions::Update();
		CHECK(InputActions::IsPressed("Jump"));
	}

	TEST_CASE("Disabled maps' actions read as up")
	{
		nlohmann::json actions = MakeGameplayActions();
		actions["Maps"].push_back({ { "Name", "Vehicle" }, { "Enabled", false }, { "Actions", nlohmann::json::array({ { { "Name", "Horn" },
			{ "Bindings", { "Key/H" } } } }) } });
		ActionsProject project(actions);
		CHECK(InputActions::IsMapEnabled("gameplay") == std::optional<bool>(true));
		CHECK(InputActions::IsMapEnabled("Vehicle") == std::optional<bool>(false));
		CHECK_FALSE(InputActions::IsMapEnabled("Flying"));
		std::string error;
		CHECK_FALSE(InputActions::SetMapEnabled("Flying", true, &error));
		CHECK(error == "There is no action map 'Flying'");

		Input::SimulateKey(Key::H, true);
		Input::SimulateKey(Key::Space, true);
		Step();
		CHECK_FALSE(InputActions::IsDown("Horn"));
		CHECK(InputActions::IsPressed("Jump"));
		// Disabling a map takes effect at once; enabling it again presses what is held.
		CHECK(InputActions::SetMapEnabled("Gameplay", false));
		CHECK_FALSE(InputActions::IsDown("Jump"));
		CHECK_FALSE(InputActions::IsPressed("Jump"));
		CHECK(InputActions::GetValue("Jump") == glm::vec2(0.0f));
		Step();
		CHECK_FALSE(InputActions::IsDown("Jump"));
		CHECK_FALSE(InputActions::IsReleased("Jump"));
		CHECK(InputActions::SetMapEnabled("Gameplay", true));
		CHECK(InputActions::SetMapEnabled("Vehicle", true));
		Step();
		CHECK(InputActions::IsPressed("Jump"));
		CHECK(InputActions::IsPressed("Horn"));
		// A new session starts with the maps as the set says.
		InputActions::ResetSession();
		CHECK(InputActions::IsMapEnabled("Vehicle") == std::optional<bool>(false));
	}

	TEST_CASE("Keyboard capture hides key bindings")
	{
		ActionsProject project(MakeGameplayActions());
		Input::SetKeyboardCapture(true); // A game UI text field being edited
		Input::SimulateKey(Key::Space, true);
		Input::SimulateKey(Key::Enter, true);
		Step();
		CHECK_FALSE(InputActions::IsDown("Jump"));
		CHECK_FALSE(InputActions::IsDown("UI/Submit"));
		Input::SimulateGamepadButton(0, GamepadButton::A, true);
		Step();
		CHECK(InputActions::IsPressed("Jump"));
		Input::SimulateGamepadButton(0, GamepadButton::A, false);
		Input::SetKeyboardCapture(false);
		Step();
		CHECK(InputActions::IsDown("Jump"));
		CHECK(InputActions::IsDown("UI/Submit")); // Enter, held through the capture (A, also Submit's, held it down meanwhile)

		// Pointer capture hides mouse bindings the same way.
		project.Set(OneMap(nlohmann::json::array({ { { "Name", "Fire" }, { "Bindings", { "Mouse/Left" } } } })));
		Input::SetPointerCapture(true);
		Input::SimulateMouseButton(Mouse::ButtonLeft, true);
		Step();
		CHECK_FALSE(InputActions::IsDown("Fire"));
		Input::SetPointerCapture(false);
		Step();
		CHECK(InputActions::IsPressed("Fire"));
	}

	TEST_CASE("Players' bindings replace an action's list and reset restores it")
	{
		ActionsProject project(MakeGameplayActions());
		const std::vector<InputBinding> rebound = ParseAll({ "Key/J", "Gamepad/Y" });
		std::string error;
		REQUIRE_MESSAGE(InputActions::SetBindings("Jump", rebound, &error), error);
		CHECK(InputActions::GetBindings("Gameplay/Jump") == rebound);
		CHECK(InputActions::GetPlayerBindings().Actions.size() == 1);
		CHECK(InputActions::GetPlayerBindings().Actions.at("Gameplay/Jump") == rebound);
		Input::SimulateKey(Key::Space, true);
		Step();
		CHECK_FALSE(InputActions::IsDown("Jump"));
		Input::SimulateKey(Key::J, true);
		Step();
		CHECK(InputActions::IsPressed("Jump"));

		// Refused, changing nothing: a binding the kind cannot use, too many, an unknown action.
		CHECK_FALSE(InputActions::SetBindings("Jump", ParseAll({ "Gamepad/LeftStick" }), &error));
		CHECK(error == "Gameplay/Jump is a Button action, which cannot use 'Gamepad/LeftStick'");
		CHECK_FALSE(InputActions::SetBindings("Jump", ParseAll({ "Key/A", "Key/B", "Key/C", "Key/D", "Key/E", "Key/F", "Key/G", "Key/H", "Key/I" }), &error));
		CHECK(error.find("at most 8 bindings") != std::string::npos);
		CHECK_FALSE(InputActions::SetBindings("Fly", rebound, &error));
		CHECK(error == "There is no action 'Fly'");
		CHECK(InputActions::GetBindings("Jump") == rebound);

		// Rebinding while held keeps the action down; reset restores the project's list.
		REQUIRE(InputActions::SetBindings("UI/Submit", ParseAll({ "Key/K" })));
		REQUIRE(InputActions::ResetBindings("jump"));
		CHECK(InputActions::GetBindings("Jump") == InputActionSet::FromJson(MakeGameplayActions())->Maps[1].Actions[0].Bindings);
		Step();
		CHECK(InputActions::IsDown("Jump")); // Space is held
		CHECK_FALSE(InputActions::IsPressed("Jump"));
		CHECK(InputActions::GetPlayerBindings().Actions.size() == 1);
		REQUIRE(InputActions::ResetBindings(""));
		CHECK(InputActions::GetPlayerBindings().Actions.empty());
		CHECK(InputActions::GetBindings("UI/Submit") == InputActionSet::GetDefaultUIActions()[0].Bindings);
		CHECK_FALSE(InputActions::ResetBindings("Fly", &error));
	}

	TEST_CASE("The bindings file round-trips, drops unknown actions and invalid bindings with one warning, and a garbage file changes nothing")
	{
		ActionsProject project(MakeGameplayActions());
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("InputBindings");
		std::string error;
		// Without a save directory there is nowhere to keep them.
		CHECK_FALSE(InputActions::SaveBindings(&error));
		CHECK(error.find("no save directory") != std::string::npos);

		SaveDirectory saves(directory);
		EngineServices::ScopedProvider<IGameSaves> savesProvider(saves);
		REQUIRE(InputActions::SetBindings("Gameplay/Jump", ParseAll({ "Key/J" })));
		REQUIRE(InputActions::SetBindings("Move", ParseAll({ "2D:Key/I,Key/K,Key/J,Key/L", "Gamepad/RightStick" })));
		REQUIRE(InputActions::SetBindings("Steer", {})); // A player may unbind an action
		REQUIRE_MESSAGE(InputActions::SaveBindings(&error), error);
		const std::filesystem::path file = directory / InputPlayerBindings::c_FileName;
		REQUIRE(FileSystem::Exists(file));
		const InputPlayerBindings saved = InputActions::GetPlayerBindings();
		CHECK(FileSystem::ReadText(file) == saved.Write());
		// The file is the engine's, outside the game's save files.
		CHECK_FALSE(SaveFiles::ValidateName("InputBindings"));
		CHECK(SaveFiles::List(directory).empty());

		// A new session starts with the project's bindings until it loads the players'.
		InputActions::ResetSession();
		CHECK(InputActions::GetPlayerBindings().Actions.empty());
		{
			Tests::LogCapture log;
			InputActions::LoadBindings();
			CHECK(log.GetEntries().empty());
		}
		CHECK(InputActions::GetPlayerBindings() == saved);
		CHECK(InputActions::GetBindings("Steer")->empty());

		// The game changed since: an action it no longer has and a binding its action's kind cannot use are dropped, the rest
		// applies, and one warning says so.
		const std::string changed = R"({"Strata": {"Format": "InputBindings", "Version": 1}, "Actions": {
			"Gameplay/Jump": ["Key/U", "Gamepad/LeftStick"], "Gameplay/Fly": ["Key/F"], "Move": ["Gamepad/LeftStick"],
			"Gameplay/Steer": ["Key/Nope"], "UI/Submit": ["Key/K"]}})";
		REQUIRE(FileSystem::WriteText(file, changed));
		{
			Tests::LogCapture log;
			InputActions::LoadBindings();
			const std::vector<LogEntry> entries = log.GetEntries();
			REQUIRE(entries.size() == 1);
			CHECK(entries[0].Message.find("4 dropped") != std::string::npos);
		}
		const InputPlayerBindings loaded = InputActions::GetPlayerBindings();
		CHECK(loaded.Actions.size() == 2);
		CHECK(loaded.Actions.at("Gameplay/Jump") == ParseAll({ "Key/U" }));
		CHECK(loaded.Actions.at("UI/Submit") == ParseAll({ "Key/K" }));
		CHECK(InputActions::GetBindings("Steer") == InputActionSet::FromJson(MakeGameplayActions())->Maps[1].Actions[1].Bindings);

		// Garbage, another format, a later version or an oversized file: ignored with a warning, the project's bindings apply.
		const std::string garbage[] = {
			"not json",
			R"({"Strata": {"Format": "Scene", "Version": 1}, "Actions": {}})",
			R"({"Strata": {"Format": "InputBindings", "Version": 2}, "Actions": {}})",
			R"({"Strata": {"Format": "InputBindings", "Version": 1}, "Actions": []})",
			std::string(InputPlayerBindings::c_MaxFileBytes + 1, ' '),
		};
		for (const std::string& text : garbage)
		{
			INFO(text.substr(0, 80));
			REQUIRE(FileSystem::WriteText(file, text));
			Tests::LogCapture log;
			InputActions::LoadBindings();
			CHECK(log.GetEntries().size() == 1);
			CHECK(InputActions::GetPlayerBindings().Actions.empty());
		}
	}

	TEST_CASE("A capture returns the first control and no action sees it")
	{
		ActionsProject project(MakeGameplayActions());
		CHECK_FALSE(InputActions::BeginCapture({ false, false, false }));
		CHECK_FALSE(InputActions::IsCapturing());

		// Space is held when the capture begins: not a press, so not what it takes.
		Input::SimulateKey(Key::Space, true);
		Step();
		CHECK(InputActions::IsDown("Jump"));
		REQUIRE(InputActions::BeginCapture({}));
		CHECK(InputActions::IsCapturing());
		CHECK_FALSE(InputActions::IsDown("Jump")); // Every action reads as up while it listens
		Step();
		CHECK(InputActions::IsCapturing());
		CHECK_FALSE(InputActions::TakeCapturedBinding());
		CHECK_FALSE(InputActions::IsDown("Jump"));

		Input::SimulateKey(Key::Escape, true);
		Step();
		CHECK_FALSE(InputActions::IsCapturing());
		CHECK(InputActions::TakeCapturedBinding() == std::optional<std::string>("Key/Escape"));
		CHECK_FALSE(InputActions::TakeCapturedBinding());
		// The press that was captured closes no menu.
		CHECK_FALSE(InputActions::IsPressed("UI/Cancel"));
		CHECK_FALSE(InputActions::IsDown("UI/Cancel"));
		// Space, held through the capture, is the game's again.
		CHECK(InputActions::IsPressed("Jump"));

		// The devices it listens to: a gamepad-only capture ignores keys and takes a stick pushed beyond 0.5 as its half.
		REQUIRE(InputActions::BeginCapture({ false, false, true }));
		Input::SimulateKey(Key::E, true);
		Step();
		CHECK(InputActions::IsCapturing());
		SetStick(-0.7f, 0.0f, 1);
		Step();
		CHECK(InputActions::TakeCapturedBinding() == std::optional<std::string>("Gamepad/LeftX-"));
		REQUIRE(InputActions::BeginCapture({ false, true, false }));
		Input::SimulateMouseButton(Mouse::ButtonRight, true);
		Step();
		CHECK(InputActions::TakeCapturedBinding() == std::optional<std::string>("Mouse/Right"));

		// Cancelled, it takes nothing.
		REQUIRE(InputActions::BeginCapture({}));
		InputActions::CancelCapture();
		Input::SimulateKey(Key::F, true);
		Step();
		CHECK_FALSE(InputActions::IsCapturing());
		CHECK_FALSE(InputActions::TakeCapturedBinding());
	}

	TEST_CASE("The captured control stays up for actions until it is released")
	{
		ActionsProject project(MakeGameplayActions());
		REQUIRE(InputActions::BeginCapture({}));
		Input::SimulateKey(Key::Space, true);
		Step();
		REQUIRE(InputActions::TakeCapturedBinding() == std::optional<std::string>("Key/Space"));
		for (int frame = 0; frame < 3; frame++)
		{
			Step();
			CHECK_FALSE(InputActions::IsDown("Jump"));
		}
		// Other bindings of the action still work meanwhile.
		Input::SimulateGamepadButton(0, GamepadButton::A, true);
		Step();
		CHECK(InputActions::IsPressed("Jump"));
		Input::SimulateGamepadButton(0, GamepadButton::A, false);
		Step();
		CHECK(InputActions::IsReleased("Jump"));
		Input::SimulateKey(Key::Space, false);
		Step();
		CHECK_FALSE(InputActions::IsReleased("Jump"));
		Input::SimulateKey(Key::Space, true);
		Step();
		CHECK(InputActions::IsPressed("Jump"));

		// A captured half axis keeps its axis at rest for actions until it falls below 0.35.
		REQUIRE(InputActions::BeginCapture({}));
		SetStick(0.9f, 0.0f);
		Step();
		REQUIRE(InputActions::TakeCapturedBinding() == std::optional<std::string>("Gamepad/LeftX+"));
		Step();
		CHECK(InputActions::GetValue("Steer").x == 0.0f);
		SetStick(0.2f, 0.0f);
		Step();
		SetStick(0.9f, 0.0f);
		Step();
		CHECK(InputActions::GetValue("Steer").x > 0.5f);
	}

	TEST_CASE("A capture runs while input frames are suspended")
	{
		ActionsProject project(MakeGameplayActions());
		// Play is paused: input frames do not advance, device events still arrive.
		Input::SetSuspended(true);
		REQUIRE(InputActions::BeginCapture({}));
		Input::ProcessKey(Key::Space, true);
		Step();
		CHECK(InputActions::TakeCapturedBinding() == std::optional<std::string>("Key/Space"));
		// Play resumes with the key still held: the action does not see the press that was captured.
		Input::SetSuspended(false);
		Step();
		CHECK_FALSE(InputActions::IsDown("Jump"));
		Input::ProcessKey(Key::Space, false);
		Step();
		Input::ProcessKey(Key::Space, true);
		Step();
		CHECK(InputActions::IsPressed("Jump"));
	}

	TEST_CASE("A new project version reparses the set at the next update")
	{
		// Without a provider the default set applies.
		Input::Reset();
		CHECK(InputActions::GetSet() == InputActionSet::MakeDefault());
		Input::SimulateKey(Key::Enter, true);
		Step();
		CHECK(InputActions::IsPressed("UI/Submit"));
		CHECK(InputActions::IsPressed("Submit"));
		Input::Reset();

		ActionsProject project(MakeGameplayActions());
		Input::SimulateKey(Key::Space, true);
		Step();
		CHECK(InputActions::IsPressed("Jump"));
		const uint64_t version = InputActions::GetVersion();

		// The editor rebinds Jump while the game runs: the next update reads the new set, and the held action stays down.
		nlohmann::json rebound = MakeGameplayActions();
		rebound["Maps"][0]["Actions"][0]["Bindings"] = { "Key/Space", "Key/J" };
		project.Set(rebound);
		CHECK(InputActions::GetBindings("Jump")->size() == 3);
		Step();
		CHECK(InputActions::GetVersion() != version);
		CHECK(InputActions::GetBindings("Jump") == ParseAll({ "Key/Space", "Key/J" }));
		CHECK(InputActions::IsDown("Jump"));
		CHECK_FALSE(InputActions::IsPressed("Jump"));
		Input::SimulateKey(Key::Space, false);
		Input::SimulateKey(Key::J, true);
		Step();
		CHECK(InputActions::IsDown("Jump"));

		// A change that leaves the set as it was rebuilds nothing.
		const uint64_t unchanged = InputActions::GetVersion();
		project.Set(rebound);
		Step();
		CHECK(InputActions::GetVersion() == unchanged);
	}

	TEST_CASE("A simulated action is down from the next input frame, whatever its bindings")
	{
		ActionsProject project(MakeGameplayActions());
		std::string error;
		CHECK_FALSE(InputActions::SimulateAction("Fly", true, glm::vec2(1.0f, 0.0f), &error));
		CHECK(error == "There is no action 'Fly'");
		REQUIRE(InputActions::SimulateAction("Jump", true));
		REQUIRE(InputActions::SimulateAction("Move", true, glm::vec2(0.0f, -1.0f)));
		REQUIRE(InputActions::SimulateAction("Steer", true, glm::vec2(4.0f, 0.0f)));
		CHECK_FALSE(InputActions::IsDown("Jump"));
		Step();
		CHECK(InputActions::IsPressed("Jump"));
		CHECK(InputActions::GetValue("Move") == glm::vec2(0.0f, -1.0f));
		CHECK(InputActions::GetValue("Steer") == glm::vec2(1.0f, 0.0f)); // Clamped
		Step();
		CHECK(InputActions::IsDown("Jump"));
		CHECK_FALSE(InputActions::IsPressed("Jump"));
		InputActions::ReleaseAllSimulated();
		Step();
		CHECK(InputActions::IsReleased("Jump"));
		CHECK(InputActions::GetValue("Move") == glm::vec2(0.0f));
		// A tap within one input frame reports the press and the release.
		REQUIRE(InputActions::SimulateAction("Jump", true));
		REQUIRE(InputActions::SimulateAction("Jump", false));
		Step();
		CHECK(InputActions::IsPressed("Jump"));
		CHECK(InputActions::IsReleased("Jump"));
		REQUIRE(InputActions::SimulateAction("Jump", true));
		InputActions::ClearSimulated();
		Step();
		CHECK_FALSE(InputActions::IsDown("Jump"));
	}
}

// Damaged binding texts, section values and bindings files never crash the readers, and what they accept is valid (a sample
// of the offsets in the core shard, all of them in StrataTests.Fuzz: Tests::SelectSweepOffsets).
TEST_SUITE("Core.InputActions.Sweep")
{
	TEST_CASE("Truncated or flipped binding texts, section values and bindings files are refused or valid")
	{
		for (const std::string& text : MakeEveryBindingText())
		{
			for (size_t length = 0; length < text.size(); length++)
			{
				const std::optional<InputBinding> binding = InputBinding::Parse(std::string_view(text).substr(0, length));
				if (binding)
					CHECK(InputBinding::Parse(binding->ToString()) == binding);
			}
			for (size_t offset = 0; offset < text.size(); offset++)
			{
				for (const uint8_t flip : { uint8_t(0xFF), uint8_t(0x20), uint8_t(0x01) })
				{
					std::string damaged = text;
					damaged[offset] = static_cast<char>(static_cast<uint8_t>(damaged[offset]) ^ flip);
					const std::optional<InputBinding> binding = InputBinding::Parse(damaged);
					if (binding)
						CHECK(InputBinding::Parse(binding->ToString()) == binding);
				}
			}
		}

		const std::optional<InputActionSet> gameplay = InputActionSet::FromJson(MakeGameplayActions());
		REQUIRE(gameplay);
		const std::string value = JsonUtils::Dump(gameplay->ToJson());
		const auto checkValue = [](std::string_view text)
		{
			const std::optional<nlohmann::json> json = JsonUtils::Parse(text);
			if (!json)
				return;
			std::string error;
			const std::optional<InputActionSet> set = InputActionSet::FromJson(*json, &error);
			if (set)
				CheckValidSet(*set);
			else
				CHECK_FALSE(error.empty());
		};
		for (const size_t length : Tests::SelectSweepOffsets(value.size(), {}, 0, 128))
			checkValue(std::string_view(value).substr(0, length));
		for (const size_t offset : Tests::SelectSweepOffsets(value.size(), {}, 0, 128))
		{
			for (const uint8_t flip : { uint8_t(0xFF), uint8_t(0x20), uint8_t(0x01) })
			{
				std::string damaged = value;
				damaged[offset] = static_cast<char>(static_cast<uint8_t>(damaged[offset]) ^ flip);
				checkValue(damaged);
			}
		}

		InputPlayerBindings players;
		players.Actions["Gameplay/Jump"] = ParseAll({ "Key/J", "Gamepad/Y" });
		players.Actions["Gameplay/Move"] = ParseAll({ "2D:Key/I,Key/K,Key/J,Key/L" });
		players.Actions["UI/Cancel"] = ParseAll({ "Key/Backspace" });
		const std::string file = players.Write();
		REQUIRE(InputPlayerBindings::Read(file, *gameplay) == players);
		const auto checkFile = [&gameplay](std::string_view text)
		{
			std::string error;
			const std::optional<InputPlayerBindings> read = InputPlayerBindings::Read(text, *gameplay, nullptr, &error);
			if (!read)
			{
				CHECK_FALSE(error.empty());
				return;
			}
			for (const auto& [name, bindings] : read->Actions)
			{
				const std::optional<InputActionLocation> location = gameplay->FindAction(name);
				REQUIRE(location);
				CHECK(bindings.size() <= InputActionSet::c_MaxBindings);
				for (const InputBinding& binding : bindings)
					CHECK(binding.FitsKind(gameplay->GetAction(*location).Kind));
			}
		};
		for (const size_t length : Tests::SelectSweepOffsets(file.size(), {}, 0, 128))
			checkFile(std::string_view(file).substr(0, length));
		for (const size_t offset : Tests::SelectSweepOffsets(file.size(), {}, 0, 128))
		{
			for (const uint8_t flip : { uint8_t(0xFF), uint8_t(0x20), uint8_t(0x01) })
			{
				std::string damaged = file;
				damaged[offset] = static_cast<char>(static_cast<uint8_t>(damaged[offset]) ^ flip);
				checkFile(damaged);
			}
		}
	}
}
