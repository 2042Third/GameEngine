#include <doctest/doctest.h>

#include "Editor/ScriptBuild.h"
#include "Editor/ScriptProject.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Project/Project.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	bool Contains(const std::vector<std::string>& arguments, const std::string& argument)
	{
		return std::find(arguments.begin(), arguments.end(), argument) != arguments.end();
	}

	// The value following `option` (e.g. "-G"), or empty.
	std::string GetOption(const std::vector<std::string>& arguments, const std::string& option)
	{
		auto it = std::find(arguments.begin(), arguments.end(), option);
		return it != arguments.end() && it + 1 != arguments.end() ? *(it + 1) : std::string();
	}

	ScriptBuildSettings MakeVisualStudioSettings()
	{
		ScriptBuildSettings settings;
		settings.CMake = "C:/Tools/cmake.exe";
		settings.Generator = "Visual Studio 18 2026";
		settings.Platform = "x64";
		settings.Toolset = "host=x64";
		settings.CXXCompiler = "C:/VS/cl.exe";
		settings.MakeProgram = "C:/VS/MSBuild.exe";
		settings.Configuration = "Release";
		settings.EngineDirectory = "C:/Engine";
		return settings;
	}

	ScriptBuildSettings MakeNinjaSettings()
	{
		ScriptBuildSettings settings;
		settings.CMake = "/usr/bin/cmake";
		settings.Generator = "Ninja";
		settings.CXXCompiler = "/usr/bin/clang++";
		settings.MakeProgram = "/usr/bin/ninja";
		settings.Configuration = "Debug";
		settings.EngineDirectory = "/home/me/Strata";
		return settings;
	}

}

