#include "stpch.h"
#include "Strata/Project/Project.h"

#include "Strata/Core/DynamicLibrary.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/StringUtils.h"
#include "Strata/Reflection/PropertyJson.h"

#include <algorithm>
#include <cctype>

namespace Strata
{

	namespace
	{

		Ref<Project>& GetActiveProjectStorage()
		{
			static Ref<Project> s_Active;
			return s_Active;
		}

		// Names and paths of a project end up in generated files (the scripts' CMakeLists.txt) and logs, where control
		// characters (a newline above all) could inject content: they never contain any.
		bool HasControlCharacters(std::string_view text)
		{
			return std::any_of(text.begin(), text.end(), [](char character)
			{
				const auto value = static_cast<unsigned char>(character);
				return value < 0x20 || value == 0x7F;
			});
		}

		// Asset and script directories are plain relative subdirectories of the project: not the project directory itself,
		// no absolute paths, no "..", nothing inside the intermediate directory, no hidden directories, no control
		// characters.
		bool IsValidProjectSubdirectory(std::string_view directory)
		{
			if (directory.empty() || HasControlCharacters(directory))
				return false;
			const std::filesystem::path path = FileSystem::FromUTF8(directory);
			if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
				return false;
			const std::filesystem::path normalized = path.lexically_normal();
			if (normalized.empty() || normalized == ".")
				return false;
			for (const std::filesystem::path& component : normalized)
			{
				const std::string name = FileSystem::ToUTF8(component);
				if (name == ".." || (!name.empty() && name[0] == '.'))
					return false;
			}
			return true;
		}

		// Names Windows reserves for devices, invalid as file names on that platform (with any extension).
		bool IsReservedFileName(std::string_view name)
		{
			std::string base = StringUtils::ToLower(name.substr(0, name.find('.')));
			for (const char* reserved : { "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
				"lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9" })
			{
				if (base == reserved)
					return true;
			}
			return false;
		}

		// Script settings must name a valid source directory and module (an empty module name is derived later).
		bool ValidateScriptSettings(const ProjectScriptSettings& settings, std::string* outError)
		{
			if (!IsValidProjectSubdirectory(settings.SourceDirectory))
			{
				if (outError)
					*outError = fmt::format("script source directory '{}' must be a relative path inside the project", settings.SourceDirectory);
				return false;
			}
			if (!settings.ModuleName.empty() && !Project::IsValidScriptModuleName(settings.ModuleName))
			{
				if (outError)
					*outError = fmt::format("script module name '{}' must be a C identifier of at most {} characters", settings.ModuleName,
						Project::c_MaxScriptModuleNameSize);
				return false;
			}
			return true;
		}

	}

	bool Project::IsValidName(std::string_view name)
	{
		if (name.empty() || name.size() > 128 || name.front() == ' ' || name.back() == ' ' || name.back() == '.' || IsReservedFileName(name)
			|| HasControlCharacters(name))
		{
			return false;
		}
		for (char character : name)
		{
			const auto value = static_cast<unsigned char>(character);
			if (value < 0x20 || std::string_view("<>:\"/\\|?*").find(character) != std::string_view::npos)
				return false;
		}
		return true;
	}

	Ref<Project> Project::Create(const std::filesystem::path& directory, const std::string& name, std::string* outError)
	{
		auto fail = [outError](std::string message) -> Ref<Project>
		{
			if (outError)
				*outError = std::move(message);
			return nullptr;
		};

		if (!IsValidName(name))
			return fail(fmt::format("'{}' is not a valid project name (it becomes a file name)", name));

		std::error_code error;
		const std::filesystem::path projectDirectory = std::filesystem::absolute(directory, error).lexically_normal();
		if (error)
			return fail(fmt::format("Invalid project directory '{}': {}", FileSystem::ToUTF8(directory), error.message()));
		if (FileSystem::Exists(projectDirectory) && !FileSystem::IsDirectory(projectDirectory))
			return fail(fmt::format("'{}' is not a directory", FileSystem::ToUTF8(projectDirectory)));
		if (FileSystem::IsDirectory(projectDirectory) && !FindProjectFile(projectDirectory).empty())
			return fail(fmt::format("'{}' already contains a project", FileSystem::ToUTF8(projectDirectory)));

		Ref<Project> project(new Project());
		project->m_ProjectFile = projectDirectory / FileSystem::FromUTF8(name + std::string(c_FileExtension));
		project->m_Config.Name = name;
		project->m_Config.Scripts.ModuleName = MakeScriptModuleName(name);

		if (!FileSystem::CreateDirectories(project->GetAssetDirectory()) || !FileSystem::CreateDirectories(project->GetCacheDirectory()))
			return fail(fmt::format("Could not create the project directories in '{}'", FileSystem::ToUTF8(projectDirectory)));

		const std::filesystem::path gitIgnore = projectDirectory / ".gitignore";
		if (!FileSystem::Exists(gitIgnore) && !FileSystem::WriteText(gitIgnore, "# Strata intermediate files (import cache, builds)\n/.strata/\n"))
			return fail(fmt::format("Could not write '{}'", FileSystem::ToUTF8(gitIgnore)));

		std::string saveError;
		if (!project->Save(&saveError))
			return fail(saveError);
		return project;
	}

