#include <doctest/doctest.h>

#include "Editor/AssetBrowserView.h"
#include "Editor/HarnessEditor.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/SceneHierarchyPanel.h"
#include "TestHelpers.h"
#include "UI/DeleteAssetDialog.h"
#include "UI/ItemProbe.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Reflection/PropertyJson.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <string>
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
}
