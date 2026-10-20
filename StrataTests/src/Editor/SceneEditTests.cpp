#include <doctest/doctest.h>

#include "Editor/SceneEdit.h"

#include <Strata/Renderer/RendererComponents.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>
#include <Strata/Scene/UnknownComponents.h>

using namespace Strata;

namespace
{

	nlohmann::json Snapshot(const Scene& scene)
	{
		return SceneSerializer::Serialize(scene)["Scene"]["Entities"];
	}

	// Root with three children; the middle child has a grandchild.
	struct TestHierarchy
	{
		Scene SceneData;
		Entity Root, First, Middle, Last, Grandchild;

		TestHierarchy()
		{
			Root = SceneData.CreateEntity("Root");
			First = SceneData.CreateChildEntity(Root, "First");
			Middle = SceneData.CreateChildEntity(Root, "Middle");
			Last = SceneData.CreateChildEntity(Root, "Last");
			Grandchild = SceneData.CreateChildEntity(Middle, "Grandchild");
			Middle.GetComponent<TransformComponent>().Translation = { 1.0f, 2.0f, 3.0f };
			Middle.AddComponent<PointLightComponent>().Intensity = 7.0f;
			SceneData.CreateEntity("Other");
		}
	};

}

TEST_SUITE("Editor.SceneEdit")
{
	TEST_CASE("Deleting a subtree is undone with UUIDs, components and sibling order intact")
	{
		TestHierarchy hierarchy;
		Scene& scene = hierarchy.SceneData;
		const nlohmann::json original = Snapshot(scene);
		const UUID middle = hierarchy.Middle.GetUUID();
		const UUID grandchild = hierarchy.Grandchild.GetUUID();

		UndoStack undo;
		SceneEditTransaction transaction(scene, "Delete", {});
		transaction.TrackSubtree(middle);
		scene.DestroyEntity(hierarchy.Middle);
		CHECK(transaction.Commit(undo));
		CHECK_FALSE(scene.GetEntityByUUID(grandchild));

		CHECK(undo.Undo());
		CHECK(Snapshot(scene) == original);
		Entity restored = scene.GetEntityByUUID(middle);
		REQUIRE(restored);
		CHECK(restored.GetParent() == scene.GetEntityByUUID(hierarchy.Root.GetUUID()));
		CHECK(restored.GetComponent<PointLightComponent>().Intensity == 7.0f);
		CHECK(scene.GetEntityByUUID(grandchild).GetParent() == restored);

		CHECK(undo.Redo());
		CHECK_FALSE(scene.GetEntityByUUID(middle));
		CHECK_FALSE(scene.GetEntityByUUID(grandchild));
		CHECK(undo.Undo());
		CHECK(Snapshot(scene) == original);
	}

	TEST_CASE("Created entities disappear on undo and return with the same UUID on redo")
	{
		TestHierarchy hierarchy;
		Scene& scene = hierarchy.SceneData;
		const nlohmann::json original = Snapshot(scene);

		UndoStack undo;
		SceneEditTransaction transaction(scene, "Create", {});
		Entity created = scene.CreateChildEntity(hierarchy.Root, "New");
		created.AddComponent<SpotLightComponent>();
		transaction.TrackCreated(created.GetUUID());
		const UUID id = created.GetUUID();
		REQUIRE(transaction.Commit(undo));
		const nlohmann::json withEntity = Snapshot(scene);

		undo.Undo();
		CHECK(Snapshot(scene) == original);
		undo.Redo();
		CHECK(Snapshot(scene) == withEntity);
		CHECK(scene.GetEntityByUUID(id).HasComponent<SpotLightComponent>());
	}

	TEST_CASE("Component edits, additions and removals are restored exactly")
	{
		TestHierarchy hierarchy;
		Scene& scene = hierarchy.SceneData;
		const nlohmann::json original = Snapshot(scene);

		UndoStack undo;
		SceneEditTransaction transaction(scene, "Edit", { hierarchy.Middle.GetUUID(), hierarchy.First.GetUUID() });
		hierarchy.Middle.GetComponent<TransformComponent>().Scale = glm::vec3(4.0f);
		hierarchy.Middle.RemoveComponent<PointLightComponent>();
		hierarchy.First.AddComponent<CameraComponent>().PerspectiveFOV = 30.0f;
		REQUIRE(transaction.Commit(undo));
		const nlohmann::json edited = Snapshot(scene);

		undo.Undo();
		CHECK(Snapshot(scene) == original);
		CHECK_FALSE(hierarchy.First.HasComponent<CameraComponent>());
		undo.Redo();
		CHECK(Snapshot(scene) == edited);
	}

	TEST_CASE("Reparenting is undone even when the old parent moved below the child")
	{
		TestHierarchy hierarchy;
		Scene& scene = hierarchy.SceneData;
		const nlohmann::json original = Snapshot(scene);

		// Swap: Middle becomes a root, then Root moves under Middle's former child.
		UndoStack undo;
		SceneEditTransaction transaction(scene, "Swap", { hierarchy.Root.GetUUID(), hierarchy.Middle.GetUUID() });
		REQUIRE(scene.SetParent(hierarchy.Middle, Entity(), false));
		REQUIRE(scene.SetParent(hierarchy.Root, hierarchy.Grandchild, false));
		REQUIRE(transaction.Commit(undo));
		const nlohmann::json swapped = Snapshot(scene);

		undo.Undo();
		CHECK(Snapshot(scene) == original);
		undo.Redo();
		CHECK(Snapshot(scene) == swapped);
	}

	TEST_CASE("Sibling orders are restored also where recorded positions agree by chance")
	{
		Scene scene;
		std::vector<Entity> roots;
		for (const char* name : { "A", "B", "C", "D" })
			roots.push_back(scene.CreateEntity(name));
		const nlohmann::json original = Snapshot(scene);
		auto names = [&scene]()
		{
			std::vector<std::string> result;
			for (UUID id : scene.GetRootEntities())
				result.push_back(scene.GetEntityByUUID(id).GetName());
			return result;
		};

		// D and B move to the front: B keeps position 1, but it now comes before A.
		UndoStack undo;
		SceneEditTransaction transaction(scene, "Reorder", { roots[3].GetUUID(), roots[1].GetUUID() });
		const std::vector<Scene::EntityPlacement> placements = { { roots[3].GetUUID(), UUID::Null(), 0 }, { roots[1].GetUUID(), UUID::Null(), 1 } };
		REQUIRE(scene.PlaceEntities(placements));
		REQUIRE(transaction.Commit(undo));
		CHECK(names() == std::vector<std::string> { "D", "B", "A", "C" });
		REQUIRE(undo.Undo());
		CHECK(names() == std::vector<std::string> { "A", "B", "C", "D" });
		CHECK(Snapshot(scene) == original);

		// Recreated entities are appended first: C's copy lands at its recorded position 4 by chance, before A's copy is
		// inserted in front of it.
		SceneEditTransaction duplicate(scene, "Duplicate", {});
		const Entity copyOfC = scene.DuplicateEntity(roots[2]);
		const Entity copyOfA = scene.DuplicateEntity(roots[0]);
		duplicate.TrackCreated(copyOfC.GetUUID());
		duplicate.TrackCreated(copyOfA.GetUUID());
		REQUIRE(duplicate.Commit(undo));
		CHECK(names() == std::vector<std::string> { "A", "A", "B", "C", "C", "D" });
		const nlohmann::json duplicated = Snapshot(scene);
		REQUIRE(undo.Undo());
		REQUIRE(undo.Redo());
		CHECK(Snapshot(scene) == duplicated);
		std::string error;
		CHECK_MESSAGE(scene.ValidateHierarchy(&error), error);
	}

	TEST_CASE("Edits without changes record nothing and continuous edits merge")
	{
		TestHierarchy hierarchy;
		Scene& scene = hierarchy.SceneData;
		UndoStack undo;

		SceneEditTransaction unchanged(scene, "Nothing", { hierarchy.Middle.GetUUID() });
		CHECK_FALSE(unchanged.Commit(undo));
		CHECK_FALSE(undo.CanUndo());

		// Dragging a value: one undo step back to the value before the drag.
		for (float x = 1.0f; x <= 5.0f; x += 1.0f)
		{
			SceneEditTransaction drag(scene, "Move", { hierarchy.Middle.GetUUID() }, "Move/Middle");
			hierarchy.Middle.GetComponent<TransformComponent>().Translation.x = 10.0f * x;
			drag.Commit(undo);
		}
		CHECK(undo.GetHistory().size() == 1);
		undo.Undo();
		CHECK(scene.GetEntityByUUID(hierarchy.Middle.GetUUID()).GetComponent<TransformComponent>().Translation.x == 1.0f);
	}

	TEST_CASE("Rolling back a failed edit restores the previous state")
	{
		TestHierarchy hierarchy;
		Scene& scene = hierarchy.SceneData;
		const nlohmann::json original = Snapshot(scene);
		SceneEditTransaction transaction(scene, "Failed", { hierarchy.Middle.GetUUID() });
		hierarchy.Middle.GetComponent<TransformComponent>().Translation = glm::vec3(100.0f);
		Entity created = scene.CreateEntity("Temporary");
		transaction.TrackCreated(created.GetUUID());
		transaction.Rollback();
		CHECK(Snapshot(scene) == original);
	}

	TEST_CASE("Undo snapshots carry the components this build does not register")
	{
		TestHierarchy hierarchy;
		Scene& scene = hierarchy.SceneData;
		const nlohmann::json vehicle = { { "Wheels", 6 }, { "Engine", { { "Power", 420.0 } } } };
		hierarchy.Middle.AddComponent<UnknownComponentsComponent>(UnknownComponentsComponent { { { "Vehicle", vehicle } } });
		hierarchy.Grandchild.AddComponent<UnknownComponentsComponent>(UnknownComponentsComponent { { { "Tire", { { "Pressure", 2.2 } } } } });
		const nlohmann::json original = Snapshot(scene);

		// Captured with the entity's other components.
		const EntityState state = SceneEdit::CaptureEntity(scene, hierarchy.Middle.GetUUID());
		CHECK(state.Components["Vehicle"] == vehicle);
		CHECK(state.Components.contains("Transform"));

		// Deleting the subtree and undoing it recreates the kept components.
		UndoStack undo;
		SceneEditTransaction deletion(scene, "Delete", {});
		deletion.TrackSubtree(hierarchy.Middle.GetUUID());
		scene.DestroyEntity(hierarchy.Middle);
		REQUIRE(deletion.Commit(undo));
		CHECK(undo.Undo());
		CHECK(Snapshot(scene) == original);
		Entity restored = scene.GetEntityByUUID(state.ID);
		REQUIRE(restored);
		CHECK(restored.GetComponent<UnknownComponentsComponent>().Components["Vehicle"] == vehicle);

		// Restoring an existing entity puts back what it kept, and takes away what the state does not have.
		SceneEditTransaction edit(scene, "Edit", { state.ID, hierarchy.First.GetUUID() });
		restored.RemoveComponent<UnknownComponentsComponent>();
		hierarchy.First.AddComponent<UnknownComponentsComponent>(UnknownComponentsComponent { { { "Stray", 1 } } });
		REQUIRE(edit.Commit(undo));
		CHECK_FALSE(restored.HasComponent<UnknownComponentsComponent>());
		CHECK(undo.Undo());
		CHECK(Snapshot(scene) == original);
		REQUIRE(restored.HasComponent<UnknownComponentsComponent>());
		CHECK(restored.GetComponent<UnknownComponentsComponent>().Components["Vehicle"] == vehicle);
		CHECK_FALSE(hierarchy.First.HasComponent<UnknownComponentsComponent>());
	}
}