	Ref<Project> Project::Load(const std::filesystem::path& projectFile, std::string* outError)
	{
		auto fail = [outError, &projectFile](const std::string& message) -> Ref<Project>
		{
			if (outError)
				*outError = fmt::format("{}: {}", FileSystem::ToUTF8(projectFile), message);
			return nullptr;
		};

		std::optional<std::string> text = FileSystem::ReadText(projectFile);
		if (!text)
			return fail("cannot read the project file");

		std::string parseError;
		std::optional<nlohmann::json> document = JsonUtils::Parse(*text, &parseError);
		if (!document || !document->is_object())
			return fail(fmt::format("invalid JSON ({})", parseError));

		const nlohmann::json* header = JsonUtils::Find(*document, "Strata");
		if (!header || JsonUtils::GetString(*header, "Format") != "Project")
			return fail("not a Strata project file");
		const uint64_t version = JsonUtils::GetUInt(*header, "Version", 0);
		if (version == 0 || version > c_FormatVersion)
			return fail(fmt::format("unsupported project version {} (this engine reads up to {})", version, c_FormatVersion));

		const nlohmann::json* config = JsonUtils::Find(*document, "Project");
		if (!config || !config->is_object())
			return fail("missing \"Project\" object");

		Ref<Project> project(new Project());
		std::error_code error;
		project->m_ProjectFile = std::filesystem::absolute(projectFile, error).lexically_normal();
		if (error)
			return fail(error.message());

		ProjectConfig& projectConfig = project->m_Config;
		projectConfig.Name = JsonUtils::GetString(*config, "Name", FileSystem::ToUTF8(projectFile.stem()));
		if (HasControlCharacters(projectConfig.Name))
			return fail("the project name contains control characters");
		projectConfig.AssetDirectory = JsonUtils::GetString(*config, "AssetDirectory", projectConfig.AssetDirectory);
		if (!IsValidProjectSubdirectory(projectConfig.AssetDirectory))
			return fail(fmt::format("asset directory '{}' must be a relative path inside the project", projectConfig.AssetDirectory));
		if (const nlohmann::json* startScene = JsonUtils::Find(*config, "StartScene"))
		{
			std::optional<UUID> handle = UUIDFromJson(*startScene);
			if (!handle)
				return fail("\"StartScene\" is not an asset handle");
			projectConfig.StartScene = *handle;
		}

		// Version 1 projects have no "Scripts" object: they keep the defaults.
		if (const nlohmann::json* scripts = JsonUtils::Find(*config, "Scripts"))
		{
			if (!scripts->is_object())
				return fail("\"Scripts\" must be an object");
			ProjectScriptSettings& settings = projectConfig.Scripts;
			settings.SourceDirectory = JsonUtils::GetString(*scripts, "SourceDirectory", settings.SourceDirectory);
			settings.ModuleName = JsonUtils::GetString(*scripts, "ModuleName", settings.ModuleName);
		}
		std::string scriptError;
		if (!ValidateScriptSettings(projectConfig.Scripts, &scriptError))
			return fail(scriptError);
		return project;
	}

