#pragma once

#include <Strata/Core/Base.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class Process;

	// Where the external editor command came from.
	enum class ExternalEditorSource : uint8_t
	{
		None = 0,    // No command: files open in the system's default application for their type
		Environment, // STRATA_EXTERNAL_EDITOR
		User         // The user's editor settings (script.setExternalEditor)
	};

	const char* ExternalEditorSourceToString(ExternalEditorSource source);

	// A code editor as the External Editor dialog offers it: a name and its command template.
	struct ExternalEditorChoice
	{
		std::string Name;    // "VS Code", "Visual Studio", "CLion"
		std::string Command;

		bool operator==(const ExternalEditorChoice&) const = default;
	};

	// The person's code editor, which opens script sources at a line (script.open, the Problems panel, crash reports). Its
	// command is a template such as
	//   code -g {file}:{line}:{column}
	//   "C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/devenv.exe" /Edit {file}
	// in which whitespace separates arguments, double quotes group them, and {file}, {line}, {column} and {directory} (the
	// project's script directory) are filled in; a template without {file} gets the file as its last argument. The program
	// is a path or a name found in PATH as the system finds programs (Platform::FindProgram: on Windows with the extensions
	// of PATHEXT), and is started directly, never through a shell, so a batch file (.cmd, .bat) cannot be one: VS Code's
	// launcher in PATH (bin/code.cmd) starts Code.exe beside it instead, and other batch files are refused. The command
	// comes from STRATA_EXTERNAL_EDITOR, else from the user's editor settings; projects cannot name one, because a project
	// may come from someone else and opening a file must not run what it chose.
	namespace ExternalEditor
	{

		// The user's editor settings file: <user data>/Strata/EditorSettings.json (private to the user), or nothing where the
		// system has no per-user data directory.
		std::optional<std::filesystem::path> GetDefaultSettingsFile();

		// Splits a template into the program and its arguments with the placeholders filled in. Nothing (with the reason) for
		// an empty template, unbalanced quotes, an unknown placeholder or a batch file as the program.
		std::optional<std::vector<std::string>> ExpandCommand(std::string_view commandTemplate, const std::filesystem::path& file, uint32_t line,
			uint32_t column, const std::filesystem::path& directory, std::string* outError = nullptr);

		// Why a template cannot be used, or nothing when it can (ExpandCommand on an example file).
		std::optional<std::string> ValidateCommand(std::string_view commandTemplate);

		// The file a template's program starts (see above): nothing, with the reason, for a template that cannot be used, a
		// program that is not there (naming where it was looked for) or a batch file that stands for no known editor.
		std::optional<std::filesystem::path> ResolveProgram(std::string_view commandTemplate, std::string* outError = nullptr);

		// The code editors found on this machine, each with a command whose program resolves: VS Code when its launcher is in
		// PATH, the Visual Studio installation that `toolchainFiles` (the engine's compiler and build program) lie in, CLion when
		// its launcher is in PATH. Reads the file system.
		std::vector<ExternalEditorChoice> FindChoices(std::span<const std::filesystem::path> toolchainFiles);
		// Commands for when none is found: VS Code, Visual Studio (with its folder to fill in) and CLion.
		std::vector<ExternalEditorChoice> GetExampleChoices();

	}

	// The external editor setting and the processes it started (they outlive nothing: finished ones are reaped as more start).
	// Main thread only.
	class ExternalEditorLauncher
	{
	public:
		// `settingsFile` keeps the user's command (empty: in memory only, as tests need). Read once here.
		explicit ExternalEditorLauncher(std::filesystem::path settingsFile = {});
		~ExternalEditorLauncher();

		ExternalEditorLauncher(const ExternalEditorLauncher&) = delete;
		ExternalEditorLauncher& operator=(const ExternalEditorLauncher&) = delete;

		// The command in use and where it came from (the environment wins over the user's setting).
		std::string GetCommand() const;
		ExternalEditorSource GetSource() const;
		const std::string& GetUserCommand() const { return m_UserCommand; }
		// Sets the user's command (empty: none), saved in the settings file. Fails for a template that cannot be used or whose
		// program is not there (ResolveProgram), or a file that cannot be written.
		bool SetUserCommand(const std::string& command, std::string* outError = nullptr);

		// Opens a file at a line and column (0: unknown) with the command, or with the default application without one.
		// `directory` fills {directory}. On success outHow says how ("Code.exe", "the default application").
		bool Open(const std::filesystem::path& file, uint32_t line, uint32_t column, const std::filesystem::path& directory, std::string* outHow = nullptr,
			std::string* outError = nullptr);
	private:
		void ReapFinished();
	private:
		std::filesystem::path m_SettingsFile;
		std::string m_UserCommand;
		std::vector<Scope<Process>> m_Processes;
	};

}
