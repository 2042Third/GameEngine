#include "Editor/StreamingView.h"

#include "Editor/SceneBounds.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Math/Frustum.h>
#include <Strata/Renderer/LODSelection.h>
#include <Strata/Renderer/Material.h>
#include <Strata/Renderer/Mesh.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>

#include <algorithm>
#include <cmath>

namespace Strata
{

	namespace
	{

		// The opaque color of a tint, for lines (which have no surface to blend over).
		glm::vec4 Opaque(const glm::vec4& color)
		{
			return glm::vec4(glm::vec3(color), 1.0f);
		}

		// Whether a world matrix draws anything: the renderer skips zero scales and non-finite matrices.
		bool IsDrawable(const glm::mat4& matrix)
		{
			const float determinant = glm::determinant(glm::mat3(matrix));
			if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12f)
				return false;
			for (int column = 0; column < 4; column++)
			{
				for (int row = 0; row < 4; row++)
				{
					if (!std::isfinite(matrix[column][row]))
						return false;
				}
			}
			return true;
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

		// Like the renderer, from the cached world transforms.
		scene.UpdateWorldTransforms();
		if (view == ViewportDebugView::Streaming)
			BuildStreaming(scene, camera, colors, placeholders);
		else
			BuildLOD(scene, camera, lodThreshold, colors);
		m_Statuses.clear();
		m_MaterialStatuses.clear();
	}

	StreamingView::AssetStatus StreamingView::GetStatus(AssetHandle handle)
	{
		const auto cached = m_Statuses.find(handle);
		if (cached != m_Statuses.end())
			return cached->second;

		// As the renderer counts pending assets: registered ones on their way; unknown ones never arrive (fallbacks).
		AssetStatus status = AssetStatus::Unknown;
		if (AssetManager::IsHandleValid(handle))
		{
			const AssetState state = AssetManager::GetAssetState(handle);
			status = state == AssetState::Ready ? AssetStatus::Ready : (state == AssetState::Failed ? AssetStatus::Failed : AssetStatus::Pending);
		}
		m_Statuses.emplace(handle, status);
		return status;
	}

	StreamingView::AssetStatus StreamingView::GetMaterialStatus(AssetHandle material)
	{
		const auto cached = m_MaterialStatuses.find(material);
		if (cached != m_MaterialStatuses.end())
			return cached->second;

		AssetStatus status = GetStatus(material);
		if (status == AssetStatus::Unknown)
		{
			status = AssetStatus::Ready; // Drawn with the default material, which is always there
		}
		else if (status == AssetStatus::Ready)
		{
			// Its textures, which the renderer replaces with fallbacks while they load.
			if (Ref<Material> loaded = AssetManager::GetAsset<Material>(material))
			{
				for (AssetHandle texture : loaded->GetTextureHandles())
				{
					const AssetStatus textureStatus = GetStatus(texture);
					if (textureStatus != AssetStatus::Unknown)
						status = std::max(status, textureStatus);
				}
			}
		}
		m_MaterialStatuses.emplace(material, status);
		return status;
	}

	void StreamingView::BuildStreaming(Scene& scene, const SceneCamera& camera, const ViewportDebugColors& colors, DebugDraw& placeholders)
	{
		const Frustum frustum(camera.Projection * camera.View);
		const AABB placeholderBox(glm::vec3(-SceneBounds::c_PlaceholderExtent), glm::vec3(SceneBounds::c_PlaceholderExtent));
		std::unordered_map<AssetHandle, Ref<Mesh>> meshes;
		entt::registry& registry = scene.GetRegistry();
		for (auto [handle, meshRenderer, world] : registry.view<MeshRendererComponent, WorldTransformComponent>().each())
		{
			if (!world.ActiveInHierarchy || !meshRenderer.Mesh.IsValid() || !IsDrawable(world.Matrix))
				continue;
			const AssetStatus meshStatus = GetStatus(meshRenderer.Mesh);
			if (meshStatus == AssetStatus::Unknown)
				continue;

			if (meshStatus != AssetStatus::Ready)
			{
				// Nothing is drawn for it, so nothing can be tinted: a box stands for the mesh.
				if (!frustum.IsAABBVisible(placeholderBox.Transformed(world.Matrix)))
					continue;
				const bool failed = meshStatus == AssetStatus::Failed;
				(failed ? m_Stats.FailedEntities : m_Stats.PendingEntities)++;
				if (m_Stats.PlaceholderBoxes < c_MaxPlaceholderBoxes)
				{
					placeholders.Box(world.Matrix, glm::vec3(SceneBounds::c_PlaceholderExtent), Opaque(failed ? colors.Failed : colors.Pending));
					m_Stats.PlaceholderBoxes++;
				}
				continue;
			}

			auto [cached, inserted] = meshes.try_emplace(meshRenderer.Mesh);
			if (inserted)
				cached->second = AssetManager::GetAsset<Mesh>(meshRenderer.Mesh);
			const Ref<Mesh>& mesh = cached->second;
			if (!mesh || !frustum.IsAABBVisible(mesh->GetBounds().Transformed(world.Matrix)))
				continue;

			AssetStatus status = AssetStatus::Ready;
			for (const Submesh& submesh : mesh->GetSubmeshes())
			{
				const AssetHandle material = meshRenderer.Material.IsValid() ? meshRenderer.Material
					: (submesh.Material.IsValid() ? submesh.Material : BuiltinAssets::DefaultMaterial);
				status = std::max(status, GetMaterialStatus(material));
			}
			if (status == AssetStatus::Ready)
				continue;
			const bool failed = status == AssetStatus::Failed;
			(failed ? m_Stats.FailedEntities : m_Stats.PendingEntities)++;
			m_Tints.push_back(SceneEntityTint { Entity(handle, &scene), failed ? colors.Failed : colors.Pending });
		}
	}

	void StreamingView::BuildLOD(Scene& scene, const SceneCamera& camera, float lodThreshold, const ViewportDebugColors& colors)
	{
		const Frustum frustum(camera.Projection * camera.View);
		std::unordered_map<AssetHandle, Ref<Mesh>> meshes;
		entt::registry& registry = scene.GetRegistry();
		for (auto [handle, meshRenderer, world] : registry.view<MeshRendererComponent, WorldTransformComponent>().each())
		{
			if (!world.ActiveInHierarchy || !meshRenderer.Mesh.IsValid() || !IsDrawable(world.Matrix) || GetStatus(meshRenderer.Mesh) != AssetStatus::Ready)
				continue;
			auto [cached, inserted] = meshes.try_emplace(meshRenderer.Mesh);
			if (inserted)
				cached->second = AssetManager::GetAsset<Mesh>(meshRenderer.Mesh);
			const Ref<Mesh>& mesh = cached->second;
			if (!mesh)
				continue;
			const AABB bounds = mesh->GetBounds().Transformed(world.Matrix);
			if (!frustum.IsAABBVisible(bounds))
				continue;

			// The renderer chooses a level per submesh; the entity shows the most simplified one it is drawn with.
			const float screenFraction = LODSelection::GetScreenFraction(camera, bounds.GetCenter(), glm::length(bounds.GetExtents()));
			uint32_t lod = 0;
			for (const Submesh& submesh : mesh->GetSubmeshes())
				lod = std::max(lod, LODSelection::SelectLOD(screenFraction, lodThreshold, static_cast<uint32_t>(submesh.LODs.size())));
			const size_t level = std::min<size_t>(lod, colors.LOD.size() - 1);
			m_Stats.EntitiesPerLOD[level]++;
			m_Tints.push_back(SceneEntityTint { Entity(handle, &scene), colors.LOD[level] });
		}
	}

}
