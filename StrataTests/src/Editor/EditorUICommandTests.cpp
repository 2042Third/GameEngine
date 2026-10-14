#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/UIRectEdit.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Timestep.h>
#include <Strata/GameUI/GameUIComponents.h>
#include <Strata/Input/Input.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>

#include <optional>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	struct UICommandHarness
	{
		EditorContext Context;
		EditorCommandRegistry Commands;

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return Tests::RunCommandToCompletion(Context, Commands, name, parameters);
		}

		nlohmann::json SceneSnapshot() const
		{
			return SceneSerializer::Serialize(*Context.GetEditScene())["Scene"]["Entities"];
		}

		EditorCommandResult Fail(std::string_view name, const nlohmann::json& parameters)
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			REQUIRE_FALSE(result.IsPending());
			CHECK_FALSE(result.Success);
			return result;
		}

		Entity Find(const nlohmann::json& id)
		{
			return Context.GetActiveScene()->GetEntityByUUID(*UUIDFromJson(id));
		}
	};

	// A project that plays (input.click and input.type need a running game), with frames run as the application runs them:
	// input frame, editor update, then the pending commands.
	struct PlayingUIHarness
	{
		EditorContext Context { EditorContextSpecification { false, false } };
		EditorCommandRegistry Commands;
		EditorCommandRunner Runner;

		PlayingUIHarness()
		{
			Input::Reset();
			const std::filesystem::path directory = Tests::CreateTemporaryDirectory("EditorUIInput") / "UI Game";
			Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "UI Game" } });
		}

		~PlayingUIHarness()
		{
			Context.Stop();
			Input::Reset();
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return Tests::RunCommandToCompletion(Context, Commands, name, parameters);
		}

		// Runs a command through the runner, a frame at a time, until it answers.
		EditorCommandResult RunFrames(std::string_view name, const nlohmann::json& parameters)
		{
			std::optional<EditorCommandResult> result;
			Runner.Run(Context, Commands, name, parameters, [&result](const EditorCommandResult& answer) { result = answer; });
			for (int frame = 0; frame < 120 && !result; frame++)
				Frame();
			REQUIRE(result.has_value());
			return *result;
		}

		void Frame()
		{
			Input::BeginFrame();
			Context.Update(Timestep(1.0f / 60.0f));
			Runner.Update(Context);
		}

		Entity Find(const nlohmann::json& id)
		{
			return Context.GetActiveScene()->GetEntityByUUID(*UUIDFromJson(id));
		}
	};

	glm::vec4 ToRect(const nlohmann::json& rect)
	{
		REQUIRE(rect.is_array());
		REQUIRE(rect.size() == 4);
		return { rect[0].get<float>(), rect[1].get<float>(), rect[2].get<float>(), rect[3].get<float>() };
	}

}

