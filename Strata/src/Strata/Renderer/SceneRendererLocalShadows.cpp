#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Core/Assert.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/BindlessTextureTable.h"
#include "Strata/Renderer/LODSelection.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/RenderLights.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Renderer/RenderSceneGPU.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/SceneDrawKey.h"
#include "Strata/Renderer/TextureTable.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

// Shadows of point and spot lights (RendererQualitySettings::LocalShadowAtlasSize). The shadowed lights in view that matter
// most (GetLocalShadowImportance, with a margin for those holding a shadow) get tiles of the renderer's depth atlas, as many
// as MaxShadowedLocalLights allows and the atlas holds: a spot light one perspective view, a point light six (a cube's faces),
// each view widened by a guard band of what its lookups reach (the normal bias and the filter) so that they never leave the
// face they sample. Tiles follow the light's size on screen, stepped down for the less important lights while they do not
// fit seven eighths of the atlas; a light whose tiles change size keeps sampling its old tiles until the new ones are drawn,
// and at most two lights change at a time. A view is drawn again only when what it shows changed (its key, a caster reaching
// it, what casters are drawn with), at most LocalShadowViewsPerFrame of them per frame where SpreadLightingUpdates spreads
// them (views without content first, a quarter to the views that waited longest, the rest by waiting times importance); a
// light casts its shadow once all its views hold content, from the matrices they were drawn with, and fades it in, or out
// once it lost its shadow while in view. Which views are drawn is the CPU's decision on both paths; what each draws is decided
// by the CPU path here and by the GPU path's culling.

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_MinLocalShadowTile = 128;
		// The smallest atlas an allocation that failed falls back to (smaller ones fail as the atlas asked for).
		constexpr uint32_t c_MinFallbackLocalShadowAtlasSize = 2048;
		// A light's tile size follows the pixels its range spans on screen, halved (the receivers its views must resolve lie
		// within the range, on average halfway), as a power of two; it changes only once that leaves this band around the
		// size it has, so sizes do not toggle while the camera moves.
		constexpr float c_TileGrowRatio = 1.25f;
		constexpr float c_TileShrinkRatio = 0.4f;
		// A light holding a shadow ranks as if it mattered this much more, and in the order sizes are fitted in a light passes one
		// it matters this much more than: lights near the cut or near each other do not trade places every frame.
		constexpr float c_LocalShadowKeepRatio = 1.25f;
		// The projected size of a light's range, r / (d - r), saturates at 1 / this where the camera nears the range.
		constexpr float c_NearCoverageDistance = 0.1f;
		// Tiles are fitted into this share of the atlas (in eighths): the rest is room for lights whose tiles change size, which
		// keep their old tiles until the new ones are drawn.
		constexpr uint64_t c_LocalShadowAtlasEighths = 7;
		// At most this many lights sample old tiles while their new ones are drawn (SpreadLightingUpdates; without it every light
		// takes its size at once).
		constexpr uint32_t c_MaxLocalShadowTransitions = 2;
		// The change tracking's grid: a light or a proxy's bounds spanning more cells than this is tested without it.
		constexpr uint64_t c_LocalShadowGridMaxCells = 64;
		// A change reaches a view when its box, grown by this fraction of the view's range, reaches the view's range and
		// frustum: the margin for the rounding of what the CPU or the GPU took as the view's casters.
		constexpr float c_LocalShadowChangeMargin = 0.01f;
		constexpr int32_t c_LocalShadowGridBias = 1 << 20;
		constexpr uint32_t c_NoLocalShadowLight = ~0u;
		// A spot light's view covers at most this half angle (radians, 80 degrees): one perspective tile cannot show a
		// hemisphere. Wider cones are shadowed within it.
		constexpr float c_MaxSpotShadowHalfAngle = 1.3962634f;
		// Views clip at a near plane of this fraction of the light's range, within these bounds (meters).
		constexpr float c_LocalShadowNearScale = 0.001f;
		constexpr float c_MinLocalShadowNear = 0.005f;
		constexpr float c_MaxLocalShadowNear = 0.1f;
		// How far the normal bias can move a lookup across a view, in texels per texel of bias: the offset along the normal has a
		// part across the view and a part along its axis, which at the view's edge also moves the projection outward; their sum
		// is at most sqrt(2) times the offset (a normal at 45 degrees between them).
		constexpr float c_NormalBiasReach = 1.4142136f;
		// Casters spanning less than this many texels of a view are left out of it, as for the cascades.
		constexpr float c_MinCasterTexels = 1.0f;
		// The CPU's query for a light's casters reaches this much beyond its range, so that it finds every proxy the exact test
		// (in camera-relative coordinates, on either path) can take.
		constexpr float c_CasterQueryScale = 1.001f;
		constexpr float c_CasterQueryMargin = 0.01f;
		// The bounds of at most this many proxies within shadowed lights' ranges are followed per light the quality shadows
		// (Quality.MaxShadowedLocalLights, counted as at least c_MinFollowedLocalShadowLights: 65,536 proxies, 0.7.0's for its
		// 16 lights); beyond, the views are drawn again and the following starts over (m_LocalShadowEpoch). The map never holds
		// more than the render scene's proxies.
		constexpr size_t c_LocalShadowBoundsPerLight = 4096;
		constexpr size_t c_MinFollowedLocalShadowLights = 16;

		// A point light's views: the cube's faces in the order the shaders pick them by the major axis.
		constexpr std::array<glm::vec3, RenderData::c_LocalShadowFaces> c_FaceDirections = {
			glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f),
			glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f) };

		glm::vec3 GetViewUp(const glm::vec3& direction)
		{
			return std::abs(direction.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
		}

		uint32_t CeilPowerOfTwo(float value)
		{
			uint32_t result = 1;
			while (static_cast<float>(result) < value && result < (1u << 30))
				result <<= 1;
			return result;
		}

		// The change tracking grid's cell of a coordinate, within the 21 bits per axis of a cell key.
		int32_t GetGridCell(float coordinate, float cellSize)
		{
			const float cell = std::floor(coordinate / cellSize);
			if (std::isnan(cell))
				return 0;
			return static_cast<int32_t>(std::clamp(cell, -static_cast<float>(c_LocalShadowGridBias), static_cast<float>(c_LocalShadowGridBias - 1)));
		}

		uint64_t GetGridKey(int32_t x, int32_t y, int32_t z)
		{
			return (static_cast<uint64_t>(x + c_LocalShadowGridBias) << 42) | (static_cast<uint64_t>(y + c_LocalShadowGridBias) << 21)
				| static_cast<uint64_t>(z + c_LocalShadowGridBias);
		}

		// The cells a box spans, each way.
		struct GridRange
		{
			glm::ivec3 Min = glm::ivec3(0);
			glm::ivec3 Max = glm::ivec3(-1);

			uint64_t GetCellCount() const
			{
				const glm::i64vec3 extent = glm::i64vec3(Max) - glm::i64vec3(Min) + glm::i64vec3(1);
				if (extent.x <= 0 || extent.y <= 0 || extent.z <= 0)
					return 0;
				// Unsigned: an extent spans at most 2^21 cells (GetGridCell), and three of them multiply to at most 2^63, one past
				// what a signed product holds.
				return static_cast<uint64_t>(extent.x) * static_cast<uint64_t>(extent.y) * static_cast<uint64_t>(extent.z);
			}
		};

		GridRange GetGridRange(const glm::vec3& boxMin, const glm::vec3& boxMax, float cellSize)
		{
			GridRange range;
			range.Min = glm::ivec3(GetGridCell(boxMin.x, cellSize), GetGridCell(boxMin.y, cellSize), GetGridCell(boxMin.z, cellSize));
			range.Max = glm::ivec3(GetGridCell(boxMax.x, cellSize), GetGridCell(boxMax.y, cellSize), GetGridCell(boxMax.z, cellSize));
			return range;
		}

		// Whether a box reaches into a sphere (the closest point of the box to the center is within the radius).
		bool BoxReachesSphere(const glm::vec3& boxMin, const glm::vec3& boxMax, const glm::vec3& center, float radius)
		{
			const glm::vec3 offset = glm::clamp(center, boxMin, boxMax) - center;
			return glm::dot(offset, offset) <= radius * radius;
		}

		// AABB::Intersects (boxes that touch count) with the six sides compared at once. The change tracking asks it of every
		// light around every change, where the side that fails first is as good as random: a branch per side is mispredicted
		// about once a light, which cost more than the comparisons.
		bool BoxesTouch(const AABB& first, const AABB& second)
		{
			const bool below = (first.Min.x <= second.Max.x) & (first.Min.y <= second.Max.y) & (first.Min.z <= second.Max.z);
			const bool above = (first.Max.x >= second.Min.x) & (first.Max.y >= second.Min.y) & (first.Max.z >= second.Min.z);
			return below & above;
		}

		// Grows a light's box in the change tracking by a sphere it lights or lit a drawn view with: the range with the tests'
		// margin, as much again and a millionth of the coordinates, so that whatever the rounding of a view's own test lets
		// pass lies in the box (a box that misses it is skipped without looking at the views).
		void ExpandByReach(AABB& box, const glm::vec3& center, float range)
		{
			const float largest = std::max({ std::abs(center.x), std::abs(center.y), std::abs(center.z) });
			const float reach = range * (1.0f + 2.0f * c_LocalShadowChangeMargin) + (largest + range) * 1.0e-6f;
			box.Expand(center - reach);
			box.Expand(center + reach);
		}

		// A box no change that reaches a drawn view misses (the change tracking's test of a view: the change's bounds, grown by
		// the margin, reach the range and lie in the frustum's planes): the range's box, cut by the planes that face along an
		// axis. Such a plane's test reads one coordinate of the bounds, as the box does (a cube face's near plane takes the
		// half behind the face away); the other planes cut nothing. Like ExpandByReach, every side lies the margin again and
		// a millionth of the coordinates further out than the test's own, so the test's rounding stays inside.
		AABB GetLocalShadowChangeBox(const glm::vec3& position, float range, const std::array<glm::vec4, 6>& planes)
		{
			AABB box;
			ExpandByReach(box, position, range);
			const float largest = std::max({ std::abs(position.x), std::abs(position.y), std::abs(position.z) });
			const float slack = range * 2.0f * c_LocalShadowChangeMargin + (largest + range) * 1.0e-6f;
			for (const glm::vec4& plane : planes)
			{
				for (int32_t axis = 0; axis < 3; axis++)
				{
					if (plane[axis] == 0.0f || plane[(axis + 1) % 3] != 0.0f || plane[(axis + 2) % 3] != 0.0f)
						continue;
					// The plane keeps the side its normal points to: coordinate * normal + w >= 0.
					const float bound = -plane.w / plane[axis];
					if (plane[axis] > 0.0f)
						box.Min[axis] = std::max(box.Min[axis], bound - slack);
					else
						box.Max[axis] = std::min(box.Max[axis], bound + slack);
				}
			}
			return box;
		}

		// Frustum::IsAABBVisible's test, also the GPU culling's (Include/Culling.slang, IsBoxInFrustum).
		bool BoxInPlanes(const glm::vec3& boxMin, const glm::vec3& boxMax, const std::array<glm::vec4, 6>& planes)
		{
			for (const glm::vec4& plane : planes)
			{
				const glm::vec3 positive(plane.x >= 0.0f ? boxMax.x : boxMin.x, plane.y >= 0.0f ? boxMax.y : boxMin.y, plane.z >= 0.0f ? boxMax.z : boxMin.z);
				if (glm::dot(glm::vec3(plane), positive) + plane.w < 0.0f)
					return false;
			}
			return true;
		}

		// How a frame uses a mesh or material slot (as SceneRendererExtraction.cpp marks casters).
		constexpr uint32_t c_UseShadow = 1u << 3;
		constexpr uint32_t c_SubmeshMaterialsShift = 4;
		constexpr uint32_t c_MaterialAvailable = 1u << 0;
		constexpr uint32_t c_MaterialDoubleSided = 1u << 1;
		constexpr uint32_t c_MaterialAlphaMask = 1u << 2;
		constexpr uint32_t c_MaterialTransparent = 1u << 3;
		static_assert(c_MaterialAvailable == RenderData::c_CullMaterialAvailable && c_MaterialDoubleSided == RenderData::c_CullMaterialDoubleSided
			&& c_MaterialAlphaMask == RenderData::c_CullMaterialAlphaMask && c_MaterialTransparent == RenderData::c_CullMaterialTransparent);

		// What one light's job finds: the proxies within its range (their bounds are followed), and per drawn view of it the
		// casters, with the marks and animators they add.
		struct LocalShadowJob
		{
			std::vector<uint32_t> Candidates;
			std::vector<std::vector<std::pair<uint32_t, float>>> ViewCasters; // (proxy, screen fraction in the view)
			std::vector<std::pair<uint32_t, uint32_t>> MeshMarks;
			std::vector<std::pair<uint32_t, uint32_t>> MaterialMarks;
			std::vector<entt::entity> Animators;
			uint32_t SubTexelCasters = 0;
		};

	}

	glm::mat4 SceneRenderer::GetLocalShadowViewProjection(const LocalShadowViewKey& key, const glm::dvec3& origin)
	{
		// The light's position relative to the origin in double: views near a camera far from the world's origin keep their
		// precision.
		const glm::vec3 position(glm::dvec3(key.Position) - origin);
		const glm::mat4 view = glm::lookAt(position, position + key.Direction, GetViewUp(key.Direction));
		const glm::mat4 projection = Math::PerspectiveReverseZ(2.0f * std::atan(key.TanHalfAngle), 1.0f, key.Near, key.Range);
		return projection * view;
	}

	void SceneRenderer::FreeLocalShadowTiles(LocalShadowLight& light)
	{
		for (uint32_t view = 0; view < light.ViewCount; view++)
		{
			LocalShadowView& state = light.Views[view];
			// A view whose light's tiles changed size may still hold its content in its old tile.
			if (state.HasContent && state.DrawnKey.Tile != light.Tiles[view])
				m_LocalShadowAllocator.Free(state.DrawnKey.Tile);
			m_LocalShadowAllocator.Free(light.Tiles[view]);
			light.Tiles[view] = {};
			state.HasContent = false;
			state.Drawing = false;
		}
		light.TileSize = 0;
	}

	bool SceneRenderer::ResizeLocalShadowTiles(LocalShadowLight& light, uint32_t size)
	{
		std::array<ShadowAtlasTile, RenderData::c_LocalShadowFaces> fresh = {};
		for (uint32_t view = 0; view < light.ViewCount; view++)
		{
			fresh[view] = m_LocalShadowAllocator.Allocate(size);
			if (fresh[view].IsValid())
				continue;
			for (uint32_t taken = 0; taken < view; taken++)
				m_LocalShadowAllocator.Free(fresh[taken]);
			return false;
		}
		for (uint32_t view = 0; view < light.ViewCount; view++)
		{
			// The tile holding the view's content stays until the view is drawn into the new one (CommitLocalShadows frees it); a
			// tile that holds nothing yet goes back now.
			const LocalShadowView& state = light.Views[view];
			if (!(state.HasContent && state.DrawnKey.Tile == light.Tiles[view]))
				m_LocalShadowAllocator.Free(light.Tiles[view]);
			light.Tiles[view] = fresh[view];
		}
		light.TileSize = size;
		return true;
	}

	float SceneRenderer::GetLocalShadowImportance(const SceneCamera& camera, const RenderData::LightRecord& record)
	{
		const glm::vec4 bounds = record.Bounds;
		const float radius = std::max(bounds.w, 1e-4f);
		float coverage = radius;
		if (!camera.Orthographic)
		{
			// The projected size of the range's sphere, r / (d - r), saturating where the camera nears it; within it, up to twice
			// that at its center (the lights around the camera first, the nearer the more).
			const float distance = glm::length(glm::vec3(bounds) - camera.Position) / radius;
			coverage = distance >= 1.0f ? 1.0f / std::max(distance - 1.0f, c_NearCoverageDistance) : (2.0f - distance) / c_NearCoverageDistance;
		}
		// A dim light's shadow shows less: the square root of its luminous intensity (color times intensity, as the record holds it).
		const float luminance = glm::dot(glm::vec3(record.Color), glm::vec3(0.2126f, 0.7152f, 0.0722f));
		return coverage * std::sqrt(std::max(luminance, 1e-6f));
	}

	size_t SceneRenderer::GetMaxLocalShadowBounds() const
	{
		return c_LocalShadowBoundsPerLight * std::max<size_t>(m_Specification.Quality.MaxShadowedLocalLights, c_MinFollowedLocalShadowLights);
	}

	void SceneRenderer::ResetLocalShadows()
	{
		m_LocalShadowLights.clear();
		m_LocalShadowViews.clear();
		m_LocalShadowCasters.clear();
		m_LocalShadowBatches.clear();
		m_LocalShadowAllocator.Clear();
		m_LocalShadowBounds.clear();
		m_LocalShadowContent = {};
		m_LocalShadowEpoch++;
	}

	void SceneRenderer::ReleaseLocalShadowAtlas(uint32_t atlasSize)
	{
		std::vector<nvrhi::ResourceHandle> released;
		if (m_LocalShadowAtlas)
			released.emplace_back(m_LocalShadowAtlas.Get());
		if (m_LocalShadowFramebuffer)
			released.emplace_back(m_LocalShadowFramebuffer.Get());
		m_LocalShadowAtlas = nullptr;
		m_LocalShadowFramebuffer = nullptr;
		Renderer::ReleaseDeferred(std::move(released));
		// The scene's binding sets, which read the atlas, are made again by the next frame's comparison.
		m_BoundLocalShadowAtlas = nullptr;
		m_LocalShadowAllocator.Reset(atlasSize, c_MinLocalShadowTile);
		ResetLocalShadows();
	}

	float SceneRenderer::GetLocalShadowFadeStep(float seconds)
	{
		if (!(seconds > 0.0f))
			return 0.0f;
		return std::min(seconds / c_LocalShadowFadeSeconds, 1.0f / static_cast<float>(c_LocalShadowFadeFrames));
	}

	uint32_t SceneRenderer::RankLocalShadowLights(const RenderLights& lights, const SceneCamera& camera, const Frustum& frustum, bool spread, float fadeStep)
	{
		// The lights holding a shadow by (entity, type), for a lookup per shadowed light in view.
		const auto keyOf = [](entt::entity entity, uint32_t type) { return (static_cast<uint64_t>(entt::to_integral(entity)) << 8) | (type & 0xFFu); };
		m_LocalShadowLightIndex.clear();
		for (uint32_t index = 0; index < m_LocalShadowLights.size(); index++)
			m_LocalShadowLightIndex.emplace_back(keyOf(m_LocalShadowLights[index].Entity, m_LocalShadowLights[index].Type), index);
		std::sort(m_LocalShadowLightIndex.begin(), m_LocalShadowLightIndex.end());
		const auto findHeld = [&](entt::entity entity, uint32_t type)
		{
			const uint64_t key = keyOf(entity, type);
			const auto it = std::lower_bound(m_LocalShadowLightIndex.begin(), m_LocalShadowLightIndex.end(), std::pair<uint64_t, uint32_t>(key, 0u));
			return it != m_LocalShadowLightIndex.end() && it->first == key ? it->second : c_NoLocalShadowLight;
		};

		// The shadowed lights in view by importance, those holding a shadow ranked as if they mattered c_LocalShadowKeepRatio more
		// (registry order between equals).
		struct Candidate
		{
			uint32_t Record = 0;
			float Importance = 0.0f;
			float Rank = 0.0f;
			uint32_t Held = c_NoLocalShadowLight;
		};
		std::vector<Candidate> candidates;
		const std::span<const uint32_t> shadowed = lights.GetShadowedRecords();
		candidates.reserve(shadowed.size());
		const std::span<const RenderData::LightRecord> records = lights.GetRecords();
		for (const uint32_t record : shadowed)
		{
			const glm::vec4 bounds = records[record].Bounds;
			if (!frustum.IsSphereVisible(glm::vec3(bounds), bounds.w))
				continue;
			const float importance = GetLocalShadowImportance(camera, records[record]);
			const uint32_t held = findHeld(lights.GetEntity(record), lights.GetType(record));
			candidates.push_back(Candidate { record, importance, held != c_NoLocalShadowLight ? importance * c_LocalShadowKeepRatio : importance, held });
		}
		std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
		{
			return a.Rank != b.Rank ? a.Rank > b.Rank : a.Record < b.Record;
		});

		// As many as the quality allows and the atlas holds at its smallest tiles in the share tiles are fitted into, counted
		// before the ranks are cut: lights the atlas cannot hold lose their shadows as those beyond the quality's count do, fading.
		const uint32_t atlasSize = m_LocalShadowAllocator.GetAtlasSize();
		const uint64_t tilesEachWay = atlasSize / std::max(std::min(c_MinLocalShadowTile, atlasSize), 1u);
		const uint64_t capacity = tilesEachWay * tilesEachWay * c_LocalShadowAtlasEighths / 8;
		size_t kept = 0;
		uint64_t views = 0;
		while (kept < candidates.size() && kept < m_Specification.Quality.MaxShadowedLocalLights)
		{
			const uint64_t viewCount = lights.GetType(candidates[kept].Record) == RenderData::c_LightPoint ? RenderData::c_LocalShadowFaces : 1u;
			if (views + viewCount > capacity)
				break;
			views += viewCount;
			kept++;
		}

		// What becomes of each light holding a shadow: ranked again, in view without a rank (it fades out), or gone.
		enum class Fate : uint8_t
		{
			Gone = 0,
			Unranked,
			Ranked
		};
		std::vector<Fate> fates(m_LocalShadowLights.size(), Fate::Gone);
		for (size_t rank = 0; rank < candidates.size(); rank++)
		{
			const Candidate& candidate = candidates[rank];
			if (candidate.Held == c_NoLocalShadowLight)
				continue;
			LocalShadowLight& light = m_LocalShadowLights[candidate.Held];
			light.Record = candidate.Record;
			light.Importance = candidate.Importance;
			fates[candidate.Held] = rank < kept ? Fate::Ranked : Fate::Unranked;
		}
		// The ranked lights: those that held a shadow in their previous order (the order their sizes are fitted in), then the new
		// ones. A light then moves up past those it matters c_LocalShadowKeepRatio more than, so the order follows importance
		// without trading places between near equals. The order is found on the lights' importances alone and each light moved
		// once into its place: where the order turns around (a camera cut) the passes make about n * n / 2 swaps, which of
		// whole lights (about 1.5 KB each) would move about 150 MB in a frame at Ultra's 256.
		struct Place
		{
			float Importance = 0.0f;
			uint32_t Held = c_NoLocalShadowLight; // Its index in m_LocalShadowLights, or a new light's
			uint32_t Candidate = 0;               // The new light's in `candidates`
		};
		std::vector<Place> places;
		places.reserve(m_LocalShadowLights.size() + kept);
		for (uint32_t index = 0; index < m_LocalShadowLights.size(); index++)
		{
			if (fates[index] == Fate::Ranked)
				places.push_back(Place { m_LocalShadowLights[index].Importance, index, 0 });
		}
		for (size_t rank = 0; rank < kept; rank++)
		{
			if (candidates[rank].Held == c_NoLocalShadowLight)
				places.push_back(Place { candidates[rank].Importance, c_NoLocalShadowLight, static_cast<uint32_t>(rank) });
		}
		for (size_t index = 1; index < places.size(); index++)
		{
			for (size_t place = index; place > 0; place--)
			{
				if (!(places[place].Importance > places[place - 1].Importance * c_LocalShadowKeepRatio))
					break;
				std::swap(places[place], places[place - 1]);
			}
		}
		m_LocalShadowScratch.clear();
		for (const Place& place : places)
		{
			if (place.Held != c_NoLocalShadowLight)
			{
				m_LocalShadowScratch.push_back(std::move(m_LocalShadowLights[place.Held]));
				m_LocalShadowScratch.back().Ranked = true;
				continue;
			}
			const Candidate& candidate = candidates[place.Candidate];
			LocalShadowLight& light = m_LocalShadowScratch.emplace_back();
			light.Entity = lights.GetEntity(candidate.Record);
			light.Type = lights.GetType(candidate.Record);
			light.ViewCount = light.Type == RenderData::c_LightPoint ? RenderData::c_LocalShadowFaces : 1u;
			light.Record = candidate.Record;
			light.Importance = candidate.Importance;
			light.Ranked = true;
		}
		// Lights still in view that lost their rank fade their shadows out where lighting updates are spread (keeping their tiles
		// meanwhile); the others give their tiles back now. A light out of view lights nothing the view shows, so its shadow goes
		// at once.
		for (uint32_t index = 0; index < m_LocalShadowLights.size(); index++)
		{
			if (fates[index] == Fate::Ranked)
				continue;
			LocalShadowLight& light = m_LocalShadowLights[index];
			if (fates[index] == Fate::Unranked && spread && light.Fade > fadeStep)
			{
				m_LocalShadowScratch.push_back(std::move(light));
				m_LocalShadowScratch.back().Ranked = false;
				continue;
			}
			FreeLocalShadowTiles(light);
		}
		m_LocalShadowLights.swap(m_LocalShadowScratch);
		return static_cast<uint32_t>(candidates.size());
	}

	void SceneRenderer::PlaceLocalShadowTiles(const RenderLights& lights, const SceneCamera& camera, bool spread)
	{
		// Tile sizes: as large as the light looks (with hysteresis), stepped down from the least important light while they do
		// not fit the share of the atlas tiles are fitted into (RankLocalShadowLights kept no more than it holds at the smallest).
		const std::span<const RenderData::LightRecord> records = lights.GetRecords();
		const uint32_t atlasSize = m_LocalShadowAllocator.GetAtlasSize();
		const uint32_t maxTile = std::min(m_Specification.Quality.LocalShadowMaxTile, atlasSize);
		const uint32_t minTile = std::min(c_MinLocalShadowTile, maxTile);
		const float viewportHeight = static_cast<float>(m_ViewportSize.y);
		size_t ranked = 0;
		while (ranked < m_LocalShadowLights.size() && m_LocalShadowLights[ranked].Ranked)
			ranked++;
		std::vector<uint32_t> sizes(ranked);
		std::vector<uint32_t> tileCounts(ranked);
		for (size_t index = 0; index < ranked; index++)
		{
			LocalShadowLight& light = m_LocalShadowLights[index];
			const glm::vec4 bounds = records[light.Record].Bounds;
			const float pixels = GetScreenFraction(camera, glm::vec3(bounds), bounds.w) * viewportHeight * 0.5f;
			uint32_t wanted = std::clamp(CeilPowerOfTwo(pixels), minTile, maxTile);
			if (light.WantedSize != 0 && pixels > static_cast<float>(light.WantedSize) * c_TileShrinkRatio
				&& pixels <= static_cast<float>(light.WantedSize) * c_TileGrowRatio)
				wanted = std::clamp(light.WantedSize, minTile, maxTile);
			light.WantedSize = wanted;
			sizes[index] = wanted;
			tileCounts[index] = light.ViewCount;
		}
		const uint64_t budget = static_cast<uint64_t>(atlasSize) * atlasSize / 8 * c_LocalShadowAtlasEighths;
		const size_t fit = ShadowAtlasAllocator::FitTileSizes(sizes, tileCounts, atlasSize, minTile, budget);
		for (size_t index = fit; index < ranked; index++)
			sizes[index] = minTile; // Only if the atlas shrank since the ranking: they take what is free

		// Changes, within the frame's limits: shrinks first (the largest tiles first), then the new lights (at their size, else a
		// smaller one: never a new layout of the atlas), then grows (the most important first). A light whose tiles change size
		// keeps sampling its old tiles until its views are drawn into the new ones; at most c_MaxLocalShadowTransitions lights
		// do at a time where lighting updates are spread, and one still changing waits for the next frame. When the atlas has no
		// room, a light keeps its size.
		const auto inTransition = [](const LocalShadowLight& light)
		{
			for (uint32_t view = 0; view < light.ViewCount; view++)
			{
				if (light.Views[view].HasContent && light.Views[view].DrawnKey.Tile != light.Tiles[view])
					return true;
			}
			return false;
		};
		const uint32_t limit = spread ? c_MaxLocalShadowTransitions : std::numeric_limits<uint32_t>::max();
		uint32_t transitions = 0;
		for (const LocalShadowLight& light : m_LocalShadowLights)
			transitions += inTransition(light) ? 1u : 0u;
		const auto resize = [&](const std::vector<uint32_t>& order)
		{
			for (const uint32_t index : order)
			{
				LocalShadowLight& light = m_LocalShadowLights[index];
				if (transitions >= limit)
					break;
				if (inTransition(light) || !ResizeLocalShadowTiles(light, sizes[index]))
					continue;
				transitions += inTransition(light) ? 1u : 0u;
			}
		};
		std::vector<uint32_t> order;
		for (uint32_t index = 0; index < ranked; index++)
		{
			if (m_LocalShadowLights[index].TileSize > sizes[index])
				order.push_back(index);
		}
		std::sort(order.begin(), order.end(), [this](uint32_t a, uint32_t b)
		{
			const uint32_t sizeA = m_LocalShadowLights[a].TileSize;
			const uint32_t sizeB = m_LocalShadowLights[b].TileSize;
			return sizeA != sizeB ? sizeA > sizeB : a > b;
		});
		resize(order);
		for (uint32_t index = 0; index < ranked; index++)
		{
			LocalShadowLight& light = m_LocalShadowLights[index];
			for (uint32_t size = sizes[index]; light.TileSize == 0 && size >= minTile && size > 0; size /= 2)
				ResizeLocalShadowTiles(light, size);
		}
		order.clear();
		for (uint32_t index = 0; index < ranked; index++)
		{
			if (m_LocalShadowLights[index].TileSize != 0 && m_LocalShadowLights[index].TileSize < sizes[index])
				order.push_back(index);
		}
		resize(order);

		// Each view's key this frame.
		for (LocalShadowLight& light : m_LocalShadowLights)
		{
			if (light.TileSize == 0)
				continue;
			const RenderData::LightRecord& record = records[light.Record];
			const LightShadowSettings& settings = lights.GetShadowSettings(light.Record);
			light.ShadowSettings = glm::vec3(settings.Bias, settings.NormalBias, settings.FilterRadius);
			const float range = record.PositionRange.w;
			// The guard band: what a lookup reaches (and one texel more) on every side of what the view must cover. The normal bias
			// moves a lookup by up to its texels times c_NormalBiasReach toward a neighbouring face, the filter's taps reach their
			// radius beyond that.
			const float guard = std::ceil(settings.FilterRadius + settings.NormalBias * c_NormalBiasReach) + 1.0f;
			const float tileSize = static_cast<float>(light.TileSize);
			const float widen = tileSize / std::max(tileSize - 2.0f * guard, 1.0f);
			float tanHalfAngle = 1.0f; // A cube face's 45 degrees
			if (light.Type == RenderData::c_LightSpot)
				tanHalfAngle = std::tan(std::clamp(std::acos(std::clamp(record.SpotAngles.y, -1.0f, 1.0f)), 0.001f, c_MaxSpotShadowHalfAngle));
			for (uint32_t view = 0; view < light.ViewCount; view++)
			{
				LocalShadowViewKey& key = light.Views[view].Key;
				key.Position = glm::vec3(record.PositionRange);
				key.Direction = light.Type == RenderData::c_LightSpot ? glm::vec3(record.DirectionType) : c_FaceDirections[view];
				key.Range = range;
				// Within its bounds, and below half the range for tiny lights (std::clamp would need the bounds in order).
				key.Near = std::min(std::max(range * c_LocalShadowNearScale, c_MinLocalShadowNear), std::min(c_MaxLocalShadowNear, range * 0.5f));
				key.TanHalfAngle = tanHalfAngle * widen;
				key.Tile = light.Tiles[view];
			}
		}
	}

	void SceneRenderer::TrackLocalShadowChanges(RenderScene& renderScene)
	{
		const std::span<const RenderData::LightRecord> records = renderScene.GetLights().GetRecords();
		// What every view's content depends on beyond the casters: a change redraws them all.
		LocalShadowContent content;
		content.RenderSceneId = renderScene.GetId();
		content.TablesVersion = m_SceneGPU->GetTablesVersion();
		content.PoolContentVersion = Renderer::GetGeometryPool()->GetContentVersion();
		// Entries as the frame's uploads will write them: what changed since another renderer's frame uploaded them is seen
		// alike by every renderer of the scene.
		content.TextureEntryVersion = m_SceneGPU->HasAlphaMaskedMaterials() ? Renderer::GetTextureTable().GetEntryVersion() : 0;
		// New proxies whose upload waits (NewInstanceBytesPerFrame) appear later without a change of their own.
		content.DeferredFrame = m_SceneGPU->GetDeferredProxies() > 0 ? m_RenderCount + 1 : 0;
		content.LODErrorPixels = m_Specification.Quality.LODErrorPixels;
		const bool contentChanged = !(content == m_LocalShadowContent);
		m_LocalShadowContent = content;

		bool anyContent = false;
		// Bounds of proxies not followed are known not to have reached a view while every view with content was drawn since the
		// following started over (each drawing follows every proxy within its light's range).
		bool followed = true;
		for (LocalShadowLight& light : m_LocalShadowLights)
		{
			for (uint32_t view = 0; view < light.ViewCount; view++)
			{
				LocalShadowView& state = light.Views[view];
				anyContent = anyContent || state.HasContent;
				followed = followed && (!state.HasContent || state.DrawnEpoch == m_LocalShadowEpoch);
				if (contentChanged)
					state.Dirty = true;
			}
		}
		const auto dirtyAll = [this]()
		{
			for (LocalShadowLight& light : m_LocalShadowLights)
			{
				for (uint32_t index = 0; index < light.ViewCount; index++)
					light.Views[index].Dirty = true;
			}
		};
		const uint64_t proxyVersion = renderScene.GetProxyVersion();
		if (!anyContent)
		{
			m_LocalShadowBounds.clear();
		}
		else if (proxyVersion != m_LocalShadowProxyVersion)
		{
			m_LocalShadowChanges.clear();
			if (!renderScene.GetProxyChanges(m_LocalShadowProxyVersion, m_LocalShadowChanges))
			{
				// More changes than the log keeps: everything is drawn again, and followed from there.
				dirtyAll();
				m_LocalShadowBounds.clear();
				m_LocalShadowEpoch++;
			}
			else if (!m_LocalShadowChanges.empty())
			{
				// A grid of the lights' reach (their ranges now and as their views were drawn) finds the few lights a change can
				// reach; cells twice the median range wide. Beside every light goes the box of its reach, which of its views a
				// change can still reach and the box of those (LocalShadowReach): most changes reach none (a caster on the ground
				// keeps moving through views that are dirty already, below the ones that look up), and are done without reading
				// the lights. The grid is sorted again only when a box or the cells' size changed: the boxes in the lights' order
				// say all it holds.
				std::vector<float> ranges;
				ranges.reserve(m_LocalShadowLights.size());
				for (const LocalShadowLight& light : m_LocalShadowLights)
					ranges.push_back(records[light.Record].PositionRange.w);
				std::nth_element(ranges.begin(), ranges.begin() + static_cast<std::ptrdiff_t>(ranges.size() / 2), ranges.end());
				const float cellSize = std::max(ranges.empty() ? 1.0f : ranges[ranges.size() / 2] * 2.0f, 0.5f);
				bool gridValid = cellSize == m_LocalShadowCellSize && m_LocalShadowReach.size() == m_LocalShadowLights.size();
				m_LocalShadowCellSize = cellSize;
				m_LocalShadowReach.resize(m_LocalShadowLights.size());
				for (uint32_t index = 0; index < m_LocalShadowLights.size(); index++)
				{
					const LocalShadowLight& light = m_LocalShadowLights[index];
					const glm::vec4 position = records[light.Record].PositionRange;
					AABB box;
					ExpandByReach(box, glm::vec3(position), position.w);
					AABB cleanBox;
					uint32_t cleanViews = 0;
					bool drawnElsewhere = false;
					for (uint32_t face = 0; face < light.ViewCount; face++)
					{
						const LocalShadowView& view = light.Views[face];
						if (!view.HasContent)
							continue;
						ExpandByReach(box, view.DrawnKey.Position, view.DrawnKey.Range);
						drawnElsewhere = drawnElsewhere || view.DrawnKey.Position != glm::vec3(position) || view.DrawnKey.Range != position.w;
						if (view.Dirty)
							continue;
						cleanViews |= 1u << face;
						cleanBox.Expand(view.DrawnBox);
					}
					LocalShadowReach& reach = m_LocalShadowReach[index];
					gridValid = gridValid && reach.Box.Min == box.Min && reach.Box.Max == box.Max;
					reach.Box = box;
					reach.CleanBox = cleanBox;
					reach.PositionRange = position;
					reach.CleanViews = cleanViews;
					reach.DrawnElsewhere = drawnElsewhere;
				}
				if (!gridValid)
				{
					m_LocalShadowGrid.clear();
					m_LocalShadowLargeLights.clear();
					for (uint32_t index = 0; index < m_LocalShadowReach.size(); index++)
					{
						const AABB& box = m_LocalShadowReach[index].Box;
						const GridRange cells = GetGridRange(box.Min, box.Max, m_LocalShadowCellSize);
						if (cells.GetCellCount() > c_LocalShadowGridMaxCells)
						{
							m_LocalShadowLargeLights.push_back(index);
							continue;
						}
						for (int32_t x = cells.Min.x; x <= cells.Max.x; x++)
						{
							for (int32_t y = cells.Min.y; y <= cells.Max.y; y++)
							{
								for (int32_t z = cells.Min.z; z <= cells.Max.z; z++)
									m_LocalShadowGrid.emplace_back(GetGridKey(x, y, z), index);
							}
						}
					}
					std::sort(m_LocalShadowGrid.begin(), m_LocalShadowGrid.end());
				}
				m_LocalShadowStamps.resize(m_LocalShadowLights.size(), 0u);
				// The lights whose reach a box may touch, each once (m_LocalShadowQueried). Changes come in runs of the same cells
				// (a proxy's bounds before and after its move, its neighbours'): the cells just asked keep their answer.
				GridRange queriedCells;
				bool queriedValid = false;
				const auto query = [&](const AABB& bounds)
				{
					if (!bounds.IsValid())
					{
						m_LocalShadowQueried.clear();
						queriedValid = false;
						return;
					}
					const GridRange cells = GetGridRange(bounds.Min, bounds.Max, m_LocalShadowCellSize);
					if (queriedValid && cells.Min == queriedCells.Min && cells.Max == queriedCells.Max)
						return;
					queriedCells = cells;
					queriedValid = true;
					m_LocalShadowQueried.clear();
					if (cells.GetCellCount() > c_LocalShadowGridMaxCells)
					{
						for (uint32_t index = 0; index < m_LocalShadowLights.size(); index++)
							m_LocalShadowQueried.push_back(index);
						return;
					}
					if (++m_LocalShadowStamp == 0)
					{
						std::fill(m_LocalShadowStamps.begin(), m_LocalShadowStamps.end(), 0u);
						m_LocalShadowStamp = 1;
					}
					m_LocalShadowQueried.insert(m_LocalShadowQueried.end(), m_LocalShadowLargeLights.begin(), m_LocalShadowLargeLights.end());
					for (int32_t x = cells.Min.x; x <= cells.Max.x; x++)
					{
						for (int32_t y = cells.Min.y; y <= cells.Max.y; y++)
						{
							for (int32_t z = cells.Min.z; z <= cells.Max.z; z++)
							{
								const uint64_t key = GetGridKey(x, y, z);
								auto it = std::lower_bound(m_LocalShadowGrid.begin(), m_LocalShadowGrid.end(), std::pair<uint64_t, uint32_t>(key, 0u));
								for (; it != m_LocalShadowGrid.end() && it->first == key; ++it)
								{
									if (m_LocalShadowStamps[it->second] == m_LocalShadowStamp)
										continue;
									m_LocalShadowStamps[it->second] = m_LocalShadowStamp;
									m_LocalShadowQueried.push_back(it->second);
								}
							}
						}
					}
				};
				// The views whose content a proxy's change reaches: its bounds before (as followed) or after reach the light's
				// range and the view's frustum as the view was drawn. Only views a change has not reached yet are asked, and only
				// of lights whose box of such views the bounds touch.
				const auto markReached = [this](const AABB& bounds)
				{
					for (const uint32_t index : m_LocalShadowQueried)
					{
						LocalShadowReach& reach = m_LocalShadowReach[index];
						if (!BoxesTouch(bounds, reach.CleanBox))
							continue;
						LocalShadowLight& light = m_LocalShadowLights[index];
						for (uint32_t face = 0; face < light.ViewCount; face++)
						{
							if ((reach.CleanViews & (1u << face)) == 0)
								continue;
							LocalShadowView& view = light.Views[face];
							const float margin = view.DrawnKey.Range * c_LocalShadowChangeMargin;
							const glm::vec3 boxMin = bounds.Min - margin;
							const glm::vec3 boxMax = bounds.Max + margin;
							if (BoxReachesSphere(boxMin, boxMax, view.DrawnKey.Position, view.DrawnKey.Range) && BoxInPlanes(boxMin, boxMax, view.DrawnPlanes))
							{
								view.Dirty = true;
								reach.CleanViews &= ~(1u << face);
							}
						}
					}
				};
				bool allDirty = false;
				for (const uint32_t proxy : m_LocalShadowChanges)
				{
					const bool alive = proxy < renderScene.GetProxyCapacity() && renderScene.GetEntity(proxy) != entt::null;
					const AABB bounds = alive ? renderScene.GetWorldBounds(proxy) : AABB();
					const auto it = m_LocalShadowBounds.find(proxy);
					if (it != m_LocalShadowBounds.end())
					{
						query(it->second);
						markReached(it->second);
					}
					else if (!followed && !allDirty)
					{
						// Once: nothing here makes a view clean again.
						dirtyAll();
						for (LocalShadowReach& reach : m_LocalShadowReach)
						{
							reach.CleanViews = 0;
							reach.CleanBox = AABB();
						}
						allDirty = true;
					}
					query(bounds);
					markReached(bounds);
					// Followed while within a shadowed light's range.
					bool inRange = false;
					for (const uint32_t index : m_LocalShadowQueried)
					{
						const LocalShadowReach& reach = m_LocalShadowReach[index];
						if (!BoxesTouch(bounds, reach.Box))
							continue;
						inRange = BoxReachesSphere(bounds.Min, bounds.Max, glm::vec3(reach.PositionRange), reach.PositionRange.w);
						// The views' own ranges only where one was drawn from elsewhere (the light moved since).
						const LocalShadowLight& light = m_LocalShadowLights[index];
						for (uint32_t face = 0; reach.DrawnElsewhere && face < light.ViewCount && !inRange; face++)
						{
							const LocalShadowView& view = light.Views[face];
							inRange = view.HasContent && BoxReachesSphere(bounds.Min, bounds.Max, view.DrawnKey.Position, view.DrawnKey.Range);
						}
						if (inRange)
							break;
					}
					if (!inRange)
					{
						if (it != m_LocalShadowBounds.end())
							m_LocalShadowBounds.erase(it);
					}
					else if (it != m_LocalShadowBounds.end())
					{
						it->second = bounds;
					}
					else
					{
						m_LocalShadowBounds.emplace(proxy, bounds);
					}
				}
			}
		}
		m_LocalShadowProxyVersion = proxyVersion;
		if (m_LocalShadowBounds.size() > GetMaxLocalShadowBounds())
		{
			dirtyAll();
			m_LocalShadowBounds.clear();
			m_LocalShadowEpoch++;
		}
	}

	uint32_t SceneRenderer::ScheduleLocalShadowViews(bool spread)
	{
		// The views not up to date (every view without the cache). The lights fading their shadows out keep what they hold.
		struct Waiting
		{
			uint32_t Light = 0;
			uint32_t Face = 0;
			bool Empty = false;  // No content yet: its light casts no shadow until it is drawn
			uint32_t Waited = 0; // Frames
			float Score = 0.0f;  // Frames waited (and this one) times its light's importance
		};
		std::vector<Waiting> waiting;
		const bool cache = m_Specification.CacheLocalShadows;
		for (uint32_t index = 0; index < m_LocalShadowLights.size(); index++)
		{
			LocalShadowLight& light = m_LocalShadowLights[index];
			for (uint32_t face = 0; face < light.ViewCount; face++)
			{
				LocalShadowView& view = light.Views[face];
				view.Drawing = false;
				if (light.TileSize == 0)
					continue;
				if (cache && view.HasContent && !view.Dirty && view.Key == view.DrawnKey)
				{
					view.WaitedFrames = 0;
					continue;
				}
				if (!light.Ranked)
					continue;
				waiting.push_back(Waiting { index, face, !view.HasContent, view.WaitedFrames, light.Importance * static_cast<float>(view.WaitedFrames + 1) });
			}
		}
		std::vector<uint8_t> chosen(waiting.size(), 1);
		const uint32_t budget = std::max(m_Specification.Quality.LocalShadowViewsPerFrame, 1u);
		if (spread && waiting.size() > budget)
		{
			// The views with content that waited longest take a share of the budget, so that none starves while nearer lights
			// change every frame; the rest go to views without content (the lights' order, a light's views together), then by
			// waiting times importance.
			std::fill(chosen.begin(), chosen.end(), 0);
			std::vector<uint32_t> order;
			for (uint32_t index = 0; index < waiting.size(); index++)
			{
				if (!waiting[index].Empty)
					order.push_back(index);
			}
			// Of each order only the first are taken (far fewer than wait where this matters): sorted no further.
			const uint32_t aged = std::max(budget / c_LocalShadowAgedShare, 1u);
			size_t agedPlaces = std::min<size_t>(aged, order.size());
			std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(agedPlaces), order.end(), [&waiting](uint32_t a, uint32_t b)
			{
				const Waiting& first = waiting[a];
				const Waiting& second = waiting[b];
				if (first.Waited != second.Waited)
					return first.Waited > second.Waited;
				return first.Score != second.Score ? first.Score > second.Score : a < b;
			});
			// A budget of one view a frame is all the share: the view with content takes it only if it waited longer than the
			// view without content the budget would take otherwise (the first in the lights' order; on a tie, views without
			// content first), so that while a view with content changes every frame a new light's views are still drawn.
			if (agedPlaces >= budget)
			{
				const auto empty = std::find_if(waiting.begin(), waiting.end(), [](const Waiting& view) { return view.Empty; });
				if (empty != waiting.end() && !(waiting[order[0]].Waited > empty->Waited))
					agedPlaces = 0;
			}
			uint32_t taken = 0;
			for (size_t place = 0; place < agedPlaces; place++, taken++)
				chosen[order[place]] = 1;
			order.resize(waiting.size());
			for (uint32_t index = 0; index < waiting.size(); index++)
				order[index] = index;
			// The first `budget` places hold what the budget still takes: at most `taken` of them are chosen already.
			const size_t places = std::min<size_t>(budget, order.size());
			std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(places), order.end(), [&waiting](uint32_t a, uint32_t b)
			{
				const Waiting& first = waiting[a];
				const Waiting& second = waiting[b];
				if (first.Empty != second.Empty)
					return first.Empty;
				if (!first.Empty && first.Score != second.Score)
					return first.Score > second.Score;
				return a < b; // The lights' order, then their faces
			});
			for (size_t place = 0; place < places && taken < budget; place++)
			{
				if (chosen[order[place]] != 0)
					continue;
				chosen[order[place]] = 1;
				taken++;
			}
		}
		// Drawn in the lights' order, so that a light's drawn views are consecutive (one caster query per light).
		uint32_t pending = 0;
		for (uint32_t index = 0; index < waiting.size(); index++)
		{
			LocalShadowView& view = m_LocalShadowLights[waiting[index].Light].Views[waiting[index].Face];
			if (chosen[index] == 0)
			{
				view.WaitedFrames++;
				pending++;
				continue;
			}
			view.Drawing = true;
			LocalShadowDrawView drawView;
			drawView.Light = waiting[index].Light;
			drawView.Face = waiting[index].Face;
			m_LocalShadowViews.push_back(drawView);
		}
		return pending;
	}

	void SceneRenderer::FindLocalShadowCasters(RenderScene& renderScene, const SceneCamera& camera, const Frustum& frustum, float fadeStep, bool exactCasters,
		bool markCasters)
	{
		ST_PROFILE_FUNCTION();
		ST_PROFILE_STAGE("Render.Extract");
		m_LocalShadowViews.clear();
		m_LocalShadowCasters.clear();
		m_LocalShadowCandidates.clear();
		// The atlas holds the shadows of one render scene: another one (also a clone, entering Play) starts without them.
		if (renderScene.GetId() != m_LocalShadowSceneId)
		{
			ResetLocalShadows();
			m_LocalShadowSceneId = renderScene.GetId();
			m_LocalShadowProxyVersion = renderScene.GetProxyVersion();
		}
		const RenderLights& lights = renderScene.GetLights();
		if (lights.GetShadowedRecords().empty() && m_LocalShadowLights.empty())
		{
			// Nothing casts: nothing to follow either.
			m_LocalShadowProxyVersion = renderScene.GetProxyVersion();
			m_LocalShadowBounds.clear();
			return;
		}

		// Spread over frames, with fades, only where the cache keeps views: without it every view is drawn every frame, so
		// nothing waits and nothing fades.
		const bool spread = m_Specification.SpreadLightingUpdates && m_Specification.CacheLocalShadows;
		const uint32_t requested = RankLocalShadowLights(lights, camera, frustum, spread, fadeStep);
		PlaceLocalShadowTiles(lights, camera, spread);
		TrackLocalShadowChanges(renderScene);
		const uint32_t pending = ScheduleLocalShadowViews(spread);

		// A light casts its shadow once every view holds content (drawn now, or kept from before, possibly from where it was or
		// in its old tiles); where lighting updates are spread it then fades in by the frame's step of the fade (the time since
		// the previous frame), and a light that lost its rank fades out as fast.
		m_Stats.LocalShadowRequested = requested;
		m_Stats.LocalShadowLights = 0;
		m_Stats.LocalShadowFading = 0;
		m_Stats.LocalShadowFadingIn = 0;
		m_Stats.LocalShadowTileSizes = {};
		m_Stats.LocalShadowSizeCounts = {};
		for (LocalShadowLight& light : m_LocalShadowLights)
		{
			light.Complete = light.TileSize > 0;
			for (uint32_t face = 0; face < light.ViewCount; face++)
				light.Complete = light.Complete && (light.Views[face].HasContent || light.Views[face].Drawing);
			if (!light.Complete)
				light.Strength = 0.0f;
			else if (!spread)
				light.Strength = 1.0f;
			else
				light.Strength = light.Ranked ? std::min(light.Fade + fadeStep, 1.0f) : std::max(light.Fade - fadeStep, 0.0f);
			if (!light.Ranked)
			{
				m_Stats.LocalShadowFading++;
				continue;
			}
			if (light.TileSize == 0)
				continue;
			m_Stats.LocalShadowFadingIn += light.Complete && light.Strength < 1.0f ? 1u : 0u;
			if (m_Stats.LocalShadowLights < m_Stats.LocalShadowTileSizes.size())
				m_Stats.LocalShadowTileSizes[m_Stats.LocalShadowLights] = static_cast<uint16_t>(std::min(light.TileSize, 65535u));
			m_Stats.LocalShadowLights++;
			const uint32_t sizeClass = static_cast<uint32_t>(std::countr_zero(std::max(light.TileSize / c_MinLocalShadowTile, 1u)));
			m_Stats.LocalShadowSizeCounts[std::min<size_t>(sizeClass, m_Stats.LocalShadowSizeCounts.size() - 1)]++;
		}
		m_Stats.LocalShadowViewsDrawn = static_cast<uint32_t>(m_LocalShadowViews.size());
		m_Stats.LocalShadowViewsPending = pending;
		if (m_LocalShadowViews.empty())
			return;

		// The drawn views' frusta and level-of-detail criteria, camera-relative (as both paths' tests are).
		const glm::dvec3 origin(camera.Position);
		for (LocalShadowDrawView& drawView : m_LocalShadowViews)
		{
			const LocalShadowLight& light = m_LocalShadowLights[drawView.Light];
			const LocalShadowViewKey& key = light.Views[drawView.Face].Key;
			drawView.Planes = Frustum(GetLocalShadowViewProjection(key, origin)).GetPlanes();
			const float height = static_cast<float>(key.Tile.Size);
			drawView.LOD = LODCriteria { m_Specification.LODThreshold, LODSelection::GetErrorThreshold(m_Specification.Quality.LODErrorPixels, height), height };
		}

		const std::span<const RenderData::LightRecord> records = lights.GetRecords();
		// The casters of each light with drawn views: one job per light queries the proxies within its range (followed from now
		// on) and, on the CPU path, tests them against each of its drawn views; merged in the views' order.
		std::vector<std::pair<uint32_t, uint32_t>> lightGroups; // (first drawn view, count): a light's drawn views are consecutive
		for (uint32_t index = 0; index < m_LocalShadowViews.size(); index++)
		{
			if (lightGroups.empty() || m_LocalShadowViews[lightGroups.back().first].Light != m_LocalShadowViews[index].Light)
				lightGroups.emplace_back(index, 0);
			lightGroups.back().second++;
		}
		std::vector<LocalShadowJob> jobs(lightGroups.size());
		const SpatialIndex& spatialIndex = renderScene.GetSpatialIndex();
		const bool skins = renderScene.HasSkinProxies();
		const glm::vec3 cameraPosition = camera.Position;
		JobSystem::ParallelFor(static_cast<uint32_t>(lightGroups.size()), 1, [&](uint32_t begin, uint32_t end)
		{
			for (uint32_t group = begin; group < end; group++)
			{
				LocalShadowJob& job = jobs[group];
				const auto [firstView, viewCount] = lightGroups[group];
				const LocalShadowLight& light = m_LocalShadowLights[m_LocalShadowViews[firstView].Light];
				const glm::vec4 position = records[light.Record].PositionRange;
				spatialIndex.QuerySphere(glm::vec3(position), position.w * c_CasterQueryScale + c_CasterQueryMargin, job.Candidates);
				job.ViewCasters.resize(viewCount);
				if (!exactCasters)
				{
					// The GPU's culling finds the casters: the animators of the skinned proxies within the range are reported
					// (their shadows may move).
					for (const uint32_t proxy : job.Candidates)
					{
						if (skins && renderScene.GetSkinAnimator(proxy) != entt::null)
							job.Animators.push_back(renderScene.GetSkinAnimator(proxy));
					}
					continue;
				}
				// Camera-relative, in float, as the GPU's culling takes them (Scene/Cull.comp).
				const glm::vec3 lightRelative(glm::dvec3(glm::vec3(position)) - origin);
				const float range = position.w;
				for (const uint32_t proxy : job.Candidates)
				{
					const uint32_t flags = renderScene.GetProxyFlags(proxy);
					const uint32_t required = RenderProxyFlags::Active | RenderProxyFlags::Indexed | RenderProxyFlags::CastShadows | RenderProxyFlags::MeshBounds;
					if ((flags & required) != required || renderScene.GetInstanceCount(proxy) == 0)
						continue;
					const AABB& bounds = renderScene.GetWorldBounds(proxy);
					const glm::vec3 relativeMin = bounds.Min - cameraPosition;
					const glm::vec3 relativeMax = bounds.Max - cameraPosition;
					if (!BoxReachesSphere(relativeMin, relativeMax, lightRelative, range))
						continue;
					const glm::vec3 center = (bounds.Min + bounds.Max) * 0.5f;
					const float radius = glm::length((bounds.Max - bounds.Min) * 0.5f);
					bool casts = false;
					for (uint32_t offset = 0; offset < viewCount; offset++)
					{
						const LocalShadowDrawView& drawView = m_LocalShadowViews[firstView + offset];
						if (!BoxInPlanes(relativeMin, relativeMax, drawView.Planes))
							continue;
						const LocalShadowViewKey& key = light.Views[drawView.Face].Key;
						const float fraction = radius / (std::max(glm::length(center - key.Position), key.Near) * key.TanHalfAngle);
						if (fraction * static_cast<float>(key.Tile.Size) < c_MinCasterTexels)
						{
							job.SubTexelCasters++;
							continue;
						}
						job.ViewCasters[offset].emplace_back(proxy, fraction);
						casts = true;
					}
					if (!casts)
						continue;
					if (skins && renderScene.GetSkinAnimator(proxy) != entt::null)
						job.Animators.push_back(renderScene.GetSkinAnimator(proxy));
					const uint32_t overrideSlot = renderScene.GetOverrideMaterialSlot(proxy);
					job.MeshMarks.emplace_back(renderScene.GetMeshSlot(proxy),
						c_UseShadow | (overrideSlot == RenderScene::c_InvalidIndex ? c_UseShadow << c_SubmeshMaterialsShift : 0u));
					if (overrideSlot != RenderScene::c_InvalidIndex)
						job.MaterialMarks.emplace_back(overrideSlot, c_UseShadow);
				}
			}
		});

		// Merged in order: the views' casters, the marks, the animators and the proxies to follow (and to bound the GPU's draws
		// with).
		const size_t maxBounds = GetMaxLocalShadowBounds();
		bool overflowed = false;
		for (uint32_t group = 0; group < lightGroups.size(); group++)
		{
			LocalShadowJob& job = jobs[group];
			const auto [firstView, viewCount] = lightGroups[group];
			const uint32_t firstCandidate = static_cast<uint32_t>(m_LocalShadowCandidates.size());
			m_LocalShadowCandidates.insert(m_LocalShadowCandidates.end(), job.Candidates.begin(), job.Candidates.end());
			for (uint32_t offset = 0; offset < viewCount; offset++)
			{
				LocalShadowDrawView& drawView = m_LocalShadowViews[firstView + offset];
				drawView.FirstCandidate = firstCandidate;
				drawView.CandidateCount = static_cast<uint32_t>(job.Candidates.size());
				drawView.FirstCaster = static_cast<uint32_t>(m_LocalShadowCasters.size());
				drawView.CasterCount = static_cast<uint32_t>(job.ViewCasters[offset].size());
				for (const auto& [proxy, fraction] : job.ViewCasters[offset])
					m_LocalShadowCasters.push_back(LocalShadowCaster { proxy, fraction });
			}
			if (markCasters)
			{
				for (const auto& [slot, usage] : job.MeshMarks)
					MarkMesh(slot, usage);
				for (const auto& [slot, usage] : job.MaterialMarks)
					MarkMaterial(slot, usage);
			}
			m_VisibleAnimators.insert(m_VisibleAnimators.end(), job.Animators.begin(), job.Animators.end());
			m_Stats.SubTexelCasters += job.SubTexelCasters;
			for (const uint32_t proxy : job.Candidates)
			{
				if (renderScene.GetEntity(proxy) == entt::null)
					continue;
				if (m_LocalShadowBounds.size() >= maxBounds && m_LocalShadowBounds.find(proxy) == m_LocalShadowBounds.end())
				{
					overflowed = true;
					break;
				}
				m_LocalShadowBounds[proxy] = renderScene.GetWorldBounds(proxy);
			}
		}
		m_LocalShadowDrawEpoch = m_LocalShadowEpoch;
		if (overflowed)
		{
			// Too many proxies to follow: views drawn from now on start the following over. The views drawn now are not followed
			// either (no epoch is 0), so that a change of a proxy that is not followed draws every view again.
			m_LocalShadowBounds.clear();
			m_LocalShadowEpoch++;
			m_LocalShadowDrawEpoch = 0;
		}
	}

	void SceneRenderer::GatherLocalShadowDraws(RenderScene& renderScene, bool casterDraws)
	{
		ST_PROFILE_FUNCTION();
		ST_PROFILE_STAGE("Render.Extract");
		m_LocalShadowBatches.clear();
		if (m_LocalShadowViews.empty() || !casterDraws)
			return;
		const std::span<const RenderData::InstanceData> instances = renderScene.GetInstances();
		const bool batchAcrossMaterials = m_Specification.BatchAcrossMaterials;
		const bool skins = renderScene.HasSkinProxies();
		uint32_t skipped = 0;
		std::vector<SortItem> draws;
		std::vector<SortItem> scratch;
		std::vector<SceneDrawBatch> batches;
		for (uint32_t viewIndex = 0; viewIndex < m_LocalShadowViews.size(); viewIndex++)
		{
			LocalShadowDrawView& drawView = m_LocalShadowViews[viewIndex];
			draws.clear();
			for (uint32_t item = drawView.FirstCaster; item < drawView.FirstCaster + drawView.CasterCount; item++)
			{
				const LocalShadowCaster& caster = m_LocalShadowCasters[item];
				const uint32_t meshSlot = renderScene.GetMeshSlot(caster.Proxy);
				const Mesh* mesh = m_FrameMeshes[meshSlot];
				const uint32_t count = renderScene.GetInstanceCount(caster.Proxy);
				if (!mesh || count == 0 || mesh->GetSubmeshes().size() != count || !m_SceneGPU->IsResident(caster.Proxy))
					continue;
				const bool mirrored = (renderScene.GetProxyFlags(caster.Proxy) & RenderProxyFlags::Mirrored) != 0;
				const bool skinned = skins && (renderScene.GetProxyFlags(caster.Proxy) & RenderProxyFlags::Skinned) != 0 && mesh->IsSkinned();
				const uint32_t first = renderScene.GetFirstInstance(caster.Proxy);
				const std::vector<Submesh>& submeshes = mesh->GetSubmeshes();
				for (uint32_t submeshIndex = 0; submeshIndex < count; submeshIndex++)
				{
					const uint32_t instance = first + submeshIndex;
					const uint32_t materialSlot = instances[instance].MaterialIndex;
					const uint32_t materialFlags = m_MaterialFlags[materialSlot];
					// Transparent surfaces cast no shadows.
					if ((materialFlags & c_MaterialAvailable) == 0 || (materialFlags & c_MaterialTransparent) != 0)
						continue;
					const uint32_t lod = LODSelection::SelectLOD(submeshes[submeshIndex], mesh->GetBounds(), caster.Fraction, drawView.LOD);
					if (submeshIndex >= SceneDrawKey::c_MaxSubmeshes || lod >= SceneDrawKey::c_MaxLODs)
					{
						skipped++;
						continue;
					}
					draws.push_back(SortItem { SceneDrawKey::Make(0, (materialFlags & c_MaterialAlphaMask) != 0, mirrored, (materialFlags & c_MaterialDoubleSided) != 0, skinned,
						meshSlot, submeshIndex, lod, batchAcrossMaterials ? 0u : materialSlot), instance, 0 });
				}
			}
			const auto sortStart = std::chrono::steady_clock::now();
			RadixSort(draws, scratch);
			BuildBatches(draws, batches, m_ShadowDrawInstancesBuild);
			m_Stats.SortMs += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - sortStart).count();
			drawView.FirstBatch = static_cast<uint32_t>(m_LocalShadowBatches.size());
			drawView.BatchCount = static_cast<uint32_t>(batches.size());
			for (SceneDrawBatch& batch : batches)
			{
				batch.Cascade = viewIndex;
				m_Stats.LocalShadowCasters += batch.InstanceCount;
				m_LocalShadowBatches.push_back(batch);
			}
		}
		m_Stats.LocalShadowDrawCalls = static_cast<uint32_t>(m_LocalShadowBatches.size());
		if (skipped > 0)
			m_Errors.Report(fmt::format("{} shadow caster instances of point and spot lights were not drawn: more than {} submeshes or {} levels of detail in a mesh",
				skipped, SceneDrawKey::c_MaxSubmeshes, SceneDrawKey::c_MaxLODs));
	}

	void SceneRenderer::BuildLocalShadowData(const SceneCamera& camera)
	{
		m_LocalShadowData.resize(m_LocalShadowLights.size());
		if (m_LocalShadowLights.empty())
			return;
		const glm::dvec3 origin(camera.Position);
		const float atlasSize = static_cast<float>(m_LocalShadowAllocator.GetAtlasSize());
		for (uint32_t slot = 0; slot < m_LocalShadowLights.size(); slot++)
		{
			const LocalShadowLight& light = m_LocalShadowLights[slot];
			RenderData::LocalShadowData& data = m_LocalShadowData[slot];
			data = {};
			if (light.TileSize == 0)
				continue; // Waits for its tiles: no views, strength 0 (no light names the slot)
			for (uint32_t face = 0; face < light.ViewCount; face++)
			{
				// What the tile holds: drawn now with its key, else what it was drawn with.
				const LocalShadowView& view = light.Views[face];
				const LocalShadowViewKey& key = view.Drawing || !view.HasContent ? view.Key : view.DrawnKey;
				data.ViewProjection[face] = GetLocalShadowViewProjection(key, origin);
				const float tileSize = static_cast<float>(std::max(key.Tile.Size, 1u));
				data.Tiles[face] = glm::vec4(static_cast<float>(key.Tile.X) / atlasSize, static_cast<float>(key.Tile.Y) / atlasSize, tileSize / atlasSize,
					2.0f * key.TanHalfAngle / tileSize);
			}
			data.Parameters = glm::vec4(light.ShadowSettings, light.Strength);
			const glm::vec3 position(glm::dvec3(light.Views[0].Key.Position) - origin);
			data.Light = glm::vec4(position, light.Type == RenderData::c_LightPoint ? 1.0f : 0.0f);
		}
	}

	bool SceneRenderer::EnsureLocalShadowAtlas()
	{
		if (m_LocalShadowAtlas)
			return true;
		nvrhi::TextureDesc desc;
		desc.width = m_LocalShadowAllocator.GetAtlasSize();
		desc.height = m_LocalShadowAllocator.GetAtlasSize();
		desc.format = nvrhi::Format::D32;
		desc.isRenderTarget = true;
		desc.debugName = m_Specification.DebugName + ".LocalShadowAtlas";
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		nvrhi::TextureHandle atlas = m_Device->createTexture(desc);
		// Large atlases (Ultra's 8192: 256 MiB) may not fit the device's memory: half the size each way, down to 2048, rather
		// than no shadows.
		while (!atlas && desc.width > c_MinFallbackLocalShadowAtlasSize)
		{
			desc.width /= 2;
			desc.height /= 2;
			atlas = m_Device->createTexture(desc);
		}
		nvrhi::FramebufferHandle framebuffer = atlas ? m_Device->createFramebuffer(nvrhi::FramebufferDesc().setDepthAttachment(atlas)) : nullptr;
		if (!atlas || !framebuffer)
		{
			m_Errors.Report(fmt::format("failed to create the {0}x{0} shadow atlas of point and spot lights; they cast no shadows", m_LocalShadowAllocator.GetAtlasSize()));
			return false;
		}
		m_LocalShadowAtlas = std::move(atlas);
		m_LocalShadowFramebuffer = std::move(framebuffer);
		m_LocalShadowAtlasCleared = false;
		if (desc.width != m_LocalShadowAllocator.GetAtlasSize())
		{
			// The frame's tiles were placed for the larger atlas (FindLocalShadowCasters ran before): this frame draws no local
			// shadows, as a failure does, and the next places its tiles in the atlas made. Released targets try the
			// quality's size again.
			if (!m_LocalShadowAtlasFallbackReported)
			{
				m_LocalShadowAtlasFallbackReported = true;
				m_Errors.Report(fmt::format("the {}-texel shadow atlas of point and spot lights could not be allocated; using {}",
					m_LocalShadowAllocator.GetAtlasSize(), desc.width));
			}
			m_LocalShadowAllocator.Reset(desc.width, c_MinLocalShadowTile);
			ResetLocalShadows();
			return false;
		}
		return true;
	}

	void SceneRenderer::ClearLocalShadowTile(nvrhi::ICommandList* commandList, const ShadowAtlasTile& tile)
	{
		if (!m_LocalShadowAtlasCleared)
		{
			// A new atlas: its tiles hold nothing yet (the far plane everywhere).
			commandList->clearDepthStencilTexture(m_LocalShadowAtlas, nvrhi::AllSubresources, true, 0.0f, false, 0);
			m_LocalShadowAtlasCleared = true;
		}
		if (!m_LocalShadowClearPipeline)
		{
			// The fullscreen triangle lies at depth 0, the far plane (reversed-Z): written into the tile whatever it held.
			nvrhi::GraphicsPipelineDesc desc;
			desc.primType = nvrhi::PrimitiveType::TriangleList;
			desc.VS = m_FullscreenVertexShader;
			desc.renderState.rasterState.setCullNone();
			desc.renderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(true).setDepthFunc(nvrhi::ComparisonFunc::Always);
			m_LocalShadowClearPipeline = m_Device->createGraphicsPipeline(desc, m_LocalShadowFramebuffer->getFramebufferInfo());
			ST_CORE_VERIFY(m_LocalShadowClearPipeline, "SceneRenderer: failed to create the shadow tiles' clear pipeline");
		}
		nvrhi::GraphicsState state;
		state.pipeline = m_LocalShadowClearPipeline;
		state.framebuffer = m_LocalShadowFramebuffer;
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(tile.X), static_cast<float>(tile.X + tile.Size), static_cast<float>(tile.Y),
			static_cast<float>(tile.Y + tile.Size), 0.0f, 1.0f));
		commandList->setGraphicsState(state);
		commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
	}

	void SceneRenderer::DrawLocalShadows(nvrhi::ICommandList* commandList)
	{
		const uint32_t firstArgument = static_cast<uint32_t>(m_OpaqueBatches.size() + m_TransparentBatches.size() + m_ShadowBatches.size() + m_SelectionBatches.size());
		for (const LocalShadowDrawView& drawView : m_LocalShadowViews)
		{
			const LocalShadowLight& light = m_LocalShadowLights[drawView.Light];
			const ShadowAtlasTile& tile = light.Views[drawView.Face].Key.Tile;
			ClearLocalShadowTile(commandList, tile);
			if (drawView.BatchCount == 0)
				continue;
			const nvrhi::Viewport viewport(static_cast<float>(tile.X), static_cast<float>(tile.X + tile.Size), static_cast<float>(tile.Y),
				static_cast<float>(tile.Y + tile.Size), 0.0f, 1.0f);
			nvrhi::GraphicsState state;
			state.viewport.addViewportAndScissorRect(viewport);
			state.bindings = { m_ShadowBindingSet, Renderer::GetBindlessTextures().GetTable(), m_ShadowPushBindingSet };
			// The view's matrix is its light's slot's (a push constant after the state, as for the cascades).
			const uint32_t viewIndex = drawView.Light * RenderData::c_LocalShadowFaces + drawView.Face;
			DrawBuckets(commandList, state, std::span<const SceneDrawBatch>(m_LocalShadowBatches.data() + drawView.FirstBatch, drawView.BatchCount),
				firstArgument + drawView.FirstBatch,
				[](const SceneDrawBatch& batch)
				{
					return PipelineKey { batch.AlphaMask ? Pass::LocalShadowMasked : Pass::LocalShadow, batch.DoubleSided, batch.Mirrored, batch.Skinned };
				},
				[this](const SceneDrawBatch&, nvrhi::GraphicsState& bucketState) { bucketState.framebuffer = m_LocalShadowFramebuffer; },
				[commandList, viewIndex](const SceneDrawBatch&) { commandList->setPushConstants(&viewIndex, sizeof(viewIndex)); });
		}
	}

	void SceneRenderer::CommitLocalShadows()
	{
		for (LocalShadowLight& light : m_LocalShadowLights)
		{
			for (uint32_t face = 0; face < light.ViewCount; face++)
			{
				LocalShadowView& view = light.Views[face];
				if (!view.Drawing)
					continue;
				// A view drawn into its light's new tiles gives the old one back: nothing samples it from now on.
				if (view.HasContent && view.DrawnKey.Tile != view.Key.Tile)
					m_LocalShadowAllocator.Free(view.DrawnKey.Tile);
				view.DrawnKey = view.Key;
				view.DrawnPlanes = Frustum(GetLocalShadowViewProjection(view.Key, glm::dvec3(0.0))).GetPlanes();
				view.DrawnBox = GetLocalShadowChangeBox(view.Key.Position, view.Key.Range, view.DrawnPlanes);
				view.DrawnEpoch = m_LocalShadowDrawEpoch;
				view.HasContent = true;
				view.Dirty = false;
				view.Drawing = false;
				view.WaitedFrames = 0;
			}
			light.Fade = light.Strength;
		}
	}

}
