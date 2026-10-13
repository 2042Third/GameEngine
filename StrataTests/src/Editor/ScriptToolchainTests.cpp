#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Editor/ScriptBuild.h"
#include "Editor/ScriptProject.h"
#include "Editor/ScriptToolchain.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Platform.h>
#include <Strata/Project/Project.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;

namespace
{

	std::string Key(const std::filesystem::path& path)
	{
		return path.lexically_normal().generic_string();
	}

	// A path as messages show it: normalized, with the platform's separators.
	std::string Shown(const std::string& path)
	{
		return FileSystem::ToUTF8(FileSystem::FromUTF8(path).lexically_normal());
	}

	// A machine as a test describes it: files, directories, programs in PATH, environment variables and what programs print.
	class FakeMachine final : public ScriptToolchainEnvironment
	{
	public:
		void AddFile(const std::filesystem::path& path) { m_Files.insert(Key(path)); }
		void AddDirectory(const std::filesystem::path& path) { m_Directories.insert(Key(path)); }
		// A program found in PATH under `name`, at `path` (which exists).
		void AddProgram(const std::string& name, const std::filesystem::path& path)
		{
			m_Programs[name] = path;
			AddFile(path);
		}
		void SetEnvVar(const std::string& name, const std::string& value) { m_EnvVars[name] = value; }
		// What `program arguments...` prints when it succeeds; programs without an output fail.
		void SetOutput(const std::filesystem::path& program, const std::vector<std::string>& arguments, const std::string& output)
		{
			m_Outputs[MakeRunKey(program, arguments)] = output;
		}
		// A CMake of this version at `path`.
		void AddCMake(const std::filesystem::path& path, const std::string& version)
		{
			AddFile(path);
			SetOutput(path, { "--version" }, "cmake version " + version + "\n\nCMake suite maintained and supported by Kitware (kitware.com/cmake).\n");
		}

		size_t GetRunCount() const { return m_RunCount.load(); }

		bool IsFile(const std::filesystem::path& path) const override { return m_Files.count(Key(path)) != 0; }
		bool IsDirectory(const std::filesystem::path& path) const override { return m_Directories.count(Key(path)) != 0; }

		std::optional<std::filesystem::path> FindProgram(std::string_view name) const override
		{
			const auto it = m_Programs.find(std::string(name));
			return it != m_Programs.end() ? std::optional<std::filesystem::path>(it->second) : std::nullopt;
		}

		std::optional<std::string> GetEnvVar(std::string_view name) const override
		{
			const auto it = m_EnvVars.find(std::string(name));
			return it != m_EnvVars.end() ? std::optional<std::string>(it->second) : std::nullopt;
		}

		std::optional<std::string> Run(const std::filesystem::path& program, const std::vector<std::string>& arguments) const override
		{
			m_RunCount++;
			const auto it = m_Outputs.find(MakeRunKey(program, arguments));
			return it != m_Outputs.end() ? std::optional<std::string>(it->second) : std::nullopt;
		}
	private:
		static std::string MakeRunKey(const std::filesystem::path& program, const std::vector<std::string>& arguments)
		{
			std::string key = Key(program);
			for (const std::string& argument : arguments)
				key += " " + argument;
			return key;
		}
	private:
		std::set<std::string> m_Files;
		std::set<std::string> m_Directories;
		std::map<std::string, std::filesystem::path> m_Programs;
		std::map<std::string, std::string> m_EnvVars;
		std::map<std::string, std::string> m_Outputs;
		mutable std::atomic<size_t> m_RunCount = 0;
	};

	const std::filesystem::path c_Engine = "C:/Strata";
	const std::filesystem::path c_ProgramFilesX86 = "C:/Program Files (x86)";
	const std::filesystem::path c_VSWhere = c_ProgramFilesX86 / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
	const std::vector<std::string> c_VSWhereArguments = { "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-prerelease",
		"-format", "json", "-utf8", "-nologo" };

	// A machine with the SDK of the engine directory `engine`.
	void AddSDK(FakeMachine& machine, const std::filesystem::path& engine = c_Engine)
	{
		machine.AddFile(engine / "StrataScriptCore" / "CMake" / "StrataScriptCoreConfig.cmake");
	}

	struct FakeInstance
	{
		std::string Path;
		std::string Version;
		std::string Name;
		bool Complete = true;
	};

	void AddVisualStudio(FakeMachine& machine, const std::vector<FakeInstance>& instances)
	{
		nlohmann::json list = nlohmann::json::array();
		for (const FakeInstance& instance : instances)
		{
			list.push_back({ { "installationPath", instance.Path }, { "installationVersion", instance.Version }, { "displayName", instance.Name },
				{ "isComplete", instance.Complete } });
		}
		machine.SetEnvVar("ProgramFiles(x86)", "C:/Program Files (x86)");
		machine.SetEnvVar("ProgramFiles", "C:/Program Files");
		machine.AddFile(c_VSWhere);
		machine.SetOutput(c_VSWhere, c_VSWhereArguments, list.dump(2));
	}

