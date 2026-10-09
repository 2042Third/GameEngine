#include "Editor/ScriptBuild.h"

#include "Editor/ScriptBuildConfig.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Process.h>
#include <Strata/Project/Project.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>

namespace Strata
{

	namespace
	{

		constexpr const char* c_StampFile = "StrataScriptBuild.json";
		// The process output is kept for diagnostics up to this size (older output is dropped).
		constexpr size_t c_MaxLogSize = 8 * 1024 * 1024;
		// Longer diagnostic messages (deeply nested template types) are cut; the log keeps them whole.
		constexpr size_t c_MaxDiagnosticMessageSize = 4096;
		constexpr size_t c_ErrorsInSummary = 5;
		constexpr size_t c_LogTailLines = 40;
		// After a process exited, its output is collected until it ends, but no longer than this: a process it started may
		// keep the output open (it is ended with the process tree).
		constexpr std::chrono::seconds c_OutputDrainLimit(5);
		// Longer output lines are split (e.g. a tool that never prints a newline).
		constexpr size_t c_MaxOutputLineSize = 64 * 1024;
		constexpr std::string_view c_CMakeError = "CMake Error";
		constexpr std::string_view c_CMakeWarning = "CMake Warning";

		bool IsVisualStudio(const ScriptBuildSettings& settings)
		{
			return settings.Generator.find("Visual Studio") != std::string::npos;
		}

		std::string ToUpper(std::string text)
		{
			for (char& character : text)
				character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
			return text;
		}

		// What the configured build tree depends on; a different stamp means the tree must be configured again.
		nlohmann::json MakeStamp(const ScriptBuildSettings& settings, const std::filesystem::path& sourceDirectory, const std::filesystem::path& binaryDirectory)
		{
			return {
				{ "Generator", settings.Generator },
				{ "Platform", settings.Platform },
				{ "Toolset", settings.Toolset },
				{ "CXXCompiler", FileSystem::ToUTF8(settings.CXXCompiler) },
				{ "MakeProgram", FileSystem::ToUTF8(settings.MakeProgram) },
				{ "Configuration", settings.Configuration },
				{ "EngineDirectory", FileSystem::ToUTF8(settings.EngineDirectory) },
				{ "SourceDirectory", FileSystem::ToUTF8(sourceDirectory) },
				{ "BinaryDirectory", FileSystem::ToUTF8(binaryDirectory) } };
		}

		std::string JoinArguments(const std::vector<std::string>& arguments)
		{
			std::string text;
			for (const std::string& argument : arguments)
			{
				text += text.empty() ? "" : " ";
				text += argument.find(' ') != std::string::npos ? "\"" + argument + "\"" : argument;
			}
			return text;
		}

		uint32_t ToNumber(const std::string& text)
		{
			uint64_t value = 0;
			for (char character : text)
			{
				if (!std::isdigit(static_cast<unsigned char>(character)))
					return 0;
				value = std::min<uint64_t>(value * 10 + static_cast<uint64_t>(character - '0'), UINT32_MAX);
			}
			return static_cast<uint32_t>(value);
		}

		std::string ToSeverity(const std::string& text)
		{
			return text == "warning" ? "warning" : "error";
		}

		std::string Trim(std::string_view text)
		{
			const size_t begin = text.find_first_not_of(" \t\r");
			if (begin == std::string_view::npos)
				return {};
			const size_t end = text.find_last_not_of(" \t\r");
			return std::string(text.substr(begin, end - begin + 1));
		}

		std::vector<std::string_view> SplitLines(std::string_view text)
		{
			std::vector<std::string_view> lines;
			size_t start = 0;
			while (start <= text.size())
			{
				size_t end = text.find('\n', start);
				if (end == std::string_view::npos)
					end = text.size();
				std::string_view line = text.substr(start, end - start);
				if (!line.empty() && line.back() == '\r')
					line.remove_suffix(1);
				lines.push_back(line);
				start = end + 1;
			}
			if (!lines.empty() && lines.back().empty())
				lines.pop_back();
			return lines;
		}

		bool IsBlank(char character)
		{
			return character == ' ' || character == '\t';
		}

