// Scripts of the Scripting.Game tests (StrataTests/src/Scripting/ScriptGameTests.cpp): quitting the game and switching
// scenes from scripts.

#include "TestScripts.h"

#include <cstddef>
#include <cstdint>
#include <string>

using namespace Strata;
using namespace ScriptTests;

// Asks for scene loads (by handle and by path, then a restart) and to quit with ExitCode, on its first update. The engine
// side checks the requests the scene holds afterwards: the quit and the restart (the last load request).
class GameFlow : public CheckingScript
{
public:
	AssetHandle Level;
	std::string LevelPath;
	int32_t ExitCode = 0;
	bool Done = false;

	void OnUpdate(float) override
	{
		if (Done)
			return;
		Done = true;

		Expect(Game::LoadScene(Level), "Game::LoadScene(handle)");
		Expect(Game::LoadScene(LevelPath), "Game::LoadScene(path)");
		Expect(Game::ReloadScene(), "Game::ReloadScene");
		Game::Quit(ExitCode);
		Expect(GetEntity().IsValid(), "scripts keep running until the frame ends");
	}
};

ST_SCRIPT_CLASS(GameFlow)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Level);
	ST_SCRIPT_FIELD(LevelPath);
	ST_SCRIPT_FIELD(ExitCode);
	ST_SCRIPT_FIELD(Done);
}

// Scene loads that are refused (the scene then holds no load request), and the game functions on an engine that predates
// them (a host table that ends before them: nothing is requested).
class GameFlowMisuse : public CheckingScript
{
public:
	AssetHandle NotAScene; // An asset of another type
	bool Done = false;

	void OnUpdate(float) override
	{
		if (Done)
			return;
		Done = true;

		const StrataScriptHostAPI* host = Detail::GetHost();
		StrataScriptContext* context = Detail::GetContext();
		Expect(!Game::LoadScene(AssetHandle()), "the null asset is no scene (Game::ReloadScene restarts)");
		Expect(!host->LoadScene(context, 0x51DE5u), "an asset that does not exist");
		Expect(!Game::LoadScene(NotAScene), "an asset that is not a scene");
		Expect(!Game::LoadScene("Levels/Missing.stscene"), "a path without an asset");

		StrataScriptHostAPI older = *host;
		older.StructSize = static_cast<uint32_t>(offsetof(StrataScriptHostAPI, QuitGame));
		Detail::s_Host = &older;
		Game::Quit(9);
		const bool loaded = Game::ReloadScene();
		Detail::s_Host = host;
		Expect(!loaded, "an engine without the functions loads nothing");
	}
};

ST_SCRIPT_CLASS(GameFlowMisuse)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(NotAScene);
	ST_SCRIPT_FIELD(Done);
}
