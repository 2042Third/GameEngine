#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Editor/GameExport.h"
#include "Editor/HarnessEditor.h"
#include "TestHelpers.h"
#include "UI/ItemProbe.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/Log.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

TEST_SUITE("Editor.ExportDialog")
{
	TEST_CASE("The Export dialog exports the open project, showing its progress, and leads to the folder")
	{
		ScopedJobSystem jobs(8, 2);
		HarnessEditor editor;
		const std::filesystem::path directory = CreateTemporaryDirectory("EditorExportDialog");
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(directory / "Game") }, { "name", "Dialog Game" } });
		editor.Run("entity.create", { { "name", "Box" }, { "components", { { "MeshRenderer", { { "Mesh", "0000000000000001" } } } } } });
		editor.Run("scene.saveAs", { { "path", "Scenes/Main.stscene" } });
		// A large file, so that the export takes frames.
		std::vector<uint8_t> noise(48u << 20);
		uint32_t state = 777;
		for (uint8_t& byte : noise)
		{
			state = state * 1664525u + 1013904223u;
			byte = static_cast<uint8_t>(state >> 24);
		}
		REQUIRE(FileSystem::WriteBytes(directory / "Game" / "Assets" / "Large.ttf", noise));
		editor.Run("project.open", { { "path", FileSystem::ToUTF8(directory / "Game") } });
		editor.Run("project.setStartScene", { { "scene", "Scenes/Main.stscene" } });
		editor.Frames(3);

		// The toolbar's Export button opens the dialog, which suggests a directory next to the project.
		REQUIRE(editor.Click("Toolbar.Export"));
		editor.Frames(2);
		REQUIRE(editor.Layer->GetExportDialog().IsOpen());
		REQUIRE(UI::ItemProbe::Find("Export.Directory").has_value());
		const std::filesystem::path output = directory / "Exported";
		REQUIRE(editor.ReplaceText("Export.Directory", FileSystem::ToUTF8(output)));
		REQUIRE(editor.Click("Export.IncludeRuntime")); // The test executable has no runtime beside it to copy
		REQUIRE(editor.Click("Export.Export"));
		CHECK(editor.Layer->GetExportDialog().IsExporting());

		// While it runs the editor keeps drawing frames, the dialog its progress and the toolbar the share done.
		std::vector<std::string> labels;
		REQUIRE(editor.FramesUntil([&]()
		{
			if (const std::optional<UI::ItemProbe::Item> bar = UI::ItemProbe::Find("Export.Progress"))
				labels.push_back(bar->Text);
			return UI::ItemProbe::Find("Export.OpenFolder").has_value();
		}));
		REQUIRE_FALSE(labels.empty());
		CHECK(labels.front().find('%') != std::string::npos);
		CHECK(labels.back() == "Done");
		CHECK_FALSE(editor.Layer->GetExportDialog().IsExporting());
		CHECK(FileSystem::IsRegularFile(output / "Dialog Game.stpak"));
		CHECK(FileSystem::IsRegularFile(output / "Dialog Game.stgame"));
		CHECK_FALSE(FileSystem::Exists(output / GetExecutableFileName("Dialog Game")));

		REQUIRE(editor.Click("Export.Close"));
		editor.Frames(2);
		CHECK_FALSE(editor.Layer->GetExportDialog().IsOpen());
	}

	TEST_CASE("The Export dialog's fields start as project.export's defaults")
	{
		HarnessEditor editor;
		const std::filesystem::path directory = CreateTemporaryDirectory("EditorExportDialogDefaults");
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(directory / "Game") }, { "name", "Defaults Game" } });
		editor.Frames(2);
		REQUIRE(editor.Click("Toolbar.Export"));
		editor.Frames(2);
		REQUIRE(editor.Layer->GetExportDialog().IsOpen());

		// What the dialog sends unless changed is what the command does without the parameters, e.g. no script symbols in
		// Dist editors.
		const GameExportOptions defaults;
		auto shown = [](std::string_view key)
		{
			const std::optional<UI::ItemProbe::Item> item = UI::ItemProbe::Find(key);
			REQUIRE_MESSAGE(item.has_value(), "No ", std::string(key), " in the last frame");
			return item->Text;
		};
		auto checked = [](bool value) { return std::string(value ? "Checked" : "Unchecked"); };
		CHECK(shown("Export.IncludeScriptSymbols") == checked(defaults.IncludeScriptSymbols));
		CHECK(shown("Export.IncludeRuntime") == checked(defaults.IncludeRuntime));
		CHECK(shown("Export.Fullscreen") == checked(defaults.Fullscreen));
		CHECK(shown("Export.Prune") == checked(defaults.Prune));
		CHECK(shown("Export.Width") == std::to_string(defaults.WindowWidth));
		CHECK(shown("Export.Height") == std::to_string(defaults.WindowHeight));
	}

	TEST_CASE("The Export dialog shows an export a command started and cancels it")
	{
		// One I/O thread, held busy until the export was cancelled: the export cannot end before.
		ScopedJobSystem jobs(2, 1);
		HarnessEditor editor;
		const std::filesystem::path directory = CreateTemporaryDirectory("EditorExportDialogWatch");
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(directory / "Game") }, { "name", "Watched Game" } });
		editor.Run("entity.create", { { "name", "Box" }, { "components", { { "MeshRenderer", { { "Mesh", "0000000000000001" } } } } } });
		editor.Run("scene.saveAs", { { "path", "Scenes/Main.stscene" } });
		editor.Run("project.setStartScene", { { "scene", "Scenes/Main.stscene" } });
		editor.Frames(3);

		std::atomic<bool> release = false;
		std::atomic<bool> holding = false;
		const JobHandle blocker = JobSystem::SubmitIO([&]()
		{
			holding = true;
			while (!release)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		});
		while (!holding)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));

		// An agent exports (automation runs commands through the runner, like command scripts).
		std::vector<EditorCommandResult> results;
		auto exportTo = [&](const char* name)
		{
			return editor.Layer->GetCommandRunner().Run(editor.Context(), editor.Layer->GetCommands(), "project.export",
				{ { "directory", FileSystem::ToUTF8(directory / name) }, { "includeRuntime", false } },
				[&results](const EditorCommandResult& result) { results.push_back(result); });
		};
		REQUIRE(exportTo("Cancelled"));

		// The toolbar's button shows the export running: clicking it shows its progress, not a form to start another.
		REQUIRE(editor.Click("Toolbar.Export"));
		editor.Frames(1);
		REQUIRE(editor.Layer->GetExportDialog().IsOpen());
		CHECK(UI::ItemProbe::Find("Export.Progress").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Export.Directory").has_value());
		CHECK_FALSE(editor.Layer->GetExportDialog().IsExporting()); // The dialog did not start it

		// Cancel Export stops it: the agent's command fails as cancelled, and the dialog says how it ended.
		const uint64_t beforeCancel = Log::GetBuffer().GetLatestSequence();
		REQUIRE(editor.Click("Export.Cancel"));
		release = true;
		blocker.Wait();
		REQUIRE(editor.FramesUntil([&]() { return !results.empty(); }));
		CHECK(results[0].ErrorKind == EditorCommandError::Cancelled);
		editor.Frames(1);
		CHECK(UI::ItemProbe::Find("Export.Back").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Export.Progress").has_value());
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(beforeCancel))
			CHECK_MESSAGE(entry.Message.find("project.cancelExport") == std::string::npos, entry.Message); // The command succeeded

		// An export an agent starts while the dialog is open is followed to its end.
		REQUIRE(exportTo("Exported"));
		std::vector<std::string> labels;
		REQUIRE(editor.FramesUntil([&]()
		{
			if (const std::optional<UI::ItemProbe::Item> bar = UI::ItemProbe::Find("Export.Progress"))
				labels.push_back(bar->Text);
			return UI::ItemProbe::Find("Export.OpenFolder").has_value();
		}));
		REQUIRE(results.size() == 2);
		CHECK(results[1].Success);
		REQUIRE_FALSE(labels.empty());
		CHECK(labels.back() == "Done");
		CHECK(FileSystem::IsRegularFile(directory / "Exported" / "Watched Game.stpak"));
		REQUIRE(editor.Click("Export.Close"));
		editor.Frames(2);
		CHECK_FALSE(editor.Layer->GetExportDialog().IsOpen());
	}

	TEST_CASE("The Export dialog checks its directory when the text changes or it opens and not every frame")
	{
		HarnessEditor editor;
		const std::filesystem::path directory = CreateTemporaryDirectory("EditorExportDialogCheck");
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(directory / "Game") }, { "name", "Checked Game" } });
		editor.Frames(2);
		REQUIRE(editor.Click("Toolbar.Export"));
		editor.Frames(2);
		auto exportEnabled = []()
		{
			const std::optional<UI::ItemProbe::Item> button = UI::ItemProbe::Find("Export.Export");
			REQUIRE(button.has_value());
			return button->Enabled;
		};
		const std::filesystem::path output = directory / "Output";
		REQUIRE(editor.ReplaceText("Export.Directory", FileSystem::ToUTF8(output)));
		editor.Frames(1);
		CHECK(exportEnabled());

		// A file appears where the game would go. The check asks the file system, which can block on a drive that went
		// away, so it does not run again for the same text in later frames.
		REQUIRE(FileSystem::WriteText(output, "not a directory"));
		editor.Frames(3);
		CHECK(exportEnabled());

		// It runs again when the dialog opens again,
		REQUIRE(editor.Click("Export.Cancel"));
		editor.Frames(2);
		REQUIRE_FALSE(editor.Layer->GetExportDialog().IsOpen());
		REQUIRE(editor.Click("Toolbar.Export"));
		editor.Frames(2);
		CHECK_FALSE(exportEnabled());

		// and when the text changes.
		REQUIRE(editor.ReplaceText("Export.Directory", FileSystem::ToUTF8(directory / "Elsewhere")));
		editor.Frames(1);
		CHECK(exportEnabled());
	}
}
