#pragma once

#include "StrataScript/Gameplay.h"

#include <array>
#include <cstdint>

// The rules of the game, without any engine calls: a 10x20 well, the seven tetrominoes from a seeded 7-bag, rotation with
// simple wall kicks, gravity steps, locking, line clears, scoring and levels. TetrisBoard (the script) feeds it input and
// time and draws its state.
namespace Tetris
{

	constexpr int32_t c_Width = 10;
	constexpr int32_t c_Height = 20;                // Visible rows
	constexpr int32_t c_TotalHeight = c_Height + 4; // Hidden rows above the well, where pieces spawn and rotate
	constexpr int32_t c_PieceTypeCount = 7;
	constexpr int32_t c_Empty = -1;

	// Values index the piece colours (TetrisBoard's material fields) and name the pieces in logs.
	enum class PieceType : int32_t
	{
		I = 0, O, T, S, Z, J, L
	};

	const char* GetPieceName(PieceType type);

	struct Cell
	{
		int32_t X = 0;
		int32_t Y = 0; // 0 is the bottom row
	};

	using PieceCells = std::array<Cell, 4>;

	// The falling piece: the bottom-left corner of its rotation box in the well, and its rotation (0 = spawn, 1 = turned
	// clockwise once, ...).
	struct Piece
	{
		PieceType Type = PieceType::I;
		int32_t X = 0;
		int32_t Y = 0;
		int32_t Rotation = 0;
	};

	// The cells of a piece type in its rotation box (for the preview), relative to the box's bottom-left corner.
	PieceCells GetShape(PieceType type, int32_t rotation);

	// What a gravity step or a hard drop did.
	struct StepResult
	{
		bool Locked = false;      // The piece came to rest and became part of the well
		int32_t LinesCleared = 0; // Full rows removed by that lock
		bool GameOver = false;    // The next piece had no room
	};

	class Game
	{
	public:
		// Starts a new game: empty well, score 0, the piece sequence of `seed`.
		void Reset(uint64_t seed, int32_t startLevel, int32_t linesPerLevel);

		// Player moves; each returns false (and changes nothing) when the piece cannot go there.
		bool Shift(int32_t dx);
		bool Rotate(int32_t direction); // +1 clockwise, -1 counterclockwise; tries a few sideways and upward kicks
		// Moves the piece down one row without locking it (soft drop: one point per row).
		bool SoftDrop();
		// Drops the piece to the bottom and locks it (two points per row).
		StepResult HardDrop();
		// Gravity: moves the piece down one row, or locks it when it rests on something.
		StepResult Step();

		// The piece type in a cell of the well, or c_Empty.
		int32_t GetCell(int32_t x, int32_t y) const;
		const Piece& GetPiece() const { return m_Piece; }
		PieceCells GetPieceCells() const { return GetCells(m_Piece); }
		// The falling piece where a hard drop would put it.
		PieceCells GetGhostCells() const;
		PieceType GetNextType() const { return m_Next; }

		int32_t GetScore() const { return m_Score; }
		int32_t GetLines() const { return m_Lines; }
		int32_t GetLevel() const { return m_StartLevel + m_Lines / m_LinesPerLevel; }
		int32_t GetPiecesDealt() const { return m_PiecesDealt; }
		bool IsGameOver() const { return m_GameOver; }
	private:
		PieceCells GetCells(const Piece& piece) const;
		bool Fits(const Piece& piece) const;
		StepResult Lock();
		int32_t ClearFullRows();
		bool SpawnNext();
		PieceType DrawFromBag();
	private:
		std::array<std::array<int32_t, c_Width>, c_TotalHeight> m_Cells = {};
		Piece m_Piece;
		PieceType m_Next = PieceType::I;
		Strata::Random m_Random;
		std::array<PieceType, c_PieceTypeCount> m_Bag = {};
		int32_t m_BagIndex = c_PieceTypeCount;
		int32_t m_Score = 0;
		int32_t m_Lines = 0;
		int32_t m_StartLevel = 1;
		int32_t m_LinesPerLevel = 10;
		int32_t m_PiecesDealt = 0;
		bool m_GameOver = false;
	};

}
