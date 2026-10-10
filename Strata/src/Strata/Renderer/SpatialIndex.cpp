#include "stpch.h"
#include "Strata/Renderer/SpatialIndex.h"

#include "Strata/Core/Assert.h"

#include <algorithm>
#include <cmath>

namespace Strata
{

	namespace
	{

		// Cell coordinates are packed into 21 bits per axis: about a million cells in each direction.
		constexpr int32_t c_MaxCoordinate = (1 << 20) - 1;
		constexpr int32_t c_MinCoordinate = -(1 << 20);

		int32_t FloorDivide(int32_t value, int32_t divisor)
		{
			const int32_t quotient = value / divisor;
			return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
		}

		void Unite(AABB& bounds, const AABB& other)
		{
			bounds.Min = glm::min(bounds.Min, other.Min);
			bounds.Max = glm::max(bounds.Max, other.Max);
		}

		bool ContainsBox(const AABB& outer, const AABB& inner)
		{
			return glm::all(glm::lessThanEqual(outer.Min, inner.Min)) && glm::all(glm::greaterThanEqual(outer.Max, inner.Max));
		}

		// Squared distance from a point to the nearest point of a box (0 inside it).
		float DistanceSquared(const AABB& bounds, const glm::vec3& point)
		{
			const glm::vec3 delta = glm::max(glm::max(bounds.Min - point, point - bounds.Max), glm::vec3(0.0f));
			return glm::dot(delta, delta);
		}

	}

	SpatialIndex::SpatialIndex(const SpatialIndexSpecification& specification)
		: m_Specification(specification)
	{
		ST_CORE_ASSERT(std::isfinite(specification.CellSize) && specification.CellSize > 0.0f, "SpatialIndex: the cell size must be positive");
		if (!(m_Specification.CellSize > 0.0f) || !std::isfinite(m_Specification.CellSize))
			m_Specification.CellSize = SpatialIndexSpecification().CellSize;
		if (!(m_Specification.MaxCellsPerObject >= 1.0f))
			m_Specification.MaxCellsPerObject = 1.0f;
		m_InverseCellSize = 1.0f / m_Specification.CellSize;
	}

	bool SpatialIndex::IntersectsPlanes(const AABB& bounds, std::span<const glm::vec4> planes)
	{
		if (!bounds.IsValid())
			return false;
		for (const glm::vec4& plane : planes)
		{
			// The box corner furthest along the plane normal (as Frustum::IsAABBVisible).
			const glm::vec3 positive(
				plane.x >= 0.0f ? bounds.Max.x : bounds.Min.x,
				plane.y >= 0.0f ? bounds.Max.y : bounds.Min.y,
				plane.z >= 0.0f ? bounds.Max.z : bounds.Min.z);
			if (glm::dot(glm::vec3(plane), positive) + plane.w < 0.0f)
				return false;
		}
		return true;
	}

	VolumeOverlap SpatialIndex::ClassifyPlanes(const AABB& bounds, std::span<const glm::vec4> planes)
	{
		if (!bounds.IsValid())
			return VolumeOverlap::Outside;
		// Inside when the corner nearest to each plane's back side is in front of it. A box inside the volume holds only
		// boxes that pass IntersectsPlanes: their furthest corners lie at least as far along every normal.
		bool inside = true;
		for (const glm::vec4& plane : planes)
		{
			const glm::vec3 normal(plane);
			const glm::vec3 positive(normal.x >= 0.0f ? bounds.Max.x : bounds.Min.x, normal.y >= 0.0f ? bounds.Max.y : bounds.Min.y,
				normal.z >= 0.0f ? bounds.Max.z : bounds.Min.z);
			if (glm::dot(normal, positive) + plane.w < 0.0f)
				return VolumeOverlap::Outside;
			const glm::vec3 negative(normal.x >= 0.0f ? bounds.Min.x : bounds.Max.x, normal.y >= 0.0f ? bounds.Min.y : bounds.Max.y,
				normal.z >= 0.0f ? bounds.Min.z : bounds.Max.z);
			if (glm::dot(normal, negative) + plane.w < 0.0f)
				inside = false;
		}
		return inside ? VolumeOverlap::Inside : VolumeOverlap::Intersecting;
	}

	bool SpatialIndex::IntersectsSphere(const AABB& bounds, const glm::vec3& center, float radius)
	{
		return bounds.IsValid() && DistanceSquared(bounds, center) <= radius * radius;
	}

