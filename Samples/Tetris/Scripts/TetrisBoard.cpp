// Plays Tetris on the entity it is attached to. The well's bottom-left cell sits at the entity's origin, one world unit per
// cell; the script creates the cube entities it draws with (under a child named "Cells") and shows score, level and lines
// in the Text entities named by its fields.
//
// Controls: Left/Right or A/D move (held keys repeat), Down or S soft drop, Space hard drop, Up or X rotate clockwise,
// Z rotate counterclockwise, P pause, R or Enter restart, Escape quit.

#include "StrataScript/StrataScript.h"

#include "TetrisGame.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace Strata;

class TetrisBoard : public Script
{
public:
	// Rules
	int32_t Seed = 60;              // Piece sequence of the first game; each restart uses the next seed
	int32_t StartLevel = 1;
	int32_t LinesPerLevel = 10;     // Cleared lines per level
	float StartInterval = 0.8f;     // Seconds per row of gravity at the start level
	float IntervalFactor = 0.85f;   // Gravity interval multiplier per level above the start level
	float MinInterval = 0.05f;      // Fastest gravity
	float SoftDropInterval = 0.04f; // Seconds per row while soft dropping
	float RepeatDelay = 0.17f;      // Held left/right: first repeat after this many seconds...
	float RepeatInterval = 0.05f;   // ...then one move per interval

	// Looks
	AssetHandle CellMesh; // Builtin/Cube when unset
	AssetHandle MaterialI;
	AssetHandle MaterialO;
	AssetHandle MaterialT;
	AssetHandle MaterialS;
	AssetHandle MaterialZ;
	AssetHandle MaterialJ;
	AssetHandle MaterialL;
	AssetHandle GhostMaterial; // Marks where the piece would land; no ghost when unset
	float CellScale = 0.92f;   // Gaps between the cubes
	glm::vec3 PreviewOffset = { 12.0f, 15.0f, 0.0f }; // Where the next piece is shown, relative to the board

	// HUD: entities with a Text component
	Entity ScoreText;
	Entity LevelText;
	Entity LinesText;
	Entity MessageText;

	// Sounds (optional)
	AssetHandle MoveSound;
	AssetHandle LockSound;
	AssetHandle ClearSound;
	AssetHandle GameOverSound;

	void OnCreate() override
	{
		for (AssetHandle asset : { MaterialI, MaterialO, MaterialT, MaterialS, MaterialZ, MaterialJ, MaterialL, GhostMaterial, MoveSound,
			LockSound, ClearSound, GameOverSound })
		{
			if (asset)
				Assets::RequestLoad(asset);
		}
		if (!CellMesh)
			CellMesh = Assets::Find("Builtin/Cube");

		CreateCells();
		SetupInput();
		NewGame();
	}

	void OnReload() override
	{
		// The game state lives in this instance, which a hot reload replaces: find the cells again and start over.
		FindCells();
		SetupInput();
		NewGame();
	}

	void OnUpdate(float deltaTime) override
	{
		if (Input::IsKeyPressed(Key::Escape))
		{
			Game::Quit(0);
			return;
		}
		if (Input::IsKeyPressed(Key::R) || (m_Game.IsGameOver() && (Input::IsKeyPressed(Key::Enter) || Input::IsKeyPressed(Key::KPEnter))))
		{
			m_GamesPlayed++;
			NewGame();
		}
		else if (!m_Game.IsGameOver())
		{
			if (Input::IsKeyPressed(Key::P))
				m_Paused = !m_Paused;
			if (!m_Paused)
				Play(deltaTime);
		}
		Refresh();
	}
private:
	static constexpr int32_t c_Ghost = Tetris::c_PieceTypeCount; // Display type of ghost cells
	static constexpr int32_t c_Unknown = -2;                      // Display type before the first refresh

