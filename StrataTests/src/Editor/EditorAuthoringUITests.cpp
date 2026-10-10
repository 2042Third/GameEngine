#include <doctest/doctest.h>

#include "Editor/HarnessEditor.h"
#include "Editor/ImGuiHarness.h"
#include "EditorLayer.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/SceneHierarchyPanel.h"
#include "TestHelpers.h"
#include "UI/EditorActions.h"
#include "UI/EntityIcons.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/PropertyWidgets.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/Material.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

// The authoring UI through the harness: the Hierarchy (rows, selection, rename, drops, filters), the Inspector (axis labels,
// component menus, several entities, scene settings), the Asset inspector (materials, import settings) and the shortcuts
// that act on the selection only where the keyboard focus is.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// An editor without a project, past the launcher.
	struct AuthoringEditor : HarnessEditor
	{
		explicit AuthoringEditor(const ImGuiHarness::Specification& specification = {}, EditorOptions options = {})
			: HarnessEditor(specification, std::move(options))
		{
			if (!Context().HasProject())
				Layer->DismissLauncher();
			Frames(2);
		}

		UUID Create(const std::string& name, const nlohmann::json& extra = nlohmann::json::object())
		{
			nlohmann::json parameters = extra;
			parameters["name"] = name;
			return *UUIDFromJson(Run("entity.create", parameters)["id"]);
		}

		Entity Get(UUID id) { return Context().GetActiveScene()->GetEntityByUUID(id); }
		size_t UndoPosition() { return Context().GetUndoStack().GetPosition(); }

		SceneHierarchyPanel& Hierarchy()
		{
			SceneHierarchyPanel* panel = Layer->GetPanels().Get<SceneHierarchyPanel>(EditorPanels::c_Hierarchy);
			REQUIRE(panel);
			return *panel;
		}

		SceneHierarchyPanel::RowRect Row(UUID id)
		{
			const std::optional<SceneHierarchyPanel::RowRect> rect = Hierarchy().FindRowRect(id);
			REQUIRE_MESSAGE(rect.has_value(), "The row of ", id.ToString(), " was not drawn");
			return *rect;
		}

		static ImVec2 Center(const SceneHierarchyPanel::RowRect& rect) { return ImVec2((rect.Min.x + rect.Max.x) * 0.5f, (rect.Min.y + rect.Max.y) * 0.5f); }

		// Frames past ImGui's double-click time, so the next click is a click of its own.
		void Pause() { Frames(30); }

		void ClickWith(const ImVec2& position, ImGuiKey modifier)
		{
			// The modifier in a frame of its own: ImGui applies input queued after a key change a frame later.
			if (modifier != ImGuiKey_None)
			{
				Harness.SetKey(modifier, true);
				Frames(1);
			}
			ClickAt(position);
			if (modifier != ImGuiKey_None)
				Harness.SetKey(modifier, false);
			Frames(1);
		}

		void ClickRow(UUID id, ImGuiKey modifier = ImGuiKey_None)
		{
			ClickWith(Center(Row(id)), modifier);
			Pause();
		}

		// A key pressed and released (with a modifier held around it).
		void Press(ImGuiKey key, ImGuiKey modifier = ImGuiKey_None)
		{
			if (modifier != ImGuiKey_None)
			{
				Harness.SetKey(modifier, true);
				Frames(1);
			}
			Harness.SetKey(key, true);
			Frames(1);
			Harness.SetKey(key, false);
			Frames(1);
			if (modifier != ImGuiKey_None)
			{
				Harness.SetKey(modifier, false);
				Frames(1);
			}
		}

		// Presses the left button at `from`, moves to `to` over a few frames (past the drag threshold) and releases.
		void Drag(const ImVec2& from, const ImVec2& to)
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

		void FocusPanel(const char* id)
		{
			Layer->GetPanels().Focus(id);
			Frames(2);
		}

		ImVec2 ProbeCenter(std::string_view key)
		{
			const std::optional<UI::ItemProbe::Item> item = UI::ItemProbe::Find(key);
			REQUIRE_MESSAGE(item.has_value(), "No ", std::string(key), " in the last frame");
			return item->GetCenter();
		}

		std::vector<std::string> RootNames()
		{
			std::vector<std::string> names;
			for (const UUID id : Context().GetActiveScene()->GetRootEntities())
				names.push_back(Get(id).GetName());
			return names;
		}
	};

}

