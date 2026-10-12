#include "Editor/ExternalEditor.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Process.h>
#include <Strata/Core/StringUtils.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace Strata
{

	namespace
	{

		constexpr const char* c_EnvironmentVariable = "STRATA_EXTERNAL_EDITOR";
		constexpr const char* c_SettingsKey = "ExternalEditor";
		constexpr size_t c_MaxSettingsFileSize = 64 * 1024;
		constexpr size_t c_MaxCommandLength = 4096;
		// Editor processes kept to be reaped; older ones are let go (letting go never ends them).
		constexpr size_t c_MaxTrackedProcesses = 16;
		// The commands of editors whose launcher goes into PATH.
		constexpr const char* c_VSCodeCommand = "code -g {file}:{line}:{column}";
		constexpr const char* c_CLionCommand = "clion --line {line} {file}";

		bool Fail(std::string* outError, std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		}

		// Splits on whitespace outside double quotes; quotes group and are removed.
		std::optional<std::vector<std::string>> SplitCommand(std::string_view command, std::string* outError)
		{
			std::vector<std::string> tokens;
			std::string token;
			bool inToken = false;
			bool quoted = false;
			for (char character : command)
			{
				if (character == '"')
				{
					quoted = !quoted;
					inToken = true;
				}
				else if (!quoted && (character == ' ' || character == '\t'))
				{
					if (inToken)
						tokens.push_back(std::exchange(token, std::string()));
					inToken = false;
				}
				else if (character == '\n' || character == '\r' || character == '\0')
				{
					Fail(outError, "the command has a line break or a null character");
					return std::nullopt;
				}
				else
				{
					token += character;
					inToken = true;
				}
			}
			if (quoted)
			{
				Fail(outError, "the command has an unbalanced double quote");
				return std::nullopt;
			}
			if (inToken)
				tokens.push_back(std::move(token));
			if (tokens.empty() || tokens.front().empty())
			{
				Fail(outError, "the command names no program");
				return std::nullopt;
			}
			return tokens;
		}

		// Fills the placeholders of one argument; false (with the reason) for an unknown one.
		bool FillPlaceholders(std::string& argument, const std::array<std::pair<std::string_view, std::string>, 4>& values, bool& outHasFile,
			std::string* outError)
		{
			std::string result;
			size_t position = 0;
			while (position < argument.size())
			{
				const size_t open = argument.find('{', position);
				if (open == std::string::npos)
				{
					result.append(argument, position, std::string::npos);
					break;
				}
				result.append(argument, position, open - position);
				const size_t close = argument.find('}', open);
				const std::string_view name = close == std::string::npos ? std::string_view() : std::string_view(argument).substr(open + 1, close - open - 1);
				const auto value = std::find_if(values.begin(), values.end(), [name](const auto& entry) { return entry.first == name; });
				if (close == std::string::npos || value == values.end())
					return Fail(outError, fmt::format("'{}' holds an unknown placeholder; use {{file}}, {{line}}, {{column}} and {{directory}}", argument));
				outHasFile |= name == "file";
				result += value->second;
				position = close + 1;
			}
			argument = std::move(result);
			return true;
		}

		std::string ToArgumentPath(const std::filesystem::path& path)
		{
			std::filesystem::path native = path;
			return FileSystem::ToUTF8(native.make_preferred());
		}

		// The file a command's program name starts (Platform::FindProgram). What PATH offers for a code editor may be a batch
		// file that launches it, which only runs through a shell: VS Code's bin/code.cmd stands for the editor beside its bin
		// folder, which takes the same arguments. Other batch files are refused.
		std::optional<std::filesystem::path> FindEditorProgram(const std::string& program, std::string* outError)
		{
			std::vector<std::filesystem::path> searched;
			const std::optional<std::filesystem::path> found = Platform::FindProgram(program, &searched);
			if (!found)
			{
				const bool isPath = program.find_first_of("\\/:") != std::string::npos;
				Fail(outError, isPath ? fmt::format("there is no program '{}'", program)
					: fmt::format("there is no program '{}' in the {} folders of PATH; name it by its path", program, searched.size()));
				return std::nullopt;
			}
			const std::string extension = StringUtils::ToLower(FileSystem::ToUTF8(found->extension()));
			if (extension != ".cmd" && extension != ".bat")
				return found;
			const std::string stem = StringUtils::ToLower(FileSystem::ToUTF8(found->stem()));
			const std::string folder = StringUtils::ToLower(FileSystem::ToUTF8(found->parent_path().filename()));
			if (folder == "bin" && (stem == "code" || stem == "code-insiders"))
			{
				const std::filesystem::path editor = found->parent_path().parent_path() / (stem == "code" ? "Code.exe" : "Code - Insiders.exe");
				if (FileSystem::IsRegularFile(editor))
					return editor;
			}
			Fail(outError, fmt::format("'{}' is the batch file '{}', which only runs through a shell; name the program it starts", program,
				FileSystem::ToUTF8(*found)));
			return std::nullopt;
		}

	}

	const char* ExternalEditorSourceToString(ExternalEditorSource source)
	{
		switch (source)
		{
			case ExternalEditorSource::None:        return "none";
			case ExternalEditorSource::Environment: return "environment";
			case ExternalEditorSource::User:        return "user";
		}
		return "none";
	}

	namespace ExternalEditor
	{

		std::optional<std::filesystem::path> GetDefaultSettingsFile()
		{
			const std::optional<std::filesystem::path> directory = Platform::FindUserDataDirectory("Strata");
			if (!directory)
				return std::nullopt;
			return *directory / "EditorSettings.json";
		}

		std::optional<std::vector<std::string>> ExpandCommand(std::string_view commandTemplate, const std::filesystem::path& file, uint32_t line,
			uint32_t column, const std::filesystem::path& directory, std::string* outError)
		{
			if (commandTemplate.size() > c_MaxCommandLength)
			{
				Fail(outError, fmt::format("the command is longer than {} characters", c_MaxCommandLength));
				return std::nullopt;
			}
			std::optional<std::vector<std::string>> arguments = SplitCommand(commandTemplate, outError);
			if (!arguments)
				return std::nullopt;

			// Batch files only run through a shell, which would also interpret the file's name.
			const std::string program = StringUtils::ToLower(arguments->front());
			if (program.ends_with(".cmd") || program.ends_with(".bat"))
			{
				Fail(outError, fmt::format("'{}' is a batch file, which only runs through a shell; name the program it starts (VS Code: Code.exe)",
					arguments->front()));
				return std::nullopt;
			}

			const std::array<std::pair<std::string_view, std::string>, 4> values = { {
				{ "file", ToArgumentPath(file) },
				{ "line", std::to_string(std::max<uint32_t>(line, 1)) },
				{ "column", std::to_string(std::max<uint32_t>(column, 1)) },
				{ "directory", ToArgumentPath(directory) } } };
			bool hasFile = false;
			for (size_t index = 0; index < arguments->size(); index++)
			{
				bool argumentHasFile = false;
				if (!FillPlaceholders((*arguments)[index], values, argumentHasFile, outError))
					return std::nullopt;
				if (index == 0 && argumentHasFile)
				{
					Fail(outError, "the program cannot be the file to open");
					return std::nullopt;
				}
				hasFile |= argumentHasFile;
			}
			if (!hasFile)
				arguments->push_back(values[0].second);
			return arguments;
		}

		std::optional<std::string> ValidateCommand(std::string_view commandTemplate)
		{
			std::string error;
			if (!ExpandCommand(commandTemplate, "Example.cpp", 1, 1, "Scripts", &error))
				return error;
			return std::nullopt;
		}

		std::optional<std::filesystem::path> ResolveProgram(std::string_view commandTemplate, std::string* outError)
		{
			const std::optional<std::vector<std::string>> arguments = ExpandCommand(commandTemplate, "Example.cpp", 1, 1, "Scripts", outError);
			if (!arguments)
				return std::nullopt;
			return FindEditorProgram(arguments->front(), outError);
		}

		std::vector<ExternalEditorChoice> FindChoices(std::span<const std::filesystem::path> toolchainFiles)
		{
			std::vector<ExternalEditorChoice> choices;
			const auto offer = [&choices](std::string name, std::string command)
			{
				if (ResolveProgram(command))
					choices.push_back({ std::move(name), std::move(command) });
			};
			offer("VS Code", c_VSCodeCommand);
			// Visual Studio: the installation the compiler (<installation>/VC/Tools/...) or MSBuild (<installation>/MSBuild/...)
			// lies in holds the IDE at Common7/IDE/devenv.exe.
			bool visualStudio = false;
			for (const std::filesystem::path& file : toolchainFiles)
			{
				for (std::filesystem::path folder = file.parent_path(); !visualStudio && folder.has_relative_path(); folder = folder.parent_path())
				{
					const std::filesystem::path devenv = folder / "Common7" / "IDE" / "devenv.exe";
					if (!FileSystem::IsRegularFile(devenv))
						continue;
					offer("Visual Studio", fmt::format("\"{}\" /Edit {{file}}", FileSystem::ToUTF8(devenv)));
					visualStudio = true;
				}
			}
			offer("CLion", c_CLionCommand);
			return choices;
		}

		std::vector<ExternalEditorChoice> GetExampleChoices()
		{
			return {
				{ "VS Code", c_VSCodeCommand },
				{ "Visual Studio", "\"<Visual Studio folder>/Common7/IDE/devenv.exe\" /Edit {file}" },
				{ "CLion", c_CLionCommand } };
		}

	}

	ExternalEditorLauncher::ExternalEditorLauncher(std::filesystem::path settingsFile)
		: m_SettingsFile(std::move(settingsFile))
	{
		if (m_SettingsFile.empty() || !FileSystem::Exists(m_SettingsFile))
			return;
		// The command names a program the editor runs: only a file the user alone could have written is read.
		std::string error;
		const std::optional<std::string> text = Platform::ReadTrustedFile(m_SettingsFile, c_MaxSettingsFileSize, &error);
		const std::optional<nlohmann::json> json = text ? JsonUtils::Parse(*text) : std::nullopt;
		const nlohmann::json* command = json ? JsonUtils::Find(*json, c_SettingsKey) : nullptr;
		if (!text || !json)
		{
			ST_WARN("The editor settings '{}' are ignored: {}", FileSystem::ToUTF8(m_SettingsFile), text ? std::string("not JSON") : error);
			return;
		}
		if (!command || !command->is_string())
			return;
		if (std::optional<std::string> problem = ExternalEditor::ValidateCommand(command->get<std::string>()))
		{
			ST_WARN("The external editor of the editor settings '{}' is ignored: {}", FileSystem::ToUTF8(m_SettingsFile), *problem);
			return;
		}
		m_UserCommand = command->get<std::string>();
	}

	ExternalEditorLauncher::~ExternalEditorLauncher() = default;

	std::string ExternalEditorLauncher::GetCommand() const
	{
		if (const std::optional<std::string> environment = Platform::GetEnvVar(c_EnvironmentVariable); environment && !environment->empty())
			return *environment;
		return m_UserCommand;
	}

	ExternalEditorSource ExternalEditorLauncher::GetSource() const
	{
		if (const std::optional<std::string> environment = Platform::GetEnvVar(c_EnvironmentVariable); environment && !environment->empty())
			return ExternalEditorSource::Environment;
		return m_UserCommand.empty() ? ExternalEditorSource::None : ExternalEditorSource::User;
	}

	bool ExternalEditorLauncher::SetUserCommand(const std::string& command, std::string* outError)
	{
		if (!command.empty())
		{
			std::string problem;
			if (!ExternalEditor::ResolveProgram(command, &problem))
				return Fail(outError, fmt::format("The external editor command cannot be used: {}", problem));
		}
		if (!m_SettingsFile.empty())
		{
			// Other settings in the file are kept.
			nlohmann::json settings = nlohmann::json::object();
			if (FileSystem::Exists(m_SettingsFile))
			{
				const std::optional<std::string> text = Platform::ReadTrustedFile(m_SettingsFile, c_MaxSettingsFileSize);
				const std::optional<nlohmann::json> json = text ? JsonUtils::Parse(*text) : std::nullopt;
				if (json && json->is_object())
					settings = *json;
			}
			if (command.empty())
				settings.erase(c_SettingsKey);
			else
				settings[c_SettingsKey] = command;
			std::string error;
			if (!Platform::WritePrivateFile(m_SettingsFile, JsonUtils::Dump(settings, 1, '\t') + "\n", &error))
				return Fail(outError, fmt::format("Cannot save the editor settings '{}': {}", FileSystem::ToUTF8(m_SettingsFile), error));
		}
		m_UserCommand = command;
		return true;
	}

	bool ExternalEditorLauncher::Open(const std::filesystem::path& file, uint32_t line, uint32_t column, const std::filesystem::path& directory,
		std::string* outHow, std::string* outError)
	{
		ReapFinished();
		const std::string command = GetCommand();
		std::string failure;
		if (!command.empty())
		{
			std::optional<std::vector<std::string>> arguments = ExternalEditor::ExpandCommand(command, file, line, column, directory, &failure);
			// The program as the system would find it (a name in PATH, VS Code's launcher), started directly.
			const std::optional<std::filesystem::path> program = arguments ? FindEditorProgram(arguments->front(), &failure) : std::nullopt;
			if (arguments && program)
			{
				ProcessSpecification specification;
				specification.Executable = *program;
				specification.Arguments.assign(arguments->begin() + 1, arguments->end());
				specification.Output = ProcessOutputMode::Discard;
				// The editor lives on after this one ends, and console signals sent to the editor are not its concern.
				specification.Detached = true;
				Scope<Process> process = CreateScope<Process>();
				if (process->Start(specification))
				{
					if (outHow)
						*outHow = FileSystem::ToUTF8(specification.Executable.filename());
					m_Processes.push_back(std::move(process));
					return true;
				}
				failure = fmt::format("'{}' did not start: {}", arguments->front(), process->GetLastError());
			}
			ST_WARN("The external editor ({}) cannot open '{}': {}; opening it with the default application instead", ExternalEditorSourceToString(GetSource()),
				FileSystem::ToUTF8(file), failure);
		}

		if (!Platform::OpenWithDefaultApplication(FileSystem::ToUTF8(file)))
		{
			return Fail(outError, fmt::format("Cannot open '{}': {}the system has no application for it (set an external editor with "
				"script.setExternalEditor)", FileSystem::ToUTF8(file), failure.empty() ? std::string() : failure + "; "));
		}
		if (outHow)
			*outHow = failure.empty() ? "the default application" : "the default application (the external editor failed: " + failure + ")";
		return true;
	}

	void ExternalEditorLauncher::ReapFinished()
	{
		m_Processes.erase(std::remove_if(m_Processes.begin(), m_Processes.end(), [](const Scope<Process>& process) { return !process->IsRunning(); }),
			m_Processes.end());
		if (m_Processes.size() >= c_MaxTrackedProcesses)
			m_Processes.erase(m_Processes.begin());
	}

}
