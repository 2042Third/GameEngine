// The island's HUD and pause menu, on the "Game" entity: the time of day, Esc pauses (the time scale goes to 0; the UI
// runs on unscaled time) with Resume, a day-speed slider, the tour again, Credits and Quit.

#include "DayNight.h"

#include "StrataScript/StrataScript.h"

#include <cmath>
#include <string>

using namespace Strata;

class IslandHud : public Script
{
public:
	Entity ClockText;
	Entity PauseMenu;
	Entity CreditsPanel;
	bool Paused = false;

	void OnUpdate(float) override
	{
		const DayNight* dayNight = Scene::FindEntityByName("Sun").GetScript<DayNight>();
		if (dayNight && ClockText.IsValid())
		{
			const float time = dayNight->TimeOfDay;
			const int32_t hours = static_cast<int32_t>(time);
			const int32_t minutes = static_cast<int32_t>((time - static_cast<float>(hours)) * 60.0f);
			ClockText.GetUI().SetText(std::to_string(hours) + (minutes < 10 ? ":0" : ":") + std::to_string(minutes));
		}
		if (Input::IsKeyPressed(Key::Escape))
			SetPaused(!Paused);
	}

	void OnUIClick(Entity widget) override
	{
		const std::string name = widget.GetName();
		if (name == "Resume")
			SetPaused(false);
		else if (name == "Credits" && CreditsPanel.IsValid())
			CreditsPanel.SetActive(!CreditsPanel.IsActive());
		else if (name == "RestartTour")
		{
			SetPaused(false);
			Game::ReloadScene();
		}
		else if (name == "Quit")
			Game::Quit(0);
	}

	void OnUIValueChanged(Entity widget, float value) override
	{
		if (widget.GetName() != "DaySpeed")
			return;
		if (Entity sun = Scene::FindEntityByName("Sun"); sun.IsValid())
		{
			if (DayNight* dayNight = sun.GetScript<DayNight>())
				dayNight->DayLengthSeconds = value;
		}
	}
private:
	void SetPaused(bool paused)
	{
		Paused = paused;
		Time::SetTimeScale(paused ? 0.0f : 1.0f);
		if (PauseMenu.IsValid())
			PauseMenu.SetActive(paused);
		if (!paused && CreditsPanel.IsValid())
			CreditsPanel.SetActive(false);
		if (paused)
		{
			if (Entity resume = Scene::FindEntityByName("Resume"); resume.IsValid())
				resume.GetUI().Focus();
		}
	}
};

ST_SCRIPT_CLASS(IslandHud)
{
	ST_SCRIPT_FIELD(ClockText);
	ST_SCRIPT_FIELD(PauseMenu);
	ST_SCRIPT_FIELD(CreditsPanel);
	ST_SCRIPT_FIELD(Paused);
}
