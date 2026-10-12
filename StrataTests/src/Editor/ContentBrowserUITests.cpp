#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Editor/AssetBrowserView.h"
#include "Editor/HarnessEditor.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/SceneHierarchyPanel.h"
#include "TestHelpers.h"
#include "UI/DeleteAssetDialog.h"
#include "UI/ItemProbe.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Events/ApplicationEvent.h>
#include <Strata/Reflection/PropertyJson.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// An editor on a copy of the feature project with the Content Browser in front.
	struct BrowserEditor : HarnessEditor
	{
		explicit BrowserEditor(const std::string& name, const ImGuiHarness::Specification& specification = { ImVec2(1600.0f, 900.0f), 1.0f })
			: HarnessEditor(specification, WithFeatureProject(name))
		{
			Layer->GetPanels().Focus(EditorPanels::c_ContentBrowser);
			Frames(3);
		}

		ContentBrowserPanel& Browser()
		{
			ContentBrowserPanel* browser = Layer->GetPanels().Get<ContentBrowserPanel>(EditorPanels::c_ContentBrowser);
			REQUIRE(browser);
			return *browser;
		}

		EditorAssetManager& Assets()
		{
			EditorAssetManager* assets = Context().GetAssetManager();
			REQUIRE(assets);
			return *assets;
		}

		static ImVec2 Center(const ContentBrowserPanel::ItemRect& rect)
		{
			return ImVec2((rect.Min.x + rect.Max.x) * 0.5f, (rect.Min.y + rect.Max.y) * 0.5f);
		}

		ImVec2 ItemCenter(AssetHandle asset)
		{
			const std::optional<ContentBrowserPanel::ItemRect> rect = Browser().FindItemRect(asset);
			REQUIRE_MESSAGE(rect.has_value(), "The item of ", asset.ToString(), " was not drawn");
			return Center(*rect);
		}

		ImVec2 FolderCenter(const std::string& folder)
		{
			const std::optional<ContentBrowserPanel::ItemRect> rect = Browser().FindFolderRect(folder);
			REQUIRE_MESSAGE(rect.has_value(), "The folder '", folder, "' was not drawn");
			return Center(*rect);
		}

		void DoubleClickAt(const ImVec2& position)
		{
			Harness.MoveMouse(position);
			Frames(1);
			for (int click = 0; click < 2; click++)
			{
				Harness.SetMouseButton(ImGuiMouseButton_Left, true);
				Frames(1);
				Harness.SetMouseButton(ImGuiMouseButton_Left, false);
				Frames(1);
			}
			Frames(1);
		}

		void DragTo(const ImVec2& from, const ImVec2& to)
		{
			Harness.MoveMouse(from);
			Frames(1);
			Harness.SetMouseButton(ImGuiMouseButton_Left, true);
			Frames(1);
			for (int step = 1; step <= 4; step++)
			{
				const float t = static_cast<float>(step) / 4.0f;
				Harness.MoveMouse(ImVec2(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t));
				Frames(1);
			}
			Frames(1);
			Harness.SetMouseButton(ImGuiMouseButton_Left, false);
			Frames(2);
		}

		void PressKey(ImGuiKey key)
		{
			Harness.SetKey(key, true);
			Frames(1);
			Harness.SetKey(key, false);
			Frames(2);
		}
	};

}

