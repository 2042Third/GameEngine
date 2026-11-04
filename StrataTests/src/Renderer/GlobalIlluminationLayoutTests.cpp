#include <doctest/doctest.h>

#include "Strata/Renderer/GlobalIlluminationLayout.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

using namespace Strata;

namespace
{

	// Include/Scene.slang's OctahedralEncode.
	glm::vec2 OctahedralEncode(glm::vec3 direction)
	{
		direction /= std::abs(direction.x) + std::abs(direction.y) + std::abs(direction.z);
		if (direction.z >= 0.0f)
			return glm::vec2(direction);
		return (1.0f - glm::abs(glm::vec2(direction.y, direction.x))) * glm::vec2(direction.x >= 0.0f ? 1.0f : -1.0f, direction.y >= 0.0f ? 1.0f : -1.0f);
	}

	GlobalIlluminationLayout GetLayout(GlobalIlluminationLevel level)
	{
		const std::optional<GlobalIlluminationLayout> layout = GlobalIllumination::ComputeLayout(level);
		REQUIRE(layout.has_value());
		return *layout;
	}

	struct ExpectedCascade
	{
		float Spacing;
		glm::uvec3 Grid;
		uint32_t Directions;
		float Start;
		float End;
		float LeadIn;
		float Overlap;
		uint32_t MinimumLevel;
		uint32_t SubRays;
	};

	void CheckCascades(const GlobalIlluminationLayout& layout, const std::vector<ExpectedCascade>& expected)
	{
		REQUIRE(layout.Cascades.size() == expected.size());
		for (size_t k = 0; k < expected.size(); k++)
		{
			CAPTURE(k);
			const ProbeCascadeLayout& cascade = layout.Cascades[k];
			CHECK(cascade.Spacing == expected[k].Spacing);
			CHECK(cascade.Grid == expected[k].Grid);
			CHECK(cascade.DirectionsPerAxis * cascade.DirectionsPerAxis == expected[k].Directions);
			CHECK(cascade.IntervalStart == expected[k].Start);
			CHECK(cascade.IntervalEnd == expected[k].End);
			CHECK(cascade.LeadIn == expected[k].LeadIn);
			CHECK(cascade.Overlap == expected[k].Overlap);
			CHECK(cascade.MinimumLevel == expected[k].MinimumLevel);
			CHECK(cascade.SubRays == expected[k].SubRays);
			// Every cascade covers the same field.
			CHECK(glm::vec3(cascade.Grid) * cascade.Spacing == layout.FieldExtent);
		}
	}

	uint64_t CountOverlap(const VoxelBox& a, const VoxelBox& b)
	{
		return a.Intersection(b).GetVoxelCount();
	}

}

