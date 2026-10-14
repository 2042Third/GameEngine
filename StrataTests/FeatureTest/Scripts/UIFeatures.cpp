// The game UI's script API (UIElement, UI, the widget callbacks, unscaled time, typed text) and Motion, on "UI Features":
// the event target of the "Feature UI" canvas's Play Button, Sound Toggle and Volume Slider.
//
// The runner simulates the player around ClickFrame (a field, so the scene decides when; after InputFeatures' input, whose
// Space, gamepad A and stick already focus and click the canvas's first selectable through the UI actions):
//   ClickFrame - 1   the pointer moves to the Play Button's center (the script clears the focus)
//   ClickFrame       the left button goes down (the Play Button takes the focus)
//   ClickFrame + 1   it goes up: a click
//   ClickFrame + 2   the pointer moves to the Sound Toggle's center and the button goes down (the focus moves there)
//   ClickFrame + 3   it goes up: the toggle switches off
//   ClickFrame + 4   the pointer moves to the Volume Slider's left end and the button goes down: the value drops to 0
//   ClickFrame + 5   the pointer moves past its right end: 10
//   ClickFrame + 6   the button goes up
//   ClickFrame + 7   Up goes down: the focus moves from the slider to the toggle above it
//   ClickFrame + 8   Up goes up; "OK" is typed
//   ClickFrame + 9   gamepad 1 connects with B down (Cancel)
//   ClickFrame + 10  B goes up, Enter goes down: Submit switches the focused toggle on again
//   ClickFrame + 11  Enter goes up

#include "FeatureScript.h"

#include <string>

using namespace Strata;
using namespace FeatureTest;

class UIFeatures : public FeatureScript
{
public:
	int32_t ClickFrame = 0;
	AssetHandle Texture;
	int32_t Clicks = 0;
	int32_t ValueChanges = 0;
	int32_t FocusGains = 0;
	int32_t FocusLosses = 0;
	// The counts when the scenario starts (ClickFrame - 1).
	int32_t BaseClicks = 0;
	int32_t BaseValueChanges = 0;
	int32_t BaseFocusGains = 0;
	int32_t BaseFocusLosses = 0;
	float LastValue = -1.0f;
	std::string LastWidget;
	std::string Typed;
	bool JournaledClick = false;
	bool JournaledValue = false;
	bool JournaledFocus = false;
	bool CheckedControl = false;
	float Damped = 0.0f;
	float DampedVelocity = 0.0f;

	void OnCreate() override
	{
		Journal(*this, "UIFeatures", "OnCreate");
		Expect(ClickFrame > 1, "the scene sets the click frame");
	}

	void OnUpdate(float) override
	{
		if (!CheckedControl)
		{
			CheckedControl = true;
			CheckControl();
		}
		Typed += Input::GetTypedText();
		Damped = Motion::SmoothDamp(Damped, 1.0f, DampedVelocity, 0.1f, Time::GetUnscaledDeltaTime());

		const int32_t frame = GetFrame();
		if (Completed || frame < ClickFrame - 1)
			return;
		if (frame == ClickFrame - 1)
		{
			// Whatever earlier input focused, the scenario starts without a focus.
			Expect(UI::ClearFocus() && !UI::GetFocused(), "UI::ClearFocus");
			BaseClicks = Clicks;
			BaseValueChanges = ValueChanges;
			BaseFocusGains = FocusGains;
			BaseFocusLosses = FocusLosses;
		}
		else if (frame == ClickFrame)
		{
			// The press focused the button (events arrive before OnUpdate).
			Expect(FocusGains == BaseFocusGains + 1 && LastWidget == "Play Button", "OnUIFocusChanged on the press");
			Expect(UI::IsPointerOverUI(), "UI::IsPointerOverUI over a button");
			Expect(Scene::FindEntityByName("Play Button").GetUI().IsFocused(), "UIElement::IsFocused");
			Expect(!Input::IsMouseButtonPressed(Mouse::ButtonLeft), "a press over the UI is hidden from the game");
		}
		else if (frame == ClickFrame + 1)
		{
			Expect(Clicks == BaseClicks + 1 && LastWidget == "Play Button", "OnUIClick on the release");
		}
		else if (frame == ClickFrame + 3)
		{
			Expect(ValueChanges == BaseValueChanges + 1 && LastValue == 0.0f && LastWidget == "Sound Toggle", "a toggle switched off by a click");
			// Two losses: the focus cleared at the start (InputFeatures' Space and stick had focused the button) and the button's.
			Expect(FocusLosses == BaseFocusLosses + 2, "the focus left the button");
		}
		else if (frame == ClickFrame + 5)
		{
			Expect(ValueChanges == BaseValueChanges + 3 && LastValue == 10.0f && LastWidget == "Volume Slider", "a dragged slider");
			Expect(Scene::FindEntityByName("Volume Slider").GetUI().GetValue() == 10.0f, "the slider's value");
		}
		else if (frame == ClickFrame + 7)
		{
			Expect(UI::GetFocused() == Scene::FindEntityByName("Sound Toggle"), "Up moved the focus to the toggle");
			Expect(UI::IsActionDown(UIAction::Up), "UI::IsActionDown");
		}
		else if (frame == ClickFrame + 8)
		{
			Expect(Typed == "OK", "Input::GetTypedText");
		}
		else if (frame == ClickFrame + 9)
		{
			Expect(UI::IsActionPressed(UIAction::Cancel), "UI::IsActionPressed (Cancel)");
		}
		else if (frame == ClickFrame + 10)
		{
			Expect(ValueChanges == BaseValueChanges + 4 && LastValue == 1.0f && LastWidget == "Sound Toggle", "Submit switched the focused toggle on");
			Expect(Time::GetUnscaledDeltaTime() > 0.0f && Time::GetRealTime() > 0.0, "unscaled time");
			Expect(Damped > 0.9f && Damped <= 1.0f, "Motion::SmoothDamp");
			Completed = true;
		}
	}

