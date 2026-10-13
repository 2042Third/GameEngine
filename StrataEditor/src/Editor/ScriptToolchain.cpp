#include "Editor/ScriptToolchain.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/PlatformDetection.h>
#include <Strata/Core/Process.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <utility>

namespace Strata
{

	namespace
	{

		// Larger markers are not ours.
		constexpr uint64_t c_MaxMarkerSize = 64 * 1024;
		// vswhere and `cmake --version` answer in well under a second; a program that hangs must not hold the build forever.
		constexpr std::chrono::seconds c_ProbeTimeout(30);
		constexpr const char* c_VisualStudioCppComponent = "Microsoft.VisualStudio.Component.VC.Tools.x86.x64";
		constexpr const char* c_SDKConfigFile = "StrataScriptCoreConfig.cmake";
		constexpr ScriptToolVersion c_MinimumCMake { 3, 25, 0 };
		constexpr ScriptToolVersion c_MinimumCMakeVisualStudio2026 { 4, 2, 0 };
		// Visual Studio 2022 before 17.10 has an MSVC older than 19.40, the oldest the engine's C++20 code is built with.
		constexpr ScriptToolVersion c_MinimumVisualStudio { 17, 10, 0 };

		std::string ToUTF8(const std::filesystem::path& path)
		{
			return FileSystem::ToUTF8(path);
		}

		// "3.31.6", "17.14.36310.24", "3.28.1-dirty": up to three numbers, anything after them ignored. Nothing without a number.
		std::optional<ScriptToolVersion> ParseVersionNumbers(std::string_view text)
		{
			uint32_t parts[3] = {};
			size_t count = 0;
			size_t position = 0;
			while (count < 3)
			{
				const size_t start = position;
				uint64_t value = 0;
				while (position < text.size() && std::isdigit(static_cast<unsigned char>(text[position])))
				{
					value = std::min<uint64_t>(value * 10 + static_cast<uint64_t>(text[position] - '0'), UINT32_MAX);
					position++;
				}
				if (position == start)
					break;
				parts[count++] = static_cast<uint32_t>(value);
				if (position >= text.size() || text[position] != '.')
					break;
				position++;
			}
			if (count == 0)
				return std::nullopt;
			return ScriptToolVersion { parts[0], parts[1], parts[2] };
		}

		std::string Trim(std::string_view text)
		{
			const size_t begin = text.find_first_not_of(" \t\r\n");
			if (begin == std::string_view::npos)
				return {};
			const size_t end = text.find_last_not_of(" \t\r\n");
			return std::string(text.substr(begin, end - begin + 1));
		}

		std::string JoinSentences(const std::vector<std::string>& sentences)
		{
			std::string text;
			for (const std::string& sentence : sentences)
				text += (text.empty() ? "" : " ") + sentence;
			return text;
		}

		// A CMake the resolution may use, with its version once asked for.
		struct CMakeCandidate
		{
			std::filesystem::path Path;
			std::optional<ScriptToolVersion> Version; // Nothing when it does not run or prints no version
			bool Probed = false;
		};

		class CMakeCandidates
		{
		public:
			explicit CMakeCandidates(const ScriptToolchainEnvironment& environment)
				: m_Environment(environment)
			{
			}

			// Adds an existing file once (the same CMake can be on PATH and in Program Files).
			void Add(const std::filesystem::path& path)
			{
				if (path.empty() || !m_Environment.IsFile(path))
					return;
				const std::filesystem::path normal = path.lexically_normal();
				for (const CMakeCandidate& candidate : m_Candidates)
				{
					if (candidate.Path == normal)
						return;
				}
				m_Candidates.push_back({ normal, std::nullopt, false });
			}

			// The first candidate (in the order added, starting at `first`) whose version reaches `minimum`.
			const CMakeCandidate* FindFirst(const ScriptToolVersion& minimum, size_t first = 0)
			{
				for (size_t index = first; index < m_Candidates.size(); index++)
				{
					CMakeCandidate& candidate = m_Candidates[index];
					Probe(candidate);
					if (candidate.Version && *candidate.Version >= minimum)
						return &candidate;
				}
				return nullptr;
			}

