#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/BindlessTextureTable.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Renderer/Texture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

// What a frame draws, from the scene's render proxies (RenderScene): the proxies in the view frustum and the shadow
// cascades come from the spatial index; the meshes and materials they use are resolved once each, at the priority their
// use asks for; draws are gathered in parallel chunks (merged in chunk order, so the result is the same on any number of
// threads) and sorted with a radix sort on 64-bit keys into instanced batches. Instance data and material data live in
// buffers that persist between frames: only what changed is uploaded, and the draw lists (an instance index per draw)
// only where they differ from the previous frame's.

namespace Strata
{

	namespace
	{

		// How a frame uses a mesh or material slot (SceneRenderer::MarkMesh, MarkMaterial).
		constexpr uint32_t c_UseView = 1u << 0;     // Drawn: requested at Normal priority
		constexpr uint32_t c_UseHigh = 1u << 1;     // Drawn large on screen: requested at High priority
		constexpr uint32_t c_UsePrefetch = 1u << 2; // Within the prefetch distance: requested at Low priority
		constexpr uint32_t c_UseShadow = 1u << 3;   // Casts into a cascade from outside the view: drawn while resident
		constexpr uint32_t c_UseMask = 0xFu;
		// Mesh slots: the use of the mesh's submesh materials (by proxies without a material override), shifted by this.
		constexpr uint32_t c_SubmeshMaterialsShift = 4;

		// Material flags of the frame (SceneRenderer::m_MaterialFlags).
		constexpr uint32_t c_MaterialAvailable = 1u << 0; // Its data (or the default material's) is in the material buffer
		constexpr uint32_t c_MaterialDoubleSided = 1u << 1;
		constexpr uint32_t c_MaterialAlphaMask = 1u << 2;
		constexpr uint32_t c_MaterialTransparent = 1u << 3;

		// Shadow casters further than this toward the light than a cascade's bounding sphere are flattened onto the cascade's
		// near plane (ShadowMesh.vert) instead of extending its depth range.
		constexpr float c_MaxShadowCasterDistance = 100000.0f;

		// Proxies per chunk of the parallel extraction.
		constexpr uint32_t c_ExtractionChunkSize = 2048;
		// Changed instances this close together are uploaded in one range: fewer copies for a little more data.
		constexpr uint32_t c_UploadRangeGap = 16;

		// The sort key of a draw: pipeline state first (cascade, alpha mask, winding, culling), then mesh, submesh and level
		// of detail, which together make a batch; the material in the lowest bits only when batches are split by material.
		constexpr uint32_t c_KeyCascadeShift = 62;
		constexpr uint32_t c_KeyAlphaMaskShift = 61;
		constexpr uint32_t c_KeyMirroredShift = 60;
		constexpr uint32_t c_KeyDoubleSidedShift = 59;
		constexpr uint32_t c_KeyMeshShift = 39;
		constexpr uint32_t c_KeySubmeshShift = 23;
		constexpr uint32_t c_KeyLODShift = 18;
		constexpr uint32_t c_KeyMeshBits = 20;     // Mesh slots: at most one per entity (Scene::c_MaxEntities)
		constexpr uint32_t c_KeySubmeshBits = 16;
		constexpr uint32_t c_KeyLODBits = 5;
		constexpr uint32_t c_KeyMaterialBits = 18; // Material slots beyond this share keys: their batches merge, which is still correct
		constexpr uint32_t c_MaxKeySubmeshes = 1u << c_KeySubmeshBits;
		constexpr uint32_t c_MaxKeyLODs = 1u << c_KeyLODBits;

		uint64_t MakeDrawKey(uint32_t cascade, bool alphaMask, bool mirrored, bool doubleSided, uint32_t meshSlot, uint32_t submesh, uint32_t lod, uint32_t material)
		{
			return (static_cast<uint64_t>(cascade) << c_KeyCascadeShift) | (static_cast<uint64_t>(alphaMask) << c_KeyAlphaMaskShift)
				| (static_cast<uint64_t>(mirrored) << c_KeyMirroredShift) | (static_cast<uint64_t>(doubleSided) << c_KeyDoubleSidedShift)
				| (static_cast<uint64_t>(meshSlot) << c_KeyMeshShift) | (static_cast<uint64_t>(submesh) << c_KeySubmeshShift)
				| (static_cast<uint64_t>(lod) << c_KeyLODShift) | static_cast<uint64_t>(material & ((1u << c_KeyMaterialBits) - 1));
		}

		uint32_t PackTextureSlot(uint32_t slot, uint32_t samplerIndex)
		{
			return (slot & 0xFFFFFFu) | (samplerIndex << 24);
		}

		uint32_t GetSamplerIndex(const TextureSpecification& specification)
		{
			return static_cast<uint32_t>(specification.Filter) * 3 + static_cast<uint32_t>(specification.Wrap);
		}

		uint32_t GetMaterialFlags(const MaterialProperties& properties)
		{
			return c_MaterialAvailable | (properties.DoubleSided ? c_MaterialDoubleSided : 0u) | (properties.AlphaMode == MaterialAlphaMode::Mask ? c_MaterialAlphaMask : 0u)
				| (properties.AlphaMode == MaterialAlphaMode::Blend ? c_MaterialTransparent : 0u);
		}

		// Run-length lists: consecutive proxies mostly share meshes and materials.
		void AppendMark(std::vector<std::pair<uint32_t, uint32_t>>& marks, uint32_t slot, uint32_t usage)
		{
			if (!marks.empty() && marks.back().first == slot)
				marks.back().second |= usage;
			else
				marks.emplace_back(slot, usage);
		}

		void AppendPixelSize(std::vector<std::pair<uint32_t, float>>& sizes, uint32_t slot, float size)
		{
			if (!sizes.empty() && sizes.back().first == slot)
				sizes.back().second = std::max(sizes.back().second, size);
			else
				sizes.emplace_back(slot, size);
		}

