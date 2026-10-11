#include "Editor/StreamingView.h"

#include "Editor/SceneBounds.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Math/Frustum.h>
#include <Strata/Renderer/LODSelection.h>
#include <Strata/Renderer/Material.h>
#include <Strata/Renderer/Mesh.h>
#include <Strata/Renderer/RenderScene.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace Strata
{

	namespace
	{

		// The opaque color of a tint, for lines (which have no surface to blend over).
		glm::vec4 Opaque(const glm::vec4& color)
		{
			return glm::vec4(glm::vec3(color), 1.0f);
		}

	}

	const char* ViewportDebugViewToString(ViewportDebugView view)
	{
		switch (view)
		{
			case ViewportDebugView::Streaming: return "streaming";
			case ViewportDebugView::LOD:       return "lod";
			case ViewportDebugView::None:      break;
		}
		return "none";
	}

	std::optional<ViewportDebugView> ViewportDebugViewFromString(std::string_view name)
	{
		for (ViewportDebugView view : { ViewportDebugView::None, ViewportDebugView::Streaming, ViewportDebugView::LOD })
		{
			if (name == ViewportDebugViewToString(view))
				return view;
		}
		return std::nullopt;
	}

	void StreamingView::Build(Scene& scene, ViewportDebugView view, const SceneCamera& camera, float lodThreshold, const ViewportDebugColors& colors,
		DebugDraw& placeholders)
	{
		m_Tints.clear();
		m_Stats = {};
		if (view == ViewportDebugView::None || !AssetManager::HasActive())
			return;

		// The proxies as the renderer will draw them this frame (its own Sync then finds nothing new), from the cached world
		// transforms; the view frustum query is the renderer's culling.
		scene.UpdateWorldTransforms();
		RenderScene& renderScene = RenderScene::Get(scene);
		renderScene.Sync(AssetManager::GetActive());
		m_Visible.clear();
		renderScene.GetSpatialIndex().QueryFrustum(Frustum(camera.Projection * camera.View), m_Visible);
		m_MeshStatuses.assign(renderScene.GetMeshSlotCapacity(), AssetStatus::NotLookedUp);
		m_MaterialStatuses.assign(renderScene.GetMaterialSlotCapacity(), AssetStatus::NotLookedUp);
		if (view == ViewportDebugView::Streaming)
			BuildStreaming(scene, renderScene, colors, placeholders);
		else
			BuildLOD(scene, renderScene, camera, lodThreshold, colors);
		m_TextureStatuses.clear();
	}

	StreamingView::AssetStatus StreamingView::GetStatus(AssetHandle handle)
	{
		// As the renderer counts pending assets: registered ones on their way; unknown ones never arrive (fallbacks).
		if (!AssetManager::IsHandleValid(handle))
			return AssetStatus::Unknown;
		const AssetState state = AssetManager::GetAssetState(handle);
		return state == AssetState::Ready ? AssetStatus::Ready : (state == AssetState::Failed ? AssetStatus::Failed : AssetStatus::Pending);
	}

	StreamingView::AssetStatus StreamingView::GetMaterialStatus(const RenderScene& renderScene, uint32_t materialSlot)
	{
		AssetStatus& cached = m_MaterialStatuses[materialSlot];
		if (cached != AssetStatus::NotLookedUp)
			return cached;

		const AssetHandle material = renderScene.GetMaterialHandle(materialSlot);
		AssetStatus status = GetStatus(material);
		if (status == AssetStatus::Unknown)
		{
			status = AssetStatus::Ready; // Drawn with the default material, which is always there
		}
		else if (status == AssetStatus::Ready)
		{
			// Its textures, which the renderer replaces with fallbacks while they load.
			if (const Ref<Material> loaded = AssetManager::PeekLoadedAsset<Material>(material))
			{
				for (AssetHandle texture : loaded->GetTextureHandles())
				{
					auto [textureStatus, inserted] = m_TextureStatuses.try_emplace(texture, AssetStatus::Unknown);
					if (inserted)
						textureStatus->second = GetStatus(texture);
					if (textureStatus->second != AssetStatus::Unknown)
						status = std::max(status, textureStatus->second);
				}
			}
		}
		cached = status;
		return status;
	}

	void StreamingView::BuildStreaming(Scene& scene, const RenderScene& renderScene, const ViewportDebugColors& colors, DebugDraw& placeholders)
	{
		for (const uint32_t proxy : m_Visible)
		{
			const uint32_t meshSlot = renderScene.GetMeshSlot(proxy);
			AssetStatus& meshStatus = m_MeshStatuses[meshSlot];
			if (meshStatus == AssetStatus::NotLookedUp)
				meshStatus = GetStatus(renderScene.GetMeshHandle(meshSlot));
			if (meshStatus == AssetStatus::Unknown)
				continue;

			const Entity entity(renderScene.GetEntity(proxy), &scene);
			if (meshStatus != AssetStatus::Ready)
			{
				// Nothing is drawn for it, so nothing can be tinted: a box stands for the mesh, of the bounds the asset pipeline
				// recorded for it (those the renderer culls it with), or a unit box without them.
				const bool failed = meshStatus == AssetStatus::Failed;
				(failed ? m_Stats.FailedEntities : m_Stats.PendingEntities)++;
				if (m_Stats.PlaceholderBoxes < c_MaxPlaceholderBoxes)
				{
					const glm::vec4 color = Opaque(failed ? colors.Failed : colors.Pending);
					const glm::mat4 world = renderScene.GetWorldTransform(proxy);
					if ((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::MeshBounds) != 0)
					{
						const AABB& local = renderScene.GetLocalBounds(proxy);
						placeholders.Box(world * glm::translate(glm::mat4(1.0f), local.GetCenter()), local.GetExtents(), color);
					}
					else
					{
						placeholders.Box(world, glm::vec3(SceneBounds::c_PlaceholderExtent), color);
					}
					m_Stats.PlaceholderBoxes++;
				}
				continue;
			}

			// The materials the renderer draws the mesh with: the override, else each submesh's (the default where it has none).
			AssetStatus status = AssetStatus::Ready;
			const uint32_t overrideSlot = renderScene.GetOverrideMaterialSlot(proxy);
			if (overrideSlot != RenderScene::c_InvalidIndex)
			{
				status = GetMaterialStatus(renderScene, overrideSlot);
			}
			else if (renderScene.IsMeshLayoutKnown(meshSlot))
			{
				for (const uint32_t materialSlot : renderScene.GetSubmeshMaterialSlots(meshSlot))
					status = std::max(status, GetMaterialStatus(renderScene, materialSlot));
			}
			if (status == AssetStatus::Ready)
				continue;
			const bool failed = status == AssetStatus::Failed;
			(failed ? m_Stats.FailedEntities : m_Stats.PendingEntities)++;
			m_Tints.push_back(SceneEntityTint { entity, failed ? colors.Failed : colors.Pending });
		}
	}

	void StreamingView::BuildLOD(Scene& scene, const RenderScene& renderScene, const SceneCamera& camera, float lodThreshold, const ViewportDebugColors& colors)
	{
		for (const uint32_t proxy : m_Visible)
		{
			const uint32_t meshSlot = renderScene.GetMeshSlot(proxy);
			AssetStatus& meshStatus = m_MeshStatuses[meshSlot];
			if (meshStatus == AssetStatus::NotLookedUp)
				meshStatus = GetStatus(renderScene.GetMeshHandle(meshSlot));
			if (meshStatus != AssetStatus::Ready)
				continue;
			// Ready: the mesh's level counts per submesh, from the loaded object (peeked at, not requested).
			const Ref<Mesh> mesh = AssetManager::PeekLoadedAsset<Mesh>(renderScene.GetMeshHandle(meshSlot));
			if (!mesh)
				continue;

			// The renderer chooses a level per submesh; the entity shows the most simplified one it is drawn with.
			const AABB& bounds = renderScene.GetWorldBounds(proxy);
			const float screenFraction = LODSelection::GetScreenFraction(camera, bounds.GetCenter(), glm::length(bounds.GetExtents()));
			uint32_t lod = 0;
			for (const Submesh& submesh : mesh->GetSubmeshes())
				lod = std::max(lod, LODSelection::SelectLOD(screenFraction, lodThreshold, static_cast<uint32_t>(submesh.LODs.size())));
			const size_t level = std::min<size_t>(lod, colors.LOD.size() - 1);
			m_Stats.EntitiesPerLOD[level]++;
			m_Tints.push_back(SceneEntityTint { Entity(renderScene.GetEntity(proxy), &scene), colors.LOD[level] });
		}
	}

}
