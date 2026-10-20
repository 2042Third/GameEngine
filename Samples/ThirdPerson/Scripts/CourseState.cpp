#include "CourseState.h"

#include "ThirdPerson.h"
#include "ThirdPersonPlayer.h"

#include <cstdlib>
#include <optional>
#include <string>

using namespace Strata;

void CourseState::OnCreate()
{
	TotalCoins = static_cast<int32_t>(Scene::FindEntitiesByTag("Coin").size());
	ReadBestTime();
	Assets::RequestLoad(CoinSound);
	Assets::RequestLoad(CompleteSound);
	ThirdPerson::ApplySettings(false);
	const bool attract = ThirdPerson::IsAttractMode();
	if (CompletePanel.IsValid())
		CompletePanel.SetActive(false);
	if (AttractBanner.IsValid())
		AttractBanner.SetActive(attract);
	Input::SetCursorMode(attract ? CursorMode::Normal : CursorMode::Locked);
	Time::SetTimeScale(1.0f);
	ShowHud();
	Log::Info("ThirdPerson: course loaded (", TotalCoins, " coins", attract ? ", attract mode)" : ")");
}

void CourseState::OnUpdate(float deltaTime)
{
	if (!Completed)
		ElapsedSeconds += deltaTime;
	ShowHud();
	if (!ThirdPerson::IsAttractMode() || m_Leaving)
		return;
	m_AttractSeconds += Time::GetUnscaledDeltaTime();
	const ThirdPersonPlayer* player = Player.IsValid() ? Player.GetScript<ThirdPersonPlayer>() : nullptr;
	if (player && player->AttractFinished)
		m_AttractDoneSeconds += Time::GetUnscaledDeltaTime();
	if ((m_AttractSeconds > 0.5f && ThirdPerson::AnyInputPressed()) || m_AttractDoneSeconds > AttractEndSeconds)
		ReturnToMenu();
}

void CourseState::OnUIClick(Entity widget)
{
	if (widget == PlayAgainButton)
	{
		Time::SetTimeScale(1.0f);
		Game::ReloadScene();
	}
	else if (widget == CompleteMenuButton)
	{
		Time::SetTimeScale(1.0f);
		Game::LoadScene(MainMenuScene);
	}
}

void CourseState::AddCoin(const glm::vec3& position)
{
	Coins++;
	if (CoinSound.IsValid())
		Audio::PlayOneShotAt(CoinSound, position, 0.8f);
	ShowHud();
	Log::Info("ThirdPerson: coin ", Coins, " of ", TotalCoins);
}

void CourseState::Complete()
{
	if (Completed || ThirdPerson::IsAttractMode())
		return;
	Completed = true;
	const bool best = BestSeconds <= 0.0f || ElapsedSeconds < BestSeconds;
	if (best)
	{
		BestSeconds = ElapsedSeconds;
		if (!Game::WriteSaveFile(ThirdPerson::c_BestTimeFile, std::to_string(BestSeconds)))
			Log::Warn("ThirdPerson: the best time could not be saved");
	}
	if (CompleteSound.IsValid())
		Audio::PlayOneShot(CompleteSound, 0.8f);
	if (CompletePanel.IsValid())
		CompletePanel.SetActive(true);
	if (CompleteTimeText.IsValid())
		CompleteTimeText.GetUI().SetText("Time  " + ThirdPerson::FormatTime(ElapsedSeconds) + "    Coins  " + std::to_string(Coins) + " / " + std::to_string(TotalCoins));
	if (CompleteBestText.IsValid())
		CompleteBestText.GetUI().SetText(best ? "A new best time!" : "Best  " + ThirdPerson::FormatTime(BestSeconds));
	if (PlayAgainButton.IsValid())
		PlayAgainButton.GetUI().Focus();
	Input::SetCursorMode(CursorMode::Normal);
	Log::Info("ThirdPerson: course complete in ", ThirdPerson::FormatTime(ElapsedSeconds), " with ", Coins, " of ", TotalCoins, " coins");
}

void CourseState::ReadBestTime()
{
	BestSeconds = 0.0f;
	const std::optional<std::string> text = Game::ReadSaveText(ThirdPerson::c_BestTimeFile);
	if (!text)
		return;
	char* end = nullptr;
	const float seconds = std::strtof(text->c_str(), &end);
	if (end != text->c_str() && seconds > 0.0f && seconds < 86400.0f)
		BestSeconds = seconds;
}

void CourseState::ShowHud()
{
	if (CoinsText.IsValid())
		CoinsText.GetUI().SetText(std::to_string(Coins) + " / " + std::to_string(TotalCoins));
	if (TimeText.IsValid())
		TimeText.GetUI().SetText(ThirdPerson::FormatTime(ElapsedSeconds));
}

void CourseState::ReturnToMenu()
{
	m_Leaving = true;
	ThirdPerson::SetAttractMode(false);
	Log::Info("ThirdPerson: the attract mode is over");
	Game::LoadScene(MainMenuScene);
}

ST_SCRIPT_CLASS(CourseState)
{
	ST_SCRIPT_FIELD(MainMenuScene);
	ST_SCRIPT_FIELD(CoinSound);
	ST_SCRIPT_FIELD(CompleteSound);
	ST_SCRIPT_FIELD(CoinsText);
	ST_SCRIPT_FIELD(TimeText);
	ST_SCRIPT_FIELD(CompletePanel);
	ST_SCRIPT_FIELD(CompleteTimeText);
	ST_SCRIPT_FIELD(CompleteBestText);
	ST_SCRIPT_FIELD(PlayAgainButton);
	ST_SCRIPT_FIELD(CompleteMenuButton);
	ST_SCRIPT_FIELD(AttractBanner);
	ST_SCRIPT_FIELD(Player);
	ST_SCRIPT_FIELD(AttractEndSeconds);
	ST_SCRIPT_FIELD(Coins);
	ST_SCRIPT_FIELD(TotalCoins);
	ST_SCRIPT_FIELD(ElapsedSeconds);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(BestSeconds);
}
