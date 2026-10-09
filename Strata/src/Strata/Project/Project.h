#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace Strata
{

	struct ProjectConfig
	{
		std::string Name = "Untitled";
		// Relative to the project directory, '/' separators; must stay inside the project directory.
		std::string AssetDirectory = "Assets";
		AssetHandle StartScene = UUID::Null(); // Scene the runtime loads first
	};

	// A game project: a directory holding the project file ("<Name>.stproj"), the asset directory and the
	// intermediate directory ".strata/" (import cache, build output; never committed).
	//
	// Project file: { "Strata": { "Format": "Project", "Version": 1 }, "Project": { "Name": ..., "AssetDirectory": ...,
	// "StartScene": "<handle>" } }
	class Project
	{
	public:
		static constexpr std::string_view c_FileExtension = ".stproj";
		static constexpr uint32_t c_FormatVersion = 1;

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