			size_t GetCount() const { return m_Candidates.size(); }

			// "CMake 3.22.1 (/usr/bin/cmake)" for every candidate that was asked and is older than `minimum` or does not run.
			std::vector<std::string> DescribeUnusable(const ScriptToolVersion& minimum) const
			{
				std::vector<std::string> unusable;
				for (const CMakeCandidate& candidate : m_Candidates)
				{
					if (!candidate.Probed)
						continue;
					if (!candidate.Version)
						unusable.push_back(fmt::format("'{}' does not run", ToUTF8(candidate.Path)));
					else if (*candidate.Version < minimum)
						unusable.push_back(fmt::format("CMake {} ('{}') is too old", FormatScriptToolVersion(*candidate.Version), ToUTF8(candidate.Path)));
				}
				return unusable;
			}
		private:
			void Probe(CMakeCandidate& candidate)
			{
				if (candidate.Probed)
					return;
				candidate.Probed = true;
				if (const std::optional<std::string> output = m_Environment.Run(candidate.Path, { "--version" }))
					candidate.Version = ParseCMakeVersion(*output);
			}
		private:
			const ScriptToolchainEnvironment& m_Environment;
			std::vector<CMakeCandidate> m_Candidates;
		};

		std::string DescribeUnusableCMakes(const CMakeCandidates& candidates, const ScriptToolVersion& minimum)
		{
			const std::vector<std::string> unusable = candidates.DescribeUnusable(minimum);
			if (unusable.empty())
				return {};
			std::string text = "Found: ";
			for (size_t index = 0; index < unusable.size(); index++)
				text += (index == 0 ? "" : "; ") + unusable[index];
			return text + ".";
		}

		bool IsInside(const std::filesystem::path& file, const std::filesystem::path& directory)
		{
			if (file.empty() || directory.empty())
				return false;
			const std::filesystem::path relative = file.lexically_normal().lexically_relative(directory.lexically_normal());
			return !relative.empty() && *relative.begin() != "..";
		}

		////////////////////////////////////////////////////////////////////////////////
		// Windows: Visual Studio and CMake
		////////////////////////////////////////////////////////////////////////////////

