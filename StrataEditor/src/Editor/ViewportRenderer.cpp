#include "Editor/ViewportRenderer.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorViewport.h"
#include "Editor/SceneEdit.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Renderer/SceneGizmos.h>
#include <Strata/Renderer/Texture.h>
#include <Strata/Renderer/TextureStreamer.h>
#include <Strata/Scene/Scene.h>

#include <algorithm>
#include <unordered_set>

namespace Strata
{

	std::optional<ViewportView> ResolveViewportView(EditorContext& context, ViewportCameraSource source, float aspectRatio, std::string* outError)
	{
		Scene& scene = *context.GetActiveScene();
		ViewportView view;
		const bool wantsSceneCamera = source == ViewportCameraSource::Scene || (source == ViewportCameraSource::Automatic && context.GetSceneState() == SceneState::Play);
		if (wantsSceneCamera)
		{
			if (Entity camera = scene.GetPrimaryCameraEntity())
			{
				view.Camera = SceneCamera::FromEntity(scene, camera, aspectRatio);
				view.FromScene = true;
				return view;
			}
			if (source == ViewportCameraSource::Scene)
			{
				if (outError)
					*outError = "The scene has no active entity with a Camera component marked Primary";
				return std::nullopt;
			}
			view.Notice = "The scene has no primary camera: showing the editor camera";
		}
		view.Camera = context.GetViewport().GetCamera().GetSceneCamera(aspectRatio);
		view.EditorOverlays = true;
		// The editor camera asked for by name is always a view for working on the scene (the Scene view while the game
		// plays); the automatic choice's fallback while playing shows the running game as it is.
		view.EditorView = source == ViewportCameraSource::Editor || context.GetSceneState() != SceneState::Play;
		return view;
	}

	SceneRenderOptions GetViewportRenderOptions(const ViewportView& view, const ViewportSettings& settings)
	{
		SceneRenderOptions options;
		options.PreviewEnvironment = view.EditorView && settings.PreviewLighting;
		options.DrawScreenSpaceText = !view.EditorView || settings.ShowGameUI;
		return options;
	}

	ViewportRenderer::ViewportRenderer(const std::string& debugName)
	{
		SceneRendererSpecification specification;
		specification.DebugName = debugName;
		m_Renderer = CreateScope<SceneRenderer>(specification);
	}

	void ViewportRenderer::UpdateSelectionSubtrees(EditorContext& context, const Ref<Scene>& scene)
	{
		if (m_OutlinedValid && m_OutlinedScene.lock() == scene && m_OutlinedHierarchyVersion == scene->GetHierarchyVersion()
			&& m_OutlinedSelection == context.GetSelection())
			return;

		// Descendants are outlined too: selecting a model's root shows the whole model.
		m_Outlined.clear();
		std::unordered_set<UUID> outlined;
		for (UUID selected : context.GetSelection())
		{
			for (UUID id : SceneEdit::CollectSubtree(*scene, selected))
			{
				if (outlined.insert(id).second)
					m_Outlined.push_back(scene->GetEntityByUUID(id));
			}
		}
		// By handle: the scene gizmos look colliders up in the list (SceneGizmoSettings::ColliderEntities).
		std::sort(m_Outlined.begin(), m_Outlined.end(), [](const Entity& a, const Entity& b) { return a.GetHandle() < b.GetHandle(); });
		m_OutlinedSelection = context.GetSelection();
		m_OutlinedScene = scene;
		m_OutlinedHierarchyVersion = scene->GetHierarchyVersion();
		m_OutlinedValid = true;
	}

	bool ViewportRenderer::Render(EditorContext& context, const glm::uvec2& size, const ViewportView& view, const ViewportSettings& settings, bool overlays)
	{
		const Ref<Scene>& scene = context.GetActiveScene();
		m_Renderer->SetViewportSize(size.x, size.y);
		if (size.x == 0 || size.y == 0)
			return false;

		SceneRenderOptions options = GetViewportRenderOptions(view, settings);
		m_DebugDraw.Clear();
		options.SelectionColor = settings.SelectionColor;
		if (overlays)
		{
			options.ShowGrid = settings.ShowGrid;
			// The selection's subtrees: outlined, and the entities whose colliders and light ranges are drawn.
			if (settings.ShowSelectionOutline || settings.ShowSceneGizmos)
				UpdateSelectionSubtrees(context, scene);
			if (settings.ShowSelectionOutline)
				options.SelectedEntities = m_Outlined;
			if (settings.ShowSceneGizmos)
			{
				// The shapes read the cached world transforms.
				scene->UpdateWorldTransforms();
				SceneGizmoSettings gizmos;
				gizmos.CameraAspectRatio = static_cast<float>(size.x) / static_cast<float>(size.y);
				gizmos.AllColliders = settings.AllColliders;
				gizmos.ColliderEntities = m_Outlined;
				// Light ranges only for the selection: the spheres of many lights would cover the scene.
				gizmos.AllLightRanges = false;
				gizmos.LightRangeEntities = m_Outlined;
				// Shapes out of view or far away are left out; small ones become icons.
				const glm::mat4 inverseView = glm::inverse(view.Camera.View);
				SceneGizmoView gizmoView;
				gizmoView.ViewProjection = view.Camera.Projection * view.Camera.View;
				gizmoView.Position = view.Camera.Position;
				gizmoView.Right = glm::vec3(inverseView[0]);
				gizmoView.Up = glm::vec3(inverseView[1]);
				gizmoView.VerticalFOV = view.Camera.VerticalFOV;
				gizmoView.Orthographic = view.Camera.Orthographic;
				gizmoView.ViewportHeight = static_cast<float>(size.y);
				gizmos.View = gizmoView;
				DrawSceneGizmos(m_DebugDraw, *scene, gizmos);
			}
		}
		// The debug view tints what is drawn and boxes what cannot be (meshes that are not there).
		m_StreamingView.Build(*scene, settings.DebugView, view.Camera, m_Renderer->GetSpecification().LODThreshold, settings.DebugColors, m_DebugDraw);
		options.EntityTints = m_StreamingView.GetTints();
		if (!m_DebugDraw.IsEmpty())
			options.DebugShapes = &m_DebugDraw;

		const bool rendered = m_Renderer->Render(*scene, view.Camera, nullptr, options);
		if (rendered)
			m_RenderedScene = scene;
		return rendered;
	}

	uint32_t ViewportRenderer::CountTexturesBelowWanted() const
	{
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		if (!manager)
			return 0;
		uint32_t count = 0;
		for (const VisibleTextureUse& use : m_Renderer->GetVisibleTextures())
		{
			// Looked up without requesting them: the frame that drew them did.
			const Ref<Asset> asset = manager->PeekLoadedAsset(use.Texture);
			if (!asset || asset->GetType() != AssetType::Texture)
				continue;
			// Level 0 is the finest: a resident level above the wanted one is coarser than the view needs.
			const uint32_t wanted = TextureStreamingPolicy::ComputeWantedLevel(use.Size, use.MipCount, use.UVScale, use.UVPixels);
			if (static_cast<const Texture&>(*asset).GetResidentLevel() > wanted)
				count++;
		}
		return count;
	}

}
