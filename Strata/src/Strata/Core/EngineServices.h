#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

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

	struct SettingsSection;

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
		// The display's scale of the game's pixels for people: 1 at 96 DPI, 2 on a Retina display (what UI scaled to a
		// physical size multiplies by: UICanvas ConstantPhysicalSize). 1 without a window.
		float ContentScale = 1.0f;
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

	//////////////////////////////////////////////////////////////////////////
	// The game's graphics options
	//////////////////////////////////////////////////////////////////////////

	// Where the value in effect of a graphics option comes from (the ABI's StrataScriptGraphicsOptionSource, same values).
	enum class GameGraphicsSource : uint8_t
	{
		Project = 0,    // The project's settings (Project Settings > Graphics Quality, the section Renderer.Quality)
		Player = 1,     // The player's choice (SetOption, or GraphicsOptions.json of an earlier session)
		CommandLine = 2 // The program's command line fixes it for the session (StrataRuntime --quality)
	};

	// The players' graphics options of the game a program runs (scripts' Graphics::Set, Get, Reset and Save): the fields of
	// the project settings section the renderer registers (Renderer.Quality), by name, ignoring case. The value every
	// renderer of the game follows is the project's, the player's choices over it and the command line's preset over both
	// (Docs/DeveloperGuide.md, "Quality settings", "Players' options"). The player's choices last for the session;
	// SaveOptions keeps them in the game's save directory, from which the program loads them when the game starts. Provided
	// by GameRuntime (exported games) and EditorContext (the editor's play mode, for the Game view). Main thread only.
	class IGameGraphics
	{
	public:
		virtual ~IGameGraphics() = default;

		// The section whose fields are the options, in field order; null when the program has none (a registry without the
		// renderer's section): every other function then reports nothing and refuses every change.
		virtual const SettingsSection* GetSection() const = 0;
		// The value in effect: an object with every field of the section at its version (a named preset's rows spelled
		// out). Valid until GetVersion changes.
		virtual const nlohmann::json& GetEffectiveValue() = 0;
		// Changes whenever the value in effect, a source or the player's choices change (the project's settings, a SetOption,
		// a reset, a load, the session's end); unique among the process's providers, so a renderer notices a provider
		// replaced by another.
		virtual uint64_t GetVersion() = 0;
		// Where the value in effect of an option comes from; nullopt for an unknown option.
		virtual std::optional<GameGraphicsSource> GetSource(std::string_view option) = 0;
		// The player's choices: an object of field names and values as the section stores them (what SaveOptions writes).
		// Valid until GetVersion changes.
		virtual const nlohmann::json& GetPlayerOptions() = 0;

		// Sets the player's choice of an option (its stored form: a choice's name, ignoring case, a number, a boolean): a row
		// that follows the preset switches the preset to Custom first, starting from the look in effect. False, changing
		// nothing, with the reason: an unknown option or a value the section refuses.
		virtual bool SetOption(std::string_view option, const nlohmann::json& value, std::string* outError = nullptr) = 0;
		// Removes the player's choice of an option ("": of every option; Preset: the preset and the rows that follow it, so
		// the project's quality applies again; a row that follows the preset under the player's Custom takes the project's
		// value for it). False for an unknown option.
		virtual bool ResetOptions(std::string_view option, std::string* outError = nullptr) = 0;
		// Writes the player's choices to <save directory>/GraphicsOptions.json, atomically (the disk is written before it
		// returns). False, with the reason, without a save directory or when the write fails; the choices stay.
		virtual bool SaveOptions(std::string* outError = nullptr) = 0;
	};

}