		std::optional<std::vector<VisualStudioInstance>> FindVisualStudioInstances(const ScriptToolchainEnvironment& environment)
		{
			std::vector<std::filesystem::path> locations;
			// The Visual Studio Installer always puts vswhere here (also for the Build Tools).
			if (const std::optional<std::string> programFiles = environment.GetEnvVar("ProgramFiles(x86)"); programFiles && !programFiles->empty())
				locations.push_back(FileSystem::FromUTF8(*programFiles) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe");
			if (const std::optional<std::filesystem::path> onPath = environment.FindProgram("vswhere.exe"))
				locations.push_back(*onPath);
			for (const std::filesystem::path& vswhere : locations)
			{
				if (!environment.IsFile(vswhere))
					continue;
				const std::optional<std::string> output = environment.Run(vswhere, { "-products", "*", "-requires", c_VisualStudioCppComponent, "-prerelease",
					"-format", "json", "-utf8", "-nologo" });
				if (!output)
					continue;
				if (std::optional<std::vector<VisualStudioInstance>> instances = ParseVisualStudioInstances(*output))
					return instances;
			}
			return std::nullopt;
		}

		std::string DescribeInstance(const VisualStudioInstance& instance)
		{
			return fmt::format("{} {}.{} ('{}')", instance.Name.empty() ? std::string("Visual Studio") : instance.Name, instance.Version.Major,
				instance.Version.Minor, ToUTF8(instance.Path));
		}

		ScriptToolchainResolution ResolveWindows(const ScriptBuildSettings& preferred, const ScriptToolchainEnvironment& environment)
		{
			ScriptToolchainResolution resolution;
			const std::optional<std::vector<VisualStudioInstance>> found = FindVisualStudioInstances(environment);

			// Usable installations, the preferred ones first: the one the preferred toolchain lies in (an installation built on
			// this machine), then those of the preferred generator, then the newest.
			std::vector<VisualStudioInstance> usable;
			std::vector<std::string> tooOld;
			for (const VisualStudioInstance& instance : found.value_or(std::vector<VisualStudioInstance>()))
			{
				if (!instance.Complete)
					continue;
				if (GetVisualStudioGenerator(instance.Version).empty())
				{
					if (instance.Version < c_MinimumVisualStudio)
						tooOld.push_back(DescribeInstance(instance) + " is too old");
					continue;
				}
				usable.push_back(instance);
			}
			const auto rank = [&preferred](const VisualStudioInstance& instance)
			{
				if (IsInside(preferred.CXXCompiler, instance.Path) || IsInside(preferred.MakeProgram, instance.Path))
					return 0;
				return GetVisualStudioGenerator(instance.Version) == preferred.Generator ? 1 : 2;
			};
			std::stable_sort(usable.begin(), usable.end(), [&rank](const VisualStudioInstance& a, const VisualStudioInstance& b)
			{
				const int rankA = rank(a);
				const int rankB = rank(b);
				return rankA != rankB ? rankA < rankB : a.Version > b.Version;
			});

			if (usable.empty())
			{
				std::vector<std::string> sentences = {
					"Building scripts needs Visual Studio 2022 (version 17.10 or later) or Visual Studio 2026 with the \"Desktop development with "
					"C++\" workload, or the Build Tools for Visual Studio with the C++ build tools, and none is installed.",
					"Install one from https://visualstudio.microsoft.com/downloads/ (the free Community edition or the Build Tools will do; their "
					"C++ CMake tools, included by default, provide CMake), then build the scripts again." };
				if (!tooOld.empty())
				{
					std::string text = "Found: ";
					for (size_t index = 0; index < tooOld.size(); index++)
						text += (index == 0 ? "" : "; ") + tooOld[index];
					sentences.push_back(text + ".");
				}
				resolution.Error = JoinSentences(sentences);
				return resolution;
			}

			// CMakes that serve every installation, then each installation's own.
			CMakeCandidates cmakes(environment);
			cmakes.Add(preferred.CMake);
			if (const std::optional<std::filesystem::path> onPath = environment.FindProgram("cmake.exe"))
				cmakes.Add(*onPath);
			if (const std::optional<std::string> programFiles = environment.GetEnvVar("ProgramFiles"); programFiles && !programFiles->empty())
				cmakes.Add(FileSystem::FromUTF8(*programFiles) / "CMake" / "bin" / "cmake.exe");
			for (const VisualStudioInstance& instance : usable)
			{
				const std::string generator = GetVisualStudioGenerator(instance.Version);
				const ScriptToolVersion minimum = GetMinimumCMakeVersion(generator);
				const CMakeCandidate* cmake = cmakes.FindFirst(minimum);
				if (!cmake)
				{
					// The candidates so far were all asked: only the installation's own CMake is new.
					const size_t first = cmakes.GetCount();
					cmakes.Add(instance.Path / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "CMake" / "bin" / "cmake.exe");
					cmake = cmakes.FindFirst(minimum, first);
				}
				if (!cmake)
					continue;

				ScriptBuildSettings settings = preferred;
				settings.FindToolchain = false;
				settings.CMake = cmake->Path;
				settings.Generator = generator;
				settings.Platform = "x64";
				// A toolset of the build machine need not exist here; the installation's default does.
				settings.Toolset.clear();
				settings.GeneratorInstance = ToUTF8(instance.Path);
				// Visual Studio picks its own compiler. MSBuild is named for what looks for the installation (the external
				// editor choices); CMake is not given it.
				settings.CXXCompiler.clear();
				const std::filesystem::path msbuild = instance.Path / "MSBuild" / "Current" / "Bin" / "amd64" / "MSBuild.exe";
				settings.MakeProgram = environment.IsFile(msbuild) ? msbuild : std::filesystem::path();
				resolution.Description = fmt::format("{} with {}, CMake {} ('{}')", generator, DescribeInstance(instance),
					FormatScriptToolVersion(*cmake->Version), ToUTF8(cmake->Path));
				resolution.Settings = std::move(settings);
				return resolution;
			}

			// An installation, but no CMake that knows its generator.
			const VisualStudioInstance& best = usable.front();
			const std::string generator = GetVisualStudioGenerator(best.Version);
			const ScriptToolVersion minimum = GetMinimumCMakeVersion(generator);
			std::vector<std::string> sentences = {
				fmt::format("Building scripts with {} needs CMake {} or later, and none was found (in PATH, in Program Files\\CMake or in Visual "
					"Studio).", DescribeInstance(best), FormatScriptToolVersion(minimum)),
				"Install CMake from https://cmake.org/download/ (let its installer add it to PATH), or add the \"C++ CMake tools for Windows\" "
				"component in the Visual Studio Installer, then build the scripts again." };
			if (std::string unusable = DescribeUnusableCMakes(cmakes, minimum); !unusable.empty())
				sentences.push_back(std::move(unusable));
			resolution.Error = JoinSentences(sentences);
			return resolution;
		}

		////////////////////////////////////////////////////////////////////////////////
		// Linux and macOS: the system compiler, Ninja or Make, CMake
		////////////////////////////////////////////////////////////////////////////////

		std::optional<std::filesystem::path> FindFirstProgram(const ScriptToolchainEnvironment& environment, const std::vector<std::string_view>& names,
			const std::vector<std::string_view>& directories = {})
		{
			for (std::string_view name : names)
			{
				if (std::optional<std::filesystem::path> found = environment.FindProgram(name))
					return found;
			}
			// Places PATH may lack (programs started from a desktop rather than a shell get a short PATH).
			for (std::string_view directory : directories)
			{
				for (std::string_view name : names)
				{
					const std::filesystem::path candidate = FileSystem::FromUTF8(directory) / FileSystem::FromUTF8(name);
					if (environment.IsFile(candidate))
						return candidate;
				}
			}
			return std::nullopt;
		}

		std::optional<std::filesystem::path> FindCompiler(const ScriptBuildSettings& preferred, const ScriptToolchainEnvironment& environment)
		{
			if (!preferred.CXXCompiler.empty() && environment.IsFile(preferred.CXXCompiler))
				return preferred.CXXCompiler;
			// CXX as people set it: a path, or a program in PATH.
			if (const std::optional<std::string> cxx = environment.GetEnvVar("CXX"); cxx && !cxx->empty())
			{
				const std::filesystem::path path = FileSystem::FromUTF8(*cxx);
				if (path.is_absolute() && environment.IsFile(path))
					return path;
				if (std::optional<std::filesystem::path> found = environment.FindProgram(*cxx))
					return found;
			}
			return FindFirstProgram(environment, { "c++", "g++", "clang++" });
		}

		struct BuildTool
		{
			std::string Generator;
			std::filesystem::path Program;
		};

		std::optional<BuildTool> FindBuildTool(const ScriptBuildSettings& preferred, ScriptToolchainHost host, const ScriptToolchainEnvironment& environment)
		{
			const bool preferNinja = preferred.Generator.find("Makefiles") == std::string::npos;
			const std::vector<std::string_view> directories = host == ScriptToolchainHost::MacOS
				? std::vector<std::string_view> { "/opt/homebrew/bin", "/usr/local/bin" }
				: std::vector<std::string_view> { "/usr/bin", "/usr/local/bin" };
			const auto findNinja = [&]() -> std::optional<BuildTool>
			{
				std::optional<std::filesystem::path> ninja;
				if (preferred.Generator.find("Ninja") != std::string::npos && !preferred.MakeProgram.empty() && environment.IsFile(preferred.MakeProgram))
					ninja = preferred.MakeProgram;
				if (!ninja)
					ninja = FindFirstProgram(environment, { "ninja", "ninja-build" }, directories);
				if (!ninja)
					return std::nullopt;
				// The engine's own generator when it is a Ninja one (Ninja Multi-Config builds every configuration in one tree).
				const std::string generator = preferred.Generator.find("Ninja") != std::string::npos ? preferred.Generator : std::string("Ninja Multi-Config");
				return BuildTool { generator, *ninja };
			};
			const auto findMake = [&]() -> std::optional<BuildTool>
			{
				std::optional<std::filesystem::path> make;
				if (preferred.Generator.find("Makefiles") != std::string::npos && !preferred.MakeProgram.empty() && environment.IsFile(preferred.MakeProgram))
					make = preferred.MakeProgram;
				if (!make)
					make = FindFirstProgram(environment, { "make", "gmake" }, directories);
				if (!make)
					return std::nullopt;
				return BuildTool { "Unix Makefiles", *make };
			};
			std::optional<BuildTool> tool = preferNinja ? findNinja() : findMake();
			if (!tool)
				tool = preferNinja ? findMake() : findNinja();
			return tool;
		}

		ScriptToolchainResolution ResolvePosix(const ScriptBuildSettings& preferred, ScriptToolchainHost host, const ScriptToolchainEnvironment& environment)
		{
			const bool macOS = host == ScriptToolchainHost::MacOS;
			ScriptToolchainResolution resolution;
			std::vector<std::string> missing;
			std::vector<std::string> notes;

			// On macOS compilers and make are shims in /usr/bin that only work once the Command Line Tools (or Xcode) are
			// installed: xcode-select names the developer directory then.
			bool developerTools = true;
			if (macOS)
			{
				const std::optional<std::string> developerDirectory = environment.Run("/usr/bin/xcode-select", { "-p" });
				developerTools = developerDirectory && !Trim(*developerDirectory).empty() && environment.IsDirectory(FileSystem::FromUTF8(Trim(*developerDirectory)));
				if (!developerTools)
					missing.push_back("Apple's Command Line Tools for Xcode (the C++ compiler and Make)");
			}

			// Without the developer tools the compiler and make shims cannot run: they are what is missing.
			const std::optional<std::filesystem::path> compiler = developerTools ? FindCompiler(preferred, environment) : std::nullopt;
			if (developerTools && !compiler)
				missing.push_back("a C++ compiler with C++20 support (GCC 13 or later, or Clang 17 or later; none of CXX, c++, g++ or clang++ was found)");
			const std::optional<BuildTool> buildTool = developerTools ? FindBuildTool(preferred, host, environment) : std::nullopt;
			if (developerTools && !buildTool)
				missing.push_back("Ninja or Make (neither was found)");

			const std::string generator = buildTool ? buildTool->Generator : preferred.Generator;
			const ScriptToolVersion minimum = GetMinimumCMakeVersion(generator);
			CMakeCandidates cmakes(environment);
			cmakes.Add(preferred.CMake);
			if (const std::optional<std::filesystem::path> onPath = environment.FindProgram("cmake"))
				cmakes.Add(*onPath);
			const std::vector<std::string_view> places = macOS
				? std::vector<std::string_view> { "/opt/homebrew/bin/cmake", "/usr/local/bin/cmake", "/Applications/CMake.app/Contents/bin/cmake" }
				: std::vector<std::string_view> { "/usr/bin/cmake", "/usr/local/bin/cmake", "/snap/bin/cmake" };
			for (std::string_view place : places)
				cmakes.Add(FileSystem::FromUTF8(place));
			const CMakeCandidate* cmake = cmakes.FindFirst(minimum);
			if (!cmake)
			{
				missing.push_back(fmt::format("CMake {} or later (none in PATH or the usual places)", FormatScriptToolVersion(minimum)));
				if (std::string unusable = DescribeUnusableCMakes(cmakes, minimum); !unusable.empty())
					notes.push_back(std::move(unusable));
			}

			if (!missing.empty())
			{
				std::string list;
				for (size_t index = 0; index < missing.size(); index++)
					list += (index == 0 ? "" : index + 1 == missing.size() ? " and " : ", ") + missing[index];
				std::vector<std::string> sentences = { fmt::format("Building scripts needs {}.", list) };
				if (macOS)
				{
					// The Command Line Tools bring the compiler and Make; CMake comes from Homebrew or cmake.org.
					std::vector<std::string> steps;
					if (!developerTools || !compiler || !buildTool)
						steps.push_back("the Command Line Tools with `xcode-select --install` in Terminal (or install Xcode)");
					if (!cmake)
						steps.push_back("CMake with Homebrew (`brew install cmake ninja`) or CMake.app from https://cmake.org/download/");
					sentences.push_back(fmt::format("Install {}, then build the scripts again.", steps.size() == 2 ? steps[0] + ", and " + steps[1] : steps[0]));
				}
				else
				{
					sentences.push_back("Install them with your package manager, on Ubuntu with `sudo apt install g++ cmake ninja-build`, then build the "
						"scripts again.");
				}
				sentences.insert(sentences.end(), notes.begin(), notes.end());
				resolution.Error = JoinSentences(sentences);
				return resolution;
			}

			ScriptBuildSettings settings = preferred;
			settings.FindToolchain = false;
			settings.CMake = cmake->Path;
			settings.Generator = buildTool->Generator;
			settings.Platform.clear();
			settings.Toolset.clear();
			settings.GeneratorInstance.clear();
			settings.CXXCompiler = *compiler;
			settings.MakeProgram = buildTool->Program;
			resolution.Description = fmt::format("{} ('{}'), compiler '{}', CMake {} ('{}')", buildTool->Generator, ToUTF8(buildTool->Program), ToUTF8(*compiler),
				FormatScriptToolVersion(*cmake->Version), ToUTF8(cmake->Path));
			resolution.Settings = std::move(settings);
			return resolution;
		}

		class SystemScriptToolchainEnvironment final : public ScriptToolchainEnvironment
		{
		public:
			bool IsFile(const std::filesystem::path& path) const override
			{
				return FileSystem::IsRegularFile(path);
			}

			bool IsDirectory(const std::filesystem::path& path) const override
			{
				return FileSystem::IsDirectory(path);
			}

			std::optional<std::filesystem::path> FindProgram(std::string_view name) const override
			{
				return Platform::FindProgram(name);
			}

			std::optional<std::string> GetEnvVar(std::string_view name) const override
			{
				return Platform::GetEnvVar(name);
			}

			std::optional<std::string> Run(const std::filesystem::path& program, const std::vector<std::string>& arguments) const override
			{
				ProcessSpecification specification;
				specification.Executable = program;
				specification.Arguments = arguments;
				specification.Output = ProcessOutputMode::CaptureSeparate;
				specification.HideWindow = true;
				const Process::RunResult result = Process::Run(std::move(specification), c_ProbeTimeout);
				if (!result.Started || result.TimedOut || result.ExitCode != 0)
					return std::nullopt;
				return result.Output;
			}
		};

	}

