#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Core/ProjectSettings.h"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// The project's game scripts: C++ sources built with CMake into one script module (see Docs/DeveloperGuide.md,
	// "Building and loading scripts").
	struct ProjectScriptSettings
	{
		// Directory holding the CMakeLists.txt of the scripts. Relative to the project directory, '/' separators, same rules
		// as the asset directory.
		std::string SourceDirectory = "Scripts";
		// CMake target and file name of the module ("<ModuleName>.dll" on Windows): a C identifier. Empty: derived from the
		// project name (Project::MakeScriptModuleName). New projects store the derived name, so renaming them keeps it.
		std::string ModuleName;
	};

	// How the project exports as a game (project.export).
	struct ProjectExportSettings
	{
		// Assets ("Prefabs/Enemy.stprefab") and folders ("Prefabs") relative to the asset directory, '/' separators, that an
		// export with pruning keeps with what they use although no scene references them: assets the scripts find by path.
		std::vector<std::string> AlwaysInclude;
		// A scene the game plays while its start scene loads (GameManifest::LoadingScene); null: none (a cleared frame).
		AssetHandle LoadingScene = UUID::Null();
	};

	struct ProjectConfig
	{
		std::string Name = "Untitled";
		// Relative to the project directory, '/' separators; must stay inside the project directory.
		std::string AssetDirectory = "Assets";
		AssetHandle StartScene = UUID::Null(); // Scene the runtime loads first
		ProjectScriptSettings Scripts;
		ProjectExportSettings Export;
	};

	// A game project: a directory holding the project file ("<Name>.stproj"), the asset directory, the script sources
	// and the intermediate directory ".strata/" (import cache, script build output; never committed).
	//
	// Project file: { "Strata": { "Format": "Project", "Version": 3 }, "Project": { "Name": ..., "AssetDirectory": ...,
	// "StartScene": "<handle>", "Scripts": { "SourceDirectory": "Scripts", "ModuleName": ... },
	// "Export": { "AlwaysInclude": [...], "LoadingScene": "<handle>" } },
	// "Settings": { "<Section id>": { "Version": n, "Value": ... } } }
	// Version 1 files (without "Scripts") load with the default script settings, version 1 and 2 files without settings;
	// "Export" is optional (written when it says something), and so is "Settings" (written while a section is set). Version 3
	// came with the settings: 0.5.0 rewrote the file from what it knew, so it refuses version 3 rather than drop them. What
	// this version does not know is kept: sections no registered module claims, sections whose value their module refuses,
	// and members of "Project" a later version added are written back as they were read.
	class Project
	{
	public:
		static constexpr std::string_view c_FileExtension = ".stproj";
		static constexpr uint32_t c_FormatVersion = 3;
		// Larger project files are refused, by Load and by Save.
		static constexpr size_t c_MaxFileBytes = 1024 * 1024;

		// Creates the directory layout, a .gitignore for the intermediate directory, and the project file inside
		// directory (which may exist but must not contain a project file yet).
		static Ref<Project> Create(const std::filesystem::path& directory, const std::string& name, std::string* outError = nullptr);
		// Validates the project's settings sections with `registry` (null: the process's registry): a section that fails keeps
		// its stored value, its default applies, and it is logged as a warning and listed by GetSettingsProblems.
		static Ref<Project> Load(const std::filesystem::path& projectFile, std::string* outError = nullptr, const SettingsRegistry* registry = nullptr);
		// Finds the project file in a directory (exactly one ".stproj" file).
		static std::filesystem::path FindProjectFile(const std::filesystem::path& directory, std::string* outError = nullptr);
		// Whether Create accepts a name: it becomes the project file's name, so it is a valid file name on every platform
		// (at most 128 bytes, none of <>:"/\|?* or control characters, no leading or trailing space, no trailing dot, no
		// reserved device name such as CON).
		static bool IsValidName(std::string_view name);

		bool Save(std::string* outError = nullptr) const;
		// The project file's text as Save writes it, or nullopt with the reason: invalid fields, or more than c_MaxFileBytes
		// (what a change of the settings is checked against before it is made).
		std::optional<std::string> MakeFileText(std::string* outError = nullptr) const;

		ProjectConfig& GetConfig() { return m_Config; }
		const ProjectConfig& GetConfig() const { return m_Config; }

		// The project's settings sections (Docs/DeveloperGuide.md, "Project settings").
		ProjectSettingsValues& GetSettings() { return m_Settings; }
		const ProjectSettingsValues& GetSettings() const { return m_Settings; }
		// The stored sections Load's validation refused.
		const std::vector<SettingsProblem>& GetSettingsProblems() const { return m_SettingsProblems; }

		const std::filesystem::path& GetProjectFile() const { return m_ProjectFile; }
		std::filesystem::path GetProjectDirectory() const { return m_ProjectFile.parent_path(); }
		std::filesystem::path GetAssetDirectory() const;
		std::filesystem::path GetIntermediateDirectory() const { return GetProjectDirectory() / ".strata"; }
		std::filesystem::path GetCacheDirectory() const { return GetIntermediateDirectory() / "Cache"; }

		// Scripts: the source directory (with the CMakeLists.txt), the module name, and where the editor builds it:
		// .strata/Scripts/Build (CMake build tree) and .strata/Scripts/Bin/<module file> (the module the editor loads).
		std::filesystem::path GetScriptSourceDirectory() const;
		std::string GetScriptModuleName() const;
		std::filesystem::path GetScriptBuildDirectory() const { return GetIntermediateDirectory() / "Scripts" / "Build"; }
		std::filesystem::path GetScriptBinaryDirectory() const { return GetIntermediateDirectory() / "Scripts" / "Bin"; }
		std::filesystem::path GetScriptModulePath() const;

		// A module name derived from a project name: its ASCII letters and digits in PascalCase, "Scripts" appended and
		// "Game" prepended when it would not start with a letter ("my game!" -> "MyGameScripts", "2D" -> "Game2DScripts").
		static std::string MakeScriptModuleName(std::string_view projectName);
		static bool IsValidScriptModuleName(std::string_view name);
		static constexpr size_t c_MaxScriptModuleNameSize = 64;

		// The project the editor or runtime is working on (null when none is open).
		static void SetActive(const Ref<Project>& project);
		static const Ref<Project>& GetActive();
		~Project();
		Project(const Project& other);
		Project& operator=(const Project& other);
	private:
		Project();
	private:
		std::filesystem::path m_ProjectFile; // Absolute
		ProjectConfig m_Config;
		ProjectSettingsValues m_Settings;
		std::vector<SettingsProblem> m_SettingsProblems;
		Scope<nlohmann::json> m_UnknownMembers; // Members of "Project" this version does not know: an object
	};

}
