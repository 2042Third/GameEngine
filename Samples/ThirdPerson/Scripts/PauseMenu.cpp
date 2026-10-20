// The course's pause menu, on its "Game" entity (the menu's buttons send it their clicks): Esc or the gamepad's Start
// stops time (Time::SetTimeScale(0)) and shows the menu with the cursor free: Resume, Settings (the settings panel beside
// it), Restart, Main Menu and Quit. Esc again closes the settings, then resumes.

#include "CourseState.h"
#include "SettingsMenu.h"
#include "ThirdPerson.h"

using namespace Strata;

class PauseMenu : public Script
{
public:
	Entity Panel;
	Entity ResumeButton;
	Entity SettingsButton;
	Entity RestartButton;
	Entity MainMenuButton;
	Entity QuitButton;
	AssetHandle MainMenuScene;
	AssetHandle ClickSound;
	AssetHandle FocusSound;

	// What the menu does (the sample's regression test)
	bool Paused = false;
	float TimeScale = 1.0f;          // The game's time scale, as of the latest update

	void OnCreate() override
	{
		Assets::RequestLoad(ClickSound);
		Assets::RequestLoad(FocusSound);
		if (Panel.IsValid())
			Panel.SetActive(false);
		Paused = false;
		TimeScale = Time::GetTimeScale();
	}

	void OnUpdate(float) override
	{
		const bool pausePressed = Input::IsKeyPressed(Key::Escape) || Input::IsGamepadButtonPressed(GamepadButton::Start);
		if (pausePressed && !ThirdPerson::IsAttractMode() && !IsCourseComplete())
		{
			SettingsMenu* settings = GetEntity().GetScript<SettingsMenu>();
			if (!Paused)
				Pause();
			else if (settings && settings->IsOpen())
				settings->Close();
			else
				Resume();
		}
		TimeScale = Time::GetTimeScale();
	}

	void OnUIClick(Entity widget) override
	{
		if (!Paused)
			return;
		if (widget == ResumeButton || widget == SettingsButton || widget == RestartButton || widget == MainMenuButton || widget == QuitButton)
			ThirdPerson::PlayInterfaceSound(ClickSound);
		if (widget == ResumeButton)
		{
			Resume();
		}
		else if (widget == SettingsButton)
		{
			if (SettingsMenu* settings = GetEntity().GetScript<SettingsMenu>())
				settings->Toggle(SettingsButton);
		}
		else if (widget == RestartButton)
		{
			Time::SetTimeScale(1.0f);
			Game::ReloadScene();
		}
		else if (widget == MainMenuButton)
		{
			Time::SetTimeScale(1.0f);
			Game::LoadScene(MainMenuScene);
		}
		else if (widget == QuitButton)
		{
			Game::Quit();
		}
	}

	void OnUIFocusChanged(Entity, bool focused) override
	{
		if (focused && Paused)
			ThirdPerson::PlayInterfaceSound(FocusSound, 0.4f);
	}
private:
	bool IsCourseComplete() const
	{
		const CourseState* course = GetEntity().GetScript<CourseState>();
		return course && course->Completed;
	}

	void Pause()
	{
		Paused = true;
		Time::SetTimeScale(0.0f);
		TimeScale = 0.0f;
		if (Panel.IsValid())
			Panel.SetActive(true);
		if (ResumeButton.IsValid())
			ResumeButton.GetUI().Focus();
		Input::SetCursorMode(CursorMode::Normal);
		Log::Info("ThirdPerson: paused");
	}

	void Resume()
	{
		Paused = false;
		Time::SetTimeScale(1.0f);
		TimeScale = 1.0f;
		if (SettingsMenu* settings = GetEntity().GetScript<SettingsMenu>(); settings && settings->IsOpen())
			settings->Close();
		if (Panel.IsValid())
			Panel.SetActive(false);
		UI::ClearFocus();
		Input::SetCursorMode(CursorMode::Locked);
		Log::Info("ThirdPerson: resumed");
	}
};

ST_SCRIPT_CLASS(PauseMenu)
{
	ST_SCRIPT_FIELD(Panel);
	ST_SCRIPT_FIELD(ResumeButton);
	ST_SCRIPT_FIELD(SettingsButton);
	ST_SCRIPT_FIELD(RestartButton);
	ST_SCRIPT_FIELD(MainMenuButton);
	ST_SCRIPT_FIELD(QuitButton);
	ST_SCRIPT_FIELD(MainMenuScene);
	ST_SCRIPT_FIELD(ClickSound);
	ST_SCRIPT_FIELD(FocusSound);
	ST_SCRIPT_FIELD(Paused);
	ST_SCRIPT_FIELD(TimeScale);
}
