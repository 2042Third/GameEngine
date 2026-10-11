#pragma once

#include <Strata/Core/EngineServices.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace Strata
{

	// The game's window in the editor's play mode (IGameWindow, provided by EditorContext): the game plays in the viewport's
	// game view, so what it asks of its window (fullscreen, a resolution, vsync) is recorded and reported instead of
	// applied to the editor's window (editor.status, a notice over the game view). The game's view of its window is the
	// game view: GetState reports its size, with the mode and vsync the game asked for. Requests are forgotten when play
	// starts and stops (Clear). Main thread only.
	class EditorGameWindow final : public IGameWindow
	{
	public:
		bool SetWindowMode(GameWindowMode mode) override;
		bool SetResolution(uint32_t width, uint32_t height) override;
		bool SetVSync(bool enabled) override;
		GameWindowState GetState() override;

		void Clear();
		bool HasRequests() const;
		// Changes with every request and Clear, so that what shows them is rebuilt only then.
		uint64_t GetVersion() const { return m_Version; }
		// The requests as editor.status reports them ({"mode", "resolution": [w, h], "vsync"}, each only when asked for), or
		// null without any.
		nlohmann::json Describe() const;
		// One sentence for people: "The game asked for fullscreen at 1920 x 1080 with vsync off; play mode keeps the editor's
		// window." (empty without requests).
		std::string DescribeForPeople() const;
	private:
		std::optional<GameWindowMode> m_Mode;
		std::optional<uint32_t> m_Width;
		std::optional<uint32_t> m_Height;
		std::optional<bool> m_VSync;
		uint64_t m_Version = 0;
	};

}
