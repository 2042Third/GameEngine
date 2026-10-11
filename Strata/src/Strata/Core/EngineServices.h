#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

// Engine services: interfaces that a module or the running program offers the whole process, so that modules which may not
// include it (the layers of StrataTests/Architecture/Layers.json) can still use it. They are like scene services
// (Scene/SceneServices.h), but not tied to a running scene: the master volume of the audio output, for instance, outlives
// every scene.
//
// A module registers its providers in its registration function, while the composition root runs
// (Engine::RegisterBuiltinModules, on the main thread, before anything uses them), and they stay registered for the rest of
// the process. What belongs to the program that runs a game (its window, where it keeps save files: the editor's play mode,
// a shipped game) is provided for as long as that program runs it (ScopedProvider). Get returns null for an interface that
// nothing provides, which callers handle like a missing feature. Main thread only.
namespace Strata::EngineServices
{

	namespace Detail
	{
		template<typename T>
		inline T* s_Provider = nullptr;
	}

	// Makes `provider` the process's provider of interface T (replacing an earlier one); it must live until the process ends.
	template<typename T>
	void Register(T& provider)
	{
		Detail::s_Provider<T> = &provider;
	}

	template<typename T>
	T* Get()
	{
		return Detail::s_Provider<T>;
	}

	// Provides interface T while it lives, then restores the provider it replaced (unless another one replaced it meanwhile,
	// which then stays). Scopes nest and must end in the reverse order of their start: an editor that runs an exported game in
	// its own process (tests) hands the game's services back to the editor when the game ends.
	template<typename T>
	class ScopedProvider
	{
	public:
		explicit ScopedProvider(T& provider)
			: m_Provider(&provider), m_Previous(Get<T>())
		{
			Register<T>(provider);
		}

		~ScopedProvider()
		{
			if (Detail::s_Provider<T> == m_Provider)
				Detail::s_Provider<T> = m_Previous;
		}

		ScopedProvider(const ScopedProvider&) = delete;
		ScopedProvider& operator=(const ScopedProvider&) = delete;
	private:
		T* m_Provider;
		T* m_Previous;
	};

}

namespace Strata
{

	//////////////////////////////////////////////////////////////////////////
	// The game's window
	//////////////////////////////////////////////////////////////////////////

	enum class GameWindowMode : uint8_t
	{
		Windowed = 0,
		Fullscreen, // Exclusive: the display switches to the game's resolution
		Borderless  // A window covering the display at the desktop's resolution
	};

	struct GameWindowState
	{
		GameWindowMode Mode = GameWindowMode::Windowed;
		uint32_t Width = 0;  // Pixels of the image the game renders (the window's framebuffer; the editor's game view)
		uint32_t Height = 0;
		bool VSync = true;
	};

	// The window a game runs in, for the game's display options (scripts' Game::SetWindowMode, SetResolution, SetVSync,
	// GetWindowSize). Requests are checked at once and applied by the provider when it can (a shipped game: after the frame,
	// StrataRuntime's window; the editor records them for its play mode and leaves its own window alone; a headless game
	// accepts and reports them). Functions return false for requests they refuse (a resolution outside 1 to
	// c_MaxResolution pixels, an unknown mode).
	class IGameWindow
	{
	public:
		static constexpr uint32_t c_MaxResolution = 16384;

		virtual ~IGameWindow() = default;

		virtual bool SetWindowMode(GameWindowMode mode) = 0;
		// The size in pixels the game wants: its window's in windowed mode, the display mode's in fullscreen (borderless keeps
		// the desktop's).
		virtual bool SetResolution(uint32_t width, uint32_t height) = 0;
		virtual bool SetVSync(bool enabled) = 0;
		// The window as it is now (requests not applied yet are not included).
		virtual GameWindowState GetState() = 0;
	};

	//////////////////////////////////////////////////////////////////////////
	// Save files
	//////////////////////////////////////////////////////////////////////////

	// Where a game keeps its save files (scripts' Game::WriteSaveFile and friends, through Core/SaveFiles.h): a shipped game
	// under <user data>/<Game>/Saves, the editor's play mode under <project>/.strata/Saves (SaveDirectory).
	class IGameSaves
	{
	public:
		virtual ~IGameSaves() = default;

		// nullopt while there is none (the editor without a project).
		virtual std::optional<std::filesystem::path> GetSaveDirectory() = 0;
	};

}