	std::optional<EngineInstallation> FindEngineInstallation(const std::filesystem::path& executableDirectory)
	{
		const std::filesystem::path marker = executableDirectory / c_InstallationMarkerFile;
		if (executableDirectory.empty() || !FileSystem::IsRegularFile(marker))
			return std::nullopt;
		EngineInstallation installation;
		installation.Directory = executableDirectory;
		// The version is informative: a marker that cannot be read still marks the installation.
		const std::optional<uint64_t> size = FileSystem::GetFileSize(marker);
		const std::optional<std::string> text = size && *size <= c_MaxMarkerSize ? FileSystem::ReadText(marker) : std::nullopt;
		const std::optional<nlohmann::json> json = text ? JsonUtils::Parse(*text) : std::nullopt;
		const nlohmann::json* header = json ? JsonUtils::Find(*json, "Strata") : nullptr;
		if (header && JsonUtils::GetString(*header, "Format") == c_InstallationFormat && JsonUtils::GetInt(*header, "Version", 0) == c_InstallationFormatVersion)
			installation.EngineVersion = JsonUtils::GetString(*json, "EngineVersion");
		return installation;
	}

	bool IsScriptToolchainPresent(const ScriptBuildSettings& settings)
	{
		return FileSystem::IsRegularFile(settings.CMake) && (settings.CXXCompiler.empty() || FileSystem::IsRegularFile(settings.CXXCompiler)) &&
			(settings.MakeProgram.empty() || FileSystem::IsRegularFile(settings.MakeProgram)) &&
			(settings.GeneratorInstance.empty() || FileSystem::IsDirectory(FileSystem::FromUTF8(settings.GeneratorInstance)));
	}