	void OnUIClick(Entity widget) override
	{
		Clicks++;
		LastWidget = widget.GetName();
		if (!JournaledClick)
		{
			JournaledClick = true;
			Journal(*this, "UIFeatures", "OnUIClick");
		}
	}

	void OnUIValueChanged(Entity widget, float value) override
	{
		ValueChanges++;
		LastValue = value;
		LastWidget = widget.GetName();
		if (!JournaledValue)
		{
			JournaledValue = true;
			Journal(*this, "UIFeatures", "OnUIValueChanged");
		}
	}

	void OnUIFocusChanged(Entity widget, bool focused) override
	{
		(focused ? FocusGains : FocusLosses)++;
		LastWidget = widget.GetName();
		if (!JournaledFocus)
		{
			JournaledFocus = true;
			Journal(*this, "UIFeatures", "OnUIFocusChanged");
		}
	}
private:
	// The element functions on the feature canvas, each change undone (the scenario's input expects the scene as authored).
	void CheckControl()
	{
		UIElement label = Scene::FindEntityByName("Play Label").GetUI();
		Expect(label.GetEntity() == Scene::FindEntityByName("Play Label"), "UIElement::GetEntity");
		Expect(label.GetText() == "Play", "UIElement::GetText");
		Expect(label.SetText("Start") && label.GetText() == "Start" && label.SetText("Play"), "UIElement::SetText");
		const glm::vec4 color = label.GetColor();
		Expect(Near(color, glm::vec4(0.05f, 0.05f, 0.1f, 1.0f)), "UIElement::GetColor");
		Expect(label.SetColor(glm::vec4(1.0f)) && label.GetColor() == glm::vec4(1.0f) && label.SetColor(color), "UIElement::SetColor");

		UIElement quit = Scene::FindEntityByName("Quit Button").GetUI();
		Expect(!quit.GetImage() && quit.SetImage(Texture) && quit.GetImage() == Texture && quit.SetImage(AssetHandle()), "UIElement::SetImage and GetImage");
		UIElement disabled = Scene::FindEntityByName("Disabled Button").GetUI();
		Expect(!disabled.IsInteractable() && disabled.SetInteractable(true) && disabled.IsInteractable() && disabled.SetInteractable(false),
			"UIElement::IsInteractable and SetInteractable");
		UIElement volume = Scene::FindEntityByName("Volume Slider").GetUI();
		Expect(volume.GetValue() == 4.0f && volume.SetValue(6.4f) && volume.GetValue() == 6.0f && volume.SetValue(4.0f), "UIElement::GetValue and SetValue");
		Expect(quit.Focus() && quit.IsFocused() && UI::GetFocused() == quit.GetEntity() && UI::ClearFocus(), "UIElement::Focus");

		// Motion: a camera at (0, 2, 5) looking at the origin turns -Z towards it.
		const glm::quat look = Motion::LookAt(glm::vec3(0.0f, 2.0f, 5.0f), glm::vec3(0.0f));
		Expect(Near(look * glm::vec3(0.0f, 0.0f, -1.0f), glm::normalize(glm::vec3(0.0f, -2.0f, -5.0f))), "Motion::LookAt");
		glm::vec2 velocity2(0.0f);
		glm::vec3 velocity3(0.0f);
		const glm::vec2 moved2 = Motion::SmoothDamp(glm::vec2(0.0f), glm::vec2(1.0f, 0.0f), velocity2, 0.2f, 0.05f);
		const glm::vec3 moved3 = Motion::SmoothDamp(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), velocity3, 0.2f, 0.05f, 100.0f);
		Expect(moved2.x > 0.0f && moved2.x < 1.0f && moved3.z > 0.0f && moved3.z < 1.0f, "Motion::SmoothDamp of vectors");
	}
};

ST_SCRIPT_CLASS(UIFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(ClickFrame);
	ST_SCRIPT_FIELD(Texture);
	ST_SCRIPT_FIELD(Clicks);
	ST_SCRIPT_FIELD(ValueChanges);
	ST_SCRIPT_FIELD(FocusGains);
	ST_SCRIPT_FIELD(FocusLosses);
	ST_SCRIPT_FIELD(BaseClicks);
	ST_SCRIPT_FIELD(BaseValueChanges);
	ST_SCRIPT_FIELD(BaseFocusGains);
	ST_SCRIPT_FIELD(BaseFocusLosses);
	ST_SCRIPT_FIELD(LastValue);
	ST_SCRIPT_FIELD(LastWidget);
	ST_SCRIPT_FIELD(Typed);
	ST_SCRIPT_FIELD(JournaledClick);
	ST_SCRIPT_FIELD(JournaledValue);
	ST_SCRIPT_FIELD(JournaledFocus);
	ST_SCRIPT_FIELD(CheckedControl);
	ST_SCRIPT_FIELD(Damped);
	ST_SCRIPT_FIELD(DampedVelocity);
}
