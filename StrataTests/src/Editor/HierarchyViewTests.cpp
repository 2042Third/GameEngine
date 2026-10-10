#include <doctest/doctest.h>

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HierarchyView.h"
#include "Perf/SceneGenerators.h"

#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>

#include <string>
#include <vector>

using namespace Strata;

namespace
{

	// Root1 > [Lamp (point light, tag "Lights"), Box > [Bulb (point light)]], Root2 > [Crate (tag "Props")], Camera.
	struct FilterScene
	{
		Ref<Scene> SceneData = CreateRef<Scene>("Filter");
		Entity Root1, Lamp, Box, Bulb, Root2, Crate, Camera;

		FilterScene()
		{
			Scene& scene = *SceneData;
			Root1 = scene.CreateEntity("Root1");
			Lamp = scene.CreateChildEntity(Root1, "Lamp");
			Lamp.AddComponent<PointLightComponent>();
			Lamp.AddComponent<TagComponent>("Lights");
			Box = scene.CreateChildEntity(Root1, "Box");
			Bulb = scene.CreateChildEntity(Box, "Bulb");
			Bulb.AddComponent<PointLightComponent>();
			Root2 = scene.CreateEntity("Root2");
			Crate = scene.CreateChildEntity(Root2, "Crate");
			Crate.AddComponent<TagComponent>("Props");
			Camera = scene.CreateEntity("Camera");
			Camera.AddComponent<CameraComponent>();
		}
	};

	std::vector<std::string> RowNames(const HierarchyView& view, const Scene& scene)
	{
		std::vector<std::string> names;
		for (const HierarchyRow& row : view.GetRows())
			names.push_back(scene.GetEntityByUUID(row.ID).GetName());
		return names;
	}

	std::vector<std::string> RootNames(const Scene& scene)
	{
		std::vector<std::string> names;
		for (UUID id : scene.GetRootEntities())
			names.push_back(scene.GetEntityByUUID(id).GetName());
		return names;
	}

}