namespace
{

	// The cell loader an open world offers: records what undo steps asked it to load.
	class RecordingCellLoader final : public ISceneCellLoader
	{
	public:
		std::string DescribeCell(AssetHandle cell) const override { return "Props (" + cell.ToString() + ")"; }
		void RequestCell(AssetHandle cell) override { Requested.push_back(cell); }

		std::vector<AssetHandle> Requested;
	};

}

TEST_SUITE("Editor.SceneEdit")
{
	TEST_CASE("Entity states record the cell of their top-level entity, and undo puts entities back into their cells")
	{
		Scene scene("World");
		Entity crate = scene.CreateEntity("Crate");
		Entity lid = scene.CreateChildEntity(crate, "Lid");
		Entity tree = scene.CreateEntity("Tree");
		const AssetHandle cellA = UUID(0xA0);
		const AssetHandle cellB = UUID(0xB0);
		const Entity rootsA[] = { crate };
		const SubSceneID a = scene.AdoptSubScene(cellA, rootsA);
		const SubSceneID b = scene.AdoptSubScene(cellB, {});
		REQUIRE((a != 0 && b != 0));

		CHECK(SceneEdit::CaptureEntity(scene, crate.GetUUID()).Cell == cellA);
		CHECK(SceneEdit::CaptureEntity(scene, lid.GetUUID()).Cell == cellA); // Descendants follow their root
		CHECK(SceneEdit::CaptureEntity(scene, tree.GetUUID()).Cell == UUID::Null()); // The persistent scene

		// A move from cell A to cell B is one step; undo and redo move it back and forth.
		UndoStack undo;
		SceneEditTransaction transaction(scene, "Move 'Crate'", { crate.GetUUID() });
		REQUIRE(scene.SetSubSceneOf(crate, b));
		REQUIRE(transaction.Commit(undo));
		CHECK(scene.GetSubSceneOf(lid) == b);
		REQUIRE(undo.Undo());
		CHECK(scene.GetSubSceneOf(crate) == a);
		CHECK(scene.GetSubSceneOf(lid) == a);
		REQUIRE(undo.Redo());
		CHECK(scene.GetSubSceneOf(crate) == b);

		// A deleted entity comes back into its cell.
		const SubSceneID treeCell = scene.AdoptSubScene(UUID(0xC0), {});
		REQUIRE(scene.SetSubSceneOf(tree, treeCell));
		const UUID treeID = tree.GetUUID();
		SceneEditTransaction deletion(scene, "Delete", {});
		deletion.Track(treeID);
		scene.DestroyEntity(tree);
		REQUIRE(deletion.Commit(undo));
		REQUIRE(undo.Undo());
		const Entity restored = scene.GetEntityByUUID(treeID);
		REQUIRE(restored);
		CHECK(scene.GetSubSceneOf(restored) == treeCell);
		CHECK(scene.ValidateHierarchy());
	}

	TEST_CASE("A step whose cell is not loaded fails, asks the world to load it and stays where it is")
	{
		Scene scene("World");
		Entity crate = scene.CreateEntity("Crate");
		const AssetHandle cellA = UUID(0xA0);
		const Entity roots[] = { crate };
		const SubSceneID a = scene.AdoptSubScene(cellA, roots);
		UndoStack undo;
		SceneEditTransaction transaction(scene, "Move 'Crate'", { crate.GetUUID() });
		REQUIRE(scene.SetSubSceneOf(crate, 0));
		REQUIRE(transaction.Commit(undo));

		// Cell A unloads (its entities go with it, but the crate left it): undoing needs A again.
		REQUIRE(scene.UnloadSubScene(a));
		scene.FlushSubSceneUnloads();
		RecordingCellLoader loader;
		REQUIRE(scene.RegisterService<ISceneCellLoader>(loader));
		std::string error;
		CHECK_FALSE(undo.Undo(&error));
		CHECK(error.find("the step changes cell Props (00000000000000A0), which is not loaded; it is loading now") != std::string::npos);
		CHECK(loader.Requested == std::vector<AssetHandle> { cellA });
		CHECK(undo.CanUndo()); // The step stays applied
		CHECK(scene.GetSubSceneOf(crate) == 0);

		// Once the cell is in again, the undo goes through.
		const SubSceneID again = scene.AdoptSubScene(cellA, {});
		REQUIRE(undo.Undo(&error));
		CHECK(scene.GetSubSceneOf(crate) == again);
		scene.UnregisterService<ISceneCellLoader>(loader);
	}

	TEST_CASE("Steps on a cell's descendants, creations and deletions wait for the cell too")
	{
		Scene scene("World");
		const AssetHandle cellA = UUID(0xA0);
		Entity crate = scene.CreateEntity("Crate");
		Entity lid = scene.CreateChildEntity(crate, "Lid");
		const UUID crateID = crate.GetUUID();
		const UUID lidID = lid.GetUUID();
		const Entity roots[] = { crate };
		const SubSceneID a = scene.AdoptSubScene(cellA, roots);
		REQUIRE(a != 0);
		RecordingCellLoader loader;
		REQUIRE(scene.RegisterService<ISceneCellLoader>(loader));
		UndoStack undo;

		// An edit of the lid only: its state names the crate's cell.
		SceneEditTransaction rename(scene, "Rename 'Lid'", { lidID });
		lid.GetComponent<NameComponent>().Name = "Top";
		lid.MarkModified<NameComponent>();
		REQUIRE(rename.Commit(undo));
		// A creation in the cell: the state before it (no entity) names the cell it went into.
		SceneEditTransaction creation(scene, "Create 'Nail'", {});
		Entity nail = scene.CreateChildEntity(crate, "Nail");
		const UUID nailID = nail.GetUUID();
		creation.TrackCreated(nailID);
		REQUIRE(creation.Commit(undo));
		const SceneEditAction* created = dynamic_cast<const SceneEditAction*>(undo.GetLatestAction());
		REQUIRE(created);
		CHECK_FALSE(created->GetBefore()[0].Exists);
		CHECK(created->GetBefore()[0].Cell == cellA);

		// The cell unloads with all three. Undoing must not recreate the nail's absence or the lid outside the cell (they would
		// come back with the cell, twice for the lid): it waits for the cell.
		REQUIRE(scene.UnloadSubScene(a));
		scene.FlushSubSceneUnloads();
		REQUIRE_FALSE(scene.GetEntityByUUID(lidID));
		std::string error;
		CHECK_FALSE(undo.Undo(&error));
		CHECK(loader.Requested == std::vector<AssetHandle> { cellA });
		CHECK(error.find("cell Props (00000000000000A0)") != std::string::npos);
		CHECK(scene.GetEntityCount() == 0);

		// The cell is back (as its document holds it, with the nail): both steps undo, and the nail goes again.
		Entity reloaded = scene.CreateEntityWithUUID(crateID, "Crate");
		Entity reloadedLid = scene.CreateEntityWithUUID(lidID, "Top");
		REQUIRE(scene.SetParent(reloadedLid, reloaded));
		REQUIRE(scene.SetParent(scene.CreateEntityWithUUID(nailID, "Nail"), reloaded));
		const Entity reloadedRoots[] = { reloaded };
		REQUIRE(scene.AdoptSubScene(cellA, reloadedRoots) != 0);
		REQUIRE(undo.Undo(&error));
		CHECK_FALSE(scene.GetEntityByUUID(nailID));
		REQUIRE(undo.Undo(&error));
		CHECK(reloadedLid.GetComponent<NameComponent>().Name == "Lid");

		// Deleting the lid, then losing the cell: redoing the deletion waits for the cell as well.
		SceneEditTransaction deletion(scene, "Delete 'Lid'", {});
		deletion.Track(lidID);
		scene.DestroyEntity(reloadedLid);
		REQUIRE(deletion.Commit(undo));
		const SceneEditAction* deleted = dynamic_cast<const SceneEditAction*>(undo.GetLatestAction());
		REQUIRE(deleted);
		CHECK(deleted->GetAfter()[0].Cell == cellA);
		REQUIRE(undo.Undo(&error));
		REQUIRE(scene.UnloadSubScene(scene.FindSubSceneBySource(cellA)));
		scene.FlushSubSceneUnloads();
		loader.Requested.clear();
		CHECK_FALSE(undo.Redo(&error));
		CHECK(loader.Requested == std::vector<AssetHandle> { cellA });
		scene.UnregisterService<ISceneCellLoader>(loader);
	}
}
