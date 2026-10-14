#include "stpch.h"
#include "Strata/World/WorldGrid.h"

#include "Strata/Core/Assert.h"
#include "Strata/World/WorldAsset.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Strata
{

	namespace
	{

		constexpr int64_t c_CoordinateOffset = int64_t(1) << 27;
		constexpr uint64_t c_CoordinateMask = (uint64_t(1) << 28) - 1;

		int32_t ToCellCoordinate(double offset, double cellSize)
		{
			const double cell = std::floor(offset / cellSize);
			// Positions far outside any world (and NaN) clamp to the outermost cells a key can name.
			if (!(cell >= -static_cast<double>(WorldCellKey::c_MaxCoordinate)))
				return -WorldCellKey::c_MaxCoordinate;
			if (cell > static_cast<double>(WorldCellKey::c_MaxCoordinate))
				return WorldCellKey::c_MaxCoordinate;
			return static_cast<int32_t>(cell);
		}

	}

	uint64_t WorldCellKey::Pack() const
	{
		ST_CORE_ASSERT(std::abs(X) <= c_MaxCoordinate && std::abs(Z) <= c_MaxCoordinate, "Cell ({}, {}) is outside what a key can pack", X, Z);
		const uint64_t x = static_cast<uint64_t>(static_cast<int64_t>(X) + c_CoordinateOffset) & c_CoordinateMask;
		const uint64_t z = static_cast<uint64_t>(static_cast<int64_t>(Z) + c_CoordinateOffset) & c_CoordinateMask;
		return (static_cast<uint64_t>(Layer) << 56) | (x << 28) | z;
	}

	WorldCellKey WorldCellKey::Unpack(uint64_t packed)
	{
		WorldCellKey key;
		key.Layer = static_cast<uint8_t>(packed >> 56);
		key.X = static_cast<int32_t>(static_cast<int64_t>((packed >> 28) & c_CoordinateMask) - c_CoordinateOffset);
		key.Z = static_cast<int32_t>(static_cast<int64_t>(packed & c_CoordinateMask) - c_CoordinateOffset);
		return key;
	}

	namespace WorldGrid
	{

		WorldCellKey CellAt(const WorldLayer& layer, const glm::dvec2& gridOrigin, const glm::dvec3& position)
		{
			const double cellSize = static_cast<double>(layer.CellSize);
			return WorldCellKey { layer.ID, ToCellCoordinate(position.x - gridOrigin.x, cellSize), ToCellCoordinate(position.z - gridOrigin.y, cellSize) };
		}

		WorldCellSquare CellBounds(const WorldLayer& layer, const glm::dvec2& gridOrigin, int32_t x, int32_t z)
		{
			const double cellSize = static_cast<double>(layer.CellSize);
			WorldCellSquare square;
			square.Min = gridOrigin + glm::dvec2(static_cast<double>(x), static_cast<double>(z)) * cellSize;
			square.Max = square.Min + glm::dvec2(cellSize);
			return square;
		}

		double Distance(const WorldCellSquare& square, const glm::dvec3& point)
		{
			const double dx = std::max({ square.Min.x - point.x, 0.0, point.x - square.Max.x });
			const double dz = std::max({ square.Min.y - point.z, 0.0, point.z - square.Max.y });
			return std::sqrt(dx * dx + dz * dz);
		}

	}

}
