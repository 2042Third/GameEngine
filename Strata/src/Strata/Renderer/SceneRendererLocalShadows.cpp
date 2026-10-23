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
#include "Strata/Renderer/TextureTable.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

// Shadows of point and spot lights (SceneRendererSpecification::LocalShadowAtlasSize). The most relevant shadowed lights in
// view get tiles of the renderer's depth atlas: a spot light one perspective view, a point light six (a cube's faces), each
// view widened by a guard band of its filter's reach so that filtering never leaves the face it samples. A view is drawn
// again only when what it shows changed (its key, a caster reaching it, what casters are drawn with), at most
// LocalShadowViewsPerFrame of them per frame (nearest lights first) where SpreadLightingUpdates spreads them; a light casts
// its shadow once all its views hold content, from the matrices they were drawn with. Which views are drawn is the CPU's
// decision on both paths; what each draws is decided by the CPU path here and by the GPU path's culling.

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_MinLocalShadowTile = 128;
		constexpr uint32_t c_MaxLocalShadowTile = 1024;
		// A light's tile size follows the pixels its range spans on screen, halved (the receivers its views must resolve lie
		// within the range, on average halfway), as a power of two; it changes only once that leaves this band around the
		// size it has, so sizes do not toggle while the camera moves.
		constexpr float c_TileGrowRatio = 1.25f;
		constexpr float c_TileShrinkRatio = 0.4f;
		// The lights ranked just above the most relevant one left out fade their shadows over this fraction of their
		// relevance's margin over it (SpreadLightingUpdates), so a light entering the ranks does not pop.
		constexpr float c_LocalShadowRankMargin = 0.25f;
		// A spot light's view covers at most this half angle (radians, 80 degrees): one perspective tile cannot show a
		// hemisphere. Wider cones are shadowed within it.
		constexpr float c_MaxSpotShadowHalfAngle = 1.3962634f;
		// Views clip at a near plane of this fraction of the light's range, within these bounds (meters).
		constexpr float c_LocalShadowNearScale = 0.001f;
		constexpr float c_MinLocalShadowNear = 0.005f;
		constexpr float c_MaxLocalShadowNear = 0.1f;
		// Casters spanning less than this many texels of a view are left out of it, as for the cascades.
		constexpr float c_MinCasterTexels = 1.0f;
		// The CPU's query for a light's casters reaches this much beyond its range, so that it finds every proxy the exact test
		// (in camera-relative coordinates, on either path) can take.
		constexpr float c_CasterQueryScale = 1.001f;
		constexpr float c_CasterQueryMargin = 0.01f;
		// The bounds of at most this many proxies within shadowed lights' ranges are followed; beyond, the views are drawn again
		// and the following starts over (m_LocalShadowEpoch).
		constexpr size_t c_MaxLocalShadowBounds = 65536;

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

		// Whether a box reaches into a sphere (the closest point of the box to the center is within the radius).
		bool BoxReachesSphere(const glm::vec3& boxMin, const glm::vec3& boxMax, const glm::vec3& center, float radius)
		{
			const glm::vec3 offset = glm::clamp(center, boxMin, boxMax) - center;
			return glm::dot(offset, offset) <= radius * radius;
		}

		// Frustum::IsAABBVisible's test, also the GPU culling's (Include/Culling.glsl, IsBoxInFrustum).
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

		// The sort key of a view's draws: pipeline state, then mesh, submesh and level of detail (SceneRendererExtraction.cpp's
		// layout below the cascade bits, which a view's own list does not need).
		constexpr uint32_t c_KeyAlphaMaskShift = 61;
		constexpr uint32_t c_KeyMirroredShift = 60;
		constexpr uint32_t c_KeyDoubleSidedShift = 59;
		constexpr uint32_t c_KeySkinnedShift = 58;
		constexpr uint32_t c_KeyMeshShift = 38;
		constexpr uint32_t c_KeySubmeshShift = 22;
		constexpr uint32_t c_KeyLODShift = 17;
		constexpr uint32_t c_KeyMaterialBits = 17;
		constexpr uint32_t c_MaxKeySubmeshes = 1u << 16;
		constexpr uint32_t c_MaxKeyLODs = 1u << 5;

		uint64_t MakeLocalDrawKey(bool alphaMask, bool mirrored, bool doubleSided, bool skinned, uint32_t meshSlot, uint32_t submesh, uint32_t lod, uint32_t material)
		{
			return (static_cast<uint64_t>(alphaMask) << c_KeyAlphaMaskShift) | (static_cast<uint64_t>(mirrored) << c_KeyMirroredShift)
				| (static_cast<uint64_t>(doubleSided) << c_KeyDoubleSidedShift) | (static_cast<uint64_t>(skinned) << c_KeySkinnedShift)
				| (static_cast<uint64_t>(meshSlot) << c_KeyMeshShift) | (static_cast<uint64_t>(submesh) << c_KeySubmeshShift)
				| (static_cast<uint64_t>(lod) << c_KeyLODShift) | static_cast<uint64_t>(material & ((1u << c_KeyMaterialBits) - 1));
		}

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
			m_LocalShadowAllocator.Free(light.Tiles[view]);
			light.Tiles[view] = {};
			light.Views[view].HasContent = false;
		}
		light.TileSize = 0;
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

	void SceneRenderer::FindLocalShadowCasters(RenderScene& renderScene, const SceneCamera& camera, const Frustum& frustum, bool exactCasters, bool markCasters)
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
		const std::span<const uint32_t> shadowed = lights.GetShadowedRecords();
		if (shadowed.empty() && m_LocalShadowLights.empty())
		{
			// Nothing casts: nothing to follow either.
			m_LocalShadowProxyVersion = renderScene.GetProxyVersion();
			m_LocalShadowBounds.clear();
			return;
		}

		// The shadowed lights in view, ranked as the light clusters rank lights: those whose range holds the camera first, then by
		// the projected size of their range (registry order between equals).
		struct Candidate
		{
			uint32_t Record = 0;
			float Relevance = 0.0f;
		};
		std::vector<Candidate> candidates;
		candidates.reserve(shadowed.size());
		const std::span<const RenderData::LightRecord> records = lights.GetRecords();
		for (const uint32_t record : shadowed)
		{
			const glm::vec4 bounds = records[record].Bounds;
			if (!frustum.IsSphereVisible(glm::vec3(bounds), bounds.w))
				continue;
			const float distance = glm::length(glm::vec3(bounds) - camera.Position);
			float relevance = 0.0f;
			if (camera.Orthographic)
				relevance = bounds.w;
			else if (distance <= bounds.w)
				relevance = std::numeric_limits<float>::max();
			else
				relevance = bounds.w / (distance - bounds.w);
			candidates.push_back(Candidate { record, relevance });
		}
		std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
		{
			return a.Relevance != b.Relevance ? a.Relevance > b.Relevance : a.Record < b.Record;
		});
		const bool spread = m_Specification.SpreadLightingUpdates;
		const size_t kept = std::min<size_t>(candidates.size(), m_Specification.MaxShadowedLocalLights);
		const std::optional<float> firstLeftOut = kept < candidates.size() ? std::optional<float>(candidates[kept].Relevance) : std::nullopt;

		// The lights keep their state (tiles, views, sizes) by entity and type; those that left the ranks give their tiles back.
		m_LocalShadowScratch.clear();
		for (size_t rank = 0; rank < kept; rank++)
		{
			const uint32_t record = candidates[rank].Record;
			const entt::entity entity = lights.GetEntity(record);
			const uint32_t type = lights.GetType(record);
			auto previous = std::find_if(m_LocalShadowLights.begin(), m_LocalShadowLights.end(), [&](const LocalShadowLight& light)
			{
				return light.Entity == entity && light.Type == type;
			});
			if (previous != m_LocalShadowLights.end())
			{
				m_LocalShadowScratch.push_back(std::move(*previous));
				previous->Entity = entt::null; // Taken
			}
			else
			{
				m_LocalShadowScratch.emplace_back();
				m_LocalShadowScratch.back().Entity = entity;
				m_LocalShadowScratch.back().Type = type;
				m_LocalShadowScratch.back().ViewCount = type == RenderData::c_LightPoint ? RenderData::c_LocalShadowFaces : 1u;
			}
			LocalShadowLight& light = m_LocalShadowScratch.back();
			light.Record = record;
			light.Relevance = candidates[rank].Relevance;
			light.RankFade = 1.0f;
			if (spread && firstLeftOut && light.Relevance < std::numeric_limits<float>::max())
				light.RankFade = std::clamp((light.Relevance - *firstLeftOut) / (c_LocalShadowRankMargin * light.Relevance), 0.0f, 1.0f);
		}
		for (LocalShadowLight& light : m_LocalShadowLights)
		{
			if (light.Entity != entt::null)
				FreeLocalShadowTiles(light);
		}
		m_LocalShadowLights.swap(m_LocalShadowScratch);

		// Tile sizes: as large as the light looks (with hysteresis), stepped down from the least relevant light while they do not
		// fit; lights that do not fit even at the smallest size go without shadows.
		const uint32_t atlasSize = m_LocalShadowAllocator.GetAtlasSize();
		const uint32_t maxTile = std::min(c_MaxLocalShadowTile, atlasSize);
		const uint32_t minTile = std::min(c_MinLocalShadowTile, maxTile);
		const float viewportHeight = static_cast<float>(m_ViewportSize.y);
		std::vector<uint32_t> sizes;
		std::vector<uint32_t> tileCounts;
		for (LocalShadowLight& light : m_LocalShadowLights)
		{
			const glm::vec4 bounds = records[light.Record].Bounds;
			const float pixels = GetScreenFraction(camera, glm::vec3(bounds), bounds.w) * viewportHeight * 0.5f;
			uint32_t wanted = std::clamp(CeilPowerOfTwo(pixels), minTile, maxTile);
			if (light.WantedSize != 0 && pixels > static_cast<float>(light.WantedSize) * c_TileShrinkRatio
				&& pixels <= static_cast<float>(light.WantedSize) * c_TileGrowRatio)
				wanted = light.WantedSize;
			light.WantedSize = wanted;
			sizes.push_back(wanted);
			tileCounts.push_back(light.ViewCount);
		}
		const size_t fit = ShadowAtlasAllocator::FitTileSizes(sizes, tileCounts, atlasSize, minTile);
		while (m_LocalShadowLights.size() > fit)
		{
			FreeLocalShadowTiles(m_LocalShadowLights.back());
			m_LocalShadowLights.pop_back();
		}
		std::vector<uint32_t> allocationOrder;
		for (uint32_t index = 0; index < m_LocalShadowLights.size(); index++)
		{
			LocalShadowLight& light = m_LocalShadowLights[index];
			if (light.TileSize != sizes[index])
				FreeLocalShadowTiles(light);
			if (light.TileSize == 0)
				allocationOrder.push_back(index);
		}
		// New tiles largest first; when the free space is too fragmented for them, every light's tiles are laid out again from
		// an empty atlas (largest first, which always fits: ShadowAtlasAllocator).
		const auto bySize = [&sizes](uint32_t a, uint32_t b) { return sizes[a] != sizes[b] ? sizes[a] > sizes[b] : a < b; };
		std::sort(allocationOrder.begin(), allocationOrder.end(), bySize);
		const auto allocate = [&](const std::vector<uint32_t>& order)
		{
			for (const uint32_t index : order)
			{
				LocalShadowLight& light = m_LocalShadowLights[index];
				for (uint32_t view = 0; view < light.ViewCount; view++)
				{
					light.Tiles[view] = m_LocalShadowAllocator.Allocate(sizes[index]);
					if (!light.Tiles[view].IsValid())
						return false;
				}
				light.TileSize = sizes[index];
			}
			return true;
		};
		if (!allocate(allocationOrder))
		{
			m_LocalShadowAllocator.Clear();
			allocationOrder.clear();
			for (uint32_t index = 0; index < m_LocalShadowLights.size(); index++)
			{
				LocalShadowLight& light = m_LocalShadowLights[index];
				light.Tiles = {};
				light.TileSize = 0;
				for (LocalShadowView& view : light.Views)
					view.HasContent = false;
				allocationOrder.push_back(index);
			}
			std::sort(allocationOrder.begin(), allocationOrder.end(), bySize);
			const bool laidOut = allocate(allocationOrder);
			ST_CORE_VERIFY(laidOut, "SceneRenderer: fitted shadow tiles did not fit an empty atlas");
		}

		// Each view's key this frame.
		for (LocalShadowLight& light : m_LocalShadowLights)
		{
			const RenderData::LightRecord& record = records[light.Record];
			const LightShadowSettings& settings = lights.GetShadowSettings(light.Record);
			light.ShadowSettings = glm::vec3(settings.Bias, settings.NormalBias, settings.FilterRadius);
			const float range = record.PositionRange.w;
			// The guard band: the filter's reach (and one texel more) on every side of what the view must cover.
			const float guard = std::ceil(settings.FilterRadius) + 1.0f;
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
		const bool contentChanged = !(content == m_LocalShadowContent);
		m_LocalShadowContent = content;

		// The views whose content a proxy's change reaches: its bounds before (as followed) or after reach the light's range and
		// the view's frustum as the view was drawn.
		bool anyContent = false;
		for (LocalShadowLight& light : m_LocalShadowLights)
		{
			for (uint32_t view = 0; view < light.ViewCount; view++)
			{
				anyContent = anyContent || light.Views[view].HasContent;
				if (contentChanged)
					light.Views[view].Dirty = true;
			}
		}
		const auto dirtyReached = [this](const AABB& bounds)
		{
			if (!bounds.IsValid())
				return;
			for (LocalShadowLight& light : m_LocalShadowLights)
			{
				for (uint32_t index = 0; index < light.ViewCount; index++)
				{
					LocalShadowView& view = light.Views[index];
					if (!view.HasContent || view.Dirty)
						continue;
					// A margin of a hundredth of the range for the rounding of what the CPU or the GPU took.
					const float margin = view.DrawnKey.Range * 0.01f;
					const glm::vec3 boxMin = bounds.Min - margin;
					const glm::vec3 boxMax = bounds.Max + margin;
					if (BoxReachesSphere(boxMin, boxMax, view.DrawnKey.Position, view.DrawnKey.Range) && BoxInPlanes(boxMin, boxMax, view.DrawnPlanes))
						view.Dirty = true;
				}
			}
		};
		const auto dirtyAll = [this]()
		{
			for (LocalShadowLight& light : m_LocalShadowLights)
			{
				for (uint32_t index = 0; index < light.ViewCount; index++)
					light.Views[index].Dirty = true;
			}
		};
		// Bounds of proxies not followed are known not to have reached a view while every view with content was drawn since the
		// following started over (each drawing follows every proxy within its light's range).
		bool followed = true;
		for (const LocalShadowLight& light : m_LocalShadowLights)
		{
			for (uint32_t index = 0; index < light.ViewCount; index++)
				followed = followed && (!light.Views[index].HasContent || light.Views[index].DrawnEpoch == m_LocalShadowEpoch);
		}
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
			else
			{
				for (const uint32_t proxy : m_LocalShadowChanges)
				{
					const bool alive = proxy < renderScene.GetProxyCapacity() && renderScene.GetEntity(proxy) != entt::null;
					const AABB bounds = alive ? renderScene.GetWorldBounds(proxy) : AABB();
					auto it = m_LocalShadowBounds.find(proxy);
					if (it != m_LocalShadowBounds.end())
						dirtyReached(it->second);
					else if (!followed)
						dirtyAll();
					dirtyReached(bounds);
					// Followed while within a shadowed light's range.
					bool inRange = false;
					for (const LocalShadowLight& light : m_LocalShadowLights)
					{
						const glm::vec4 position = records[light.Record].PositionRange;
						inRange = inRange || (bounds.IsValid() && BoxReachesSphere(bounds.Min, bounds.Max, glm::vec3(position), position.w));
						for (uint32_t index = 0; index < light.ViewCount && !inRange; index++)
						{
							const LocalShadowView& view = light.Views[index];
							inRange = view.HasContent && bounds.IsValid() && BoxReachesSphere(bounds.Min, bounds.Max, view.DrawnKey.Position, view.DrawnKey.Range);
						}
					}
					if (inRange)
						m_LocalShadowBounds[proxy] = bounds;
					else if (it != m_LocalShadowBounds.end())
						m_LocalShadowBounds.erase(it);
				}
			}
		}
		m_LocalShadowProxyVersion = proxyVersion;
		if (m_LocalShadowBounds.size() > c_MaxLocalShadowBounds)
		{
			dirtyAll();
			m_LocalShadowBounds.clear();
			m_LocalShadowEpoch++;
		}

		// The views the frame draws: those not up to date (every view without the cache), the nearest lights' first, at most
		// LocalShadowViewsPerFrame of them where lighting updates are spread.
		const bool cache = m_Specification.CacheLocalShadows;
		const uint32_t budget = spread && cache ? std::max(m_Specification.LocalShadowViewsPerFrame, 1u) : std::numeric_limits<uint32_t>::max();
		std::vector<uint32_t> byDistance(m_LocalShadowLights.size());
		for (uint32_t index = 0; index < byDistance.size(); index++)
			byDistance[index] = index;
		std::vector<float> distances(m_LocalShadowLights.size());
		for (uint32_t index = 0; index < distances.size(); index++)
		{
			const glm::vec3 offset = glm::vec3(records[m_LocalShadowLights[index].Record].PositionRange) - camera.Position;
			distances[index] = glm::dot(offset, offset);
		}
		std::sort(byDistance.begin(), byDistance.end(), [&distances](uint32_t a, uint32_t b) { return distances[a] != distances[b] ? distances[a] < distances[b] : a < b; });
		uint32_t pending = 0;
		for (const uint32_t index : byDistance)
		{
			LocalShadowLight& light = m_LocalShadowLights[index];
			for (uint32_t face = 0; face < light.ViewCount; face++)
			{
				const LocalShadowView& view = light.Views[face];
				const bool upToDate = cache && view.HasContent && !view.Dirty && view.Key == view.DrawnKey;
				if (upToDate)
					continue;
				if (m_LocalShadowViews.size() >= budget)
				{
					pending++;
					continue;
				}
				LocalShadowDrawView drawView;
				drawView.Light = index;
				drawView.Face = face;
				m_LocalShadowViews.push_back(drawView);
			}
		}

		// A light casts its shadow once every view holds content (drawn now, or kept from before, possibly from where it was);
		// where lighting updates are spread it then fades in, and lights near the end of the ranks fade by their margin.
		std::vector<uint8_t> drawnNow(static_cast<size_t>(m_LocalShadowLights.size()) * RenderData::c_LocalShadowFaces, 0);
		for (const LocalShadowDrawView& view : m_LocalShadowViews)
			drawnNow[static_cast<size_t>(view.Light) * RenderData::c_LocalShadowFaces + view.Face] = 1;
		for (uint32_t index = 0; index < m_LocalShadowLights.size(); index++)
		{
			LocalShadowLight& light = m_LocalShadowLights[index];
			light.Complete = true;
			for (uint32_t face = 0; face < light.ViewCount; face++)
				light.Complete = light.Complete && (light.Views[face].HasContent || drawnNow[static_cast<size_t>(index) * RenderData::c_LocalShadowFaces + face] != 0);
			if (!light.Complete)
				light.Strength = 0.0f;
			else if (!spread)
				light.Strength = 1.0f;
			else
				light.Strength = std::min(static_cast<float>(light.FadeFrames + 1) / static_cast<float>(c_LocalShadowFadeFrames), 1.0f) * light.RankFade;
		}
		m_Stats.LocalShadowLights = static_cast<uint32_t>(m_LocalShadowLights.size());
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
			drawView.LOD = LODCriteria { m_Specification.LODThreshold, LODSelection::GetErrorThreshold(m_Specification.LODErrorPixels, height), height };
		}

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
				if (m_LocalShadowBounds.size() >= c_MaxLocalShadowBounds && m_LocalShadowBounds.find(proxy) == m_LocalShadowBounds.end())
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
					if (submeshIndex >= c_MaxKeySubmeshes || lod >= c_MaxKeyLODs)
					{
						skipped++;
						continue;
					}
					draws.push_back(SortItem { MakeLocalDrawKey((materialFlags & c_MaterialAlphaMask) != 0, mirrored, (materialFlags & c_MaterialDoubleSided) != 0, skinned,
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
				skipped, c_MaxKeySubmeshes, c_MaxKeyLODs));
	}

	void SceneRenderer::BuildLocalShadowData(const SceneCamera& camera)
	{
		m_LocalShadowData.resize(m_LocalShadowLights.size());
		if (m_LocalShadowLights.empty())
			return;
		const glm::dvec3 origin(camera.Position);
		const float atlasSize = static_cast<float>(m_LocalShadowAllocator.GetAtlasSize());
		std::array<bool, RenderData::c_MaxShadowedLocalLights * RenderData::c_LocalShadowFaces> drawn = {};
		for (const LocalShadowDrawView& view : m_LocalShadowViews)
			drawn[view.Light * RenderData::c_LocalShadowFaces + view.Face] = true;
		for (uint32_t slot = 0; slot < m_LocalShadowLights.size(); slot++)
		{
			const LocalShadowLight& light = m_LocalShadowLights[slot];
			RenderData::LocalShadowData& data = m_LocalShadowData[slot];
			data = {};
			for (uint32_t face = 0; face < light.ViewCount; face++)
			{
				// What the tile holds: drawn now with its key, else what it was drawn with.
				const LocalShadowView& view = light.Views[face];
				const LocalShadowViewKey& key = drawn[slot * RenderData::c_LocalShadowFaces + face] || !view.HasContent ? view.Key : view.DrawnKey;
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
		nvrhi::FramebufferHandle framebuffer = atlas ? m_Device->createFramebuffer(nvrhi::FramebufferDesc().setDepthAttachment(atlas)) : nullptr;
		if (!atlas || !framebuffer)
		{
			m_Errors.Report(fmt::format("failed to create the {0}x{0} shadow atlas of point and spot lights; they cast no shadows", desc.width));
			return false;
		}
		m_LocalShadowAtlas = std::move(atlas);
		m_LocalShadowFramebuffer = std::move(framebuffer);
		m_LocalShadowAtlasCleared = false;
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
		std::array<bool, RenderData::c_MaxShadowedLocalLights * RenderData::c_LocalShadowFaces> drawn = {};
		for (const LocalShadowDrawView& view : m_LocalShadowViews)
			drawn[view.Light * RenderData::c_LocalShadowFaces + view.Face] = true;
		for (uint32_t slot = 0; slot < m_LocalShadowLights.size(); slot++)
		{
			LocalShadowLight& light = m_LocalShadowLights[slot];
			for (uint32_t face = 0; face < light.ViewCount; face++)
			{
				if (!drawn[slot * RenderData::c_LocalShadowFaces + face])
					continue;
				LocalShadowView& view = light.Views[face];
				view.DrawnKey = view.Key;
				view.DrawnPlanes = Frustum(GetLocalShadowViewProjection(view.Key, glm::dvec3(0.0))).GetPlanes();
				view.DrawnEpoch = m_LocalShadowDrawEpoch;
				view.HasContent = true;
				view.Dirty = false;
			}
			light.FadeFrames = light.Complete ? std::min(light.FadeFrames + 1, c_LocalShadowFadeFrames) : 0u;
		}
	}

}