		// Runs function(chunk) for every chunk in [0, count) on the job system's workers and the calling thread.
		template<typename Function>
		void ForEachChunk(uint32_t count, const Function& function)
		{
			JobSystem::ParallelFor(count, 1, [&function](uint32_t begin, uint32_t end)
			{
				for (uint32_t chunk = begin; chunk < end; chunk++)
					function(chunk);
			});
		}

		uint32_t ChunkCount(size_t items)
		{
			return static_cast<uint32_t>((items + c_ExtractionChunkSize - 1) / c_ExtractionChunkSize);
		}

		float MillisecondsSince(std::chrono::steady_clock::time_point start)
		{
			return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
		}

	}

	void SceneRenderer::PrepareExtraction(RenderScene& renderScene)
	{
		if (renderScene.GetId() != m_RenderSceneId)
		{
			// Another scene: its slots mean something else, and the GPU copies hold the previous scene's data.
			m_RenderSceneId = renderScene.GetId();
			m_MeshUsage.clear();
			m_FrameMeshes.clear();
			m_MaterialUsage.clear();
			m_MaterialFlags.clear();
			m_MaterialData.clear();
			m_MaterialUploaded.clear();
			m_MaterialPixelSizes.clear();
			m_MaterialUVScales.clear();
			m_ProxyStamps.clear();
			m_CasterStamps.clear();
			m_DrawInstances.clear();
			m_ShadowDrawInstances.clear();
			m_FullUpload = true;
		}
		else
		{
			// The meshes went with the previous frame (ReleaseFrameAssets).
			for (const uint32_t slot : m_UsedMeshSlots)
				m_MeshUsage[slot] = 0;
			for (const uint32_t slot : m_UsedMaterialSlots)
			{
				m_MaterialUsage[slot] = 0;
				m_MaterialFlags[slot] = 0;
				m_MaterialPixelSizes[slot] = 0.0f;
			}
		}
		m_UsedMeshSlots.clear();
		m_UsedMaterialSlots.clear();
		m_MeshUsage.resize(renderScene.GetMeshSlotCapacity(), 0);
		m_FrameMeshes.resize(renderScene.GetMeshSlotCapacity(), nullptr);
		EnsureMaterialSlots(renderScene.GetMaterialSlotCapacity());

		const uint32_t proxies = renderScene.GetProxyCapacity();
		m_ProxyStamps.resize(proxies, 0);
		m_CasterStamps.resize(proxies, 0);
		if (++m_FrameStamp == 0)
		{
			// The stamps wrapped around: old ones could match again.
			std::fill(m_ProxyStamps.begin(), m_ProxyStamps.end(), 0u);
			std::fill(m_CasterStamps.begin(), m_CasterStamps.end(), 0u);
			m_FrameStamp = 1;
		}
		m_VisibleMaterials.clear();
		m_DefaultMaterialData.reset();
		m_DefaultMaterialFlags = 0;
	}

	void SceneRenderer::ReleaseFrameAssets()
	{
		for (const uint32_t slot : m_UsedMeshSlots)
			m_FrameMeshes[slot] = nullptr;
		m_Assets.EndFrame();
	}

	void SceneRenderer::EnsureMaterialSlots(uint32_t count)
	{
		if (m_MaterialUsage.size() >= count)
			return;
		m_MaterialUsage.resize(count, 0);
		m_MaterialFlags.resize(count, 0);
		m_MaterialData.resize(count, RenderData::MaterialData {});
		m_MaterialUploaded.resize(count, 0);
		m_MaterialPixelSizes.resize(count, 0.0f);
		m_MaterialUVScales.resize(count, 1.0f);
	}

	void SceneRenderer::MarkMesh(uint32_t slot, uint32_t usage)
	{
		if (m_MeshUsage[slot] == 0)
			m_UsedMeshSlots.push_back(slot);
		m_MeshUsage[slot] |= usage;
	}

	void SceneRenderer::MarkMaterial(uint32_t slot, uint32_t usage)
	{
		if (m_MaterialUsage[slot] == 0)
			m_UsedMaterialSlots.push_back(slot);
		m_MaterialUsage[slot] |= usage;
	}

	void SceneRenderer::CullView(RenderScene& renderScene, const SceneCamera& camera, const Frustum& frustum)
	{
		ST_PROFILE_FUNCTION();
		const SpatialIndex& index = renderScene.GetSpatialIndex();
		m_VisibleProxies.clear();
		index.QueryFrustum(frustum, m_VisibleProxies);
		const uint32_t visible = static_cast<uint32_t>(m_VisibleProxies.size());
		m_Stats.VisibleProxies = visible;
		m_Stats.CulledInstances = renderScene.GetStats().IndexedProxies - visible;
		m_VisibleFractions.resize(visible);

		const uint32_t chunks = ChunkCount(visible);
		if (m_Chunks.size() < chunks)
			m_Chunks.resize(chunks);
		const float highFraction = m_Specification.HighPriorityScreenFraction;
		ForEachChunk(chunks, [&](uint32_t chunk)
		{
			ExtractionChunk& output = m_Chunks[chunk];
			output.MeshMarks.clear();
			output.MaterialMarks.clear();
			const uint32_t end = std::min(visible, (chunk + 1) * c_ExtractionChunkSize);
			for (uint32_t item = chunk * c_ExtractionChunkSize; item < end; item++)
			{
				const uint32_t proxy = m_VisibleProxies[item];
				m_ProxyStamps[proxy] = m_FrameStamp;
				const AABB& bounds = renderScene.GetWorldBounds(proxy);
				const float fraction = GetScreenFraction(camera, bounds.GetCenter(), glm::length(bounds.GetExtents()));
				m_VisibleFractions[item] = fraction;
				const uint32_t usage = fraction > highFraction ? (c_UseView | c_UseHigh) : c_UseView;
				const uint32_t overrideSlot = renderScene.GetOverrideMaterialSlot(proxy);
				AppendMark(output.MeshMarks, renderScene.GetMeshSlot(proxy), usage | (overrideSlot == RenderScene::c_InvalidIndex ? usage << c_SubmeshMaterialsShift : 0u));
				if (overrideSlot != RenderScene::c_InvalidIndex)
					AppendMark(output.MaterialMarks, overrideSlot, usage);
			}
		});
		for (uint32_t chunk = 0; chunk < chunks; chunk++)
		{
			for (const auto& [slot, usage] : m_Chunks[chunk].MeshMarks)
				MarkMesh(slot, usage);
			for (const auto& [slot, usage] : m_Chunks[chunk].MaterialMarks)
				MarkMaterial(slot, usage);
		}

		// A mesh whose bounds are unknown (never loaded, none recorded) may be anywhere, in view too, whatever the
		// placeholder bounds of its proxies say: it is requested as if in view while any of them is active, until it loads
		// and its layout gives them real bounds.
		for (const uint32_t slot : renderScene.GetMeshesWithoutBounds())
		{
			if (renderScene.GetIndexedUserCount(slot) == 0)
				continue;
			MarkMesh(slot, c_UseView);
			m_Stats.MeshesWithoutBounds++;
		}

		// Prefetch what is near the camera but outside the view.
		if (!(m_Specification.PrefetchDistance > 0.0f))
			return;
		m_QueryScratch.clear();
		index.QuerySphere(camera.Position, m_Specification.PrefetchDistance, m_QueryScratch);
		for (const uint32_t proxy : m_QueryScratch)
		{
			if (m_ProxyStamps[proxy] == m_FrameStamp)
				continue;
			const uint32_t overrideSlot = renderScene.GetOverrideMaterialSlot(proxy);
			MarkMesh(renderScene.GetMeshSlot(proxy), c_UsePrefetch | (overrideSlot == RenderScene::c_InvalidIndex ? c_UsePrefetch << c_SubmeshMaterialsShift : 0u));
			if (overrideSlot != RenderScene::c_InvalidIndex)
				MarkMaterial(overrideSlot, c_UsePrefetch);
		}
	}

