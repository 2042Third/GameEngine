#include "ThirdPerson.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <optional>

using namespace Strata;

namespace ThirdPerson
{

	namespace
	{

		GameSettings s_Settings;
		bool s_SettingsRead = false;
		bool s_DisplayApplied = false;
		bool s_AttractMode = false;

		// The text after `"key":` in a flat JSON object, up to the next ',' or '}' (whitespace trimmed); nothing when the key
		// is missing.
		std::optional<std::string_view> FindValue(std::string_view text, std::string_view key)
		{
			const std::string quoted = "\"" + std::string(key) + "\"";
			const size_t keyAt = text.find(quoted);
			if (keyAt == std::string_view::npos)
				return std::nullopt;
			size_t at = text.find(':', keyAt + quoted.size());
			if (at == std::string_view::npos)
				return std::nullopt;
			at++;
			const size_t end = text.find_first_of(",}", at);
			std::string_view value = text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
			while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
				value.remove_prefix(1);
			while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
				value.remove_suffix(1);
			return value;
		}

		void ReadFloat(std::string_view text, std::string_view key, float minimum, float maximum, float& value)
		{
			const std::optional<std::string_view> found = FindValue(text, key);
			if (!found)
				return;
			// std::from_chars has no float overload in every standard library the sample builds with (Apple Clang 15).
			const std::string copy(*found);
			char* end = nullptr;
			const float parsed = std::strtof(copy.c_str(), &end);
			if (end != copy.c_str() && std::isfinite(parsed))
				value = std::clamp(parsed, minimum, maximum);
		}

		void ReadBool(std::string_view text, std::string_view key, bool& value)
		{
			const std::optional<std::string_view> found = FindValue(text, key);
			if (found && *found == "true")
				value = true;
			else if (found && *found == "false")
				value = false;
		}

	}

	GameSettings& GetSettings()
	{
		if (!s_SettingsRead)
		{
			s_SettingsRead = true;
			if (const std::optional<std::string> text = Game::ReadSaveText(c_SettingsFile))
				s_Settings = ParseSettings(*text);
		}
		return s_Settings;
	}

	bool SaveSettings(bool display)
	{
		const bool saved = Game::WriteSaveFile(c_SettingsFile, FormatSettings(GetSettings()));
		if (saved)
			Log::Info("ThirdPerson: settings saved");
		else
			Log::Warn("ThirdPerson: the settings could not be saved");
		ApplySettings(display);
		return saved;
	}

	void ApplySettings(bool display)
	{
		const GameSettings& settings = GetSettings();
		Audio::SetMasterVolume(settings.MasterVolume);
		if (!display && s_DisplayApplied)
			return;
		s_DisplayApplied = true;
		Game::SetWindowMode(settings.Fullscreen ? WindowMode::Borderless : WindowMode::Windowed);
		Game::SetVSync(settings.VSync);
	}

	std::string FormatSettings(const GameSettings& settings)
	{
		char text[256];
		std::snprintf(text, sizeof(text),
			"{\n\t\"MasterVolume\": %.3f,\n\t\"MouseSensitivity\": %.3f,\n\t\"InvertY\": %s,\n\t\"Fullscreen\": %s,\n\t\"VSync\": %s\n}\n",
			settings.MasterVolume, settings.MouseSensitivity, settings.InvertY ? "true" : "false", settings.Fullscreen ? "true" : "false",
			settings.VSync ? "true" : "false");
		return text;
	}

	GameSettings ParseSettings(std::string_view text)
	{
		GameSettings settings;
		ReadFloat(text, "MasterVolume", 0.0f, 1.0f, settings.MasterVolume);
		ReadFloat(text, "MouseSensitivity", 0.25f, 3.0f, settings.MouseSensitivity);
		ReadBool(text, "InvertY", settings.InvertY);
		ReadBool(text, "Fullscreen", settings.Fullscreen);
		ReadBool(text, "VSync", settings.VSync);
		return settings;
	}

	bool IsAttractMode()
	{
		return s_AttractMode;
	}

	void SetAttractMode(bool attract)
	{
		s_AttractMode = attract;
	}

	bool AnyInputPressed()
	{
		constexpr KeyCode c_Keys[] = { Key::W, Key::A, Key::S, Key::D, Key::Up, Key::Down, Key::Left, Key::Right, Key::Space, Key::Enter, Key::Escape,
			Key::Tab, Key::LeftShift, Key::RightShift, Key::R, Key::D1, Key::D2 };
		for (const KeyCode key : c_Keys)
		{
			if (Input::IsKeyPressed(key))
				return true;
		}
		if (Input::IsMouseButtonPressed(Mouse::ButtonLeft) || Input::IsMouseButtonPressed(Mouse::ButtonRight))
			return true;
		constexpr GamepadButton c_Buttons[] = { GamepadButton::A, GamepadButton::B, GamepadButton::X, GamepadButton::Y, GamepadButton::Start,
			GamepadButton::Back, GamepadButton::DPadUp, GamepadButton::DPadDown, GamepadButton::DPadLeft, GamepadButton::DPadRight };
		for (const GamepadButton button : c_Buttons)
		{
			if (Input::IsGamepadButtonPressed(button))
				return true;
		}
		return false;
	}

	std::string FormatTime(float seconds)
	{
		const int32_t tenths = static_cast<int32_t>(std::lround(std::max(0.0f, seconds) * 10.0f));
		char text[32];
		std::snprintf(text, sizeof(text), "%d:%02d.%d", tenths / 600, (tenths / 10) % 60, tenths % 10);
		return text;
	}

	int32_t GetAnimatorStateIndex(std::string_view state)
	{
		for (size_t index = 0; index < c_AnimatorStates.size(); index++)
		{
			if (c_AnimatorStates[index] == state)
				return static_cast<int32_t>(index);
		}
		return -1;
	}

	void PlayInterfaceSound(AssetHandle clip, float volume)
	{
		if (clip.IsValid())
			Audio::PlayOneShot(clip, volume);
	}

}
