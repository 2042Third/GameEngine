#pragma once

#include "StrataScript/StrataScript.h"

#include <cstdint>

// The settings panel of the main menu and the pause menu, on the scene's "Game" entity (the panel's widgets send it their
// events): master volume and mouse sensitivity sliders, invert-Y, fullscreen and vsync toggles. Every change applies at
// once and is written to the save file ThirdPerson::c_SettingsFile (a toggle at once, a slider once it rests); the
// settings are read from it when the game starts.
class SettingsMenu : public Strata::Script
{
public:
	Strata::Entity Panel;
	Strata::Entity VolumeSlider;
	Strata::Entity SensitivitySlider;
	Strata::Entity InvertYToggle;
	Strata::Entity FullscreenToggle;
	Strata::Entity VSyncToggle;
	Strata::Entity BackButton;
	Strata::AssetHandle ToggleSound;
	float SaveDelaySeconds = 0.4f;   // How long a slider rests before its value is saved
	int32_t Saves = 0;

	void OnCreate() override;
	void OnUpdate(float deltaTime) override;
	void OnUIClick(Strata::Entity widget) override;
	void OnUIValueChanged(Strata::Entity widget, float value) override;

	bool IsOpen() const;
	// Shows the panel with the current settings; `returnFocus` gets the focus back when it closes.
	void Open(Strata::Entity returnFocus);
	void Close();
	void Toggle(Strata::Entity returnFocus);
private:
	void ShowValues();
	void Save(bool display);
private:
	Strata::Entity m_ReturnFocus;
	bool m_SavePending = false;
	double m_LastChange = 0.0;
};
