#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/EngineServices.h"
#include "Strata/Renderer/RendererQuality.h"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class IProjectSettings;
	class SettingsRegistry;

	// The players' graphics options file, <IGameSaves directory>/GraphicsOptions.json:
	// {"Options": {"Preset": "Low"}, "Section": {"Id": "Renderer.Quality", "Version": n}, "Strata": {"Format":
	// "GraphicsOptions", "Version": 1}}, tab-indented with a final newline. Only GameGraphicsService reads, writes and
	// deletes it (Docs/DeveloperGuide.md, "Players' options").
	namespace GraphicsOptionsFile
	{
		constexpr const char* c_FileName = "GraphicsOptions.json";
		constexpr uint64_t c_MaxFileBytes = 64 * 1024;
		constexpr uint32_t c_FormatVersion = 1; // Of the envelope: a change of it raises this

		// Reads a file's text against the section and the project's value (valid at the section's version): the player's
		// choices at the section's version in outOptions (names in their fields' spelling), members the section no longer has
		// or refuses dropped and described in outDropped. False, with the reason, for text that is no options file at all
		// (too large, not JSON, another format, a newer format or section version, another section, no Options object, two
		// members naming one field).
		bool Read(std::string_view text, const SettingsSection& section, const nlohmann::json& projectValue, nlohmann::json& outOptions,
			std::vector<std::string>* outDropped = nullptr, std::string* outError = nullptr);
		// The file's text for these choices (an object of fields and values; anything else is written as no choice).
		std::string Write(const nlohmann::json& options, const SettingsSection& section);
	}

	// IGameGraphics for a program that runs a game: GameRuntime and EditorContext own one. `project` is the program's
	// project settings (null: none, the section's default applies) and `saves` where its game keeps save files (null:
	// nowhere, SaveOptions fails); both must outlive the service. The value in effect is pulled from them when asked (an
	// integer compare when nothing changed): no per-frame work. Main thread only.
	class GameGraphicsService final : public IGameGraphics
	{
	public:
		// The options are `registry`'s Renderer.Quality section (null: the process's registry) with the engine's rows and
		// presets (RendererQuality::GetOptionModel); without that section the service has no options (GetSection null).
		GameGraphicsService(const IProjectSettings* project, IGameSaves* saves, const SettingsRegistry* registry = nullptr);
		// Tests: any simple quality section with its rows and presets' values (RendererQuality::OptionModel; its section and
		// spans must outlive the service).
		GameGraphicsService(const IProjectSettings* project, IGameSaves* saves, const RendererQuality::OptionModel& model);
		~GameGraphicsService() override;

		GameGraphicsService(const GameGraphicsService&) = delete;
		GameGraphicsService& operator=(const GameGraphicsService&) = delete;

		// The preset StrataRuntime --quality fixes for the session (nullopt: none; Custom is no preset of a command line and
		// counts as none). Never saved.
		void SetCommandLinePreset(std::optional<RendererQualityPreset> preset);
		std::optional<RendererQualityPreset> GetCommandLinePreset() const;
		// Replaces the player's choices with those of GraphicsOptions.json in the save directory: a file that is no options
		// file is ignored with one warning (the project's quality applies), members it can no longer use are dropped with one
		// warning. Without a save directory or a file the player has no choices.
		void LoadPlayerOptions();
		// The session ends (play stops in the editor): the player's choices are forgotten; the file stays.
		void ResetSession();
		// The file's choices without applying them (the editor while not playing: what the next session starts with); an
		// empty object without a file. outProblem: why a file is ignored, or what it drops (empty when nothing is wrong).
		nlohmann::json ReadSavedOptions(std::string* outProblem = nullptr) const;
		// Deletes the file; false, with the reason, when it stays. True when there was none.
		bool DeleteSavedOptions(std::string* outError = nullptr);
		// The save directory's options file (nullopt without a save directory), whether it exists or not.
		std::optional<std::filesystem::path> GetOptionsFile() const;

		// What renderers draw with now: RendererQuality::FromSectionValue(&GetEffectiveValue()), made again only when the
		// version changed (Auto and AutoFine stay unresolved: each SceneRenderer resolves them by its own rows). The engine's
		// model only: a test model's service returns High's preset.
		const RendererQualitySettings& GetQuality();

		// IGameGraphics
		const SettingsSection* GetSection() const override;
		const nlohmann::json& GetEffectiveValue() override;
		uint64_t GetVersion() override;
		std::optional<GameGraphicsSource> GetSource(std::string_view option) override;
		const nlohmann::json& GetPlayerOptions() override;
		bool SetOption(std::string_view option, const nlohmann::json& value, std::string* outError = nullptr) override;
		bool ResetOptions(std::string_view option, std::string* outError = nullptr) override;
		bool SaveOptions(std::string* outError = nullptr) override;
	private:
		struct Data;
		Scope<Data> m_Data; // Never null
	};

}