	void SceneRenderer::FindShadowCasters(RenderScene& renderScene, const SceneCamera& camera, RenderData::FrameConstants& frame)
	{
		ST_PROFILE_FUNCTION();
		m_ShadowCasters.clear();
		m_CascadeCount = 0;
		frame.ShadowParams = glm::vec4(0.0f);
		if (!m_ShadowLight.Enabled)
			return;

		const uint32_t cascadeCount = m_Specification.ShadowCascades;
		const float nearClip = std::max(camera.Near, 0.01f);
		const float farClip = std::clamp(m_ShadowLight.Distance, nearClip + 0.1f, std::max(camera.Far, nearClip + 0.1f));
		const float aspect = static_cast<float>(m_ViewportSize.x) / static_cast<float>(m_ViewportSize.y);
		const float tanHalfFOV = camera.Orthographic ? 0.0f : std::tan(camera.VerticalFOV * 0.5f);
		const glm::mat4 inverseView = glm::inverse(camera.View);
		const glm::vec3 lightDirection = m_ShadowLight.Direction;
		const glm::vec3 up = std::abs(lightDirection.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
		m_LightView = glm::lookAt(glm::vec3(0.0f), lightDirection, up);
		const float mapSize = static_cast<float>(m_Specification.ShadowMapSize);

		// Each cascade's square covers the bounding sphere of a view frustum slice; its depth range is fitted to the casters
		// (GatherShadowDraws).
		float sliceNear = nearClip;
		for (uint32_t cascade = 0; cascade < cascadeCount; cascade++)
		{
			// Practical split scheme: a blend of logarithmic and uniform splits.
			const float fraction = static_cast<float>(cascade + 1) / static_cast<float>(cascadeCount);
			const float logarithmic = nearClip * std::pow(farClip / nearClip, fraction);
			const float uniform = nearClip + (farClip - nearClip) * fraction;
			const float sliceFar = 0.8f * logarithmic + 0.2f * uniform;

			// Bounding sphere of the view frustum slice: its size does not change when the camera rotates, which keeps the
			// shadow texel footprint (and thus the edges) stable.
			std::array<glm::vec3, 8> corners;
			size_t cornerIndex = 0;
			for (float distance : { sliceNear, sliceFar })
			{
				const float halfHeight = camera.Orthographic ? camera.VerticalFOV * 0.5f : distance * tanHalfFOV;
				const float halfWidth = halfHeight * aspect;
				for (float x : { -1.0f, 1.0f })
				{
					for (float y : { -1.0f, 1.0f })
						corners[cornerIndex++] = glm::vec3(inverseView * glm::vec4(x * halfWidth, y * halfHeight, -distance, 1.0f));
				}
			}
			glm::vec3 center(0.0f);
			for (const glm::vec3& corner : corners)
				center += corner;
			center /= 8.0f;
			float radius = 0.0f;
			for (const glm::vec3& corner : corners)
				radius = std::max(radius, glm::length(corner - center));
			radius = std::ceil(radius * 16.0f) / 16.0f;

			// Snap the cascade origin to whole texels so edges do not shimmer while the camera moves.
			const float worldPerTexel = 2.0f * radius / mapSize;
			glm::vec3 lightCenter = glm::vec3(m_LightView * glm::vec4(center, 1.0f));
			lightCenter.x = std::floor(lightCenter.x / worldPerTexel) * worldPerTexel;
			lightCenter.y = std::floor(lightCenter.y / worldPerTexel) * worldPerTexel;

			CascadeBox& box = m_Cascades[cascade];
			box.Min = glm::vec2(lightCenter) - radius;
			box.Max = glm::vec2(lightCenter) + radius;
			box.FarZ = lightCenter.z - radius;
			box.SphereNearZ = lightCenter.z + radius;
			box.WorldPerTexel = worldPerTexel;
			box.Radius = radius;
			frame.CascadeSplits[cascade] = sliceFar;
			sliceNear = sliceFar;
		}
		m_CascadeCount = cascadeCount;

		// A caster shadows the cascades whose square it overlaps and whose far side it does not lie beyond; casters closer to
		// the light than a cascade's sphere are kept (its depth range grows to include them), so each cascade's volume is open
		// toward the light. The index finds candidates by their world bounds, whose extent along any axis covers that of the
		// light-space bounds tested below, so no caster is missed.
		const SpatialIndex& index = renderScene.GetSpatialIndex();
		const glm::vec3 lightX(m_LightView[0][0], m_LightView[1][0], m_LightView[2][0]);
		const glm::vec3 lightY(m_LightView[0][1], m_LightView[1][1], m_LightView[2][1]);
		const glm::vec3 lightZ(m_LightView[0][2], m_LightView[1][2], m_LightView[2][2]);
		const glm::vec3 lightOffset(m_LightView[3]);
		for (uint32_t cascade = 0; cascade < cascadeCount; cascade++)
		{
			const CascadeBox& box = m_Cascades[cascade];
			const std::array<glm::vec4, 5> planes = {
				glm::vec4(lightX, lightOffset.x - box.Min.x), glm::vec4(-lightX, box.Max.x - lightOffset.x),
				glm::vec4(lightY, lightOffset.y - box.Min.y), glm::vec4(-lightY, box.Max.y - lightOffset.y),
				glm::vec4(lightZ, lightOffset.z - box.FarZ) };
			m_QueryScratch.clear();
			index.QueryPlanes(planes, m_QueryScratch);
			for (const uint32_t proxy : m_QueryScratch)
			{
				if (m_CasterStamps[proxy] == m_FrameStamp)
					continue;
				m_CasterStamps[proxy] = m_FrameStamp;
				m_ShadowCasters.push_back(ShadowCaster { proxy, 0, 0.0f });
			}
		}

		// The exact test, on the bounds of the mesh in light space.
		size_t kept = 0;
		for (const ShadowCaster& candidate : m_ShadowCasters)
		{
			const uint32_t proxy = candidate.Proxy;
			const uint32_t flags = renderScene.GetProxyFlags(proxy);
			if ((flags & RenderProxyFlags::CastShadows) == 0 || (flags & RenderProxyFlags::MeshBounds) == 0 || renderScene.GetInstanceCount(proxy) == 0)
				continue;
			const AABB lightBounds = renderScene.GetLocalBounds(proxy).Transformed(m_LightView * renderScene.GetWorldTransform(proxy));
			uint32_t cascadeMask = 0;
			for (uint32_t cascade = 0; cascade < cascadeCount; cascade++)
			{
				const CascadeBox& box = m_Cascades[cascade];
				if (lightBounds.Max.x < box.Min.x || lightBounds.Min.x > box.Max.x || lightBounds.Max.y < box.Min.y || lightBounds.Min.y > box.Max.y
					|| lightBounds.Max.z < box.FarZ)
					continue;
				cascadeMask |= 1u << cascade;
			}
			if (cascadeMask == 0)
				continue;
			m_ShadowCasters[kept++] = ShadowCaster { proxy, cascadeMask, lightBounds.Max.z };
		}
		m_ShadowCasters.resize(kept);

		for (const ShadowCaster& caster : m_ShadowCasters)
		{
			const uint32_t overrideSlot = renderScene.GetOverrideMaterialSlot(caster.Proxy);
			MarkMesh(renderScene.GetMeshSlot(caster.Proxy), c_UseShadow | (overrideSlot == RenderScene::c_InvalidIndex ? c_UseShadow << c_SubmeshMaterialsShift : 0u));
			if (overrideSlot != RenderScene::c_InvalidIndex)
				MarkMaterial(overrideSlot, c_UseShadow);
		}
	}

	uint32_t SceneRenderer::ResolveTexture(AssetHandle handle, uint32_t missingSlot, uint32_t loadingSlot, bool residentOnly)
	{
		if (!handle.IsValid())
			return PackTextureSlot(missingSlot, 0);

		const Ref<Texture> texture = residentOnly ? m_Assets.FindResident<Texture>(handle) : m_Assets.Resolve<Texture>(handle, AssetPriority::Normal);
		if (!texture || texture->GetBindlessSlot() == BindlessTextureTable::c_InvalidSlot)
			return PackTextureSlot(loadingSlot, 0);
		return PackTextureSlot(texture->GetBindlessSlot(), GetSamplerIndex(texture->GetSpecification()));
	}

	RenderData::MaterialData SceneRenderer::BuildMaterialData(const Material& material, bool residentOnly)
	{
		const MaterialProperties& properties = material.GetProperties();
		RenderData::MaterialData data = {};
		data.BaseColor = properties.BaseColor;
		data.Emissive = glm::vec4(properties.EmissiveColor * properties.EmissiveIntensity, 0.0f);
		data.Metallic = properties.Metallic;
		data.Roughness = properties.Roughness;
		data.NormalScale = properties.NormalScale;
		data.OcclusionStrength = properties.OcclusionStrength;
		data.AlphaCutoff = properties.AlphaCutoff;
		data.Flags = (properties.AlphaMode == MaterialAlphaMode::Mask ? RenderData::c_MaterialAlphaMask : 0u)
			| (properties.AlphaMode == MaterialAlphaMode::Blend ? RenderData::c_MaterialAlphaBlend : 0u)
			| (properties.Unlit ? RenderData::c_MaterialUnlit : 0u)
			| (properties.DoubleSided ? RenderData::c_MaterialDoubleSided : 0u);
		data.UVTransform = glm::vec4(properties.UVTiling, properties.UVOffset);

		// Missing maps use neutral textures; maps still loading use neutral ones too, except emission (black until the map
		// arrives, so surfaces do not flash at full emissive strength).
		data.BaseColorMap = ResolveTexture(properties.BaseColorMap, BindlessTextureTable::c_WhiteSlot, BindlessTextureTable::c_WhiteSlot, residentOnly);
		data.MetallicRoughnessMap = ResolveTexture(properties.MetallicRoughnessMap, BindlessTextureTable::c_WhiteSlot, BindlessTextureTable::c_WhiteSlot, residentOnly);
		data.NormalMap = ResolveTexture(properties.NormalMap, BindlessTextureTable::c_FlatNormalSlot, BindlessTextureTable::c_FlatNormalSlot, residentOnly);
		data.OcclusionMap = ResolveTexture(properties.OcclusionMap, BindlessTextureTable::c_WhiteSlot, BindlessTextureTable::c_WhiteSlot, residentOnly);
		data.EmissiveMap = ResolveTexture(properties.EmissiveMap, BindlessTextureTable::c_WhiteSlot, BindlessTextureTable::c_BlackSlot, residentOnly);
		return data;
	}

	void SceneRenderer::ResolveAssets(RenderScene& renderScene)
	{
		ST_PROFILE_FUNCTION();
		// Slot order, so the requests (and the order loads are queued in) do not depend on the order of the proxies.
		std::sort(m_UsedMeshSlots.begin(), m_UsedMeshSlots.end());
		for (const uint32_t slot : m_UsedMeshSlots)
		{
			const uint32_t usage = m_MeshUsage[slot];
			const AssetHandle handle = renderScene.GetMeshHandle(slot);
			Ref<Mesh> mesh;
			if ((usage & (c_UseView | c_UseHigh)) != 0)
			{
				mesh = m_Assets.Resolve<Mesh>(handle, (usage & c_UseHigh) != 0 ? AssetPriority::High : AssetPriority::Normal);
			}
			else
			{
				if ((usage & c_UsePrefetch) != 0)
					m_Assets.Prefetch(handle);
				if ((usage & c_UseShadow) != 0)
					mesh = m_Assets.FindResident<Mesh>(handle);
			}
			// Held by the resolve cache until the frame ends; RenderScene::Sync keeps the mesh's layout current.
			m_FrameMeshes[slot] = mesh && mesh->GetIndexBuffer() ? mesh.get() : nullptr;

			const uint32_t submeshUsage = (usage >> c_SubmeshMaterialsShift) & c_UseMask;
			if (submeshUsage != 0 && renderScene.IsMeshLayoutKnown(slot))
			{
				for (const uint32_t materialSlot : renderScene.GetSubmeshMaterialSlots(slot))
					MarkMaterial(materialSlot, submeshUsage);
			}
		}

		std::sort(m_UsedMaterialSlots.begin(), m_UsedMaterialSlots.end());
		for (const uint32_t slot : m_UsedMaterialSlots)
		{
			const uint32_t usage = m_MaterialUsage[slot];
			const AssetHandle handle = renderScene.GetMaterialHandle(slot);
			const bool drawn = (usage & (c_UseView | c_UseHigh)) != 0;
			Ref<Material> material;
			if (drawn)
			{
				material = m_Assets.Resolve<Material>(handle, (usage & c_UseHigh) != 0 ? AssetPriority::High : AssetPriority::Normal);
			}
			else
			{
				if ((usage & c_UsePrefetch) != 0)
					m_Assets.Prefetch(handle);
				if ((usage & c_UseShadow) == 0)
					continue; // Prefetched only: nothing draws with it
				material = m_Assets.FindResident<Material>(handle);
			}

			RenderData::MaterialData data;
			uint32_t flags = 0;
			if (material)
			{
				data = BuildMaterialData(*material, !drawn);
				flags = GetMaterialFlags(material->GetProperties());
				m_MaterialUVScales[slot] = std::max(std::abs(material->GetProperties().UVTiling.x), std::abs(material->GetProperties().UVTiling.y));
			}
			else
			{
				// The default material stands in while the material loads (or when it never will).
				if (!m_DefaultMaterialData)
				{
					const Ref<Material> fallback = m_Assets.Resolve<Material>(BuiltinAssets::DefaultMaterial, AssetPriority::High);
					if (!fallback)
						continue; // No asset manager with built-in assets: nothing can be drawn with this material
					m_DefaultMaterialData = BuildMaterialData(*fallback, false);
					m_DefaultMaterialFlags = GetMaterialFlags(fallback->GetProperties());
				}
				data = *m_DefaultMaterialData;
				flags = m_DefaultMaterialFlags;
				m_MaterialUVScales[slot] = 1.0f;
			}
			m_MaterialFlags[slot] = flags;
			if (!m_MaterialUploaded[slot] || std::memcmp(&data, &m_MaterialData[slot], sizeof(data)) != 0)
			{
				m_MaterialData[slot] = data;
				m_MaterialUploaded[slot] = 0;
				m_DirtyMaterialSlots.push_back(slot);
			}
			if (drawn)
				m_Stats.Materials++;
		}
	}

	void SceneRenderer::BuildBatches(std::vector<SortItem>& draws, std::vector<SceneDrawBatch>& outBatches, std::vector<uint32_t>& outDrawList)
	{
		outBatches.clear();
		uint64_t batchKey = 0;
		for (const SortItem& draw : draws)
		{
			if (outBatches.empty() || draw.Key != batchKey)
			{
				batchKey = draw.Key;
				SceneDrawBatch batch;
				batch.MeshSlot = static_cast<uint32_t>((draw.Key >> c_KeyMeshShift) & ((1ull << c_KeyMeshBits) - 1));
				batch.SubmeshIndex = static_cast<uint32_t>((draw.Key >> c_KeySubmeshShift) & (c_MaxKeySubmeshes - 1));
				batch.LOD = static_cast<uint32_t>((draw.Key >> c_KeyLODShift) & (c_MaxKeyLODs - 1));
				batch.DoubleSided = ((draw.Key >> c_KeyDoubleSidedShift) & 1) != 0;
				batch.Mirrored = ((draw.Key >> c_KeyMirroredShift) & 1) != 0;
				batch.AlphaMask = ((draw.Key >> c_KeyAlphaMaskShift) & 1) != 0;
				batch.Cascade = static_cast<uint32_t>(draw.Key >> c_KeyCascadeShift);
				batch.FirstInstance = static_cast<uint32_t>(outDrawList.size());
				outBatches.push_back(batch);
			}
			outDrawList.push_back(draw.Value);
			outBatches.back().InstanceCount++;
		}
	}

	void SceneRenderer::GatherDraws(RenderScene& renderScene, const SceneCamera& camera)
	{
		ST_PROFILE_FUNCTION();
		const uint32_t visible = static_cast<uint32_t>(m_VisibleProxies.size());
		const uint32_t chunks = ChunkCount(visible);
		const std::span<const RenderData::InstanceData> instances = renderScene.GetInstances();
		const float viewportHeight = static_cast<float>(m_ViewportSize.y);
		const bool batchAcrossMaterials = m_Specification.BatchAcrossMaterials;
		ForEachChunk(chunks, [&](uint32_t chunk)
		{
			ExtractionChunk& output = m_Chunks[chunk];
			output.Draws.clear();
			output.Transparent.clear();
			output.MaterialPixelSizes.clear();
			output.InstancesPerLOD = {};
			output.Skipped = 0;
			const uint32_t end = std::min(visible, (chunk + 1) * c_ExtractionChunkSize);
			for (uint32_t item = chunk * c_ExtractionChunkSize; item < end; item++)
			{
				const uint32_t proxy = m_VisibleProxies[item];
				const uint32_t meshSlot = renderScene.GetMeshSlot(proxy);
				const Mesh* mesh = m_FrameMeshes[meshSlot];
				const uint32_t count = renderScene.GetInstanceCount(proxy);
				if (!mesh || count == 0 || mesh->GetSubmeshes().size() != count)
					continue;

				const AABB& bounds = renderScene.GetWorldBounds(proxy);
				const float fraction = m_VisibleFractions[item];
				const float viewDepth = -(camera.View * glm::vec4(bounds.GetCenter(), 1.0f)).z;
				const bool mirrored = (renderScene.GetProxyFlags(proxy) & RenderProxyFlags::Mirrored) != 0;
				const uint32_t first = renderScene.GetFirstInstance(proxy);
				const std::vector<Submesh>& submeshes = mesh->GetSubmeshes();
				for (uint32_t submeshIndex = 0; submeshIndex < count; submeshIndex++)
				{
					const uint32_t instance = first + submeshIndex;
					const uint32_t materialSlot = instances[instance].MaterialIndex;
					const uint32_t materialFlags = m_MaterialFlags[materialSlot];
					if ((materialFlags & c_MaterialAvailable) == 0)
						continue;
					const uint32_t lod = SelectLOD(submeshes[submeshIndex], fraction);
					if (submeshIndex >= c_MaxKeySubmeshes || lod >= c_MaxKeyLODs)
					{
						output.Skipped++;
						continue;
					}
					output.InstancesPerLOD[std::min<size_t>(lod, SceneRendererStats::c_LODStatCount - 1)]++;
					AppendPixelSize(output.MaterialPixelSizes, materialSlot, fraction * viewportHeight);
					const bool doubleSided = (materialFlags & c_MaterialDoubleSided) != 0;
					if ((materialFlags & c_MaterialTransparent) != 0)
					{
						output.Transparent.push_back(TransparentDraw { viewDepth, renderScene.GetEntityID(proxy), submeshIndex, instance, meshSlot, lod, doubleSided, mirrored });
						continue;
					}
					output.Draws.push_back(SortItem { MakeDrawKey(0, false, mirrored, doubleSided, meshSlot, submeshIndex, lod, batchAcrossMaterials ? 0u : materialSlot), instance, 0 });
				}
			}
		});

		m_Draws.clear();
		m_Transparent.clear();
		uint32_t skipped = 0;
		for (uint32_t chunk = 0; chunk < chunks; chunk++)
		{
			const ExtractionChunk& output = m_Chunks[chunk];
			m_Draws.insert(m_Draws.end(), output.Draws.begin(), output.Draws.end());
			m_Transparent.insert(m_Transparent.end(), output.Transparent.begin(), output.Transparent.end());
			for (size_t lod = 0; lod < output.InstancesPerLOD.size(); lod++)
				m_Stats.InstancesPerLOD[lod] += output.InstancesPerLOD[lod];
			for (const auto& [slot, size] : output.MaterialPixelSizes)
				m_MaterialPixelSizes[slot] = std::max(m_MaterialPixelSizes[slot], size);
			skipped += output.Skipped;
		}
		if (skipped > 0)
			m_Errors.Report(fmt::format("{} submesh instances were not drawn: more than {} submeshes or {} levels of detail in a mesh", skipped, c_MaxKeySubmeshes, c_MaxKeyLODs));

		const auto sortStart = std::chrono::steady_clock::now();
		RadixSort(m_Draws, m_SortScratch);
		// Back to front; equal depths by entity, then submesh, so the order does not depend on the proxies' order.
		std::sort(m_Transparent.begin(), m_Transparent.end(), [](const TransparentDraw& a, const TransparentDraw& b)
		{
			if (a.ViewDepth != b.ViewDepth)
				return a.ViewDepth > b.ViewDepth;
			return a.EntityID != b.EntityID ? a.EntityID < b.EntityID : a.SubmeshIndex < b.SubmeshIndex;
		});
		m_DrawInstancesBuild.clear();
		BuildBatches(m_Draws, m_OpaqueBatches, m_DrawInstancesBuild);
		m_TransparentBatches.clear();
		for (const TransparentDraw& draw : m_Transparent)
		{
			SceneDrawBatch batch;
			batch.MeshSlot = draw.MeshSlot;
			batch.SubmeshIndex = draw.SubmeshIndex;
			batch.LOD = draw.LOD;
			batch.DoubleSided = draw.DoubleSided;
			batch.Mirrored = draw.Mirrored;
			batch.FirstInstance = static_cast<uint32_t>(m_DrawInstancesBuild.size());
			batch.InstanceCount = 1;
			m_TransparentBatches.push_back(batch);
			m_DrawInstancesBuild.push_back(draw.Instance);
		}
		m_Stats.SortMs += MillisecondsSince(sortStart);
		m_Stats.Instances = static_cast<uint32_t>(m_DrawInstancesBuild.size());
		m_Stats.OpaqueDrawCalls = static_cast<uint32_t>(m_OpaqueBatches.size());

		for (const uint32_t slot : m_UsedMaterialSlots)
		{
			if (m_MaterialPixelSizes[slot] > 0.0f)
				m_VisibleMaterials.push_back(VisibleMaterialUse { renderScene.GetMaterialHandle(slot), m_MaterialPixelSizes[slot], m_MaterialUVScales[slot] });
		}
	}

	void SceneRenderer::GatherShadowDraws(RenderScene& renderScene, const SceneCamera& camera, RenderData::FrameConstants& frame)
	{
		ST_PROFILE_FUNCTION();
		m_ShadowDraws.clear();
		m_ShadowDrawInstancesBuild.clear();
		m_ShadowBatches.clear();
		if (m_CascadeCount == 0)
			return;

		const uint32_t cascadeCount = m_CascadeCount;
		const uint32_t casters = static_cast<uint32_t>(m_ShadowCasters.size());
		const uint32_t chunks = ChunkCount(casters);
		if (m_Chunks.size() < chunks)
			m_Chunks.resize(chunks);
		const std::span<const RenderData::InstanceData> instances = renderScene.GetInstances();
		const bool batchAcrossMaterials = m_Specification.BatchAcrossMaterials;
		ForEachChunk(chunks, [&](uint32_t chunk)
		{
			ExtractionChunk& output = m_Chunks[chunk];
			output.Draws.clear();
			output.Skipped = 0;
			for (uint32_t cascade = 0; cascade < cascadeCount; cascade++)
				output.CasterNearZ[cascade] = m_Cascades[cascade].SphereNearZ;
			const uint32_t end = std::min(casters, (chunk + 1) * c_ExtractionChunkSize);
			for (uint32_t item = chunk * c_ExtractionChunkSize; item < end; item++)
			{
				const ShadowCaster& caster = m_ShadowCasters[item];
				const uint32_t meshSlot = renderScene.GetMeshSlot(caster.Proxy);
				const Mesh* mesh = m_FrameMeshes[meshSlot];
				const uint32_t count = renderScene.GetInstanceCount(caster.Proxy);
				if (!mesh || count == 0 || mesh->GetSubmeshes().size() != count)
					continue;
				// Every drawn caster extends the depth range of its cascades toward the light.
				for (uint32_t cascade = 0; cascade < cascadeCount; cascade++)
				{
					if ((caster.CascadeMask & (1u << cascade)) != 0)
						output.CasterNearZ[cascade] = std::max(output.CasterNearZ[cascade], caster.LightNearZ);
				}

				const AABB& bounds = renderScene.GetWorldBounds(caster.Proxy);
				const float fraction = GetScreenFraction(camera, bounds.GetCenter(), glm::length(bounds.GetExtents()));
				const bool mirrored = (renderScene.GetProxyFlags(caster.Proxy) & RenderProxyFlags::Mirrored) != 0;
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
					const uint32_t lod = SelectLOD(submeshes[submeshIndex], fraction);
					if (submeshIndex >= c_MaxKeySubmeshes || lod >= c_MaxKeyLODs)
					{
						output.Skipped++;
						continue;
					}
					const bool alphaMask = (materialFlags & c_MaterialAlphaMask) != 0;
					const bool doubleSided = (materialFlags & c_MaterialDoubleSided) != 0;
					for (uint32_t cascade = 0; cascade < cascadeCount; cascade++)
					{
						if ((caster.CascadeMask & (1u << cascade)) != 0)
						{
							output.Draws.push_back(SortItem { MakeDrawKey(cascade, alphaMask, mirrored, doubleSided, meshSlot, submeshIndex, lod,
								batchAcrossMaterials ? 0u : materialSlot), instance, 0 });
						}
					}
				}
			}
		});

		std::array<float, RenderData::c_MaxCascades> casterNearZ = {};
		for (uint32_t cascade = 0; cascade < cascadeCount; cascade++)
			casterNearZ[cascade] = m_Cascades[cascade].SphereNearZ;
		uint32_t skipped = 0;
		for (uint32_t chunk = 0; chunk < chunks; chunk++)
		{
			const ExtractionChunk& output = m_Chunks[chunk];
			m_ShadowDraws.insert(m_ShadowDraws.end(), output.Draws.begin(), output.Draws.end());
			for (uint32_t cascade = 0; cascade < cascadeCount; cascade++)
				casterNearZ[cascade] = std::max(casterNearZ[cascade], output.CasterNearZ[cascade]);
			skipped += output.Skipped;
		}
		if (skipped > 0)
			m_Errors.Report(fmt::format("{} shadow caster instances were not drawn: more than {} submeshes or {} levels of detail in a mesh", skipped, c_MaxKeySubmeshes, c_MaxKeyLODs));

		const float mapSize = static_cast<float>(m_Specification.ShadowMapSize);
		for (uint32_t cascade = 0; cascade < cascadeCount; cascade++)
		{
			const CascadeBox& box = m_Cascades[cascade];
			// The depth range reaches from the far side of the slice's sphere to the nearest caster (up to a limit beyond which
			// ShadowMesh.vert flattens casters onto the near plane).
			const float nearZ = std::clamp(casterNearZ[cascade], box.SphereNearZ, box.SphereNearZ + c_MaxShadowCasterDistance);
			const float nearPlane = -nearZ;
			const float farPlane = -box.FarZ;
			const glm::mat4 projection = Math::OrthographicReverseZ(box.Min.x, box.Max.x, box.Min.y, box.Max.y, nearPlane, farPlane);
			frame.CascadeViewProjection[cascade] = projection * m_LightView;
			// Penumbrae are searched for blockers up to this distance from the receiver (world units).
			const float searchDistance = 0.2f * box.Radius + 5.0f;
			frame.CascadeData[cascade] = glm::vec4(box.WorldPerTexel, farPlane - nearPlane, searchDistance, 0.0f);
		}
		const float halfAngle = glm::radians(std::clamp(m_ShadowLight.Softness, 0.0f, 10.0f)) * 0.5f;
		frame.ShadowParams = glm::vec4(2.0f * std::tan(halfAngle), 1.0f / mapSize, 0.0f, static_cast<float>(cascadeCount));
		frame.ShadowBias = glm::vec4(m_ShadowLight.Bias, m_ShadowLight.NormalBias, 0.0f, 0.0f);

		const auto sortStart = std::chrono::steady_clock::now();
		RadixSort(m_ShadowDraws, m_SortScratch);
		BuildBatches(m_ShadowDraws, m_ShadowBatches, m_ShadowDrawInstancesBuild);
		m_Stats.SortMs += MillisecondsSince(sortStart);
		m_Stats.ShadowCasters = static_cast<uint32_t>(m_ShadowDrawInstancesBuild.size());
		m_Stats.ShadowDrawCalls = static_cast<uint32_t>(m_ShadowBatches.size());
	}

