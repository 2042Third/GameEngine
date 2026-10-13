#include "Editor/ScriptBuild.h"

#include "Editor/ScriptBuildConfig.h"
#include "Editor/ScriptToolchain.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Hash.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Process.h>
#include <Strata/Project/Project.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <iterator>
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
		constexpr size_t c_FailureLogTailLines = 15;
		// After a process exited, its output is collected until it ends, but no longer than this: a process it started may
		// keep the output open (it is ended with the process tree).
		constexpr std::chrono::seconds c_OutputDrainLimit(5);
		// Longer output lines are split (e.g. a tool that never prints a newline).
		constexpr size_t c_MaxOutputLineSize = 64 * 1024;
		constexpr std::string_view c_CMakeError = "CMake Error";
		constexpr std::string_view c_CMakeWarning = "CMake Warning";
		constexpr const char* c_PresetsFile = "CMakePresets.json";
		constexpr const char* c_PresetName = "strata";
		// The vendor key that marks presets the editor wrote (and may rewrite).
		constexpr const char* c_PresetsVendor = "strata.dev/editor";

		bool IsVisualStudio(const ScriptBuildSettings& settings)
		{
			return settings.Generator.find("Visual Studio") != std::string::npos;
		}

		// CMake writes compile_commands.json for the Makefile and Ninja generators only.
		bool ExportsCompileCommands(const ScriptBuildSettings& settings)
		{
			return settings.Generator.find("Makefiles") != std::string::npos || settings.Generator.find("Ninja") != std::string::npos;
		}

		// The longest paths below a build directory at which MSBuild writes tracking logs (c_MSBuildMaxTrackedPathLength),
		// counting the separator after the build directory; 0 for generators without MSBuild.
		struct TrackedPathDepths
		{
			size_t Configure = 0; // CMake's compiler check while configuring
			size_t Target = 0;    // The module target's own build
		};

		TrackedPathDepths GetTrackedPathDepths(const ScriptBuildSettings& settings, std::string_view targetName)
		{
			if (!IsVisualStudio(settings))
				return {};
			// CMake's compiler check builds a project with random names of fixed length (TryCompile-xxxxxx, cmTC_xxxxx) in
			// the Debug configuration.
			constexpr std::string_view deepestConfigureLog =
				"/CMakeFiles/CMakeScratch/TryCompile-xxxxxx/cmTC_xxxxx.dir/Debug/cmTC_xxxxx.tlog/link-cvtres.write.1.tlog";
			// The target's own build logs into <target>.dir/<configuration>/<tracking name>.tlog/, whose longest file is the
			// custom build step's command log. MSBuild names the directory after the target, or, for names longer than 16
			// characters, after their first 8 characters and 8 of the project's GUID (Microsoft.BuildSteps.targets).
			constexpr size_t maxTrackingName = 16;
			constexpr size_t shortenedTrackingName = 8 + 1 + 8;
			constexpr std::string_view longestTargetLog = "/CustomBuild.command.1.tlog";
			const size_t trackingName = targetName.size() > maxTrackingName ? shortenedTrackingName : targetName.size();
			TrackedPathDepths depths;
			depths.Configure = deepestConfigureLog.size();
			depths.Target = 1 + targetName.size() + std::string_view(".dir/").size() + settings.Configuration.size() + 1 + trackingName +
				std::string_view(".tlog").size() + longestTargetLog.size();
			return depths;
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
			nlohmann::json stamp = {
				{ "Generator", settings.Generator },
				{ "Platform", settings.Platform },
				{ "Toolset", settings.Toolset },
				{ "CXXCompiler", FileSystem::ToUTF8(settings.CXXCompiler) },
				{ "MakeProgram", FileSystem::ToUTF8(settings.MakeProgram) },
				{ "Configuration", settings.Configuration },
				{ "EngineDirectory", FileSystem::ToUTF8(settings.EngineDirectory) },
				{ "SourceDirectory", FileSystem::ToUTF8(sourceDirectory) },
				{ "BinaryDirectory", FileSystem::ToUTF8(binaryDirectory) } };
			// Only where one is named, so that the trees of toolchains without one keep their stamps.
			if (!settings.GeneratorInstance.empty())
				stamp["GeneratorInstance"] = settings.GeneratorInstance;
			return stamp;
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

		bool StartsWithBlank(std::string_view line)
		{
			return !line.empty() && IsBlank(line.front());
		}

		// GNU ld reports undefined references without an "error:" marker (modules link with --no-undefined):
		// "Player.cpp:(.text+0x15): undefined reference to `Missing()'", with debug information "Player.cpp:12: ...", and
		// newer versions prefix the line with the linker ("/usr/bin/ld: Player.cpp:(.text+0x15): ...").
		std::optional<ScriptDiagnostic> ParseUndefinedReference(std::string_view line)
		{
			constexpr std::string_view c_Marker = ": undefined reference to ";
			const size_t marker = line.find(c_Marker);
			if (marker == std::string_view::npos)
				return std::nullopt;

			std::string_view location = line.substr(0, marker);
			if (const size_t prefix = location.rfind(": "); prefix != std::string_view::npos)
				location = location.substr(prefix + 2);
			if (const size_t section = location.find(":("); section != std::string_view::npos)
				location = location.substr(0, section);

			ScriptDiagnostic diagnostic;
			diagnostic.Severity = "error";
			diagnostic.Message = CutMessage(Trim(line.substr(marker + 2)));
			ParseLocation(Trim(location), diagnostic);
			if (diagnostic.File.empty())
				diagnostic.File = "ld";
			return diagnostic;
		}

		// Apple's linker lists undefined symbols in a block, without file or line:
		//   Undefined symbols for architecture arm64:      (ld-prime: "ld: Undefined symbols:")
		//     "Missing()", referenced from:
		//         Player::OnUpdate(float) in Player.cpp.o
		bool IsUndefinedSymbolsBlock(std::string_view line)
		{
			return line.rfind("Undefined symbols for architecture ", 0) == 0 || line.rfind("ld: Undefined symbols:", 0) == 0;
		}

		// One diagnostic per symbol of the block starting at lines[start]; returns the index of the block's last line.
		size_t ParseUndefinedSymbolsBlock(const std::vector<std::string_view>& lines, size_t start, std::vector<ScriptDiagnostic>& outDiagnostics)
		{
			constexpr std::string_view c_ReferencedFrom = ", referenced from:";
			size_t next = start + 1;
			while (next < lines.size() && StartsWithBlank(lines[next]))
			{
				const std::string symbolLine = Trim(lines[next++]);
				if (symbolLine.size() <= c_ReferencedFrom.size() || !symbolLine.ends_with(c_ReferencedFrom))
					continue;
				std::string symbol = symbolLine.substr(0, symbolLine.size() - c_ReferencedFrom.size());
				if (symbol.size() >= 2 && symbol.front() == '"' && symbol.back() == '"')
					symbol = symbol.substr(1, symbol.size() - 2);

				ScriptDiagnostic diagnostic;
				diagnostic.Severity = "error";
				diagnostic.File = "ld";
				diagnostic.Message = "undefined symbol " + symbol;
				if (next < lines.size() && StartsWithBlank(lines[next]) && !Trim(lines[next]).ends_with(c_ReferencedFrom))
				{
					// "<function> in <object file>"
					const std::string reference = Trim(lines[next]);
					const size_t in = reference.rfind(" in ");
					if (in != std::string::npos)
						diagnostic.File = reference.substr(in + 4);
					diagnostic.Message += " (referenced from " + (in != std::string::npos ? reference.substr(0, in) : reference) + ")";
				}
				diagnostic.Message = CutMessage(std::move(diagnostic.Message));
				outDiagnostics.push_back(std::move(diagnostic));
			}
			return next - 1;
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
		// The Visual Studio installation the toolchain was found in (several can have the generator's version).
		if (IsVisualStudio(settings) && !settings.GeneratorInstance.empty())
			arguments.push_back("-DCMAKE_GENERATOR_INSTANCE=" + settings.GeneratorInstance);
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
		// Tools that read compile_commands.json (clangd, IDEs opened on the build tree) see the scripts' include paths.
		if (ExportsCompileCommands(settings))
			arguments.push_back("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON");

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

	size_t GetScriptBuildTrackedPathDepth(const ScriptBuildSettings& settings, std::string_view targetName)
	{
		const TrackedPathDepths depths = GetTrackedPathDepths(settings, targetName);
		return std::max(depths.Configure, depths.Target);
	}

	std::optional<std::string> CheckScriptBuildPathLength(const ScriptBuildSettings& settings, const std::filesystem::path& buildDirectory,
		std::string_view targetName)
	{
		const TrackedPathDepths depths = GetTrackedPathDepths(settings, targetName);
		// native(): counted in the units the operating system counts (UTF-16 on Windows, the only home of MSBuild).
		const size_t longest = buildDirectory.native().size() + std::max(depths.Configure, depths.Target);
		if (!IsVisualStudio(settings) || longest <= c_MSBuildMaxTrackedPathLength)
			return std::nullopt;
		const size_t excess = longest - c_MSBuildMaxTrackedPathLength;
		const std::string shorterName = depths.Target > depths.Configure
			? fmt::format(", or give its script module a shorter name than '{}'", targetName) : std::string();
		const std::optional<std::filesystem::path> home = Platform::FindHomeDirectory();
		const std::string shorterPlace = home ? fmt::format("such as one in '{}'", FileSystem::ToUTF8((*home / "StrataProjects").make_preferred()))
			: std::string("such as one near the root of a drive");
		return fmt::format("The project's path is too long to build its scripts with {}: the build would write files at paths of {} characters below "
			"'{}', and MSBuild's file tracker cannot create files at paths longer than {} characters, even where Windows allows long paths. Move "
			"the project to a folder whose path is at least {} {} shorter, {}{}", settings.Generator, longest, FileSystem::ToUTF8(buildDirectory),
			c_MSBuildMaxTrackedPathLength, excess, excess == 1 ? "character" : "characters", shorterPlace, shorterName);
	}

	std::filesystem::path GetScriptBuildsRoot()
	{
		if (const std::optional<std::string> overridden = Platform::GetEnvVar("STRATA_SCRIPT_BUILDS_DIR"); overridden && !overridden->empty())
			return FileSystem::FromUTF8(*overridden);
		return Platform::GetUserDataDirectory("Strata") / "Builds";
	}

	std::filesystem::path GetPerUserScriptBuildDirectory(const Project& project)
	{
		// Named by the project's place, so that each project keeps its own tree across sessions.
		std::error_code error;
		const std::filesystem::path absolute = std::filesystem::absolute(project.GetProjectDirectory(), error);
		const std::string directory = FileSystem::ToUTF8((error ? project.GetProjectDirectory() : absolute).lexically_normal());
		return GetScriptBuildsRoot() / fmt::format("{:016x}", Hash::FNV1a(directory));
	}

	std::optional<ScriptBuildLocation> ResolveScriptBuildLocation(const Project& project, const ScriptBuildSettings& settings, std::string* outError)
	{
		const std::string module = project.GetScriptModuleName();
		ScriptBuildLocation location;
		location.BuildDirectory = project.GetScriptBuildDirectory();
		location.IDEBuildDirectory = project.GetIntermediateDirectory() / "Scripts" / "IDE";
		const std::optional<std::string> projectProblem = CheckScriptBuildPathLength(settings, location.BuildDirectory, module);
		if (!projectProblem)
			return location;

		// The per-user directory is short: the project's path no longer matters there.
		const std::filesystem::path perUser = GetPerUserScriptBuildDirectory(project);
		if (CheckScriptBuildPathLength(settings, perUser / "Build", module))
		{
			if (outError)
				*outError = *projectProblem;
			return std::nullopt;
		}
		location.BuildDirectory = perUser / "Build";
		location.IDEBuildDirectory = perUser / "IDE";
		location.MovedReason = fmt::format("The project's path is too long for MSBuild's file tracker to build its scripts in '{}' (it cannot create "
			"files at paths over {} characters), so they build in the per-user directory '{}'", FileSystem::ToUTF8(project.GetScriptBuildDirectory()),
			c_MSBuildMaxTrackedPathLength, FileSystem::ToUTF8(location.BuildDirectory));
		return location;
	}

	std::string MakeScriptCMakePresets(const Project& project, const ScriptBuildSettings& settings)
	{
		// Preferences are no toolchain: IDEs would get the build machine's.
		if (settings.FindToolchain)
			return {};
		const std::optional<ScriptBuildLocation> location = ResolveScriptBuildLocation(project, settings);
		if (!location)
			return {};

		// The cache variables of the editor's own configure step, for the IDE's build tree: the same toolchain, engine and
		// module output, so that what the IDE builds is what the editor loads (and hot reloads).
		nlohmann::json cacheVariables = nlohmann::json::object();
		const std::vector<std::string> arguments = MakeScriptConfigureArguments(settings, project.GetScriptSourceDirectory(), location->IDEBuildDirectory,
			project.GetScriptBinaryDirectory());
		for (const std::string& argument : arguments)
		{
			if (argument.rfind("-D", 0) != 0)
				continue;
			const size_t equals = argument.find('=');
			if (equals != std::string::npos)
				cacheVariables[argument.substr(2, equals - 2)] = argument.substr(equals + 1);
		}
		cacheVariables["CMAKE_EXPORT_COMPILE_COMMANDS"] = "ON";

		nlohmann::json configure = {
			{ "name", c_PresetName },
			{ "displayName", fmt::format("Strata ({}, {})", settings.Generator, settings.Configuration) },
			{ "description", "The Strata engine's toolchain and configuration, as the editor builds the scripts (written by the editor)" },
			{ "generator", settings.Generator },
			{ "binaryDir", FileSystem::ToUTF8(location->IDEBuildDirectory) },
			{ "cacheVariables", std::move(cacheVariables) } };
		if (!settings.Platform.empty())
			configure["architecture"] = { { "value", settings.Platform }, { "strategy", "set" } };
		if (!settings.Toolset.empty())
			configure["toolset"] = { { "value", settings.Toolset }, { "strategy", "set" } };
		if (FileSystem::IsRegularFile(settings.CMake))
			configure["cmakeExecutable"] = FileSystem::ToUTF8(settings.CMake);

		nlohmann::json build = { { "name", c_PresetName }, { "configurePreset", c_PresetName } };
		if (!settings.Configuration.empty())
			build["configuration"] = settings.Configuration;

		const nlohmann::json presets = {
			{ "version", 3 },
			{ "cmakeMinimumRequired", { { "major", 3 }, { "minor", 25 }, { "patch", 0 } } },
			{ "configurePresets", nlohmann::json::array({ std::move(configure) }) },
			{ "buildPresets", nlohmann::json::array({ std::move(build) }) },
			{ "vendor", { { c_PresetsVendor, { { "note", "Written by the Strata editor for this machine's engine and toolchain; it rewrites the file "
				"when they change. Remove this vendor entry to keep your own." } } } } } };
		return JsonUtils::Dump(presets, 2, ' ') + "\n";
	}

	bool UpdateScriptCMakePresets(const Project& project, const ScriptBuildSettings& settings, bool* outWritten, std::string* outError)
	{
		if (outWritten)
			*outWritten = false;
		const std::filesystem::path directory = project.GetScriptSourceDirectory();
		const std::filesystem::path path = directory / c_PresetsFile;
		const std::string content = MakeScriptCMakePresets(project, settings);
		if (content.empty())
			return true; // The project cannot build its scripts here, or the toolchain is not known yet: nothing for an IDE either

		if (const std::optional<std::string> existing = FileSystem::ReadText(path))
		{
			if (*existing == content)
				return true;
			// Presets of the project's own (or edited to be) are left alone.
			const std::optional<nlohmann::json> json = JsonUtils::Parse(*existing);
			const bool ours = json && json->is_object() && json->contains("vendor") && (*json)["vendor"].is_object() && (*json)["vendor"].contains(c_PresetsVendor);
			if (!ours)
				return true;
		}
		else if (FileSystem::Exists(path))
		{
			return true;
		}

		if (!FileSystem::CreateDirectories(directory) || !FileSystem::WriteText(path, content))
		{
			if (outError)
				*outError = fmt::format("Cannot write '{}'", FileSystem::ToUTF8(path));
			return false;
		}
		const std::filesystem::path gitIgnore = directory / ".gitignore";
		if (!FileSystem::Exists(gitIgnore) && !FileSystem::WriteText(gitIgnore, "# This machine's engine and toolchain (the Strata editor writes it)\n/CMakePresets.json\n"))
		{
			if (outError)
				*outError = fmt::format("Cannot write '{}'", FileSystem::ToUTF8(gitIgnore));
			return false;
		}
		if (outWritten)
			*outWritten = true;
		return true;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Diagnostics
	////////////////////////////////////////////////////////////////////////////////

	std::vector<ScriptDiagnostic> ParseScriptBuildDiagnostics(std::string_view log, size_t maxDiagnostics)
	{
		std::vector<ScriptDiagnostic> diagnostics;
		auto add = [&](ScriptDiagnostic diagnostic)
		{
			if (diagnostics.size() < maxDiagnostics && std::find(diagnostics.begin(), diagnostics.end(), diagnostic) == diagnostics.end())
				diagnostics.push_back(std::move(diagnostic));
		};

		const std::vector<std::string_view> lines = SplitLines(log);
		for (size_t index = 0; index < lines.size() && diagnostics.size() < maxDiagnostics; index++)
		{
			const std::string_view line = lines[index];
			// CMake: "CMake Error at CMakeLists.txt:5 (find_package):" or "CMake Error in CMakeLists.txt:" (generation, no
			// line) with the message on the following indented lines; or "CMake Error: <message>".
			const bool cmakeError = line.rfind(c_CMakeError, 0) == 0;
			if (cmakeError || line.rfind(c_CMakeWarning, 0) == 0)
			{
				ScriptDiagnostic cmake;
				cmake.Severity = cmakeError ? "error" : "warning";
				std::string_view rest = line.substr(cmakeError ? c_CMakeError.size() : c_CMakeWarning.size());
				// "CMake Warning (dev) at ...".
				if (rest.rfind(" (", 0) == 0)
				{
					const size_t close = rest.find(')');
					rest = close == std::string_view::npos ? std::string_view() : rest.substr(close + 1);
				}
				if ((rest.rfind(" at ", 0) == 0 || rest.rfind(" in ", 0) == 0) && rest.size() > 5 && rest.back() == ':')
				{
					// "<file>[:<line>] [(<command>)]"
					std::string location = Trim(rest.substr(4, rest.size() - 5));
					if (const size_t command = location.rfind(" ("); command != std::string::npos)
						location.resize(command);
					const size_t lineSeparator = location.rfind(':');
					if (lineSeparator != std::string::npos && IsDigits(std::string_view(location).substr(lineSeparator + 1)))
					{
						cmake.Line = ToNumber(location.substr(lineSeparator + 1));
						location.resize(lineSeparator);
					}
					cmake.File = std::move(location);
					std::string message;
					for (size_t next = index + 1; next < lines.size() && StartsWithBlank(lines[next]); next++)
						message += (message.empty() ? "" : " ") + Trim(lines[next]);
					cmake.Message = CutMessage(std::move(message));
				}
				else if (rest.rfind(": ", 0) == 0)
				{
					cmake.Message = CutMessage(Trim(rest.substr(2)));
				}
				if (!cmake.Message.empty())
					add(std::move(cmake));
				continue;
			}

			if (IsUndefinedSymbolsBlock(line))
			{
				std::vector<ScriptDiagnostic> symbols;
				index = ParseUndefinedSymbolsBlock(lines, index, symbols);
				for (ScriptDiagnostic& symbol : symbols)
					add(std::move(symbol));
				continue;
			}
			// Apple's linker ends a failed link with this summary, without a severity.
			if (line.rfind("ld: symbol(s) not found", 0) == 0)
			{
				ScriptDiagnostic summary;
				summary.File = "ld";
				summary.Severity = "error";
				summary.Message = Trim(line.substr(4));
				add(std::move(summary));
				continue;
			}

			if (std::optional<ScriptDiagnostic> diagnostic = ParseDiagnosticLine(line))
				add(std::move(*diagnostic));
			else if (std::optional<ScriptDiagnostic> reference = ParseUndefinedReference(line))
				add(std::move(*reference));
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

	bool IsScriptBuildSummary(const ScriptDiagnostic& diagnostic)
	{
		const std::string_view message = diagnostic.Message;
		if (message.find("ld returned") != std::string_view::npos && message.find("exit status") != std::string_view::npos)
			return true;
		if (message.rfind("linker command failed", 0) == 0 || message.rfind("symbol(s) not found", 0) == 0)
			return true;
		// LNK1120: "<n> unresolved externals" after the LNK2019/LNK2001 errors naming them.
		if (diagnostic.Code == "LNK1120")
			return true;
		// MSBuild: "The command ... exited with code 1", "\"CL.exe\" exited with code 2".
		return diagnostic.Code.rfind("MSB", 0) == 0 && message.find("exited with code") != std::string_view::npos;
	}

	std::string DescribeScriptBuildFailure(const std::string& reason, const std::vector<ScriptDiagnostic>& diagnostics, std::string_view log)
	{
		std::string description = reason;
		size_t errors = 0;
		bool onlySummaries = true;
		for (const ScriptDiagnostic& diagnostic : diagnostics)
		{
			if (diagnostic.Severity != "error")
				continue;
			onlySummaries = onlySummaries && IsScriptBuildSummary(diagnostic);
			if (errors++ < c_ErrorsInSummary)
				description += "\n" + FormatScriptDiagnostic(diagnostic);
		}
		if (onlySummaries)
			description += "\nLast lines of the build log:\n" + GetLogTail(log, c_FailureLogTailLines);
		return description;
	}

	std::optional<ScriptDiagnostic> FindSourceReference(std::string_view message)
	{
		constexpr std::string_view c_CodeExtensions[] = { ".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh", ".hxx", ".inl", ".ipp", ".cmake", ".txt" };
		const auto isCode = [&c_CodeExtensions](const std::string& file)
		{
			const size_t dot = file.rfind('.');
			if (dot == std::string::npos)
				return false;
			std::string extension = file.substr(dot);
			for (char& character : extension)
				character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			return std::find(std::begin(c_CodeExtensions), std::end(c_CodeExtensions), extension) != std::end(c_CodeExtensions);
		};

		constexpr std::string_view c_BuildPrefix = "[Scripts] ";
		if (message.rfind(c_BuildPrefix, 0) == 0)
			message.remove_prefix(c_BuildPrefix.size());
		if (std::optional<ScriptDiagnostic> diagnostic = ParseDiagnosticLine(message); diagnostic && diagnostic->Line > 0 && isCode(diagnostic->File))
			return diagnostic;

		// "... at <file>(<line>) ...": the location of a crash report.
		for (size_t at = message.find(" at "); at != std::string_view::npos; at = message.find(" at ", at + 1))
		{
			const size_t start = at + 4;
			const size_t open = message.find('(', start);
			const size_t close = open == std::string_view::npos ? std::string_view::npos : message.find(')', open);
			if (close == std::string_view::npos)
				continue;
			const std::string_view line = message.substr(open + 1, close - open - 1);
			const std::string file(Trim(message.substr(start, open - start)));
			if (!IsDigits(line) || !isCode(file))
				continue;
			ScriptDiagnostic reference;
			reference.File = file;
			reference.Line = ToNumber(std::string(line));
			reference.Severity = "error";
			return reference;
		}
		return std::nullopt;
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

	// Filled by the job that looks for a toolchain; read by the main thread once Done.
	struct ScriptBuilder::ToolchainSearch
	{
		std::atomic<bool> Done = false;
		ScriptToolchainResolution Result;
	};

	const char* ScriptBuildPhaseToString(ScriptBuildPhase phase)
	{
		switch (phase)
		{
			case ScriptBuildPhase::Idle:        return "Idle";
			case ScriptBuildPhase::Configuring: return "Configuring";
			case ScriptBuildPhase::Building:    return "Building";
			case ScriptBuildPhase::FindingToolchain: return "FindingToolchain";
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
		if (!settings.FindToolchain)
			return Begin(project, settings, false, outError);
		// The toolchain found for these settings before serves while it is there: only the first build looks (and one after
		// the tools changed).
		if (const ScriptBuildSettings* found = GetFoundToolchain(settings); found && IsScriptToolchainPresent(*found))
			return Begin(project, *found, false, outError);

		m_Settings = settings;
		m_PendingProject = CreateScope<Project>(project);
		m_BuildDirectory.clear();
		m_Module = project.GetScriptModulePath();
		m_ModuleExistedBefore = FileSystem::IsRegularFile(m_Module);
		m_ModuleTimeBefore = FileSystem::GetLastWriteTime(m_Module).value_or(0);
		m_Configured = false;
		m_Log.clear();
		m_OutputLines.Reset();
		m_StartTime = std::chrono::steady_clock::now();
		m_LastResult.ID = 0;
		const uint64_t id = m_NextID++;
		m_Phase = ScriptBuildPhase::FindingToolchain;
		ST_INFO("Building the scripts of '{}' (build {}): finding a C++ toolchain on this machine", project.GetConfig().Name, id);
		m_Log += "Finding a C++ toolchain on this machine\n";

		Ref<ToolchainSearch> search = CreateRef<ToolchainSearch>();
		m_ToolchainSearch = search;
		std::function<Scope<ScriptToolchainEnvironment>()> environment = m_ToolchainEnvironment ? m_ToolchainEnvironment
			: std::function<Scope<ScriptToolchainEnvironment>()>(&CreateSystemScriptToolchainEnvironment);
		// It runs programs (vswhere, cmake --version) and waits for them: an I/O job, never the frame. A cancelled build drops
		// the search, which the job alone then holds.
		JobSystem::SubmitIO([search, settings, environment = std::move(environment)]()
		{
			const Scope<ScriptToolchainEnvironment> machine = environment();
			search->Result = ResolveScriptToolchain(settings, GetScriptToolchainHost(), *machine);
			search->Done.store(true, std::memory_order_release);
		});
		return true;
	}

	bool ScriptBuilder::Begin(const Project& project, const ScriptBuildSettings& settings, bool counted, std::string* outError)
	{
		// What stops a build before it starts a process: a build that already counts (it found its toolchain) finishes as
		// failed; one that does not is refused, as if it had never started.
		auto fail = [this, counted, outError](std::string message)
		{
			if (counted)
				Finish(false, message);
			if (outError)
				*outError = std::move(message);
			return false;
		};
		auto finishFailed = [this, outError](std::string message)
		{
			Finish(false, std::move(message));
			if (outError)
				*outError = m_LastResult.Error;
			return false;
		};

		const std::filesystem::path sourceDirectory = project.GetScriptSourceDirectory();
		// Refused before anything is written: the build would fail later with errors that do not name the cause.
		std::string locationError;
		const std::optional<ScriptBuildLocation> location = ResolveScriptBuildLocation(project, settings, &locationError);
		if (!location)
			return fail(std::move(locationError));
		if (!location->MovedReason.empty())
		{
			// The per-user tree holds object files that end up in code the engine loads: only the user may change it.
			std::string privateError;
			if (!Platform::EnsurePrivateDirectory(location->BuildDirectory.parent_path(), &privateError))
				return fail(fmt::format("Cannot create the per-user script build directory '{}': {}", FileSystem::ToUTF8(location->BuildDirectory.parent_path()),
					privateError));
		}
		// IDEs follow the engine's toolchain too; a presets file the project wrote itself is kept.
		std::string presetsError;
		if (!UpdateScriptCMakePresets(project, settings, nullptr, &presetsError))
			ST_WARN("[Scripts] {}", presetsError);

		m_Settings = settings;
		m_BuildDirectory = location->BuildDirectory;
		m_Module = project.GetScriptModulePath();
		if (!counted)
		{
			m_ModuleExistedBefore = FileSystem::IsRegularFile(m_Module);
			m_ModuleTimeBefore = FileSystem::GetLastWriteTime(m_Module).value_or(0);
			m_Log.clear();
			m_OutputLines.Reset();
			m_StartTime = std::chrono::steady_clock::now();
			m_LastResult.ID = 0;
			m_NextID++;
		}
		const uint64_t id = m_NextID - 1;

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
		if (!location->MovedReason.empty())
		{
			m_Log += location->MovedReason + "\n";
			ST_INFO("[Scripts] {}", location->MovedReason);
		}
		if (!configured && !FileSystem::WriteText(m_BuildDirectory / c_StampFile, ""))
			return finishFailed(fmt::format("Cannot write to the script build directory '{}'", FileSystem::ToUTF8(m_BuildDirectory)));
		if (!StartProcess(arguments, &error))
			return finishFailed(error);
		if (!configured)
		{
			// Written once the configure step succeeds; an empty stamp never matches, so a failed configure runs again.
			m_PendingStamp = JsonUtils::Dump(stamp, 1, '\t') + "\n";
		}
		return true;
	}

	void ScriptBuilder::FinishToolchainSearch()
	{
		const Ref<ToolchainSearch> search = std::move(m_ToolchainSearch);
		m_ToolchainSearch.reset();
		const Scope<Project> project = std::move(m_PendingProject);
		m_PendingProject.reset();
		const ScriptToolchainResolution& resolution = search->Result;
		if (!resolution.Settings)
		{
			// One row for the Problems panel and the notification, saying what to install; the error leads to it.
			ScriptDiagnostic diagnostic;
			diagnostic.Severity = "error";
			diagnostic.Message = resolution.Error;
			Finish(false, "No C++ toolchain to build the scripts with was found on this machine", false, std::move(diagnostic));
			return;
		}
		m_FoundToolchainPreferences = m_Settings;
		m_FoundToolchain = resolution.Settings;
		m_FoundToolchainDescription = resolution.Description;
		const std::string found = "Found " + resolution.Description;
		m_Log += found + "\n";
		ST_INFO("[Scripts] {}", found);
		// A build that cannot begin finishes as failed there.
		Begin(*project, *m_FoundToolchain, true, nullptr);
	}

	const ScriptBuildSettings* ScriptBuilder::GetFoundToolchain(const ScriptBuildSettings& settings) const
	{
		return m_FoundToolchain && m_FoundToolchainPreferences == settings ? &*m_FoundToolchain : nullptr;
	}

	void ScriptBuilder::SetToolchainEnvironment(std::function<Scope<ScriptToolchainEnvironment>()> factory)
	{
		m_ToolchainEnvironment = std::move(factory);
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
		if (m_Phase == ScriptBuildPhase::FindingToolchain)
		{
			if (!m_ToolchainSearch->Done.load(std::memory_order_acquire))
				return false;
			FinishToolchainSearch();
			// Finished in this call when no toolchain was found or the build could not begin.
			return !IsRunning();
		}

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
		// A search that still runs ends on its own; its result is dropped with it.
		m_ToolchainSearch.reset();
		m_PendingProject.reset();
		Finish(false, "The script build was cancelled", true);
	}

	void ScriptBuilder::Finish(bool success, std::string error, bool cancelled, std::optional<ScriptDiagnostic> diagnostic)
	{
		ScriptBuildResult result;
		result.ID = m_NextID - 1;
		result.Success = success;
		result.Cancelled = cancelled;
		result.Diagnostics = ParseScriptBuildDiagnostics(m_Log);
		if (diagnostic)
			result.Diagnostics.insert(result.Diagnostics.begin(), std::move(*diagnostic));
		result.Settings = m_Settings;
		result.LogTail = GetLogTail(m_Log, c_LogTailLines);
		result.Configured = m_Configured;
		result.BuildDirectory = m_BuildDirectory;
		result.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_StartTime).count();
		result.Module = m_Module;
		result.ModuleChanged = success && (!m_ModuleExistedBefore || FileSystem::GetLastWriteTime(m_Module).value_or(0) != m_ModuleTimeBefore);

		if (cancelled)
		{
			// Someone ended it: nothing failed, and the output so far says nothing about the scripts.
			result.Error = std::move(error);
			ST_INFO("Script build {} was cancelled", result.ID);
		}
		else if (!success)
		{
			// The first errors make the message actionable on its own; the full list is in Diagnostics.
			result.Error = DescribeScriptBuildFailure(error, result.Diagnostics, m_Log);
			ST_ERROR("Script build {} failed: {}", result.ID, result.Error);
		}
		else
		{
			ST_INFO("Script build {} finished in {:.1f} s{}", result.ID, result.Seconds, result.ModuleChanged ? "" : " (the module was up to date)");
		}

		m_Phase = ScriptBuildPhase::Idle;
		m_Draining = false;
		m_PendingStamp.clear();
		m_ToolchainSearch.reset();
		m_PendingProject.reset();
		m_LastResult = std::move(result);
	}

}
