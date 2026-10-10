#include <doctest/doctest.h>

#include "Strata/Math/Math.h"
#include "Strata/Renderer/SpatialIndex.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <random>
#include <vector>

using namespace Strata;

namespace
{

	constexpr uint32_t c_BoxCount = 100'000;
	constexpr float c_WorldSize = 4000.0f;

	// A box of random size somewhere in the world: mostly small, some spanning several cells, a few huge ones.
	AABB RandomBox(std::mt19937& random)
	{
		std::uniform_real_distribution<float> position(-c_WorldSize * 0.5f, c_WorldSize * 0.5f);
		std::uniform_real_distribution<float> unit(0.0f, 1.0f);
		const float kind = unit(random);
		const float size = kind < 0.9f ? 0.1f + unit(random) * 4.0f : (kind < 0.995f ? 20.0f + unit(random) * 200.0f : 500.0f + unit(random) * 3000.0f);
		const glm::vec3 center(position(random), position(random) * 0.1f, position(random));
		const glm::vec3 extents = glm::vec3(0.5f + unit(random), 0.5f + unit(random), 0.5f + unit(random)) * size * 0.5f;
		return AABB(center - extents, center + extents);
	}

	Frustum RandomFrustum(std::mt19937& random, bool orthographic)
	{
		std::uniform_real_distribution<float> position(-c_WorldSize * 0.5f, c_WorldSize * 0.5f);
		std::uniform_real_distribution<float> unit(0.0f, 1.0f);
		const glm::vec3 eye(position(random), 5.0f + unit(random) * 300.0f, position(random));
		const glm::vec3 target(position(random), 0.0f, position(random));
		const glm::mat4 view = glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
		const float farClip = 50.0f + unit(random) * 3000.0f;
		const glm::mat4 projection = orthographic ? Math::OrthographicReverseZ(-300.0f, 300.0f, -200.0f, 200.0f, 0.1f, farClip)
			: Math::PerspectiveReverseZ(glm::radians(20.0f + unit(random) * 70.0f), 0.5f + unit(random) * 2.0f, 0.1f, farClip);
		return Frustum(projection * view);
	}

	template<typename Test>
	std::vector<uint32_t> BruteForce(const std::vector<AABB>& boxes, const std::vector<bool>& present, const Test& test)
	{
		std::vector<uint32_t> ids;
		for (uint32_t id = 0; id < boxes.size(); id++)
		{
			if (present[id] && test(boxes[id]))
				ids.push_back(id);
		}
		return ids;
	}

	std::vector<uint32_t> Sorted(std::vector<uint32_t> ids)
	{
		std::sort(ids.begin(), ids.end());
		return ids;
	}

	// Every kind of query, against a brute-force test of every box.
	void CheckQueries(const SpatialIndex& index, const std::vector<AABB>& boxes, const std::vector<bool>& present, std::mt19937& random)
	{
		std::uniform_real_distribution<float> position(-c_WorldSize * 0.5f, c_WorldSize * 0.5f);
		std::uniform_real_distribution<float> unit(0.0f, 1.0f);
		for (int query = 0; query < 8; query++)
		{
			CAPTURE(query);
			const Frustum frustum = RandomFrustum(random, query % 4 == 3);
			std::vector<uint32_t> found;
			index.QueryFrustum(frustum, found);
			CHECK(Sorted(found) == BruteForce(boxes, present, [&](const AABB& box) { return frustum.IsAABBVisible(box); }));

			// A cascade-like volume: a box with an open side, in an arbitrary orientation.
			const glm::vec3 normal = glm::normalize(glm::vec3(unit(random) - 0.5f, -1.0f, unit(random) - 0.5f));
			const glm::vec3 side = glm::normalize(glm::cross(normal, glm::vec3(0.0f, 0.0f, 1.0f)));
			const glm::vec3 up = glm::cross(side, normal);
			const glm::vec3 center(position(random), 0.0f, position(random));
			const float halfSize = 20.0f + unit(random) * 400.0f;
			const std::array<glm::vec4, 5> planes = {
				glm::vec4(side, -glm::dot(side, center) + halfSize), glm::vec4(-side, glm::dot(side, center) + halfSize),
				glm::vec4(up, -glm::dot(up, center) + halfSize), glm::vec4(-up, glm::dot(up, center) + halfSize),
				glm::vec4(normal, -glm::dot(normal, center) + 100.0f) };
			found.clear();
			index.QueryPlanes(planes, found);
			CHECK(Sorted(found) == BruteForce(boxes, present, [&](const AABB& box) { return SpatialIndex::IntersectsPlanes(box, planes); }));

			const glm::vec3 sphereCenter(position(random), position(random) * 0.05f, position(random));
			const float radius = query == 0 ? 0.0f : unit(random) * 600.0f;
			found.clear();
			index.QuerySphere(sphereCenter, radius, found);
			CHECK(Sorted(found) == BruteForce(boxes, present, [&](const AABB& box) { return SpatialIndex::IntersectsSphere(box, sphereCenter, radius); }));
		}
	}

}