TEST_SUITE("Editor.HierarchyView")
{
	TEST_CASE("Rows follow the hierarchy and the expansion, and are rebuilt only when what they show changed")
	{
		FilterScene data;
		Scene& scene = *data.SceneData;
		HierarchyView view;
		CHECK(view.Update(data.SceneData));
		CHECK(RowNames(view, scene) == std::vector<std::string> { "Root1", "Root2", "Camera" });
		CHECK(view.GetRows()[0].HasChildren());
		CHECK_FALSE(view.GetRows()[0].IsExpanded());
		CHECK_FALSE(view.GetRows()[2].HasChildren());
		const uint64_t built = view.GetRebuildCount();

		// Unchanged frames, and changes the rows do not show (names are drawn, not kept), rebuild nothing.
		for (int frame = 0; frame < 10; frame++)
			CHECK_FALSE(view.Update(data.SceneData));
		data.Lamp.GetComponent<NameComponent>().Name = "Lantern";
		data.Lamp.MarkModified<NameComponent>();
		data.Crate.GetComponent<TransformComponent>().Translation.x = 4.0f;
		data.Crate.MarkModified<TransformComponent>();
		CHECK_FALSE(view.Update(data.SceneData));
		CHECK(view.GetRebuildCount() == built);

		view.SetExpanded(data.Root1.GetUUID(), true);
		view.SetExpanded(data.Box.GetUUID(), true);
		CHECK(view.Update(data.SceneData));
		CHECK(RowNames(view, scene) == std::vector<std::string> { "Root1", "Lantern", "Box", "Bulb", "Root2", "Camera" });
		CHECK(view.GetRows()[3].Depth == 2);
		CHECK(view.GetRows()[0].IsExpanded());
		CHECK(view.FindRow(data.Bulb.GetUUID()) == 3u);
		CHECK_FALSE(view.FindRow(data.Crate.GetUUID()).has_value());
		CHECK(view.GetRowRange(5, 3) == std::vector<UUID> { data.Bulb.GetUUID(), data.Root2.GetUUID(), data.Camera.GetUUID() });
		CHECK(view.GetRowRange(4, 99).size() == 2);

		// Hierarchy changes (new entities, moves) rebuild.
		Entity added = scene.CreateChildEntity(data.Box, "Added");
		CHECK(view.Update(data.SceneData));
		CHECK(RowNames(view, scene)[4] == "Added");
		scene.SetSiblingIndex(data.Camera, 0);
		CHECK(view.Update(data.SceneData));
		CHECK(RowNames(view, scene).front() == "Camera");
		scene.DestroyEntity(added);
		CHECK(view.Update(data.SceneData));

		// Collapsing a subtree, or everything.
		view.SetSubtreeExpanded(scene, data.Root1.GetUUID(), false);
		view.Update(data.SceneData);
		CHECK(RowNames(view, scene) == std::vector<std::string> { "Camera", "Root1", "Root2" });
		view.SetSubtreeExpanded(scene, data.Root1.GetUUID(), true);
		view.Update(data.SceneData);
		CHECK(view.GetRows().size() == 6);
		CHECK(view.IsExpanded(data.Box.GetUUID()));
		view.CollapseAll();
		view.Update(data.SceneData);
		CHECK(view.GetRows().size() == 3);

		// Another scene (play mode's copy, a scene opened) rebuilds; so does a scene that replaced a destroyed one.
		Ref<Scene> copy = Scene::Copy(data.SceneData);
		CHECK(view.Update(copy));
		CHECK(view.GetRows().size() == 3);
		CHECK(view.GetRows()[0].Handle == copy->GetEntityByUUID(view.GetRows()[0].ID).GetHandle());
		copy.reset();
		Ref<Scene> empty = CreateRef<Scene>();
		CHECK(view.Update(empty));
		CHECK(view.GetRows().empty());
		CHECK(view.Update(nullptr) == false);
	}

	TEST_CASE("With 100,000 top-level entities, unchanged frames rebuild no rows")
	{
		Ref<Scene> scene = CreateRef<Scene>("Wide");
		std::vector<Entity> roots = Tests::Perf::CreateFlatRoots(*scene, 100'000);
		HierarchyView view;
		CHECK(view.Update(scene));
		CHECK(view.GetRows().size() == 100'000);
		CHECK(view.GetRebuildCount() == 1);
		for (int frame = 0; frame < 1000; frame++)
			view.Update(scene);
		CHECK(view.GetRebuildCount() == 1);
		CHECK(view.FindRow(roots[77'777].GetUUID()) == 77'777u);

		// A rename shows in the drawn name only; a filter rebuilds, and while it is on, renames rebuild once.
		roots[5].GetComponent<NameComponent>().Name = "Needle";
		roots[5].MarkModified<NameComponent>();
		view.Update(scene);
		CHECK(view.GetRebuildCount() == 1);
		view.SetFilter("needle");
		view.Update(scene);
		CHECK(view.GetRows().size() == 1);
		CHECK(view.GetRebuildCount() == 2);
		for (int frame = 0; frame < 100; frame++)
			view.Update(scene);
		CHECK(view.GetRebuildCount() == 2);
		roots[9].GetComponent<NameComponent>().Name = "Needle too";
		roots[9].MarkModified<NameComponent>();
		view.Update(scene);
		view.Update(scene);
		CHECK(view.GetRebuildCount() == 3);
		CHECK(view.GetRows().size() == 2);
	}

	TEST_CASE("Filters show the matches with their ancestors, by name, component and tag")
	{
		FilterScene data;
		Scene& scene = *data.SceneData;
		HierarchyView view;

		view.SetFilter("BULB");
		view.Update(data.SceneData);
		CHECK(RowNames(view, scene) == std::vector<std::string> { "Root1", "Box", "Bulb" });
		CHECK(view.GetRows()[0].IsExpanded());
		CHECK_FALSE(view.GetRows()[0].IsMatch());
		CHECK(view.GetRows()[2].IsMatch());
		CHECK(view.GetRows()[2].Depth == 2);

		view.SetFilter("c:light");
		view.Update(data.SceneData);
		CHECK(RowNames(view, scene) == std::vector<std::string> { "Root1", "Lamp", "Box", "Bulb" });
		view.SetFilter("c:PointLight");
		view.Update(data.SceneData);
		CHECK(RowNames(view, scene) == std::vector<std::string> { "Root1", "Lamp", "Box", "Bulb" });
		view.SetFilter("t:light");
		view.Update(data.SceneData);
		CHECK(RowNames(view, scene) == std::vector<std::string> { "Root1", "Lamp" });
		view.SetFilter("t:Props c:transform");
		view.Update(data.SceneData);
		CHECK(RowNames(view, scene) == std::vector<std::string> { "Root2", "Crate" });
		view.SetFilter("c:camera crate");
		view.Update(data.SceneData);
		CHECK(view.GetRows().empty());
		view.SetFilter("  c:  ");
		CHECK_FALSE(view.IsFiltering()); // A prefix alone asks for nothing yet
		view.SetFilter("c:spaceship");
		view.Update(data.SceneData);
		CHECK(view.GetUnknownComponents() == std::vector<std::string> { "spaceship" });
		CHECK(view.GetRows().empty());

		// While filtering, changes of what the filter reads show at the next update.
		view.SetFilter("c:light");
		view.Update(data.SceneData);
		data.Crate.AddComponent<SpotLightComponent>();
		CHECK(view.Update(data.SceneData));
		CHECK(RowNames(view, scene) == std::vector<std::string> { "Root1", "Lamp", "Box", "Bulb", "Root2", "Crate" });
		data.Lamp.RemoveComponent<PointLightComponent>();
		CHECK(view.Update(data.SceneData));
		CHECK(RowNames(view, scene) == std::vector<std::string> { "Root1", "Box", "Bulb", "Root2", "Crate" });
		view.SetFilter("t:light");
		view.Update(data.SceneData);
		data.Lamp.GetComponent<TagComponent>().Tag = "Dark";
		data.Lamp.MarkModified<TagComponent>();
		CHECK(view.Update(data.SceneData));
		CHECK(view.GetRows().empty());
		// Expansion does not change what a filter shows.
		view.SetFilter("bulb");
		view.Update(data.SceneData);
		view.SetExpanded(data.Root1.GetUUID(), true);
		CHECK_FALSE(view.Update(data.SceneData));

		// Clearing the filter returns to the tree with its expansion.
		view.SetFilter("");
		view.Update(data.SceneData);
		CHECK(RowNames(view, scene) == std::vector<std::string> { "Root1", "Lamp", "Box", "Root2", "Camera" });
		CHECK(HierarchyFilter::Parse("Red c:Mesh t:Enemy box").Words == std::vector<std::string> { "red", "box" });
	}

	TEST_CASE("Revealing an entity 50 levels deep expands its ancestors and returns its row")
	{
		Ref<Scene> scene = CreateRef<Scene>("Deep");
		const UUID before = scene->CreateEntity("Before").GetUUID();
		Entity current = scene->CreateEntity("Level0");
		std::vector<UUID> chain = { current.GetUUID() };
		for (int level = 1; level <= 50; level++)
		{
			current = scene->CreateChildEntity(current, "Level" + std::to_string(level));
			chain.push_back(current.GetUUID());
		}
		scene->CreateEntity("After");
		HierarchyView view;
		view.Update(scene);
		CHECK(view.GetRows().size() == 3);

		const HierarchyView::RevealResult revealed = view.Reveal(scene, chain.back());
		CHECK(revealed.Expanded == std::vector<UUID>(chain.begin(), chain.end() - 1));
		REQUIRE(revealed.Row.has_value());
		CHECK(*revealed.Row == 51u);
		CHECK(view.GetRows()[51].ID == chain.back());
		CHECK(view.GetRows()[51].Depth == 50);
		CHECK(view.GetRows().size() == 53);

		// Revealing it again expands nothing; a missing entity has no row.
		CHECK(view.Reveal(scene, chain.back()).Expanded.empty());
		CHECK_FALSE(view.Reveal(scene, UUID(0x1234)).Row.has_value());
		// While filtering, an entity is revealed only when it is shown (a match or an ancestor of one).
		view.SetFilter("level50");
		CHECK(view.Reveal(scene, chain.back()).Row == 50u);
		CHECK(view.Reveal(scene, chain[3]).Row == 3u);
		CHECK_FALSE(view.Reveal(scene, before).Row.has_value());
	}

	TEST_CASE("Drop zones map to the parent and index entity.setParent takes")
	{
		CHECK(HierarchyView::GetDropZone(0.0f) == HierarchyDropZone::Before);
		CHECK(HierarchyView::GetDropZone(0.24f) == HierarchyDropZone::Before);
		CHECK(HierarchyView::GetDropZone(0.25f) == HierarchyDropZone::Inside);
		CHECK(HierarchyView::GetDropZone(0.5f) == HierarchyDropZone::Inside);
		CHECK(HierarchyView::GetDropZone(0.75f) == HierarchyDropZone::Inside);
		CHECK(HierarchyView::GetDropZone(0.76f) == HierarchyDropZone::After);
		CHECK(HierarchyView::GetDropZone(1.0f) == HierarchyDropZone::After);

		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		Scene& scene = *context.GetEditScene();
		Entity a = scene.CreateEntity("A");
		Entity b = scene.CreateEntity("B");
		Entity x = scene.CreateChildEntity(b, "X");
		Entity y = scene.CreateChildEntity(b, "Y");
		Entity c = scene.CreateEntity("C");
		HierarchyView view;
		view.SetExpanded(b.GetUUID(), true);
		view.Update(context.GetEditScene());
		REQUIRE(RowNames(view, scene) == std::vector<std::string> { "A", "B", "X", "Y", "C" });
		const std::vector<UUID> draggedC = { c.GetUUID() };
		const std::vector<UUID> draggedA = { a.GetUUID() };

		auto expect = [&](size_t row, float fraction, const std::vector<UUID>& dragged, UUID parent, size_t index)
		{
			const std::optional<HierarchyDropTarget> target = view.GetDropTarget(scene, row, fraction, dragged);
			REQUIRE(target.has_value());
			CHECK(target->Parent == parent);
			CHECK(target->Index == index);
		};
		expect(0, 0.1f, draggedC, UUID::Null(), 0);  // Before A
		expect(0, 0.5f, draggedC, a.GetUUID(), 0);   // Inside A (no children yet)
		expect(1, 0.5f, draggedC, b.GetUUID(), 2);   // Inside B: last
		expect(1, 0.9f, draggedC, b.GetUUID(), 0);   // After expanded B: in front of X
		expect(2, 0.9f, draggedC, b.GetUUID(), 1);   // After X
		expect(3, 0.1f, draggedC, b.GetUUID(), 1);   // Before Y
		expect(4, 0.1f, draggedA, UUID::Null(), 1);  // Before C, with A leaving the list in front of it
		expect(4, 0.9f, draggedA, UUID::Null(), 2);  // After C (last), A leaving

		// Impossible drops: into what is dragged or below it; no such row.
		CHECK_FALSE(view.GetDropTarget(scene, 2, 0.5f, std::vector<UUID> { b.GetUUID() }).has_value());
		CHECK_FALSE(view.GetDropTarget(scene, 1, 0.5f, std::vector<UUID> { b.GetUUID() }).has_value());
		CHECK_FALSE(view.GetDropTarget(scene, 9, 0.5f, draggedC).has_value());

		// The mapping does what the drop shows: A dropped on the line before C lands right before C.
		const std::optional<HierarchyDropTarget> beforeC = view.GetDropTarget(scene, 4, 0.1f, draggedA);
		REQUIRE(beforeC);
		const EditorCommandResult moved = commands.Execute(context, "entity.setParent", { { "entities", { UUIDToJson(a.GetUUID()) } },
			{ "parent", nullptr }, { "index", beforeC->Index } });
		REQUIRE(moved.Success);
		CHECK(RootNames(scene) == std::vector<std::string> { "B", "A", "C" });
		// X and Y (dragged in any order) dropped after C, the last row, become the last top-level entities in hierarchy order.
		view.Update(context.GetEditScene());
		const std::vector<UUID> children = HierarchyView::GetTopmostInHierarchyOrder(scene, std::vector<UUID> { y.GetUUID(), x.GetUUID() });
		CHECK(children == std::vector<UUID> { x.GetUUID(), y.GetUUID() });
		const std::optional<HierarchyDropTarget> afterC = view.GetDropTarget(scene, view.FindRow(c.GetUUID()).value(), 0.9f, children);
		REQUIRE(afterC);
		nlohmann::json ids = nlohmann::json::array();
		for (UUID id : children)
			ids.push_back(UUIDToJson(id));
		REQUIRE(commands.Execute(context, "entity.setParent", { { "entities", ids }, { "parent", nullptr }, { "index", afterC->Index } }).Success);
		CHECK(RootNames(scene) == std::vector<std::string> { "B", "A", "C", "X", "Y" });
	}

	TEST_CASE("The topmost entities of a selection come in hierarchy order")
	{
		FilterScene data;
		Scene& scene = *data.SceneData;
		const std::vector<UUID> picked = { data.Camera.GetUUID(), data.Bulb.GetUUID(), UUID(0x1234), data.Root1.GetUUID(), data.Crate.GetUUID(),
			data.Camera.GetUUID() };
		CHECK(HierarchyView::GetTopmostInHierarchyOrder(scene, picked) == std::vector<UUID> { data.Root1.GetUUID(), data.Crate.GetUUID(), data.Camera.GetUUID() });
		CHECK(HierarchyView::GetTopmostInHierarchyOrder(scene, std::vector<UUID>()).empty());
	}
}