	VolumeOverlap SpatialIndex::ClassifySphere(const AABB& bounds, const glm::vec3& center, float radius)
	{
		if (!IntersectsSphere(bounds, center, radius))
			return VolumeOverlap::Outside;
		// Inside when the furthest corner is within the radius.
		const glm::vec3 furthest = glm::max(glm::abs(bounds.Min - center), glm::abs(bounds.Max - center));
		return glm::dot(furthest, furthest) <= radius * radius ? VolumeOverlap::Inside : VolumeOverlap::Intersecting;
	}

	bool SpatialIndex::IsLarge(const AABB& bounds) const
	{
		const glm::vec3 size = bounds.Max - bounds.Min;
		const float limit = m_Specification.CellSize * m_Specification.MaxCellsPerObject;
		return size.x > limit || size.y > limit || size.z > limit;
	}

	glm::ivec3 SpatialIndex::GetCellCoordinates(const AABB& bounds) const
	{
		const glm::vec3 cell = glm::floor(bounds.GetCenter() * m_InverseCellSize);
		const glm::vec3 clamped = glm::clamp(cell, glm::vec3(static_cast<float>(c_MinCoordinate)), glm::vec3(static_cast<float>(c_MaxCoordinate)));
		return glm::ivec3(clamped);
	}

	uint64_t SpatialIndex::PackCoordinates(const glm::ivec3& coordinates)
	{
		constexpr uint64_t c_Mask = (1ull << 21) - 1;
		return (static_cast<uint64_t>(static_cast<uint32_t>(coordinates.x)) & c_Mask) | ((static_cast<uint64_t>(static_cast<uint32_t>(coordinates.y)) & c_Mask) << 21)
			| ((static_cast<uint64_t>(static_cast<uint32_t>(coordinates.z)) & c_Mask) << 42);
	}

	uint32_t SpatialIndex::AcquireCell(const glm::ivec3& coordinates)
	{
		const uint64_t key = PackCoordinates(coordinates);
		if (auto it = m_CellMap.find(key); it != m_CellMap.end())
			return it->second;

		uint32_t cellIndex = 0;
		if (!m_FreeCells.empty())
		{
			cellIndex = m_FreeCells.back();
			m_FreeCells.pop_back();
		}
		else
		{
			cellIndex = static_cast<uint32_t>(m_Cells.size());
			m_Cells.emplace_back();
		}
		Cell& cell = m_Cells[cellIndex];
		cell.Coordinates = coordinates;
		cell.Bounds = AABB();
		cell.Ids.clear();
		cell.BoundsStale = false;
		m_CellMap.emplace(key, cellIndex);

		const int32_t blockCells = static_cast<int32_t>(c_BlockCells);
		const glm::ivec3 blockCoordinates(FloorDivide(coordinates.x, blockCells), FloorDivide(coordinates.y, blockCells), FloorDivide(coordinates.z, blockCells));
		const uint64_t blockKey = PackCoordinates(blockCoordinates);
		uint32_t blockIndex = 0;
		if (auto it = m_BlockMap.find(blockKey); it != m_BlockMap.end())
		{
			blockIndex = it->second;
		}
		else
		{
			if (!m_FreeBlocks.empty())
			{
				blockIndex = m_FreeBlocks.back();
				m_FreeBlocks.pop_back();
			}
			else
			{
				blockIndex = static_cast<uint32_t>(m_Blocks.size());
				m_Blocks.emplace_back();
				m_ActiveBlockSlots.push_back(c_None);
			}
			Block& block = m_Blocks[blockIndex];
			block.Coordinates = blockCoordinates;
			block.Bounds = AABB();
			block.Cells.clear();
			block.BoundsStale = false;
			m_BlockMap.emplace(blockKey, blockIndex);
			m_ActiveBlockSlots[blockIndex] = static_cast<uint32_t>(m_ActiveBlocks.size());
			m_ActiveBlocks.push_back(blockIndex);
		}
		Block& block = m_Blocks[blockIndex];
		cell.Block = blockIndex;
		cell.PositionInBlock = static_cast<uint32_t>(block.Cells.size());
		block.Cells.push_back(cellIndex);
		return cellIndex;
	}

	void SpatialIndex::ReleaseCell(uint32_t cellIndex)
	{
		Cell& cell = m_Cells[cellIndex];
		ST_CORE_ASSERT(cell.Ids.empty(), "SpatialIndex: released a cell that holds objects");
		Block& block = m_Blocks[cell.Block];
		const uint32_t moved = block.Cells.back();
		block.Cells[cell.PositionInBlock] = moved;
		m_Cells[moved].PositionInBlock = cell.PositionInBlock;
		block.Cells.pop_back();

		if (block.Cells.empty())
		{
			const uint32_t blockIndex = cell.Block;
			const uint32_t slot = m_ActiveBlockSlots[blockIndex];
			const uint32_t movedBlock = m_ActiveBlocks.back();
			m_ActiveBlocks[slot] = movedBlock;
			m_ActiveBlockSlots[movedBlock] = slot;
			m_ActiveBlocks.pop_back();
			m_ActiveBlockSlots[blockIndex] = c_None;
			m_BlockMap.erase(PackCoordinates(block.Coordinates));
			block.BoundsStale = false;
			m_FreeBlocks.push_back(blockIndex);
		}
		else if (!block.BoundsStale)
		{
			block.BoundsStale = true;
			m_StaleBlocks.push_back(cell.Block);
		}

		m_CellMap.erase(PackCoordinates(cell.Coordinates));
		cell.Block = c_None;
		cell.BoundsStale = false;
		m_FreeCells.push_back(cellIndex);
	}