TEST_SUITE("Editor.UI.Hierarchy")
{
	TEST_CASE("Rows select with clicks; Ctrl toggles and Shift selects a range")
	{
		AuthoringEditor editor;
		std::vector<UUID> ids;
		for (int index = 0; index < 6; index++)
			ids.push_back(editor.Create(fmt::format("Row {}", index)));
		editor.Frames(2);
		EditorContext& context = editor.Context();

		editor.ClickRow(ids[1]);
		CHECK(context.GetSelection() == std::vector<UUID> { ids[1] });
		editor.ClickRow(ids[3], ImGuiMod_Ctrl);
		CHECK(context.GetSelection() == std::vector<UUID> { ids[1], ids[3] });
		CHECK(context.GetPrimarySelection().GetUUID() == ids[3]);
		editor.ClickRow(ids[1], ImGuiMod_Ctrl);
		CHECK(context.GetSelection() == std::vector<UUID> { ids[3] });

		editor.ClickRow(ids[0]);
		editor.ClickRow(ids[4], ImGuiMod_Shift);
		std::vector<UUID> range = context.GetSelection();
		CHECK(range.size() == 5);
		for (int index = 0; index <= 4; index++)
			CHECK(context.IsSelected(ids[static_cast<size_t>(index)]));
		// The row clicked last is the primary selection.
		CHECK(context.GetPrimarySelection().GetUUID() == ids[4]);

		// A click on the space below the rows clears the selection.
		const SceneHierarchyPanel::RowRect last = editor.Row(ids[5]);
		editor.ClickWith(ImVec2((last.Min.x + last.Max.x) * 0.5f, last.Max.y + ImGui::GetFrameHeight() * 3.0f), ImGuiKey_None);
		CHECK(context.GetSelection().empty());
	}

	TEST_CASE("A Shift+click range starts at the entity clicked first, also after rows appeared above it")
	{
		AuthoringEditor editor;
		const UUID parent = editor.Create("Parent");
		for (int index = 0; index < 3; index++)
			editor.Create(fmt::format("Child {}", index), { { "parent", UUIDToJson(parent) } });
		std::vector<UUID> rows;
		for (int index = 0; index < 5; index++)
			rows.push_back(editor.Create(fmt::format("Row {}", index)));
		editor.Frames(2);
		REQUIRE_FALSE(editor.Hierarchy().GetView().IsExpanded(parent));
		EditorContext& context = editor.Context();

		editor.ClickRow(rows[1]);
		REQUIRE(context.GetSelection() == std::vector<UUID> { rows[1] });
		// Opened with its arrow: three rows appear above the clicked one, and the click on the arrow selects nothing.
		const SceneHierarchyPanel::RowRect parentRow = editor.Row(parent);
		editor.ClickWith(ImVec2(parentRow.Min.x + ImGui::GetStyle().FramePadding.x + ImGui::GetFontSize() * 0.5f, AuthoringEditor::Center(parentRow).y), ImGuiKey_None);
		editor.Pause();
		REQUIRE(editor.Hierarchy().GetView().IsExpanded(parent));
		REQUIRE(context.GetSelection() == std::vector<UUID> { rows[1] });

		// The range runs from the entity clicked first to the one clicked now, whatever rows they are on now.
		editor.ClickRow(rows[3], ImGuiMod_Shift);
		CHECK(context.GetSelection().size() == 3);
		for (int index = 1; index <= 3; index++)
			CHECK(context.IsSelected(rows[static_cast<size_t>(index)]));
		CHECK(context.GetPrimarySelection().GetUUID() == rows[3]);
	}

	TEST_CASE("Inline rename commits on Enter and cancels on Escape")
	{
		AuthoringEditor editor;
		const UUID id = editor.Create("Original");
		editor.Frames(2);
		editor.ClickRow(id);
		REQUIRE(editor.Context().IsSelected(id));
		const size_t position = editor.UndoPosition();

		// F2 (the panel has the focus after the click): a text field over the row, holding the name selected.
		editor.Press(ImGuiKey_F2);
		editor.Frames(1);
		CHECK(editor.Hierarchy().IsRenaming());
		REQUIRE(UI::ItemProbe::Find("Hierarchy.Rename").has_value());
		CHECK(ImGui::GetIO().WantTextInput);
		editor.Harness.TypeText("Renamed");
		editor.Frames(1);
		editor.Press(ImGuiKey_Enter);
		CHECK_FALSE(editor.Hierarchy().IsRenaming());
		CHECK(editor.Get(id).GetName() == "Renamed");
		CHECK(editor.UndoPosition() == position + 1);
		CHECK(editor.Context().GetUndoStack().GetUndoName() == "Rename Entity");

		// Escape keeps the name.
		CHECK(editor.Context().IsSelected(id));
		editor.Press(ImGuiKey_F2);
		REQUIRE(editor.Hierarchy().IsRenaming());
		editor.Harness.TypeText("Discarded");
		editor.Frames(1);
		editor.Press(ImGuiKey_Escape);
		CHECK_FALSE(editor.Hierarchy().IsRenaming());
		CHECK(editor.Get(id).GetName() == "Renamed");
		CHECK(editor.UndoPosition() == position + 1);
		// Escape ended the rename, not the selection.
		CHECK(editor.Context().IsSelected(id));

		// A double click on the name renames too.
		editor.Pause();
		const ImVec2 center = AuthoringEditor::Center(editor.Row(id));
		editor.ClickAt(center);
		editor.ClickAt(center);
		editor.Frames(1);
		CHECK(editor.Hierarchy().IsRenaming());
		editor.Press(ImGuiKey_Escape);
		CHECK_FALSE(editor.Hierarchy().IsRenaming());
	}

	TEST_CASE("Delete deletes the selection only while the Hierarchy or the Viewport has the keyboard focus")
	{
		AuthoringEditor editor;
		const UUID first = editor.Create("First");
		const UUID second = editor.Create("Second");
		editor.Frames(2);
		editor.ClickRow(first);
		REQUIRE(editor.Context().IsSelected(first));

		// The Content Browser has the focus: Delete is not the scene's.
		editor.FocusPanel(EditorPanels::c_ContentBrowser);
		editor.Press(ImGuiKey_Delete);
		CHECK(editor.Get(first));
		// Nor the Inspector's.
		editor.FocusPanel(EditorPanels::c_Inspector);
		editor.Press(ImGuiKey_Delete);
		CHECK(editor.Get(first));

		// The Hierarchy has it: Delete deletes, in one undo step.
		const size_t position = editor.UndoPosition();
		editor.FocusPanel(EditorPanels::c_Hierarchy);
		editor.Press(ImGuiKey_Delete);
		CHECK_FALSE(editor.Get(first));
		CHECK(editor.UndoPosition() == position + 1);

		// The Viewport has it: Ctrl+D duplicates, Delete deletes.
		editor.Context().Select(second);
		editor.FocusPanel(EditorPanels::c_Viewport);
		const size_t entities = editor.Context().GetActiveScene()->GetEntityCount();
		editor.Press(ImGuiKey_D, ImGuiMod_Ctrl);
		CHECK(editor.Context().GetActiveScene()->GetEntityCount() == entities + 1);
		CHECK(editor.Context().GetPrimarySelection().GetUUID() != second);
		editor.Press(ImGuiKey_Delete);
		CHECK(editor.Context().GetActiveScene()->GetEntityCount() == entities);
		CHECK(editor.Get(second));
	}

	TEST_CASE("Ctrl+C and Ctrl+V in the Hierarchy paste copies with new IDs after the selection, in one undo step")
	{
		AuthoringEditor editor;
		const UUID parent = editor.Create("Parent");
		editor.Create("Child", { { "parent", UUIDToJson(parent) } });
		const UUID after = editor.Create("After");
		editor.Frames(2);
		editor.ClickRow(parent);
		editor.Press(ImGuiKey_C, ImGuiMod_Ctrl);
		CHECK(EditorActions::CanPaste());
		const size_t position = editor.UndoPosition();
		editor.Press(ImGuiKey_V, ImGuiMod_Ctrl);
		CHECK(editor.UndoPosition() == position + 1);
		CHECK(editor.RootNames() == std::vector<std::string> { "Parent", "Parent", "After" });
		// The copy is selected, with a new ID and its child.
		const Entity copy = editor.Context().GetPrimarySelection();
		REQUIRE(copy);
		CHECK(copy.GetUUID() != parent);
		REQUIRE(copy.GetChildren().size() == 1);
		CHECK(copy.GetChildren().front().GetName() == "Child");
		CHECK(editor.Get(after));

		// Ctrl+X cuts: copied, then deleted in one step.
		editor.Press(ImGuiKey_X, ImGuiMod_Ctrl);
		CHECK(editor.RootNames() == std::vector<std::string> { "Parent", "After" });
		CHECK(editor.UndoPosition() == position + 2);
	}

	TEST_CASE("Rows dropped before, after and into rows reorder and reparent in one undo step")
	{
		AuthoringEditor editor;
		const UUID a = editor.Create("A");
		const UUID b = editor.Create("B");
		const UUID c = editor.Create("C");
		editor.Frames(2);

		// C onto the top quarter of A: before it.
		const size_t position = editor.UndoPosition();
		SceneHierarchyPanel::RowRect target = editor.Row(a);
		editor.Drag(AuthoringEditor::Center(editor.Row(c)), ImVec2((target.Min.x + target.Max.x) * 0.5f, target.Min.y + (target.Max.y - target.Min.y) * 0.1f));
		CHECK(editor.RootNames() == std::vector<std::string> { "C", "A", "B" });
		CHECK(editor.UndoPosition() == position + 1);

		// A onto the middle of B: into it (as its last child), and B opens.
		editor.Pause();
		target = editor.Row(b);
		editor.Drag(AuthoringEditor::Center(editor.Row(a)), AuthoringEditor::Center(target));
		CHECK(editor.RootNames() == std::vector<std::string> { "C", "B" });
		REQUIRE(editor.Get(a).GetParent());
		CHECK(editor.Get(a).GetParent().GetUUID() == b);
		CHECK(editor.UndoPosition() == position + 2);
		CHECK(editor.Hierarchy().GetView().IsExpanded(b));

		// A onto the space below the rows: back to the end of the top level.
		editor.Pause();
		const SceneHierarchyPanel::RowRect lastRow = editor.Row(a);
		editor.Drag(AuthoringEditor::Center(lastRow), ImVec2((lastRow.Min.x + lastRow.Max.x) * 0.5f, lastRow.Max.y + ImGui::GetFrameHeight() * 4.0f));
		CHECK(editor.RootNames() == std::vector<std::string> { "C", "B", "A" });

		// Undo restores each step.
		REQUIRE(editor.Context().Undo());
		REQUIRE(editor.Context().Undo());
		REQUIRE(editor.Context().Undo());
		CHECK(editor.RootNames() == std::vector<std::string> { "A", "B", "C" });
	}

	TEST_CASE("An entity selected elsewhere is revealed: its ancestors open and its row comes into view")
	{
		AuthoringEditor editor;
		// Rows enough to scroll, then a chain five levels deep at the end.
		for (int index = 0; index < 200; index++)
			editor.Create(fmt::format("Filler {}", index));
		UUID parent = UUID::Null();
		std::vector<UUID> chain;
		for (int depth = 0; depth < 5; depth++)
		{
			nlohmann::json extra = nlohmann::json::object();
			if (parent.IsValid())
				extra["parent"] = UUIDToJson(parent);
			parent = editor.Create(fmt::format("Level {}", depth), extra);
			chain.push_back(parent);
		}
		editor.Frames(2);
		CHECK_FALSE(editor.Hierarchy().FindRowRect(chain.back()).has_value());

		// As a viewport pick or a tool selects it.
		editor.Context().Select(chain.back());
		editor.Frames(3);
		for (size_t index = 0; index + 1 < chain.size(); index++)
			CHECK(editor.Hierarchy().GetView().IsExpanded(chain[index]));
		const SceneHierarchyPanel::RowRect row = editor.Row(chain.back());
		ImGuiWindow* window = editor.FindPanelWindow(EditorPanels::c_Hierarchy);
		REQUIRE(window);
		CHECK(row.Min.y >= window->Pos.y);
		CHECK(row.Max.y <= window->Pos.y + window->Size.y);
	}

	TEST_CASE("The Hierarchy draws only the rows in view and does not rebuild them on idle frames")
	{
		AuthoringEditor editor;
		Scene& scene = *editor.Context().GetEditScene();
		for (int index = 0; index < 5000; index++)
			scene.CreateEntity(fmt::format("Root {}", index));
		editor.Frames(2);
		const uint64_t rebuilds = editor.Hierarchy().GetView().GetRebuildCount();
		editor.Frames(10);
		CHECK(editor.Hierarchy().GetView().GetRebuildCount() == rebuilds);
		CHECK(editor.Hierarchy().GetView().GetRows().size() == 5000);
		// A panel the height of the harness's window shows tens of rows, not thousands.
		size_t drawn = 0;
		for (const UUID id : scene.GetRootEntities())
			drawn += editor.Hierarchy().FindRowRect(id).has_value() ? 1 : 0;
		CHECK(drawn > 10);
		CHECK(drawn < 200);
	}

	TEST_CASE("The search box filters by name, component and tag")
	{
		AuthoringEditor editor;
		const UUID lamp = editor.Create("Lamp", { { "components", { { "PointLight", nlohmann::json::object() } } } });
		const UUID box = editor.Create("Box");
		const UUID group = editor.Create("Group");
		const UUID inner = editor.Create("Inner Lamp", { { "parent", UUIDToJson(group) }, { "components", { { "SpotLight", nlohmann::json::object() } } } });
		editor.Frames(2);
		REQUIRE(editor.ReplaceText("Hierarchy.Search", "c:light"));
		editor.Frames(2);
		CHECK(editor.Hierarchy().FindRowRect(lamp).has_value());
		CHECK(editor.Hierarchy().FindRowRect(inner).has_value());
		// The ancestor of a match is shown for context; the rest is not.
		CHECK(editor.Hierarchy().FindRowRect(group).has_value());
		CHECK_FALSE(editor.Hierarchy().FindRowRect(box).has_value());
		// An unknown component says so.
		REQUIRE(editor.ReplaceText("Hierarchy.Search", "c:nothing"));
		editor.Frames(2);
		CHECK(editor.Hierarchy().GetView().GetUnknownComponents() == std::vector<std::string> { "nothing" });
		CHECK_FALSE(editor.Hierarchy().FindRowRect(lamp).has_value());
		// The clear button shows the hierarchy again.
		REQUIRE(editor.Click("Hierarchy.Search.Clear"));
		editor.Frames(1);
		CHECK(editor.Hierarchy().FindRowRect(box).has_value());
	}

	TEST_CASE("While filtering, Ctrl+A and Shift+click ranges select the matches, not the ancestors shown for context")
	{
		AuthoringEditor editor;
		const UUID lamp = editor.Create("Lamp", { { "components", { { "PointLight", nlohmann::json::object() } } } });
		const UUID group = editor.Create("Group");
		const UUID inner = editor.Create("Inner", { { "parent", UUIDToJson(group) }, { "components", { { "PointLight", nlohmann::json::object() } } } });
		const UUID other = editor.Create("Other");
		editor.Frames(2);
		REQUIRE(editor.ReplaceText("Hierarchy.Search", "c:PointLight"));
		editor.Frames(2);
		// The rows: Lamp, Group (dimmed, for context) and Inner.
		REQUIRE(editor.Hierarchy().GetView().GetRows().size() == 3);
		REQUIRE_FALSE(editor.Hierarchy().FindRowRect(other).has_value());
		EditorContext& context = editor.Context();

		editor.ClickRow(lamp);
		editor.Press(ImGuiKey_A, ImGuiMod_Ctrl);
		CHECK(context.GetSelection().size() == 2);
		CHECK(context.IsSelected(lamp));
		CHECK(context.IsSelected(inner));
		CHECK_FALSE(context.IsSelected(group));

		editor.Pause();
		editor.ClickRow(lamp);
		editor.ClickRow(inner, ImGuiMod_Shift);
		CHECK(context.GetSelection().size() == 2);
		CHECK_FALSE(context.IsSelected(group));

		// A context row clicked on its own is selected: it is shown, after all.
		editor.ClickRow(group);
		CHECK(context.GetSelection() == std::vector<UUID> { group });
	}

	TEST_CASE("The eye toggles an entity, and an empty scene offers Cube, Light and Camera")
	{
		AuthoringEditor editor;
		REQUIRE(editor.Context().GetActiveScene()->GetEntityCount() == 0);
		REQUIRE(editor.Click("Hierarchy.AddCube"));
		REQUIRE(editor.Context().GetActiveScene()->GetEntityCount() == 1);
		const Entity cube = editor.Context().GetPrimarySelection();
		REQUIRE(cube);
		CHECK(cube.GetName() == "Cube");
		CHECK(cube.GetComponent<MeshRendererComponent>().Mesh == BuiltinAssets::CubeMesh);
		// The scene is not empty any more: no hint.
		editor.Frames(1);
		CHECK_FALSE(UI::ItemProbe::Find("Hierarchy.AddLight").has_value());

		// The eye at the right end of the row.
		const SceneHierarchyPanel::RowRect row = editor.Row(cube.GetUUID());
		const float button = ImGui::GetFrameHeight();
		const size_t position = editor.UndoPosition();
		editor.ClickWith(ImVec2(row.Max.x - button * 0.5f, (row.Min.y + row.Max.y) * 0.5f), ImGuiKey_None);
		CHECK_FALSE(cube.IsActive());
		CHECK(editor.UndoPosition() == position + 1);
		editor.Pause();
		editor.ClickWith(ImVec2(row.Max.x - button * 0.5f, (row.Min.y + row.Max.y) * 0.5f), ImGuiKey_None);
		CHECK(cube.IsActive());
	}

	TEST_CASE("The prefab badge of an instance's row shows the prefab in the Asset tab")
	{
		AuthoringEditor editor({}, WithFeatureProject("EditorAuthoringPrefabBadge"));
		const nlohmann::json found = editor.Run("entity.find", { { "name", "Crate" } })["entities"];
		REQUIRE(found.size() == 1);
		const UUID crate = *UUIDFromJson(found[0]);
		const Entity entity = editor.Get(crate);
		REQUIRE(entity.HasComponent<PrefabInstanceComponent>());
		const AssetHandle prefab = entity.GetComponent<PrefabInstanceComponent>().Prefab;
		// Selected, so its row is revealed; the badge sits left of the eye at the row's right end.
		editor.Context().Select(crate);
		editor.Frames(3);
		const SceneHierarchyPanel::RowRect row = editor.Row(crate);
		const float button = ImGui::GetFrameHeight();
		editor.ClickWith(ImVec2(row.Max.x - button * 1.5f, AuthoringEditor::Center(row).y), ImGuiKey_None);
		editor.Frames(2);
		CHECK(editor.Context().GetInspectedAsset() == prefab);
		ImGuiWindow* assetWindow = editor.FindPanelWindow(EditorPanels::c_AssetInspector);
		REQUIRE(assetWindow);
		CHECK_FALSE(assetWindow->Hidden);
	}

	TEST_CASE("Plain entities get a full-size icon: a dashed square, or a folder while they group children")
	{
		Scene scene("Icons");
		const Entity plain = scene.CreateEntity("Plain");
		const Entity group = scene.CreateEntity("Group");
		const Entity child = scene.CreateEntity("Child");
		REQUIRE(scene.SetParent(child, group));
		const entt::registry& registry = scene.GetRegistry();
		const auto icon = [&registry](Entity entity) { return std::string(UI::GetEntityIcon(registry, entity.GetHandle())); };
		CHECK(icon(plain) == Icons::SquareDashed);
		CHECK(icon(child) == Icons::SquareDashed);
		CHECK(icon(group) == Icons::Folder);
		// What an entity is still wins: a light that has children is a light.
		Entity light = group;
		light.AddComponent<PointLightComponent>();
		CHECK(icon(group) == Icons::Lightbulb);
	}

	TEST_CASE("The create button and a row's context menu create entities")
	{
		AuthoringEditor editor;
		const UUID parent = editor.Create("Parent");
		editor.Frames(1);
		REQUIRE(editor.Click("Hierarchy.Create"));
		REQUIRE(editor.Click("Create.Point Light"));
		const Entity light = editor.Context().GetPrimarySelection();
		REQUIRE(light);
		CHECK(light.HasComponent<PointLightComponent>());
		CHECK_FALSE(light.GetParent());

		// A right click selects the row and opens its menu.
		editor.Pause();
		const ImVec2 center = AuthoringEditor::Center(editor.Row(parent));
		editor.Harness.MoveMouse(center);
		editor.Frames(1);
		editor.Harness.SetMouseButton(ImGuiMouseButton_Right, true);
		editor.Frames(1);
		editor.Harness.SetMouseButton(ImGuiMouseButton_Right, false);
		editor.Frames(2);
		CHECK(editor.Context().IsSelected(parent));
		CHECK(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
	}

	TEST_CASE("A banner tops the Hierarchy and the Inspector while playing")
	{
		AuthoringEditor editor({}, WithFeatureProject("EditorAuthoringPlay"));
		CHECK_FALSE(UI::ItemProbe::Find("Hierarchy.PlayBanner").has_value());
		editor.Run("play.start");
		editor.Frames(2);
		CHECK(UI::ItemProbe::Find("Hierarchy.PlayBanner").has_value());
		CHECK(UI::ItemProbe::Find("Inspector.PlayBanner").has_value());
		CHECK(UI::ItemProbe::Find("Inspector.PlayBanner")->Text == "Playing: changes are discarded when play stops");
		editor.Run("play.stop");
		editor.Frames(2);
		CHECK_FALSE(UI::ItemProbe::Find("Hierarchy.PlayBanner").has_value());
	}
}

TEST_SUITE("Editor.UI.Inspector")
{
	TEST_CASE("Clicking the X-axis label resets only X")
	{
		AuthoringEditor editor;
		const UUID id = editor.Create("Moved", { { "components", { { "Transform", { { "Translation", { 1, 2, 3 } }, { "Scale", { 2, 3, 4 } } } } } } });
		editor.Context().Select(id);
		editor.Frames(2);
		const size_t position = editor.UndoPosition();
		REQUIRE(editor.Click("Inspector.Transform.Translation.X"));
		CHECK(editor.Get(id).GetComponent<TransformComponent>().Translation == glm::vec3(0.0f, 2.0f, 3.0f));
		CHECK(editor.UndoPosition() == position + 1);
		// Scale resets to its own default, 1.
		REQUIRE(editor.Click("Inspector.Transform.Scale.Y"));
		CHECK(editor.Get(id).GetComponent<TransformComponent>().Scale == glm::vec3(2.0f, 1.0f, 4.0f));
		CHECK(editor.UndoPosition() == position + 2);

		// Several entities whose X differs, the first one's at the default already: the label still resets every X, and
		// only X.
		const UUID other = editor.Create("Other", { { "components", { { "Transform", { { "Translation", { 5, 6, 7 } } } } } } });
		editor.Context().SetSelection({ id, other });
		editor.Frames(2);
		REQUIRE(editor.Click("Inspector.Transform.Translation.X"));
		CHECK(editor.Get(id).GetComponent<TransformComponent>().Translation == glm::vec3(0.0f, 2.0f, 3.0f));
		CHECK(editor.Get(other).GetComponent<TransformComponent>().Translation == glm::vec3(0.0f, 6.0f, 7.0f));
		CHECK(editor.UndoPosition() == position + 4);
	}

	TEST_CASE("Paste Values onto three selected entities is one undo step")
	{
		AuthoringEditor editor;
		const UUID source = editor.Create("Source", { { "components", { { "PointLight", { { "Intensity", 9.0 }, { "Range", 3.0 }, { "Color", { 0.25, 0.5, 0.75 } } } } } } });
		std::vector<UUID> targets;
		for (int index = 0; index < 3; index++)
			targets.push_back(editor.Create(fmt::format("Target {}", index), { { "components", { { "PointLight", nlohmann::json::object() } } } }));

		editor.Context().Select(source);
		editor.Frames(2);
		REQUIRE(editor.Click("Inspector.PointLight.Menu"));
		REQUIRE(editor.Click("Inspector.PointLight.Menu.Copy"));
		REQUIRE(editor.Harness.GetClipboard().find(EditorActions::c_ComponentValuesFormat) != std::string::npos);

		editor.Context().SetSelection(targets);
		editor.Frames(2);
		const size_t position = editor.UndoPosition();
		REQUIRE(editor.Click("Inspector.PointLight.Menu"));
		REQUIRE(editor.Click("Inspector.PointLight.Menu.Paste"));
		CHECK(editor.UndoPosition() == position + 1);
		for (const UUID target : targets)
		{
			const PointLightComponent& light = editor.Get(target).GetComponent<PointLightComponent>();
			CHECK(light.Intensity == 9.0f);
			CHECK(light.Range == 3.0f);
			CHECK(light.Color == glm::vec3(0.25f, 0.5f, 0.75f));
		}

		// Reset restores the defaults of all three, again in one step; Remove takes the component off all three.
		REQUIRE(editor.Click("Inspector.PointLight.Menu"));
		REQUIRE(editor.Click("Inspector.PointLight.Menu.Reset"));
		CHECK(editor.UndoPosition() == position + 2);
		for (const UUID target : targets)
			CHECK(editor.Get(target).GetComponent<PointLightComponent>().Intensity == PointLightComponent().Intensity);
		REQUIRE(editor.Click("Inspector.PointLight.Remove"));
		CHECK(editor.UndoPosition() == position + 3);
		for (const UUID target : targets)
			CHECK_FALSE(editor.Get(target).HasComponent<PointLightComponent>());
	}

	TEST_CASE("Components without settable properties offer no Reset, Copy or Paste")
	{
		AuthoringEditor editor({}, WithFeatureProject("EditorAuthoringReadOnlyMenu"));
		const nlohmann::json found = editor.Run("entity.find", { { "name", "Crate" } })["entities"];
		REQUIRE(found.size() == 1);
		REQUIRE(editor.Get(*UUIDFromJson(found[0])).HasComponent<PrefabInstanceComponent>());
		editor.Context().Select(*UUIDFromJson(found[0]));
		editor.Frames(2);
		// The prefab link: shown, but every property is read-only and the engine adds it, so it has no menu at all.
		REQUIRE(UI::ItemProbe::Find("Inspector.Transform.Menu").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.PrefabInstance.Menu").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Inspector.PrefabInstance.Remove").has_value());

		// Copied values hold only what component.set accepts: the link's read-only values are nothing to paste.
		const ComponentInfo* prefab = ComponentRegistry::Find<PrefabInstanceComponent>();
		REQUIRE(prefab);
		const nlohmann::json values = editor.Run("component.get", { { "entity", found[0] }, { "component", "PrefabInstance" } })["values"];
		REQUIRE_FALSE(values.empty());
		EditorActions::CopyComponentValues(*prefab, values);
		CHECK_FALSE(EditorActions::ReadComponentValues(*prefab).has_value());
		// Others keep their settable values.
		const ComponentInfo* transform = ComponentRegistry::Find<TransformComponent>();
		REQUIRE(transform);
		EditorActions::CopyComponentValues(*transform, { { "Translation", { 1, 2, 3 } } });
		const std::optional<nlohmann::json> pasted = EditorActions::ReadComponentValues(*transform);
		REQUIRE(pasted);
		CHECK((*pasted)["Translation"] == nlohmann::json { 1, 2, 3 });
	}

	TEST_CASE("Editing the intensity of 30 selected lights is one undo step")
	{
		AuthoringEditor editor;
		std::vector<UUID> lights;
		for (int index = 0; index < 30; index++)
			lights.push_back(editor.Create(fmt::format("Light {}", index), { { "components", { { "PointLight", { { "Intensity", 1.0 + index } } } } } }));
		editor.Context().SetSelection(lights);
		editor.Frames(2);
		const size_t position = editor.UndoPosition();

		// A drag over the field: the values differ (mixed), and every light gets the dragged value.
		const ImVec2 field = editor.ProbeCenter("Inspector.PointLight.Intensity");
		editor.Drag(field, ImVec2(field.x + ImGui::GetFontSize() * 6.0f, field.y));
		CHECK(editor.UndoPosition() == position + 1);
		const float intensity = editor.Get(lights.front()).GetComponent<PointLightComponent>().Intensity;
		CHECK(intensity > 1.0f);
		for (const UUID light : lights)
			CHECK(editor.Get(light).GetComponent<PointLightComponent>().Intensity == intensity);
		CHECK(editor.Context().GetUndoStack().GetUndoName() == "Edit Point Light Intensity of 30 Entities");
		REQUIRE(editor.Context().Undo());
		for (size_t index = 0; index < lights.size(); index++)
			CHECK(editor.Get(lights[index]).GetComponent<PointLightComponent>().Intensity == 1.0f + static_cast<float>(index));
	}

	TEST_CASE("The header renames the entity and toggles several entities at once")
	{
		AuthoringEditor editor;
		const UUID first = editor.Create("First");
		const UUID second = editor.Create("Second");
		editor.Context().Select(first);
		editor.Frames(2);
		REQUIRE(editor.ReplaceText("Inspector.Name", "Header Name"));
		editor.Press(ImGuiKey_Enter);
		CHECK(editor.Get(first).GetName() == "Header Name");

		editor.Context().SetSelection({ first, second });
		editor.Frames(2);
		const size_t position = editor.UndoPosition();
		REQUIRE(editor.Click("Inspector.Active"));
		CHECK_FALSE(editor.Get(first).IsActive());
		CHECK_FALSE(editor.Get(second).IsActive());
		CHECK(editor.UndoPosition() == position + 1);
	}

	TEST_CASE("With nothing selected the Inspector shows the scene's settings and sky")
	{
		AuthoringEditor editor;
		editor.Context().ClearSelection();
		editor.Frames(2);
		REQUIRE(UI::ItemProbe::Find("Inspector.SceneSettings").has_value());
		editor.Run("scene.setSettings", { { "gravity", { 1.0, 2.0, 3.0 } } });
		editor.Frames(1);
		const size_t position = editor.UndoPosition();
		// The Y label resets gravity's Y to the default, through scene.setSettings: one step.
		REQUIRE(editor.Click("Inspector.Scene.Gravity.Y"));
		CHECK(editor.Context().GetActiveScene()->GetSettings().Gravity == glm::vec3(1.0f, SceneSettings().Gravity.y, 3.0f));
		CHECK(editor.UndoPosition() == position + 1);

		// Without a sky light: a button adds one, and its settings show here.
		REQUIRE(editor.Click("Inspector.AddSky"));
		editor.Frames(1);
		CHECK(UI::ItemProbe::Find("Inspector.SkyLight.Intensity").has_value());
		REQUIRE(editor.Click("Inspector.SelectSky"));
		REQUIRE(editor.Context().GetPrimarySelection());
		CHECK(editor.Context().GetPrimarySelection().HasComponent<SkyLightComponent>());
	}

	TEST_CASE("The asset picker searches the assets of the property's type and picks with Enter")
	{
		AuthoringEditor editor;
		const UUID id = editor.Create("Shape", { { "components", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } } } } });
		editor.Context().Select(id);
		editor.Frames(2);
		REQUIRE(editor.Click("Inspector.MeshRenderer.Mesh"));
		editor.Frames(1);
		editor.Harness.TypeText("sphere");
		editor.Frames(2);
		editor.Press(ImGuiKey_Enter);
		CHECK(editor.Get(id).GetComponent<MeshRendererComponent>().Mesh == BuiltinAssets::SphereMesh);
	}

	TEST_CASE("The entity picker lists the scene's entities and picks one")
	{
		ImGuiHarness harness;
		Scene scene("Picker");
		scene.CreateEntity("Alpha");
		const UUID beta = scene.CreateEntity("Beta Target").GetUUID();
		PropertyInfo property;
		property.Name = "Target";
		property.DisplayName = "Target";
		property.Type = PropertyType::Entity;
		PropertyValue value = UUID::Null();
		PropertyEditResult last;
		PropertyWidgetOptions options;
		options.ProbeKey = "Test.EntityPicker";
		const auto draw = [&]()
		{
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, ImGui::GetFontSize() * 30.0f));
			ImGui::Begin("Picker");
			const PropertyEditResult result = DrawPropertyWidget(property, value, scene, options);
			if (result.Changed)
				last = result;
			ImGui::End();
		};
		harness.Frame(draw);
		harness.Frame(draw);
		const std::optional<UI::ItemProbe::Item> button = UI::ItemProbe::Find("Test.EntityPicker");
		REQUIRE(button);
		harness.MoveMouse(button->GetCenter());
		harness.Frame(draw);
		harness.SetMouseButton(ImGuiMouseButton_Left, true);
		harness.Frame(draw);
		harness.SetMouseButton(ImGuiMouseButton_Left, false);
		harness.Frame(draw);
		harness.Frame(draw);
		harness.TypeText("beta");
		harness.Frame(draw);
		harness.Frame(draw);
		harness.SetKey(ImGuiKey_Enter, true);
		harness.Frame(draw);
		harness.SetKey(ImGuiKey_Enter, false);
		harness.Frame(draw);
		CHECK(std::get<UUID>(value) == beta);
		CHECK(last.Changed);
		CHECK(last.Finished);
	}

	TEST_CASE("A zero that is negative shows as 0")
	{
		ImGuiHarness harness;
		Scene scene("Zero");
		PropertyInfo property;
		property.Name = "Value";
		property.Type = PropertyType::Float;
		PropertyValue value = -0.0f;
		PropertyWidgetOptions options;
		options.ProbeKey = "Test.Zero";
		const auto draw = [&]()
		{
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 20.0f, ImGui::GetFontSize() * 10.0f));
			ImGui::Begin("Zero");
			DrawPropertyWidget(property, value, scene, options);
			ImGui::End();
		};
		harness.Frame(draw);
		harness.Frame(draw);
		const std::optional<UI::ItemProbe::Item> field = UI::ItemProbe::Find("Test.Zero");
		REQUIRE(field);
		// The field's text, as Ctrl+click shows it for typing, copied to the harness's clipboard.
		const auto press = [&](ImGuiKey key, bool down)
		{
			harness.SetKey(key, down);
			harness.Frame(draw);
		};
		harness.MoveMouse(field->GetCenter());
		harness.Frame(draw);
		press(ImGuiMod_Ctrl, true);
		harness.SetMouseButton(ImGuiMouseButton_Left, true);
		harness.Frame(draw);
		harness.SetMouseButton(ImGuiMouseButton_Left, false);
		harness.Frame(draw);
		press(ImGuiKey_A, true);
		press(ImGuiKey_A, false);
		press(ImGuiKey_C, true);
		press(ImGuiKey_C, false);
		press(ImGuiMod_Ctrl, false);
		CHECK(harness.GetClipboard() == "0.000");
		// Showing it changed nothing.
		press(ImGuiKey_Escape, true);
		press(ImGuiKey_Escape, false);
		CHECK(std::signbit(std::get<float>(value)));
	}
}

