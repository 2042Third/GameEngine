#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/EngineServices.h"
#include "Strata/Renderer/RendererQuality.h"
#include "Strata/Renderer/SceneRenderer.h"
#include "Strata/Runtime/FramePerfLog.h"
#include "Strata/Scene/Entity.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Strata
{

	class Scene;

	// Renders a running game's scene into a target, typically the window's back buffer: from the scene's primary camera
	// (the first active entity with a Camera marked Primary), with its screen-space text and canvases as the HUD. A scene
	// without such a camera shows its screen-space UI over black when it has some (a main menu needs no camera), and
	// otherwise a message frame that names the problem (and one error in the log), so a broken game never shows a stale or
	// black window. Needs an initialized renderer. Main thread only.
	class GameRenderer
	{
	public:
		GameRenderer();
		~GameRenderer();

		GameRenderer(const GameRenderer&) = delete;
		GameRenderer& operator=(const GameRenderer&) = delete;

		// Renders into `target`, which must be `size` pixels with a non-sRGB UNORM color format (see SceneRenderer).
		// Returns false when nothing was rendered: no scene or target, a zero size (e.g. a minimized window) or a failure the
		// renderer logged.
		bool Render(const Ref<Scene>& scene, nvrhi::IFramebuffer* target, const glm::uvec2& size);
		// Renders a frame of plain black into `target`: what a game shows while it has no scene yet (its start scene loads
		// and it has no loading scene). False under the conditions of Render.
		bool RenderEmpty(nvrhi::IFramebuffer* target, const glm::uvec2& size);

		// The perf log's Render columns (StrataRuntime --perf-log): what the renderer's latest frame drew, the GPU-driven path's
		// counts as far as the GPU reported them (AddGPUCounts, usually two or three frames older): DrawCalls, Instances,
		// Triangles, VisibleProxies, OccludedProxies, InstanceUploadMB (instance data, draw lists and the GPU scene's proxy
		// records uploaded, MB of 2^20 bytes), LocalShadowLights and LocalShadowViewsDrawn (the point and spot lights with shadows
		// and their shadow views drawn in the frame). Empty fields without a renderer (null: a headless game) or before it rendered a frame. The renderer outlives
		// the log.
		static FramePerfColumnGroup MakePerfLogColumns(const GameRenderer* renderer);

		// The quality the game draws with: the game's graphics options while an IGameGraphics with a section is provided (the
		// project's Graphics Quality, the players' choices over it and StrataRuntime --quality over both: GameRuntime and the
		// editor's play mode provide it), else the project's Renderer.Quality section (RendererQuality::GetProjectQuality);
		// both followed every frame by their version. An override (tests and tools) wins over both; nullopt clears it.
		// `source` names the override in the log line each change of the preset or its source writes ("Graphics quality: Low
		// (the player's)", "Ultra (--quality)").
		void SetQualityOverride(std::optional<RendererQualitySettings> quality, std::string_view source = "override");
		const RendererQualitySettings& GetQuality() const { return m_Renderer->GetQuality(); }

		// Whether the last frame showed the missing-camera message.
		bool IsShowingMessage() const { return m_ShowingMessage; }
		const SceneRendererStats& GetStats() const { return m_Renderer->GetStats(); }
		// The scene renderer it draws with: no entity-ID buffer, and no output texture unless text is composed.
		const SceneRenderer& GetSceneRenderer() const { return *m_Renderer; }
		SceneRenderer& GetSceneRenderer() { return *m_Renderer; }
	private:
		// Applies the override, the game's or the project's quality to the renderer, after its SetViewportSize (Auto resolves
		// for the frame's rows).
		void ApplyQuality();
		// The provided graphics options' value in effect, made again only when their version changed (versions are unique
		// across providers, so a replaced provider is noticed too).
		const RendererQualitySettings& FollowQuality(IGameGraphics& graphics);
		void UpdateMessage(const Scene& scene, const glm::uvec2& size);
	private:
		Scope<SceneRenderer> m_Renderer;
		Scope<Scene> m_MessageScene;
		Entity m_MessageText;
		std::weak_ptr<Scene> m_LastScene;
		bool m_ShowingMessage = false;
		std::optional<RendererQualitySettings> m_QualityOverride;
		std::string m_QualitySource;
		uint64_t m_GraphicsVersion = 0; // Of the IGameGraphics value m_GraphicsQuality was made from
		RendererQualitySettings m_GraphicsQuality;
		GameGraphicsSource m_GraphicsPresetSource = GameGraphicsSource::Project;
		// What the log said last: the preset and its source (none: nothing logged since the override changed).
		std::optional<RendererQualityPreset> m_LoggedPreset;
		std::string m_LoggedSource;
	};

}
