#pragma once

#include "StrataScript/StrataScript.h"

#include <cstdint>

// The course's state, on its "Game" entity: the coins collected (the coins call AddCoin), the time, the completion panel
// with the best time (kept in the save file ThirdPerson::c_BestTimeFile) once the player reaches the goal, and the attract
// mode's ending: any key, click or button, or the end of its walk, loads the main menu again.
class CourseState : public Strata::Script
{
public:
	Strata::AssetHandle MainMenuScene;
	Strata::AssetHandle CoinSound;
	Strata::AssetHandle CompleteSound;
	Strata::Entity CoinsText;        // "3 / 12"
	Strata::Entity TimeText;
	Strata::Entity CompletePanel;    // Shown when the player reaches the goal
	Strata::Entity CompleteTimeText;
	Strata::Entity CompleteBestText;
	Strata::Entity PlayAgainButton;
	Strata::Entity CompleteMenuButton;
	Strata::Entity AttractBanner;    // Shown in the attract mode
	Strata::Entity Player;
	float AttractEndSeconds = 3.0f;  // How long the attract mode lingers after its walk

	// What happened (the HUD, and the sample's regression test)
	int32_t Coins = 0;
	int32_t TotalCoins = 0;          // The scene's entities tagged "Coin"
	float ElapsedSeconds = 0.0f;
	bool Completed = false;
	float BestSeconds = 0.0f;        // 0: no best time yet

	void OnCreate() override;
	void OnUpdate(float deltaTime) override;
	void OnUIClick(Strata::Entity widget) override;

	void AddCoin(const glm::vec3& position);
	void Complete();
private:
	void ReadBestTime();
	void ShowHud();
	void ReturnToMenu();
private:
	float m_AttractSeconds = 0.0f;   // Since the attract mode started (input in its first moment is the menu's)
	float m_AttractDoneSeconds = 0.0f;
	bool m_Leaving = false;
};
