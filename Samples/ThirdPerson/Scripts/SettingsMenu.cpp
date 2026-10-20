#include "SettingsMenu.h"

#include "ThirdPerson.h"

using namespace Strata;

void SettingsMenu::OnCreate()
{
	Assets::RequestLoad(ToggleSound);
	if (Panel.IsValid())
		Panel.SetActive(false);
}

void SettingsMenu::OnUpdate(float)
{
	if (m_SavePending && Time::GetRealTime() - m_LastChange >= static_cast<double>(SaveDelaySeconds))
		Save(false);
}

void SettingsMenu::OnUIClick(Entity widget)
{
	if (widget == BackButton)
		Close();
}

void SettingsMenu::OnUIValueChanged(Entity widget, float value)
{
	ThirdPerson::GameSettings& settings = ThirdPerson::GetSettings();
	if (widget == VolumeSlider || widget == SensitivitySlider)
	{
		if (widget == VolumeSlider)
			settings.MasterVolume = value;
		else
			settings.MouseSensitivity = value;
		ThirdPerson::ApplySettings(false);
		m_SavePending = true;
		m_LastChange = Time::GetRealTime();
		return;
	}
	if (widget != InvertYToggle && widget != FullscreenToggle && widget != VSyncToggle)
		return;
	const bool on = value != 0.0f;
	if (widget == InvertYToggle)
		settings.InvertY = on;
	else if (widget == FullscreenToggle)
		settings.Fullscreen = on;
	else
		settings.VSync = on;
	ThirdPerson::PlayInterfaceSound(ToggleSound);
	Save(widget != InvertYToggle);
}

bool SettingsMenu::IsOpen() const
{
	return Panel.IsValid() && Panel.IsActive();
}

void SettingsMenu::Open(Entity returnFocus)
{
	if (!Panel.IsValid())
		return;
	m_ReturnFocus = returnFocus;
	Panel.SetActive(true);
	ShowValues();
	if (VolumeSlider.IsValid())
		VolumeSlider.GetUI().Focus();
}

void SettingsMenu::Close()
{
	if (m_SavePending)
		Save(false);
	if (Panel.IsValid())
		Panel.SetActive(false);
	if (m_ReturnFocus.IsValid())
		m_ReturnFocus.GetUI().Focus();
}

void SettingsMenu::Toggle(Entity returnFocus)
{
	if (IsOpen())
		Close();
	else
		Open(returnFocus);
}

void SettingsMenu::ShowValues()
{
	// Values set here raise no OnUIValueChanged: only the player's changes save.
	const ThirdPerson::GameSettings& settings = ThirdPerson::GetSettings();
	if (VolumeSlider.IsValid())
		VolumeSlider.GetUI().SetValue(settings.MasterVolume);
	if (SensitivitySlider.IsValid())
		SensitivitySlider.GetUI().SetValue(settings.MouseSensitivity);
	if (InvertYToggle.IsValid())
		InvertYToggle.GetUI().SetValue(settings.InvertY ? 1.0f : 0.0f);
	if (FullscreenToggle.IsValid())
		FullscreenToggle.GetUI().SetValue(settings.Fullscreen ? 1.0f : 0.0f);
	if (VSyncToggle.IsValid())
		VSyncToggle.GetUI().SetValue(settings.VSync ? 1.0f : 0.0f);
}

void SettingsMenu::Save(bool display)
{
	m_SavePending = false;
	if (ThirdPerson::SaveSettings(display))
		Saves++;
}

ST_SCRIPT_CLASS(SettingsMenu)
{
	ST_SCRIPT_FIELD(Panel);
	ST_SCRIPT_FIELD(VolumeSlider);
	ST_SCRIPT_FIELD(SensitivitySlider);
	ST_SCRIPT_FIELD(InvertYToggle);
	ST_SCRIPT_FIELD(FullscreenToggle);
	ST_SCRIPT_FIELD(VSyncToggle);
	ST_SCRIPT_FIELD(BackButton);
	ST_SCRIPT_FIELD(ToggleSound);
	ST_SCRIPT_FIELD(SaveDelaySeconds);
	ST_SCRIPT_FIELD(Saves);
}