TEST_SUITE("Editor.ScriptBuild")
{
	TEST_CASE("Script builds use the engine's toolchain by default")
	{
		const ScriptBuildSettings settings = ScriptBuildSettings::GetEngineDefaults();
		CHECK_FALSE(settings.Generator.empty());
		CHECK_FALSE(settings.Configuration.empty());
		CHECK(FileSystem::IsRegularFile(settings.EngineDirectory / "StrataScriptCore" / "CMake" / "StrataScriptCoreConfig.cmake"));
#if defined(ST_DEBUG)
		CHECK(settings.Configuration == "Debug");
#elif defined(ST_RELEASE)
		CHECK(settings.Configuration == "Release");
#endif
	}

	TEST_CASE("Configure and build commands follow the generator")
	{
		const std::filesystem::path source = "C:/Game/Scripts";
		const std::filesystem::path build = "C:/Game/.strata/Scripts/Build";
		const std::filesystem::path binary = "C:/Game/.strata/Scripts/Bin";

		// Visual Studio picks its own compiler; the module goes to the binary directory for the configuration.
		const ScriptBuildSettings visualStudio = MakeVisualStudioSettings();
		CHECK(visualStudio.IsMultiConfig());
		const std::vector<std::string> configure = MakeScriptConfigureArguments(visualStudio, source, build, binary);
		CHECK(GetOption(configure, "-S") == "C:/Game/Scripts");
		CHECK(GetOption(configure, "-B") == "C:/Game/.strata/Scripts/Build");
		CHECK(GetOption(configure, "-G") == "Visual Studio 18 2026");
		CHECK(GetOption(configure, "-A") == "x64");
		CHECK(GetOption(configure, "-T") == "host=x64");
		CHECK(Contains(configure, "-DSTRATA_ENGINE_DIR=C:/Engine"));
		CHECK(Contains(configure, "-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=C:/Game/.strata/Scripts/Bin"));
		CHECK(Contains(configure, "-DCMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE=C:/Game/.strata/Scripts/Bin"));
		for (const std::string& argument : configure)
		{
			CHECK(argument.rfind("-DCMAKE_CXX_COMPILER=", 0) != 0);
			CHECK(argument.rfind("-DCMAKE_BUILD_TYPE=", 0) != 0);
			CHECK(argument.rfind("-DCMAKE_MAKE_PROGRAM=", 0) != 0);
		}
		const std::vector<std::string> buildArguments = MakeScriptBuildArguments(visualStudio, build);
		CHECK(GetOption(buildArguments, "--build") == "C:/Game/.strata/Scripts/Build");
		CHECK(GetOption(buildArguments, "--config") == "Release");
		CHECK(Contains(buildArguments, "/nodeReuse:false"));

		// Single-configuration generators get the compiler, build tool and build type.
		const ScriptBuildSettings ninja = MakeNinjaSettings();
		CHECK_FALSE(ninja.IsMultiConfig());
		const std::vector<std::string> ninjaConfigure = MakeScriptConfigureArguments(ninja, source, build, binary);
		CHECK(Contains(ninjaConfigure, "-DCMAKE_CXX_COMPILER=/usr/bin/clang++"));
		CHECK(Contains(ninjaConfigure, "-DCMAKE_MAKE_PROGRAM=/usr/bin/ninja"));
		CHECK(Contains(ninjaConfigure, "-DCMAKE_BUILD_TYPE=Debug"));
		CHECK_FALSE(Contains(ninjaConfigure, "-A"));
		CHECK_FALSE(Contains(ninjaConfigure, "-DCMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG=C:/Game/.strata/Scripts/Bin"));
		CHECK_FALSE(Contains(MakeScriptBuildArguments(ninja, build), "/nodeReuse:false"));
		for (const std::string& argument : ninjaConfigure)
			CHECK(argument.rfind("-DCMAKE_CONFIGURATION_TYPES=", 0) != 0);
	}

	TEST_CASE("Dist script builds name their configuration to every generator")
	{
		const std::filesystem::path source = "C:/Game/Scripts";
		const std::filesystem::path build = "C:/Game/.strata/Scripts/Build";
		const std::filesystem::path binary = "C:/Game/.strata/Scripts/Bin";

		// CMake has no Dist configuration of its own: multi-config build trees are limited to (and so define) it.
		ScriptBuildSettings visualStudio = MakeVisualStudioSettings();
		visualStudio.Configuration = "Dist";
		const std::vector<std::string> configure = MakeScriptConfigureArguments(visualStudio, source, build, binary);
		CHECK(Contains(configure, "-DCMAKE_CONFIGURATION_TYPES=Dist"));
		CHECK(Contains(configure, "-DCMAKE_LIBRARY_OUTPUT_DIRECTORY_DIST=C:/Game/.strata/Scripts/Bin"));
		CHECK(GetOption(MakeScriptBuildArguments(visualStudio, build), "--config") == "Dist");

		ScriptBuildSettings ninjaMulti = MakeNinjaSettings();
		ninjaMulti.Generator = "Ninja Multi-Config";
		ninjaMulti.Configuration = "Dist";
		CHECK(ninjaMulti.IsMultiConfig());
		CHECK(Contains(MakeScriptConfigureArguments(ninjaMulti, source, build, binary), "-DCMAKE_CONFIGURATION_TYPES=Dist"));

		ScriptBuildSettings ninja = MakeNinjaSettings();
		ninja.Configuration = "Dist";
		const std::vector<std::string> ninjaConfigure = MakeScriptConfigureArguments(ninja, source, build, binary);
		CHECK(Contains(ninjaConfigure, "-DCMAKE_BUILD_TYPE=Dist"));
		CHECK(GetOption(MakeScriptBuildArguments(ninja, build), "--config") == "Dist");
	}

	TEST_CASE("Build diagnostics are parsed from MSVC, GCC, Clang, linker and CMake output")
	{
		const std::string log =
			"  Player.cpp\n"
			"G:\\Game\\Scripts\\Player.cpp(12,5): error C2065: 'speed': undeclared identifier [G:\\Game\\.strata\\Scripts\\Build\\GameScripts.vcxproj]\n"
			"G:\\Game\\Scripts\\Player.cpp(20): warning C4244: 'argument': conversion from 'double' to 'float' [G:\\Game\\GameScripts.vcxproj]\n"
			"LINK : fatal error LNK1104: cannot open file 'GameScripts.dll' [G:\\Game\\GameScripts.vcxproj]\n"
			"/home/me/Game/Scripts/Enemy.cpp:7:3: error: use of undeclared identifier 'health'\n"
			"/home/me/Game/Scripts/Enemy.cpp:9: warning: unused variable 'x'\n"
			"collect2: error: ld returned 1 exit status\n"
			"CMake Error at CMakeLists.txt:8 (find_package):\n"
			"  Could not find a package configuration file provided by \"StrataScriptCore\"\n"
			"\n"
			"Build FAILED.\n"
			// MSBuild repeats errors in its summary.
			"G:\\Game\\Scripts\\Player.cpp(12,5): error C2065: 'speed': undeclared identifier [G:\\Game\\.strata\\Scripts\\Build\\GameScripts.vcxproj]\n";

		const std::vector<ScriptDiagnostic> diagnostics = ParseScriptBuildDiagnostics(log);
		REQUIRE(diagnostics.size() == 7);

		CHECK(diagnostics[0].File == "G:\\Game\\Scripts\\Player.cpp");
		CHECK(diagnostics[0].Line == 12);
		CHECK(diagnostics[0].Column == 5);
		CHECK(diagnostics[0].Severity == "error");
		CHECK(diagnostics[0].Code == "C2065");
		CHECK(diagnostics[0].Message == "'speed': undeclared identifier");
		CHECK(FormatScriptDiagnostic(diagnostics[0]) == "G:\\Game\\Scripts\\Player.cpp(12,5): error C2065: 'speed': undeclared identifier");

		CHECK(diagnostics[1].Line == 20);
		CHECK(diagnostics[1].Column == 0);
		CHECK(diagnostics[1].Severity == "warning");

		CHECK(diagnostics[2].File == "LINK");
		CHECK(diagnostics[2].Line == 0);
		CHECK(diagnostics[2].Severity == "error");
		CHECK(diagnostics[2].Code == "LNK1104");
		CHECK(diagnostics[2].Message == "cannot open file 'GameScripts.dll'");

		CHECK(diagnostics[3].File == "/home/me/Game/Scripts/Enemy.cpp");
		CHECK(diagnostics[3].Line == 7);
		CHECK(diagnostics[3].Column == 3);
		CHECK(diagnostics[3].Message == "use of undeclared identifier 'health'");
		CHECK(diagnostics[3].Code.empty());

		CHECK(diagnostics[4].Line == 9);
		CHECK(diagnostics[4].Severity == "warning");

		CHECK(diagnostics[5].File == "collect2");
		CHECK(diagnostics[5].Message == "ld returned 1 exit status");

		CHECK(diagnostics[6].File == "CMakeLists.txt");
		CHECK(diagnostics[6].Line == 8);
		CHECK(diagnostics[6].Severity == "error");
		CHECK(diagnostics[6].Message == "Could not find a package configuration file provided by \"StrataScriptCore\"");

		CHECK(ParseScriptBuildDiagnostics(log, 2).size() == 2);
		CHECK(ParseScriptBuildDiagnostics("Build succeeded.\n    0 Warning(s)\n    0 Error(s)\n").empty());
		CHECK(ParseScriptBuildDiagnostics("").empty());
	}

	TEST_CASE("Long diagnostics are parsed and their messages bounded")
	{
		// Template errors produce long messages; they must parse like short ones.
		const std::string type = "std::vector<std::pair<std::string,std::map<int,float>>>";
		std::string message = "'void Accept(int)': cannot convert argument 1 from '";
		while (message.size() < 3000)
			message += type;
		message += "' to 'int'";
		const std::string msvc = "G:\\Game\\Scripts\\Player.cpp(40,9): error C2664: " + message + " [G:\\Game\\GameScripts.vcxproj]\n";
		const std::string gnu = "/home/me/Game/Scripts/Player.cpp:40:9: error: " + message + "\n";

		std::vector<ScriptDiagnostic> diagnostics = ParseScriptBuildDiagnostics(msvc + gnu);
		REQUIRE(diagnostics.size() == 2);
		CHECK(diagnostics[0].Code == "C2664");
		CHECK(diagnostics[0].Line == 40);
		CHECK(diagnostics[0].Message == message);
		CHECK(diagnostics[1].File == "/home/me/Game/Scripts/Player.cpp");
		CHECK(diagnostics[1].Message == message);

		// Huge messages are cut (at a UTF-8 character boundary); the parse stays linear.
		std::string huge = "G:\\Game\\Scripts\\Big.cpp(1): error C1000: ";
		while (huge.size() < 200000)
			huge += "\xC3\xA9x"; // "éx"
		diagnostics = ParseScriptBuildDiagnostics(huge + "\n" + gnu);
		REQUIRE(diagnostics.size() == 2);
		CHECK(diagnostics[0].Code == "C1000");
		CHECK(diagnostics[0].Message.size() <= 4096 + 3);
		CHECK(diagnostics[0].Message.size() > 4000);
		CHECK(diagnostics[0].Message.ends_with("x..."));
		CHECK(diagnostics[1].File == "/home/me/Game/Scripts/Player.cpp");
	}

	TEST_CASE("Diagnostic locations and markers are recognized exactly")
	{
		std::vector<ScriptDiagnostic> diagnostics = ParseScriptBuildDiagnostics(
			"C:/Game/Scripts/Enemy.cpp:3:14: fatal error: Missing.h: No such file or directory\n"
			"  C:\\Program Files\\MSBuild\\Microsoft.CppCommon.targets(558,5): error MSB8066: Custom build exited with code 1. [G:\\x.vcxproj]\n"
			"ld.lld: error: undefined symbol: Spin()\n"
			"Note: errors are counted below\n"
			"Player.cpp(7): error: no code here\n"
			": error: nothing in front\n"
			"G:\\Game\\error.cpp(2,1): warning C4100: 'unused': unreferenced parameter\n");
		REQUIRE(diagnostics.size() == 5);

		CHECK(diagnostics[0].File == "C:/Game/Scripts/Enemy.cpp");
		CHECK(diagnostics[0].Line == 3);
		CHECK(diagnostics[0].Column == 14);
		CHECK(diagnostics[0].Severity == "error");
		CHECK(diagnostics[0].Message == "Missing.h: No such file or directory");

		CHECK(diagnostics[1].File == "C:\\Program Files\\MSBuild\\Microsoft.CppCommon.targets");
		CHECK(diagnostics[1].Line == 558);
		CHECK(diagnostics[1].Code == "MSB8066");
		CHECK(diagnostics[1].Message == "Custom build exited with code 1.");

		CHECK(diagnostics[2].File == "ld.lld");
		CHECK(diagnostics[2].Line == 0);
		CHECK(diagnostics[2].Message == "undefined symbol: Spin()");

		CHECK(diagnostics[3].File == "Player.cpp");
		CHECK(diagnostics[3].Line == 7);
		CHECK(diagnostics[3].Code.empty());

		CHECK(diagnostics[4].File == "G:\\Game\\error.cpp");
		CHECK(diagnostics[4].Severity == "warning");
		CHECK(diagnostics[4].Code == "C4100");
	}

	TEST_CASE("The log tail keeps the last lines")
	{
		CHECK(GetLogTail("a\nb\nc\nd\n", 2) == "c\nd\n");
		CHECK(GetLogTail("a\r\nb", 5) == "a\nb\n");
		CHECK(GetLogTail("", 3).empty());
	}

	TEST_CASE("New script builds come from a template and never overwrite files")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ScriptProject") / "Dungeon";
		std::string error;
		Ref<Project> project = Project::Create(directory, "Dungeon Crawler", &error);
		REQUIRE_MESSAGE(project, error);

		const std::string cmakeLists = MakeScriptCMakeLists(*project);
		CHECK(cmakeLists.find("project(DungeonCrawlerScripts CXX)") != std::string::npos);
		CHECK(cmakeLists.find("strata_add_script_module(DungeonCrawlerScripts SOURCE_DIR") != std::string::npos);
		CHECK(cmakeLists.find("find_package(StrataScriptCore CONFIG REQUIRED PATHS \"${STRATA_ENGINE_DIR}/StrataScriptCore/CMake\" NO_DEFAULT_PATH)") != std::string::npos);
		CHECK(MakeExampleScript().find("ST_SCRIPT_CLASS(Spinner)") != std::string::npos);

		std::vector<std::filesystem::path> created;
		REQUIRE_MESSAGE(CreateScriptProjectFiles(*project, false, &created, &error), error);
		REQUIRE(created.size() == 1);
		CHECK(created[0] == project->GetScriptSourceDirectory() / "CMakeLists.txt");
		CHECK(FileSystem::ReadText(created[0]) == cmakeLists);
		CHECK_FALSE(FileSystem::Exists(project->GetScriptSourceDirectory() / "Spinner.cpp"));

		// Existing files are kept; only missing ones are written.
		REQUIRE(FileSystem::WriteText(created[0], "# Edited by hand\n"));
		created.clear();
		REQUIRE_MESSAGE(CreateScriptProjectFiles(*project, true, &created, &error), error);
		REQUIRE(created.size() == 1);
		CHECK(created[0] == project->GetScriptSourceDirectory() / "Spinner.cpp");
		CHECK(FileSystem::ReadText(project->GetScriptSourceDirectory() / "CMakeLists.txt") == "# Edited by hand\n");
	}

	TEST_CASE("A script build needs the project's CMakeLists.txt")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ScriptBuildMissing");
		std::string error;
		Ref<Project> project = Project::Create(directory, "Empty", &error);
		REQUIRE_MESSAGE(project, error);

		ScriptBuilder builder;
		CHECK_FALSE(builder.Start(*project, ScriptBuildSettings::GetEngineDefaults(), &error));
		CHECK(error.find("CMakeLists.txt") != std::string::npos);
		CHECK(error.find("script.init") != std::string::npos);
		CHECK_FALSE(builder.IsRunning());
		CHECK(builder.GetLastResult().ID == 0);
		CHECK_FALSE(builder.Update());
	}
}