TEST_SUITE("Renderer.SpatialIndex")
{
	TEST_CASE("Queries return exactly what a brute-force test of 100,000 random boxes returns, through edits")
	{
		std::mt19937 random(1234);
		SpatialIndex index;
		std::vector<AABB> boxes(c_BoxCount);
		std::vector<bool> present(c_BoxCount, true);
		for (uint32_t id = 0; id < c_BoxCount; id++)
		{
			boxes[id] = RandomBox(random);
			index.Insert(id, boxes[id]);
		}
		CHECK(index.GetCount() == c_BoxCount);
		// Boxes longer than four cells along an axis are kept apart from the grid.
		const float largeSize = index.GetSpecification().CellSize * index.GetSpecification().MaxCellsPerObject;
		const size_t large = static_cast<size_t>(std::count_if(boxes.begin(), boxes.end(), [largeSize](const AABB& box)
		{
			const glm::vec3 size = box.GetSize();
			return size.x > largeSize || size.y > largeSize || size.z > largeSize;
		}));
		CHECK(large > 0);
		CHECK(index.GetLargeObjectCount() == large);
		CheckQueries(index, boxes, present, random);

		// Moves within a cell and across cells, objects growing large and shrinking back, removals and reinsertions: the
		// queries stay exact before and after the bounds are refreshed.
		std::uniform_real_distribution<float> unit(0.0f, 1.0f);
		std::uniform_int_distribution<uint32_t> pick(0, c_BoxCount - 1);
		for (int round = 0; round < 3; round++)
		{
			CAPTURE(round);
			for (int edit = 0; edit < 20'000; edit++)
			{
				const uint32_t id = pick(random);
				const float kind = unit(random);
				if (!present[id])
				{
					boxes[id] = RandomBox(random);
					index.Insert(id, boxes[id]);
					present[id] = true;
				}
				else if (kind < 0.15f)
				{
					index.Remove(id);
					present[id] = false;
				}
				else if (kind < 0.6f)
				{
					const glm::vec3 offset = (glm::vec3(unit(random), unit(random), unit(random)) - 0.5f) * 2.0f;
					boxes[id] = AABB(boxes[id].Min + offset, boxes[id].Max + offset);
					index.Update(id, boxes[id]);
				}
				else
				{
					boxes[id] = RandomBox(random);
					index.Update(id, boxes[id]);
				}
				CHECK(index.Contains(id) == present[id]);
			}
			CheckQueries(index, boxes, present, random);
			index.Refresh();
			CheckQueries(index, boxes, present, random);
		}
	}

	TEST_CASE("Results follow the order of edits, and empty cells and blocks are released")
	{
		std::mt19937 random(99);
		SpatialIndex first;
		SpatialIndex second;
		std::vector<AABB> boxes;
		for (uint32_t id = 0; id < 2000; id++)
		{
			boxes.push_back(RandomBox(random));
			first.Insert(id, boxes.back());
			second.Insert(id, boxes.back());
		}
		for (uint32_t id = 0; id < 2000; id += 3)
		{
			first.Remove(id);
			second.Remove(id);
		}
		const Frustum frustum = RandomFrustum(random, false);
		std::vector<uint32_t> firstIds;
		std::vector<uint32_t> secondIds;
		first.QueryFrustum(frustum, firstIds);
		second.QueryFrustum(frustum, secondIds);
		CHECK(firstIds == secondIds);
		first.QuerySphere(glm::vec3(0.0f), 1.0e6f, firstIds);
		CHECK(firstIds.size() == secondIds.size() + first.GetCount());

		for (uint32_t id = 0; id < 2000; id++)
		{
			if (first.Contains(id))
				first.Remove(id);
		}
		CHECK(first.GetCount() == 0);
		CHECK(first.GetCellCount() == 0);
		CHECK(first.GetBlockCount() == 0);
		CHECK(first.GetLargeObjectCount() == 0);
		firstIds.clear();
		first.QuerySphere(glm::vec3(0.0f), 1.0e6f, firstIds);
		CHECK(firstIds.empty());

		second.Clear();
		CHECK(second.GetCount() == 0);
		CHECK_FALSE(second.Contains(1));
		second.Insert(1, AABB(glm::vec3(0.0f), glm::vec3(1.0f)));
		CHECK(second.GetCellCount() == 1);
	}

	TEST_CASE("Objects in a cell inside the volume are taken whole; large objects are tested one by one")
	{
		SpatialIndexSpecification specification;
		specification.CellSize = 10.0f;
		specification.MaxCellsPerObject = 2.0f;
		SpatialIndex index(specification);
		index.Insert(0, AABB(glm::vec3(1.0f), glm::vec3(2.0f)));
		index.Insert(1, AABB(glm::vec3(3.0f), glm::vec3(4.0f)));
		index.Insert(2, AABB(glm::vec3(-100.0f), glm::vec3(100.0f))); // Longer than two cells
		CHECK(index.GetCellCount() == 1);
		CHECK(index.GetLargeObjectCount() == 1);

		const std::array<glm::vec4, 1> halfSpace = { glm::vec4(1.0f, 0.0f, 0.0f, 0.0f) }; // x >= 0
		std::vector<uint32_t> ids;
		index.QueryPlanes(halfSpace, ids);
		CHECK(Sorted(ids) == std::vector<uint32_t> { 0, 1, 2 });
		const std::array<glm::vec4, 1> beyond = { glm::vec4(1.0f, 0.0f, 0.0f, -150.0f) }; // x >= 150
		ids.clear();
		index.QueryPlanes(beyond, ids);
		CHECK(ids.empty());

		CHECK(SpatialIndex::ClassifyPlanes(AABB(glm::vec3(1.0f), glm::vec3(2.0f)), halfSpace) == VolumeOverlap::Inside);
		CHECK(SpatialIndex::ClassifyPlanes(AABB(glm::vec3(-1.0f), glm::vec3(2.0f)), halfSpace) == VolumeOverlap::Intersecting);
		CHECK(SpatialIndex::ClassifyPlanes(AABB(glm::vec3(-2.0f), glm::vec3(-1.0f)), halfSpace) == VolumeOverlap::Outside);
		CHECK(SpatialIndex::ClassifySphere(AABB(glm::vec3(-1.0f), glm::vec3(1.0f)), glm::vec3(0.0f), 2.0f) == VolumeOverlap::Inside);
		CHECK(SpatialIndex::ClassifySphere(AABB(glm::vec3(-1.0f), glm::vec3(1.0f)), glm::vec3(2.0f, 0.0f, 0.0f), 1.5f) == VolumeOverlap::Intersecting);
		CHECK(SpatialIndex::ClassifySphere(AABB(glm::vec3(-1.0f), glm::vec3(1.0f)), glm::vec3(5.0f, 0.0f, 0.0f), 1.5f) == VolumeOverlap::Outside);

		// Shrinking back below the limit moves the object into a cell.
		index.Update(2, AABB(glm::vec3(20.0f), glm::vec3(21.0f)));
		CHECK(index.GetLargeObjectCount() == 0);
		CHECK(index.GetCellCount() == 2);
		CHECK(index.GetBounds(2).Min == glm::vec3(20.0f));
	}
}