	ScriptBuildSettings ScriptBuildSettings::Detect()
	{
		ScriptBuildSettings settings = GetEngineDefaults();
		if (const std::optional<EngineInstallation> installation = FindEngineInstallation(Platform::GetExecutableDirectory()))
		{
			// The build machine's paths mean nothing here: the installation's SDK, and a toolchain of this machine.
			settings.EngineDirectory = installation->Directory;
			settings.FindToolchain = true;
		}
		else
		{
			settings.FindToolchain = !IsScriptToolchainPresent(settings);
		}
		return settings;
	}

	ScriptToolchainHost GetScriptToolchainHost()
	{
#if defined(ST_PLATFORM_WINDOWS)
		return ScriptToolchainHost::Windows;
#elif defined(ST_PLATFORM_MACOS)
		return ScriptToolchainHost::MacOS;
#else
		return ScriptToolchainHost::Linux;
#endif
	}

	Scope<ScriptToolchainEnvironment> CreateSystemScriptToolchainEnvironment()
	{
		return CreateScope<SystemScriptToolchainEnvironment>();
	}

	std::string FormatScriptToolVersion(const ScriptToolVersion& version)
	{
		return fmt::format("{}.{}.{}", version.Major, version.Minor, version.Patch);
	}

	std::optional<ScriptToolVersion> ParseCMakeVersion(std::string_view output)
	{
		// The first line: "cmake version 3.31.6" (distributions may name the program cmake3).
		const std::string_view line = output.substr(0, output.find('\n'));
		const size_t marker = line.find(" version ");
		if (marker == std::string_view::npos || line.substr(0, marker).find("cmake") == std::string_view::npos)
			return std::nullopt;
		return ParseVersionNumbers(line.substr(marker + std::string_view(" version ").size()));
	}

