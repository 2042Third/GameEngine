#pragma once

#include <Strata/Core/Base.h>
#include <Strata/Renderer/DebugDraw.h>
#include <Strata/Renderer/SceneRenderer.h>
#include <Strata/Scene/Entity.h>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	class EditorContext;
	class Scene;
	struct ViewportSettings;

	// The camera a viewport image is rendered from, chosen by ResolveViewportView.
	struct ViewportView
	{
		SceneCamera Camera;
		bool FromScene = false;      // The scene's primary camera (otherwise the editor camera)
		bool EditorOverlays = false; // Whether editor overlays and tools belong on this view by default
		std::string Notice;          // Shown over the image, e.g. when the scene has no camera to play from
	};

	enum class ViewportCameraSource : uint8_t
	{
		Automatic = 0, // The scene's primary camera in play mode (the editor camera when there is none), else the editor camera
		Editor,
		Scene          // The scene's primary camera; fails without one
	};

	// Chooses the camera for the editor's active scene: play mode looks through the scene's primary camera, editing and
	// simulating use the editor camera. Nullopt (with an error) only for ViewportCameraSource::Scene without a camera.
	std::optional<ViewportView> ResolveViewportView(EditorContext& context, ViewportCameraSource source, float aspectRatio, std::string* outError = nullptr);

	// Renders the editor's active scene for the viewport or a capture, with the overlays the viewport settings enable:
	// the ground grid, the selection outline (selected entities and their descendants) and the light, camera and collider
	// shapes. Owns a SceneRenderer, so it needs an initialized renderer. Main thread only.
	class ViewportRenderer
	{
	public:
		explicit ViewportRenderer(const std::string& debugName);

		ViewportRenderer(const ViewportRenderer&) = delete;
		ViewportRenderer& operator=(const ViewportRenderer&) = delete;

		// Renders into the output texture at `size` (render targets are recreated only when the size changes). Returns
		// false when nothing was rendered: a zero size, or a failure the scene renderer logged.
		bool Render(EditorContext& context, const glm::uvec2& size, const ViewportView& view, const ViewportSettings& settings, bool overlays);

		// Display-ready RGBA8 image of the last Render (null before the first one or after a failed resize).
		nvrhi::ITexture* GetOutputTexture() const { return m_Renderer->GetOutputTexture(); }
		glm::uvec2 GetSize() const { return m_Renderer->GetViewportSize(); }
		const SceneRendererStats& GetStats() const { return m_Renderer->GetStats(); }
		SceneRenderer& GetSceneRenderer() { return *m_Renderer; }
		// The scene of the last successful Render.
		const std::weak_ptr<Scene>& GetRenderedScene() const { return m_RenderedScene; }
		// Frees the render targets until the next Render (work already submitted keeps what it uses alive).
		void ReleaseTargets() { m_Renderer->SetViewportSize(0, 0); }
	private:
		Scope<SceneRenderer> m_Renderer;
		DebugDraw m_DebugDraw;
		std::vector<Entity> m_Outlined;
		std::weak_ptr<Scene> m_RenderedScene;
	};

}
