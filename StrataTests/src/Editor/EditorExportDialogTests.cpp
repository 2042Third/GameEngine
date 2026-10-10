#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Editor/GameExport.h"
#include "Editor/HarnessEditor.h"
#include "TestHelpers.h"
#include "UI/ItemProbe.h"

#include <Strata/Core/FileSystem.h>

#include <optional>
#include <string>
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
}