TEST_SUITE("Editor.UICommands")
{
	TEST_CASE("Adding a UI component adds the rect it requires in the same undo step")
	{
		UICommandHarness harness;
		const std::string id = harness.Run("entity.create", { { "name", "Badge" } })["id"].get<std::string>();
		const nlohmann::json before = harness.SceneSnapshot();

		harness.Run("component.add", { { "entity", id }, { "component", "UIImage" }, { "values", { { "CornerRadius", 4.0 } } } });
		const nlohmann::json badge = harness.Run("entity.get", { { "entity", id } });
		CHECK(badge["components"].contains("UIRect"));
		CHECK(badge["components"]["UIImage"]["CornerRadius"] == 4.0);
		const nlohmann::json added = harness.SceneSnapshot();

		harness.Run("edit.undo");
		CHECK(harness.SceneSnapshot() == before);
		harness.Run("edit.redo");
		CHECK(harness.SceneSnapshot() == added);

		// entity.create with a UI component adds the rect too.
		const std::string label = harness.Run("entity.create", { { "name", "Label" }, { "components", { { "UIText", { { "Text", "Hi" } } } } } })["id"].get<std::string>();
		CHECK(harness.Run("entity.get", { { "entity", label } })["components"].contains("UIRect"));
	}

	TEST_CASE("ui.create makes each element ready to use in one undo step, under a new canvas when its parent is outside one")
	{
		UICommandHarness harness;
		const nlohmann::json empty = harness.SceneSnapshot();
		const nlohmann::json button = harness.Run("ui.create", { { "type", "button" }, { "text", "Play" } });
		REQUIRE(button["created"].size() == 3); // Canvas, button, label
		CHECK(button["created"][0] == button["canvas"]);
		const Entity canvas = harness.Find(button["canvas"]);
		CHECK(canvas.HasComponent<UICanvasComponent>());
		CHECK(canvas.GetName() == "Canvas");
		const Entity play = harness.Find(button["entity"]);
		CHECK(play.GetName() == "Button");
		CHECK(play.GetParent() == canvas);
		CHECK((play.HasComponent<UIButtonComponent>() && play.HasComponent<UISelectableComponent>() && play.HasComponent<UIImageComponent>()));
		CHECK(harness.Find(button["parts"]["label"]).GetComponent<UITextComponent>().Text == "Play");
		// At the default screen (1920x1080) the 1920x1080 canvas has one unit per pixel: a 220x56 button in the middle.
		CHECK(ToRect(button["rect"]) == glm::vec4(850.0f, 512.0f, 220.0f, 56.0f));
		harness.Run("edit.undo");
		CHECK(harness.SceneSnapshot() == empty);
		harness.Run("edit.redo");

		// Under a canvas no new one; under an entity outside canvases a new one.
		const nlohmann::json toggle = harness.Run("ui.create", { { "type", "toggle" }, { "parent", button["canvas"] }, { "name", "Sound" } });
		CHECK(toggle["canvas"] == button["canvas"]);
		CHECK(toggle["created"].size() == 4); // Toggle, box, checkmark, label
		const Entity sound = harness.Find(toggle["entity"]);
		CHECK(sound.GetComponent<UIToggleComponent>().Checkmark == harness.Find(toggle["parts"]["checkmark"]).GetUUID());
		CHECK(sound.GetComponent<UISelectableComponent>().TargetImage == harness.Find(toggle["parts"]["background"]).GetUUID());
		const nlohmann::json slider = harness.Run("ui.create", { { "type", "slider" }, { "parent", toggle["entity"] } });
		CHECK(slider["canvas"] == button["canvas"]);
		const Entity sliderEntity = harness.Find(slider["entity"]);
		CHECK(sliderEntity.GetComponent<UISliderComponent>().Fill == harness.Find(slider["parts"]["fill"]).GetUUID());
		CHECK(sliderEntity.GetComponent<UISliderComponent>().Handle == harness.Find(slider["parts"]["handle"]).GetUUID());
		const std::string plain = harness.Run("entity.create", { { "name", "Plain" } })["id"].get<std::string>();
		const nlohmann::json image = harness.Run("ui.create", { { "type", "image" }, { "parent", plain } });
		CHECK(image["canvas"] != button["canvas"]);
		CHECK_FALSE(harness.Find(image["canvas"]).GetParent());
		// An input field shows its text through its label, and its placeholder (the text given) while the text is empty.
		const nlohmann::json name = harness.Run("ui.create", { { "type", "inputField" }, { "parent", button["canvas"] }, { "text", "Your name" } });
		const Entity nameField = harness.Find(name["entity"]);
		CHECK(nameField.GetComponent<UIInputFieldComponent>().TextElement == harness.Find(name["parts"]["label"]).GetUUID());
		CHECK(nameField.GetComponent<UIInputFieldComponent>().Placeholder == harness.Find(name["parts"]["placeholder"]).GetUUID());
		CHECK(harness.Find(name["parts"]["placeholder"]).GetComponent<UITextComponent>().Text == "Your name");
		CHECK((nameField.HasComponent<UISelectableComponent>() && nameField.HasComponent<UIMaskComponent>()));
		const nlohmann::json nameLayout = harness.Run("ui.layout", { { "entity", name["entity"] } });
		CHECK(nameLayout["elements"][0]["text"] == "");
		CHECK(nameLayout["elements"][0]["editing"] == false);

		// Every type, each one undo step.
		for (const char* type : { "canvas", "panel", "image", "text", "button", "toggle", "slider", "inputField", "horizontalLayout", "verticalLayout", "gridLayout" })
		{
			CAPTURE(type);
			const nlohmann::json before = harness.SceneSnapshot();
			const nlohmann::json created = harness.Run("ui.create", { { "type", type }, { "parent", button["canvas"] } });
			CHECK(harness.Find(created["entity"]).HasComponent<UIRectComponent>());
			CHECK(created["rect"].is_array());
			harness.Run("edit.undo");
			CHECK(harness.SceneSnapshot() == before);
		}
		CHECK(harness.Find(harness.Run("ui.create", { { "type", "verticalLayout" }, { "parent", button["canvas"] } })["entity"]).GetComponent<UILayoutComponent>().Type
			== UILayoutType::Vertical);
	}

	TEST_CASE("ui.create places elements by anchor, position and size, and refuses what it cannot make")
	{
		UICommandHarness harness;
		const nlohmann::json canvas = harness.Run("ui.create", { { "type", "canvas" } });
		const nlohmann::json corner = harness.Run("ui.create", { { "type", "panel" }, { "parent", canvas["entity"] }, { "anchor", "topLeft" },
			{ "position", { 16, 24 } }, { "size", { 200, 50 } } });
		CHECK(ToRect(corner["rect"]) == glm::vec4(16.0f, 24.0f, 200.0f, 50.0f));
		const UIRectComponent& rect = harness.Find(corner["entity"]).GetComponent<UIRectComponent>();
		CHECK(rect.Pivot == glm::vec2(0.0f));
		const nlohmann::json bar = harness.Run("ui.create", { { "type", "panel" }, { "parent", canvas["entity"] }, { "anchor", "bottomStretch" },
			{ "size", { -40, 60 } } });
		CHECK(ToRect(bar["rect"]) == glm::vec4(20.0f, 1020.0f, 1880.0f, 60.0f));
		const nlohmann::json fill = harness.Run("ui.create", { { "type", "image" }, { "parent", canvas["entity"] }, { "anchor", "stretch" } });
		CHECK(ToRect(fill["rect"]) == glm::vec4(0.0f, 0.0f, 1920.0f, 1080.0f));

		const nlohmann::json before = harness.SceneSnapshot();
		CHECK(harness.Fail("ui.create", { { "type", "dropdown" } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("ui.create", { { "type", "image" }, { "anchor", "upperLeft" } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("ui.create", { { "type", "image" }, { "size", { -5, 10 } } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("ui.create", { { "type", "image" }, { "position", { "a", 1 } } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.SceneSnapshot() == before);
	}

	TEST_CASE("ui.layout reports where elements are at any screen, and the same fractions under ScaleWithScreen")
	{
		UICommandHarness harness;
		const nlohmann::json canvas = harness.Run("ui.create", { { "type", "canvas" } });
		const nlohmann::json score = harness.Run("ui.create", { { "type", "text" }, { "parent", canvas["entity"] }, { "anchor", "topRight" },
			{ "position", { -40, 30 } }, { "size", { 300, 60 } }, { "text", "Score: 0" } });
		const nlohmann::json button = harness.Run("ui.create", { { "type", "button" }, { "parent", canvas["entity"] } });

		const nlohmann::json small = harness.Run("ui.layout", { { "screen", { 1280, 720 } } });
		const nlohmann::json large = harness.Run("ui.layout", { { "screen", { 2560, 1440 } } });
		REQUIRE(small["canvases"].size() == 1);
		CHECK(small["canvases"][0]["scale"].get<float>() == doctest::Approx(1280.0f / 1920.0f));
		CHECK(large["canvases"][0]["scale"].get<float>() == doctest::Approx(2560.0f / 1920.0f));
		auto findRect = [](const nlohmann::json& layout, const nlohmann::json& id)
		{
			for (const nlohmann::json& element : layout["elements"])
			{
				if (element["entity"] == id)
					return ToRect(element["rect"]);
			}
			FAIL("The element is not listed");
			return glm::vec4(0.0f);
		};
		const glm::vec4 smallRect = findRect(small, score["entity"]) / glm::vec4(1280.0f, 720.0f, 1280.0f, 720.0f);
		const glm::vec4 largeRect = findRect(large, score["entity"]) / glm::vec4(2560.0f, 1440.0f, 2560.0f, 1440.0f);
		for (int index = 0; index < 4; index++)
			CHECK(smallRect[index] == doctest::Approx(largeRect[index]).epsilon(1e-5));

		// What each element is: texts, selectables and their states.
		const nlohmann::json subtree = harness.Run("ui.layout", { { "entity", button["entity"] } });
		REQUIRE(subtree["elements"].size() == 2); // The button and its label
		CHECK(subtree["elements"][0]["interactable"] == true);
		CHECK(subtree["elements"][0]["state"] == "normal");
		CHECK(subtree["elements"][0]["focused"] == false);
		CHECK(subtree["elements"][1]["text"] == "Button");
		CHECK(subtree["truncated"] == false);
		CHECK(harness.Fail("ui.layout", { { "screen", { 0, 100 } } }).ErrorKind == EditorCommandError::InvalidParameters);
		const std::string plain = harness.Run("entity.create", { { "name", "Plain" } })["id"].get<std::string>();
		CHECK(harness.Fail("ui.layout", { { "entity", plain } }).Error.find("'Plain'") != std::string::npos);

		// Long lists stop at 1,000 elements.
		Scene& scene = *harness.Context.GetEditScene();
		const Entity canvasEntity = harness.Find(canvas["entity"]);
		for (int index = 0; index < 1100; index++)
			scene.CreateChildEntity(canvasEntity, "Item").AddComponent<UIRectComponent>();
		const nlohmann::json full = harness.Run("ui.layout");
		CHECK(full["elements"].size() == 1000);
		CHECK(full["truncated"] == true);
	}

	TEST_CASE("ui.hitTest finds what a click would hit, and with all everything at the position")
	{
		UICommandHarness harness;
		const nlohmann::json canvas = harness.Run("ui.create", { { "type", "canvas" } });
		const nlohmann::json panel = harness.Run("ui.create", { { "type", "panel" }, { "parent", canvas["entity"] } });
		const nlohmann::json button = harness.Run("ui.create", { { "type", "button" }, { "parent", panel["entity"] }, { "name", "Play" } });

		const nlohmann::json hit = harness.Run("ui.hitTest", { { "position", { 960, 540 } } });
		CHECK(hit["entity"] == button["entity"]); // Its label is no raycast target
		CHECK(hit["path"] == "Canvas/Panel/Play");
		CHECK(hit["selectable"] == button["entity"]);
		CHECK(hit["interactable"] == true);
		const nlohmann::json onPanel = harness.Run("ui.hitTest", { { "position", { 800, 420 } } });
		CHECK(onPanel["entity"] == panel["entity"]);
		CHECK(onPanel["interactable"] == false);
		CHECK(harness.Run("ui.hitTest", { { "position", { 10, 10 } } })["entity"].is_null());

		const nlohmann::json all = harness.Run("ui.hitTest", { { "position", { 960, 540 } }, { "all", true } });
		REQUIRE(all["hits"].size() == 4); // Label, button, panel, canvas
		CHECK(all["hits"][0]["entity"] == button["parts"]["label"]);
		CHECK(all["hits"][3]["entity"] == canvas["entity"]);
		// Another screen: the same element at its place there.
		CHECK(harness.Run("ui.hitTest", { { "position", { 640, 360 } }, { "screen", { 1280, 720 } } })["entity"] == button["entity"]);
	}

	TEST_CASE("input.click clicks an element where the player sees it, and fails when something covers it")
	{
		PlayingUIHarness harness;
		const nlohmann::json canvas = harness.Run("ui.create", { { "type", "canvas" } });
		const nlohmann::json button = harness.Run("ui.create", { { "type", "button" }, { "parent", canvas["entity"] }, { "anchor", "topLeft" },
			{ "position", { 100, 100 } } });
		const nlohmann::json toggle = harness.Run("ui.create", { { "type", "toggle" }, { "parent", canvas["entity"] }, { "anchor", "topLeft" },
			{ "position", { 100, 300 } } });
		const nlohmann::json cover = harness.Run("ui.create", { { "type", "panel" }, { "parent", canvas["entity"] }, { "name", "Cover" } });
		CHECK_FALSE(harness.Commands.Execute(harness.Context, "input.click", { { "entity", button["entity"] } }).Success); // Not playing

		harness.Run("play.start");
		harness.Frame();
		const EditorCommandResult clicked = harness.RunFrames("input.click", { { "entity", button["entity"] } });
		INFO(clicked.Error);
		REQUIRE(clicked.Success);
		CHECK(clicked.Value["clicked"] == true);
		CHECK(clicked.Value["position"] == nlohmann::json { 210.0f, 128.0f });
		bool click = false;
		for (const nlohmann::json& event : clicked.Value["events"])
			click |= event["type"] == "click" && event["widget"] == button["entity"];
		CHECK(click);

		const EditorCommandResult flipped = harness.RunFrames("input.click", { { "entity", toggle["entity"] } });
		REQUIRE(flipped.Success);
		CHECK(flipped.Value["clicked"] == true);
		CHECK(harness.Find(toggle["entity"]).GetComponent<UIToggleComponent>().IsOn == false);

		// The panel in the middle of the screen covers nothing of those; a button moved under it is covered.
		Entity moved = harness.Context.GetActiveScene()->GetEntityByUUID(*UUIDFromJson(button["entity"]));
		UIRectComponent& rect = moved.GetComponent<UIRectComponent>();
		rect.AnchorMin = rect.AnchorMax = rect.Pivot = glm::vec2(0.5f);
		rect.Position = glm::vec2(0.0f);
		moved.MarkModified<UIRectComponent>();
		const EditorCommandResult covered = harness.Commands.Execute(harness.Context, "input.click", { { "entity", button["entity"] } });
		REQUIRE_FALSE(covered.IsPending());
		CHECK_FALSE(covered.Success);
		CHECK(covered.Error.find("Canvas/Cover") != std::string::npos);
		CHECK(covered.ErrorData["cover"] == cover["entity"]);
	}

	TEST_CASE("input.type types text the game reads in its next frame")
	{
		PlayingUIHarness harness;
		CHECK_FALSE(harness.Commands.Execute(harness.Context, "input.type", { { "text", "hi" } }).Success); // Not playing
		harness.Run("play.start");
		harness.Frame();
		CHECK(harness.Commands.Execute(harness.Context, "input.type", { { "text", "" } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Commands.Execute(harness.Context, "input.type", { { "text", std::string(1025, 'x') } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		const EditorCommandResult typed = harness.RunFrames("input.type", { { "text", "Name \xC3\xA9" } });
		REQUIRE(typed.Success);
		CHECK(typed.Value["seen"] == true);
		CHECK(Input::GetTypedText() == "Name \xC3\xA9");
		harness.Frame();
		CHECK(Input::GetTypedText().empty());
	}

	TEST_CASE("An agent edits an input field: input.click focuses it, input.type types into it, ui.layout reads it")
	{
		PlayingUIHarness harness;
		const nlohmann::json canvas = harness.Run("ui.create", { { "type", "canvas" } });
		const nlohmann::json field = harness.Run("ui.create", { { "type", "inputField" }, { "parent", canvas["entity"] } });
		harness.Run("play.start");
		harness.Frame();
		const EditorCommandResult clicked = harness.RunFrames("input.click", { { "entity", field["entity"] } });
		INFO(clicked.Error);
		REQUIRE(clicked.Success);
		bool focused = false;
		for (const nlohmann::json& event : clicked.Value["events"])
			focused |= event["type"] == "focusChanged" && event["widget"] == field["entity"] && event["focused"] == true;
		CHECK(focused);

		const EditorCommandResult typed = harness.RunFrames("input.type", { { "text", "Ada" } });
		REQUIRE(typed.Success);
		CHECK(Input::GetTypedText().empty()); // The field has the keyboard
		harness.Frame();
		const nlohmann::json layout = harness.Run("ui.layout", { { "entity", field["entity"] } });
		CHECK(layout["elements"][0]["text"] == "Ada");
		CHECK(layout["elements"][0]["editing"] == true);
		CHECK(layout["elements"][1]["text"] == "Ada"); // Its text element shows it
		CHECK(layout["elements"][2]["visible"] == false); // The placeholder hides
	}
}
