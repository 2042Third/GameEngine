#pragma once

#include "StrataScript/Host.h"
#include "StrataScript/Scene.h"
#include "StrataScript/Value.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// How the game's window shows (Game::SetWindowMode).
	enum class WindowMode : uint32_t
	{
		Windowed = StrataScriptWindowMode_Windowed,
		Fullscreen = StrataScriptWindowMode_Fullscreen, // Exclusive: the display switches to the game's resolution
		Borderless = StrataScriptWindowMode_Borderless  // A window covering the display at the desktop's resolution
	};

	// The game as a whole: ending it and switching scenes, its window (display options) and its save files. Quitting and scene
	// switches take effect once the current frame's update is done (scripts keep running until then); quitting wins over a
	// scene switch, a request for another scene replaces an earlier one, and later window requests replace earlier ones.
	class Game
	{
	public:
		// Ends the game: an exported game exits with `exitCode` (0 means success), the editor stops play mode.
		static void Quit(int32_t exitCode = 0)
		{
			if (const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(QuitGame))
				host->QuitGame(Detail::GetContext(), exitCode);
		}

		// Replaces the running scene with a scene asset of the project: every entity of the current scene goes away (its
		// scripts receive OnDestroy) and the new scene starts. Returns false for assets that are not scenes. The scene and
		// what it uses load in the background while the current scene goes on (show GetSceneLoadProgress); the switch happens
		// once it is in (with what it uses where that fits in the memory budgets, else the new scene streams it in as it draws
		// it; other scenes it names stay unloaded).
		// Asking again for the scene that is loading changes nothing, so calling this in every update while a condition holds
		// is fine.
		static bool LoadScene(AssetHandle scene)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(LoadScene);
			return host && scene && host->LoadScene(Detail::GetContext(), scene.ID);
		}

		static bool LoadScene(std::string_view path) { return LoadScene(Assets::Find(path)); }

		// Starts the running scene over (e.g. after the player lost): an exported game loads it again from its asset (asking
		// again while that load is under way changes nothing); the editor restarts what play mode runs (the edited scene with
		// its unsaved changes, or the scene asset switched to last).
		static bool ReloadScene()
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(LoadScene);
			return host && host->LoadScene(Detail::GetContext(), 0);
		}

		// How far the scene LoadScene asked for has loaded, in [0, 1] (a loading bar while the current scene goes on), or a
		// negative value while no switch is under way (and with engines that predate it).
		static float GetSceneLoadProgress()
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(GetSceneLoadProgress);
			return host ? host->GetSceneLoadProgress(Detail::GetContext()) : -1.0f;
		}

		// Display options. The requests apply after the frame: an exported game changes its window; the editor's play mode
		// only records them (editor.status, a notice over the game view), its own window stays; a headless game accepts them.
		// False for refused requests (a resolution outside 1 to 16384 pixels).
		static bool SetWindowMode(WindowMode mode)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(SetWindowMode);
			return host && host->SetWindowMode(Detail::GetContext(), static_cast<uint32_t>(mode));
		}

		// Pixels: the window's in windowed mode, the display mode's in fullscreen (borderless keeps the desktop's).
		static bool SetResolution(uint32_t width, uint32_t height)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(SetResolution);
			return host && host->SetResolution(Detail::GetContext(), width, height);
		}

		static bool SetVSync(bool enabled)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(SetVSync);
			return host && host->SetVSync(Detail::GetContext(), enabled);
		}

		// The pixels the game renders into now (the window, or the editor's game view); zero when unknown.
		static glm::uvec2 GetWindowSize()
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(GetWindowSize);
			uint32_t size[2] = { 0, 0 };
			if (!host || !host->GetWindowSize(Detail::GetContext(), size))
				return glm::uvec2(0);
			return glm::uvec2(size[0], size[1]);
		}

		// Save files: named blobs in the game's save directory (an exported game: <user data>/<Game>/Saves; the editor:
		// <project>/.strata/Saves). Names are 1 to 64 ASCII letters, digits, spaces, '-', '_' and '.' (not at the start or end),
		// without "..": "Slot 1", "settings.json". A write replaces the file whole (never half written; at most 64 MiB). The
		// calls read and write the disk before they return: keep saves small, and save at quiet moments (checkpoints, menus).
		static bool WriteSaveFile(std::string_view name, std::span<const uint8_t> bytes)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(WriteSaveFile);
			return host && host->WriteSaveFile(Detail::GetContext(), Detail::ToABIString(name), bytes.data(), static_cast<uint64_t>(bytes.size()));
		}

		static bool WriteSaveFile(std::string_view name, std::string_view text)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(WriteSaveFile);
			return host && host->WriteSaveFile(Detail::GetContext(), Detail::ToABIString(name), text.data(), static_cast<uint64_t>(text.size()));
		}

		// Nothing when there is no such save file (yet).
		static std::optional<std::vector<uint8_t>> ReadSaveFile(std::string_view name)
		{
			std::optional<std::string> text = ReadSaveText(name);
			if (!text)
				return std::nullopt;
			return std::vector<uint8_t>(text->begin(), text->end());
		}

		static std::optional<std::string> ReadSaveText(std::string_view name)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(ReadSaveFile);
			if (!host)
				return std::nullopt;
			char stackBuffer[256];
			uint64_t size = 0;
			if (!host->ReadSaveFile(Detail::GetContext(), Detail::ToABIString(name), stackBuffer, sizeof(stackBuffer), &size))
				return std::nullopt;
			if (size <= sizeof(stackBuffer))
				return std::string(stackBuffer, static_cast<size_t>(size));
			// A larger file: read again into a buffer of its size (it changes between the calls only if a script rewrote it,
			// which cannot happen in between: calls are single-threaded).
			std::string text(static_cast<size_t>(size), '\0');
			uint64_t secondSize = 0;
			if (!host->ReadSaveFile(Detail::GetContext(), Detail::ToABIString(name), text.data(), static_cast<uint64_t>(text.size()), &secondSize))
				return std::nullopt;
			text.resize(static_cast<size_t>(secondSize < size ? secondSize : size));
			return text;
		}

		// False when there is no such save file.
		static bool DeleteSaveFile(std::string_view name)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(DeleteSaveFile);
			return host && host->DeleteSaveFile(Detail::GetContext(), Detail::ToABIString(name));
		}

		// The names of the game's save files, sorted.
		static std::vector<std::string> ListSaveFiles()
		{
			std::vector<std::string> names;
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(ListSaveFiles);
			if (!host)
				return names;
			const std::string text = Detail::ReadHostString([&](char* buffer, uint64_t capacity) { return host->ListSaveFiles(Detail::GetContext(), buffer, capacity); });
			size_t start = 0;
			for (size_t end = text.find('\n'); end != std::string::npos; end = text.find('\n', start))
			{
				names.push_back(text.substr(start, end - start));
				start = end + 1;
			}
			return names;
		}
	};

}