	std::filesystem::path Project::FindProjectFile(const std::filesystem::path& directory, std::string* outError)
	{
		std::filesystem::path result;
		uint32_t count = 0;
		std::error_code error;
		for (std::filesystem::directory_iterator iterator(directory, error), end; !error && iterator != end; iterator.increment(error))
		{
			if (iterator->is_regular_file(error) && FileSystem::ToUTF8(iterator->path().extension()) == c_FileExtension)
			{
				result = iterator->path();
				count++;
			}
		}

		if (count == 1)
			return result;
		if (outError)
		{
			*outError = count == 0 ? fmt::format("No project file in '{}'", FileSystem::ToUTF8(directory))
				: fmt::format("'{}' contains {} project files; pass the one to open", FileSystem::ToUTF8(directory), count);
		}
		return {};
	}

	bool Project::Save(std::string* outError) const
	{
		if (HasControlCharacters(m_Config.Name))
		{
			if (outError)
				*outError = "The project name contains control characters";
			return false;
		}
		if (!IsValidProjectSubdirectory(m_Config.AssetDirectory))
		{
			if (outError)
				*outError = fmt::format("Asset directory '{}' must be a relative path inside the project", m_Config.AssetDirectory);
			return false;
		}

		if (!ValidateScriptSettings(m_Config.Scripts, outError))
			return false;

		nlohmann::json document = nlohmann::json::object();
		document["Strata"] = { { "Format", "Project" }, { "Version", c_FormatVersion } };
		nlohmann::json config = nlohmann::json::object();
		config["Name"] = m_Config.Name;
		config["AssetDirectory"] = m_Config.AssetDirectory;
		config["StartScene"] = UUIDToJson(m_Config.StartScene);
		// The module name is stored even when derived, so that renaming the project keeps the module's name.
		config["Scripts"] = { { "SourceDirectory", m_Config.Scripts.SourceDirectory }, { "ModuleName", GetScriptModuleName() } };
		document["Project"] = std::move(config);

		if (!FileSystem::WriteText(m_ProjectFile, JsonUtils::Dump(document, 1, '\t') + "\n"))
		{
			if (outError)
				*outError = fmt::format("Could not write '{}'", FileSystem::ToUTF8(m_ProjectFile));
			return false;
		}
		return true;
	}

	std::filesystem::path Project::GetAssetDirectory() const
	{
		return (GetProjectDirectory() / FileSystem::FromUTF8(m_Config.AssetDirectory)).lexically_normal();
	}

	std::filesystem::path Project::GetScriptSourceDirectory() const
	{
		return (GetProjectDirectory() / FileSystem::FromUTF8(m_Config.Scripts.SourceDirectory)).lexically_normal();
	}

	std::string Project::GetScriptModuleName() const
	{
		return m_Config.Scripts.ModuleName.empty() ? MakeScriptModuleName(m_Config.Name) : m_Config.Scripts.ModuleName;
	}

	std::filesystem::path Project::GetScriptModulePath() const
	{
		return GetScriptBinaryDirectory() / FileSystem::FromUTF8(GetScriptModuleName() + std::string(DynamicLibrary::GetFileExtension()));
	}

	std::string Project::MakeScriptModuleName(std::string_view projectName)
	{
		// PascalCase from the ASCII letters and digits; every other character separates words.
		std::string name;
		bool wordStart = true;
		for (char character : projectName)
		{
			const auto value = static_cast<unsigned char>(character);
			if (value >= 0x80 || !std::isalnum(value))
			{
				wordStart = true;
				continue;
			}
			name += wordStart ? static_cast<char>(std::toupper(value)) : character;
			wordStart = false;
		}
		if (name.empty() || !std::isalpha(static_cast<unsigned char>(name.front())))
			name.insert(0, "Game");
		// Keep room for the suffix within the identifier limit.
		if (name.size() > c_MaxScriptModuleNameSize - 7)
			name.resize(c_MaxScriptModuleNameSize - 7);
		return name + "Scripts";
	}

	bool Project::IsValidScriptModuleName(std::string_view name)
	{
		if (name.empty() || name.size() > c_MaxScriptModuleNameSize || !(std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_'))
			return false;
		return std::all_of(name.begin(), name.end(), [](char character)
		{
			const auto value = static_cast<unsigned char>(character);
			return value < 0x80 && (std::isalnum(value) || character == '_');
		});
	}

	void Project::SetActive(const Ref<Project>& project)
	{
		GetActiveProjectStorage() = project;
	}

	const Ref<Project>& Project::GetActive()
	{
		return GetActiveProjectStorage();
	}

}