		bool IsDigits(std::string_view text)
		{
			return !text.empty() && std::all_of(text.begin(), text.end(), [](char character) { return std::isdigit(static_cast<unsigned char>(character)) != 0; });
		}

		std::string CutMessage(std::string message)
		{
			if (message.size() <= c_MaxDiagnosticMessageSize)
				return message;
			// Cut at a character boundary: UTF-8 continuation bytes are 10xxxxxx.
			size_t size = c_MaxDiagnosticMessageSize;
			while (size > 0 && (static_cast<unsigned char>(message[size]) & 0xC0) == 0x80)
				size--;
			message.resize(size);
			return message + "...";
		}

		// Splits the location in front of a diagnostic into file, line and column: "File(12,5)" or "File(12)" (MSVC),
		// "file:12:5" or "file:12" (GCC, Clang) or a plain tool or file name ("LINK", "collect2").
		void ParseLocation(std::string_view location, ScriptDiagnostic& diagnostic)
		{
			if (!location.empty() && location.back() == ')')
			{
				const size_t open = location.rfind('(');
				if (open != std::string_view::npos && open > 0)
				{
					const std::string_view inside = location.substr(open + 1, location.size() - open - 2);
					const size_t comma = inside.find(',');
					const std::string_view line = inside.substr(0, comma);
					const std::string_view column = comma == std::string_view::npos ? std::string_view() : inside.substr(comma + 1);
					if (IsDigits(line) && (comma == std::string_view::npos || IsDigits(column)))
					{
						diagnostic.File = Trim(location.substr(0, open));
						diagnostic.Line = ToNumber(std::string(line));
						diagnostic.Column = comma == std::string_view::npos ? 0 : ToNumber(std::string(column));
						return;
					}
				}
			}

			// GCC and Clang: up to two trailing ":<number>" parts (line, then column).
			std::string_view rest = location;
			uint32_t numbers[2] = {};
			size_t numberCount = 0;
			while (numberCount < 2)
			{
				const size_t colon = rest.rfind(':');
				if (colon == std::string_view::npos || colon == 0 || !IsDigits(rest.substr(colon + 1)))
					break;
				numbers[numberCount++] = ToNumber(std::string(rest.substr(colon + 1)));
				rest = rest.substr(0, colon);
			}
			diagnostic.File = Trim(rest);
			diagnostic.Line = numberCount == 2 ? numbers[1] : numbers[0];
			diagnostic.Column = numberCount == 2 ? numbers[0] : 0;
		}

