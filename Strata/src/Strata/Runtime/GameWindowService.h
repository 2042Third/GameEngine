#pragma once

#include "Strata/Core/EngineServices.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>

namespace Strata
{

	class Window;

	// The game's window (IGameWindow) in a shipped game: StrataRuntime provides it while the game runs. Requests are kept
	// and applied by ApplyRequests, which the runtime calls once per frame after the game's update, so the window never
	// changes in the middle of a frame (a mode or size change resizes the swapchain at the next frame). Without a window
	// (headless) requests are accepted, logged and reflected in the state reported back. Main thread only.
	class GameWindowService final : public IGameWindow
	{
	public:
		// `window` may be null (headless); it must outlive the service.
		explicit GameWindowService(Window* window);

		bool SetWindowMode(GameWindowMode mode) override;
		bool SetResolution(uint32_t width, uint32_t height) override;
		bool SetVSync(bool enabled) override;
		GameWindowState GetState() override;

		// Applies the requests made since the last call (the latest of each kind). What the window already is changes nothing
		// and is not logged, so a game may apply its options every frame.
		void ApplyRequests();
		bool HasPendingRequests() const;
	private:
		Window* m_Window = nullptr;
		std::optional<GameWindowMode> m_PendingMode;
		std::optional<uint32_t> m_PendingWidth;
		std::optional<uint32_t> m_PendingHeight;
		std::optional<bool> m_PendingVSync;
		// The resolution applied last, and the framebuffer's size after the last change: the same resolution asked for again
		// is applied only when the window was resized since.
		std::optional<glm::uvec2> m_AppliedResolution;
		glm::uvec2 m_FramebufferAfterApply = glm::uvec2(0);
		GameWindowState m_HeadlessState; // What a headless game asked for
	};

}
