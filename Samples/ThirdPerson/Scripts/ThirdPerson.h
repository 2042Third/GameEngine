#pragma once

// What the sample's scripts share across its scenes. The script module stays loaded while the game switches scenes
// (Game::LoadScene), so the main menu tells the course through it that the course runs as the attract mode's demo, and
// the settings are read from their save file once and kept here.

#include "StrataScript/StrataScript.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace ThirdPerson
{

	struct GameSettings
	{
		float MasterVolume = 1.0f;     // 0 to 1
		float MouseSensitivity = 1.0f; // 0.25 to 3: times the camera's degrees per pixel of mouse movement
		bool InvertY = false;          // Moving the mouse up looks down
		bool Fullscreen = false;       // A borderless window covering the display
		bool VSync = true;
	};

	// The save file the settings menu writes.
	constexpr std::string_view c_SettingsFile = "settings.json";
	// The save file that keeps the best time of the course.
	constexpr std::string_view c_BestTimeFile = "best-time.txt";

	// The settings the game runs with: read from the save file the first time they are asked for (defaults for what the
	// file lacks, or without one).
	GameSettings& GetSettings();
	// Writes the settings to their save file and applies them (volume always, the window when `display`); false when the
	// write failed (logged).
	bool SaveSettings(bool display);
	// Applies the volume and, once per run of the game (or when `display`), the window mode and vsync.
	void ApplySettings(bool display);
	// The settings as the save file holds them (a flat JSON object), and back: values a text lacks or holds out of range
	// keep their defaults or are clamped.
	std::string FormatSettings(const GameSettings& settings);
	GameSettings ParseSettings(std::string_view text);

	// The attract mode: the main menu's demo, in which the course plays itself until the player touches anything.
	bool IsAttractMode();
	void SetAttractMode(bool attract);

	// Whether the player pressed any key, mouse button or gamepad button this frame (the attract mode hands over on it).
	bool AnyInputPressed();

	// "1:05.3" for 65.3 seconds.
	std::string FormatTime(float seconds);

	// The player's animation states, in the order ThirdPersonPlayer::AnimatorState numbers them.
	constexpr std::array<std::string_view, 6> c_AnimatorStates = { "Idle", "Walk", "Run", "Jump", "Wave", "Dance" };

	// The index of a state in c_AnimatorStates, -1 for another (or none).
	int32_t GetAnimatorStateIndex(std::string_view state);

	// Plays an interface sound (clicks, focus changes) if it is set.
	void PlayInterfaceSound(Strata::AssetHandle clip, float volume = 0.6f);

}