		// One diagnostic from a single line, or nothing. A diagnostic is "<location>: <severity>: <message>" (GCC, Clang)
		// or "<location>: <severity> <code>: <message> [<project>]" (MSVC, MSBuild, the linker), where the severity is
		// "error", "fatal error" or "warning" and the location the part before the first such marker. Linear in the
		// line's length (no backtracking), so arbitrarily long lines are safe.
		std::optional<ScriptDiagnostic> ParseDiagnosticLine(std::string_view line)
		{
			for (size_t colon = line.find(':'); colon != std::string_view::npos; colon = line.find(':', colon + 1))
			{
				size_t position = colon + 1;
				while (position < line.size() && IsBlank(line[position]))
					position++;

				std::string_view severity;
				for (std::string_view candidate : { std::string_view("fatal error"), std::string_view("error"), std::string_view("warning") })
				{
					if (line.substr(position, candidate.size()) == candidate)
					{
						severity = candidate;
						break;
					}
				}
				if (severity.empty())
					continue;
				position += severity.size();

				ScriptDiagnostic diagnostic;
				diagnostic.Severity = ToSeverity(std::string(severity));
				if (position < line.size() && line[position] == ':')
				{
					diagnostic.Message = Trim(line.substr(position + 1));
				}
				else
				{
					// "<severity> <code>:", the code being letters followed by digits ("C2065", "LNK1104", "MSB8066").
					if (position >= line.size() || !IsBlank(line[position]))
						continue;
					while (position < line.size() && IsBlank(line[position]))
						position++;
					const size_t codeStart = position;
					while (position < line.size() && std::isalpha(static_cast<unsigned char>(line[position])))
						position++;
					const size_t digitsStart = position;
					while (position < line.size() && std::isdigit(static_cast<unsigned char>(line[position])))
						position++;
					if (digitsStart == codeStart || position == digitsStart)
						continue;
					diagnostic.Code = std::string(line.substr(codeStart, position - codeStart));
					while (position < line.size() && IsBlank(line[position]))
						position++;
					if (position >= line.size() || line[position] != ':')
						continue;

					// MSBuild appends the project: "message [G:\Game\GameScripts.vcxproj]".
					std::string_view message = line.substr(position + 1);
					while (!message.empty() && (IsBlank(message.back()) || message.back() == '\r'))
						message.remove_suffix(1);
					if (!message.empty() && message.back() == ']')
					{
						const size_t open = message.rfind('[');
						if (open != std::string_view::npos && message.find(']', open) == message.size() - 1)
							message = message.substr(0, open);
					}
					diagnostic.Message = Trim(message);
				}

				ParseLocation(Trim(line.substr(0, colon)), diagnostic);
				if (diagnostic.File.empty())
					return std::nullopt;
				diagnostic.Message = CutMessage(std::move(diagnostic.Message));
				return diagnostic;
			}
			return std::nullopt;
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// Settings and commands
	////////////////////////////////////////////////////////////////////////////////

	ScriptBuildSettings ScriptBuildSettings::GetEngineDefaults()
	{
		ScriptBuildSettings settings;
		settings.CMake = FileSystem::FromUTF8(ST_SCRIPT_BUILD_CMAKE);
		settings.Generator = ST_SCRIPT_BUILD_GENERATOR;
		settings.Platform = ST_SCRIPT_BUILD_PLATFORM;
		settings.Toolset = ST_SCRIPT_BUILD_TOOLSET;
		settings.CXXCompiler = FileSystem::FromUTF8(ST_SCRIPT_BUILD_CXX_COMPILER);
		settings.MakeProgram = FileSystem::FromUTF8(ST_SCRIPT_BUILD_MAKE_PROGRAM);
		settings.Configuration = ST_SCRIPT_BUILD_CONFIGURATION;
		settings.EngineDirectory = FileSystem::FromUTF8(ST_SCRIPT_BUILD_ENGINE_DIR);
		return settings;
	}

	bool ScriptBuildSettings::IsMultiConfig() const
	{
		return IsVisualStudio(*this) || Generator == "Xcode" || Generator.find("Multi-Config") != std::string::npos;
	}

	std::vector<std::string> MakeScriptConfigureArguments(const ScriptBuildSettings& settings, const std::filesystem::path& sourceDirectory,
		const std::filesystem::path& buildDirectory, const std::filesystem::path& binaryDirectory)
	{
		std::vector<std::string> arguments = { "-S", FileSystem::ToUTF8(sourceDirectory), "-B", FileSystem::ToUTF8(buildDirectory), "-G", settings.Generator };
		if (!settings.Platform.empty())
			arguments.insert(arguments.end(), { "-A", settings.Platform });
		if (!settings.Toolset.empty())
			arguments.insert(arguments.end(), { "-T", settings.Toolset });
		// Visual Studio and Xcode pick their own compiler and build tool.
		const bool ownToolchain = IsVisualStudio(settings) || settings.Generator == "Xcode";
		if (!ownToolchain && !settings.CXXCompiler.empty())
			arguments.push_back("-DCMAKE_CXX_COMPILER=" + FileSystem::ToUTF8(settings.CXXCompiler));
		if (!ownToolchain && !settings.MakeProgram.empty())
			arguments.push_back("-DCMAKE_MAKE_PROGRAM=" + FileSystem::ToUTF8(settings.MakeProgram));
		// The build tree only needs the engine's configuration. Naming it also makes it exist for multi-config generators
		// when CMake does not know it (Dist; the StrataScriptCore package defines its flags).
		if (!settings.Configuration.empty())
			arguments.push_back((settings.IsMultiConfig() ? "-DCMAKE_CONFIGURATION_TYPES=" : "-DCMAKE_BUILD_TYPE=") + settings.Configuration);
		arguments.push_back("-DSTRATA_ENGINE_DIR=" + FileSystem::ToUTF8(settings.EngineDirectory));

		// Script modules are MODULE libraries, which always go to the library output directory. Multi-config generators
		// append the configuration to the plain variable, so the per-configuration one is set as well.
		const std::string binary = FileSystem::ToUTF8(binaryDirectory);
		arguments.push_back("-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=" + binary);
		if (settings.IsMultiConfig() && !settings.Configuration.empty())
			arguments.push_back("-DCMAKE_LIBRARY_OUTPUT_DIRECTORY_" + ToUpper(settings.Configuration) + "=" + binary);
		return arguments;
	}

	std::vector<std::string> MakeScriptBuildArguments(const ScriptBuildSettings& settings, const std::filesystem::path& buildDirectory)
	{
		std::vector<std::string> arguments = { "--build", FileSystem::ToUTF8(buildDirectory), "--parallel" };
		if (!settings.Configuration.empty())
			arguments.insert(arguments.end(), { "--config", settings.Configuration });
		// MSBuild leaves worker nodes behind by default; they would keep the output pipe (and the module's PDB) open.
		if (IsVisualStudio(settings))
			arguments.insert(arguments.end(), { "--", "/nodeReuse:false" });
		return arguments;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Diagnostics
	////////////////////////////////////////////////////////////////////////////////

	std::vector<ScriptDiagnostic> ParseScriptBuildDiagnostics(std::string_view log, size_t maxDiagnostics)
	{
		std::vector<ScriptDiagnostic> diagnostics;
		const std::vector<std::string_view> lines = SplitLines(log);
		for (size_t index = 0; index < lines.size() && diagnostics.size() < maxDiagnostics; index++)
		{
			const std::string_view line = lines[index];
			std::optional<ScriptDiagnostic> diagnostic;
			// CMake: "CMake Error at CMakeLists.txt:5 (find_package):" with the message on the following indented lines.
			const bool cmakeError = line.rfind(c_CMakeError, 0) == 0;
			if (cmakeError || line.rfind(c_CMakeWarning, 0) == 0)
			{
				ScriptDiagnostic cmake;
				cmake.Severity = cmakeError ? "error" : "warning";
				const size_t at = line.find(" at ");
				const size_t colon = line.rfind(':');
				if (at != std::string_view::npos && colon != std::string_view::npos && colon > at)
				{
					// "<file>:<line> (<command>):"
					std::string location(line.substr(at + 4, colon - at - 4));
					if (const size_t command = location.rfind(" ("); command != std::string::npos)
						location.resize(command);
					if (const size_t lineSeparator = location.rfind(':'); lineSeparator != std::string::npos)
					{
						cmake.File = location.substr(0, lineSeparator);
						cmake.Line = ToNumber(location.substr(lineSeparator + 1));
					}
					std::string message;
					for (size_t next = index + 1; next < lines.size() && !lines[next].empty() && (lines[next][0] == ' ' || lines[next][0] == '\t'); next++)
						message += (message.empty() ? "" : " ") + Trim(lines[next]);
					cmake.Message = CutMessage(std::move(message));
				}
				else if (const size_t separator = line.find(": "); separator != std::string_view::npos)
				{
					cmake.Message = Trim(line.substr(separator + 2));
				}
				if (!cmake.Message.empty())
					diagnostic = std::move(cmake);
			}
			else
			{
				diagnostic = ParseDiagnosticLine(line);
			}

			if (diagnostic && std::find(diagnostics.begin(), diagnostics.end(), *diagnostic) == diagnostics.end())
				diagnostics.push_back(std::move(*diagnostic));
		}
		return diagnostics;
	}

	std::string FormatScriptDiagnostic(const ScriptDiagnostic& diagnostic)
	{
		std::string text = diagnostic.File;
		if (diagnostic.Line > 0)
			text += diagnostic.Column > 0 ? fmt::format("({},{})", diagnostic.Line, diagnostic.Column) : fmt::format("({})", diagnostic.Line);
		text += text.empty() ? "" : ": ";
		text += diagnostic.Severity;
		if (!diagnostic.Code.empty())
			text += " " + diagnostic.Code;
		return text + ": " + diagnostic.Message;
	}

	std::string GetLogTail(std::string_view log, size_t lineCount)
	{
		const std::vector<std::string_view> lines = SplitLines(log);
		std::string tail;
		for (size_t index = lines.size() > lineCount ? lines.size() - lineCount : 0; index < lines.size(); index++)
		{
			tail += lines[index];
			tail += '\n';
		}
		return tail;
	}

	////////////////////////////////////////////////////////////////////////////////
	// OutputLineSplitter
	////////////////////////////////////////////////////////////////////////////////

	OutputLineSplitter::OutputLineSplitter(size_t maxLineSize)
		: m_MaxLineSize(std::max<size_t>(maxLineSize, 1))
	{
	}

	std::vector<std::string> OutputLineSplitter::Append(std::string_view output)
	{
		std::vector<std::string> lines;
		while (!output.empty())
		{
			const size_t newline = output.find('\n');
			std::string_view piece = output.substr(0, newline);
			while (!piece.empty())
			{
				const size_t take = std::min(piece.size(), m_MaxLineSize - m_Pending.size());
				m_Pending.append(piece.substr(0, take));
				piece.remove_prefix(take);
				m_PendingWasCut = false;
				if (m_Pending.size() == m_MaxLineSize)
				{
					lines.push_back(std::exchange(m_Pending, std::string()));
					m_PendingWasCut = true;
				}
			}
			if (newline == std::string_view::npos)
				break;

			if (!m_PendingWasCut)
			{
				if (!m_Pending.empty() && m_Pending.back() == '\r')
					m_Pending.pop_back();
				lines.push_back(std::exchange(m_Pending, std::string()));
			}
			m_PendingWasCut = false;
			output.remove_prefix(newline + 1);
		}
		return lines;
	}

	std::optional<std::string> OutputLineSplitter::Flush()
	{
		m_PendingWasCut = false;
		if (m_Pending.empty())
			return std::nullopt;
		if (m_Pending.back() == '\r')
			m_Pending.pop_back();
		return std::exchange(m_Pending, std::string());
	}

	void OutputLineSplitter::Reset()
	{
		m_Pending.clear();
		m_PendingWasCut = false;
	}

	////////////////////////////////////////////////////////////////////////////////
	// ScriptBuilder
	////////////////////////////////////////////////////////////////////////////////

	const char* ScriptBuildPhaseToString(ScriptBuildPhase phase)
	{
		switch (phase)
		{
			case ScriptBuildPhase::Idle:        return "Idle";
			case ScriptBuildPhase::Configuring: return "Configuring";
			case ScriptBuildPhase::Building:    return "Building";
		}
		return "Unknown";
	}

	ScriptBuilder::ScriptBuilder()
		: m_OutputLines(c_MaxOutputLineSize)
	{
	}

	ScriptBuilder::~ScriptBuilder()
	{
		Cancel();
	}

	double ScriptBuilder::GetElapsedSeconds() const
	{
		if (!IsRunning())
			return 0.0;
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_StartTime).count();
	}

	bool ScriptBuilder::Start(const Project& project, const ScriptBuildSettings& settings, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		if (IsRunning())
			return fail(fmt::format("Script build {} is still running ({:.0f} s so far); one build runs at a time: wait for it (script.status) and build again",
				GetCurrentID(), GetElapsedSeconds()));

		const std::filesystem::path sourceDirectory = project.GetScriptSourceDirectory();
		if (!FileSystem::IsRegularFile(sourceDirectory / "CMakeLists.txt"))
			return fail(fmt::format("The project has no script build: '{}' is missing (script.init creates it)", FileSystem::ToUTF8(sourceDirectory / "CMakeLists.txt")));

		m_Settings = settings;
		m_BuildDirectory = project.GetScriptBuildDirectory();
		m_Module = project.GetScriptModulePath();
		m_ModuleExistedBefore = FileSystem::IsRegularFile(m_Module);
		m_ModuleTimeBefore = FileSystem::GetLastWriteTime(m_Module).value_or(0);
		m_Log.clear();
		m_OutputLines.Reset();
		m_StartTime = std::chrono::steady_clock::now();
		m_LastResult.ID = 0;
		const uint64_t id = m_NextID++;

		// The configure step runs when the build tree is new or was configured differently. A tree configured with another
		// generator cannot be reused, so it is removed first.
		const std::filesystem::path binaryDirectory = project.GetScriptBinaryDirectory();
		const nlohmann::json stamp = MakeStamp(settings, sourceDirectory, binaryDirectory);
		const std::optional<std::string> stampText = FileSystem::ReadText(m_BuildDirectory / c_StampFile);
		const std::optional<nlohmann::json> previousStamp = stampText ? JsonUtils::Parse(*stampText) : std::nullopt;
		const bool configured = FileSystem::IsRegularFile(m_BuildDirectory / "CMakeCache.txt") && previousStamp && *previousStamp == stamp;
		if (!configured && FileSystem::Exists(m_BuildDirectory) && !FileSystem::Remove(m_BuildDirectory))
			return fail(fmt::format("Cannot remove the outdated script build directory '{}'", FileSystem::ToUTF8(m_BuildDirectory)));
		if (!FileSystem::CreateDirectories(m_BuildDirectory) || !FileSystem::CreateDirectories(binaryDirectory))
			return fail(fmt::format("Cannot create the script build directories in '{}'", FileSystem::ToUTF8(project.GetIntermediateDirectory())));

		m_Configured = !configured;
		std::string error;
		const std::vector<std::string> arguments = configured ? MakeScriptBuildArguments(settings, m_BuildDirectory)
			: MakeScriptConfigureArguments(settings, sourceDirectory, m_BuildDirectory, binaryDirectory);
		m_Phase = configured ? ScriptBuildPhase::Building : ScriptBuildPhase::Configuring;
		ST_INFO("Building the scripts of '{}' (build {}, {} {})", project.GetConfig().Name, id, settings.Generator, settings.Configuration);
		if (!configured && !FileSystem::WriteText(m_BuildDirectory / c_StampFile, ""))
		{
			Finish(false, fmt::format("Cannot write to the script build directory '{}'", FileSystem::ToUTF8(m_BuildDirectory)));
			return fail(m_LastResult.Error);
		}
		if (!StartProcess(arguments, &error))
		{
			Finish(false, error);
			return fail(error);
		}
		if (!configured)
		{
			// Written once the configure step succeeds; an empty stamp never matches, so a failed configure runs again.
			m_PendingStamp = JsonUtils::Dump(stamp, 1, '\t') + "\n";
		}
		return true;
	}

	bool ScriptBuilder::StartProcess(const std::vector<std::string>& arguments, std::string* outError)
	{
		ProcessSpecification specification;
		specification.Executable = FileSystem::IsRegularFile(m_Settings.CMake) ? m_Settings.CMake : std::filesystem::path("cmake");
		specification.Arguments = arguments;
		specification.Output = ProcessOutputMode::Capture;
		specification.HideWindow = true;
		// Cancelling (also by closing the project or the editor) must stop MSBuild, ninja, the compilers and the linker too,
		// or they keep writing the build and binary directories.
		specification.TerminateTree = true;

		const std::string commandLine = fmt::format("> {} {}", FileSystem::ToUTF8(specification.Executable), JoinArguments(arguments));
		m_Log += commandLine + "\n";
		ST_INFO("[Scripts] {}", commandLine);

		m_Process = CreateScope<Process>();
		m_Draining = false;
		if (!m_Process->Start(specification))
		{
			if (outError)
			{
				*outError = fmt::format("Cannot run CMake ('{}', configured with the engine, nor 'cmake' on PATH): {}", FileSystem::ToUTF8(m_Settings.CMake),
					m_Process->GetLastError());
			}
			m_Process.reset();
			return false;
		}
		return true;
	}

	void ScriptBuilder::CollectOutput(bool flushPartialLine)
	{
		std::vector<std::string> lines;
		if (m_Process)
			lines = m_OutputLines.Append(m_Process->TakeOutput());
		if (flushPartialLine)
		{
			if (std::optional<std::string> rest = m_OutputLines.Flush())
				lines.push_back(std::move(*rest));
		}
		for (const std::string& line : lines)
		{
			m_Log += line;
			m_Log += '\n';
			if (!line.empty())
				ST_INFO("[Scripts] {}", line);
		}

		if (m_Log.size() > c_MaxLogSize)
			m_Log.erase(0, m_Log.size() - c_MaxLogSize / 2);
	}

	bool ScriptBuilder::Update()
	{
		if (!IsRunning())
			return false;

		CollectOutput(false);
		if (!m_Draining)
		{
			if (m_Process->IsRunning())
				return false;
			m_Draining = true;
			m_ExitTime = std::chrono::steady_clock::now();
		}
		// The last lines (often the errors) may still be on their way: wait until the output ended.
		if (!m_Process->IsOutputFinished())
		{
			if (std::chrono::steady_clock::now() - m_ExitTime < c_OutputDrainLimit)
				return false;
			ST_WARN("[Scripts] The build's output did not end within {} s after CMake exited; a process it started still holds it",
				c_OutputDrainLimit.count());
		}

		CollectOutput(true);
		const int exitCode = m_Process->GetExitCode().value_or(-1);
		m_Process.reset();

		if (m_Phase == ScriptBuildPhase::Configuring)
		{
			if (exitCode != 0)
			{
				Finish(false, fmt::format("Configuring the scripts with CMake failed (exit code {})", exitCode));
				return true;
			}
			if (!FileSystem::WriteText(m_BuildDirectory / c_StampFile, m_PendingStamp))
			{
				Finish(false, fmt::format("Cannot write to the script build directory '{}'", FileSystem::ToUTF8(m_BuildDirectory)));
				return true;
			}
			std::string error;
			m_Phase = ScriptBuildPhase::Building;
			if (!StartProcess(MakeScriptBuildArguments(m_Settings, m_BuildDirectory), &error))
			{
				Finish(false, error);
				return true;
			}
			return false;
		}

		if (exitCode != 0)
		{
			Finish(false, fmt::format("Building the scripts failed (exit code {})", exitCode));
			return true;
		}
		if (!FileSystem::IsRegularFile(m_Module))
		{
			Finish(false, fmt::format("The build succeeded but produced no '{}'; keep the module's name and default output directory in the "
				"scripts' CMakeLists.txt", FileSystem::ToUTF8(m_Module)));
			return true;
		}
		Finish(true, {});
		return true;
	}

	void ScriptBuilder::Cancel()
	{
		if (!IsRunning())
			return;
		if (m_Process)
		{
			m_Process->Terminate();
			CollectOutput(true);
			m_Process.reset();
		}
		Finish(false, "The script build was cancelled");
	}

	void ScriptBuilder::Finish(bool success, std::string error)
	{
		ScriptBuildResult result;
		result.ID = m_NextID - 1;
		result.Success = success;
		result.Diagnostics = ParseScriptBuildDiagnostics(m_Log);
		result.LogTail = GetLogTail(m_Log, c_LogTailLines);
		result.Configured = m_Configured;
		result.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_StartTime).count();
		result.Module = m_Module;
		result.ModuleChanged = success && (!m_ModuleExistedBefore || FileSystem::GetLastWriteTime(m_Module).value_or(0) != m_ModuleTimeBefore);

		if (!success)
		{
			// The first errors make the message actionable on its own; the full list is in Diagnostics.
			std::vector<std::string> errors;
			for (const ScriptDiagnostic& diagnostic : result.Diagnostics)
			{
				if (diagnostic.Severity == "error" && errors.size() < c_ErrorsInSummary)
					errors.push_back(FormatScriptDiagnostic(diagnostic));
			}
			result.Error = std::move(error);
			if (!errors.empty())
			{
				for (const std::string& line : errors)
					result.Error += "\n" + line;
			}
			else
			{
				result.Error += "\nLast lines of the build log:\n" + GetLogTail(m_Log, 15);
			}
			ST_ERROR("Script build {} failed: {}", result.ID, result.Error);
		}
		else
		{
			ST_INFO("Script build {} finished in {:.1f} s{}", result.ID, result.Seconds, result.ModuleChanged ? "" : " (the module was up to date)");
		}

		m_Phase = ScriptBuildPhase::Idle;
		m_Draining = false;
		m_PendingStamp.clear();
		m_LastResult = std::move(result);
	}

}