TEST_SUITE("Editor.AssetBrowserView")
{
	TEST_CASE("The view lists folders first, then assets by name, and rebuilds only when something it shows changed")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("AssetBrowserView");
		REQUIRE(FileSystem::WriteBytes(directory / "Assets" / "Textures" / "brick.png", CreateSolidPNG(4, 4, 1, 1, 1)));
		REQUIRE(FileSystem::WriteBytes(directory / "Assets" / "Textures" / "Arch.png", CreateSolidPNG(4, 4, 2, 2, 2)));
		REQUIRE(FileSystem::WriteBytes(directory / "Assets" / "Textures" / "Walls" / "Stone.png", CreateSolidPNG(4, 4, 3, 3, 3)));
		REQUIRE(FileSystem::WriteBytes(directory / "Assets" / "Sounds" / "Beep.wav", CreateSineWav(0.1f)));
		REQUIRE(FileSystem::CreateDirectories(directory / "Assets" / "Empty"));
		EditorAssetManagerSpecification specification;
		specification.AssetDirectory = directory / "Assets";
		specification.CacheDirectory = directory / "Cache";
		specification.WatchFiles = false;
		const Ref<EditorAssetManager> assets = CreateRef<EditorAssetManager>(specification);
		assets->Scan();

		AssetBrowserView view;
		CHECK(view.Update(assets.get()));
		CHECK_FALSE(view.Update(assets.get())); // Nothing changed
		CHECK(view.GetRebuildCount() == 1);
		// The root: its folders.
		REQUIRE(view.GetItems().size() == 3);
		CHECK(view.GetItems()[0].Name == "Empty");
		CHECK(view.GetItems()[2].Name == "Textures");
		// The tree: the asset directory and, expanded by default, its folders.
		REQUIRE(view.GetFolderRows().size() == 4);
		CHECK(view.GetFolderRows()[0].Name == "Assets");
		CHECK(view.GetFolderRows()[3].Path == "Textures");
		CHECK(view.GetFolderRows()[3].HasChildren);
		CHECK_FALSE(view.GetFolderRows()[3].Expanded);

		view.SetFolder("Textures");
		view.SetFolderExpanded("Textures", true);
		CHECK(view.Update(assets.get()));
		REQUIRE(view.GetItems().size() == 3);
		CHECK(view.GetItems()[0].Kind == AssetBrowserItemKind::Folder);
		CHECK(view.GetItems()[1].Name == "Arch.png");
		CHECK(view.GetItems()[2].Name == "brick.png"); // By name, ignoring case
		CHECK(view.GetFolderRows().size() == 5);

		// A search lists matches below the folder; the type filter narrows them.
		view.SetSearch("st");
		CHECK(view.Update(assets.get()));
		REQUIRE(view.GetItems().size() == 1);
		CHECK(view.GetItems()[0].Path == "Textures/Walls/Stone.png");
		view.SetSearch({});
		view.SetFolder({});
		view.SetTypeFilter({ AssetType::AudioClip });
		view.SetSearch("e");
		CHECK(view.Update(assets.get()));
		REQUIRE(view.GetItems().size() == 1);
		CHECK(view.GetItems()[0].Type == AssetType::AudioClip);

		// A change of the manager rebuilds; a folder that went falls back to its parent.
		view.SetSearch({});
		view.SetTypeFilter({});
		view.SetFolder("Textures/Walls");
		CHECK(view.Update(assets.get()));
		const uint64_t rebuilds = view.GetRebuildCount();
		std::string error;
		TrashedAssets trashed;
		REQUIRE_FALSE(assets->MoveToTrash("Textures/Walls", trashed, &error)); // No trash directory: nothing changes
		CHECK_FALSE(view.Update(assets.get()));
		REQUIRE(assets->MoveFolder("Textures/Walls", "Walls", &error));
		CHECK(view.Update(assets.get()));
		CHECK(view.GetRebuildCount() == rebuilds + 1);
		CHECK(view.GetFolder() == "Textures");
		CHECK(view.FindItem(assets->FindAssetByPath("Textures/Arch.png")).has_value());
		CHECK_FALSE(view.FindItem(assets->FindAssetByPath("Walls/Stone.png")).has_value());
	}
}

