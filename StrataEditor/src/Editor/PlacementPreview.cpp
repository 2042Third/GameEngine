#include "Editor/PlacementPreview.h"

#include "Editor/AssetBounds.h"
#include "Editor/EditorContext.h"
#include "Editor/SceneBounds.h"
#include "Editor/SceneRaycast.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/Log.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/DebugDraw.h>
#include <Strata/Renderer/RenderScene.h>
#include <Strata/Renderer/SceneRenderer.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Strata
{

	namespace
	{

		// The marker that stands for a placement whose size is still loading: a ring this wide and an arrow this long.
		constexpr float c_PendingRingRadius = 0.25f;
		constexpr float c_PendingArrowLength = 0.5f;
		// The contact ring's radius: half the footprint across the normal, within these bounds (meters).
		constexpr float c_MinContactRadius = 0.1f;
		constexpr float c_MaxContactRadius = 5.0f;

		bool IsPlaceable(AssetType type)
		{
			return type == AssetType::Prefab || type == AssetType::Model || type == AssetType::Mesh;
		}

		std::string GetAssetName(const EditorAssetManager& assets, AssetHandle asset)
		{
			const std::optional<AssetMetadata> metadata = assets.GetMetadata(asset);
			if (!metadata)
				return "The asset";
			return metadata->Name.empty() ? metadata->Path : metadata->Name;
		}

		nlohmann::json ToJson(const glm::vec3& value)
		{
			return nlohmann::json::array({ value.x, value.y, value.z });
		}

	}

	PlacementColors PlacementColors::Default()
	{
		const glm::vec4 hit = SceneRenderOptions().SelectionColor;
		return PlacementColors { hit, glm::vec4(glm::vec3(hit), 0.5f), glm::vec4(glm::vec3(hit), 0.35f) };
	}

	void PlacementPreview::Update(EditorContext& context, const PlacementPointer& pointer)
	{
		EditorAssetManager* assets = context.GetAssetManager();
		const Ref<Scene>& scene = context.GetActiveScene();
		if (!assets || !scene)
		{
			Clear();
			return;
		}
		const AssetType type = assets->GetAssetType(pointer.Asset);
		const bool placeable = IsPlaceable(type);
		const bool castable = (placeable || type == AssetType::Material) && !pointer.CanvasMode && !context.IsSceneLoading();

		// Synced first (as the cast would), so that the updates counted next already hold what this frame's cast syncs: a
		// still pointer over a still scene never casts twice.
		RenderScene* renderScene = nullptr;
		if (castable)
		{
			renderScene = &RenderScene::Get(*scene);
			renderScene->Sync(assets->shared_from_this());
		}
		Key key;
		key.Asset = pointer.Asset;
		key.Origin = pointer.PointerRay.Origin;
		key.Direction = pointer.PointerRay.Direction;
		key.MaxDistance = pointer.MaxDistance;
		key.FallbackDistance = pointer.FallbackDistance;
		key.Snap = pointer.Snap;
		key.RenderScene = renderScene ? renderScene->GetId() : 0;
		key.RenderSceneUpdates = renderScene ? renderScene->GetStats().TotalUpdates : 0;
		key.ContentVersion = assets->GetContentVersion();
		key.SceneLoading = context.IsSceneLoading();
		key.CanvasMode = pointer.CanvasMode;
		key.Playing = context.IsPlaying();
		// What the drop needs stays the most recently requested, also while nothing changes.
		if (castable && placeable)
			assets->RequestLoad(pointer.Asset);
		if (m_Key && *m_Key == key)
			return;
		m_Key = key;
		m_Asset = pointer.Asset;
		m_Pointer = pointer;
		m_Placement.reset();
		m_SizePending = false;
		m_Target = Entity();
		m_TargetBounds = AABB();

		if (pointer.CanvasMode)
		{
			m_Kind = Kind::Refused;
			SetLabel("Leave the 2D canvas mode to place assets", LabelStyle::Warning);
			return;
		}
		if (context.IsSceneLoading())
		{
			m_Kind = Kind::Refused;
			SetLabel("The scene is being opened", LabelStyle::Warning);
			return;
		}
		if (!placeable && type != AssetType::Material)
		{
			m_Kind = Kind::Refused;
			SetLabel(fmt::format("Cannot place a {} here", AssetTypeToString(type)), LabelStyle::Warning);
			return;
		}

		m_CastCount++;
		const std::string name = GetAssetName(*assets, pointer.Asset);
		// While playing, a drop changes the running copy, which play.stop discards.
		const char* running = context.IsPlaying() ? " (the running game)" : "";
		if (placeable)
		{
			m_Kind = Kind::Place;
			const AssetBounds::Result measured = context.GetAssetBounds().Measure(*assets, pointer.Asset);
			const EditorPlacement::PlacementRequest request = EditorPlacement::MakeRayRequest(pointer.PointerRay.Origin, pointer.PointerRay.Direction,
				pointer.MaxDistance, pointer.FallbackDistance, pointer.Snap);
			m_Placement = EditorPlacement::Place(*scene, request, measured.Bounds);
			m_SizePending = measured.Pending;
			const char* pending = m_SizePending ? " (its size is loading)" : "";
			switch (m_Placement->Source)
			{
				case EditorPlacement::PlacementSource::Grid:
					SetLabel(fmt::format("{} on the grid (nothing under the pointer){}{}", name, pending, running), LabelStyle::Quiet);
					break;
				case EditorPlacement::PlacementSource::InFront:
					SetLabel(fmt::format("{} in front of the camera (nothing under the pointer){}{}", name, pending, running), LabelStyle::Quiet);
					break;
				default:
					SetLabel(fmt::format("{} on '{}'{}{}", name, m_Placement->HitEntity ? m_Placement->HitEntity.GetName() : std::string(), pending, running),
						LabelStyle::Normal);
					break;
			}
			return;
		}

		const std::optional<SceneRayHit> hit = SceneRaycast::Cast(*scene, pointer.PointerRay, pointer.MaxDistance);
		if (hit && hit->HitEntity.HasComponent<MeshRendererComponent>())
		{
			m_Kind = Kind::Material;
			m_Target = hit->HitEntity;
			m_TargetBounds = SceneBounds::GetEntityBounds(*scene, m_Target, false);
			SetLabel(fmt::format("{} onto '{}'{}", name, m_Target.GetName(), running), LabelStyle::Normal);
		}
		else
		{
			m_Kind = Kind::NoTarget;
			SetLabel("Drop a material onto a mesh", LabelStyle::Warning);
		}
	}

	void PlacementPreview::Clear()
	{
		m_Kind = Kind::None;
		m_Key.reset();
		m_Asset = UUID::Null();
		m_Placement.reset();
		m_SizePending = false;
		m_Target = Entity();
		m_TargetBounds = AABB();
		if (!m_Label.empty())
			SetLabel(std::string(), LabelStyle::Normal);
	}

	void PlacementPreview::SetLabel(std::string label, LabelStyle style)
	{
		if (label == m_Label && style == m_LabelStyle)
			return;
		m_Label = std::move(label);
		m_LabelStyle = style;
		m_LabelVersion++;
	}

	nlohmann::json PlacementPreview::MakeDropParameters() const
	{
		if (m_Kind != Kind::Place)
			return nullptr;
		// The pointer ray's floats unchanged: asset.place reads them back exactly (float to JSON double to float) and builds
		// the same request (MakeRayRequest), so the drop lands where the ghost stood.
		nlohmann::json parameters = {
			{ "asset", UUIDToJson(m_Asset) },
			{ "ray", { { "origin", ToJson(m_Pointer.PointerRay.Origin) }, { "direction", ToJson(m_Pointer.PointerRay.Direction) } } },
			{ "maxDistance", m_Pointer.MaxDistance },
			{ "fallbackDistance", m_Pointer.FallbackDistance } };
		if (m_Pointer.Snap)
			parameters["snap"] = *m_Pointer.Snap;
		return parameters;
	}

	void PlacementPreview::Draw(DebugDraw& draw, const PlacementColors& colors) const
	{
		if (m_Kind == Kind::Material)
		{
			if (m_TargetBounds.IsValid())
				draw.Box(m_TargetBounds, colors.Hit, DebugDrawDepth::Tested);
			return;
		}
		if (m_Kind != Kind::Place || !m_Placement)
			return;
		const EditorPlacement::Placement& placement = *m_Placement;
		const bool onSurface = placement.Source == EditorPlacement::PlacementSource::Surface || placement.Source == EditorPlacement::PlacementSource::Point;
		const glm::vec3& point = placement.RestingOn.Point;
		const glm::vec3& normal = placement.RestingOn.Normal;
		if (m_SizePending || !placement.PlacedBounds.IsValid())
		{
			const glm::vec4& color = onSurface ? colors.Hit : colors.Guess;
			draw.Circle(point, normal, c_PendingRingRadius, color, DebugDrawDepth::OnTop);
			draw.Arrow(point, point + normal * c_PendingArrowLength, color, DebugDrawDepth::OnTop);
			return;
		}
		const AABB box(placement.PlacedBounds.Min + placement.Translation, placement.PlacedBounds.Max + placement.Translation);
		if (!onSurface)
		{
			// A guess never looks like a hit: no contact ring, no tested box.
			draw.Box(box, colors.Guess, DebugDrawDepth::OnTop);
			return;
		}
		draw.Box(box, colors.Hit, DebugDrawDepth::Tested);
		draw.Box(box, glm::vec4(glm::vec3(colors.Hit), colors.Hit.a * c_HiddenEdgeAlpha), DebugDrawDepth::OnTop);
		// The ring where it touches: half its footprint across the normal's dominant axis.
		const glm::vec3 extents = placement.PlacedBounds.GetExtents();
		const glm::vec3 magnitude = glm::abs(normal);
		const int axis = magnitude.x > magnitude.y && magnitude.x > magnitude.z ? 0 : (magnitude.y >= magnitude.z ? 1 : 2);
		float footprint = 0.0f;
		for (int i = 0; i < 3; i++)
		{
			if (i != axis)
				footprint = std::max(footprint, extents[i]);
		}
		const float radius = std::min(std::max(footprint, c_MinContactRadius), c_MaxContactRadius);
		draw.Circle(point, normal, radius, colors.Hit, DebugDrawDepth::Tested);
	}

}
