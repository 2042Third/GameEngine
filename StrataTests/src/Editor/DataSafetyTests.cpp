#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "Editor/SceneRecovery.h"
#include "Editor/SettingsFileWriter.h"
#include "TestHelpers.h"
#include "UI/ItemProbe.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Project/Project.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// No silent data loss: every command that replaces the edited scene refuses to discard its unsaved changes unless told to
// (force), and says when it did.

using namespace Strata;

namespace
{

	struct SafetyHarness
	{
		std::filesystem::path Root;
		EditorContext Context;
		EditorCommandRegistry Commands;

		explicit SafetyHarness(const std::string& name)
			: Root(Tests::CreateTemporaryDirectory(name)), Context(MakeSpecification(Root))
		{
			Run("project.create", { { "directory", FileSystem::ToUTF8(Root / "Game") }, { "name", "Game" }, { "template", "basic3d" } });
			REQUIRE(Context.GetSceneHandle().IsValid());
			REQUIRE_FALSE(Context.IsSceneModified());
			// Another project to open, and a copy of a sample to open (only Tetris: not every sample of the repository).
			REQUIRE(Project::Create(Root / "Other", "Other") != nullptr);
			std::string error;
			REQUIRE_MESSAGE(Tests::CopySamples(Root / "Samples", { "Tetris" }, &error), error);
		}

		static EditorContextSpecification MakeSpecification(const std::filesystem::path& root)
		{
			EditorContextSpecification specification;
			specification.WatchAssetFiles = false;
			specification.HotReloadScripts = false;
			specification.SamplesDirectory = root / "Samples";
			return specification;
		}

		// Commands that open scenes and projects answer once the scene is open (scene streaming): run to completion.
		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return Tests::RunCommandToCompletion(Context, Commands, name, parameters);
		}

		EditorCommandResult Execute(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return Tests::CompleteCommand(Context, Commands.Execute(Context, name, parameters));
		}
	};

	// What a refused command must leave as it was: the scene, its undo history and the open project.
	struct EditorState
	{
		nlohmann::json Scene;
		AssetHandle SceneHandle = UUID::Null();
		std::vector<std::string> History;
		size_t Position = 0;
		bool Modified = false;
		std::string Project;

		bool operator==(const EditorState& other) const = default;
	};

	EditorState CaptureState(EditorContext& context)
	{
		EditorState state;
		state.Scene = SceneSerializer::Serialize(*context.GetEditScene());
		state.SceneHandle = context.GetSceneHandle();
		state.History = context.GetUndoStack().GetHistory();
		state.Position = context.GetUndoStack().GetPosition();
		state.Modified = context.IsSceneModified();
		state.Project = context.HasProject() ? FileSystem::ToUTF8(context.GetProject()->GetProjectFile()) : std::string();
		return state;
	}

	// The commands that replace the edited scene.
	constexpr const char* c_DiscardingCommands[] = { "scene.open", "scene.new", "project.open", "project.create", "project.close", "project.openSample" };

	// One of them with parameters that work in a SafetyHarness rooted at `root`.
	struct DiscardingCommand
	{
		nlohmann::json Parameters;
		std::filesystem::path Created; // What the command makes on disk (it must not exist after a refusal)
	};

	DiscardingCommand MakeDiscardingCommand(std::string_view name, const std::filesystem::path& root)
	{
		if (name == "scene.open")
			return { { { "scene", "Scenes/Main.stscene" } }, {} };
		if (name == "scene.new")
			return { { { "name", "Fresh" } }, {} };
		if (name == "project.open")
			return { { { "path", FileSystem::ToUTF8(root / "Other") } }, {} };
		if (name == "project.create")
			return { { { "directory", FileSystem::ToUTF8(root / "Created") }, { "name", "Created" } }, root / "Created" };
		if (name == "project.openSample")
			return { { { "sample", "Tetris" }, { "directory", FileSystem::ToUTF8(root / "Tetris") } }, root / "Tetris" };
		return { nlohmann::json::object(), {} };
	}

	nlohmann::json WithForce(nlohmann::json parameters)
	{
		parameters["force"] = true;
		return parameters;
	}

	// Recovery files of a project marked discarded for a scene (SceneRecovery).
	size_t CountDiscardedRecoveries(const std::filesystem::path& project, const std::string& sceneName)
	{
		size_t count = 0;
		std::error_code error;
		for (std::filesystem::directory_iterator it(project / ".strata" / SceneRecovery::c_DirectoryName, error), end; !error && it != end; it.increment(error))
		{
			const std::optional<std::string> text = FileSystem::ReadText(it->path());
			const std::optional<nlohmann::json> header = text ? JsonUtils::Parse(std::string_view(*text).substr(0, text->find('\n'))) : std::nullopt;
			if (header && header->value("Discarded", false) && header->value("SceneName", "") == sceneName)
				count++;
		}
		return count;
	}

}

