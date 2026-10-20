// The main menu, on the start scene's "Game" entity (the menu's buttons send it their clicks): Play loads the course,
// Settings shows the settings panel beside the menu, Credits a panel whose text scrolls inside a UIMask, Quit ends the
// game. After AttractDelaySeconds without input (0: never) it starts the attract mode: the course plays itself as a demo
// until the player touches anything (the arcade convention).

#include "SettingsMenu.h"
#include "ThirdPerson.h"

#include <algorithm>

using namespace Strata;

class MainMenu : public Script
{
public:
	AssetHandle CourseScene;
	float AttractDelaySeconds = 10.0f;
	Entity PlayButton;
	Entity SettingsButton;
	Entity CreditsButton;
	Entity QuitButton;
	Entity CreditsPanel;
	Entity CreditsContent;           // The text that scrolls inside the credits' mask
	Entity CreditsBackButton;
	float CreditsScrollSpeed = 45.0f; // Canvas units per second
	float CreditsViewHeight = 360.0f; // The mask's height: the text starts below it and leaves above it
	AssetHandle ClickSound;
	AssetHandle FocusSound;

	// What the menu does (the sample's regression test)
	float IdleSeconds = 0.0f;        // Since the player last touched anything
	bool Loading = false;            // The course was asked for

	void OnCreate() override
	{
		ThirdPerson::SetAttractMode(false);
		ThirdPerson::ApplySettings(false);
		Time::SetTimeScale(1.0f);
		Input::SetCursorMode(CursorMode::Normal);
		Assets::RequestLoad(ClickSound);
		Assets::RequestLoad(FocusSound);
		if (CreditsPanel.IsValid())
			CreditsPanel.SetActive(false);
		if (PlayButton.IsValid())
			PlayButton.GetUI().Focus();
		IdleSeconds = 0.0f;
		Loading = false;
		Log::Info("ThirdPerson: main menu ready");
	}

	void OnUpdate(float) override
	{
		const float deltaTime = Time::GetUnscaledDeltaTime();
		if (ThirdPerson::AnyInputPressed() || glm::length(Input::GetMouseDelta()) > 0.0f)
			IdleSeconds = 0.0f;
		else
			IdleSeconds += deltaTime;
		if (!Loading && AttractDelaySeconds > 0.0f && IdleSeconds >= AttractDelaySeconds)
		{
			Loading = true;
			ThirdPerson::SetAttractMode(true);
			Log::Info("ThirdPerson: attract mode");
			Game::LoadScene(CourseScene);
		}

		if (UI::IsActionPressed(UIAction::Cancel))
		{
			if (SettingsMenu* settings = GetEntity().GetScript<SettingsMenu>(); settings && settings->IsOpen())
				settings->Close();
			else if (CreditsPanel.IsValid() && CreditsPanel.IsActive())
				ShowCredits(false);
		}
		if (CreditsPanel.IsValid() && CreditsPanel.IsActive() && CreditsContent.IsValid())
		{
			// Up from below the mask until the whole text has left it, then again.
			glm::vec2 position = CreditsContent.GetProperty<glm::vec2>("UIRect", "Position").value_or(glm::vec2(0.0f));
			const float height = CreditsContent.GetProperty<glm::vec2>("UIRect", "Size").value_or(glm::vec2(0.0f)).y;
			position.y -= CreditsScrollSpeed * deltaTime;
			if (position.y < -height)
				position.y = CreditsViewHeight;
			CreditsContent.SetProperty("UIRect", "Position", position);
		}
	}

	void OnUIClick(Entity widget) override
	{
		if (widget == PlayButton || widget == SettingsButton || widget == CreditsButton || widget == QuitButton || widget == CreditsBackButton)
			ThirdPerson::PlayInterfaceSound(ClickSound);
		if (widget == PlayButton && !Loading)
		{
			Loading = true;
			Log::Info("ThirdPerson: starting the course");
			Game::LoadScene(CourseScene);
		}
		else if (widget == SettingsButton)
		{
			ShowCredits(false);
			if (SettingsMenu* settings = GetEntity().GetScript<SettingsMenu>())
				settings->Toggle(SettingsButton);
		}
		else if (widget == CreditsButton)
		{
			if (SettingsMenu* settings = GetEntity().GetScript<SettingsMenu>(); settings && settings->IsOpen())
				settings->Close();
			ShowCredits(!(CreditsPanel.IsValid() && CreditsPanel.IsActive()));
		}
		else if (widget == CreditsBackButton)
		{
			ShowCredits(false);
		}
		else if (widget == QuitButton)
		{
			Game::Quit();
		}
	}

	void OnUIFocusChanged(Entity, bool focused) override
	{
		if (focused)
			ThirdPerson::PlayInterfaceSound(FocusSound, 0.4f);
	}
private:
	void ShowCredits(bool show)
	{
		if (!CreditsPanel.IsValid())
			return;
		CreditsPanel.SetActive(show);
		if (show && CreditsContent.IsValid())
		{
			const glm::vec2 position = CreditsContent.GetProperty<glm::vec2>("UIRect", "Position").value_or(glm::vec2(0.0f));
			CreditsContent.SetProperty("UIRect", "Position", glm::vec2(position.x, CreditsViewHeight * 0.25f));
		}
		if (show && CreditsBackButton.IsValid())
			CreditsBackButton.GetUI().Focus();
		else if (!show && CreditsButton.IsValid())
			CreditsButton.GetUI().Focus();
	}
};

ST_SCRIPT_CLASS(MainMenu)
{
	ST_SCRIPT_FIELD(CourseScene);
	ST_SCRIPT_FIELD(AttractDelaySeconds);
	ST_SCRIPT_FIELD(PlayButton);
	ST_SCRIPT_FIELD(SettingsButton);
	ST_SCRIPT_FIELD(CreditsButton);
	ST_SCRIPT_FIELD(QuitButton);
	ST_SCRIPT_FIELD(CreditsPanel);
	ST_SCRIPT_FIELD(CreditsContent);
	ST_SCRIPT_FIELD(CreditsBackButton);
	ST_SCRIPT_FIELD(CreditsScrollSpeed);
	ST_SCRIPT_FIELD(CreditsViewHeight);
	ST_SCRIPT_FIELD(ClickSound);
	ST_SCRIPT_FIELD(FocusSound);
	ST_SCRIPT_FIELD(IdleSeconds);
	ST_SCRIPT_FIELD(Loading);
}
