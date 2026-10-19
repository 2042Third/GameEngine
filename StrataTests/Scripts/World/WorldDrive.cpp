// The drive of StrataRuntime.World (StrataTests/CMakeLists.txt): on "Driver", the streaming source of a generated world
// (StrataTests --strata-test-helper=write-world-game), it visits every Ground cell in turn. At each cell's center it waits
// for World::IsLoadedAround, lets physics build the cell's bodies, then casts a ray down onto the cell's floor and counts
// the bodies over the cell with Physics::OverlapBox: a cell whose floor no ray hits, or with fewer bodies than it holds,
// is one "without collision". After the last cell it logs "World drive: <cells> cells visited, <n> without collision" and
// ends the game (exit code 0 when every cell had collision, 1 otherwise).

#include "StrataScript/StrataScript.h"

#include <cstdint>
#include <vector>

using namespace Strata;

class WorldDrive : public Script
{
public:
	// The world's Ground grid, set by the helper that writes the game.
	int32_t GridX = 6;
	int32_t GridZ = 6;
	float CellSize = 32.0f;
	glm::vec3 Origin = glm::vec3(0.0f); // Corner of cell (0, 0)
	int32_t BodiesPerCell = 0;          // Static bodies of a Ground cell, its floor included
	// Progress.
	int32_t Cell = 0;
	int32_t SettleFrames = 0;
	int32_t WithoutCollision = 0;
	bool Reported = false;

	void OnUpdate(float) override
	{
		if (Reported)
			return;
		const int32_t cells = GridX * GridZ;
		if (Cell >= cells)
		{
			Log::Info("World drive: ", cells, " cells visited, ", WithoutCollision, " without collision");
			Reported = true;
			Game::Quit(WithoutCollision == 0 ? 0 : 1);
			return;
		}
		const glm::vec3 center = Origin + glm::vec3((static_cast<float>(Cell % GridX) + 0.5f) * CellSize, 1.0f, (static_cast<float>(Cell / GridX) + 0.5f) * CellSize);
		if (GetEntity().GetTransform().GetWorldPosition() != center)
		{
			GetEntity().GetTransform().SetWorldPosition(center);
			SettleFrames = 0;
			return;
		}
		if (!World::IsLoadedAround(center, 1.0f))
			return;
		// The cell's bodies are built in the physics update after its entities appear.
		if (SettleFrames++ < 2)
			return;
		const std::optional<RaycastHit> floor = Physics::Raycast(center + glm::vec3(0.0f, 20.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 40.0f);
		// Just inside the cell: its boxes start next to its edges, and the neighbors' a little further out.
		const float inset = 0.04f;
		const std::vector<Entity> bodies = Physics::OverlapBox(center, glm::vec3(CellSize * 0.5f - inset, 4.0f, CellSize * 0.5f - inset));
		if (!floor || static_cast<int32_t>(bodies.size()) < BodiesPerCell)
		{
			Log::Warn("World drive: cell ", Cell, " has ", bodies.size(), " of ", BodiesPerCell, " bodies", floor ? "" : " and no floor under its center");
			WithoutCollision++;
		}
		Cell++;
	}
};

ST_SCRIPT_CLASS(WorldDrive)
{
	ST_SCRIPT_FIELD(GridX);
	ST_SCRIPT_FIELD(GridZ);
	ST_SCRIPT_FIELD(CellSize);
	ST_SCRIPT_FIELD(Origin);
	ST_SCRIPT_FIELD(BodiesPerCell);
	ST_SCRIPT_FIELD(Cell);
	ST_SCRIPT_FIELD(SettleFrames);
	ST_SCRIPT_FIELD(WithoutCollision);
	ST_SCRIPT_FIELD(Reported);
}
