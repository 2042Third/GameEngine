// The export's loading scene: shows how far the island's scene has loaded (Game::GetSceneLoadProgress) on a filled bar
// and a text while the game streams it in.

#include "StrataScript/StrataScript.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace Strata;

class LoadingScreen : public Script
{
public:
	Entity Bar;   // A UI image of type Filled
	Entity Label;

	void OnUpdate(float) override
	{
		const float progress = Game::GetSceneLoadProgress();
		if (progress < 0.0f)
			return; // Nothing loads (yet)
		if (Bar.IsValid())
			Bar.SetProperty("UIImage", "FillAmount", std::clamp(progress, 0.0f, 1.0f));
		if (Label.IsValid())
			Label.GetUI().SetText("Loading the island... " + std::to_string(static_cast<int32_t>(std::lround(progress * 100.0f))) + "%");
	}
};

ST_SCRIPT_CLASS(LoadingScreen)
{
	ST_SCRIPT_FIELD(Bar);
	ST_SCRIPT_FIELD(Label);
}
