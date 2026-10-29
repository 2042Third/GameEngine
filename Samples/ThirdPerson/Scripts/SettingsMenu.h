#pragma once

#include "StrataScript/StrataScript.h"

#include <cstdint>
#include <optional>

// The settings panel of the main menu and the pause menu, on the scene's "Game" entity (the panel's widgets send it their
// events): master volume and mouse sensitivity sliders, invert-Y, fullscreen and vsync toggles, and the Graphics row (Low,
// Medium, High, Ultra: one toggle group). Every change applies at once and is written to the save file
// ThirdPerson::c_SettingsFile (a toggle at once, a slider once it rests); the settings are read from it when the game
// starts. The graphics preset is the engine's players' option (Graphics::SetPreset), kept by Graphics::Save in the engine's
// file beside the game's saves, which the engine loads when the game starts.
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
	Strata::Entity GraphicsLow;
	Strata::Entity GraphicsMedium;
	Strata::Entity GraphicsHigh;
	Strata::Entity GraphicsUltra;
	Strata::AssetHandle ToggleSound;
	float SaveDelaySeconds = 0.4f;   // How long a slider rests before its value is saved
	int32_t Saves = 0;
	int32_t GraphicsSaves = 0; // Times the graphics options were saved

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
	// The preset of a Graphics row toggle, if the widget is one.
	std::optional<Strata::QualityPreset> GetGraphicsPreset(Strata::Entity widget) const;
	void Save(bool display);
private:
	Strata::Entity m_ReturnFocus;
	bool m_SavePending = false;
	double m_LastChange = 0.0;
};
