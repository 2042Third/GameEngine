#include "stpch.h"
#include "Strata/Runtime/GameRenderer.h"

#include "Strata/Math/Math.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

namespace Strata
{

	namespace
	{

		// Background of the missing-camera message (sRGB), shown unchanged thanks to neutral post-processing.
		const glm::vec4 c_MessageBackground = { 0.16f, 0.05f, 0.05f, 1.0f };
		const glm::vec4 c_MessageColor = { 1.0f, 0.85f, 0.6f, 1.0f };

	}

	GameRenderer::GameRenderer()
	{
		SceneRendererSpecification specification;
		specification.DebugName = "Game";
		m_Renderer = CreateScope<SceneRenderer>(specification);

		// The message frame is a scene of its own: centered screen-space text over a plain background.
		m_MessageScene = CreateScope<Scene>("Message");
		PostProcessComponent& postProcess = m_MessageScene->CreateEntity("PostProcess").AddComponent<PostProcessComponent>();
		postProcess.Tonemapper = TonemapOperator::None;
		postProcess.AutoExposure = false;
		postProcess.Exposure = 0.0f;
		postProcess.Vignette = 0.0f;
		postProcess.Bloom = false;
		postProcess.AmbientOcclusion = false;
		postProcess.AntiAliasing = false;
		m_MessageText = m_MessageScene->CreateEntity("Message");
		TextComponent& text = m_MessageText.AddComponent<TextComponent>();
		text.ScreenSpace = true;
		text.ScreenAnchor = glm::vec2(0.5f, 0.5f);
		text.Alignment = TextAlignment::Center;
		text.Color = c_MessageColor;
	}

	GameRenderer::~GameRenderer() = default;

	bool GameRenderer::Render(const Ref<Scene>& scene, nvrhi::IFramebuffer* target, const glm::uvec2& size)
	{
		ST_PROFILE_FUNCTION();
		m_Renderer->SetViewportSize(size.x, size.y);
		if (!scene || !target || size.x == 0 || size.y == 0)
			return false;

		// Another scene (a level was loaded) starts at its own brightness instead of adapting from the previous one.
		if (m_LastScene.lock() != scene)
		{
			m_Renderer->ResetExposureAdaptation();
			m_LastScene = scene;
		}

		const float aspectRatio = static_cast<float>(size.x) / static_cast<float>(size.y);
		if (Entity camera = scene->GetPrimaryCameraEntity())
		{
			if (m_ShowingMessage)
			{
				ST_CORE_INFO("The scene '{}' has a primary camera again", scene->GetName());
				m_Renderer->ResetExposureAdaptation();
				m_ShowingMessage = false;
			}
			return m_Renderer->Render(*scene, SceneCamera::FromEntity(*scene, camera, aspectRatio), target);
		}

		if (!m_ShowingMessage)
		{
			ST_CORE_ERROR("The scene '{}' has no active entity with a Camera component marked Primary: showing a message instead", scene->GetName());
			m_ShowingMessage = true;
		}
		UpdateMessage(*scene, size);
		SceneCamera camera;
		camera.Projection = Math::PerspectiveReverseZ(camera.VerticalFOV, aspectRatio, camera.Near, camera.Far);
		camera.ClearColor = c_MessageBackground;
		return m_Renderer->Render(*m_MessageScene, camera, target);
	}

	void GameRenderer::UpdateMessage(const Scene& scene, const glm::uvec2& size)
	{
		TextComponent& text = m_MessageText.GetComponent<TextComponent>();
		text.Text = fmt::format("No camera to show\n\nThe scene '{}' has no active entity\nwith a Camera component marked Primary.", scene.GetName());
		// Readable at any window size.
		text.FontSize = std::clamp(static_cast<float>(size.y) / 24.0f, 12.0f, 64.0f);
	}

}