TEST_SUITE("Editor.UI.ContentBrowser")
{
	TEST_CASE("The browser shows the folder tree and the folder's items, and an idle editor rebuilds nothing")
	{
		BrowserEditor editor("ContentBrowserIdle");
		ContentBrowserPanel& browser = editor.Browser();
		// The root's folders as tiles, and the tree with the asset directory and its folders.
		CHECK(browser.FindFolderRect("Materials").has_value());
		CHECK(browser.FindFolderRect("").has_value());
		const uint64_t rebuilds = browser.GetView().GetRebuildCount();
		editor.Frames(120);
		CHECK(browser.GetView().GetRebuildCount() == rebuilds);

		// A change of the project's assets rebuilds once.
		const AssetHandle material = *UUIDFromJson(editor.Run("material.create", { { "path", "Materials/New.stmat" } })["asset"]);
		browser.SetCurrentDirectory("Materials");
		editor.Frames(3);
		CHECK(browser.GetView().GetRebuildCount() <= rebuilds + 2);
		CHECK(browser.FindItemRect(material).has_value());
		const uint64_t after = browser.GetView().GetRebuildCount();
		editor.Frames(60);
		CHECK(browser.GetView().GetRebuildCount() == after);

		// The list shows the same items.
		browser.SetViewMode(ContentBrowserPanel::ViewMode::List);
		editor.Frames(2);
		CHECK(browser.FindItemRect(material).has_value());
	}

	TEST_CASE("selection.set {asset} reveals the asset: its folder opens and the grid shows it")
	{
		BrowserEditor editor("ContentBrowserReveal");
		ContentBrowserPanel& browser = editor.Browser();
		const AssetHandle crate = editor.Assets().FindAssetByPath("Prefabs/Crate.stprefab");
		REQUIRE(crate.IsValid());
		CHECK(browser.GetCurrentDirectory().empty());
		CHECK_FALSE(browser.FindItemRect(crate).has_value());

		editor.Run("selection.set", { { "asset", "Prefabs/Crate.stprefab" } });
		editor.Frames(3);
		CHECK(browser.GetCurrentDirectory() == "Prefabs");
		CHECK(browser.GetRevealedAsset() == crate);
		REQUIRE(browser.FindItemRect(crate).has_value());
		// The tree's row of the folder shows too.
		CHECK(browser.FindFolderRect("Prefabs").has_value());

		// A sub-asset shows inside its expanded model.
		const AssetHandle model = editor.Assets().FindAssetByPath("Models/Platform.gltf");
		const std::vector<AssetHandle> subAssets = editor.Assets().GetImportInfo(model).SubAssets;
		REQUIRE_FALSE(subAssets.empty());
		editor.Run("selection.set", { { "asset", UUIDToJson(subAssets.front()) } });
		editor.Frames(3);
		CHECK(browser.GetCurrentDirectory() == "Models");
		CHECK(browser.FindItemRect(subAssets.front()).has_value());
		CHECK(browser.FindItemRect(model).has_value());

		// With only textures shown, revealing a material turns the filter off, so the material shows; a filter that shows it stays.
		REQUIRE(editor.Click("ContentBrowser.Filter.Texture"));
		editor.Frames(2);
		REQUIRE(browser.GetView().GetTypeFilter() == std::set<AssetType> { AssetType::Texture });
		const AssetHandle material = editor.Assets().FindAssetByPath("Materials/Feature.stmat");
		editor.Run("selection.set", { { "asset", "Materials/Feature.stmat" } });
		editor.Frames(3);
		CHECK(browser.GetCurrentDirectory() == "Materials");
		CHECK(browser.GetView().GetTypeFilter().empty());
		CHECK(browser.FindItemRect(material).has_value());
		REQUIRE(editor.Click("ContentBrowser.Filter.Texture"));
		editor.Frames(2);
		editor.Run("selection.set", { { "asset", "Textures/Checker.png" } });
		editor.Frames(3);
		CHECK(browser.GetView().GetTypeFilter() == std::set<AssetType> { AssetType::Texture });
		CHECK(browser.FindItemRect(editor.Assets().FindAssetByPath("Textures/Checker.png")).has_value());
	}

	TEST_CASE("Double-clicking a folder opens it, and dragging an asset onto a folder moves it in one undo step")
	{
		BrowserEditor editor("ContentBrowserDrag");
		ContentBrowserPanel& browser = editor.Browser();
		editor.DoubleClickAt(editor.FolderCenter("Materials"));
		editor.Frames(2);
		CHECK(browser.GetCurrentDirectory() == "Materials");

		const AssetHandle material = editor.Assets().FindAssetByPath("Materials/Feature.stmat");
		REQUIRE(material.IsValid());
		editor.Run("asset.createFolder", { { "path", "Materials/Metals" } });
		editor.Frames(3);
		editor.DragTo(editor.ItemCenter(material), editor.FolderCenter("Materials/Metals"));
		editor.Frames(2);
		CHECK(editor.Assets().GetMetadata(material)->Path == "Materials/Metals/Feature.stmat");
		editor.Run("edit.undo");
		CHECK(editor.Assets().GetMetadata(material)->Path == "Materials/Feature.stmat");
	}

	TEST_CASE("Delete asks first, naming what uses the asset, and deletes into the trash")
	{
		BrowserEditor editor("ContentBrowserDelete");
		ContentBrowserPanel& browser = editor.Browser();
		const AssetHandle material = editor.Assets().FindAssetByPath("Materials/Feature.stmat");
		browser.SetCurrentDirectory("Materials");
		editor.Frames(3);
		editor.ClickAt(editor.ItemCenter(material));
		editor.PressKey(ImGuiKey_Delete);
		// The confirmation lists the scene that uses it; nothing is deleted yet.
		REQUIRE(UI::ItemProbe::Find("DeleteAsset.Confirm").has_value());
		CHECK(editor.Assets().IsHandleValid(material));
		ImGuiWindow* dialog = ImGui::FindWindowByName(DeleteAssetDialog::c_PopupName);
		REQUIRE(dialog);
		REQUIRE(editor.Click("DeleteAsset.Cancel"));
		editor.Frames(2);
		CHECK(editor.Assets().IsHandleValid(material));
		// Escape cancels as well.
		editor.ClickAt(editor.ItemCenter(material));
		editor.PressKey(ImGuiKey_Delete);
		REQUIRE(UI::ItemProbe::Find("DeleteAsset.Cancel").has_value());
		editor.PressKey(ImGuiKey_Escape);
		CHECK_FALSE(UI::ItemProbe::Find("DeleteAsset.Cancel").has_value());
		CHECK(editor.Assets().IsHandleValid(material));

		editor.ClickAt(editor.ItemCenter(material));
		editor.PressKey(ImGuiKey_Delete);
		REQUIRE(editor.Click("DeleteAsset.Confirm")); // "Delete Anyway": the feature scene uses it
		editor.Frames(2);
		CHECK_FALSE(editor.Assets().IsHandleValid(material));
		CHECK(editor.Run("edit.history")["undo"] == "Delete Materials/Feature.stmat");
		editor.Run("edit.undo");
		CHECK(editor.Assets().IsHandleValid(material));
	}

	TEST_CASE("Type chips filter the items, and a model's sub-assets expand as drag sources")
	{
		// Tall enough that the docked browser shows a whole row of tiles.
		BrowserEditor editor("ContentBrowserFilter", { ImVec2(1600.0f, 1400.0f), 1.0f });
		ContentBrowserPanel& browser = editor.Browser();
		const AssetHandle checker = editor.Assets().FindAssetByPath("Textures/Checker.png");
		const AssetHandle sky = editor.Assets().FindAssetByPath("Textures/Sky.hdr");
		browser.SetCurrentDirectory("Textures");
		editor.Frames(3);
		CHECK(browser.FindItemRect(checker).has_value());
		REQUIRE(editor.Click("ContentBrowser.Filter.Material"));
		editor.Frames(2);
		CHECK_FALSE(browser.FindItemRect(checker).has_value());
		REQUIRE(editor.Click("ContentBrowser.Filter.Material"));
		editor.Frames(2);
		CHECK(browser.FindItemRect(sky).has_value());

		// Animation controllers have a chip of their own; models (whose skeletons and clips are sub-assets) do not pass it.
		const AssetHandle controller = editor.Assets().FindAssetByPath("Animations/Arm.stanimator");
		const AssetHandle arm = editor.Assets().FindAssetByPath("Models/Arm.gltf");
		REQUIRE(editor.Click("ContentBrowser.Filter.AnimationController"));
		browser.SetCurrentDirectory("Animations");
		editor.Frames(3);
		CHECK(browser.FindItemRect(controller).has_value());
		browser.SetCurrentDirectory("Models");
		editor.Frames(3);
		CHECK_FALSE(browser.FindItemRect(arm).has_value());
		REQUIRE(editor.Click("ContentBrowser.Filter.AnimationController"));
		editor.Frames(2);
		CHECK(browser.FindItemRect(arm).has_value());

		// The model's expander shows its meshes and materials after it.
		const AssetHandle model = editor.Assets().FindAssetByPath("Models/Platform.gltf");
		browser.SetCurrentDirectory("Models");
		editor.Frames(3);
		const std::optional<ContentBrowserPanel::ItemRect> tile = browser.FindItemRect(model);
		REQUIRE(tile);
		const float frame = ImGui::GetFrameHeight();
		const float inner = ImGui::GetStyle().ItemInnerSpacing.x;
		editor.ClickAt(ImVec2(tile->Max.x - inner - frame * 0.5f, tile->Max.y - ImGui::GetTextLineHeight() - ImGui::GetStyle().ItemInnerSpacing.y * 2.0f - frame * 0.5f));
		editor.Frames(2);
		const std::vector<AssetHandle> subAssets = editor.Assets().GetImportInfo(model).SubAssets;
		REQUIRE_FALSE(subAssets.empty());
		CHECK(std::any_of(subAssets.begin(), subAssets.end(), [&](AssetHandle subAsset) { return browser.FindItemRect(subAsset).has_value(); }));
	}

	TEST_CASE("Dragging entities from the Hierarchy onto a folder makes them a prefab there, linked to it")
	{
		BrowserEditor editor("ContentBrowserPrefabDrop");
		const std::string lamp = editor.Run("entity.create", { { "name", "Lamp" } })["id"].get<std::string>();
		editor.Run("entity.create", { { "name", "Bulb" }, { "parent", lamp } });
		// Selected elsewhere, the lamp is scrolled into the Hierarchy's view.
		editor.Run("selection.set", { { "entities", { lamp } } });
		editor.Frames(3);
		SceneHierarchyPanel* hierarchy = editor.Layer->GetPanels().Get<SceneHierarchyPanel>(EditorPanels::c_Hierarchy);
		REQUIRE(hierarchy);
		const std::optional<SceneHierarchyPanel::RowRect> row = hierarchy->FindRowRect(*UUIDFromJson(lamp));
		REQUIRE_MESSAGE(row.has_value(), "The Hierarchy shows beside the Content Browser in the default layout");
		const size_t historyBefore = editor.Run("edit.history")["history"].size();

		editor.DragTo(ImVec2((row->Min.x + row->Max.x) * 0.5f, (row->Min.y + row->Max.y) * 0.5f), editor.FolderCenter("Materials"));
		editor.Frames(2);
		const AssetHandle prefab = editor.Assets().FindAssetByPath("Materials/Lamp.stprefab");
		REQUIRE(prefab.IsValid());
		const nlohmann::json status = editor.Run("prefab.status", { { "entity", lamp } });
		CHECK(status["linked"] == true);
		CHECK(status["prefab"] == UUIDToJson(prefab));
		CHECK(editor.Run("edit.history")["history"].size() == historyBefore + 1);
		editor.Run("edit.undo");
		CHECK_FALSE(editor.Assets().FindAssetByPath("Materials/Lamp.stprefab").IsValid());
		CHECK(editor.Run("prefab.status", { { "entity", lamp } })["linked"] == false);
	}

	TEST_CASE("A short browser lowers its tiles' pictures so that a whole tile with its name shows, keeping the names' width")
	{
		// The default layout in a window of laptop proportions (1600x900 at any scale) leaves the docked browser too short for
		// tiles of the slider's size: their pictures get lower, while the tiles keep the slider's width for their names.

		// The checker's tile, the name roughness's tile shows and the bottom of the items' pane in view, for a window of this
		// height.
		struct Shown
		{
			ContentBrowserPanel::ItemRect Tile;
			std::string Title;
			float PaneBottom = 0.0f;

			float GetWidth() const { return Tile.Max.x - Tile.Min.x; }
			float GetHeight() const { return Tile.Max.y - Tile.Min.y; }
		};
		auto show = [](float height)
		{
			BrowserEditor editor(fmt::format("ContentBrowserTiles{}", height), { ImVec2(1600.0f, height), 1.0f });
			editor.Browser().SetCurrentDirectory("Textures");
			editor.Frames(3);
			const std::optional<ContentBrowserPanel::ItemRect> tile = editor.Browser().FindItemRect(editor.Assets().FindAssetByPath("Textures/Checker.png"));
			REQUIRE(tile);
			const ImGuiWindow* pane = nullptr;
			for (const ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
			{
				if (std::string_view(window->Name).find("ContentBrowser.Items") != std::string_view::npos)
					pane = window;
			}
			REQUIRE(pane);
			return Shown { *tile, editor.Browser().GetShownTileTitle(editor.Assets().FindAssetByPath("Textures/Roughness.png")), pane->InnerRect.Max.y };
		};

		// Where there is room, the slider's size; where there is not, as wide but lower, and a nine-letter name whole.
		const Shown desktop = show(1400.0f);
		CHECK(desktop.Tile.Max.y <= desktop.PaneBottom + 0.5f);
		CHECK(desktop.Title == "Roughness");
		const Shown laptop = show(900.0f);
		CHECK(laptop.Tile.Max.y <= laptop.PaneBottom + 0.5f);
		CHECK(laptop.GetWidth() == doctest::Approx(desktop.GetWidth()));
		CHECK(laptop.GetHeight() < desktop.GetHeight());
		CHECK(laptop.Title == "Roughness");
	}

	TEST_CASE("The arrow keys move the selection over the items, and F2 and Delete act on a folder selected in the tree")
	{
		BrowserEditor editor("ContentBrowserKeyboard", { ImVec2(1600.0f, 1400.0f), 1.0f });
		ContentBrowserPanel& browser = editor.Browser();
		browser.SetCurrentDirectory("Textures");
		editor.Frames(3);
		const AssetHandle bumps = editor.Assets().FindAssetByPath("Textures/Bumps_normal.png");
		const AssetHandle checker = editor.Assets().FindAssetByPath("Textures/Checker.png");
		REQUIRE(editor.Browser().GetView().FindItem(bumps) == 0);
		REQUIRE(editor.Browser().GetView().FindItem(checker) == 1);
		editor.ClickAt(editor.ItemCenter(bumps));
		CHECK(browser.GetSelectedAsset() == bumps);
		CHECK(editor.Context().GetInspectedAsset() == bumps);
		// The keyboard moves the selection: Enter then shows the item the keyboard is at.
		editor.PressKey(ImGuiKey_RightArrow);
		CHECK(browser.GetSelectedAsset() == checker);
		CHECK(editor.Context().GetInspectedAsset() == checker);

		// A folder clicked in the tree is selected: Delete asks about it.
		editor.ClickAt(editor.FolderCenter("Materials"));
		CHECK(browser.GetCurrentDirectory() == "Materials");
		CHECK(browser.GetSelectedFolder() == "Materials");
		editor.PressKey(ImGuiKey_Delete);
		CHECK(UI::ItemProbe::Find("DeleteAsset.Cancel").has_value());
		editor.PressKey(ImGuiKey_Escape);
		CHECK(editor.Assets().HasFolder("Materials"));
	}

	TEST_CASE("New > Material makes a material in the open folder, selects it and starts its rename")
	{
		BrowserEditor editor("ContentBrowserNewMaterial");
		ContentBrowserPanel& browser = editor.Browser();
		browser.SetCurrentDirectory("Materials");
		editor.Frames(3);
		REQUIRE(editor.Click("ContentBrowser.New"));
		editor.Frames(1);
		REQUIRE(editor.Click("ContentBrowser.New.Material"));
		editor.Frames(3);
		const AssetHandle created = editor.Assets().FindAssetByPath("Materials/Material.stmat");
		REQUIRE(created.IsValid());
		CHECK(browser.GetSelectedAsset() == created);
		CHECK(editor.Context().GetInspectedAsset() == created);
		CHECK(browser.FindItemRect(created).has_value());
		// The rename field has the keyboard: typing names it.
		REQUIRE(ImGui::GetIO().WantTextInput);
		editor.Harness.SetKey(ImGuiMod_Ctrl, true);
		editor.Harness.SetKey(ImGuiKey_A, true);
		editor.Frames(1);
		editor.Harness.SetKey(ImGuiKey_A, false);
		editor.Harness.SetKey(ImGuiMod_Ctrl, false);
		editor.Frames(1);
		editor.Harness.TypeText("Brick.stmat");
		editor.Frames(1);
		editor.PressKey(ImGuiKey_Enter);
		CHECK(editor.Assets().GetMetadata(created)->Path == "Materials/Brick.stmat");
	}

	TEST_CASE("Breadcrumb links sit on the line of the current folder's name, and New and Import end where the view buttons do")
	{
		BrowserEditor editor("ContentBrowserToolbar");
		editor.Browser().SetCurrentDirectory("Textures");
		editor.Frames(3);
		const std::optional<UI::ItemProbe::Item> link = UI::ItemProbe::Find("ContentBrowser.Crumb0");
		const std::optional<UI::ItemProbe::Item> current = UI::ItemProbe::Find("ContentBrowser.Crumb1");
		REQUIRE(link);
		REQUIRE(current);
		CHECK(link->Max.y == doctest::Approx(current->Max.y));
		const std::optional<UI::ItemProbe::Item> import = UI::ItemProbe::Find("ContentBrowser.Import");
		const std::optional<UI::ItemProbe::Item> list = UI::ItemProbe::Find("ContentBrowser.List");
		REQUIRE(import);
		REQUIRE(list);
		CHECK(import->Max.x == doctest::Approx(list->Max.x).epsilon(0.001));
	}

	TEST_CASE("Files and folders dropped on the editor import into the open folder; files of no asset type are named and left out")
	{
		ScopedJobSystem jobs(2, 2);
		BrowserEditor editor("ContentBrowserFileDrop");
		editor.Browser().SetCurrentDirectory("Textures");
		editor.Frames(2);
		const std::filesystem::path dropped = CreateTemporaryDirectory("ContentBrowserFileDropFiles");
		REQUIRE(FileSystem::WriteText(dropped / "Notes.txt", "Not an asset"));
		REQUIRE(FileSystem::WriteBytes(dropped / "Dropped.png", CreateSolidPNG(4, 4, 10, 200, 30)));
		REQUIRE(FileSystem::WriteBytes(dropped / "Pack" / "Inner.png", CreateSolidPNG(4, 4, 200, 10, 30)));

		// A file no importer reads would make asset.import refuse the whole drop: it is left out, and the log says so.
		const uint64_t sequence = Log::GetBuffer().GetLatestSequence();
		WindowFileDropEvent drop({ dropped / "Notes.txt", dropped / "Dropped.png", dropped / "Pack" });
		editor.Harness.DispatchEvent(drop, *editor.Layer);
		REQUIRE(editor.FramesUntil([&]()
		{
			return editor.Assets().FindAssetByPath("Textures/Dropped.png").IsValid() && editor.Assets().FindAssetByPath("Textures/Pack/Inner.png").IsValid()
				&& editor.Assets().GetExternalImportCount() == 0;
		}));
		CHECK(editor.Assets().GetImportInfo(editor.Assets().FindAssetByPath("Textures/Dropped.png")).Error.empty());
		CHECK_FALSE(FileSystem::Exists(editor.Assets().GetAssetDirectory() / "Textures" / "Notes.txt"));
		bool named = false;
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(sequence))
			named = named || (entry.Message.find("Not imported") != std::string::npos && entry.Message.find("Notes.txt") != std::string::npos);
		CHECK(named);
	}
}