TEST_SUITE("Editor.DataSafety")
{
	TEST_CASE("Commands that replace a modified scene refuse without force, change nothing, and report a forced discard")
	{
		for (const std::string name : c_DiscardingCommands)
		{
			CAPTURE(name);
			SafetyHarness harness("DataSafety");
			const DiscardingCommand current = MakeDiscardingCommand(name, harness.Root);

			harness.Run("entity.create", { { "name", "Unsaved" } });
			REQUIRE(harness.Context.IsSceneModified());
			const EditorState before = CaptureState(harness.Context);

			const EditorCommandResult refused = harness.Execute(name, current.Parameters);
			CHECK_FALSE(refused.Success);
			CHECK(refused.ErrorKind == EditorCommandError::Failed);
			CHECK(refused.Error.find("'Main'") != std::string::npos);
			CHECK(refused.Error.find("scene.save") != std::string::npos);
			CHECK(refused.Error.find("force: true") != std::string::npos);
			CHECK(CaptureState(harness.Context) == before);
			if (!current.Created.empty())
				CHECK_FALSE(FileSystem::Exists(current.Created));

			const nlohmann::json forced = harness.Run(name, WithForce(current.Parameters));
			CHECK(forced["discardedChanges"] == true);
			CHECK_FALSE(harness.Context.GetEditScene()->FindEntityByName("Unsaved"));

			// Nothing is lost even then: the discarded changes are recovery data of the project they were made in.
			harness.Context.GetRecovery().Wait();
			CHECK(CountDiscardedRecoveries(harness.Root / "Game", "Main") == 1);
		}
	}

	TEST_CASE("Without unsaved changes the commands need no force and discard nothing")
	{
		for (const std::string name : c_DiscardingCommands)
		{
			CAPTURE(name);
			SafetyHarness harness("DataSafetyClean");
			CHECK(harness.Run(name, MakeDiscardingCommand(name, harness.Root).Parameters)["discardedChanges"] == false);
		}
	}

	TEST_CASE("The guard holds while a copy of the scene plays, and saving lifts it")
	{
		SafetyHarness harness("DataSafetyPlaying");
		harness.Run("entity.create", { { "name", "Unsaved" } });
		harness.Run("play.start");
		// The running copy is not what is at stake: the edited scene with its unsaved changes is.
		const EditorCommandResult refused = harness.Execute("scene.open", { { "scene", "Scenes/Main.stscene" } });
		CHECK_FALSE(refused.Success);
		CHECK(harness.Context.IsPlaying());
		CHECK(harness.Context.GetEditScene()->FindEntityByName("Unsaved"));

		harness.Run("play.stop");
		harness.Run("scene.save");
		CHECK(harness.Run("scene.open", { { "scene", "Scenes/Main.stscene" } })["discardedChanges"] == false);
		CHECK(harness.Context.GetEditScene()->FindEntityByName("Unsaved"));
	}

	TEST_CASE("A scene without a project can only be discarded with force")
	{
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		REQUIRE(commands.Execute(context, "entity.create", { { "name", "Unsaved" } }).Success);
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("DataSafetyNoProject") / "Game";
		const EditorCommandResult refused = commands.Execute(context, "project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });
		CHECK_FALSE(refused.Success);
		CHECK(refused.Error.find("without a project") != std::string::npos);
		CHECK_FALSE(FileSystem::Exists(directory));
		// project.close without a project replaces nothing.
		const EditorCommandResult close = commands.Execute(context, "project.close");
		REQUIRE(close.Success);
		CHECK(close.Value["closed"] == false);
		CHECK(context.GetEditScene()->FindEntityByName("Unsaved"));
	}

	TEST_CASE("The UI still asks about unsaved changes, and its answer discards them")
	{
		Tests::HarnessEditor editor({}, Tests::WithFeatureProject("DataSafetyUI"));
		editor.Frames(2);
		REQUIRE(editor.Context().HasProject());
		editor.Run("entity.create", { { "name", "Unsaved" } });
		REQUIRE(editor.Context().IsSceneModified());
		const std::string sceneName = editor.Context().GetEditScene()->GetName();

		// File > New Scene asks first; Cancel keeps everything.
		REQUIRE(editor.Click("Menu.File"));
		editor.Frames(1);
		REQUIRE(editor.Click("Menu.File.NewScene"));
		editor.Frames(2);
		REQUIRE(UI::ItemProbe::Find("UnsavedChanges.Cancel").has_value());
		CHECK(editor.Context().GetEditScene()->FindEntityByName("Unsaved"));
		REQUIRE(editor.Click("UnsavedChanges.Cancel"));
		editor.Frames(1);
		CHECK(editor.Context().GetEditScene()->GetName() == sceneName);
		CHECK(editor.Context().IsSceneModified());

		// Don't Save discards them: the command runs with force.
		REQUIRE(editor.Click("Menu.File"));
		editor.Frames(1);
		REQUIRE(editor.Click("Menu.File.NewScene"));
		editor.Frames(2);
		REQUIRE(editor.Click("UnsavedChanges.DontSave"));
		editor.Frames(1);
		CHECK(editor.Context().GetEditScene()->GetName() == "Untitled");
		CHECK_FALSE(editor.Context().GetEditScene()->FindEntityByName("Unsaved"));
		CHECK_FALSE(editor.Context().IsSceneModified());
	}

	TEST_CASE("Closing the project after a failed project file write asks about it, and Retry writes it first")
	{
		Tests::HarnessEditor editor;
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("DataSafetyUnwrittenSettings") / "Game";
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" }, { "template", "basic3d" } });
		REQUIRE(editor.FramesUntil([&editor]() { return editor.Context().GetProjectFileState() != SettingsFileState::Writing; }));
		const std::filesystem::path projectFile = editor.Context().GetProject()->GetProjectFile();
		// A directory in the project file's place makes its writes fail.
		REQUIRE(FileSystem::Remove(projectFile));
		REQUIRE(FileSystem::CreateDirectories(projectFile));
		editor.Run("project.setExportSettings", { { "alwaysInclude", { "Scenes" } } });
		REQUIRE(editor.FramesUntil([&editor]() { return editor.Context().HasUnwrittenProjectSettings(); }));
		REQUIRE_FALSE(editor.Context().IsSceneModified());

		// File > Close Project asks; Cancel keeps the project open.
		REQUIRE(editor.Click("Menu.File"));
		editor.Frames(1);
		REQUIRE(editor.Click("Menu.File.CloseProject"));
		editor.Frames(2);
		REQUIRE(UI::ItemProbe::Find("UnwrittenSettings.Dialog").has_value());
		REQUIRE(editor.Click("UnwrittenSettings.Cancel"));
		editor.Frames(2);
		CHECK_FALSE(UI::ItemProbe::Find("UnwrittenSettings.Dialog").has_value());
		CHECK(editor.Context().HasProject());

		// Retry writes the file once its cause is gone, and the project closes after it.
		REQUIRE(editor.Click("Menu.File"));
		editor.Frames(1);
		REQUIRE(editor.Click("Menu.File.CloseProject"));
		editor.Frames(2);
		REQUIRE(UI::ItemProbe::Find("UnwrittenSettings.Dialog").has_value());
		REQUIRE(FileSystem::Remove(projectFile));
		REQUIRE(editor.Click("UnwrittenSettings.Retry"));
		REQUIRE(editor.FramesUntil([&editor]() { return !editor.Context().HasProject(); }));
		std::string error;
		const Ref<Project> written = Project::Load(projectFile, &error);
		REQUIRE(written);
		CHECK(written->GetConfig().Export.AlwaysInclude == std::vector<std::string> { "Scenes" });
	}

	TEST_CASE("Scene changes made while the unwritten settings question waits are asked about before the project closes")
	{
		Tests::HarnessEditor editor;
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("DataSafetyUnwrittenMeanwhile") / "Game";
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" }, { "template", "basic3d" } });
		REQUIRE(editor.FramesUntil([&editor]() { return editor.Context().GetProjectFileState() != SettingsFileState::Writing; }));
		const std::filesystem::path projectFile = editor.Context().GetProject()->GetProjectFile();
		REQUIRE(FileSystem::Remove(projectFile));
		REQUIRE(FileSystem::CreateDirectories(projectFile));
		editor.Run("project.setExportSettings", { { "alwaysInclude", { "Scenes" } } });
		REQUIRE(editor.FramesUntil([&editor]() { return editor.Context().HasUnwrittenProjectSettings(); }));
		REQUIRE_FALSE(editor.Context().IsSceneModified());

		REQUIRE(editor.Click("Menu.File"));
		editor.Frames(1);
		REQUIRE(editor.Click("Menu.File.CloseProject"));
		editor.Frames(2);
		REQUIRE(UI::ItemProbe::Find("UnwrittenSettings.Dialog").has_value());
		// An agent edits the scene while the person reads the question.
		editor.Run("entity.create", { { "name", "Meanwhile" } });
		REQUIRE(editor.Context().IsSceneModified());

		// Don't Save answers for the settings: the scene's new changes are asked about, and Cancel keeps them and the project.
		REQUIRE(editor.Click("UnwrittenSettings.Discard"));
		editor.Frames(2);
		REQUIRE(UI::ItemProbe::Find("UnsavedChanges.Cancel").has_value());
		CHECK(editor.Context().HasProject());
		REQUIRE(editor.Click("UnsavedChanges.Cancel"));
		editor.Frames(2);
		CHECK(editor.Context().HasProject());
		CHECK(editor.Context().GetEditScene()->FindEntityByName("Meanwhile"));
		CHECK(editor.Context().HasUnwrittenProjectSettings());
	}

	TEST_CASE("File > Open Scene asks about unsaved changes, then opens a scene file of the asset directory")
	{
		Tests::HarnessEditor editor;
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("DataSafetyOpenScene") / "Game";
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" }, { "template", "basic3d" } });
		editor.Frames(2);
		const std::filesystem::path assets = editor.Context().GetProject()->GetAssetDirectory();
		const AssetHandle main = editor.Context().GetSceneHandle();
		REQUIRE(main.IsValid());
		editor.Run("scene.saveAs", { { "path", "Scenes/Other.stscene" } });
		REQUIRE(editor.Context().GetSceneHandle() != main);
		editor.Run("entity.create", { { "name", "Unsaved" } });

		// The menu offers it with a project; Ctrl+O is its shortcut.
		REQUIRE(editor.Click("Menu.File"));
		editor.Frames(1);
		const std::optional<UI::ItemProbe::Item> item = UI::ItemProbe::Find("Menu.File.OpenScene");
		REQUIRE(item.has_value());
		CHECK(item->Enabled);
		editor.Harness.SetKey(ImGuiKey_Escape, true);
		editor.Frames(1);
		editor.Harness.SetKey(ImGuiKey_Escape, false);
		editor.Frames(1);

		// A file outside the asset directory is refused without a question.
		CHECK_FALSE(editor.Layer->OpenSceneFile(directory.parent_path() / "Elsewhere.stscene"));
		editor.Frames(2);
		CHECK_FALSE(UI::ItemProbe::Find("UnsavedChanges.DontSave").has_value());

		// The chosen file opens once the person answered the question about the unsaved changes.
		REQUIRE(editor.Layer->OpenSceneFile(assets / "Scenes" / "Main.stscene"));
		editor.Frames(2);
		REQUIRE(editor.Click("UnsavedChanges.DontSave"));
		REQUIRE(editor.FramesUntil([&editor, main]() { return editor.Context().GetSceneHandle() == main && !editor.Context().IsSceneLoading(); }));
		CHECK_FALSE(editor.Context().GetEditScene()->FindEntityByName("Unsaved"));
		CHECK_FALSE(editor.Context().IsSceneModified());

		// Not while playing.
		editor.Run("play.start", { { "build", false } });
		REQUIRE(editor.Context().IsPlaying());
		CHECK_FALSE(editor.Layer->OpenSceneFile(assets / "Scenes" / "Other.stscene"));
		editor.Run("play.stop");
	}

	TEST_CASE("Don't Save asks again when an agent changed the scene while the question was open")
	{
		Tests::HarnessEditor editor({}, Tests::WithFeatureProject("DataSafetyAgentEdit"));
		editor.Frames(2);
		REQUIRE(editor.Context().HasProject());
		editor.Run("entity.create", { { "name", "Unsaved" } });
		const std::string sceneName = editor.Context().GetEditScene()->GetName();

		// The person picks File > New Scene; while the question waits, an agent adds an entity.
		REQUIRE(editor.Click("Menu.File"));
		editor.Frames(1);
		REQUIRE(editor.Click("Menu.File.NewScene"));
		editor.Frames(2);
		REQUIRE(UI::ItemProbe::Find("UnsavedChanges.DontSave").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("UnsavedChanges.Changed").has_value());
		editor.Run("entity.create", { { "name", "FromAgent" } });

		// Don't Save answered for the person's changes only: the question stays and says why.
		REQUIRE(editor.Click("UnsavedChanges.DontSave"));
		editor.Frames(2);
		CHECK(editor.Context().GetEditScene()->GetName() == sceneName);
		CHECK(editor.Context().GetEditScene()->FindEntityByName("FromAgent"));
		CHECK(editor.Context().GetEditScene()->FindEntityByName("Unsaved"));
		REQUIRE(UI::ItemProbe::Find("UnsavedChanges.DontSave").has_value());
		CHECK(UI::ItemProbe::Find("UnsavedChanges.Changed").has_value());

		// Asked again, with nothing changed since, Don't Save discards both.
		REQUIRE(editor.Click("UnsavedChanges.DontSave"));
		editor.Frames(2);
		CHECK(editor.Context().GetEditScene()->GetName() == "Untitled");
		CHECK_FALSE(editor.Context().GetEditScene()->FindEntityByName("FromAgent"));
		CHECK_FALSE(UI::ItemProbe::Find("UnsavedChanges.DontSave").has_value());
	}
}
