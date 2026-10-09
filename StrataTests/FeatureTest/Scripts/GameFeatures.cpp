// The game flow (Game) on "Game Features". The scene sets QuitFrame to the frame the runners play after the scenario: in it,
// the script asks for scene loads and then to quit with QuitCode, and the runners check that whoever runs the scene (the
// headless scene itself, the editor, the game runtime) honors the quit.

#include "FeatureScript.h"

using namespace Strata;
using namespace FeatureTest;

class GameFeatures : public FeatureScript
{
public:
	int32_t QuitFrame = 0;
	int32_t QuitCode = 0;

	void OnCreate() override
	{
		Journal(*this, "GameFeatures", "OnCreate");
		Expect(QuitFrame > 0 && QuitCode != 0, "the scene sets the quit frame and exit code");
		// What remains happens after the scenario, and the runners check it.
		Completed = true;
	}

	void OnUpdate(float) override
	{
		if (GetFrame() != QuitFrame)
			return;

		const AssetHandle scene = Assets::Find("Scenes/Feature.stscene");
		Expect(Game::LoadScene(scene), "Game::LoadScene(handle)");
		Expect(Game::LoadScene("Scenes/Feature.stscene"), "Game::LoadScene(path)");
		Expect(Game::ReloadScene(), "Game::ReloadScene");
		Game::Quit(QuitCode);
		Journal(*this, "GameFeatures", "Quit");
	}
};

ST_SCRIPT_CLASS(GameFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(QuitFrame);
	ST_SCRIPT_FIELD(QuitCode);
}
