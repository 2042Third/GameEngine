#include "Editor/ViewportRenderer.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorWorld.h"
#include "Editor/EditorViewport.h"
#include "Editor/PlacementPreview.h"
#include "Editor/SceneEdit.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Math/Math.h>
#include <Strata/Renderer/RendererComponents.h>
#include <Strata/Renderer/SceneGizmos.h>
#include <Strata/Renderer/Texture.h>
#include <Strata/Renderer/TextureStreamer.h>
#include <Strata/Scene/Scene.h>
#include <Strata/World/WorldStreamingSystem.h>

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
				view.Lens = CameraLens::FromEntity(scene, camera);
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

	std::optional<ViewportView> ResolveSceneView(EditorContext& context, float aspectRatio)
	{
		const ViewportSettings& settings = context.GetViewport().GetSettings();
		if (settings.PilotGameCamera)
		{
			if (std::optional<ViewportView> view = ResolveViewportView(context, ViewportCameraSource::Scene, aspectRatio))
			{
				view->EditorOverlays = true;
				view->EditorView = true;
				return view;
			}
		}
		std::optional<ViewportView> view = ResolveViewportView(context, ViewportCameraSource::Editor, aspectRatio);
		if (view && settings.PilotGameCamera)
			view->Notice = "The scene has no primary camera: showing the editor camera";
		return view;
	}

	std::optional<ViewportView> ResolveGameView(EditorContext& context, ViewportRenderer& renderer, float aspectRatio, std::string* outError)
	{
		if (std::optional<ViewportView> view = ResolveViewportView(context, ViewportCameraSource::Scene, aspectRatio, outError))
			return view;
		if (!renderer.HasOverlayContent(*context.GetActiveScene()))
			return std::nullopt;
		if (outError)
			outError->clear();
		// GameRenderer's frame of a camera-less scene with UI: a default camera over nothing, cleared to black.
		ViewportView view;
		view.Camera.Projection = Math::PerspectiveReverseZ(view.Camera.VerticalFOV, aspectRatio, view.Camera.Near, view.Camera.Far);
		view.Camera.ClearColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		view.OverlayOnly = true;
		return view;
	}

	SceneRenderOptions GetViewportRenderOptions(const ViewportView& view, const ViewportSettings& settings)
	{
		SceneRenderOptions options;
		options.PreviewEnvironment = view.EditorView && settings.PreviewLighting;
		options.DrawScreenSpaceText = !view.EditorView || settings.ShowGameUI;
		options.ScreenView = view.ScreenView;
		return options;
	}

	namespace
	{

		constexpr std::pair<ViewportQuality, const char*> c_ViewportQualityNames[] = { { ViewportQuality::Project, "project" },
			{ ViewportQuality::Low, "low" }, { ViewportQuality::Medium, "medium" }, { ViewportQuality::High, "high" }, { ViewportQuality::Ultra, "ultra" } };

	}

	const char* ViewportQualityToString(ViewportQuality quality)
	{
		for (const auto& [value, name] : c_ViewportQualityNames)
		{
			if (value == quality)
				return name;
		}
		return "project";
	}

	std::optional<ViewportQuality> ViewportQualityFromString(std::string_view name)
	{
		for (const auto& [value, text] : c_ViewportQualityNames)
		{
			if (name == text)
				return value;
		}
		return std::nullopt;
	}

	RendererQualitySettings ResolveViewportQuality(ViewportQuality quality)
	{
		switch (quality)
		{
			case ViewportQuality::Low: return RendererQuality::GetPreset(RendererQualityPreset::Low);
			case ViewportQuality::Medium: return RendererQuality::GetPreset(RendererQualityPreset::Medium);
			case ViewportQuality::High: return RendererQuality::GetPreset(RendererQualityPreset::High);
			case ViewportQuality::Ultra: return RendererQuality::GetPreset(RendererQualityPreset::Ultra);
			case ViewportQuality::Project: break;
		}
		return RendererQuality::GetProjectQuality();
	}

	RendererQualitySettings GetSceneViewQuality(const ViewportView& view, ViewportQuality setting, const RendererQualitySettings& game)
	{
		return view.FromScene ? game : ResolveViewportQuality(setting);
	}

	ViewportRenderer::ViewportRenderer(const std::string& debugName, bool fades)
		: m_SingleImages(!fades)
	{
		SceneRendererSpecification specification;
		specification.DebugName = debugName;
		specification.FadeTextureLevels = fades;
		specification.FadeLevelsOfDetail = fades;
		// The capture renderer draws single pictures: every shadow view of every shadowed light in its first frame (no fades),
		// at the quality of the view it captures (its atlas is released with its targets after each picture).
		specification.SpreadLightingUpdates = fades;
		// Single pictures: the cascades' fit follows Strata 0.7.0's rules (no holds), so a capture is the same on any machine.
		specification.ShadowFitHolds = fades;
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

	bool ViewportRenderer::Render(EditorContext& context, const glm::uvec2& size, const ViewportView& view, const ViewportSettings& settings, bool overlays,
		const RendererQualitySettings& quality, const PlacementPreview* placement)
	{
		const Ref<Scene>& scene = context.GetActiveScene();
		m_Renderer->SetViewportSize(size.x, size.y);
		m_Renderer->SetQuality(quality);
		if (size.x == 0 || size.y == 0)
			return false;

		if (view.OverlayOnly)
		{
			// The scene's UI over an empty frame, as GameRenderer draws a scene without a camera (no overlays or debug view: there
			// is no scene to see).
			if (!m_EmptyScene)
			{
				m_EmptyScene = CreateScope<Scene>("Empty");
				PostProcessComponent& postProcess = m_EmptyScene->CreateEntity("PostProcess").AddComponent<PostProcessComponent>();
				postProcess.Tonemapper = TonemapOperator::None;
				postProcess.AutoExposure = false;
				postProcess.Exposure = 0.0f;
				postProcess.Vignette = 0.0f;
				postProcess.Bloom = false;
				postProcess.AmbientOcclusion = false;
				postProcess.AntiAliasing = false;
			}
			SceneRenderOptions options;
			options.OverlayScene = scene.get();
			options.ScreenView = view.ScreenView;
			const bool rendered = m_Renderer->Render(*m_EmptyScene, view.Camera, nullptr, options);
			if (rendered)
				OnRendered(context, view);
			return rendered;
		}

		SceneRenderOptions options = GetViewportRenderOptions(view, settings);
		// Only the game's camera blurs, while the game plays, in the live views: the editor camera never does (camera moves
		// made by editing are not the game's), and the capture renderer's single pictures never do (it renders a view again for
		// its settle frames while a running game moves).
		options.MotionBlur = !m_SingleImages && view.FromScene && context.IsPlaying();
		// Depth of field is a lens's, not a moment's: views and captures through a scene camera show it, playing or not.
		options.Lens = view.Lens;
		// A capture taken again is the same picture: its film grain keeps one pattern.
		if (m_SingleImages)
			options.GrainSeed = 0;
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
			if (settings.CellBounds)
				DrawCellBounds(context, view);
		}
		// The ghost of a dragged asset: lines over the finished image, like the gizmos (no history, no entity id).
		const bool ghost = overlays && placement && (placement->GetKind() == PlacementPreview::Kind::Place || placement->GetKind() == PlacementPreview::Kind::Material);
		if (ghost)
			placement->Draw(m_DebugDraw, settings.Placement);
		// The debug view tints what is drawn and boxes what cannot be (meshes that are not there).
		m_StreamingView.Build(*scene, settings.DebugView, view.Camera, m_Renderer->GetLODCriteria(), settings.DebugColors, m_DebugDraw);
		options.EntityTints = m_StreamingView.GetTints();
		if (ghost && placement->GetKind() == PlacementPreview::Kind::Material && placement->GetTarget())
		{
			// The material's target first: an entity's first tint wins over the debug view's.
			const std::span<const SceneEntityTint> debugTints = m_StreamingView.GetTints();
			m_PlacementTints.clear();
			m_PlacementTints.push_back(SceneEntityTint { placement->GetTarget(), settings.Placement.Tint });
			m_PlacementTints.insert(m_PlacementTints.end(), debugTints.begin(), debugTints.end());
			options.EntityTints = m_PlacementTints;
		}
		options.ShowMotionVectors = settings.DebugView == ViewportDebugView::Motion;
		options.ShowShadowView = settings.DebugView == ViewportDebugView::Shadows;
		options.ShadowViewColors = settings.DebugColors.ShadowRatio;
		options.LightingView = GetLightingView(settings.DebugView);
		options.LightingColors = settings.DebugColors.Lighting;
		if (!m_DebugDraw.IsEmpty())
			options.DebugShapes = &m_DebugDraw;

		const bool rendered = m_Renderer->Render(*scene, view.Camera, nullptr, options);
		if (rendered)
			OnRendered(context, view);
		return rendered;
	}

	void ViewportRenderer::OnRendered(EditorContext& context, const ViewportView& view)
	{
		m_RenderedScene = context.GetActiveScene();
		m_RenderStamp.RenderCount = m_Renderer->GetRenderCount();
		m_RenderStamp.PlaySession = context.IsPlaying() ? context.GetPlaySession() : 0;
		m_RenderStamp.FixedSteps = context.GetFixedStepCount();
		m_RenderStamp.FromScene = view.FromScene;
	}

	void ViewportRenderer::DrawCellBounds(EditorContext& context, const ViewportView& view)
	{
		const ViewportSettings& settings = context.GetViewport().GetSettings();
		if (context.IsPlaying())
		{
			const Ref<Scene>& scene = context.GetActiveScene();
			const WorldStreamingSystem* system = scene && scene->IsRunning() ? scene->GetSystem<WorldStreamingSystem>() : nullptr;
			if (system && system->GetStreamer())
				m_CellBounds.Draw(*system->GetStreamer(), view.Camera.Position, 1.0f, settings.DebugColors, m_DebugDraw);
			return;
		}
		if (const EditorWorld* world = context.GetWorld())
			m_CellBounds.Draw(world->GetStreamer(), view.Camera.Position, world->GetRadiusScale(), settings.DebugColors, m_DebugDraw);
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
