#include "Editor/EditorGameWindow.h"

#include <Strata/Core/Log.h>
#include <Strata/Input/Input.h>

#include <algorithm>

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

	}

	bool EditorGameWindow::SetWindowMode(GameWindowMode mode)
	{
		if (mode != GameWindowMode::Windowed && mode != GameWindowMode::Fullscreen && mode != GameWindowMode::Borderless)
			return false;
		m_Mode = mode;
		m_Version++;
		ST_INFO("The game asked for a {} window; play mode keeps the editor's window", GetModeName(mode));
		return true;
	}

	bool EditorGameWindow::SetResolution(uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0 || width > c_MaxResolution || height > c_MaxResolution)
			return false;
		m_Width = width;
		m_Height = height;
		m_Version++;
		ST_INFO("The game asked for a resolution of {} x {}; play mode renders at the game view's size", width, height);
		return true;
	}

	bool EditorGameWindow::SetVSync(bool enabled)
	{
		m_VSync = enabled;
		m_Version++;
		ST_INFO("The game asked for vsync {}; play mode keeps the editor's", enabled ? "on" : "off");
		return true;
	}

	GameWindowState EditorGameWindow::GetState()
	{
		GameWindowState state;
		state.Mode = m_Mode.value_or(GameWindowMode::Windowed);
		state.VSync = m_VSync.value_or(true);
		// The game renders into the game view: its size is the game's window size.
		const glm::vec2 size = Input::GetViewportSize();
		state.Width = static_cast<uint32_t>(std::max(size.x, 0.0f));
		state.Height = static_cast<uint32_t>(std::max(size.y, 0.0f));
		return state;
	}

	void EditorGameWindow::Clear()
	{
		if (!HasRequests())
			return;
		m_Mode.reset();
		m_Width.reset();
		m_Height.reset();
		m_VSync.reset();
		m_Version++;
	}

	bool EditorGameWindow::HasRequests() const
	{
		return m_Mode || m_Width || m_VSync;
	}

	nlohmann::json EditorGameWindow::Describe() const
	{
		if (!HasRequests())
			return nullptr;
		nlohmann::json requests = nlohmann::json::object();
		if (m_Mode)
			requests["mode"] = GetModeName(*m_Mode);
		if (m_Width && m_Height)
			requests["resolution"] = { *m_Width, *m_Height };
		if (m_VSync)
			requests["vsync"] = *m_VSync;
		requests["applied"] = false;
		return requests;
	}

	std::string EditorGameWindow::DescribeForPeople() const
	{
		if (!HasRequests())
			return {};
		std::string what;
		if (m_Mode)
			what = fmt::format("a {} window", GetModeName(*m_Mode));
		if (m_Width && m_Height)
			what += fmt::format("{}{} x {} pixels", what.empty() ? "" : " at ", *m_Width, *m_Height);
		if (m_VSync)
			what += fmt::format("{}vsync {}", what.empty() ? "" : " with ", *m_VSync ? "on" : "off");
		return fmt::format("The game asked for {}; play mode keeps the editor's window.", what);
	}

}