	void Play(float deltaTime)
	{
		const bool left = UpdatePair(m_Left, m_LeftAlt, deltaTime);
		const bool right = UpdatePair(m_Right, m_RightAlt, deltaTime);
		const bool softDrop = UpdatePair(m_Down, m_DownAlt, deltaTime);
		if (left != right && m_Game.Shift(left ? -1 : 1))
			PlaySound(MoveSound);
		if (Input::IsKeyPressed(Key::Up) || Input::IsKeyPressed(Key::X))
		{
			if (m_Game.Rotate(1))
				PlaySound(MoveSound);
		}
		if (Input::IsKeyPressed(Key::Z))
		{
			if (m_Game.Rotate(-1))
				PlaySound(MoveSound);
		}

		if (Input::IsKeyPressed(Key::Space))
		{
			HandleStep(m_Game.HardDrop());
			return;
		}
		if (softDrop)
		{
			if (m_Game.SoftDrop())
				m_Gravity = 0.0f;
			else
				HandleStep(m_Game.Step()); // Resting on something: lock now
			if (m_Game.IsGameOver())
				return;
		}

		m_Gravity += deltaTime;
		const float interval = GetGravityInterval();
		while (m_Gravity >= interval && !m_Game.IsGameOver())
		{
			m_Gravity -= interval;
			if (HandleStep(m_Game.Step()))
				break;
		}
	}

	// Two keys for one action (an arrow key and a letter): both are updated every frame, so their repeat state stays current.
	static bool UpdatePair(KeyRepeat& primary, KeyRepeat& secondary, float deltaTime)
	{
		const bool primaryFired = primary.Update(deltaTime);
		const bool secondaryFired = secondary.Update(deltaTime);
		return primaryFired || secondaryFired;
	}

	// Returns true when the piece locked.
	bool HandleStep(const Tetris::StepResult& result)
	{
		if (!result.Locked)
			return false;
		m_Gravity = 0.0f;
		if (result.LinesCleared > 0)
		{
			PlaySound(ClearSound);
			Log::Info("Tetris: cleared ", result.LinesCleared, result.LinesCleared == 1 ? " line" : " lines", "; score ", m_Game.GetScore(),
				", lines ", m_Game.GetLines(), ", level ", m_Game.GetLevel());
		}
		else
		{
			PlaySound(LockSound);
		}
		if (result.GameOver)
		{
			PlaySound(GameOverSound);
			Log::Info("Tetris: game over after ", m_Game.GetPiecesDealt(), " pieces; score ", m_Game.GetScore(), ", lines ", m_Game.GetLines(),
				", level ", m_Game.GetLevel());
		}
		else
		{
			LogPiece();
		}
		return true;
	}

	float GetGravityInterval() const
	{
		const int32_t levelsAbove = std::max(m_Game.GetLevel() - std::max(StartLevel, 1), 0);
		const float interval = StartInterval * std::pow(IntervalFactor, static_cast<float>(levelsAbove));
		return std::max(interval, std::max(MinInterval, 0.001f));
	}

	void NewGame()
	{
		const uint64_t seed = static_cast<uint64_t>(static_cast<int64_t>(Seed) + m_GamesPlayed);
		m_Game.Reset(seed, StartLevel, LinesPerLevel);
		m_Gravity = 0.0f;
		m_Paused = false;
		Log::Info("Tetris: new game with seed ", seed);
		LogPiece();
	}

	void LogPiece()
	{
		Log::Info("Tetris: piece ", m_Game.GetPiecesDealt(), " is ", Tetris::GetPieceName(m_Game.GetPiece().Type), ", next ",
			Tetris::GetPieceName(m_Game.GetNextType()));
	}

	void SetupInput()
	{
		m_Left = KeyRepeat(Key::Left, RepeatDelay, RepeatInterval);
		m_LeftAlt = KeyRepeat(Key::A, RepeatDelay, RepeatInterval);
		m_Right = KeyRepeat(Key::Right, RepeatDelay, RepeatInterval);
		m_RightAlt = KeyRepeat(Key::D, RepeatDelay, RepeatInterval);
		m_Down = KeyRepeat(Key::Down, SoftDropInterval, SoftDropInterval);
		m_DownAlt = KeyRepeat(Key::S, SoftDropInterval, SoftDropInterval);
	}

	void CreateCells()
	{
		Entity container = Scene::CreateEntity("Cells", GetEntity());
		m_Cells.clear();
		for (int32_t y = 0; y < Tetris::c_Height; y++)
		{
			for (int32_t x = 0; x < Tetris::c_Width; x++)
				m_Cells.push_back(CreateCell(container, glm::vec3(static_cast<float>(x), static_cast<float>(y), 0.0f)));
		}
		m_Preview.clear();
		for (int32_t index = 0; index < 4; index++)
			m_Preview.push_back(CreateCell(container, PreviewOffset));
		ResetShownTypes();
	}

