// The gameplay helpers (Random, Timer, KeyRepeat) and the game flow (Game) on "Game Features". The scene sets QuitFrame to
// the frame the runners play after the scenario: in it, the script asks for scene loads and then to quit with QuitCode,
// and the runners check that whoever runs the scene (the headless scene itself, the editor, the game runtime) honors the
// quit.

#include "FeatureScript.h"

using namespace Strata;
using namespace FeatureTest;

class GameFeatures : public FeatureScript
{
public:
	// The helpers are members, not fields: the hot reload (after frame 100) starts them over, so they finish before.
	static constexpr int32_t c_TickFrames = 60;
	static constexpr int32_t c_CheckFrame = 90;

	int32_t QuitFrame = 0;
	int32_t QuitCode = 0;
	int32_t CountdownFrame = -1; // The frame the one-shot timer elapsed in
	int32_t Ticks = 0;           // Times the repeating timer elapsed during the first c_TickFrames frames
	int32_t KeyRepeats = 0;      // Frames KeyRepeat fired in

	void OnCreate() override
	{
		Journal(*this, "GameFeatures", "OnCreate");
		Expect(QuitFrame > 0 && QuitCode != 0, "the scene sets the quit frame and exit code");
		CheckRandom();
		m_Countdown.Start(0.5f);
		m_Ticker.Start(0.1f, true);
	}

	void OnUpdate(float deltaTime) override
	{
		const int32_t frame = GetFrame();
		if (m_Countdown.Update(deltaTime) > 0)
		{
			CountdownFrame = frame;
			Expect(!m_Countdown.IsRunning() && m_Countdown.GetRemaining() == 0.0f && m_Countdown.GetProgress() == 1.0f, "a one-shot Timer stops when it elapses");
		}
		if (frame < c_TickFrames)
		{
			Ticks += m_Ticker.Update(deltaTime);
		}
		else if (frame == c_TickFrames)
		{
			m_Ticker.Stop();
			Expect(!m_Ticker.IsRunning() && m_Ticker.Update(deltaTime) == 0, "Timer::Stop");
		}
		// The runners hold Space for two frames (see InputFeatures): the press, and the repeat after the short delay.
		if (m_KeyRepeat.Update(deltaTime))
			KeyRepeats++;

		if (frame == c_CheckFrame)
		{
			// Game time: frames of 1/60 s, one of them at half speed (see TimeFeatures).
			Expect(CountdownFrame >= 29 && CountdownFrame <= 31, "a Timer of 0.5 s elapses after 30 frames");
			Expect(Ticks >= 9 && Ticks <= 10, "a repeating Timer of 0.1 s elapses about ten times a second");
			Expect(KeyRepeats == 2, "KeyRepeat fires on the press and after the delay");
			Completed = true;
		}
		else if (frame == QuitFrame)
		{
			const AssetHandle scene = Assets::Find("Scenes/Feature.stscene");
			Expect(Game::LoadScene(scene), "Game::LoadScene(handle)");
			Expect(Game::LoadScene("Scenes/Feature.stscene"), "Game::LoadScene(path)");
			Expect(Game::ReloadScene(), "Game::ReloadScene");
			Game::Quit(QuitCode);
			Journal(*this, "GameFeatures", "Quit");
		}
	}
private:
	void CheckRandom()
	{
		Random random(static_cast<uint64_t>(QuitCode));
		Random replay;
		replay.Seed(static_cast<uint64_t>(QuitCode));
		bool same = true;
		for (int index = 0; index < 16; index++)
			same &= random.NextUInt() == replay.NextUInt();
		Expect(same, "Random: a seed fixes the sequence");
		const int32_t die = random.Range(1, 6);
		const float unit = random.NextFloat();
		const float offset = random.Range(-1.0f, 1.0f);
		Expect(die >= 1 && die <= 6 && unit >= 0.0f && unit < 1.0f && offset >= -1.0f && offset < 1.0f, "Random ranges");
		Expect(random.Chance(1.0f) && !random.Chance(0.0f), "Random::Chance");
	}
private:
	Timer m_Countdown;
	Timer m_Ticker;
	KeyRepeat m_KeyRepeat { Key::Space, 0.01f, 1.0f };
};

ST_SCRIPT_CLASS(GameFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(QuitFrame);
	ST_SCRIPT_FIELD(QuitCode);
	ST_SCRIPT_FIELD(CountdownFrame);
	ST_SCRIPT_FIELD(Ticks);
	ST_SCRIPT_FIELD(KeyRepeats);
}
