#include <doctest/doctest.h>

#include "Editor/SceneEdit.h"

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