	std::filesystem::path BundledCMake(const std::string& instance)
	{
		return FileSystem::FromUTF8(instance) / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "CMake" / "bin" / "cmake.exe";
	}

	// An installed editor's preferences: the build machine's toolchain, which the machine under test does not have.
	ScriptBuildSettings MakeWindowsPreferences()
	{
		ScriptBuildSettings settings;
		settings.CMake = "D:/BuildMachine/CMake/bin/cmake.exe";
		settings.Generator = "Visual Studio 17 2022";
		settings.Platform = "x64";
		settings.Toolset = "v143";
		settings.CXXCompiler = "D:/BuildMachine/VS/2022/Enterprise/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe";
		settings.MakeProgram = "D:/BuildMachine/VS/2022/Enterprise/MSBuild/Current/Bin/amd64/MSBuild.exe";
		settings.Configuration = "Dist";
		settings.EngineDirectory = c_Engine;
		settings.FindToolchain = true;
		return settings;
	}

	ScriptBuildSettings MakePosixPreferences()
	{
		ScriptBuildSettings settings;
		settings.CMake = "/opt/runner/cmake/bin/cmake";
		settings.Generator = "Ninja Multi-Config";
		settings.CXXCompiler = "/usr/bin/g++-14";
		settings.MakeProgram = "/opt/runner/bin/ninja";
		settings.Configuration = "Dist";
		settings.EngineDirectory = c_Engine;
		settings.FindToolchain = true;
		return settings;
	}

	bool Contains(const std::string& text, const std::string& part)
	{
		return text.find(part) != std::string::npos;
	}

	// The test executable stands in for CMake in builds (STRATA_TEST_FAKE_CMAKE, see TestMain.cpp); a machine whose toolchain
	// for this platform consists of it.
	Scope<FakeMachine> MakeMachineWithFakeCMake(const std::filesystem::path& engine, const std::filesystem::path& visualStudio)
	{
		Scope<FakeMachine> machine = CreateScope<FakeMachine>();
		AddSDK(*machine, engine);
		const std::filesystem::path tool = Tests::GetTestExecutablePath();
		machine->AddCMake(tool, "3.31.6");
		switch (GetScriptToolchainHost())
		{
			case ScriptToolchainHost::Windows:
				AddVisualStudio(*machine, { { FileSystem::ToUTF8(visualStudio), "17.14.36310.24", "Visual Studio Community 2022" } });
				machine->AddProgram("cmake.exe", tool);
				break;
			case ScriptToolchainHost::MacOS:
				machine->SetOutput("/usr/bin/xcode-select", { "-p" }, "/Library/Developer/CommandLineTools\n");
				machine->AddDirectory("/Library/Developer/CommandLineTools");
				[[fallthrough]];
			case ScriptToolchainHost::Linux:
				machine->AddProgram("c++", tool);
				machine->AddProgram("ninja", tool);
				machine->AddProgram("cmake", tool);
				break;
		}
		return machine;
	}

	void RunBuild(ScriptBuilder& builder)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (builder.IsRunning() && std::chrono::steady_clock::now() < deadline)
		{
			builder.Update();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		REQUIRE_FALSE(builder.IsRunning());
	}

}

