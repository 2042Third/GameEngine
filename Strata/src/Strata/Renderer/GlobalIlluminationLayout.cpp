#include "stpch.h"
#include "Strata/Renderer/GlobalIlluminationLayout.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace Strata
{

	namespace
	{

		int64_t FloorDivide(int64_t value, int64_t divisor)
		{
			const int64_t quotient = value / divisor;
			return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
		}

		glm::i64vec3 FloorDivide(const glm::i64vec3& value, int64_t divisor)
		{
			return glm::i64vec3(FloorDivide(value.x, divisor), FloorDivide(value.y, divisor), FloorDivide(value.z, divisor));
		}

		// The point on the octahedron |x| + |y| + |z| = 1 that OctahedralDecode normalizes for an encoded position in [-1, 1]^2.
		glm::vec3 GetOctahedronPoint(const glm::vec2& encoded)
		{
			glm::vec3 point(encoded, 1.0f - std::abs(encoded.x) - std::abs(encoded.y));
			const float fold = std::max(-point.z, 0.0f);
			point.x += point.x >= 0.0f ? -fold : fold;
			point.y += point.y >= 0.0f ? -fold : fold;
			return point;
		}

		glm::vec2 GetTexelCenter(uint32_t u, uint32_t v, uint32_t a)
		{
			const float size = static_cast<float>(a);
			return glm::vec2((static_cast<float>(u) + 0.5f) / size * 2.0f - 1.0f, (static_cast<float>(v) + 0.5f) / size * 2.0f - 1.0f);
		}

		// The octahedral map's Jacobian at a texel's center, up to a constant factor.
		double GetSolidAngleWeight(uint32_t u, uint32_t v, uint32_t a)
		{
			const double length = glm::length(glm::dvec3(GetOctahedronPoint(GetTexelCenter(u, v, a))));
			return 1.0 / (length * length * length);
		}

		// The pieces of [first, end) along one axis: split where the texels wrap and at most c_GIMaxRegionVoxels long.
		void SplitAxis(int64_t first, int64_t end, int64_t dimension, std::vector<std::pair<int64_t, int64_t>>& outRanges)
		{
			outRanges.clear();
			for (int64_t start = first; start < end;)
			{
				const int64_t wrap = (FloorDivide(start, dimension) + 1) * dimension;
				const int64_t stop = std::min({ end, wrap, start + static_cast<int64_t>(RenderData::c_GIMaxRegionVoxels) });
				outRanges.emplace_back(start, stop);
				start = stop;
			}
		}

	}

	uint64_t GlobalIlluminationMemory::GetTotal() const
	{
		return VoxelLevels + Scratch + Atlases + Weights + Validity + Field + FieldDepth + Tiles + WorkItems + Regions;
	}

	GlobalIlluminationMemory GlobalIlluminationLayout::GetMemory() const
	{
		// Material (RGBA8), emission (R32_UINT), normal (RGBA8_SNORM) and radiance (RGBA16F) per voxel; the scratch's eleven words
		// per voxel (key, winding, emission, three normal sums, the emission's coverage, the normals' count and their magnitudes' three sums).
		constexpr uint64_t c_VoxelBytes = 4 + 4 + 4 + 8;
		constexpr uint64_t c_ScratchBytes = RenderData::c_GIScratchWords * 4;
		constexpr uint64_t c_RayBytes = 8;
		constexpr uint64_t c_WeightBytes = 2 * 4;
		constexpr uint64_t c_ValidityBytes = 4;
		constexpr uint64_t c_FieldBytes = 6 * 8;
		constexpr uint64_t c_FieldDepthBytes = RenderData::c_GIFieldDepthTexels * RenderData::c_GIFieldDepthTexels * 4;
		constexpr uint64_t c_MaxTiles = 32768;
		constexpr uint64_t c_TileBytes = 8;
		constexpr uint64_t c_MaxWorkItems = 65536;
		constexpr uint64_t c_WorkItemBytes = 16 + 8; // A work item and a triangle group

		GlobalIlluminationMemory memory;
		for (const VoxelLevelLayout& level : Levels)
			memory.VoxelLevels += static_cast<uint64_t>(level.Dimensions.x) * level.Dimensions.y * level.Dimensions.z * c_VoxelBytes;
		memory.Scratch = static_cast<uint64_t>(BatchVoxels) * c_ScratchBytes;
		for (size_t k = 0; k < Cascades.size(); k++)
		{
			const ProbeCascadeLayout& cascade = Cascades[k];
			const uint64_t probes = static_cast<uint64_t>(cascade.Grid.x) * cascade.Grid.y * cascade.Grid.z;
			if (k > 0) // Cascade 0's rays go straight into the field
				memory.Atlases += probes * cascade.DirectionsPerAxis * cascade.DirectionsPerAxis * c_RayBytes;
			if (k + 1 < Cascades.size())
				memory.Weights += probes * c_WeightBytes;
			memory.Validity += probes * c_ValidityBytes;
			if (k == 0)
			{
				memory.Field = probes * c_FieldBytes;
				memory.FieldDepth = probes * c_FieldDepthBytes;
			}
		}
		memory.Tiles = c_MaxTiles * c_TileBytes;
		memory.WorkItems = c_MaxWorkItems * c_WorkItemBytes;
		memory.Regions = static_cast<uint64_t>(RenderData::c_GIMaxRegions) * sizeof(RenderData::VoxelRegion) + RenderData::c_GICounterWords * sizeof(uint32_t);
		return memory;
	}

	uint64_t GlobalIlluminationLayout::GetRaysPerFrame() const
	{
		uint64_t rays = 0;
		for (const ProbeCascadeLayout& cascade : Cascades)
			rays += static_cast<uint64_t>(cascade.Grid.x) * cascade.Grid.y * cascade.Grid.z * cascade.DirectionsPerAxis * cascade.DirectionsPerAxis
				* cascade.SubRays * cascade.SubRays;
		return rays;
	}

	uint64_t VoxelBox::GetVoxelCount() const
	{
		if (IsEmpty())
			return 0;
		const glm::i64vec3 size = Maximum - Minimum;
		return static_cast<uint64_t>(size.x) * static_cast<uint64_t>(size.y) * static_cast<uint64_t>(size.z);
	}

	bool VoxelBox::Contains(const VoxelBox& other) const
	{
		return other.IsEmpty() || (glm::all(glm::lessThanEqual(Minimum, other.Minimum)) && glm::all(glm::greaterThanEqual(Maximum, other.Maximum)));
	}

	bool VoxelBox::Intersects(const VoxelBox& other) const
	{
		return !Intersection(other).IsEmpty();
	}

	VoxelBox VoxelBox::Intersection(const VoxelBox& other) const
	{
		return VoxelBox { glm::max(Minimum, other.Minimum), glm::min(Maximum, other.Maximum) };
	}

	namespace GlobalIllumination
	{

		std::optional<GlobalIlluminationLayout> ComputeLayout(GlobalIlluminationLevel level)
		{
			if (level == GlobalIlluminationLevel::Off)
				return std::nullopt;
			const bool high = level == GlobalIlluminationLevel::High;
			const uint32_t levelCount = high ? 4 : 3;
			const float firstVoxelSize = high ? 0.10f : 0.20f;
			const glm::uvec3 dimensions = high ? glm::uvec3(128, 64, 128) : glm::uvec3(64, 32, 64);
			const uint32_t cascadeCount = high ? 4 : 3;
			const float firstSpacing = high ? 0.5f : 1.0f;
			const glm::uvec3 firstGrid = high ? glm::uvec3(48, 40, 48) : glm::uvec3(24, 20, 24);

			GlobalIlluminationLayout layout;
			for (uint32_t index = 0; index < levelCount; index++)
				layout.Levels.push_back(VoxelLevelLayout { firstVoxelSize * static_cast<float>(1u << index), dimensions, index == 0 && high ? RenderData::c_GIFinestLevelDrop : 0.0f });
			for (uint32_t k = 0; k < cascadeCount; k++)
			{
				ProbeCascadeLayout cascade;
				cascade.Spacing = firstSpacing * static_cast<float>(1u << k);
				cascade.Grid = firstGrid / (1u << k);
				// Directions four times as many per cascade from cascade 1's 64; cascade 0 marches cascade 1's directions (its rays go
				// straight into the field, each continued by cascade 1's merged ray of the same direction: FieldReduce.comp).
				cascade.DirectionsPerAxis = 4u << std::max(k, 1u);
				// Intervals four times as long per cascade: [0, s0], [s0, 4 s0], [4 s0, 16 s0]..., the top one without an end.
				cascade.IntervalStart = k == 0 ? 0.0f : firstSpacing * static_cast<float>(1u << (2 * (k - 1)));
				cascade.IntervalEnd = k + 1 == cascadeCount ? std::numeric_limits<float>::infinity() : firstSpacing * static_cast<float>(1u << (2 * k));
				cascade.LeadIn = cascade.IntervalStart;
				cascade.Overlap = k + 1 == cascadeCount ? 0.0f : cascade.Spacing;
				cascade.MinimumLevel = k == 0 ? 0 : std::min(k - 1, 1u);
				cascade.SubRays = k > 0 && k + 1 < cascadeCount ? 2u : 1u;
				layout.Cascades.push_back(cascade);
			}
			layout.BatchVoxels = std::min(RenderData::c_GIMaxRegionVoxels * RenderData::c_GIMaxRegionVoxels * RenderData::c_GIMaxRegionVoxels,
				dimensions.x * dimensions.y * dimensions.z);
			layout.FieldExtent = glm::vec3(firstGrid) * firstSpacing;
			layout.FieldSnap = layout.Cascades.back().Spacing;
			// The field reaches only where voxel level 2, the coarsest the gather reads, holds every point while a scroll is
			// voxelized: its valid box a slab short on the side the camera moves toward, the camera up to a step on, less
			// c_GILevelGuardVoxels; a point no level holds takes today's light at once (Include/GIField.slang), a seam that would
			// jump. Low's level 2 (32 voxels of 0.8 m up) holds (16 - 2 * 4 - 2) * 0.8 = 4.8 m up and down: 2.8 m at full weight
			// and the fade. High's holds 8.8 m.
			layout.FieldFullExtent = high ? glm::vec2(7.5f, 5.5f) : glm::vec2(7.0f, 2.8f);
			layout.FieldFade = 2.0f;
			layout.Bounces = high;
			return layout;
		}

		glm::vec3 GetDirection(uint32_t u, uint32_t v, uint32_t a)
		{
			return glm::normalize(GetOctahedronPoint(GetTexelCenter(u, v, a)));
		}

		float GetSolidAngle(uint32_t u, uint32_t v, uint32_t a)
		{
			double total = 0.0;
			for (uint32_t row = 0; row < a; row++)
			{
				for (uint32_t column = 0; column < a; column++)
					total += GetSolidAngleWeight(column, row, a);
			}
			return static_cast<float>(4.0 * glm::pi<double>() * GetSolidAngleWeight(u, v, a) / total);
		}

		float GetSolidAngleNormalization(uint32_t a)
		{
			double total = 0.0;
			for (uint32_t row = 0; row < a; row++)
			{
				for (uint32_t column = 0; column < a; column++)
					total += GetSolidAngleWeight(column, row, a);
			}
			return total > 0.0 ? static_cast<float>(4.0 * glm::pi<double>() / total) : 0.0f;
		}

		glm::vec3 GetFaceAxis(uint32_t face)
		{
			glm::vec3 axis(0.0f);
			axis[static_cast<int>(std::min(face, 5u) / 2)] = (face % 2) == 0 ? 1.0f : -1.0f;
			return axis;
		}

		float GetFaceCosineSum(uint32_t face, uint32_t a)
		{
			const glm::vec3 axis = GetFaceAxis(face);
			double sum = 0.0;
			for (uint32_t v = 0; v < a; v++)
			{
				for (uint32_t u = 0; u < a; u++)
					sum += static_cast<double>(std::max(glm::dot(GetDirection(u, v, a), axis), 0.0f)) * GetSolidAngle(u, v, a);
			}
			return static_cast<float>(sum);
		}

		glm::dvec3 GetFieldMinimum(const GlobalIlluminationLayout& layout, const glm::dvec3& cameraPosition)
		{
			const double snap = static_cast<double>(layout.FieldSnap);
			if (!(snap > 0.0))
				return cameraPosition;
			return snap * glm::floor((cameraPosition - glm::dvec3(layout.FieldExtent) * 0.5) / snap + 0.5);
		}

		glm::i64vec3 GetVoxelIndex(const glm::dvec3& position, float voxelSize)
		{
			const glm::dvec3 scaled = glm::floor(position / static_cast<double>(voxelSize));
			return glm::i64vec3(static_cast<int64_t>(scaled.x), static_cast<int64_t>(scaled.y), static_cast<int64_t>(scaled.z));
		}

		glm::i64vec3 GetWindowMinimum(const VoxelLevelLayout& level, const glm::dvec3& cameraPosition)
		{
			const glm::i64vec3 center = GetVoxelIndex(cameraPosition - glm::dvec3(0.0, static_cast<double>(level.Drop), 0.0), level.VoxelSize);
			const int64_t step = RenderData::c_GIScrollVoxels;
			return FloorDivide(center - glm::i64vec3(level.Dimensions / 2u), step) * step;
		}

		VoxelBox GetWindow(const VoxelLevelLayout& level, const glm::i64vec3& minimum)
		{
			return VoxelBox { minimum, minimum + glm::i64vec3(level.Dimensions) };
		}

		glm::uvec3 GetTexel(const glm::i64vec3& voxel, const glm::uvec3& dimensions)
		{
			glm::uvec3 texel;
			for (int axis = 0; axis < 3; axis++)
			{
				const int64_t dimension = dimensions[axis];
				texel[axis] = static_cast<uint32_t>(voxel[axis] - FloorDivide(voxel[axis], dimension) * dimension);
			}
			return texel;
		}

		void SplitRegion(const VoxelBox& box, const glm::uvec3& dimensions, std::vector<VoxelBox>& outPieces)
		{
			if (box.IsEmpty())
				return;
			std::vector<std::pair<int64_t, int64_t>> ranges[3];
			for (int axis = 0; axis < 3; axis++)
				SplitAxis(box.Minimum[axis], box.Maximum[axis], dimensions[axis], ranges[axis]);
			for (const auto& [z0, z1] : ranges[2])
			{
				for (const auto& [y0, y1] : ranges[1])
				{
					for (const auto& [x0, x1] : ranges[0])
						outPieces.push_back(VoxelBox { glm::i64vec3(x0, y0, z0), glm::i64vec3(x1, y1, z1) });
				}
			}
		}

		bool GetScrollSlabs(const glm::i64vec3& oldMinimum, const glm::i64vec3& newMinimum, const glm::uvec3& dimensions, std::vector<VoxelBox>& outSlabs)
		{
			const glm::i64vec3 size(dimensions);
			for (int axis = 0; axis < 3; axis++)
			{
				const int64_t move = newMinimum[axis] - oldMinimum[axis];
				if (move >= size[axis] || -move >= size[axis])
					return false;
			}
			const VoxelBox before { oldMinimum, oldMinimum + size };
			const VoxelBox after { newMinimum, newMinimum + size };
			// Axis by axis: the new window's part beyond the old one on this axis, within the overlap on the axes before it and
			// the new window's whole range on the axes after it. The three are disjoint and make up the new window minus the old.
			for (int axis = 0; axis < 3; axis++)
			{
				if (newMinimum[axis] == oldMinimum[axis])
					continue;
				VoxelBox slab = after;
				for (int earlier = 0; earlier < axis; earlier++)
				{
					slab.Minimum[earlier] = std::max(before.Minimum[earlier], after.Minimum[earlier]);
					slab.Maximum[earlier] = std::min(before.Maximum[earlier], after.Maximum[earlier]);
				}
				if (newMinimum[axis] > oldMinimum[axis])
					slab.Minimum[axis] = before.Maximum[axis];
				else
					slab.Maximum[axis] = before.Minimum[axis];
				SplitRegion(slab, dimensions, outSlabs);
			}
			return true;
		}

		VoxelBox SnapToBricks(const VoxelBox& box)
		{
			const int64_t brick = RenderData::c_GIRegionBrick;
			VoxelBox snapped;
			snapped.Minimum = FloorDivide(box.Minimum, brick) * brick;
			snapped.Maximum = -FloorDivide(-box.Maximum, brick) * brick;
			return snapped;
		}

		glm::i64vec3 GetAnchorVoxel(const glm::i64vec3& windowMinimum)
		{
			return FloorDivide(windowMinimum, RenderData::c_GIAnchorVoxels) * RenderData::c_GIAnchorVoxels;
		}

		glm::vec3 GetVoxelCoordinate(const RenderData::AffineTransform& world, const glm::vec3& position, const glm::vec3& anchor, float voxelSize)
		{
			// The operations of Include/Voxelize.slang's GetVoxelCoordinate, in its order (precise there: no contraction).
			glm::vec3 coordinate;
			for (int row = 0; row < 3; row++)
			{
				const glm::vec4& rows = world.Rows[row];
				const float translation = rows.w - anchor[row];
				const float rotated = (rows.x * position.x + rows.y * position.y) + rows.z * position.z;
				const float value = (rotated + translation) / voxelSize;
				const float boundary = std::round(value);
				coordinate[row] = std::abs(value - boundary) <= RenderData::c_GISnapVoxels ? boundary : value;
			}
			return coordinate;
		}

		float GetLevelWeight(const VoxelLevelLayout& level, const glm::vec3& relative)
		{
			const float voxel = level.VoxelSize;
			const glm::vec3 margin = glm::vec3(level.Dimensions) * (0.5f * voxel)
				- static_cast<float>(RenderData::c_GIScrollVoxels + RenderData::c_GILevelGuardVoxels) * voxel - glm::abs(relative + glm::vec3(0.0f, level.Drop, 0.0f));
			const float nearest = std::min({ margin.x, margin.y, margin.z });
			return std::clamp(nearest / (static_cast<float>(RenderData::c_GILevelBlendVoxels) * voxel), 0.0f, 1.0f);
		}

		uint32_t PackAlbedo(const glm::vec3& albedo)
		{
			uint32_t packed = 0;
			for (int channel = 0; channel < 3; channel++)
			{
				const float value = std::isfinite(albedo[channel]) ? std::clamp(albedo[channel], 0.0f, 1.0f) : 0.0f;
				packed |= static_cast<uint32_t>(std::lround(value * 255.0f)) << (8 * channel);
			}
			return packed;
		}

		uint32_t PackRGB9E5(const glm::vec3& color)
		{
			constexpr float c_Largest = 65408.0f;
			glm::vec3 clamped;
			for (int channel = 0; channel < 3; channel++)
				clamped[channel] = std::isfinite(color[channel]) ? std::clamp(color[channel], 0.0f, c_Largest) : (color[channel] > 0.0f ? c_Largest : 0.0f);
			const float largest = std::max({ clamped.x, clamped.y, clamped.z });
			if (largest < 1.52587891e-5f)
				return 0;
			// floor(log2(largest)) from the float's exponent bits, as the shader takes it, plus 1 and the bias 15.
			int exponent = std::max(-16, static_cast<int>((std::bit_cast<uint32_t>(largest) >> 23) & 255u) - 127) + 16;
			float denominator = std::exp2(static_cast<float>(exponent - 24));
			if (std::floor(largest / denominator + 0.5f) >= 512.0f)
			{
				exponent++;
				denominator *= 2.0f;
			}
			uint32_t packed = static_cast<uint32_t>(exponent) << 27;
			for (int channel = 0; channel < 3; channel++)
				packed |= static_cast<uint32_t>(std::min(std::floor(clamped[channel] / denominator + 0.5f), 511.0f)) << (9 * channel);
			return packed;
		}

		glm::vec3 UnpackRGB9E5(uint32_t value)
		{
			const float scale = std::exp2(static_cast<float>(static_cast<int>(value >> 27)) - 24.0f);
			return glm::vec3(static_cast<float>(value & 511u), static_cast<float>((value >> 9) & 511u), static_cast<float>((value >> 18) & 511u)) * scale;
		}

		uint32_t GetSettleFrames(float ratio)
		{
			if (!std::isfinite(ratio) || ratio <= 0.0f)
				return 0;
			const double kept = std::min(static_cast<double>(ratio), static_cast<double>(RenderData::c_GIMaxBounceAlbedo));
			const double frames = std::ceil(std::log(static_cast<double>(RenderData::c_GISettleChange) / (1.0 - kept)) / std::log(kept));
			return static_cast<uint32_t>(std::max(frames, 1.0)) + RenderData::c_GISettleMarginFrames;
		}

	}

}
