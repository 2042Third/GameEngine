#include "Editor/ViewportRenderer.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorViewport.h"
#include "Editor/SceneEdit.h"

#include <Strata/Renderer/SceneGizmos.h>
#include <Strata/Scene/Scene.h>

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
		view.EditorView = context.GetSceneState() != SceneState::Play;
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

	bool ViewportRenderer::Render(EditorContext& context, const glm::uvec2& size, const ViewportView& view, const ViewportSettings& settings, bool overlays)
	{
		const Ref<Scene>& scene = context.GetActiveScene();
		m_Renderer->SetViewportSize(size.x, size.y);
		if (size.x == 0 || size.y == 0)
			return false;

		SceneRenderOptions options = GetViewportRenderOptions(view, settings);
		m_DebugDraw.Clear();
		m_Outlined.clear();
		if (overlays)
		{
			options.ShowGrid = settings.ShowGrid;
			if (settings.ShowSelectionOutline)
			{
				// Descendants are outlined too: selecting a model's root shows the whole model.
				std::unordered_set<UUID> outlined;
				for (UUID selected : context.GetSelection())
				{
					for (UUID id : SceneEdit::CollectSubtree(*scene, selected))
					{
						if (outlined.insert(id).second)
							m_Outlined.push_back(scene->GetEntityByUUID(id));
					}
				}
				options.SelectedEntities = m_Outlined;
			}
			if (settings.ShowSceneGizmos)
			{
				// The shapes read the cached world transforms.
				scene->UpdateWorldTransforms();
				SceneGizmoSettings gizmos;
				gizmos.CameraAspectRatio = static_cast<float>(size.x) / static_cast<float>(size.y);
				DrawSceneGizmos(m_DebugDraw, *scene, gizmos);
				options.DebugShapes = &m_DebugDraw;
			}
		}

		const bool rendered = m_Renderer->Render(*scene, view.Camera, nullptr, options);
		if (rendered)
			m_RenderedScene = scene;
		return rendered;
	}

}
