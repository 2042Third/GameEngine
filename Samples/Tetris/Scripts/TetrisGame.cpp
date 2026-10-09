#include "TetrisGame.h"

#include <algorithm>

namespace Tetris
{

	namespace
	{

		struct ShapeInfo
		{
			int32_t BoxSize;  // Side of the rotation box
			PieceCells Cells; // Spawn orientation, y up
		};

		// Spawn orientations as in most modern versions: flat side down, I and O centered, the rest in the left three columns
		// of their box.
		constexpr std::array<ShapeInfo, c_PieceTypeCount> c_Shapes = { {
			{ 4, { { { 0, 2 }, { 1, 2 }, { 2, 2 }, { 3, 2 } } } }, // I
			{ 2, { { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } } } }, // O
			{ 3, { { { 1, 2 }, { 0, 1 }, { 1, 1 }, { 2, 1 } } } }, // T
			{ 3, { { { 1, 2 }, { 2, 2 }, { 0, 1 }, { 1, 1 } } } }, // S
			{ 3, { { { 0, 2 }, { 1, 2 }, { 1, 1 }, { 2, 1 } } } }, // Z
			{ 3, { { { 0, 2 }, { 0, 1 }, { 1, 1 }, { 2, 1 } } } }, // J
			{ 3, { { { 2, 2 }, { 0, 1 }, { 1, 1 }, { 2, 1 } } } }, // L
		} };

		// Offsets tried in order when a rotation does not fit where it is.
		constexpr std::array<Cell, 6> c_Kicks = { { { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, 1 }, { -2, 0 }, { 2, 0 } } };

		// Points for 1 to 4 rows cleared at once, multiplied by the level.
		constexpr std::array<int32_t, 5> c_LineScores = { 0, 100, 300, 500, 800 };