TEST_SUITE("Editor.UI.AssetInspector")
{
	TEST_CASE("Clicking an asset in the Content Browser shows it in the Asset tab, and selecting an entity returns to the Inspector")
	{
		AuthoringEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorAuthoringAssetTab"));
		EditorAssetManager* assets = editor.Context().GetAssetManager();
		REQUIRE(assets);
		const AssetHandle material = assets->FindAssetByPath("Materials/Feature.stmat");
		REQUIRE(material.IsValid());
		ContentBrowserPanel* browser = editor.Layer->GetPanels().Get<ContentBrowserPanel>(EditorPanels::c_ContentBrowser);
		REQUIRE(browser);
		browser->SetCurrentDirectory("Materials");
		editor.Frames(3);
		const std::optional<ContentBrowserPanel::ItemRect> item = browser->FindItemRect(material);
		REQUIRE(item);
		editor.ClickAt(ImVec2((item->Min.x + item->Max.x) * 0.5f, (item->Min.y + item->Max.y) * 0.5f));
		editor.Frames(2);
		CHECK(editor.Context().GetInspectedAsset() == material);
		ImGuiWindow* assetWindow = editor.FindPanelWindow(EditorPanels::c_AssetInspector);
		ImGuiWindow* inspectorWindow = editor.FindPanelWindow(EditorPanels::c_Inspector);
		REQUIRE(assetWindow);
		REQUIRE(inspectorWindow);
		CHECK_FALSE(assetWindow->Hidden);
		CHECK(inspectorWindow->Hidden);
		// The material's properties once it has loaded.
		CHECK(editor.FramesUntil([]() { return UI::ItemProbe::Find("AssetInspector.Material.BaseColor").has_value(); }));
		// The Content Browser keeps the keyboard focus (Delete there must not reach the scene).
		CHECK(ImGui::GetCurrentContext()->NavWindow != assetWindow);

		const nlohmann::json found = editor.Run("entity.find", { { "name", "Ball" } })["entities"];
		REQUIRE(found.size() == 1);
		editor.Run("selection.set", { { "entities", found } });
		editor.Frames(2);
		CHECK_FALSE(inspectorWindow->Hidden);
		CHECK(assetWindow->Hidden);
	}

	TEST_CASE("Clicking the asset the Asset tab shows brings the tab back after another tab came forward")
	{
		AuthoringEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorAuthoringInspectAgain"));
		EditorAssetManager* assets = editor.Context().GetAssetManager();
		REQUIRE(assets);
		const AssetHandle material = assets->FindAssetByPath("Materials/Feature.stmat");
		ContentBrowserPanel* browser = editor.Layer->GetPanels().Get<ContentBrowserPanel>(EditorPanels::c_ContentBrowser);
		REQUIRE(browser);
		browser->SetCurrentDirectory("Materials");
		editor.Frames(3);
		const auto clickMaterial = [&]()
		{
			const std::optional<ContentBrowserPanel::ItemRect> item = browser->FindItemRect(material);
			REQUIRE(item);
			editor.ClickAt(ImVec2((item->Min.x + item->Max.x) * 0.5f, (item->Min.y + item->Max.y) * 0.5f));
			editor.Frames(2);
		};
		clickMaterial();
		ImGuiWindow* assetWindow = editor.FindPanelWindow(EditorPanels::c_AssetInspector);
		REQUIRE(assetWindow);
		REQUIRE_FALSE(assetWindow->Hidden);

		// The user looks at the Inspector tab, then clicks the same asset again: it comes back.
		editor.FocusPanel(EditorPanels::c_Inspector);
		REQUIRE(assetWindow->Hidden);
		editor.Pause();
		clickMaterial();
		CHECK(editor.Context().GetInspectedAsset() == material);
		CHECK_FALSE(assetWindow->Hidden);

		// The same through selection.set.
		editor.FocusPanel(EditorPanels::c_Inspector);
		REQUIRE(assetWindow->Hidden);
		editor.Run("selection.set", { { "asset", UUIDToJson(material) } });
		editor.Frames(2);
		CHECK_FALSE(assetWindow->Hidden);
	}

	TEST_CASE("A material drag in the Asset tab previews, saves the file once and is one undo step")
	{
		AuthoringEditor editor({}, WithFeatureProject("EditorAuthoringMaterialDrag"));
		EditorAssetManager* assets = editor.Context().GetAssetManager();
		REQUIRE(assets);
		const AssetHandle material = assets->FindAssetByPath("Materials/Feature.stmat");
		REQUIRE(editor.Context().SetInspectedAsset(material));
		REQUIRE(AssetManager::LoadAssetSync<Material>(material));
		editor.Frames(3);
		const std::filesystem::path file = assets->GetAbsolutePath(material);
		const std::optional<std::string> before = FileSystem::ReadText(file);
		REQUIRE(before);
		const float roughness = AssetManager::GetAsset<Material>(material)->GetProperties().Roughness;
		const size_t position = editor.UndoPosition();

		// Pressed and moved: the loaded material follows, the file does not.
		const ImVec2 field = editor.ProbeCenter("AssetInspector.Material.Roughness");
		editor.Harness.MoveMouse(field);
		editor.Frames(1);
		editor.Harness.SetMouseButton(ImGuiMouseButton_Left, true);
		editor.Frames(1);
		editor.Harness.MoveMouse(ImVec2(field.x + ImGui::GetFontSize() * 4.0f, field.y));
		editor.Frames(2);
		CHECK(AssetManager::GetAsset<Material>(material)->GetProperties().Roughness != roughness);
		CHECK(FileSystem::ReadText(file) == before);
		CHECK(editor.UndoPosition() == position);

		// Released: saved once, one undo step; undo restores the file.
		editor.Harness.SetMouseButton(ImGuiMouseButton_Left, false);
		editor.Frames(2);
		CHECK(FileSystem::ReadText(file) != before);
		CHECK(editor.UndoPosition() == position + 1);
		// Material edits never mark the scene unsaved.
		CHECK_FALSE(editor.Context().IsSceneModified());
		REQUIRE(editor.Context().Undo());
		CHECK(FileSystem::ReadText(file) == before);
		CHECK(AssetManager::GetAsset<Material>(material)->GetProperties().Roughness == roughness);
	}

	TEST_CASE("Import settings: Apply imports in the background, Revert discards the draft")
	{
		AuthoringEditor editor({}, WithFeatureProject("EditorAuthoringImport"));
		EditorAssetManager* assets = editor.Context().GetAssetManager();
		REQUIRE(assets);
		const AssetHandle texture = assets->FindAssetByPath("Textures/Checker.png");
		REQUIRE(texture.IsValid());
		REQUIRE(editor.Context().SetInspectedAsset(texture));
		editor.Frames(2);
		const bool mips = assets->GetImportSettings(texture).value("GenerateMips", true);
		CHECK_FALSE(UI::ItemProbe::Find("AssetInspector.Apply")->Enabled);

		// A change is a draft until applied; Revert drops it.
		REQUIRE(editor.Click("AssetInspector.Import.GenerateMips"));
		CHECK(UI::ItemProbe::Find("AssetInspector.Apply")->Enabled);
		CHECK(assets->GetImportSettings(texture).value("GenerateMips", true) == mips);
		REQUIRE(editor.Click("AssetInspector.Revert"));
		editor.Frames(1);
		CHECK_FALSE(UI::ItemProbe::Find("AssetInspector.Apply")->Enabled);

		// Applied: asset.setImportSettings imports in the background while frames go on.
		REQUIRE(editor.Click("AssetInspector.Import.GenerateMips"));
		REQUIRE(editor.Click("AssetInspector.Apply"));
		REQUIRE(editor.FramesUntil([&]() { return !assets->IsImportPending(texture) && editor.Layer->GetCommandRunner().GetPendingCount() == 0; }));
		editor.Frames(2);
		CHECK(assets->GetImportSettings(texture).value("GenerateMips", true) != mips);
		CHECK(assets->GetImportInfo(texture).Error.empty());
		CHECK_FALSE(UI::ItemProbe::Find("AssetInspector.Apply")->Enabled);
	}

	TEST_CASE("Apply sends only the changed settings: a setting the importer does not describe stays in the .meta")
	{
		// A .meta written by another engine version, holding a setting this importer does not know.
		EditorOptions options = WithFeatureProject("EditorAuthoringUnknownSetting");
		const std::filesystem::path meta = options.ProjectPath.parent_path() / "Assets" / "Textures" / "Checker.png.meta";
		const std::optional<std::string> text = FileSystem::ReadText(meta);
		REQUIRE(text);
		std::optional<nlohmann::json> document = JsonUtils::Parse(*text);
		REQUIRE(document);
		(*document)["ImportSettings"]["Compression"] = "Auto";
		REQUIRE(FileSystem::WriteText(meta, JsonUtils::Dump(*document)));

		AuthoringEditor editor({}, options);
		EditorAssetManager* assets = editor.Context().GetAssetManager();
		REQUIRE(assets);
		const AssetHandle texture = assets->FindAssetByPath("Textures/Checker.png");
		REQUIRE(texture.IsValid());
		REQUIRE(assets->GetImportSettings(texture).value("Compression", "") == "Auto");
		REQUIRE(editor.Context().SetInspectedAsset(texture));
		editor.Frames(2);
		const bool mips = assets->GetImportSettings(texture).value("GenerateMips", true);

		REQUIRE(editor.Click("AssetInspector.Import.GenerateMips"));
		REQUIRE(editor.Click("AssetInspector.Apply"));
		REQUIRE(editor.FramesUntil([&]() { return !assets->IsImportPending(texture) && editor.Layer->GetCommandRunner().GetPendingCount() == 0; }));
		editor.Frames(2);
		// Applied: the changed setting, and the unknown one kept as it was.
		CHECK(assets->GetImportSettings(texture).value("GenerateMips", true) != mips);
		CHECK(assets->GetImportSettings(texture).value("Compression", "") == "Auto");
		CHECK(assets->GetImportInfo(texture).Error.empty());
		CHECK_FALSE(UI::ItemProbe::Find("AssetInspector.Apply")->Enabled);
	}

	TEST_CASE("By hand: a cube, a new material with a color and a texture, saved, equals the same commands")
	{
		// The UI: the Hierarchy's create menu, the Inspector's New Material and its material section, Ctrl+S.
		const EditorOptions options = WithFeatureProject("EditorAuthoringByHand");
		std::string handMaterial;
		std::string handScene;
		UUID handCube = UUID::Null();
		AssetHandle handMaterialHandle = UUID::Null();
		AssetHandle texture = UUID::Null();
		{
			AuthoringEditor editor({ ImVec2(1600.0f, 1200.0f), 1.0f }, options);
			EditorAssetManager* assets = editor.Context().GetAssetManager();
			REQUIRE(assets);
			texture = assets->FindAssetByPath("Textures/Checker.png");
			REQUIRE(texture.IsValid());

			REQUIRE(editor.Click("Hierarchy.Create"));
			REQUIRE(editor.Click("Create.Cube"));
			const Entity cube = editor.Context().GetPrimarySelection();
			REQUIRE(cube);
			handCube = cube.GetUUID();
			editor.Frames(2);
			REQUIRE(editor.Click("Inspector.MeshRenderer.NewMaterial"));
			handMaterialHandle = cube.GetComponent<MeshRendererComponent>().Material;
			REQUIRE(handMaterialHandle.IsValid());
			REQUIRE(assets->GetMetadata(handMaterialHandle)->Path == "Materials/Cube.stmat");
			editor.Frames(1);
			REQUIRE(editor.Click("Inspector.MeshRenderer.EditMaterial"));
			REQUIRE(editor.FramesUntil([&]() { return UI::ItemProbe::Find("Inspector.Material.BaseColor").has_value(); }));

			// The base color's red channel typed into its field (Ctrl+click turns a drag field into text).
			const UI::ItemProbe::Item color = *UI::ItemProbe::Find("Inspector.Material.BaseColor");
			const float channel = (color.Max.x - color.Min.x - ImGui::GetFrameHeight()) / 4.0f;
			editor.ClickWith(ImVec2(color.Min.x + channel * 0.5f, color.GetCenter().y), ImGuiMod_Ctrl);
			editor.Harness.SetKey(ImGuiMod_Ctrl, true);
			editor.Harness.SetKey(ImGuiKey_A, true);
			editor.Frames(1);
			editor.Harness.SetKey(ImGuiKey_A, false);
			editor.Harness.SetKey(ImGuiMod_Ctrl, false);
			editor.Frames(1);
			editor.Harness.TypeText("0.25");
			editor.Frames(1);
			editor.Press(ImGuiKey_Enter);
			REQUIRE(AssetManager::GetAsset<Material>(handMaterialHandle));
			CHECK(AssetManager::GetAsset<Material>(handMaterialHandle)->GetProperties().BaseColor.r == doctest::Approx(0.25f));

			// The texture through the picker: search, Enter.
			editor.Pause();
			REQUIRE(editor.Click("Inspector.Material.BaseColorMap"));
			editor.Frames(1);
			editor.Harness.TypeText("Checker");
			editor.Frames(2);
			editor.Press(ImGuiKey_Enter);
			CHECK(AssetManager::GetAsset<Material>(handMaterialHandle)->GetProperties().BaseColorMap == texture);

			editor.Press(ImGuiKey_S, ImGuiMod_Ctrl);
			CHECK_FALSE(editor.Context().IsSceneModified());
			const std::optional<std::string> material = FileSystem::ReadText(assets->GetAbsolutePath(handMaterialHandle));
			const std::optional<std::string> scene = FileSystem::ReadText(assets->GetAbsolutePath(editor.Context().GetSceneHandle()));
			REQUIRE(material);
			REQUIRE(scene);
			handMaterial = *material;
			handScene = *scene;
		}

		// The same through commands, in another copy of the project.
		const std::filesystem::path commandProject = CopyFeatureProject(CreateTemporaryDirectory("EditorAuthoringByCommands") / "Project");
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		std::string error;
		REQUIRE_MESSAGE(context.OpenProject(commandProject, &error), error);
		auto run = [&](std::string_view name, const nlohmann::json& parameters)
		{
			const EditorCommandResult result = commands.Execute(context, name, parameters);
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		};
		const nlohmann::json cube = run("entity.create", { { "name", "Cube" }, { "components", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } } } } })["id"];
		const nlohmann::json material = run("material.create", { { "path", "Materials/Cube.stmat" } })["asset"];
		run("component.set", { { "entity", cube }, { "component", "MeshRenderer" }, { "values", { { "Material", material } } } });
		const glm::vec4 white = MaterialProperties().BaseColor;
		run("material.set", { { "material", material }, { "properties", { { "BaseColor", { 0.25, white.g, white.b, white.a } }, { "BaseColorMap", UUIDToJson(texture) } } } });
		run("scene.save", nlohmann::json::object());
		const std::optional<std::string> commandMaterial = FileSystem::ReadText(context.GetAssetManager()->GetAbsolutePath(*UUIDFromJson(material)));
		const std::optional<std::string> commandScene = FileSystem::ReadText(context.GetAssetManager()->GetAbsolutePath(context.GetSceneHandle()));
		REQUIRE(commandMaterial);
		REQUIRE(commandScene);

		// The material files are the same; the scenes are, but for the new entity's ID and the new material's handle.
		CHECK(handMaterial == *commandMaterial);
		const auto normalize = [](std::string text, const std::string& entity, const std::string& asset)
		{
			for (const auto& [from, to] : { std::pair { entity, std::string("<cube>") }, std::pair { asset, std::string("<material>") } })
			{
				for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
					text.replace(at, from.size(), to);
			}
			return text;
		};
		CHECK(normalize(handScene, UUIDToJson(handCube).get<std::string>(), UUIDToJson(handMaterialHandle).get<std::string>())
			== normalize(*commandScene, cube.get<std::string>(), material.get<std::string>()));
		context.CloseProject();
	}
}