	void SpatialIndex::GrowCell(uint32_t cellIndex, const AABB& bounds)
	{
		Cell& cell = m_Cells[cellIndex];
		if (cell.Bounds.IsValid())
			Unite(cell.Bounds, bounds);
		else
			cell.Bounds = bounds;
		Block& block = m_Blocks[cell.Block];
		if (block.Bounds.IsValid())
			Unite(block.Bounds, bounds);
		else
			block.Bounds = bounds;
	}

	void SpatialIndex::AddToCell(uint32_t id, uint32_t cellIndex)
	{
		Object& object = m_Objects[id];
		Cell& cell = m_Cells[cellIndex];
		object.Cell = cellIndex;
		object.Position = static_cast<uint32_t>(cell.Ids.size());
		cell.Ids.push_back(id);
		GrowCell(cellIndex, object.Bounds);
	}

	void SpatialIndex::RemoveFromCell(uint32_t id)
	{
		Object& object = m_Objects[id];
		if (object.Cell == c_LargeCell)
		{
			const uint32_t moved = m_LargeObjects.back();
			m_LargeObjects[object.Position] = moved;
			m_Objects[moved].Position = object.Position;
			m_LargeObjects.pop_back();
		}
		else
		{
			const uint32_t cellIndex = object.Cell;
			Cell& cell = m_Cells[cellIndex];
			const uint32_t moved = cell.Ids.back();
			cell.Ids[object.Position] = moved;
			m_Objects[moved].Position = object.Position;
			cell.Ids.pop_back();
			if (cell.Ids.empty())
			{
				ReleaseCell(cellIndex);
			}
			else if (!cell.BoundsStale)
			{
				cell.BoundsStale = true;
				m_StaleCells.push_back(cellIndex);
			}
		}
		object.Cell = c_None;
	}

	void SpatialIndex::Insert(uint32_t id, const AABB& bounds)
	{
		ST_CORE_ASSERT(!Contains(id), "SpatialIndex: object {} is already in the index", id);
		ST_CORE_ASSERT(bounds.IsValid() && std::isfinite(bounds.Min.x + bounds.Min.y + bounds.Min.z + bounds.Max.x + bounds.Max.y + bounds.Max.z),
			"SpatialIndex: object {} has an invalid box", id);
		if (id >= m_Objects.size())
			m_Objects.resize(static_cast<size_t>(id) + 1);
		Object& object = m_Objects[id];
		object.Bounds = bounds;
		if (IsLarge(bounds))
		{
			object.Cell = c_LargeCell;
			object.Position = static_cast<uint32_t>(m_LargeObjects.size());
			m_LargeObjects.push_back(id);
		}
		else
		{
			AddToCell(id, AcquireCell(GetCellCoordinates(bounds)));
		}
		m_Count++;
	}

	void SpatialIndex::Update(uint32_t id, const AABB& bounds)
	{
		ST_CORE_ASSERT(Contains(id), "SpatialIndex: object {} is not in the index", id);
		ST_CORE_ASSERT(bounds.IsValid() && std::isfinite(bounds.Min.x + bounds.Min.y + bounds.Min.z + bounds.Max.x + bounds.Max.y + bounds.Max.z),
			"SpatialIndex: object {} has an invalid box", id);
		Object& object = m_Objects[id];
		const bool large = IsLarge(bounds);
		if (object.Cell == c_LargeCell && large)
		{
			object.Bounds = bounds;
			return;
		}
		if (object.Cell != c_LargeCell && !large)
		{
			Cell& cell = m_Cells[object.Cell];
			if (cell.Coordinates == GetCellCoordinates(bounds))
			{
				// Stays in its cell: the cell's bounds grow at once and are tightened by Refresh if the object shrank or
				// moved away from part of them.
				if (!ContainsBox(bounds, object.Bounds) && !cell.BoundsStale)
				{
					cell.BoundsStale = true;
					m_StaleCells.push_back(object.Cell);
				}
				object.Bounds = bounds;
				GrowCell(object.Cell, bounds);
				return;
			}
		}

		RemoveFromCell(id);
		object.Bounds = bounds;
		if (large)
		{
			object.Cell = c_LargeCell;
			object.Position = static_cast<uint32_t>(m_LargeObjects.size());
			m_LargeObjects.push_back(id);
		}
		else
		{
			AddToCell(id, AcquireCell(GetCellCoordinates(bounds)));
		}
	}

