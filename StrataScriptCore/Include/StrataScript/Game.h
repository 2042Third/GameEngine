#pragma once

#include "StrataScript/Host.h"
#include "StrataScript/Scene.h"
#include "StrataScript/Value.h"

#include <cstdint>
#include <string_view>

namespace Strata
{

	// The game as a whole: ending it and switching scenes. Both take effect once the current frame's update is done (scripts
	// keep running until then); quitting wins over a scene switch, and a request for another scene replaces an earlier one.
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
	};

}
