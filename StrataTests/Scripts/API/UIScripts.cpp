// Scripts of the Scripting.UI tests (StrataTests/src/Scripting/ScriptUITests.cpp). The scene (CreateUIScene there): a canvas
// "Menu" with one unit per pixel holding "Title" (a white UIText "Hello"), "Logo" (a UIImage colored (0.2, 0.4, 0.6, 1)),
// "Play" (a button at (100, 100), 200 x 50, whose event target is "Controller"), "Sound" and "Music" (toggles of the
// group "Options", Sound on), "Volume" (a slider from 0 to 10 with whole numbers at 4) and "Locked" (a button that is not
// interactable); "Plain" has no UI components.

#include "TestScripts.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

using namespace Strata;
using namespace ScriptTests;

namespace
{

	constexpr float c_NaN = std::numeric_limits<float>::quiet_NaN();

	UIElement Find(std::string_view name)
	{
		return Scene::FindEntityByName(name).GetUI();
	}

}

// Reads and changes every kind of element through UIElement and UI, in its first update.
class UIControlAPI : public CheckingScript
{
public:
	AssetHandle Texture;
	bool Done = false;

	void OnUpdate(float) override
	{
		if (Done)
			return;
		Done = true;

		UIElement title = Find("Title");
		Expect(title.GetEntity() == Scene::FindEntityByName("Title"), "UIElement::GetEntity");
		Expect(title.GetText() == "Hello", "GetText");
		Expect(title.SetText("Score 10") && title.GetText() == "Score 10", "SetText");
		const std::string longText(300, 'x'); // Longer than the SDK's first read
		Expect(title.SetText(longText) && title.GetText() == longText, "a long text");
		Expect(title.SetText("Score 10") && title.SetText("Score 10"), "the same text again");
		Expect(title.GetColor() == glm::vec4(1.0f), "a text's color");
		Expect(title.SetColor({ 1.0f, 0.0f, 0.0f, 1.0f }) && title.GetColor() == glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), "SetColor of a text");

		UIElement logo = Find("Logo");
		Expect(Near(logo.GetColor(), glm::vec4(0.2f, 0.4f, 0.6f, 1.0f)), "GetColor of an image");
		Expect(logo.SetColor({ 2.0f, -1.0f, 0.5f, 1.0f }) && logo.GetColor() == glm::vec4(1.0f, 0.0f, 0.5f, 1.0f), "SetColor clamps");
		Expect(!logo.GetImage(), "an image without a texture");
		Expect(logo.SetImage(Texture) && logo.GetImage() == Texture, "SetImage");
		Expect(logo.SetImage(AssetHandle()) && !logo.GetImage(), "SetImage clears");

		UIElement play = Find("Play");
		Expect(play.IsInteractable(), "IsInteractable");
		Expect(play.SetInteractable(false) && !play.IsInteractable() && play.SetInteractable(true) && play.IsInteractable(), "SetInteractable");
		Expect(play.Focus() && play.IsFocused() && UI::GetFocused() == play.GetEntity(), "Focus");
		Expect(UI::ClearFocus() && !play.IsFocused() && !UI::GetFocused(), "ClearFocus");

		UIElement volume = Find("Volume");
		Expect(volume.GetValue() == 4.0f, "a slider's value");
		Expect(volume.SetValue(12.0f) && volume.GetValue() == 10.0f, "SetValue clamps to the range");
		Expect(volume.SetValue(2.6f) && volume.GetValue() == 3.0f, "SetValue rounds whole numbers");

		UIElement sound = Find("Sound");
		UIElement music = Find("Music");
		Expect(sound.GetValue() == 1.0f && music.GetValue() == 0.0f, "toggles' values");
		Expect(music.SetValue(1.0f) && music.GetValue() == 1.0f && sound.GetValue() == 0.0f, "one toggle of a group is on");
		Expect(music.SetValue(0.0f) && music.GetValue() == 0.0f, "a toggle switched off");

		Expect(!UI::IsPointerOverUI(), "IsPointerOverUI without a pointer over the UI");
		Expect(!UI::IsActionPressed(UIAction::Submit) && !UI::IsActionDown(UIAction::Cancel), "actions without input");
		Expect(Time::GetUnscaledDeltaTime() > 0.0f && Time::GetRealTime() > 0.0, "unscaled time");
		Expect(Input::GetTypedText().empty(), "no typed text");
	}
};

ST_SCRIPT_CLASS(UIControlAPI)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Texture);
	ST_SCRIPT_FIELD(Done);
}

// Calls the UI functions with what they refuse: every call fails harmlessly.
class UIMisuse : public CheckingScript
{
public:
	AssetHandle NotTexture;
	bool Done = false;