	uint64_t SceneRenderer::UploadDrawList(nvrhi::ICommandList* commandList, nvrhi::IBuffer* buffer, std::vector<uint32_t>& list, std::vector<uint32_t>& uploaded, bool full)
	{
		size_t first = 0;
		size_t last = list.size();
		if (!full)
		{
			// The range from the first to the last entry that differs from what the buffer holds.
			const size_t common = std::min(list.size(), uploaded.size());
			first = static_cast<size_t>(std::mismatch(list.begin(), list.begin() + static_cast<ptrdiff_t>(common), uploaded.begin()).first - list.begin());
			if (list.size() <= uploaded.size())
			{
				last = first;
				for (size_t index = common; index > first; index--)
				{
					if (list[index - 1] != uploaded[index - 1])
					{
						last = index;
						break;
					}
				}
			}
		}
		uint64_t bytes = 0;
		if (last > first)
		{
			bytes = (last - first) * sizeof(uint32_t);
			commandList->writeBuffer(buffer, list.data() + first, bytes, first * sizeof(uint32_t));
		}
		// The buffer now holds the new list; the old one is the scratch of the next frame's list.
		uploaded.swap(list);
		return bytes;
	}

	void SceneRenderer::UploadSceneData(nvrhi::ICommandList* commandList, const RenderScene& renderScene, bool instancesRecreated, bool materialsRecreated,
		bool drawListsRecreated)
	{
		ST_PROFILE_FUNCTION();
		const std::span<const RenderData::InstanceData> instances = renderScene.GetInstances();
		if (!instances.empty())
		{
			m_InstanceChanges.clear();
			const bool full = instancesRecreated || m_FullUpload || !renderScene.GetInstanceChanges(m_UploadedInstanceVersion, m_InstanceChanges);
			if (full)
			{
				commandList->writeBuffer(m_InstanceBuffer, instances.data(), instances.size_bytes());
				m_Stats.InstanceBytesUploaded += instances.size_bytes();
				m_Stats.InstanceUploadRanges++;
			}
			else if (!m_InstanceChanges.empty())
			{
				std::sort(m_InstanceChanges.begin(), m_InstanceChanges.end());
				size_t start = 0;
				for (size_t index = 1; index <= m_InstanceChanges.size(); index++)
				{
					if (index < m_InstanceChanges.size() && m_InstanceChanges[index] <= m_InstanceChanges[index - 1] + c_UploadRangeGap)
						continue;
					const uint32_t firstInstance = m_InstanceChanges[start];
					const uint32_t endInstance = m_InstanceChanges[index - 1] + 1;
					const size_t bytes = (endInstance - firstInstance) * sizeof(RenderData::InstanceData);
					commandList->writeBuffer(m_InstanceBuffer, instances.data() + firstInstance, bytes, firstInstance * sizeof(RenderData::InstanceData));
					m_Stats.InstanceBytesUploaded += bytes;
					m_Stats.InstanceUploadRanges++;
					start = index;
				}
			}
		}
		m_UploadedInstanceVersion = renderScene.GetInstanceVersion();

		if (materialsRecreated || m_FullUpload)
		{
			if (!m_MaterialData.empty())
			{
				commandList->writeBuffer(m_MaterialBuffer, m_MaterialData.data(), m_MaterialData.size() * sizeof(RenderData::MaterialData));
				m_Stats.MaterialBytesUploaded += m_MaterialData.size() * sizeof(RenderData::MaterialData);
			}
			std::fill(m_MaterialUploaded.begin(), m_MaterialUploaded.end(), uint8_t(1));
		}
		else if (!m_DirtyMaterialSlots.empty())
		{
			// A frame that failed before its upload leaves its dirty slots to the next one, which may list them again.
			std::sort(m_DirtyMaterialSlots.begin(), m_DirtyMaterialSlots.end());
			m_DirtyMaterialSlots.erase(std::unique(m_DirtyMaterialSlots.begin(), m_DirtyMaterialSlots.end()), m_DirtyMaterialSlots.end());
			size_t start = 0;
			for (size_t index = 1; index <= m_DirtyMaterialSlots.size(); index++)
			{
				if (index < m_DirtyMaterialSlots.size() && m_DirtyMaterialSlots[index] == m_DirtyMaterialSlots[index - 1] + 1)
					continue;
				const uint32_t firstSlot = m_DirtyMaterialSlots[start];
				const uint32_t endSlot = m_DirtyMaterialSlots[index - 1] + 1;
				const size_t bytes = (endSlot - firstSlot) * sizeof(RenderData::MaterialData);
				commandList->writeBuffer(m_MaterialBuffer, m_MaterialData.data() + firstSlot, bytes, firstSlot * sizeof(RenderData::MaterialData));
				m_Stats.MaterialBytesUploaded += bytes;
				for (uint32_t slot = firstSlot; slot < endSlot; slot++)
					m_MaterialUploaded[slot] = 1;
				start = index;
			}
		}
		m_DirtyMaterialSlots.clear();

		const bool fullLists = drawListsRecreated || m_FullUpload;
		m_Stats.InstanceBytesUploaded += UploadDrawList(commandList, m_DrawListBuffer, m_DrawInstancesBuild, m_DrawInstances, fullLists);
		m_Stats.InstanceBytesUploaded += UploadDrawList(commandList, m_ShadowDrawListBuffer, m_ShadowDrawInstancesBuild, m_ShadowDrawInstances, fullLists);
		m_FullUpload = false;
	}

}
