#pragma once

#include "Editor/ScriptBuild.h"

#include <Strata/Core/Base.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// An installed Strata (an unpacked release, `cmake --install`): the editor, the runtime and the CLI side by side with the
	// script SDK (StrataScriptCore), the samples and this marker file, which the install writes next to the executables. An
	// editor that finds it builds scripts with the installation's SDK instead of the source tree it was built from, and finds
	// a toolchain on the machine it runs on (ScriptBuildSettings::FindToolchain).
	constexpr const char* c_InstallationMarkerFile = "StrataInstall.json";
	constexpr const char* c_InstallationFormat = "Installation";
	constexpr uint32_t c_InstallationFormatVersion = 1;

	struct EngineInstallation
	{
		std::filesystem::path Directory; // Holds the executables, StrataScriptCore and the marker
		std::string EngineVersion;       // The version the marker names (empty when it names none)
	};

	// The installation of the executables in `executableDirectory`: the directory, when the marker lies there; nothing for an
	// editor in a build tree. The marker's content only names the version: a marker that cannot be read still makes the
	// directory an installation, whose scripts must never build against a source tree that need not exist on this machine
	// (a missing SDK is reported by the build, ResolveScriptToolchain).
	std::optional<EngineInstallation> FindEngineInstallation(const std::filesystem::path& executableDirectory);

	// Whether the toolchain of these settings is on this machine: CMake, the compiler, the build tool and the Visual Studio
	// installation (where named) exist. The engine's own settings are, in the tree it was built in.
	bool IsScriptToolchainPresent(const ScriptBuildSettings& settings);

	// The platform whose toolchains ResolveScriptToolchain looks for (tests resolve for every platform on any machine).
	enum class ScriptToolchainHost : uint8_t
	{
		Windows = 0,
		Linux,
		MacOS
	};

	ScriptToolchainHost GetScriptToolchainHost();

	// What a machine offers to build scripts with. The editor asks the system (CreateSystemScriptToolchainEnvironment); tests
	// describe machines. Called from a background job.
	class ScriptToolchainEnvironment
	{
	public:
		virtual ~ScriptToolchainEnvironment() = default;

		virtual bool IsFile(const std::filesystem::path& path) const = 0;
		virtual bool IsDirectory(const std::filesystem::path& path) const = 0;
		// A program in the directories of PATH (Platform::FindProgram); `name` as the platform spells it ("cmake.exe" on Windows).
		virtual std::optional<std::filesystem::path> FindProgram(std::string_view name) const = 0;
		virtual std::optional<std::string> GetEnvVar(std::string_view name) const = 0;
		// Runs a program to completion: its output (stdout and stderr), or nothing when it cannot start, fails or does not end in
		// time.
		virtual std::optional<std::string> Run(const std::filesystem::path& program, const std::vector<std::string>& arguments) const = 0;
	};

	Scope<ScriptToolchainEnvironment> CreateSystemScriptToolchainEnvironment();

	struct ScriptToolVersion
	{
		uint32_t Major = 0;
		uint32_t Minor = 0;
		uint32_t Patch = 0;

		auto operator<=>(const ScriptToolVersion& other) const = default;
	};

	std::string FormatScriptToolVersion(const ScriptToolVersion& version);
	// The version `cmake --version` prints ("cmake version 3.31.6", also "cmake3 version 3.28.1-dirty").
	std::optional<ScriptToolVersion> ParseCMakeVersion(std::string_view output);

	// A Visual Studio installation as vswhere reports it.
	struct VisualStudioInstance
	{
		std::filesystem::path Path;   // installationPath
		std::string Name;             // displayName, e.g. "Visual Studio Community 2022"
		ScriptToolVersion Version;    // installationVersion
		bool Complete = true;         // isComplete: false for an installation that failed or is being changed
	};

	// The instances of `vswhere -format json` output; nothing for output that is not such a list.
	std::optional<std::vector<VisualStudioInstance>> ParseVisualStudioInstances(std::string_view json);
	// The CMake generator of a Visual Studio version ("Visual Studio 17 2022"), or empty for versions Strata does not build
	// scripts with (older than 2022, or newer than it knows).
	std::string GetVisualStudioGenerator(const ScriptToolVersion& version);
	// The oldest CMake that can configure a project with this generator: 3.25 (the scripts' CMakeLists.txt requires it), 4.2
	// for Visual Studio 2026.
	ScriptToolVersion GetMinimumCMakeVersion(std::string_view generator);

	struct ScriptToolchainResolution
	{
		std::optional<ScriptBuildSettings> Settings; // The toolchain found (FindToolchain false), nothing when none is usable
		std::string Description;                     // What was chosen, for the build log
		std::string Error;                           // What is missing and what to install, when nothing is usable
	};

	// Finds a toolchain to build scripts with on a machine: the preferred settings' tools where they exist and work, else the
	// machine's own: on Windows a Visual Studio 2022 (17.10 or later) or 2026 installation with the C++ tools (vswhere) and a
	// CMake that knows its generator (the preferred one, PATH, Program Files or the one Visual Studio ships); on Linux and
	// macOS the C++ compiler (CXX, c++, g++, clang++; on macOS the Command Line Tools must be installed), Ninja or Make and
	// a CMake 3.25 or later (the preferred one, PATH, the usual install places). The configuration and the engine directory
	// are the preferred ones; the engine directory must hold the script SDK. When nothing is usable, Error names what to
	// install for this platform.
	ScriptToolchainResolution ResolveScriptToolchain(const ScriptBuildSettings& preferred, ScriptToolchainHost host,
		const ScriptToolchainEnvironment& environment);

}