	void SpatialIndex::Remove(uint32_t id)
	{
		ST_CORE_ASSERT(Contains(id), "SpatialIndex: object {} is not in the index", id);
		RemoveFromCell(id);
		m_Count--;
	}

	void SpatialIndex::Clear()
	{
		m_Objects.clear();
		m_Count = 0;
		m_Cells.clear();
		m_FreeCells.clear();
		m_CellMap.clear();
		m_Blocks.clear();
		m_FreeBlocks.clear();
		m_BlockMap.clear();
		m_ActiveBlocks.clear();
		m_ActiveBlockSlots.clear();
		m_LargeObjects.clear();
		m_StaleCells.clear();
		m_StaleBlocks.clear();
	}

	void SpatialIndex::Refresh()
	{
		for (const uint32_t cellIndex : m_StaleCells)
		{
			Cell& cell = m_Cells[cellIndex];
			if (!cell.BoundsStale)
				continue; // Released (or already refreshed) since it was listed
			cell.BoundsStale = false;
			AABB bounds = m_Objects[cell.Ids.front()].Bounds;
			for (const uint32_t id : cell.Ids)
				Unite(bounds, m_Objects[id].Bounds);
			cell.Bounds = bounds;
			Block& block = m_Blocks[cell.Block];
			if (!block.BoundsStale)
			{
				block.BoundsStale = true;
				m_StaleBlocks.push_back(cell.Block);
			}
		}
		m_StaleCells.clear();

		for (const uint32_t blockIndex : m_StaleBlocks)
		{
			Block& block = m_Blocks[blockIndex];
			if (!block.BoundsStale)
				continue;
			block.BoundsStale = false;
			AABB bounds = m_Cells[block.Cells.front()].Bounds;
			for (const uint32_t cellIndex : block.Cells)
				Unite(bounds, m_Cells[cellIndex].Bounds);
			block.Bounds = bounds;
		}
		m_StaleBlocks.clear();
	}

	template<typename Classify, typename Test>
	void SpatialIndex::Query(const Classify& classify, const Test& test, std::vector<uint32_t>& outIds) const
	{
		for (const uint32_t blockIndex : m_ActiveBlocks)
		{
			const Block& block = m_Blocks[blockIndex];
			const VolumeOverlap blockOverlap = classify(block.Bounds);
			if (blockOverlap == VolumeOverlap::Outside)
				continue;
			for (const uint32_t cellIndex : block.Cells)
			{
				const Cell& cell = m_Cells[cellIndex];
				const VolumeOverlap cellOverlap = blockOverlap == VolumeOverlap::Inside ? VolumeOverlap::Inside : classify(cell.Bounds);
				if (cellOverlap == VolumeOverlap::Outside)
					continue;
				if (cellOverlap == VolumeOverlap::Inside)
				{
					outIds.insert(outIds.end(), cell.Ids.begin(), cell.Ids.end());
					continue;
				}
				for (const uint32_t id : cell.Ids)
				{
					if (test(m_Objects[id].Bounds))
						outIds.push_back(id);
				}
			}
		}
		for (const uint32_t id : m_LargeObjects)
		{
			if (test(m_Objects[id].Bounds))
				outIds.push_back(id);
		}
	}

	void SpatialIndex::QueryFrustum(const Frustum& frustum, std::vector<uint32_t>& outIds) const
	{
		const std::span<const glm::vec4> planes = frustum.GetPlanes();
		Query([&](const AABB& bounds) { return ClassifyPlanes(bounds, planes); }, [&](const AABB& bounds) { return frustum.IsAABBVisible(bounds); }, outIds);
	}

	void SpatialIndex::QueryPlanes(std::span<const glm::vec4> planes, std::vector<uint32_t>& outIds) const
	{
		Query([&](const AABB& bounds) { return ClassifyPlanes(bounds, planes); }, [&](const AABB& bounds) { return IntersectsPlanes(bounds, planes); }, outIds);
	}

	void SpatialIndex::QuerySphere(const glm::vec3& center, float radius, std::vector<uint32_t>& outIds) const
	{
		Query([&](const AABB& bounds) { return ClassifySphere(bounds, center, radius); }, [&](const AABB& bounds) { return IntersectsSphere(bounds, center, radius); },
			outIds);
	}

}