	std::optional<std::vector<VisualStudioInstance>> ParseVisualStudioInstances(std::string_view json)
	{
		const std::optional<nlohmann::json> parsed = JsonUtils::Parse(json);
		if (!parsed || !parsed->is_array())
			return std::nullopt;
		std::vector<VisualStudioInstance> instances;
		for (const nlohmann::json& entry : *parsed)
		{
			if (!entry.is_object())
				continue;
			const std::string path = JsonUtils::GetString(entry, "installationPath");
			const std::optional<ScriptToolVersion> version = ParseVersionNumbers(JsonUtils::GetString(entry, "installationVersion"));
			if (path.empty() || !version)
				continue;
			VisualStudioInstance instance;
			instance.Path = FileSystem::FromUTF8(path);
			instance.Name = JsonUtils::GetString(entry, "displayName");
			instance.Version = *version;
			instance.Complete = JsonUtils::GetBool(entry, "isComplete", true);
			instances.push_back(std::move(instance));
		}
		return instances;
	}

	std::string GetVisualStudioGenerator(const ScriptToolVersion& version)
	{
		if (version.Major == 17 && version >= c_MinimumVisualStudio)
			return "Visual Studio 17 2022";
		if (version.Major == 18)
			return "Visual Studio 18 2026";
		return {};
	}

	ScriptToolVersion GetMinimumCMakeVersion(std::string_view generator)
	{
		return generator == "Visual Studio 18 2026" ? c_MinimumCMakeVisualStudio2026 : c_MinimumCMake;
	}

	ScriptToolchainResolution ResolveScriptToolchain(const ScriptBuildSettings& preferred, ScriptToolchainHost host,
		const ScriptToolchainEnvironment& environment)
	{
		// Without the SDK no toolchain helps: the installation is damaged.
		const std::filesystem::path config = preferred.EngineDirectory / "StrataScriptCore" / "CMake" / c_SDKConfigFile;
		if (!environment.IsFile(config))
		{
			ScriptToolchainResolution resolution;
			resolution.Error = fmt::format("The script SDK is missing: '{}' does not exist. Unpack the Strata release again into an empty folder (or "
				"reinstall it), then build the scripts again.", ToUTF8(config));
			return resolution;
		}
		return host == ScriptToolchainHost::Windows ? ResolveWindows(preferred, environment) : ResolvePosix(preferred, host, environment);
	}

}