	Entity CreateCell(Entity parent, const glm::vec3& position)
	{
		Entity cell = Scene::CreateEntity("Cell", parent);
		cell.AddComponent("MeshRenderer");
		cell.SetProperty("MeshRenderer", "Mesh", CellMesh);
		TransformComponent transform = cell.GetTransform();
		transform.SetTranslation(position);
		transform.SetScale(glm::vec3(CellScale));
		cell.SetActive(false);
		return cell;
	}

	void FindCells()
	{
		m_Cells.clear();
		m_Preview.clear();
		for (Entity child : GetEntity().GetChildren())
		{
			if (child.GetName() != "Cells")
				continue;
			std::vector<Entity> cells = child.GetChildren();
			const size_t boardCount = static_cast<size_t>(Tetris::c_Width * Tetris::c_Height);
			if (cells.size() == boardCount + 4)
			{
				m_Cells.assign(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(boardCount));
				m_Preview.assign(cells.begin() + static_cast<std::ptrdiff_t>(boardCount), cells.end());
			}
			break;
		}
		if (m_Cells.empty())
			CreateCells();
		ResetShownTypes();
	}

	void ResetShownTypes()
	{
		m_ShownCells.assign(m_Cells.size(), c_Unknown);
		m_ShownPreview.assign(m_Preview.size(), c_Unknown);
		m_ShownPreviewType = c_Unknown;
		m_ShownScore = -1;
		m_ShownLevel = -1;
		m_ShownLines = -1;
		m_ShownMessage = "\n"; // Never a real message: the first refresh writes it
	}

	// Brings the cell entities and the HUD up to date, touching only what changed.
	void Refresh()
	{
		std::vector<int32_t> types(m_Cells.size(), Tetris::c_Empty);
		for (int32_t y = 0; y < Tetris::c_Height; y++)
		{
			for (int32_t x = 0; x < Tetris::c_Width; x++)
				types[static_cast<size_t>(y * Tetris::c_Width + x)] = m_Game.GetCell(x, y);
		}
		auto place = [&](const Tetris::PieceCells& cells, int32_t type)
		{
			for (const Tetris::Cell& cell : cells)
			{
				if (cell.Y < Tetris::c_Height && types[static_cast<size_t>(cell.Y * Tetris::c_Width + cell.X)] == Tetris::c_Empty)
					types[static_cast<size_t>(cell.Y * Tetris::c_Width + cell.X)] = type;
			}
		};
		if (!m_Game.IsGameOver())
		{
			if (GhostMaterial)
				place(m_Game.GetGhostCells(), c_Ghost);
			// The piece covers its own ghost: overwrite instead of filling empty cells only.
			for (const Tetris::Cell& cell : m_Game.GetPieceCells())
			{
				if (cell.Y < Tetris::c_Height)
					types[static_cast<size_t>(cell.Y * Tetris::c_Width + cell.X)] = static_cast<int32_t>(m_Game.GetPiece().Type);
			}
		}
		for (size_t index = 0; index < m_Cells.size() && index < types.size(); index++)
			ShowCell(m_Cells[index], m_ShownCells[index], types[index]);

		const int32_t nextType = static_cast<int32_t>(m_Game.GetNextType());
		if (nextType != m_ShownPreviewType && m_Preview.size() == 4)
		{
			const Tetris::PieceCells shape = Tetris::GetShape(m_Game.GetNextType(), 0);
			for (size_t index = 0; index < 4; index++)
			{
				const glm::vec3 offset(static_cast<float>(shape[index].X), static_cast<float>(shape[index].Y), 0.0f);
				m_Preview[index].GetTransform().SetTranslation(PreviewOffset + offset);
				ShowCell(m_Preview[index], m_ShownPreview[index], nextType);
			}
			m_ShownPreviewType = nextType;
		}

		if (m_Game.GetScore() != m_ShownScore)
		{
			m_ShownScore = m_Game.GetScore();
			SetText(ScoreText, "Score: " + std::to_string(m_ShownScore));
		}
		if (m_Game.GetLevel() != m_ShownLevel)
		{
			m_ShownLevel = m_Game.GetLevel();
			SetText(LevelText, "Level: " + std::to_string(m_ShownLevel));
		}
		if (m_Game.GetLines() != m_ShownLines)
		{
			m_ShownLines = m_Game.GetLines();
			SetText(LinesText, "Lines: " + std::to_string(m_ShownLines));
		}
		std::string message;
		if (m_Game.IsGameOver())
			message = "GAME OVER\nPress R or Enter to play again";
		else if (m_Paused)
			message = "PAUSED\nPress P to continue";
		if (message != m_ShownMessage)
		{
			m_ShownMessage = message;
			SetText(MessageText, message);
		}
	}

