#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "Editor/SceneEdit.h"
#include "TestHelpers.h"
#include "UI/EditorActions.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

// Names the editor gives: copies and instances unique among their siblings ("Crate (1)"), and undo steps that say what they
// changed ("Create 'Wall'", "Delete 3 entities", "Set Material of 'Ground'").

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	struct NameHarness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return RunCommandToCompletion(Context, Commands, name, parameters);
		}

		std::string Create(const std::string& name, const nlohmann::json& parent = nullptr)
		{
			nlohmann::json parameters = { { "name", name } };
			if (!parent.is_null())
				parameters["parent"] = parent;
			return Run("entity.create", parameters)["id"].get<std::string>();
		}

		std::string NameOf(const std::string& id) const
		{
			return Context.GetEditScene()->GetEntityByUUID(*UUID::FromString(id)).GetName();
		}

		std::string LastStep() { return Context.GetUndoStack().GetUndoName(); }
	};

}

TEST_SUITE("Editor.Names")
{
	TEST_CASE("Copy numbers come off and go on")
	{
		CHECK(SceneEdit::GetBaseName("Crate") == "Crate");
		CHECK(SceneEdit::GetBaseName("Crate (1)") == "Crate");
		CHECK(SceneEdit::GetBaseName("Crate (12)") == "Crate");
		CHECK(SceneEdit::GetBaseName("Crate (x)") == "Crate (x)");
		CHECK(SceneEdit::GetBaseName("(1)") == "(1)");
		CHECK(SceneEdit::GetBaseName("Crate ()") == "Crate ()");
		CHECK(SceneEdit::GetBaseName("Crate(1)") == "Crate(1)");
		CHECK(SceneEdit::QuoteName("Wall") == "'Wall'");
		CHECK(SceneEdit::QuoteName(std::string(60, 'w')) == "'" + std::string(SceneEdit::c_MaxNameInStep, 'w') + "...'");
	}

	TEST_CASE("Duplicates, pastes and prefab instances are named uniquely among their siblings")
	{
		NameHarness harness;
		const std::string crate = harness.Create("Crate");
		const std::string first = harness.Run("entity.duplicate", { { "entities", { crate } } })["id"].get<std::string>();
		CHECK(harness.NameOf(first) == "Crate (1)");
		const std::string second = harness.Run("entity.duplicate", { { "entities", { crate } } })["id"].get<std::string>();
		CHECK(harness.NameOf(second) == "Crate (2)");
		// A copy of a copy counts on from the base name; several copies of one name in one command each get their own number.
		const std::string third = harness.Run("entity.duplicate", { { "entities", { first } } })["id"].get<std::string>();
		CHECK(harness.NameOf(third) == "Crate (3)");
		const nlohmann::json both = harness.Run("entity.duplicate", { { "entities", { crate, second } } })["entities"];
		CHECK(harness.NameOf(both[0].get<std::string>()) == "Crate (4)");
		CHECK(harness.NameOf(both[1].get<std::string>()) == "Crate (5)");

		// Siblings only: under another parent the name is free, and children keep their names.
		const std::string group = harness.Create("Group");
		const std::string child = harness.Create("Crate", group);
		CHECK(harness.NameOf(child) == "Crate");
		const nlohmann::json copied = harness.Run("entity.copy", { { "entities", { crate } } });
		const std::string pasted = harness.Run("entity.paste", { { "data", copied["data"] }, { "parent", group } })["entities"][0].get<std::string>();
		CHECK(harness.NameOf(pasted) == "Crate (1)");
		const std::string elsewhere = harness.Create("Shelf");
		const std::string alone = harness.Run("entity.paste", { { "data", copied["data"] }, { "parent", elsewhere } })["entities"][0].get<std::string>();
		CHECK(harness.NameOf(alone) == "Crate");

		// Renaming frees a number again.
		harness.Run("entity.rename", { { "entity", first }, { "name", "Barrel" } });
		CHECK(harness.NameOf(harness.Run("entity.duplicate", { { "entities", { crate } } })["id"].get<std::string>()) == "Crate (1)");
		// Undo takes the copy away with its name.
		harness.Run("edit.undo");
		CHECK_FALSE(harness.Context.GetEditScene()->HasChildNamed(Entity(), "Crate (1)", Entity()));
	}

	TEST_CASE("Numbering many copies of one name costs checks in proportion to the copies")
	{
		NameHarness harness;
		constexpr size_t c_Count = 1000;
		std::vector<std::string> crates;
		for (size_t index = 0; index < c_Count; index++)
			crates.push_back(harness.Create("Crate"));
		const Scene& scene = *harness.Context.GetEditScene();
		REQUIRE(scene.HasChildNamed(Entity(), "Crate", Entity())); // Builds the name index first
		const uint64_t visits = scene.GetLookupVisitCount();
		const nlohmann::json copies = harness.Run("entity.duplicate", { { "entities", crates } })["entities"];
		REQUIRE(copies.size() == c_Count);
		CHECK(harness.NameOf(copies[0].get<std::string>()) == "Crate (1)");
		CHECK(harness.NameOf(copies[c_Count - 1].get<std::string>()) == fmt::format("Crate ({})", c_Count));
		// Each copy looks at the entities named "Crate" once and at one numbered name: no search from 1 for every copy.
		CHECK(scene.GetLookupVisitCount() - visits <= c_Count * 4);
		CHECK(scene.GetLookupVisitCount() - visits >= c_Count);
	}

	TEST_CASE("Prefab instances among siblings of the same name get a number")
	{
		NameHarness harness;
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(CreateTemporaryDirectory("NamesPrefab") / "Game") }, { "name", "Game" } });
		const std::string crate = harness.Create("Crate");
		harness.Run("prefab.create", { { "entities", { crate } }, { "path", "Prefabs/Crate.stprefab" } });
		const std::string instance = harness.Run("prefab.instantiate", { { "prefab", "Prefabs/Crate.stprefab" } })["entities"][0].get<std::string>();
		CHECK(harness.NameOf(instance) == "Crate (1)");
		CHECK(harness.LastStep() == "Instantiate 'Crate (1)'");
	}

	TEST_CASE("Undo steps say what they changed, and edit.history lists them")
	{
		NameHarness harness;
		const std::string wall = harness.Create("Wall");
		CHECK(harness.LastStep() == "Create 'Wall'");
		const std::string ground = harness.Create("Ground");
		harness.Run("component.add", { { "entity", ground }, { "component", "MeshRenderer" } });
		CHECK(harness.LastStep() == "Add Mesh Renderer to 'Ground'");
		harness.Run("component.set", { { "entity", ground }, { "component", "MeshRenderer" }, { "values", { { "Mesh", "Builtin/Cube" } } } });
		CHECK(harness.LastStep() == "Set Mesh of 'Ground'");
		harness.Run("component.add", { { "entities", { wall, ground } }, { "component", "PointLight" } });
		CHECK(harness.LastStep() == "Add Point Light to 2 entities");
		harness.Run("component.set", { { "entities", { wall, ground } }, { "component", "PointLight" }, { "values", { { "Intensity", 3 }, { "Color", { 1, 0, 0 } } } } });
		CHECK(harness.LastStep() == "Set Color and Intensity of 2 entities"); // In the order of their names
		harness.Run("component.set", { { "entity", wall }, { "component", "PointLight" }, { "values", { { "Intensity", 2 }, { "Color", { 1, 1, 0 } }, { "Range", 4 } } } });
		CHECK(harness.LastStep() == "Edit Point Light of 'Wall'");
		harness.Run("component.remove", { { "entity", wall }, { "component", "PointLight" } });
		CHECK(harness.LastStep() == "Remove Point Light from 'Wall'");
		harness.Run("entity.rename", { { "entity", wall }, { "name", "North Wall" } });
		CHECK(harness.LastStep() == "Rename 'Wall' to 'North Wall'");
		harness.Run("entity.setParent", { { "entities", { wall } }, { "parent", ground } });
		CHECK(harness.LastStep() == "Move 'North Wall' under 'Ground'");
		harness.Run("entity.setParent", { { "entities", { wall } }, { "parent", nullptr } });
		CHECK(harness.LastStep() == "Move 'North Wall' to the top level");
		harness.Run("entity.setActive", { { "entities", { wall } }, { "active", false } });
		CHECK(harness.LastStep() == "Deactivate 'North Wall'");
		harness.Run("entity.duplicate", { { "entities", { wall, ground } } });
		CHECK(harness.LastStep() == "Duplicate 2 entities");
		const std::string third = harness.Create("Third");
		harness.Run("entity.delete", { { "entities", { wall, ground, third } } });
		CHECK(harness.LastStep() == "Delete 3 entities");

		const nlohmann::json history = harness.Run("edit.history")["history"];
		CHECK(history.size() >= 14);
		CHECK(history.back() == "Delete 3 entities");
		// The activity journal names the step of each command.
		CHECK(harness.Context.GetJournal().GetEntries().back().Command == "edit.history");
		const CommandJournal& journal = harness.Context.GetJournal();
		CHECK(journal.Find(journal.GetLatestId() - 1)->UndoStep == "Delete 3 entities");
	}
}

TEST_SUITE("Editor.UI.Names")
{
	TEST_CASE("Duplicating from the UI names the copy 'X (1)'")
	{
		HarnessEditor editor;
		editor.Layer->DismissLauncher();
		editor.Frames(2);
		const std::string crate = editor.Run("entity.create", { { "name", "Crate" } })["id"].get<std::string>();
		editor.Context().SetSelection({ *UUID::FromString(crate) });
		EditorActions::DuplicateSelection(editor.Context(), editor.Layer->GetCommands());
		editor.Frames(1);
		REQUIRE(editor.Context().GetSelection().size() == 1);
		CHECK(editor.Context().GetEditScene()->GetEntityByUUID(editor.Context().GetSelection()[0]).GetName() == "Crate (1)");
		CHECK(editor.Context().GetUndoStack().GetUndoName() == "Duplicate 'Crate'");
	}
}
