#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace Strata
{

	// The project's game scripts: C++ sources built with CMake into one script module (see AGENTS.md, "Scripting").
	struct ProjectScriptSettings
	{
		// Directory holding the CMakeLists.txt of the scripts. Relative to the project directory, '/' separators, same rules
		// as the asset directory.
		std::string SourceDirectory = "Scripts";
		// CMake target and file name of the module ("<ModuleName>.dll" on Windows): a C identifier. Empty: derived from the
		// project name (Project::MakeScriptModuleName). New projects store the derived name, so renaming them keeps it.
		std::string ModuleName;
	};

	struct ProjectConfig
	{
		std::string Name = "Untitled";
		// Relative to the project directory, '/' separators; must stay inside the project directory.
		std::string AssetDirectory = "Assets";
		AssetHandle StartScene = UUID::Null(); // Scene the runtime loads first
		ProjectScriptSettings Scripts;
	};

	// A game project: a directory holding the project file ("<Name>.stproj"), the asset directory, the script sources
	// and the intermediate directory ".strata/" (import cache, script build output; never committed).
	//
	// Project file: { "Strata": { "Format": "Project", "Version": 2 }, "Project": { "Name": ..., "AssetDirectory": ...,
	// "StartScene": "<handle>", "Scripts": { "SourceDirectory": "Scripts", "ModuleName": ... } } }
	// Version 1 files (without "Scripts") load with the default script settings.
	class Project
	{
	public:
		static constexpr std::string_view c_FileExtension = ".stproj";
		static constexpr uint32_t c_FormatVersion = 2;

		// Creates the directory layout, a .gitignore for the intermediate directory, and the project file inside
		// directory (which may exist but must not contain a project file yet).
		static Ref<Project> Create(const std::filesystem::path& directory, const std::string& name, std::string* outError = nullptr);
		static Ref<Project> Load(const std::filesystem::path& projectFile, std::string* outError = nullptr);
		// Finds the project file in a directory (exactly one ".stproj" file).
		static std::filesystem::path FindProjectFile(const std::filesystem::path& directory, std::string* outError = nullptr);

		bool Save(std::string* outError = nullptr) const;

		ProjectConfig& GetConfig() { return m_Config; }
		const ProjectConfig& GetConfig() const { return m_Config; }

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
	private:
		Project() = default;
	private:
		std::filesystem::path m_ProjectFile; // Absolute
		ProjectConfig m_Config;
	};

}