		const ShapeInfo& GetShapeInfo(PieceType type)
		{
			return c_Shapes[static_cast<size_t>(type)];
		}

	}

	const char* GetPieceName(PieceType type)
	{
		constexpr std::array<const char*, c_PieceTypeCount> c_Names = { "I", "O", "T", "S", "Z", "J", "L" };
		return c_Names[static_cast<size_t>(type)];
	}

	PieceCells GetShape(PieceType type, int32_t rotation)
	{
		const ShapeInfo& shape = GetShapeInfo(type);
		PieceCells cells = shape.Cells;
		const int32_t turns = ((rotation % 4) + 4) % 4;
		for (int32_t turn = 0; turn < turns; turn++)
		{
			// A clockwise quarter turn inside the box (y up): (x, y) -> (y, size - 1 - x).
			for (Cell& cell : cells)
				cell = Cell { cell.Y, shape.BoxSize - 1 - cell.X };
		}
		return cells;
	}

	void Game::Reset(uint64_t seed, int32_t startLevel, int32_t linesPerLevel)
	{
		for (std::array<int32_t, c_Width>& row : m_Cells)
			row.fill(c_Empty);
		m_Random.Seed(seed);
		m_BagIndex = c_PieceTypeCount;
		m_Score = 0;
		m_Lines = 0;
		m_StartLevel = std::max(startLevel, 1);
		m_LinesPerLevel = std::max(linesPerLevel, 1);
		m_PiecesDealt = 0;
		m_GameOver = false;
		m_Next = DrawFromBag();
		SpawnNext();
	}

	bool Game::Shift(int32_t dx)
	{
		if (m_GameOver)
			return false;
		Piece moved = m_Piece;
		moved.X += dx;
		if (!Fits(moved))
			return false;
		m_Piece = moved;
		return true;
	}

	bool Game::Rotate(int32_t direction)
	{
		if (m_GameOver || m_Piece.Type == PieceType::O)
			return false;
		Piece rotated = m_Piece;
		rotated.Rotation = (m_Piece.Rotation + (direction >= 0 ? 1 : 3)) % 4;
		for (const Cell& kick : c_Kicks)
		{
			Piece kicked = rotated;
			kicked.X += kick.X;
			kicked.Y += kick.Y;
			if (Fits(kicked))
			{
				m_Piece = kicked;
				return true;
			}
		}
		return false;
	}

	bool Game::SoftDrop()
	{
		if (m_GameOver)
			return false;
		Piece moved = m_Piece;
		moved.Y--;
		if (!Fits(moved))
			return false;
		m_Piece = moved;
		m_Score++;
		return true;
	}

	StepResult Game::HardDrop()
	{
		if (m_GameOver)
			return {};
		Piece moved = m_Piece;
		moved.Y--;
		while (Fits(moved))
		{
			m_Piece = moved;
			m_Score += 2;
			moved.Y--;
		}
		return Lock();
	}

	StepResult Game::Step()
	{
		if (m_GameOver)
			return {};
		Piece moved = m_Piece;
		moved.Y--;
		if (Fits(moved))
		{
			m_Piece = moved;
			return {};
		}
		return Lock();
	}

	int32_t Game::GetCell(int32_t x, int32_t y) const
	{
		if (x < 0 || x >= c_Width || y < 0 || y >= c_TotalHeight)
			return c_Empty;
		return m_Cells[static_cast<size_t>(y)][static_cast<size_t>(x)];
	}

	PieceCells Game::GetGhostCells() const
	{
		Piece ghost = m_Piece;
		Piece below = ghost;
		below.Y--;
		while (Fits(below))
		{
			ghost = below;
			below.Y--;
		}
		return GetCells(ghost);
	}

	PieceCells Game::GetCells(const Piece& piece) const
	{
		PieceCells cells = GetShape(piece.Type, piece.Rotation);
		for (Cell& cell : cells)
		{
			cell.X += piece.X;
			cell.Y += piece.Y;
		}
		return cells;
	}

	bool Game::Fits(const Piece& piece) const
	{
		for (const Cell& cell : GetCells(piece))
		{
			if (cell.X < 0 || cell.X >= c_Width || cell.Y < 0 || cell.Y >= c_TotalHeight)
				return false;
			if (GetCell(cell.X, cell.Y) != c_Empty)
				return false;
		}
		return true;
	}

	StepResult Game::Lock()
	{
		StepResult result;
		result.Locked = true;
		bool aboveWell = true;
		for (const Cell& cell : GetPieceCells())
		{
			m_Cells[static_cast<size_t>(cell.Y)][static_cast<size_t>(cell.X)] = static_cast<int32_t>(m_Piece.Type);
			aboveWell = aboveWell && cell.Y >= c_Height;
		}

		result.LinesCleared = ClearFullRows();
		const int32_t level = GetLevel();
		m_Score += c_LineScores[static_cast<size_t>(result.LinesCleared)] * level;
		m_Lines += result.LinesCleared;

		// A piece that locked entirely above the well, or a next piece without room, ends the game.
		m_GameOver = aboveWell || !SpawnNext();
		result.GameOver = m_GameOver;
		return result;
	}

	int32_t Game::ClearFullRows()
	{
		int32_t cleared = 0;
		int32_t target = 0;
		for (int32_t y = 0; y < c_TotalHeight; y++)
		{
			const std::array<int32_t, c_Width>& row = m_Cells[static_cast<size_t>(y)];
			const bool full = std::none_of(row.begin(), row.end(), [](int32_t cell) { return cell == c_Empty; });
			if (full)
			{
				cleared++;
				continue;
			}
			if (target != y)
				m_Cells[static_cast<size_t>(target)] = row;
			target++;
		}
		for (int32_t y = target; y < c_TotalHeight; y++)
			m_Cells[static_cast<size_t>(y)].fill(c_Empty);
		return cleared;
	}

	bool Game::SpawnNext()
	{
		const ShapeInfo& shape = GetShapeInfo(m_Next);
		int32_t top = 0;
		for (const Cell& cell : shape.Cells)
			top = std::max(top, cell.Y);

		m_Piece.Type = m_Next;
		m_Piece.Rotation = 0;
		m_Piece.X = (c_Width - shape.BoxSize) / 2;
		m_Piece.Y = c_Height - 1 - top; // Its top row is the well's top row
		m_Next = DrawFromBag();
		m_PiecesDealt++;
		return Fits(m_Piece);
	}

	PieceType Game::DrawFromBag()
	{
		if (m_BagIndex >= c_PieceTypeCount)
		{
			// A new bag holds each piece once, in random order (Fisher-Yates shuffle).
			for (int32_t index = 0; index < c_PieceTypeCount; index++)
				m_Bag[static_cast<size_t>(index)] = static_cast<PieceType>(index);
			for (int32_t index = c_PieceTypeCount - 1; index > 0; index--)
				std::swap(m_Bag[static_cast<size_t>(index)], m_Bag[static_cast<size_t>(m_Random.Range(0, index))]);
			m_BagIndex = 0;
		}
		return m_Bag[static_cast<size_t>(m_BagIndex++)];
	}

}
