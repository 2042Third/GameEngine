#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/ExternalEditor.h"
#include "Editor/ScriptBuild.h"
#include "Editor/ScriptProject.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "Scripting/ScriptTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scripting/ScriptingComponents.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// An editor with a new project (its script build and example), commands run through a runner and frames as the
	// application runs them.
	struct WorkflowHarness
	{
		EditorContext Context;
		EditorCommandRegistry Commands;
		EditorCommandRunner Runner;
		std::filesystem::path Directory;

		explicit WorkflowHarness(const EditorContextSpecification& specification = EditorContextSpecification { false, false })
			: Context(specification), Directory(CreateTemporaryDirectory("ScriptWorkflow") / "Workflow")
		{
			Run("project.create", { { "directory", FileSystem::ToUTF8(Directory) }, { "name", "Workflow" } });
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			const EditorCommandResult result = Tests::CompleteCommand(Context, Commands.Execute(Context, name, parameters));
			INFO(std::string(name), ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		}

		EditorCommandResult Fail(std::string_view name, const nlohmann::json& parameters)
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			REQUIRE_FALSE(result.IsPending());
			CHECK_FALSE(result.Success);
			return result;
		}

		// Runs a command through the runner until it completes (60 s at most).
		EditorCommandResult Complete(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			std::optional<EditorCommandResult> outcome;
			Runner.Run(Context, Commands, name, parameters, [&outcome](const EditorCommandResult& result) { outcome = result; });
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
			while (!outcome && std::chrono::steady_clock::now() < deadline)
			{
				Context.Update(Timestep(1.0f / 60.0f));
				Runner.Update(Context);
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			REQUIRE(outcome);
			return *outcome;
		}

		const Project& GetProject() { return *Context.GetProject(); }
	};

	EditorContextSpecification MakeFakeBuildSpecification()
	{
		EditorContextSpecification specification { false, false };
		specification.ScriptBuild.CMake = GetTestExecutablePath();
		return specification;
	}

	// Waits for a file a helper process writes (10 s at most) and returns its lines.
	std::vector<std::string> WaitForLines(const std::filesystem::path& file)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!FileSystem::IsRegularFile(file) && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		const std::optional<std::string> text = FileSystem::ReadText(file);
		REQUIRE_MESSAGE(text, "The external editor did not start: no ", FileSystem::ToUTF8(file));
		std::vector<std::string> lines;
		size_t start = 0;
		for (size_t end = text->find('\n'); end != std::string::npos; end = text->find('\n', start))
		{
			lines.push_back(text->substr(start, end - start));
			start = end + 1;
		}
		return lines;
	}

	// A command that runs the test executable as the external editor: it writes its arguments to `output`.
	std::string MakeRecordingEditorCommand(const std::filesystem::path& output)
	{
		return fmt::format("\"{}\" --strata-test-helper=record-arguments \"{}\" {{file}} {{line}} {{column}}", FileSystem::ToUTF8(GetTestExecutablePath()),
			FileSystem::ToUTF8(output));
	}

	// What separates the folders of PATH.
#if defined(ST_PLATFORM_WINDOWS)
	constexpr const char* c_PathListSeparator = ";";
#else
	constexpr const char* c_PathListSeparator = ":";
#endif

	// The file name of a program called `name` ("name.exe" on Windows).
	std::string ProgramFileName(const std::string& name)
	{
#if defined(ST_PLATFORM_WINDOWS)
		return name + ".exe";
#else
		return name;
#endif
	}

	// Writes a file that stands for a program: finding a program only needs it to be there (and, on POSIX, executable).
	std::filesystem::path WriteProgramFile(const std::filesystem::path& file)
	{
		REQUIRE(FileSystem::CreateDirectories(file.parent_path()));
		REQUIRE(FileSystem::WriteText(file, "Not really a program\n"));
		std::error_code error;
		std::filesystem::permissions(file, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add, error);
		REQUIRE_FALSE(error);
		return file;
	}

	// Puts the API test module at `module` as a module built before the project's sources were written. A copy keeps the
	// original's write time on Windows but gets the current time on Linux and macOS, so the time is set explicitly.
	void WriteOutdatedModule(const std::filesystem::path& module)
	{
		REQUIRE(FileSystem::CreateDirectories(module.parent_path()));
		REQUIRE(FileSystem::Copy(GetTestScriptModule(STRATA_TEST_SCRIPTS_API), module));
		std::error_code error;
		std::filesystem::last_write_time(module, std::filesystem::file_time_type::clock::now() - std::chrono::hours(1), error);
		REQUIRE_FALSE(error);
	}

	bool IsSameFile(const std::filesystem::path& a, const std::filesystem::path& b)
	{
		std::error_code error;
		return std::filesystem::equivalent(a, b, error);
	}

	std::string Preferred(const std::filesystem::path& path)
	{
		std::filesystem::path native = path;
		return FileSystem::ToUTF8(native.make_preferred());
	}

}

