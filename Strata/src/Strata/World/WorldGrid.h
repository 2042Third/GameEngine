#pragma once

#include <glm/glm.hpp>

#include <compare>
#include <cstdint>

namespace Strata
{

	struct WorldLayer;

	// A cell of a world: its layer's ID and its square's column (X) and row (Z) in that layer's grid.
	struct WorldCellKey
	{
		// Cell coordinates a key can pack: every cell of a valid world fits (WorldData::c_MaxExtent with the smallest cells).
		static constexpr int32_t c_MaxCoordinate = (1 << 27) - 1;

		uint8_t Layer = 0;
		int32_t X = 0;
		int32_t Z = 0;

		// One 64-bit number per key (the layer in the top byte, then X and Z offset to be unsigned): hash maps and the
		// streamer's total order of cells use it. Its order is the order of (Layer, X, Z).
		uint64_t Pack() const;
		static WorldCellKey Unpack(uint64_t packed);

		auto operator<=>(const WorldCellKey&) const = default;
	};

	// A cell's square on the ground plane (XZ, world units); cells reach from minus to plus infinity in Y.
	struct WorldCellSquare
	{
		glm::dvec2 Min { 0.0 };
		glm::dvec2 Max { 0.0 };
	};

	// The grid math of a world's layers: square cells of the layer's CellSize, from the world's GridOrigin (X and Z).
	namespace WorldGrid
	{

		// The cell of `layer` that holds a position (cells include their lower edges).
		WorldCellKey CellAt(const WorldLayer& layer, const glm::dvec2& gridOrigin, const glm::dvec3& position);
		// The square of a cell of `layer`.
		WorldCellSquare CellBounds(const WorldLayer& layer, const glm::dvec2& gridOrigin, int32_t x, int32_t z);
		// The distance from a point to a cell's square on the ground plane (its Y is ignored): 0 inside.
		double Distance(const WorldCellSquare& square, const glm::dvec3& point);

	}

}