TEST_SUITE("Renderer.GlobalIlluminationLayout")
{
	TEST_CASE("The High and Low layouts")
	{
		CHECK_FALSE(GlobalIllumination::ComputeLayout(GlobalIlluminationLevel::Off).has_value());
		constexpr float c_Infinity = std::numeric_limits<float>::infinity();

		const GlobalIlluminationLayout high = GetLayout(GlobalIlluminationLevel::High);
		REQUIRE(high.Levels.size() == 4);
		const float highSizes[] = { 0.1f, 0.2f, 0.4f, 0.8f };
		for (size_t index = 0; index < high.Levels.size(); index++)
		{
			CAPTURE(index);
			CHECK(high.Levels[index].VoxelSize == highSizes[index]);
			CHECK(high.Levels[index].Dimensions == glm::uvec3(128, 64, 128));
			// Extents 12.8 x 6.4 x 12.8 m, doubling with each level.
			const glm::vec3 extent = glm::vec3(high.Levels[index].Dimensions) * high.Levels[index].VoxelSize;
			CHECK(extent.x == doctest::Approx(12.8f * static_cast<float>(1u << index)));
			CHECK(extent.y == doctest::Approx(6.4f * static_cast<float>(1u << index)));
			CHECK(extent.z == doctest::Approx(12.8f * static_cast<float>(1u << index)));
		}
		CheckCascades(high, {
			{ 0.5f, { 48, 40, 48 }, 64, 0.0f, 0.5f, 0.0f, 0.5f, 0, 1 },
			{ 1.0f, { 24, 20, 24 }, 64, 0.5f, 2.0f, 0.5f, 1.0f, 0, 2 },
			{ 2.0f, { 12, 10, 12 }, 256, 2.0f, 8.0f, 2.0f, 2.0f, 1, 2 },
			{ 4.0f, { 6, 5, 6 }, 1024, 8.0f, c_Infinity, 8.0f, 0.0f, 1, 1 },
		});
		CHECK(high.FieldExtent == glm::vec3(24.0f, 20.0f, 24.0f));
		CHECK(high.FieldSnap == 4.0f);
		CHECK(high.FieldFullExtent == glm::vec2(7.5f, 5.5f));
		CHECK(high.FieldFade == 2.0f);
		CHECK(high.BatchVoxels == 262144);
		CHECK(high.GetRaysPerFrame() == 10506240);

		const GlobalIlluminationLayout low = GetLayout(GlobalIlluminationLevel::Low);
		REQUIRE(low.Levels.size() == 3);
		const float lowSizes[] = { 0.2f, 0.4f, 0.8f };
		for (size_t index = 0; index < low.Levels.size(); index++)
		{
			CAPTURE(index);
			CHECK(low.Levels[index].VoxelSize == lowSizes[index]);
			CHECK(low.Levels[index].Dimensions == glm::uvec3(64, 32, 64));
			const glm::vec3 extent = glm::vec3(low.Levels[index].Dimensions) * low.Levels[index].VoxelSize;
			CHECK(extent.x == doctest::Approx(12.8f * static_cast<float>(1u << index)));
			CHECK(extent.y == doctest::Approx(6.4f * static_cast<float>(1u << index)));
		}
		CheckCascades(low, {
			{ 1.0f, { 24, 20, 24 }, 64, 0.0f, 1.0f, 0.0f, 1.0f, 0, 1 },
			{ 2.0f, { 12, 10, 12 }, 64, 1.0f, 4.0f, 1.0f, 2.0f, 0, 2 },
			{ 4.0f, { 6, 5, 6 }, 256, 4.0f, c_Infinity, 4.0f, 0.0f, 1, 1 },
		});
		CHECK(low.FieldExtent == glm::vec3(24.0f, 20.0f, 24.0f));
		CHECK(low.FieldSnap == 4.0f);
		CHECK(low.FieldFullExtent == glm::vec2(7.0f, 5.0f));
		CHECK(low.FieldFade == 2.0f);
		// Low's scratch is one level: a 64^3 batch would not fit its 64 x 32 x 64 levels' worth.
		CHECK(low.BatchVoxels == 131072);
		CHECK(low.GetRaysPerFrame() == 1152000);
	}

	TEST_CASE("Memory adds up")
	{
		const GlobalIlluminationMemory high = GetLayout(GlobalIlluminationLevel::High).GetMemory();
		CHECK(high.VoxelLevels == 83886080);
		CHECK(high.Scratch == 8388608);
		CHECK(high.Atlases == 10321920); // Cascades 1-3: cascade 0's rays go straight into the field
		CHECK(high.Weights == 840960);
		CHECK(high.Validity == 421200);
		CHECK(high.Field == 4423680);
		CHECK(high.Tiles == 262144);
		CHECK(high.WorkItems == 1572864);
		CHECK(high.GetTotal() == 110117456);
		CHECK(GetLayout(GlobalIlluminationLevel::High).GetMemoryBytes() == 110117456);

		const GlobalIlluminationMemory low = GetLayout(GlobalIlluminationLevel::Low).GetMemory();
		CHECK(low.VoxelLevels == 7864320);
		CHECK(low.Scratch == 4194304);
		CHECK(low.Atlases == 1105920);
		CHECK(low.Weights == 103680);
		CHECK(low.Validity == 52560);
		CHECK(low.Field == 552960);
		CHECK(low.Tiles == 262144);
		CHECK(low.WorkItems == 1572864);
		CHECK(low.GetTotal() == 15708752);
	}

	TEST_CASE("Directions and solid angles")
	{
		for (uint32_t a : { 4u, 8u, 16u, 32u })
		{
			CAPTURE(a);
			std::vector<glm::vec3> directions;
			double totalSolidAngle = 0.0;
			glm::dvec3 sum(0.0);
			for (uint32_t v = 0; v < a; v++)
			{
				for (uint32_t u = 0; u < a; u++)
				{
					const glm::vec3 direction = GlobalIllumination::GetDirection(u, v, a);
					CHECK(glm::length(direction) == doctest::Approx(1.0f).epsilon(1e-6));
					const float solidAngle = GlobalIllumination::GetSolidAngle(u, v, a);
					CHECK(solidAngle > 0.0f);
					totalSolidAngle += solidAngle;
					sum += glm::dvec3(direction) * static_cast<double>(solidAngle);
					directions.push_back(direction);
				}
			}
			CHECK(std::abs(totalSolidAngle - 4.0 * glm::pi<double>()) < 1e-5);
			// Symmetric: the directions' mirror images through each axis plane are directions of the tile too, so the
			// weighted directions add up to nothing.
			CHECK(glm::length(sum) < 1e-5);
			for (const glm::vec3& direction : directions)
			{
				for (int axis = 0; axis < 3; axis++)
				{
					glm::vec3 mirrored = direction;
					mirrored[axis] = -mirrored[axis];
					const bool found = std::any_of(directions.begin(), directions.end(), [&](const glm::vec3& other) { return glm::length(other - mirrored) < 1e-5f; });
					CHECK(found);
				}
			}
			// The four children of a texel (in the next cascade's tile of 2a) lie within it.
			for (uint32_t v = 0; v < a; v++)
			{
				for (uint32_t u = 0; u < a; u++)
				{
					const glm::vec2 low = glm::vec2(static_cast<float>(u), static_cast<float>(v)) / static_cast<float>(a) * 2.0f - 1.0f;
					const glm::vec2 high = glm::vec2(static_cast<float>(u + 1), static_cast<float>(v + 1)) / static_cast<float>(a) * 2.0f - 1.0f;
					for (uint32_t child = 0; child < 4; child++)
					{
						const glm::vec2 encoded = OctahedralEncode(GlobalIllumination::GetDirection(2 * u + child % 2, 2 * v + child / 2, 2 * a));
						CHECK(encoded.x >= low.x - 1e-5f);
						CHECK(encoded.y >= low.y - 1e-5f);
						CHECK(encoded.x <= high.x + 1e-5f);
						CHECK(encoded.y <= high.y + 1e-5f);
					}
				}
			}
		}
	}

	TEST_CASE("Windows snap and follow")
	{
		for (GlobalIlluminationLevel quality : { GlobalIlluminationLevel::High, GlobalIlluminationLevel::Low })
		{
			const GlobalIlluminationLayout layout = GetLayout(quality);
			for (const glm::dvec3& camera : { glm::dvec3(0.0), glm::dvec3(1000.37, 2.1, -999.93), glm::dvec3(20000.0, 1.7, 20000.0),
					 glm::dvec3(-20000.0, -3.3, -1234.567), glm::dvec3(-0.05, -0.05, -0.05) })
			{
				CAPTURE(camera.x);
				CAPTURE(camera.z);
				for (const VoxelLevelLayout& level : layout.Levels)
				{
					CAPTURE(level.VoxelSize);
					const glm::i64vec3 minimum = GlobalIllumination::GetWindowMinimum(level, camera);
					for (int axis = 0; axis < 3; axis++)
					{
						CHECK(minimum[axis] % static_cast<int64_t>(RenderData::c_GIScrollVoxels) == 0);
						// The camera lies between half the dimensions and half plus a step from the minimum face.
						const double fromMinimum = camera[axis] / static_cast<double>(level.VoxelSize) - static_cast<double>(minimum[axis]);
						const double half = static_cast<double>(level.Dimensions[axis] / 2);
						CHECK(fromMinimum >= half);
						CHECK(fromMinimum <= half + RenderData::c_GIScrollVoxels);
					}
				}
			}
		}
	}

	TEST_CASE("Scrolling queues exactly what entered")
	{
		const glm::uvec3 dimensions(128, 64, 128);
		const glm::i64vec3 size(dimensions);
		const glm::i64vec3 start(-1003, 37, 4096 - 64);
		std::vector<glm::i64vec3> moves;
		for (int64_t distance : { 1, 3, 4, 5, 64, 127, 300 })
		{
			for (int64_t sign : { 1, -1 })
			{
				for (int axis = 0; axis < 3; axis++)
				{
					glm::i64vec3 move(0);
					move[axis] = distance * sign;
					moves.push_back(move);
				}
				moves.push_back(glm::i64vec3(distance * sign, distance, -distance * sign));
			}
		}
		for (const glm::i64vec3& move : moves)
		{
			CAPTURE(move.x);
			CAPTURE(move.y);
			CAPTURE(move.z);
			const glm::i64vec3 end = start + move;
			std::vector<VoxelBox> slabs;
			const bool scrolled = GlobalIllumination::GetScrollSlabs(start, end, dimensions, slabs);
			const bool rebuild = std::abs(move.x) >= size.x || std::abs(move.y) >= size.y || std::abs(move.z) >= size.z;
			CHECK(scrolled == !rebuild);
			if (rebuild)
			{
				CHECK(slabs.empty());
				continue;
			}
			const VoxelBox before { start, start + size };
			const VoxelBox after { end, end + size };
			uint64_t total = 0;
			for (size_t index = 0; index < slabs.size(); index++)
			{
				const VoxelBox& slab = slabs[index];
				CHECK_FALSE(slab.IsEmpty());
				CHECK(after.Contains(slab));
				CHECK(CountOverlap(slab, before) == 0);
				for (size_t other = index + 1; other < slabs.size(); other++)
					CHECK(CountOverlap(slab, slabs[other]) == 0);
				// Contiguous in the textures and at most 64 voxels a side.
				const glm::uvec3 texel = GlobalIllumination::GetTexel(slab.Minimum, dimensions);
				const glm::i64vec3 extent = slab.Maximum - slab.Minimum;
				for (int axis = 0; axis < 3; axis++)
				{
					CHECK(static_cast<int64_t>(texel[axis]) + extent[axis] <= size[axis]);
					CHECK(extent[axis] <= static_cast<int64_t>(RenderData::c_GIMaxRegionVoxels));
				}
				total += slab.GetVoxelCount();
			}
			// Disjoint pieces of the new window outside the old one, as many voxels as that difference: exactly it.
			CHECK(total == after.GetVoxelCount() - CountOverlap(after, before));
		}
	}

	TEST_CASE("Voxels are independent of the camera")
	{
		const GlobalIlluminationLayout layout = GetLayout(GlobalIlluminationLevel::High);
		// A rotated, scaled instance 1.4 km out, and two cameras 37.3 m apart near it, both within one anchor cell of every level.
		const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(1361.37f, 2.5f, -517.81f))
			* glm::rotate(glm::mat4(1.0f), 0.7f, glm::normalize(glm::vec3(0.3f, 1.0f, -0.2f))) * glm::scale(glm::mat4(1.0f), glm::vec3(1.7f, 0.9f, 2.3f));
		const RenderData::AffineTransform rows = RenderData::AffineTransform::FromMatrix(world);
		const glm::vec3 vertices[] = { { 0.13f, -0.41f, 0.77f }, { -0.91f, 0.05f, 0.33f }, { 0.52f, 0.87f, -0.64f } };
		const glm::dvec3 cameraA(1350.0, 1.7, -520.0);
		const glm::dvec3 cameraB = cameraA + glm::dvec3(37.3, 0.0, 0.0);
		for (const VoxelLevelLayout& level : layout.Levels)
		{
			CAPTURE(level.VoxelSize);
			const glm::i64vec3 windowA = GlobalIllumination::GetWindowMinimum(level, cameraA);
			const glm::i64vec3 windowB = GlobalIllumination::GetWindowMinimum(level, cameraB);
			CHECK(windowA != windowB);
			// The windows differ, their anchors (what the clipmap keeps until the camera is 1 km away) do not.
			const glm::i64vec3 anchor = GlobalIllumination::GetAnchorVoxel(windowA);
			CHECK(GlobalIllumination::GetAnchorVoxel(windowB) == anchor);
			const glm::vec3 anchorPosition = glm::vec3(glm::dvec3(anchor) * static_cast<double>(level.VoxelSize));
			for (const glm::vec3& vertex : vertices)
			{
				const glm::vec3 a = GlobalIllumination::GetVoxelCoordinate(rows, vertex, anchorPosition, level.VoxelSize);
				const glm::vec3 b = GlobalIllumination::GetVoxelCoordinate(rows, vertex, glm::vec3(glm::dvec3(GlobalIllumination::GetAnchorVoxel(windowB))
					* static_cast<double>(level.VoxelSize)), level.VoxelSize);
				CHECK(std::memcmp(&a, &b, sizeof(a)) == 0);
				// Within the camera's window, near the instance's world position.
				const glm::i64vec3 voxel = anchor + glm::i64vec3(glm::floor(a));
				const glm::vec3 position = glm::vec3(world * glm::vec4(vertex, 1.0f));
				const glm::i64vec3 expected = GlobalIllumination::GetVoxelIndex(glm::dvec3(position), level.VoxelSize);
				CHECK(glm::all(glm::lessThanEqual(glm::abs(voxel - expected), glm::i64vec3(1))));
			}
		}
	}

	TEST_CASE("Voxel coordinates snap onto voxel boundaries")
	{
		// Within c_GISnapVoxels (1/256 voxel) of a boundary a coordinate is the boundary exactly; further off it stays.
		const RenderData::AffineTransform rows = RenderData::AffineTransform::FromMatrix(glm::mat4(1.0f));
		const glm::vec3 anchor(-102.4f, 0.0f, 0.0f);
		const glm::vec3 coordinate = GlobalIllumination::GetVoxelCoordinate(rows, glm::vec3(0.30001f, -0.29999f, 0.301f), anchor, 0.1f);
		CHECK(coordinate.x == 1027.0f);
		CHECK(coordinate.y == -3.0f);
		CHECK(coordinate.z == doctest::Approx(3.01f).epsilon(1e-5));
		// Two faces meant to meet (a box's bottom on a floor's top), placed through different rows, meet exactly.
		const RenderData::AffineTransform floorRows = RenderData::AffineTransform::FromMatrix(
			glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.05f, 0.0f)), glm::vec3(6.4f, 0.1f, 6.4f)));
		const RenderData::AffineTransform boxRows = RenderData::AffineTransform::FromMatrix(
			glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(-10.5f, 0.35f, -3.0f)), glm::vec3(0.5f)));
		const glm::vec3 floorTop = GlobalIllumination::GetVoxelCoordinate(floorRows, glm::vec3(0.0f, 0.5f, 0.0f), anchor, 0.1f);
		const glm::vec3 boxBottom = GlobalIllumination::GetVoxelCoordinate(boxRows, glm::vec3(0.0f, -0.5f, 0.0f), anchor, 0.1f);
		CHECK(floorTop.y == 1.0f);
		CHECK(boxBottom.y == 1.0f);
	}

	TEST_CASE("Voxel colors pack as the voxelizer reads them")
	{
		// Linear albedo as RGB8, red in the low byte, clamped.
		CHECK(GlobalIllumination::PackAlbedo(glm::vec3(1.0f, 0.5f, 0.0f)) == (255u | (128u << 8)));
		CHECK(GlobalIllumination::PackAlbedo(glm::vec3(-1.0f, 2.0f, std::numeric_limits<float>::quiet_NaN())) == (255u << 8));
		// RGB9E5: nine bits of mantissa a channel under a shared exponent, the brightest channel within half a step.
		CHECK(GlobalIllumination::PackRGB9E5(glm::vec3(0.0f)) == 0u);
		for (const glm::vec3& color : { glm::vec3(100.0f, 50.0f, 1.0f), glm::vec3(0.01f, 0.002f, 0.0f), glm::vec3(3000.0f, 2250.0f, 1350.0f),
				 glm::vec3(1.0f), glm::vec3(65408.0f, 0.0f, 0.0f) })
		{
			CAPTURE(color.x);
			const glm::vec3 unpacked = GlobalIllumination::UnpackRGB9E5(GlobalIllumination::PackRGB9E5(color));
			const float largest = std::max({ color.x, color.y, color.z });
			for (int channel = 0; channel < 3; channel++)
				CHECK(std::abs(unpacked[channel] - color[channel]) <= largest / 511.0f);
		}
		// Beyond the format's largest value: clamped, never infinite.
		const glm::vec3 clamped = GlobalIllumination::UnpackRGB9E5(GlobalIllumination::PackRGB9E5(glm::vec3(1e9f, std::numeric_limits<float>::infinity(), 5.0f)));
		CHECK(clamped.x == 65408.0f);
		CHECK(clamped.y == 65408.0f);
	}

	TEST_CASE("Level weights are continuous")
	{
		for (GlobalIlluminationLevel quality : { GlobalIlluminationLevel::High, GlobalIlluminationLevel::Low })
		{
			const GlobalIlluminationLayout layout = GetLayout(quality);
			for (const VoxelLevelLayout& level : layout.Levels)
			{
				CAPTURE(level.VoxelSize);
				CHECK(GlobalIllumination::GetLevelWeight(level, glm::vec3(0.0f)) == 1.0f);
				const glm::vec3 half = glm::vec3(level.Dimensions) * (0.5f * level.VoxelSize);
				const float edge = static_cast<float>(RenderData::c_GIScrollVoxels + RenderData::c_GILevelGuardVoxels) * level.VoxelSize;
				const float band = static_cast<float>(RenderData::c_GILevelBlendVoxels) * level.VoxelSize;
				// 1 inside the band, 0 past it, on every axis.
				for (int axis = 0; axis < 3; axis++)
				{
					glm::vec3 inside(0.0f);
					inside[axis] = half[axis] - edge - band - 0.01f;
					CHECK(GlobalIllumination::GetLevelWeight(level, inside) == 1.0f);
					glm::vec3 past(0.0f);
					past[axis] = -(half[axis] - edge + 0.01f);
					CHECK(GlobalIllumination::GetLevelWeight(level, past) == 0.0f);
				}
				// A camera moving 1 mm moves every point 1 mm relative to it: no weight changes by more than the blend's slope
				// allows (1 mm over the band of c_GILevelBlendVoxels voxels).
				const float largestStep = 0.001f / band + 1e-5f;
				for (float x = -half.x; x <= half.x; x += 0.0371f)
				{
					const glm::vec3 point(x, 0.3f * x, -0.7f * x);
					const float weight = GlobalIllumination::GetLevelWeight(level, point);
					const float moved = GlobalIllumination::GetLevelWeight(level, point + glm::vec3(0.001f, 0.0f, 0.0f));
					CHECK(std::abs(weight - moved) <= largestStep);
				}
			}
		}
	}
}
