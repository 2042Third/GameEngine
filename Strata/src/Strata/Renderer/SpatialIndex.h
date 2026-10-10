#pragma once

#include "Strata/Math/AABB.h"
#include "Strata/Math/Frustum.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace Strata
{

	struct SpatialIndexSpecification
	{
		float CellSize = 32.0f; // World units per cell edge
		// Objects longer than this many cells along an axis (terrains, sky domes) are kept in a list of their own and tested
		// one by one, so they do not make the cell they fall into intersect every query.
		float MaxCellsPerObject = 4.0f;
	};

	// Where an AABB lies relative to a convex volume.
	enum class VolumeOverlap : uint8_t
	{
		Outside = 0,
		Intersecting,
		Inside
	};

	// A loose two-level grid of axis-aligned boxes identified by small integer ids (render proxies), for the queries of
	// rendering: view frustums, shadow cascade volumes and spheres. Each object lives in the cell that contains the center
	// of its box; cells keep the union of their objects' boxes (loose bounds, so objects may reach into neighboring cells)
	// and are grouped in blocks of c_BlockCells^3 cells with the union of their cells. A query tests blocks, then the cells
	// of blocks that straddle the volume, then the objects of cells that straddle it; whatever lies wholly inside is taken
	// without further tests. The result is exactly the objects whose own box passes the volume's test (the same test as
	// Frustum::IsAABBVisible), in an order that depends only on the sequence of edits.
	//
	// Edits cost a constant amount: an object that stays in its cell only has its box replaced. Bounds grow at once and
	// shrink when Refresh recomputes them, so between the two they are larger than needed, which costs time but never
	// changes a result. Queries are const and may run concurrently; edits and Refresh need exclusive access.
	class SpatialIndex
	{
	public:
		static constexpr uint32_t c_BlockCells = 8;

		explicit SpatialIndex(const SpatialIndexSpecification& specification = {});

		// The id must not be in the index; the box must be valid and finite.
		void Insert(uint32_t id, const AABB& bounds);
		// Replaces the box of an object in the index.
		void Update(uint32_t id, const AABB& bounds);
		void Remove(uint32_t id);
		bool Contains(uint32_t id) const { return id < m_Objects.size() && m_Objects[id].Cell != c_None; }
		const AABB& GetBounds(uint32_t id) const { return m_Objects[id].Bounds; }
		void Clear();
		size_t GetCount() const { return m_Count; }

		// Tightens the bounds of cells and blocks that lost or shrank objects since the last refresh.
		void Refresh();

		// Appends the objects whose box intersects the frustum (Frustum::IsAABBVisible).
		void QueryFrustum(const Frustum& frustum, std::vector<uint32_t>& outIds) const;
		// Appends the objects whose box is not wholly behind any of the planes (xyz: unit inward normal, w: distance), the
		// same test with any convex volume.
		void QueryPlanes(std::span<const glm::vec4> planes, std::vector<uint32_t>& outIds) const;
		// Appends the objects whose box comes within `radius` of `center` (IntersectsSphere).
		void QuerySphere(const glm::vec3& center, float radius, std::vector<uint32_t>& outIds) const;

		// The tests the queries apply to each object.
		static bool IntersectsPlanes(const AABB& bounds, std::span<const glm::vec4> planes);
		static bool IntersectsSphere(const AABB& bounds, const glm::vec3& center, float radius);
		static VolumeOverlap ClassifyPlanes(const AABB& bounds, std::span<const glm::vec4> planes);
		static VolumeOverlap ClassifySphere(const AABB& bounds, const glm::vec3& center, float radius);

		// Diagnostics: occupied cells and blocks, and objects kept in the list of large ones.
		size_t GetCellCount() const { return m_Cells.size() - m_FreeCells.size(); }
		size_t GetBlockCount() const { return m_Blocks.size() - m_FreeBlocks.size(); }
		size_t GetLargeObjectCount() const { return m_LargeObjects.size(); }
		const SpatialIndexSpecification& GetSpecification() const { return m_Specification; }
	private:
		static constexpr uint32_t c_None = ~0u;
		static constexpr uint32_t c_LargeCell = ~0u - 1u; // Object kept in m_LargeObjects

		struct Object
		{
			AABB Bounds;
			uint32_t Cell = c_None;  // Index into m_Cells, c_LargeCell or c_None (not in the index)
			uint32_t Position = 0;   // In the cell's object list (or m_LargeObjects)
		};

		struct Cell
		{
			glm::ivec3 Coordinates = glm::ivec3(0);
			AABB Bounds;               // Union of its objects' boxes (or larger, until refreshed)
			std::vector<uint32_t> Ids;
			uint32_t Block = c_None;
			uint32_t PositionInBlock = 0;
			bool BoundsStale = false;  // Lost or shrank an object since its bounds were computed
		};

		struct Block
		{
			glm::ivec3 Coordinates = glm::ivec3(0);
			AABB Bounds;               // Union of its cells' bounds (or larger, until refreshed)
			std::vector<uint32_t> Cells;
			bool BoundsStale = false;
		};

		// Appends the objects of the blocks and cells `classify` finds inside the volume, and the objects passing `test` in
		// those it finds straddling it.
		template<typename Classify, typename Test>
		void Query(const Classify& classify, const Test& test, std::vector<uint32_t>& outIds) const;

		bool IsLarge(const AABB& bounds) const;
		glm::ivec3 GetCellCoordinates(const AABB& bounds) const;
		uint32_t AcquireCell(const glm::ivec3& coordinates);
		void ReleaseCell(uint32_t cellIndex);
		void AddToCell(uint32_t id, uint32_t cellIndex);
		void RemoveFromCell(uint32_t id);
		void GrowCell(uint32_t cellIndex, const AABB& bounds);
		static uint64_t PackCoordinates(const glm::ivec3& coordinates);
	private:
		SpatialIndexSpecification m_Specification;
		float m_InverseCellSize = 1.0f / 32.0f;
		std::vector<Object> m_Objects; // Indexed by id
		size_t m_Count = 0;
		std::vector<Cell> m_Cells;
		std::vector<uint32_t> m_FreeCells;
		std::unordered_map<uint64_t, uint32_t> m_CellMap;
		std::vector<Block> m_Blocks;
		std::vector<uint32_t> m_FreeBlocks;
		std::unordered_map<uint64_t, uint32_t> m_BlockMap;
		std::vector<uint32_t> m_ActiveBlocks;      // Occupied blocks, in the order queries visit them
		std::vector<uint32_t> m_ActiveBlockSlots;  // Per block: its position in m_ActiveBlocks
		std::vector<uint32_t> m_LargeObjects;
		std::vector<uint32_t> m_StaleCells;
		std::vector<uint32_t> m_StaleBlocks;
	};

}
