#pragma once

#include "Editor/CellBoundsOverlay.h"
#include "Editor/StreamingView.h"

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
		// The editor camera while the game is not playing (editing or simulating): views for working on the scene, where
		// the viewport settings' preview lighting and hidden game UI apply. Views through the scene's camera and of the
		// running game show the scene as the game does.
		bool EditorView = false;
		std::string Notice;          // Shown over the image, e.g. when the scene has no camera to play from
		// Where the game's screen lies in the image (SceneRenderOptions::ScreenView): the whole image, unless the 2D canvas
		// mode zooms and pans it.
		SceneScreenView ScreenView;
		// The scene has no camera but screen-space UI to show (ResolveGameView): an empty black frame with the scene's UI over
		// it, as GameRenderer shows a menu scene without a camera.
		bool OverlayOnly = false;
	};

	enum class ViewportCameraSource : uint8_t
	{
		Automatic = 0, // The scene's primary camera in play mode (the editor camera when there is none), else the editor camera
		Editor,        // The editor camera, as an editor view also while playing (the Scene view)
		Scene          // The scene's primary camera; fails without one (the Game view)
	};

	// Chooses the camera for the editor's active scene: Automatic (captures) looks through the scene's primary camera in
	// play mode and uses the editor camera while editing and simulating; the panels pass Editor (the Scene view) or Scene
	// (the Game view). Nullopt (with an error) only for ViewportCameraSource::Scene without a camera.
	std::optional<ViewportView> ResolveViewportView(EditorContext& context, ViewportCameraSource source, float aspectRatio, std::string* outError = nullptr);

	class ViewportRenderer;

	// The Game view's view: the scene's primary camera (ViewportCameraSource::Scene); without one, when the scene has
	// screen-space UI (SceneRenderer::HasOverlayContent of `renderer`), a view of only that UI over black (OverlayOnly), as
	// GameRenderer shows a menu scene without a camera. Nullopt (with an error) for a scene with neither.
	std::optional<ViewportView> ResolveGameView(EditorContext& context, ViewportRenderer& renderer, float aspectRatio, std::string* outError = nullptr);
	// How a view shows the scene beyond the overlays: preview lighting and the game's screen-space UI, from the viewport
	// settings for editor views (ViewportView::EditorView), as the game shows them otherwise.
	SceneRenderOptions GetViewportRenderOptions(const ViewportView& view, const ViewportSettings& settings);

	// Frames of temporal anti-aliasing a single picture of a view settles over (SceneRendererStats::TemporalFrames): the
	// jitter's eight phases, after which the image is their mean, as the viewport's converged view shows it. Captures and the
	// project's picture render their still view again until then.
	constexpr uint32_t c_TemporalSettleFrames = SceneRenderer::c_TemporalJitterPhases;

	// What a view's last successful Render showed of the game: live captures (viewport.capture {live}) pair a view's frames
	// with the game's fixed steps.
	struct ViewportRenderStamp
	{
		uint64_t RenderCount = 0; // SceneRenderer::GetRenderCount after that Render (0: none yet)
		uint64_t PlaySession = 0; // EditorContext::GetPlaySession while the game ran, 0 while editing
		uint64_t FixedSteps = 0;  // EditorContext::GetFixedStepCount then: the steps the frame shows
	};

	// Renders the editor's active scene for the viewport or a capture, with the overlays the viewport settings enable:
	// the ground grid, the selection outline (selected entities and their descendants) and the light, camera and collider
	// shapes; editor views also get the settings' preview lighting and game UI (GetViewportRenderOptions). The settings'
	// debug view (StreamingView) colors every view, with or without overlays. Owns a SceneRenderer, so it needs an
	// initialized renderer. Main thread only.
	class ViewportRenderer
	{
	public:
		// `fades`: SceneRendererSpecification::FadeTextureLevels, FadeLevelsOfDetail and SpreadLightingUpdates (off for the
		// capture renderer, whose images wait for the levels they need and show them and every light's shadow at once; it also
		// draws local light shadows into a 2048 atlas).
		explicit ViewportRenderer(const std::string& debugName, bool fades = true);

		ViewportRenderer(const ViewportRenderer&) = delete;
		ViewportRenderer& operator=(const ViewportRenderer&) = delete;

		// Renders into the output texture at `size` (render targets are recreated only when the size changes). Returns
		// false when nothing was rendered: a zero size, or a failure the scene renderer logged.
		bool Render(EditorContext& context, const glm::uvec2& size, const ViewportView& view, const ViewportSettings& settings, bool overlays);

		// Display-ready RGBA8 image of the last Render (null before the first one or after a failed resize).
		nvrhi::ITexture* GetOutputTexture() const { return m_Renderer->GetOutputTexture(); }
		glm::uvec2 GetSize() const { return m_Renderer->GetViewportSize(); }
		const SceneRendererStats& GetStats() const { return m_Renderer->GetStats(); }
		// The GPU's side of the latest frame whose GPU work is known (SceneRenderer::GetGPUStats).
		const SceneRendererGPUStats& GetGPUStats() const { return m_Renderer->GetGPUStats(); }
		// The textures the last Render drew coarser than its view wants them: the level the texture streamer works out from
		// the view's report (TextureStreamingPolicy::ComputeWantedLevel), known from the first render on, while the streamer's
		// own wanted level follows a frame later. A texture counts as loaded with its mip tail, so a picture that must show
		// the view's detail (captures, the project's picture) renders again while this is not 0.
		uint32_t CountTexturesBelowWanted() const;
		// What the debug view of the last Render found (empty without one).
		const StreamingViewStats& GetDebugViewStats() const { return m_StreamingView.GetStats(); }
		SceneRenderer& GetSceneRenderer() { return *m_Renderer; }
		// Whether the renderer's overlay passes would draw screen-space content of the scene (its game UI).
		bool HasOverlayContent(Scene& scene) const { return m_Renderer->HasOverlayContent(scene); }
		// The scene of the last successful Render.
		const std::weak_ptr<Scene>& GetRenderedScene() const { return m_RenderedScene; }
		// The game's state the last successful Render showed.
		const ViewportRenderStamp& GetRenderStamp() const { return m_RenderStamp; }
		// Frees the render targets until the next Render, once the GPU has finished the work submitted with them
		// (Renderer::ReleaseDeferred).
		void ReleaseTargets() { m_Renderer->SetViewportSize(0, 0); }
		// The cell bounds of the last Render (ViewportSettings::CellBounds).
		const CellBoundsOverlay& GetCellBounds() const { return m_CellBounds; }
	private:
		// The world's cell squares around the camera (ViewportSettings::CellBounds): the running game's streamer while it
		// plays, the editor's world otherwise. Called only while the setting is on.
		void DrawCellBounds(EditorContext& context, const ViewportView& view);
		// The selected entities and their descendants (outlined, and whose colliders are drawn), collected again only when
		// the selection, the scene or its hierarchy changed.
		void UpdateSelectionSubtrees(EditorContext& context, const Ref<Scene>& scene);
		// Records a successful Render of the active scene (m_RenderedScene and m_RenderStamp).
		void OnRendered(EditorContext& context);
	private:
		Scope<SceneRenderer> m_Renderer;
		DebugDraw m_DebugDraw;
		StreamingView m_StreamingView;
		CellBoundsOverlay m_CellBounds;
		std::vector<Entity> m_Outlined;
		std::vector<UUID> m_OutlinedSelection;
		std::weak_ptr<Scene> m_OutlinedScene;
		uint64_t m_OutlinedHierarchyVersion = 0;
		bool m_OutlinedValid = false;
		std::weak_ptr<Scene> m_RenderedScene;
		ViewportRenderStamp m_RenderStamp;
		// What an OverlayOnly view renders under the scene's UI: nothing, with neutral post-processing (made on first use).
		Scope<Scene> m_EmptyScene;
	};

}