	void OnUpdate(float) override
	{
		if (Done)
			return;
		Done = true;

		const StrataScriptHostAPI* host = Detail::GetHost();
		StrataScriptContext* context = Detail::GetContext();
		UIElement plain = Find("Plain");
		Expect(plain.GetText().empty() && !plain.SetText("x"), "text of an entity without a UIText");
		Expect(!plain.GetImage() && !plain.SetImage(AssetHandle()), "the image of an entity without a UIImage");
		Expect(plain.GetColor() == glm::vec4(0.0f) && !plain.SetColor(glm::vec4(1.0f)), "the color of an entity without an image or text");
		Expect(!plain.IsInteractable() && !plain.SetInteractable(true), "an entity without a UISelectable");
		Expect(plain.GetValue() == 0.0f && !plain.SetValue(1.0f), "the value of an entity without a slider or toggle");
		Expect(!plain.Focus(), "focusing an entity without a UISelectable");
		Expect(!Find("Locked").Focus(), "focusing a selectable that is not interactable");
		Expect(!Find("Title").GetImage() && !Find("Title").SetImage(AssetHandle()), "the image of a text");
		Expect(!UIElement(Entity()).SetText("x"), "the SDK's null entity");
		Expect(!host->UISetText(context, 0, Detail::ToABIString("x")), "the null entity");
		Expect(!host->UISetValue(context, 0x51DE5u, 1.0f), "an entity that does not exist");
		Expect(!host->UISetFocus(context, 0x51DE5u), "focusing an entity that does not exist");

		const uint64_t logo = Scene::FindEntityByName("Logo").GetID();
		const float nan[4] = { c_NaN, 0.0f, 0.0f, 1.0f };
		Expect(!host->UISetColor(context, logo, nan), "a color that is not finite");
		Expect(!host->UISetColor(context, logo, nullptr) && !host->UIGetColor(context, logo, nullptr), "null colors");
		Expect(!Find("Logo").SetImage(NotTexture), "an asset that is not a texture");
		Expect(!Find("Volume").SetValue(c_NaN), "a value that is not finite");
		Expect(Find("Volume").GetValue() == 4.0f, "a refused value changes nothing");
		Expect(!host->UIIsActionPressed(context, 6) && !host->UIIsActionDown(context, 0xFFFFFFFFu), "actions that do not exist");
		Expect(host->UIGetText(context, Scene::FindEntityByName("Title").GetID(), nullptr, 0) == 5, "the size of a text without a buffer");
		Expect(host->GetTypedText(context, nullptr, 0) == 0, "no typed text without a buffer");
	}
};

ST_SCRIPT_CLASS(UIMisuse)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(NotTexture);
	ST_SCRIPT_FIELD(Done);
}

// Mode 0: a scene playing without the game UI. Mode 1: an engine whose host table ends before the UI's functions. Every
// call fails or answers nothing.
class UIUnavailable : public CheckingScript
{
public:
	int32_t Mode = 0;
	bool Done = false;

	void OnUpdate(float) override
	{
		if (Done)
			return;
		Done = true;

		StrataScriptHostAPI older = *Detail::GetHost();
		older.StructSize = static_cast<uint32_t>(offsetof(StrataScriptHostAPI, UIGetText));
		const StrataScriptHostAPI* engine = Detail::s_Host;
		if (Mode == 1)
			Detail::s_Host = &older;

		UIElement title = Find("Title");
		UIElement volume = Find("Volume");
		UIElement play = Find("Play");
		const bool changed = title.SetText("x") || title.SetColor(glm::vec4(1.0f)) || Find("Logo").SetImage(AssetHandle()) || play.SetInteractable(false)
			|| volume.SetValue(1.0f) || play.Focus() || UI::ClearFocus();
		const bool answered = !title.GetText().empty() || title.GetColor() != glm::vec4(0.0f) || play.IsInteractable() || volume.GetValue() != 0.0f
			|| play.IsFocused() || UI::GetFocused() || UI::IsPointerOverUI() || UI::IsActionPressed(UIAction::Submit) || UI::IsActionDown(UIAction::Up);
		const bool timed = Mode == 1 && (Time::GetUnscaledDeltaTime() != 0.0f || Time::GetRealTime() != 0.0 || !Input::GetTypedText().empty());
		Detail::s_Host = engine;

		Expect(!changed, "changes fail");
		Expect(!answered, "queries answer nothing");
		Expect(!timed, "an older engine has no unscaled time or typed text");
	}
};

ST_SCRIPT_CLASS(UIUnavailable)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Mode);
	ST_SCRIPT_FIELD(Done);
}

// Records the UI callbacks it receives as "<callback>:<widget>[=<value>|+|-];" in Events, and what its OnUpdate saw: the
// number of callbacks so far (SeenInUpdate), so a test can tell they arrive before the frame's OnUpdate.
class UIEventRecorder : public Script
{
public:
	std::string Events;
	int32_t Count = 0;
	int32_t SeenInUpdate = 0;
	float LastValue = 0.0f;

	void OnUpdate(float) override
	{
		SeenInUpdate = Count;
	}

	void OnUIClick(Entity widget) override
	{
		Record("click:" + widget.GetName());
	}

	void OnUIValueChanged(Entity widget, float value) override
	{
		LastValue = value;
		Record("value:" + widget.GetName() + "=" + std::to_string(static_cast<int32_t>(std::lround(value))));
	}

	void OnUIFocusChanged(Entity widget, bool focused) override
	{
		Record("focus:" + widget.GetName() + (focused ? "+" : "-"));
	}
private:
	void Record(const std::string& event)
	{
		Count++;
		Events += event;
		Events += ';';
	}
};

ST_SCRIPT_CLASS(UIEventRecorder)
{
	ST_SCRIPT_FIELD(Events);
	ST_SCRIPT_FIELD(Count);
	ST_SCRIPT_FIELD(SeenInUpdate);
	ST_SCRIPT_FIELD(LastValue);
}

// Reads the frame's time and typed text into fields every update.
class UIFrameProbe : public Script
{
public:
	float DeltaTime = 0.0f;
	float UnscaledDeltaTime = 0.0f;
	float RealTime = 0.0f;
	std::string Typed;

	void OnUpdate(float deltaTime) override
	{
		DeltaTime = deltaTime;
		UnscaledDeltaTime = Time::GetUnscaledDeltaTime();
		RealTime = static_cast<float>(Time::GetRealTime());
		Typed += Input::GetTypedText();
	}
};

ST_SCRIPT_CLASS(UIFrameProbe)
{
	ST_SCRIPT_FIELD(DeltaTime);
	ST_SCRIPT_FIELD(UnscaledDeltaTime);
	ST_SCRIPT_FIELD(RealTime);
	ST_SCRIPT_FIELD(Typed);
}
