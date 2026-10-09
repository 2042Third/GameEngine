#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Renderer/SceneRenderer.h"
#include "Strata/Scene/Entity.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <memory>
#include <string>

namespace Strata
{

	class Scene;

	// Renders a running game's scene into a target, typically the window's back buffer: from the scene's primary camera
	// (the first active entity with a Camera marked Primary), with its screen-space text as the HUD. A scene without such
	// a camera gets a message frame that names the problem instead (and one error in the log), so a broken game never
	// shows a stale or black window. Needs an initialized renderer. Main thread only.
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

		// Whether the last frame showed the missing-camera message.
		bool IsShowingMessage() const { return m_ShowingMessage; }
		const SceneRendererStats& GetStats() const { return m_Renderer->GetStats(); }
	private:
		void UpdateMessage(const Scene& scene, const glm::uvec2& size);
	private:
		Scope<SceneRenderer> m_Renderer;
		Scope<Scene> m_MessageScene;
		Entity m_MessageText;
		std::weak_ptr<Scene> m_LastScene;
		bool m_ShowingMessage = false;
	};

}
