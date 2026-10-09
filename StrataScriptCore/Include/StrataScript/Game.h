#pragma once

#include "StrataScript/Host.h"
#include "StrataScript/Scene.h"
#include "StrataScript/Value.h"

#include <cstdint>
#include <string_view>

namespace Strata
{

	// The game as a whole: ending it and switching scenes. Both take effect once the current frame's update is done (scripts
	// keep running until then); quitting wins over a scene switch, and later requests replace earlier ones.
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
		// scripts receive OnDestroy) and the new scene starts. Returns false for assets that are not scenes. The switch loads
		// the scene synchronously, so the frame waits for a scene that is not loaded yet: request it early
		// (Assets::RequestLoad, e.g. while the level runs) to switch without a hitch.
		static bool LoadScene(AssetHandle scene)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(LoadScene);
			return host && scene && host->LoadScene(Detail::GetContext(), scene.ID);
		}

		static bool LoadScene(std::string_view path) { return LoadScene(Assets::Find(path)); }

		// Starts the running scene over (e.g. after the player lost): an exported game loads it again from its asset; the
		// editor restarts what play mode runs (the edited scene with its unsaved changes, or the scene asset switched to last).
		static bool ReloadScene()
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(LoadScene);
			return host && host->LoadScene(Detail::GetContext(), 0);
		}
	};

}