TEST_SUITE("Editor.ScriptToolchain")
{
	TEST_CASE("An installation marker makes the executable's directory the engine directory")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("Installation");
		CHECK_FALSE(FindEngineInstallation(directory));
		CHECK_FALSE(FindEngineInstallation({}));

		REQUIRE(FileSystem::WriteText(directory / c_InstallationMarkerFile,
			R"({ "Strata": { "Format": "Installation", "Version": 1 }, "EngineVersion": "0.3.0" })"));
		std::optional<EngineInstallation> installation = FindEngineInstallation(directory);
		REQUIRE(installation);
		CHECK(installation->Directory == directory);
		CHECK(installation->EngineVersion == "0.3.0");

		// A marker that cannot be read still marks the installation: its scripts must not build against a source tree.
		REQUIRE(FileSystem::WriteText(directory / c_InstallationMarkerFile, "not json"));
		installation = FindEngineInstallation(directory);
		REQUIRE(installation);
		CHECK(installation->Directory == directory);
		CHECK(installation->EngineVersion.empty());
		REQUIRE(FileSystem::WriteText(directory / c_InstallationMarkerFile, R"({ "Strata": { "Format": "Project", "Version": 1 }, "EngineVersion": "9" })"));
		installation = FindEngineInstallation(directory);
		REQUIRE(installation);
		CHECK(installation->EngineVersion.empty());
	}

	TEST_CASE("The test executable's directory is a build tree, whose editor keeps the engine's toolchain")
	{
		// The source-tree workflow: no marker next to the executables, and the toolchain the engine was built with is here.
		CHECK_FALSE(FindEngineInstallation(Platform::GetExecutableDirectory()));
		const ScriptBuildSettings engine = ScriptBuildSettings::GetEngineDefaults();
		CHECK_FALSE(engine.FindToolchain);
		CHECK(IsScriptToolchainPresent(engine));
		CHECK(ScriptBuildSettings::Detect() == engine);

		ScriptBuildSettings gone = engine;
		gone.CMake = engine.EngineDirectory / "no-such-cmake" / "cmake.exe";
		CHECK_FALSE(IsScriptToolchainPresent(gone));
		gone = engine;
		gone.CXXCompiler = engine.EngineDirectory / "no-such-compiler";
		CHECK_FALSE(IsScriptToolchainPresent(gone));
		gone = engine;
		gone.GeneratorInstance = FileSystem::ToUTF8(engine.EngineDirectory / "no-such-installation");
		CHECK_FALSE(IsScriptToolchainPresent(gone));
	}

	TEST_CASE("Tool versions are read from cmake --version and vswhere")
	{
		CHECK(ParseCMakeVersion("cmake version 3.31.6\n\nCMake suite maintained and supported by Kitware (kitware.com/cmake).\n") == ScriptToolVersion { 3, 31, 6 });
		CHECK(ParseCMakeVersion("cmake version 4.2.2\r\n") == ScriptToolVersion { 4, 2, 2 });
		CHECK(ParseCMakeVersion("cmake3 version 3.28.1-dirty\n") == ScriptToolVersion { 3, 28, 1 });
		CHECK(ParseCMakeVersion("cmake version 3.25\n") == ScriptToolVersion { 3, 25, 0 });
		CHECK_FALSE(ParseCMakeVersion(""));
		CHECK_FALSE(ParseCMakeVersion("ninja version 1.12.1"));
		CHECK_FALSE(ParseCMakeVersion("cmake version unknown"));
		CHECK(FormatScriptToolVersion({ 4, 2, 0 }) == "4.2.0");

		const std::optional<std::vector<VisualStudioInstance>> instances = ParseVisualStudioInstances(R"([
			{ "installationPath": "C:/VS/2022/Community", "installationVersion": "17.14.36310.24", "displayName": "Visual Studio Community 2022",
			  "isComplete": true, "catalog": { "productLineVersion": "2022" } },
			{ "installationPath": "C:/VS/18/BuildTools", "installationVersion": "18.0.11111.16", "displayName": "Visual Studio Build Tools 2026",
			  "isComplete": false },
			{ "installationVersion": "17.1.0.0" },
			{ "installationPath": "C:/VS/Broken", "installationVersion": "" },
			"not an instance"
		])");
		REQUIRE(instances);
		REQUIRE(instances->size() == 2);
		CHECK((*instances)[0].Path == FileSystem::FromUTF8("C:/VS/2022/Community"));
		CHECK((*instances)[0].Name == "Visual Studio Community 2022");
		CHECK((*instances)[0].Version == ScriptToolVersion { 17, 14, 36310 });
		CHECK((*instances)[0].Complete);
		CHECK((*instances)[1].Version == ScriptToolVersion { 18, 0, 11111 });
		CHECK_FALSE((*instances)[1].Complete);
		CHECK(ParseVisualStudioInstances("[]")->empty());
		CHECK_FALSE(ParseVisualStudioInstances("{}"));
		CHECK_FALSE(ParseVisualStudioInstances("Error 87: invalid parameter"));

		CHECK(GetVisualStudioGenerator({ 17, 14, 0 }) == "Visual Studio 17 2022");
		CHECK(GetVisualStudioGenerator({ 17, 10, 0 }) == "Visual Studio 17 2022");
		CHECK(GetVisualStudioGenerator({ 17, 9, 9 }).empty()); // MSVC before 19.40
		CHECK(GetVisualStudioGenerator({ 16, 11, 0 }).empty());
		CHECK(GetVisualStudioGenerator({ 18, 0, 0 }) == "Visual Studio 18 2026");
		CHECK(GetVisualStudioGenerator({ 19, 0, 0 }).empty());
		CHECK(GetMinimumCMakeVersion("Visual Studio 18 2026") == ScriptToolVersion { 4, 2, 0 });
		CHECK(GetMinimumCMakeVersion("Visual Studio 17 2022") == ScriptToolVersion { 3, 25, 0 });
		CHECK(GetMinimumCMakeVersion("Ninja Multi-Config") == ScriptToolVersion { 3, 25, 0 });
	}

	TEST_CASE("Windows: a Visual Studio installation with the C++ tools and a CMake that knows its generator")
	{
		FakeMachine machine;
		AddSDK(machine);
		const ScriptBuildSettings preferences = MakeWindowsPreferences();

		SUBCASE("Visual Studio 2022 with CMake in PATH")
		{
			AddVisualStudio(machine, { { "C:/VS/2022/Community", "17.14.36310.24", "Visual Studio Community 2022" } });
			machine.AddFile("C:/VS/2022/Community/MSBuild/Current/Bin/amd64/MSBuild.exe");
			machine.AddProgram("cmake.exe", "C:/Tools/CMake/bin/cmake.exe");
			machine.AddCMake("C:/Tools/CMake/bin/cmake.exe", "3.31.6");
			const ScriptToolchainResolution resolution = ResolveScriptToolchain(preferences, ScriptToolchainHost::Windows, machine);
			REQUIRE_MESSAGE(resolution.Settings, resolution.Error);
			const ScriptBuildSettings& settings = *resolution.Settings;
			CHECK(settings.CMake == FileSystem::FromUTF8("C:/Tools/CMake/bin/cmake.exe").lexically_normal());
			CHECK(settings.Generator == "Visual Studio 17 2022");
			CHECK(settings.Platform == "x64");
			CHECK(settings.Toolset.empty()); // The build machine's toolset need not exist here
			CHECK(settings.GeneratorInstance == "C:/VS/2022/Community");
			CHECK(settings.CXXCompiler.empty());
			CHECK(Key(settings.MakeProgram) == "C:/VS/2022/Community/MSBuild/Current/Bin/amd64/MSBuild.exe");
			CHECK(settings.Configuration == "Dist");
			CHECK(settings.EngineDirectory == c_Engine);
			CHECK_FALSE(settings.FindToolchain);
			CHECK(Contains(resolution.Description, "Visual Studio Community 2022 17.14"));
			CHECK(Contains(resolution.Description, "CMake 3.31.6"));
			CHECK(resolution.Error.empty());
		}

		SUBCASE("Visual Studio 2026 uses the CMake it ships when the one in PATH is too old for it")
		{
			AddVisualStudio(machine, { { "C:/VS/18/Community", "18.0.11111.16", "Visual Studio Community 2026" } });
			machine.AddProgram("cmake.exe", "C:/Tools/CMake/bin/cmake.exe");
			machine.AddCMake("C:/Tools/CMake/bin/cmake.exe", "3.31.6");
			machine.AddCMake(BundledCMake("C:/VS/18/Community"), "4.2.1");
			const ScriptToolchainResolution resolution = ResolveScriptToolchain(preferences, ScriptToolchainHost::Windows, machine);
			REQUIRE_MESSAGE(resolution.Settings, resolution.Error);
			CHECK(resolution.Settings->Generator == "Visual Studio 18 2026");
			CHECK(Key(resolution.Settings->CMake) == Key(BundledCMake("C:/VS/18/Community")));
			CHECK(resolution.Settings->MakeProgram.empty());
		}

		SUBCASE("The engine's generator and the installation of the build machine's compiler come first")
		{
			AddVisualStudio(machine, { { "C:/VS/18/Community", "18.0.11111.16", "Visual Studio Community 2026" },
				{ "C:/VS/2022/Professional", "17.12.1.0", "Visual Studio Professional 2022" },
				{ "C:/VS/2022/Community", "17.14.36310.24", "Visual Studio Community 2022" } });
			machine.AddCMake("C:/Program Files/CMake/bin/cmake.exe", "4.2.2");
			// The engine was built with Visual Studio 2022: the newest 2022 rather than the newer 2026.
			ScriptToolchainResolution resolution = ResolveScriptToolchain(preferences, ScriptToolchainHost::Windows, machine);
			REQUIRE_MESSAGE(resolution.Settings, resolution.Error);
			CHECK(resolution.Settings->GeneratorInstance == "C:/VS/2022/Community");
			CHECK(Key(resolution.Settings->CMake) == "C:/Program Files/CMake/bin/cmake.exe");
			// An editor installed from a build of this machine: the installation its compiler lies in.
			ScriptBuildSettings local = preferences;
			local.CXXCompiler = "C:/VS/2022/Professional/VC/Tools/MSVC/14.42.34433/bin/Hostx64/x64/cl.exe";
			resolution = ResolveScriptToolchain(local, ScriptToolchainHost::Windows, machine);
			REQUIRE_MESSAGE(resolution.Settings, resolution.Error);
			CHECK(resolution.Settings->GeneratorInstance == "C:/VS/2022/Professional");
			// An engine built with Visual Studio 2026.
			ScriptBuildSettings newer = preferences;
			newer.Generator = "Visual Studio 18 2026";
			resolution = ResolveScriptToolchain(newer, ScriptToolchainHost::Windows, machine);
			REQUIRE_MESSAGE(resolution.Settings, resolution.Error);
			CHECK(resolution.Settings->GeneratorInstance == "C:/VS/18/Community");
		}

		SUBCASE("The preferred CMake is used where it exists and is new enough")
		{
			AddVisualStudio(machine, { { "C:/VS/2022/Community", "17.14.36310.24", "Visual Studio Community 2022" } });
			machine.AddCMake(preferences.CMake, "3.30.0");
			machine.AddProgram("cmake.exe", "C:/Tools/cmake.exe");
			machine.AddCMake("C:/Tools/cmake.exe", "3.31.6");
			const ScriptToolchainResolution resolution = ResolveScriptToolchain(preferences, ScriptToolchainHost::Windows, machine);
			REQUIRE_MESSAGE(resolution.Settings, resolution.Error);
			CHECK(Key(resolution.Settings->CMake) == Key(preferences.CMake));
		}

		SUBCASE("Without Visual Studio the error says what to install")
		{
			machine.AddProgram("cmake.exe", "C:/Tools/cmake.exe");
			machine.AddCMake("C:/Tools/cmake.exe", "3.31.6");
			ScriptToolchainResolution resolution = ResolveScriptToolchain(preferences, ScriptToolchainHost::Windows, machine);
			CHECK_FALSE(resolution.Settings);
			CHECK(Contains(resolution.Error, "Building scripts needs Visual Studio 2022 (version 17.10 or later) or Visual Studio 2026"));
			CHECK(Contains(resolution.Error, "\"Desktop development with C++\" workload"));
			CHECK(Contains(resolution.Error, "Build Tools for Visual Studio"));
			CHECK(Contains(resolution.Error, "https://visualstudio.microsoft.com/downloads/"));
			CHECK_FALSE(Contains(resolution.Error, "Found:"));

			// Installations that cannot build the scripts are named, so the person knows why they do not count.
			AddVisualStudio(machine, { { "C:/VS/2019/Community", "16.11.35026.282", "Visual Studio Community 2019" },
				{ "C:/VS/2022/Old", "17.4.0.0", "Visual Studio Community 2022" },
				{ "C:/VS/2022/Broken", "17.14.0.0", "Visual Studio Community 2022", false } });
			resolution = ResolveScriptToolchain(preferences, ScriptToolchainHost::Windows, machine);
			CHECK_FALSE(resolution.Settings);
			CHECK(Contains(resolution.Error, "Found: Visual Studio Community 2019 16.11 ('C:/VS/2019/Community') is too old; Visual Studio Community 2022 17.4"));
			CHECK_FALSE(Contains(resolution.Error, "Broken"));
		}

		SUBCASE("Without a CMake that knows the generator the error names the versions found")
		{
			AddVisualStudio(machine, { { "C:/VS/18/Community", "18.0.11111.16", "Visual Studio Community 2026" } });
			machine.AddProgram("cmake.exe", "C:/Tools/cmake.exe");
			machine.AddCMake("C:/Tools/cmake.exe", "3.31.6");
			machine.AddFile("C:/Program Files/CMake/bin/cmake.exe"); // Prints nothing: does not run
			const ScriptToolchainResolution resolution = ResolveScriptToolchain(preferences, ScriptToolchainHost::Windows, machine);
			CHECK_FALSE(resolution.Settings);
			CHECK(Contains(resolution.Error, "Building scripts with Visual Studio Community 2026 18.0 ('C:/VS/18/Community') needs CMake 4.2.0 or later"));
			CHECK(Contains(resolution.Error, "https://cmake.org/download/"));
			CHECK(Contains(resolution.Error, "\"C++ CMake tools for Windows\" component"));
			CHECK(Contains(resolution.Error, "CMake 3.31.6 ('" + Shown("C:/Tools/cmake.exe") + "') is too old"));
			CHECK(Contains(resolution.Error, "'" + Shown("C:/Program Files/CMake/bin/cmake.exe") + "' does not run"));
		}

		SUBCASE("Without the SDK no toolchain helps")
		{
			FakeMachine empty;
			const ScriptToolchainResolution resolution = ResolveScriptToolchain(preferences, ScriptToolchainHost::Windows, empty);
			CHECK_FALSE(resolution.Settings);
			CHECK(Contains(resolution.Error, "The script SDK is missing"));
			CHECK(Contains(resolution.Error, "StrataScriptCoreConfig.cmake"));
			CHECK(Contains(resolution.Error, "Unpack the Strata release again"));
			CHECK(empty.GetRunCount() == 0);
		}
	}

	TEST_CASE("Linux: the system compiler, Ninja or Make, and CMake 3.25 or later")
	{
		FakeMachine machine;
		AddSDK(machine);
		const ScriptBuildSettings preferences = MakePosixPreferences();

		SUBCASE("The build machine's toolchain where it exists")
		{
			machine.AddFile(preferences.CXXCompiler);
			machine.AddFile(preferences.MakeProgram);
			machine.AddCMake(preferences.CMake, "3.31.6");
			const ScriptToolchainResolution resolution = ResolveScriptToolchain(preferences, ScriptToolchainHost::Linux, machine);
			REQUIRE_MESSAGE(resolution.Settings, resolution.Error);
			ScriptBuildSettings expected = preferences;
			expected.FindToolchain = false;
			expected.CMake = expected.CMake.lexically_normal();
			CHECK(*resolution.Settings == expected);
		}

		SUBCASE("The system's tools: c++, Make when there is no Ninja, and CMake in PATH")
		{
			machine.AddProgram("c++", "/usr/bin/c++");
			machine.AddProgram("make", "/usr/bin/make");
			machine.AddProgram("cmake", "/usr/bin/cmake");
			machine.AddCMake("/usr/bin/cmake", "3.28.3");
			const ScriptToolchainResolution resolution = ResolveScriptToolchain(preferences, ScriptToolchainHost::Linux, machine);
			REQUIRE_MESSAGE(resolution.Settings, resolution.Error);
			CHECK(resolution.Settings->Generator == "Unix Makefiles");
			CHECK(Key(resolution.Settings->MakeProgram) == "/usr/bin/make");
			CHECK(Key(resolution.Settings->CXXCompiler) == "/usr/bin/c++");
			CHECK(Key(resolution.Settings->CMake) == "/usr/bin/cmake");
			CHECK(resolution.Settings->Configuration == "Dist");
			CHECK(Contains(resolution.Description, "Unix Makefiles"));
		}

		SUBCASE("CXX and Ninja in PATH")
		{
			machine.SetEnvVar("CXX", "clang++-18");
			machine.AddProgram("clang++-18", "/usr/bin/clang++-18");
			machine.AddProgram("g++", "/usr/bin/g++");
			machine.AddProgram("ninja", "/usr/local/bin/ninja");
			machine.AddProgram("make", "/usr/bin/make");
			machine.AddCMake("/usr/local/bin/cmake", "3.30.1"); // Not in PATH: a usual place
			const ScriptToolchainResolution resolution = ResolveScriptToolchain(preferences, ScriptToolchainHost::Linux, machine);
			REQUIRE_MESSAGE(resolution.Settings, resolution.Error);
			CHECK(Key(resolution.Settings->CXXCompiler) == "/usr/bin/clang++-18");
			CHECK(resolution.Settings->Generator == "Ninja Multi-Config");
			CHECK(Key(resolution.Settings->MakeProgram) == "/usr/local/bin/ninja");
			CHECK(Key(resolution.Settings->CMake) == "/usr/local/bin/cmake");
		}

		SUBCASE("Nothing installed: one error names everything and the packages to install")
		{
			const ScriptToolchainResolution resolution = ResolveScriptToolchain(preferences, ScriptToolchainHost::Linux, machine);
			CHECK_FALSE(resolution.Settings);
			CHECK(Contains(resolution.Error, "Building scripts needs a C++ compiler with C++20 support (GCC 13 or later, or Clang 17 or later"));
			CHECK(Contains(resolution.Error, "Ninja or Make (neither was found)"));
			CHECK(Contains(resolution.Error, "and CMake 3.25.0 or later"));
			CHECK(Contains(resolution.Error, "sudo apt install g++ cmake ninja-build"));
		}

		SUBCASE("A CMake that is too old")
		{
			machine.AddProgram("c++", "/usr/bin/c++");
			machine.AddProgram("ninja", "/usr/bin/ninja");
			machine.AddProgram("cmake", "/usr/bin/cmake");
			machine.AddCMake("/usr/bin/cmake", "3.22.1");
			const ScriptToolchainResolution resolution = ResolveScriptToolchain(preferences, ScriptToolchainHost::Linux, machine);
			CHECK_FALSE(resolution.Settings);
			CHECK(Contains(resolution.Error, "Building scripts needs CMake 3.25.0 or later"));
			CHECK_FALSE(Contains(resolution.Error, "compiler"));
			CHECK(Contains(resolution.Error, "CMake 3.22.1 ('" + Shown("/usr/bin/cmake") + "') is too old"));
		}
	}

	TEST_CASE("macOS: the Command Line Tools, Ninja or Make, and CMake from Homebrew or CMake.app")
	{
		FakeMachine machine;
		AddSDK(machine);
		const ScriptBuildSettings preferences = MakePosixPreferences();
		// The compiler and make shims exist without the Command Line Tools; only xcode-select knows whether they work.
		machine.AddProgram("c++", "/usr/bin/c++");
		machine.AddProgram("make", "/usr/bin/make");

		SUBCASE("Without the Command Line Tools")
		{
			machine.AddCMake("/opt/homebrew/bin/cmake", "3.31.6");
			const ScriptToolchainResolution resolution = ResolveScriptToolchain(preferences, ScriptToolchainHost::MacOS, machine);
			CHECK_FALSE(resolution.Settings);
			CHECK(Contains(resolution.Error, "Building scripts needs Apple's Command Line Tools for Xcode"));
			CHECK(Contains(resolution.Error, "xcode-select --install"));
			CHECK_FALSE(Contains(resolution.Error, "brew install cmake"));
		}

		SUBCASE("The Command Line Tools and CMake from Homebrew outside PATH")
		{
			machine.SetOutput("/usr/bin/xcode-select", { "-p" }, "/Library/Developer/CommandLineTools\n");
			machine.AddDirectory("/Library/Developer/CommandLineTools");
			machine.AddCMake("/opt/homebrew/bin/cmake", "4.1.2");
			const ScriptToolchainResolution resolution = ResolveScriptToolchain(preferences, ScriptToolchainHost::MacOS, machine);
			REQUIRE_MESSAGE(resolution.Settings, resolution.Error);
			CHECK(Key(resolution.Settings->CXXCompiler) == "/usr/bin/c++");
			CHECK(resolution.Settings->Generator == "Unix Makefiles");
			CHECK(Key(resolution.Settings->CMake) == "/opt/homebrew/bin/cmake");
		}

		SUBCASE("The Command Line Tools without CMake")
		{
			machine.SetOutput("/usr/bin/xcode-select", { "-p" }, "/Applications/Xcode.app/Contents/Developer\n");
			machine.AddDirectory("/Applications/Xcode.app/Contents/Developer");
			const ScriptToolchainResolution resolution = ResolveScriptToolchain(preferences, ScriptToolchainHost::MacOS, machine);
			CHECK_FALSE(resolution.Settings);
			CHECK(Contains(resolution.Error, "Building scripts needs CMake 3.25.0 or later"));
			CHECK(Contains(resolution.Error, "brew install cmake ninja"));
			CHECK(Contains(resolution.Error, "CMake.app from https://cmake.org/download/"));
			CHECK_FALSE(Contains(resolution.Error, "xcode-select --install"));
		}
	}

	TEST_CASE("A build that finds its toolchain first configures with it, and later builds keep it")
	{
		Tests::ScopedEnvironmentVariable fakeCMake("STRATA_TEST_FAKE_CMAKE", "succeed");
		const std::filesystem::path root = Tests::CreateTemporaryDirectory("ScriptToolchainBuild");
		const std::filesystem::path engine = root / "Strata";
		const std::filesystem::path visualStudio = root / "VS";
		REQUIRE(FileSystem::CreateDirectories(visualStudio));
		std::string error;
		Ref<Project> project = Project::Create(root / "Game", "Game", &error);
		REQUIRE_MESSAGE(project, error);
		REQUIRE_MESSAGE(CreateScriptProjectFiles(*project, true, nullptr, &error), error);
		const std::filesystem::path module = project->GetScriptModulePath();
		REQUIRE(FileSystem::CreateDirectories(module.parent_path()));
		REQUIRE(FileSystem::WriteText(module, "not a real module")); // The fake CMake builds nothing

		ScriptBuildSettings preferences = GetScriptToolchainHost() == ScriptToolchainHost::Windows ? MakeWindowsPreferences() : MakePosixPreferences();
		preferences.EngineDirectory = engine;
		preferences.Configuration = "Release";
		std::atomic<int> searches = 0;
		ScriptBuilder builder;
		builder.SetToolchainEnvironment([&]()
		{
			searches++;
			return Scope<ScriptToolchainEnvironment>(MakeMachineWithFakeCMake(engine, visualStudio));
		});
		CHECK_FALSE(builder.GetFoundToolchain(preferences));

		REQUIRE_MESSAGE(builder.Start(*project, preferences, &error), error);
		CHECK(builder.IsRunning());
		CHECK(builder.GetCurrentID() == 1);
		RunBuild(builder);
		const ScriptBuildResult& result = builder.GetLastResult();
		REQUIRE_MESSAGE(result.Success, result.Error);
		CHECK(result.ID == 1);
		CHECK(result.Configured);
		CHECK_FALSE(result.Settings.FindToolchain);
		CHECK(result.Settings.EngineDirectory == engine);
		CHECK(result.Settings.Configuration == "Release");
		CHECK(result.Settings.CMake == Tests::GetTestExecutablePath().lexically_normal());
		CHECK(Contains(result.LogTail, "Found "));
		CHECK(Contains(result.LogTail, "-DSTRATA_ENGINE_DIR=" + FileSystem::ToUTF8(engine)));
		if (GetScriptToolchainHost() == ScriptToolchainHost::Windows)
		{
			CHECK(result.Settings.Generator == "Visual Studio 17 2022");
			CHECK(result.Settings.GeneratorInstance == FileSystem::ToUTF8(visualStudio));
			CHECK(Contains(result.LogTail, "-DCMAKE_GENERATOR_INSTANCE=" + FileSystem::ToUTF8(visualStudio)));
		}
		else
		{
			CHECK(result.Settings.Generator == "Ninja Multi-Config");
			CHECK(result.Settings.CXXCompiler == Tests::GetTestExecutablePath());
		}
		const ScriptBuildSettings* found = builder.GetFoundToolchain(preferences);
		REQUIRE(found);
		CHECK(*found == result.Settings);
		CHECK_FALSE(builder.GetFoundToolchainDescription().empty());

		// The IDE presets carry the toolchain that was found, not the build machine's.
		const std::optional<nlohmann::json> presets = JsonUtils::Parse(FileSystem::ReadText(project->GetScriptSourceDirectory() / "CMakePresets.json").value_or(""));
		REQUIRE(presets);
		CHECK((*presets)["configurePresets"][0]["generator"] == result.Settings.Generator);
		CHECK((*presets)["configurePresets"][0]["cacheVariables"]["STRATA_ENGINE_DIR"] == FileSystem::ToUTF8(engine));

		// The next build uses it without looking again, and its tree stays configured (the fake CMake writes no cache: the
		// test does, as CMake's configure step would).
		REQUIRE(searches.load() == 1);
		REQUIRE(FileSystem::WriteText(result.BuildDirectory / "CMakeCache.txt", "# configured\n"));
		REQUIRE_MESSAGE(builder.Start(*project, preferences, &error), error);
		CHECK(builder.GetPhase() != ScriptBuildPhase::FindingToolchain);
		RunBuild(builder);
		REQUIRE_MESSAGE(builder.GetLastResult().Success, builder.GetLastResult().Error);
		CHECK(searches.load() == 1);
		CHECK_FALSE(builder.GetLastResult().Configured);
		CHECK(builder.GetLastResult().Settings == *found);
	}

	TEST_CASE("A build that finds no toolchain fails with what to install, and a cancelled search ends the build")
	{
		const std::filesystem::path root = Tests::CreateTemporaryDirectory("ScriptToolchainMissing");
		std::string error;
		Ref<Project> project = Project::Create(root / "Game", "Game", &error);
		REQUIRE_MESSAGE(project, error);
		REQUIRE_MESSAGE(CreateScriptProjectFiles(*project, true, nullptr, &error), error);
		ScriptBuildSettings preferences = GetScriptToolchainHost() == ScriptToolchainHost::Windows ? MakeWindowsPreferences() : MakePosixPreferences();
		preferences.EngineDirectory = root / "Strata";

		ScriptBuilder builder;
		builder.SetToolchainEnvironment([&]()
		{
			Scope<FakeMachine> machine = CreateScope<FakeMachine>();
			AddSDK(*machine, preferences.EngineDirectory);
			return Scope<ScriptToolchainEnvironment>(std::move(machine));
		});
		REQUIRE_MESSAGE(builder.Start(*project, preferences, &error), error);
		RunBuild(builder);
		const ScriptBuildResult& result = builder.GetLastResult();
		CHECK(result.ID == 1);
		CHECK_FALSE(result.Success);
		CHECK_FALSE(result.Cancelled);
		CHECK(Contains(result.Error, "No C++ toolchain to build the scripts with was found on this machine"));
		CHECK(Contains(result.Error, "Building scripts needs"));
		REQUIRE(result.Diagnostics.size() == 1);
		CHECK(result.Diagnostics[0].Severity == "error");
		CHECK(result.Diagnostics[0].File.empty());
		CHECK(Contains(result.Diagnostics[0].Message, "Building scripts needs"));
		CHECK(result.Settings.FindToolchain);
		CHECK_FALSE(builder.GetFoundToolchain(preferences));
		// Nothing was configured, and no presets with the build machine's toolchain were written.
		CHECK_FALSE(FileSystem::Exists(project->GetScriptBuildDirectory()));
		CHECK_FALSE(FileSystem::Exists(project->GetScriptSourceDirectory() / "CMakePresets.json"));

		// A build cancelled while it looks finishes as cancelled; the search's job ends on its own (the job system, declared
		// after what the job reads, joins it first).
		std::atomic<bool> release = false;
		Tests::ScopedJobSystem jobs(1, 1);
		builder.SetToolchainEnvironment([&]()
		{
			while (!release.load())
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			return Scope<ScriptToolchainEnvironment>(CreateScope<FakeMachine>());
		});
		REQUIRE_MESSAGE(builder.Start(*project, preferences, &error), error);
		CHECK(builder.GetPhase() == ScriptBuildPhase::FindingToolchain);
		CHECK(std::string(ScriptBuildPhaseToString(builder.GetPhase())) == "FindingToolchain");
		builder.Cancel();
		release.store(true);
		CHECK_FALSE(builder.IsRunning());
		CHECK(builder.GetLastResult().ID == 2);
		CHECK(builder.GetLastResult().Cancelled);
	}
}
