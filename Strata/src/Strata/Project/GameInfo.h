#pragma once

#include "Strata/Asset/AssetTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Strata
{

	// The game's identity: what an exported game shows of itself (its window's title and icon, the log, the executable's
	// version information on Windows). The project's ProjectConfig::Game, set by project.setGameInfo; the export carries it
	// in the manifest (GameManifest). The project's name, not the title, keys the game's log and save directories, so a new
	// title or company never moves players' saves.
	struct ProjectGameInfo
	{
		// Characters (code points) the title, the company and the copyright line may have.
		static constexpr size_t c_MaxTextLength = 128;
		static constexpr std::string_view c_DefaultVersion = "1.0.0";
		// Parts of a version, and the largest value of each (Windows keeps each in 16 bits).
		static constexpr size_t c_MaxVersionParts = 4;
		static constexpr uint32_t c_MaxVersionPart = 65535;

		std::string Title; // UTF-8; empty: the project's name
		std::string Version = std::string(c_DefaultVersion);
		std::string Company;   // Optional
		std::string Copyright; // Optional, e.g. "(c) 2026 Studio"
		AssetHandle Icon = UUID::Null(); // A texture of the project with a PNG, JPEG, TGA or BMP source; null: none

		bool operator==(const ProjectGameInfo& other) const = default;
		bool IsDefault() const { return *this == ProjectGameInfo(); }
		// The first invalid field and why ("Version: ..."), nullopt when every field is valid. The icon is not checked here: it
		// names an asset, which the editor checks (project.setGameInfo) and the export resolves.
		std::optional<std::string> Validate() const;

		// Why text cannot be a title, a company or a copyright line, nullopt when it can: it must be valid UTF-8 of at most
		// c_MaxTextLength characters without control characters (the title shows in window bars and dialogs, all of them in
		// the executable's version information).
		static std::optional<std::string> ValidateText(std::string_view text);
		// Why text cannot be a version, nullopt when it can: one to c_MaxVersionParts whole numbers from 0 to
		// c_MaxVersionPart separated by dots ("1", "1.0.0", "2026.10.22.1").
		static std::optional<std::string> ValidateVersion(std::string_view version);
		// The version's numbers with the missing ones 0 ("1.2" -> 1, 2, 0, 0); nullopt for an invalid version.
		static std::optional<std::array<uint16_t, c_MaxVersionParts>> ParseVersion(std::string_view version);
	};

}