TEST_SUITE("Editor.ScriptWorkflow")
{
	TEST_CASE("Script class names are identifiers that neither the project nor the SDK declare")
	{
		CHECK(IsValidScriptIdentifier("PlayerController"));
		CHECK(IsValidScriptIdentifier("_private"));
		CHECK(IsValidScriptIdentifier("Enemy2"));
		CHECK_FALSE(IsValidScriptIdentifier(""));
		CHECK_FALSE(IsValidScriptIdentifier("2Fast"));
		CHECK_FALSE(IsValidScriptIdentifier("Bad-Name"));
		CHECK_FALSE(IsValidScriptIdentifier("Two Words"));
		CHECK_FALSE(IsValidScriptIdentifier("class"));
		CHECK_FALSE(IsValidScriptIdentifier("override"));
		CHECK_FALSE(IsValidScriptIdentifier("std"));
		CHECK_FALSE(IsValidScriptIdentifier("Reserved__Name"));
		CHECK_FALSE(IsValidScriptIdentifier("_Upper"));
		CHECK_FALSE(IsValidScriptIdentifier(std::string(129, 'a')));
		CHECK_FALSE(IsValidScriptIdentifier("Caf\xC3\xA9"));

		WorkflowHarness harness;
		const std::filesystem::path engine = ScriptBuildSettings::GetEngineDefaults().EngineDirectory;
		const std::vector<std::string> sdk = FindDeclaredScriptTypes(engine / "StrataScriptCore" / "Include" / "StrataScript");
		for (const char* name : { "Script", "Entity", "Input", "CharacterController", "GamepadButton", "Timer" })
		{
			INFO(name);
			CHECK(std::binary_search(sdk.begin(), sdk.end(), std::string(name)));
		}
		CHECK_FALSE(ValidateNewScriptClass(harness.GetProject(), engine, "PlayerController"));
		const std::optional<std::string> example = ValidateNewScriptClass(harness.GetProject(), engine, "Spinner"); // The example script
		REQUIRE(example);
		CHECK(example->find("declare") != std::string::npos);
		const std::optional<std::string> sdkName = ValidateNewScriptClass(harness.GetProject(), engine, "Input");
		REQUIRE(sdkName);
		CHECK(sdkName->find("SDK") != std::string::npos);
		CHECK(ValidateNewScriptClass(harness.GetProject(), engine, "not valid"));

		// Names the SDK's usage comments mention (ST_SCRIPT_CLASS(Player), ST_SCRIPT_CLASS(Spinner)) are no names of the SDK,
		// and neither are names in the project's comments and literals.
		CHECK_FALSE(std::binary_search(sdk.begin(), sdk.end(), std::string("Player")));
		CHECK_FALSE(std::binary_search(sdk.begin(), sdk.end(), std::string("Spinner")));
		CHECK_FALSE(ValidateNewScriptClass(harness.GetProject(), engine, "Player"));
		const std::filesystem::path scripts = harness.GetProject().GetScriptSourceDirectory();
		REQUIRE(FileSystem::Remove(scripts / "Spinner.cpp"));
		CHECK_FALSE(ValidateNewScriptClass(harness.GetProject(), engine, "Spinner"));
		REQUIRE(FileSystem::WriteText(scripts / "Notes.h", "#pragma once\n"
			"// class Ghost : public Script, one day\n"
			"/* struct Phantom\n   ST_SCRIPT_CLASS(Wraith) */\n"
			"inline const char* c_Banner = \"enum Banner\";\n"
			"inline const char* c_Raw = R\"text(class Raw \"quoted\" )\" struct Still)text\";\n"
			"inline constexpr int c_Million = 1'000'000; struct Counted {};\n"
			"inline constexpr char c_Quote = '\"'; struct AfterQuote {};\n"));
		const std::vector<std::string> declared = FindDeclaredScriptTypes(scripts);
		CHECK(declared == std::vector<std::string> { "AfterQuote", "Counted" });
		for (const char* name : { "Ghost", "Phantom", "Wraith", "Banner", "Raw", "Still" })
		{
			INFO(name);
			CHECK_FALSE(ValidateNewScriptClass(harness.GetProject(), engine, name));
		}
		CHECK(ValidateNewScriptClass(harness.GetProject(), engine, "Counted"));
	}

	TEST_CASE("script.create writes a script class with its fields, and refuses what would not build")
	{
		WorkflowHarness harness;
		const std::filesystem::path scripts = harness.GetProject().GetScriptSourceDirectory();
		const nlohmann::json created = harness.Run("script.create", { { "class", "PlayerController" },
			{ "fields", { { "Speed", "float" }, { "Target", "Entity" }, { "Label", "string" }, { "Jumps", "int" } } } });
		CHECK(created["class"] == "PlayerController");
		CHECK(FileSystem::FromUTF8(created["file"].get<std::string>()) == scripts / "PlayerController.cpp");
		CHECK(created["relativePath"] == "Scripts/PlayerController.cpp");
		const std::string source = FileSystem::ReadText(scripts / "PlayerController.cpp").value_or("");
		CHECK(source.find("#include \"StrataScript/StrataScript.h\"") != std::string::npos);
		CHECK(source.find("class PlayerController : public Script") != std::string::npos);
		CHECK(source.find("void OnCreate() override") != std::string::npos);
		CHECK(source.find("void OnUpdate([[maybe_unused]] float deltaTime) override") != std::string::npos);
		CHECK(source.find("\tfloat Speed = 1.0f;") != std::string::npos);
		CHECK(source.find("\tEntity Target;") != std::string::npos);
		CHECK(source.find("\tstd::string Label;") != std::string::npos);
		CHECK(source.find("\tint32_t Jumps = 0;") != std::string::npos);
		CHECK(source.find("ST_SCRIPT_CLASS(PlayerController)") != std::string::npos);
		for (const char* field : { "Speed", "Target", "Label", "Jumps" })
			CHECK(source.find(fmt::format("ST_SCRIPT_FIELD({});", field)) != std::string::npos);
		CHECK(source.find("Docs/Manual/Scripting.md") != std::string::npos);

		// An empty class in a folder of its own; the behaviour template's example field without fields.
		harness.Run("script.create", { { "class", "Enemy" }, { "path", "Gameplay/Enemy.cpp" }, { "template", "empty" } });
		const std::string empty = FileSystem::ReadText(scripts / "Gameplay" / "Enemy.cpp").value_or("");
		CHECK(empty.find("class Enemy : public Script") != std::string::npos);
		CHECK(empty.find("OnUpdate") == std::string::npos);
		harness.Run("script.create", { { "class", "Pickup" } });
		CHECK(FileSystem::ReadText(scripts / "Pickup.cpp").value_or("").find("ST_SCRIPT_FIELD(Speed);") != std::string::npos);

		// What would not build, or would leave the script directory, is refused without writing anything.
		CHECK(harness.Fail("script.create", { { "class", "PlayerController" }, { "path", "Other.cpp" } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("script.create", { { "class", "Input" } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("script.create", { { "class", "two words" } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("script.create", { { "class", "Escape" }, { "path", "../Escape.cpp" } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("script.create", { { "class", "Header" }, { "path", "Header.h" } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("script.create", { { "class", "Absolute" }, { "path", FileSystem::ToUTF8(harness.Directory / "Absolute.cpp") } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("script.create", { { "class", "Weird" }, { "fields", { { "Speed", "double" } } } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("script.create", { { "class", "Weird" }, { "fields", { { "class", "int" } } } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("script.create", { { "class", "Weird" }, { "template", "behavior" } }).ErrorKind == EditorCommandError::InvalidParameters);
		REQUIRE(FileSystem::WriteText(scripts / "Taken.cpp", "// Someone's file\n"));
		CHECK(harness.Fail("script.create", { { "class", "Fresh" }, { "path", "Taken.cpp" } }).ErrorKind == EditorCommandError::Failed);
		CHECK(FileSystem::ReadText(scripts / "Taken.cpp") == std::optional<std::string>("// Someone's file\n"));
		CHECK_FALSE(FileSystem::Exists(harness.Directory / "Escape.cpp"));

		// A project without a script build gets one first.
		REQUIRE(FileSystem::Remove(scripts / "CMakeLists.txt"));
		const nlohmann::json withBuild = harness.Run("script.create", { { "class", "Spawner" } });
		CHECK(FileSystem::IsRegularFile(scripts / "CMakeLists.txt"));
		bool listed = false;
		for (const nlohmann::json& file : withBuild["createdBuildFiles"])
			listed |= FileSystem::FromUTF8(file.get<std::string>()).filename() == "CMakeLists.txt";
		CHECK(listed);

		// What agents read: a new class must be built before script.add can attach it, and Play is no way around that.
		const EditorCommand* command = harness.Commands.Find("script.create");
		REQUIRE(command);
		CHECK(command->Description.find("script.build before attaching") != std::string::npos);
		CHECK(command->Description.find("press Play") == std::string::npos);
	}

	TEST_CASE("External editor commands become a program and its arguments, never a shell command")
	{
		const std::filesystem::path file = "Scripts/Player.cpp";
		std::string error;
		std::optional<std::vector<std::string>> arguments = ExternalEditor::ExpandCommand("code -g {file}:{line}:{column}", file, 12, 5, "Scripts", &error);
		REQUIRE_MESSAGE(arguments, error);
		CHECK(*arguments == std::vector<std::string> { "code", "-g", Preferred(file) + ":12:5" });

		// Quotes group (a program in a folder with spaces); without {file} the file goes last; unknown lines are the first.
		arguments = ExternalEditor::ExpandCommand("\"C:/Program Files/Editor/edit.exe\" /Edit --in {directory}", file, 0, 0, "Scripts", &error);
		REQUIRE_MESSAGE(arguments, error);
		CHECK(*arguments == std::vector<std::string> { "C:/Program Files/Editor/edit.exe", "/Edit", "--in", Preferred("Scripts"), Preferred(file) });
		arguments = ExternalEditor::ExpandCommand("edit --line={line} \"{file}\"", file, 0, 0, "Scripts", &error);
		REQUIRE_MESSAGE(arguments, error);
		CHECK(*arguments == std::vector<std::string> { "edit", "--line=1", Preferred(file) });

		// What cannot run without a shell, or makes no sense, is refused.
		for (const char* command : { "", "   ", "\"unbalanced {file}", "edit.cmd {file}", "C:/Tools/EDIT.BAT {file}", "{file} --open", "code {unknown}",
			"code {file", "code\n{file}" })
		{
			INFO(command);
			CHECK_FALSE(ExternalEditor::ExpandCommand(command, file, 1, 1, "Scripts", &error));
			CHECK(ExternalEditor::ValidateCommand(command).has_value());
		}
		CHECK_FALSE(ExternalEditor::ValidateCommand("code -g {file}:{line}"));
	}

	TEST_CASE("The external editor's program is found as the system finds programs: a path, or a name in PATH")
	{
		const std::filesystem::path tools = CreateTemporaryDirectory("EditorPrograms");
		const std::filesystem::path editor = WriteProgramFile(tools / "Bin" / ProgramFileName("myeditor"));
		ScopedEnvironmentVariable path("PATH", FileSystem::ToUTF8(tools / "Empty") + c_PathListSeparator + FileSystem::ToUTF8(tools / "Bin"));
		std::string error;
		std::optional<std::filesystem::path> program = ExternalEditor::ResolveProgram("myeditor --goto {file}:{line}", &error);
		REQUIRE_MESSAGE(program, error);
		CHECK(IsSameFile(*program, editor));
		program = ExternalEditor::ResolveProgram(fmt::format("\"{}\" {{file}}", FileSystem::ToUTF8(editor)), &error);
		REQUIRE_MESSAGE(program, error);
		CHECK(IsSameFile(*program, editor));

		// What is not there is refused, saying where it was looked for.
		CHECK_FALSE(ExternalEditor::ResolveProgram("noeditor {file}", &error));
		CHECK(error.find("'noeditor' in the 2 folders of PATH") != std::string::npos);
		CHECK_FALSE(ExternalEditor::ResolveProgram(fmt::format("\"{}\" {{file}}", FileSystem::ToUTF8(tools / "Missing" / "editor")), &error));
		CHECK(error.find("there is no program") != std::string::npos);
		CHECK_FALSE(ExternalEditor::ResolveProgram("myeditor {unknown}", &error)); // And what cannot be a command at all

#if defined(ST_PLATFORM_WINDOWS)
		// VS Code puts a batch file in PATH (bin/code.cmd, beside a shell script named code) that starts the editor beside its
		// folder, which takes the same arguments: that program is the one to start. Other batch files cannot be.
		const std::filesystem::path vscode = tools / "Microsoft VS Code";
		WriteProgramFile(vscode / "bin" / "code.cmd");
		WriteProgramFile(vscode / "bin" / "code");
		WriteProgramFile(tools / "Scripts" / "edit.cmd");
		ScopedEnvironmentVariable launchers("PATH", FileSystem::ToUTF8(vscode / "bin") + c_PathListSeparator + FileSystem::ToUTF8(tools / "Scripts"));
		CHECK_FALSE(ExternalEditor::ResolveProgram("code -g {file}", &error)); // Its editor is missing
		CHECK(error.find("batch file") != std::string::npos);
		const std::filesystem::path code = WriteProgramFile(vscode / "Code.exe");
		program = ExternalEditor::ResolveProgram("code -g {file}:{line}:{column}", &error);
		REQUIRE_MESSAGE(program, error);
		CHECK(IsSameFile(*program, code));
		CHECK_FALSE(ExternalEditor::ResolveProgram("edit {file}", &error));
		CHECK(error.find("batch file") != std::string::npos);
#endif
	}

	TEST_CASE("script.setExternalEditor refuses a program that is not there and reports the one it starts")
	{
		ScopedEnvironmentVariable noOverride("STRATA_EXTERNAL_EDITOR", "");
		WorkflowHarness harness;
		const std::filesystem::path editor = WriteProgramFile(CreateTemporaryDirectory("EditorPrograms") / ProgramFileName("myeditor"));
		ScopedEnvironmentVariable path("PATH", FileSystem::ToUTF8(editor.parent_path()));
		const EditorCommandResult refused = harness.Fail("script.setExternalEditor", { { "command", "noeditor -g {file}:{line}" } });
		CHECK(refused.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(refused.Error.find("there is no program 'noeditor'") != std::string::npos);
		CHECK(harness.Context.GetExternalEditor().GetUserCommand().empty());
		const nlohmann::json set = harness.Run("script.setExternalEditor", { { "command", "myeditor -g {file}:{line}" } });
		CHECK(set["userCommand"] == "myeditor -g {file}:{line}");
		REQUIRE(set["program"].is_string());
		CHECK(IsSameFile(FileSystem::FromUTF8(set["program"].get<std::string>()), editor));
		CHECK(harness.Run("script.setExternalEditor", { { "command", "" } })["program"].is_null());
	}

	TEST_CASE("The external editor setting is the user's, kept in their settings file; the environment overrides it")
	{
		ScopedEnvironmentVariable noOverride("STRATA_EXTERNAL_EDITOR", "");
		const std::filesystem::path settings = CreateTemporaryDirectory("EditorSettings") / "Strata" / "EditorSettings.json";
		// The editor the setting names is a program in PATH.
		const std::filesystem::path tools = CreateTemporaryDirectory("EditorPrograms");
		WriteProgramFile(tools / ProgramFileName("code"));
		ScopedEnvironmentVariable path("PATH", FileSystem::ToUTF8(tools));
		{
			ExternalEditorLauncher launcher(settings);
			CHECK(launcher.GetSource() == ExternalEditorSource::None);
			std::string error;
			CHECK_FALSE(launcher.SetUserCommand("edit.bat {file}", &error));
			CHECK(error.find("batch") != std::string::npos);
			REQUIRE_MESSAGE(launcher.SetUserCommand("code -g {file}:{line}", &error), error);
			CHECK(launcher.GetSource() == ExternalEditorSource::User);
		}
		CHECK(Platform::IsTrustedFile(settings));
		{
			ExternalEditorLauncher launcher(settings);
			CHECK(launcher.GetUserCommand() == "code -g {file}:{line}");
			ScopedEnvironmentVariable environment("STRATA_EXTERNAL_EDITOR", "subl {file}:{line}");
			CHECK(launcher.GetSource() == ExternalEditorSource::Environment);
			CHECK(launcher.GetCommand() == "subl {file}:{line}");
			REQUIRE(launcher.SetUserCommand(""));
		}
		ExternalEditorLauncher cleared(settings);
		CHECK(cleared.GetUserCommand().empty());

		// A settings file that is not JSON is ignored (with a warning), not trusted blindly.
		REQUIRE(Platform::WritePrivateFile(settings, "not json"));
		LogCapture log;
		ExternalEditorLauncher garbage(settings);
		CHECK(garbage.GetUserCommand().empty());
		bool warned = false;
		for (const LogEntry& entry : log.GetEntries())
			warned |= entry.Message.find("are ignored") != std::string::npos;
		CHECK(warned);
	}

	TEST_CASE("script.open starts the external editor at the line, for sources of the project or the engine only")
	{
		WorkflowHarness harness;
		const std::filesystem::path output = CreateTemporaryDirectory("ExternalEditor") / "Arguments.txt";
		ScopedEnvironmentVariable editor("STRATA_EXTERNAL_EDITOR", MakeRecordingEditorCommand(output));
		const nlohmann::json status = harness.Run("script.status");
		CHECK(status["externalEditor"]["source"] == "environment");

		const nlohmann::json opened = harness.Run("script.open", { { "file", "Scripts/Spinner.cpp" }, { "line", 7 }, { "column", 3 } });
		CHECK(opened["openedWith"] == FileSystem::ToUTF8(GetTestExecutablePath().filename()));
		const std::vector<std::string> lines = WaitForLines(output);
		REQUIRE(lines.size() == 3);
		std::error_code error;
		CHECK(std::filesystem::equivalent(FileSystem::FromUTF8(lines[0]), harness.GetProject().GetScriptSourceDirectory() / "Spinner.cpp", error));
		CHECK(lines[1] == "7");
		CHECK(lines[2] == "3");

		// Only code and text of the project or the engine: the default application of anything else might run it.
		const std::filesystem::path outside = CreateTemporaryDirectory("Outside") / "Notes.cpp";
		REQUIRE(FileSystem::WriteText(outside, "// Not the project's\n"));
		CHECK(harness.Fail("script.open", { { "file", FileSystem::ToUTF8(outside) } }).Error.find("neither in the project nor in the engine") != std::string::npos);
		REQUIRE(FileSystem::WriteText(harness.Directory / "Tool.exe", "MZ"));
		CHECK(harness.Fail("script.open", { { "file", "Tool.exe" } }).Error.find("not a source file") != std::string::npos);
		CHECK(harness.Fail("script.open", { { "file", "Scripts/Missing.cpp" } }).ErrorKind == EditorCommandError::Failed);
		CHECK(harness.Fail("script.open", { { "file", "Scripts/Spinner.cpp" }, { "line", 0 } }).ErrorKind == EditorCommandError::InvalidParameters);

		// The engine's SDK headers (compiler diagnostics name them) open too.
		REQUIRE(FileSystem::Remove(output));
		const std::filesystem::path header = ScriptBuildSettings::GetEngineDefaults().EngineDirectory / "StrataScriptCore" / "Include" / "StrataScript" / "Script.h";
		harness.Run("script.open", { { "file", FileSystem::ToUTF8(header) }, { "line", 1 } });
		CHECK(WaitForLines(output).size() == 3);

		CHECK(harness.Fail("script.setExternalEditor", { { "command", "edit.bat {file}" } }).ErrorKind == EditorCommandError::InvalidParameters);
		const std::string userCommand = fmt::format("\"{}\" -g {{file}}:{{line}}", FileSystem::ToUTF8(GetTestExecutablePath()));
		const nlohmann::json set = harness.Run("script.setExternalEditor", { { "command", userCommand } });
		CHECK(set["userCommand"] == userCommand);
		CHECK(set["source"] == "environment"); // STRATA_EXTERNAL_EDITOR still wins
	}

	TEST_CASE("Projects whose scenes use scripts are asked to build them; a new project's example is not")
	{
		WorkflowHarness harness;
		CHECK_FALSE(harness.Context.DoScenesUseScripts());
		CHECK_FALSE(harness.Context.GetPlayBuildReason());
		CHECK(harness.Run("script.status")["scenesUseScripts"] == false);

		// The edited scene uses one.
		Entity entity = harness.Context.GetEditScene()->CreateEntity("Scripted");
		entity.AddComponent<ScriptComponent>();
		CHECK(harness.Context.DoScenesUseScripts());
		const std::optional<std::string> reason = harness.Context.GetPlayBuildReason();
		REQUIRE(reason);
		CHECK(reason->find("not built") != std::string::npos);
		harness.Run("scene.saveAs", { { "path", "Scenes/Scripted.stscene" } });

		// Reopened, the saved scene's files say so (scanned when the project opens), also while another scene is edited.
		const std::filesystem::path projectFile = harness.GetProject().GetProjectFile();
		harness.Run("project.close");
		LogCapture log;
		std::string error;
		REQUIRE_MESSAGE(harness.Context.OpenProject(projectFile, &error), error);
		harness.Run("scene.new");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!harness.Context.DoScenesUseScripts() && std::chrono::steady_clock::now() < deadline)
		{
			harness.Context.Update(Timestep(1.0f / 60.0f));
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		CHECK(harness.Context.DoScenesUseScripts());
		bool warned = false;
		for (const LogEntry& entry : log.GetEntries())
			warned |= entry.Message.find("are not built yet") != std::string::npos;
		CHECK(warned);
	}

	TEST_CASE("Play builds scripts whose sources changed first, then plays; build: false plays what is loaded")
	{
		ScopedEnvironmentVariable fakeCMake("STRATA_TEST_FAKE_CMAKE", "succeed");
		WorkflowHarness harness(MakeFakeBuildSpecification());
		// A module of the project's own (the stand-in for CMake builds nothing), older than the sources.
		const std::filesystem::path module = harness.GetProject().GetScriptModulePath();
		WriteOutdatedModule(module);
		harness.Run("script.reload");
		const std::optional<std::string> reason = harness.Context.GetPlayBuildReason();
		REQUIRE(reason);
		CHECK(reason->find("changed after the scripts were built") != std::string::npos);
		CHECK(harness.Run("script.status")["playBuildsFirst"].is_string());

		// play.start answers once the build finished and the game plays; paused, before its first step.
		const uint64_t buildsBefore = harness.Context.GetScriptBuilder().GetLastResult().ID;
		const EditorCommandResult played = harness.Complete("play.start", { { "paused", true } });
		REQUIRE_MESSAGE(played.Success, played.Error);
		CHECK(harness.Context.IsPlaying());
		CHECK(harness.Context.GetScriptBuilder().GetLastResult().ID == buildsBefore + 1);
		REQUIRE(played.Value.contains("built"));
		CHECK(played.Value["built"]["reason"] == *reason);
		CHECK(played.Value["paused"] == true);
		CHECK(harness.Context.GetPlayBuildID() == 0);
		// Updates of a paused game run no step.
		for (int update = 0; update < 3; update++)
			harness.Context.Update(Timestep(1.0f / 60.0f));
		CHECK(harness.Context.IsPaused());
		CHECK(harness.Context.GetFixedStepCount() == 0);
		harness.Run("play.stop");

		// The build compiled what changed: although it left the module as it was (as an up-to-date build does), the next play
		// plays at once.
		CHECK_FALSE(harness.Context.GetPlayBuildReason());
		CHECK(harness.Run("script.status")["playBuildsFirst"].is_null());
		const nlohmann::json again = harness.Run("play.start");
		CHECK_FALSE(again.contains("built"));
		CHECK(harness.Context.IsPlaying());
		CHECK(harness.Context.GetScriptBuilder().GetLastResult().ID == buildsBefore + 1);
		harness.Run("play.stop");

		// build: false plays at once.
		harness.Run("play.start", { { "build", false } });
		CHECK(harness.Context.IsPlaying());
		harness.Run("play.stop");

		// A module loaded from elsewhere is played as it is.
		harness.Run("script.load", { { "path", FileSystem::ToUTF8(GetTestScriptModule(STRATA_TEST_SCRIPTS_API)) } });
		CHECK_FALSE(harness.Context.GetPlayBuildReason());
		harness.Run("play.start");
		CHECK(harness.Context.IsPlaying());
	}

	TEST_CASE("Play refuses a scene being opened before it builds anything")
	{
		ScopedEnvironmentVariable fakeCMake("STRATA_TEST_FAKE_CMAKE", "succeed");
		WorkflowHarness harness(MakeFakeBuildSpecification());
		// Scripts that need a build: a module of the project's own, older than the sources.
		const std::filesystem::path module = harness.GetProject().GetScriptModulePath();
		WriteOutdatedModule(module);
		harness.Run("script.reload");
		REQUIRE(harness.Context.GetPlayBuildReason());

		// A saved scene, opened again in the background from a new one.
		const nlohmann::json saved = harness.Run("scene.saveAs", { { "path", "Scenes/Level.stscene" } });
		harness.Run("scene.new");
		EditorCommandResult opening = harness.Commands.Execute(harness.Context, "scene.open", { { "scene", saved["scene"] } });
		REQUIRE_MESSAGE(opening.Success, opening.Error);
		REQUIRE(opening.IsPending());
		REQUIRE(harness.Context.IsSceneLoading());

		// Play says why at once: no build starts for a play that would be refused after it.
		const uint64_t buildsBefore = harness.Context.GetScriptBuilder().GetLastResult().ID;
		const EditorCommandResult play = harness.Commands.Execute(harness.Context, "play.start");
		REQUIRE_FALSE(play.IsPending());
		CHECK_FALSE(play.Success);
		CHECK(play.Error.find("being opened") != std::string::npos);
		CHECK_FALSE(harness.Context.GetScriptBuilder().IsRunning());
		CHECK(harness.Context.GetPlayBuildID() == 0);
		CHECK_FALSE(harness.Context.IsPlaying());

		// Once the scene is open, Play builds first as before.
		opening = CompleteCommand(harness.Context, std::move(opening));
		REQUIRE_MESSAGE(opening.Success, opening.Error);
		CHECK(harness.Context.GetScriptBuilder().GetLastResult().ID == buildsBefore);
		const EditorCommandResult played = harness.Complete("play.start");
		REQUIRE_MESSAGE(played.Success, played.Error);
		CHECK(played.Value.contains("built"));
		CHECK(harness.Context.IsPlaying());
		CHECK(harness.Context.GetScriptBuilder().GetLastResult().ID == buildsBefore + 1);
	}

	TEST_CASE("Play builds again for sources added, changed or removed since its build, whatever the module compiles")
	{
		ScopedEnvironmentVariable fakeCMake("STRATA_TEST_FAKE_CMAKE", "succeed");
		WorkflowHarness harness(MakeFakeBuildSpecification());
		const std::filesystem::path scripts = harness.GetProject().GetScriptSourceDirectory();
		const std::filesystem::path module = harness.GetProject().GetScriptModulePath();
		WriteOutdatedModule(module);
		harness.Run("script.reload");
		// The stand-in for CMake builds nothing: the module stays as it was, older than every source.
		const auto build = [&harness]()
		{
			const EditorCommandResult built = harness.Complete("script.build", { { "wait", true } });
			REQUIRE_MESSAGE(built.Success, built.Error);
			CHECK(built.Value["moduleChanged"] == false);
		};
		const auto reason = [&harness]() { return harness.Context.GetPlayBuildReason().value_or(std::string()); };
		REQUIRE_FALSE(reason().empty());
		build();
		CHECK(reason().empty());

		// Files the module does not compile count too: a header no code includes yet, C code, CMake files.
		REQUIRE(FileSystem::WriteText(scripts / "Unused.hpp", "#pragma once\n"));
		CHECK(reason() == "'Scripts/Unused.hpp' was added after the scripts were built");
		build();
		CHECK(reason().empty());
		REQUIRE(FileSystem::WriteText(scripts / "Helpers.c", "int Helper(void) { return 1; }\n"));
		REQUIRE(FileSystem::WriteText(scripts / "Player.CPP", "// The extension's case does not matter\n"));
		CHECK(reason().find("was added after the scripts were built") != std::string::npos);
		build();
		CHECK(reason().empty());

		// A changed file, a file that was removed.
		REQUIRE(FileSystem::WriteText(scripts / "Spinner.cpp", *FileSystem::ReadText(scripts / "Spinner.cpp") + "// Changed\n"));
		CHECK(reason() == "'Scripts/Spinner.cpp' changed after the scripts were built");
		build();
		CHECK(reason().empty());
		REQUIRE(FileSystem::Remove(scripts / "Player.CPP"));
		CHECK(reason() == "'Scripts/Player.CPP' was removed after the scripts were built");
		build();
		CHECK(reason().empty());

		// An IDE's build tree inside the scripts (its CMake files, its compiler checks) and hidden folders are not sources.
		const std::filesystem::path ide = scripts / "out" / "build";
		REQUIRE(FileSystem::CreateDirectories(ide / "CMakeFiles"));
		REQUIRE(FileSystem::WriteText(ide / "CMakeCache.txt", "# CMake's cache\n"));
		REQUIRE(FileSystem::WriteText(ide / "CMakeFiles" / "CMakeCXXCompilerId.cpp", "int main() { return 0; }\n"));
		REQUIRE(FileSystem::WriteText(ide / "CMakeFiles" / "Targets.cmake", "# Generated\n"));
		REQUIRE(FileSystem::CreateDirectories(scripts / ".vscode"));
		REQUIRE(FileSystem::WriteText(scripts / ".vscode" / "Tasks.cmake", "# The IDE's\n"));
		CHECK(reason().empty());

		// A module rebuilt elsewhere (an IDE's build, hot reloaded) was built from what is there: only sources newer than it
		// count then.
		std::error_code error;
		std::filesystem::last_write_time(module, std::filesystem::file_time_type::clock::now() + std::chrono::hours(1), error);
		REQUIRE_FALSE(error);
		CHECK(reason().empty());
		REQUIRE(FileSystem::Remove(scripts / "Unused.hpp"));
		CHECK(reason().empty()); // Its removal leaves nothing newer than the module
	}

	TEST_CASE("The build after script.create, or after a file is added by hand, configures the scripts first")
	{
		// The stand-in for CMake builds nothing; whether a build ran the configure step is what is checked. A Visual Studio
		// build of a tree configured before the class existed would leave the class out of the module.
		ScopedEnvironmentVariable fakeCMake("STRATA_TEST_FAKE_CMAKE", "succeed");
		WorkflowHarness harness(MakeFakeBuildSpecification());
		const std::filesystem::path scripts = harness.GetProject().GetScriptSourceDirectory();
		WriteOutdatedModule(harness.GetProject().GetScriptModulePath());
		// Builds through the command, as the UI and agents do; true when the build configured its tree.
		const auto build = [&harness]()
		{
			const EditorCommandResult built = harness.Complete("script.build", { { "wait", true } });
			REQUIRE_MESSAGE(built.Success, built.Error);
			return built.Value["configured"].get<bool>();
		};

		CHECK(build());
		// CMake's configure step leaves its cache in the tree.
		REQUIRE(FileSystem::WriteText(harness.Context.GetScriptBuilder().GetLastResult().BuildDirectory / "CMakeCache.txt", "# configured\n"));
		CHECK_FALSE(build());

		harness.Run("script.create", { { "class", "Mover" } });
		CHECK(build());
		CHECK_FALSE(build());

		REQUIRE(FileSystem::WriteText(scripts / "Gameplay" / "Jumper.cpp", MakeScriptSource("Jumper", ScriptTemplate::Behaviour, {})));
		CHECK(build());
		CHECK_FALSE(build());
	}

	TEST_CASE("A play whose build fails fails with the build's errors and asks for the Problems panel")
	{
		ScopedEnvironmentVariable fakeCMake("STRATA_TEST_FAKE_CMAKE", "fail");
		WorkflowHarness harness(MakeFakeBuildSpecification());
		Entity entity = harness.Context.GetEditScene()->CreateEntity("Scripted");
		entity.AddComponent<ScriptComponent>();
		const uint64_t requests = harness.Context.GetProblemsPanelRequests();
		const EditorCommandResult played = harness.Complete("play.start");
		CHECK_FALSE(played.Success);
		CHECK(played.ErrorKind == EditorCommandError::Failed);
		CHECK(played.Error.find("Broken.cpp(7,5)") != std::string::npos);
		CHECK_FALSE(harness.Context.IsPlaying());
		CHECK(harness.Context.GetProblemsPanelRequests() == requests + 1);
		const ScriptBuildResult& build = harness.Context.GetScriptBuilder().GetLastResult();
		REQUIRE(build.Diagnostics.size() == 2);
		CHECK(build.Diagnostics[0].File == "Scripts/Broken.cpp");
		CHECK(build.Diagnostics[1].Severity == "warning");
	}

	TEST_CASE("A play waiting for its build stops when the build is cancelled")
	{
		WorkflowHarness harness(MakeFakeBuildSpecification());
		Entity entity = harness.Context.GetEditScene()->CreateEntity("Scripted");
		entity.AddComponent<ScriptComponent>();
		// The stand-in for CMake: its build is cancelled before the editor saw it end.
		ScopedEnvironmentVariable fakeCMake("STRATA_TEST_FAKE_CMAKE", "succeed");
		std::optional<EditorCommandResult> outcome;
		harness.Runner.Run(harness.Context, harness.Commands, "play.start", nlohmann::json::object(), [&outcome](const EditorCommandResult& result) { outcome = result; });
		REQUIRE(harness.Context.GetScriptBuilder().IsRunning());
		CHECK(harness.Context.GetPlayBuildID() == harness.Context.GetScriptBuilder().GetCurrentID());
		const uint64_t requests = harness.Context.GetProblemsPanelRequests();
		LogCapture log;
		harness.Context.CancelScriptBuild();
		harness.Context.Update(Timestep(1.0f / 60.0f));
		harness.Runner.Update(harness.Context);
		REQUIRE(outcome);
		CHECK_FALSE(outcome->Success);
		CHECK(outcome->Error.find("cancelled") != std::string::npos);
		CHECK_FALSE(harness.Context.IsPlaying());
		CHECK(harness.Context.GetPlayBuildID() == 0);
		// Someone ended the build on purpose: play.start was cancelled, nothing failed, and nothing asks for the Problems panel
		// or writes an error.
		CHECK(outcome->ErrorKind == EditorCommandError::Cancelled);
		CHECK(harness.Context.GetProblemsPanelRequests() == requests);
		const ScriptBuildResult& build = harness.Context.GetScriptBuilder().GetLastResult();
		CHECK(build.Cancelled);
		CHECK_FALSE(build.Success);
		CHECK(harness.Run("script.status")["build"]["last"]["cancelled"] == true);
		for (const LogEntry& entry : log.GetEntries())
			CHECK_MESSAGE(entry.Level < LogLevel::Error, entry.Message);
	}

	TEST_CASE("Console messages that name a source line point to it")
	{
		std::optional<ScriptDiagnostic> reference = FindSourceReference("[Scripts] C:\\Game\\Scripts\\Player.cpp(12,5): error C2065: 'x': undeclared identifier");
		REQUIRE(reference);
		CHECK(reference->File == "C:\\Game\\Scripts\\Player.cpp");
		CHECK(reference->Line == 12);
		CHECK(reference->Column == 5);
		reference = FindSourceReference("/home/me/Game/Scripts/Enemy.h:40:9: error: expected ';'");
		REQUIRE(reference);
		CHECK(reference->File == "/home/me/Game/Scripts/Enemy.h");
		CHECK(reference->Line == 40);
		reference = FindSourceReference("The script crash in Player::OnUpdate happened at C:/My Game/Scripts/Player.cpp(42) in Player::OnUpdate");
		REQUIRE(reference);
		CHECK(reference->File == "C:/My Game/Scripts/Player.cpp");
		CHECK(reference->Line == 42);
		CHECK_FALSE(FindSourceReference("Opened project 'Game' (C:/Game)"));
		CHECK_FALSE(FindSourceReference("Loaded at C:/Game/Textures/Wall.png(3)"));
		CHECK_FALSE(FindSourceReference("LINK : fatal error LNK1104: cannot open file 'Game.dll'"));
	}
}