	void ShowCell(Entity cell, int32_t& shown, int32_t type)
	{
		if (shown == type)
			return;
		if (type == Tetris::c_Empty)
		{
			cell.SetActive(false);
		}
		else
		{
			cell.SetProperty("MeshRenderer", "Material", GetMaterial(type));
			cell.SetActive(true);
		}
		shown = type;
	}

	AssetHandle GetMaterial(int32_t type) const
	{
		switch (type)
		{
			case 0: return MaterialI;
			case 1: return MaterialO;
			case 2: return MaterialT;
			case 3: return MaterialS;
			case 4: return MaterialZ;
			case 5: return MaterialJ;
			case 6: return MaterialL;
			default: return GhostMaterial;
		}
	}

	static void SetText(Entity entity, const std::string& text)
	{
		if (entity)
			entity.SetProperty("Text", "Text", text);
	}

	static void PlaySound(AssetHandle clip)
	{
		if (clip)
			Audio::PlayOneShot(clip);
	}
private:
	Tetris::Game m_Game;
	int32_t m_GamesPlayed = 0;
	float m_Gravity = 0.0f;
	bool m_Paused = false;

	KeyRepeat m_Left { Key::Left };
	KeyRepeat m_LeftAlt { Key::A };
	KeyRepeat m_Right { Key::Right };
	KeyRepeat m_RightAlt { Key::D };
	KeyRepeat m_Down { Key::Down };
	KeyRepeat m_DownAlt { Key::S };

	std::vector<Entity> m_Cells;   // Row by row from the bottom
	std::vector<Entity> m_Preview; // The next piece's four cells
	std::vector<int32_t> m_ShownCells;
	std::vector<int32_t> m_ShownPreview;
	int32_t m_ShownPreviewType = c_Unknown;
	int32_t m_ShownScore = -1;
	int32_t m_ShownLevel = -1;
	int32_t m_ShownLines = -1;
	std::string m_ShownMessage;
};

ST_SCRIPT_CLASS(TetrisBoard)
{
	ST_SCRIPT_FIELD(Seed);
	ST_SCRIPT_FIELD(StartLevel);
	ST_SCRIPT_FIELD(LinesPerLevel);
	ST_SCRIPT_FIELD(StartInterval);
	ST_SCRIPT_FIELD(IntervalFactor);
	ST_SCRIPT_FIELD(MinInterval);
	ST_SCRIPT_FIELD(SoftDropInterval);
	ST_SCRIPT_FIELD(RepeatDelay);
	ST_SCRIPT_FIELD(RepeatInterval);
	ST_SCRIPT_FIELD(CellMesh);
	ST_SCRIPT_FIELD(MaterialI);
	ST_SCRIPT_FIELD(MaterialO);
	ST_SCRIPT_FIELD(MaterialT);
	ST_SCRIPT_FIELD(MaterialS);
	ST_SCRIPT_FIELD(MaterialZ);
	ST_SCRIPT_FIELD(MaterialJ);
	ST_SCRIPT_FIELD(MaterialL);
	ST_SCRIPT_FIELD(GhostMaterial);
	ST_SCRIPT_FIELD(CellScale);
	ST_SCRIPT_FIELD(PreviewOffset);
	ST_SCRIPT_FIELD(ScoreText);
	ST_SCRIPT_FIELD(LevelText);
	ST_SCRIPT_FIELD(LinesText);
	ST_SCRIPT_FIELD(MessageText);
	ST_SCRIPT_FIELD(MoveSound);
	ST_SCRIPT_FIELD(LockSound);
	ST_SCRIPT_FIELD(ClearSound);
	ST_SCRIPT_FIELD(GameOverSound);
}
