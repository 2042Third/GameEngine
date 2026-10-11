#include "stpch.h"
#include "Strata/Runtime/GameWindowService.h"

#include "Strata/Core/Log.h"
#include "Strata/Core/Window.h"

namespace Strata
{

	namespace
	{

		const char* GetModeName(GameWindowMode mode)
		{
			switch (mode)
			{
				case GameWindowMode::Windowed: return "windowed";
				case GameWindowMode::Fullscreen: return "fullscreen";
				case GameWindowMode::Borderless: return "borderless";
			}
			return "windowed";
		}

		WindowMode ToWindowMode(GameWindowMode mode)
		{
			switch (mode)
			{
				case GameWindowMode::Windowed: return WindowMode::Windowed;
				case GameWindowMode::Fullscreen: return WindowMode::Fullscreen;
				case GameWindowMode::Borderless: return WindowMode::Borderless;
			}
			return WindowMode::Windowed;
		}

		GameWindowMode ToGameWindowMode(WindowMode mode)
		{
			switch (mode)
			{
				case WindowMode::Windowed: return GameWindowMode::Windowed;
				case WindowMode::Fullscreen: return GameWindowMode::Fullscreen;
				case WindowMode::Borderless: return GameWindowMode::Borderless;
			}
			return GameWindowMode::Windowed;
		}

	}

	GameWindowService::GameWindowService(Window* window)
		: m_Window(window)
	{
	}

	bool GameWindowService::SetWindowMode(GameWindowMode mode)
	{
		if (mode != GameWindowMode::Windowed && mode != GameWindowMode::Fullscreen && mode != GameWindowMode::Borderless)
			return false;
		m_PendingMode = mode;
		return true;
	}

	bool GameWindowService::SetResolution(uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0 || width > c_MaxResolution || height > c_MaxResolution)
			return false;
		m_PendingWidth = width;
		m_PendingHeight = height;
		return true;
	}

	bool GameWindowService::SetVSync(bool enabled)
	{
		m_PendingVSync = enabled;
		return true;
	}

	GameWindowState GameWindowService::GetState()
	{
		if (!m_Window)
			return m_HeadlessState;
		GameWindowState state;
		state.Mode = ToGameWindowMode(m_Window->GetMode());
		const glm::uvec2 size = m_Window->GetFramebufferSize();
		state.Width = size.x;
		state.Height = size.y;
		state.VSync = m_Window->IsVSync();
		return state;
	}

	bool GameWindowService::HasPendingRequests() const
	{
		return m_PendingMode || m_PendingWidth || m_PendingVSync;
	}

	void GameWindowService::ApplyRequests()
	{
		if (!HasPendingRequests())
			return;

		if (!m_Window)
		{
			// Headless: nothing to change, but the game sees what it asked for.
			if (m_PendingMode)
				m_HeadlessState.Mode = *m_PendingMode;
			if (m_PendingWidth && m_PendingHeight)
			{
				m_HeadlessState.Width = *m_PendingWidth;
				m_HeadlessState.Height = *m_PendingHeight;
			}
			if (m_PendingVSync)
				m_HeadlessState.VSync = *m_PendingVSync;
			ST_CORE_INFO("Game window (headless, nothing to change): {} {}x{}, vsync {}", GetModeName(m_HeadlessState.Mode), m_HeadlessState.Width,
				m_HeadlessState.Height, m_HeadlessState.VSync ? "on" : "off");
		}
		else
		{
			// The resolution first: a window going fullscreen switches the display to it.
			if (m_PendingWidth && m_PendingHeight)
				m_Window->SetResolution(*m_PendingWidth, *m_PendingHeight);
			if (m_PendingMode)
				m_Window->SetMode(ToWindowMode(*m_PendingMode));
			if (m_PendingVSync)
				m_Window->SetVSync(*m_PendingVSync);
			const GameWindowState state = GetState();
			ST_CORE_INFO("Game window: {} {}x{}, vsync {}", GetModeName(state.Mode), state.Width, state.Height, state.VSync ? "on" : "off");
		}
		m_PendingMode.reset();
		m_PendingWidth.reset();
		m_PendingHeight.reset();
		m_PendingVSync.reset();
	}

}
