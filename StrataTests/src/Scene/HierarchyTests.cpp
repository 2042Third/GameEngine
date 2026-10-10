#include <doctest/doctest.h>

#include "Scene/SceneTestUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// The children of an entity as the RelationshipComponent lists them.
	std::vector<Entity> GetRelationshipChildren(const Scene& scene, Entity entity)
	{
		std::vector<Entity> children;
		for (UUID child : entity.GetComponent<RelationshipComponent>().Children)
			children.push_back(scene.GetEntityByUUID(child));
		return children;
	}

	// The hierarchy as (parent, position) per entity, to compare a scene against a recorded layout.
	struct Placement
	{
		UUID Parent = UUID::Null();
		size_t SiblingIndex = 0;

		bool operator==(const Placement& other) const = default;
	};

	std::unordered_map<UUID, Placement> RecordLayout(const Scene& scene)
	{
		std::unordered_map<UUID, Placement> layout;
		for (const Entity entity : scene.GetEntitiesInHierarchyOrder())
			layout[entity.GetUUID()] = Placement { entity.GetComponent<RelationshipComponent>().Parent, scene.GetSiblingIndex(entity) };
		return layout;
	}

	size_t CountLogMessages(uint64_t afterSequence, std::string_view text)
	{
		size_t count = 0;
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(afterSequence))
		{
			if (entry.Message.find(text) != std::string::npos)
				count++;
		}
		return count;
	}

	// Records, for every entity announced by OnEntityDestroying, whether the entity named "Watched" still existed.
	struct DestroyOrderSystem : public SceneSystem
	{
		explicit DestroyOrderSystem(Scene& scene)
			: TargetScene(scene)
		{
		}

		void OnEntityDestroying(const Entity& entity) override
		{
			Announced.push_back(entity.GetName());
			for (const std::string& name : { std::string("First"), std::string("Second") })
				AllListedAlive = AllListedAlive && TargetScene.FindEntityByName(name).IsValid();
		}

		Scene& TargetScene;
		static inline std::vector<std::string> Announced;
		static inline bool AllListedAlive = true;
	};

	// Destroys the entities in Doomed during its update (the scene defers that to the end of the frame), and records how
	// many of them still existed whenever one was announced.
	struct DeferredDestroySystem : public SceneSystem
	{
		explicit DeferredDestroySystem(Scene& scene)
			: TargetScene(scene)
		{
		}

		void OnUpdate(Timestep) override
		{
			for (const Entity entity : Doomed)
				TargetScene.DestroyEntity(entity);
		}

		void OnEntityDestroying(const Entity&) override
		{
			size_t alive = 0;
			for (const Entity entity : Doomed)
				alive += entity.IsValid() ? 1 : 0;
			MinimumAliveWhenAnnounced = std::min(MinimumAliveWhenAnnounced, alive);
			Announced++;
		}

		Scene& TargetScene;
		static inline std::vector<Entity> Doomed;
		static inline size_t MinimumAliveWhenAnnounced = SIZE_MAX;
		static inline size_t Announced = 0;
	};

	std::vector<entt::entity> GetMoves(const Scene& scene, uint64_t sinceVersion)
	{
		std::vector<entt::entity> moves;
		REQUIRE(scene.GetHierarchyMoves(sinceVersion, moves));
		std::sort(moves.begin(), moves.end());
		moves.erase(std::unique(moves.begin(), moves.end()), moves.end());
		return moves;
	}

	std::vector<entt::entity> Handles(std::vector<Entity> entities)
	{
		std::vector<entt::entity> handles;
		for (const Entity entity : entities)
			handles.push_back(entity.GetHandle());
		std::sort(handles.begin(), handles.end());
		return handles;
	}

}

TEST_SUITE("Scene.Hierarchy")
{
	TEST_CASE("Hierarchy links mirror the relationships through every structural operation")
	{
		Scene scene;
		Entity a = scene.CreateEntity("A");
		Entity b = scene.CreateEntity("B");
		Entity c = scene.CreateChildEntity(a, "C");
		Entity d = scene.CreateChildEntity(a, "D");
		Entity e = scene.CreateChildEntity(c, "E");
		CheckSceneCaches(scene);
		CHECK(a.GetChildren() == GetRelationshipChildren(scene, a));
		CHECK(scene.GetDepth(e) == 2);
		CHECK(e.GetParent() == c);

		CHECK(scene.SetParent(c, b));
		CHECK(scene.GetDepth(e) == 2);
		CHECK(scene.GetDepth(c) == 1);
		CHECK(scene.SetParent(b, d));
		CHECK(scene.GetDepth(e) == 4); // a > d > b > c > e
		CheckSceneCaches(scene);

		CHECK(scene.SetSiblingIndex(d, 0));
		CHECK(scene.SetParent(d, Entity()));
		CHECK(scene.SetSiblingIndex(d, 0));
		CHECK(scene.GetRootEntities() == std::vector<UUID> { d.GetUUID(), a.GetUUID() });
		CheckSceneCaches(scene);

		Entity copy = scene.DuplicateEntity(b);
		REQUIRE(copy);
		CHECK(d.GetChildren() == std::vector<Entity> { b, copy });
		CHECK(scene.GetDepth(copy.GetChildren()[0].GetChildren()[0]) == 3); // d > copy > c > e
		CheckSceneCaches(scene);

		scene.DestroyEntity(c);
		CHECK_FALSE(e.IsValid());
		CHECK(b.GetChildren().empty());
		CheckSceneCaches(scene);
		for (const Entity entity : scene.GetEntitiesInHierarchyOrder())
			CHECK(entity.GetChildren() == GetRelationshipChildren(scene, entity));
	}

	TEST_CASE("Sibling positions stay correct as sibling lists change")
	{
		Scene scene;
		std::vector<Entity> roots;
		for (int index = 0; index < 10; index++)
			roots.push_back(scene.CreateEntity("Root" + std::to_string(index)));
		for (size_t index = 0; index < roots.size(); index++)
			CHECK(scene.GetSiblingIndex(roots[index]) == index);

		// Appending, removing, moving and inserting: every position matches the root list after each change.
		auto checkRoots = [&]()
		{
			const std::vector<UUID>& ids = scene.GetRootEntities();
			for (size_t index = 0; index < ids.size(); index++)
				CHECK(scene.GetSiblingIndex(scene.GetEntityByUUID(ids[index])) == index);
			CheckSceneCaches(scene);
		};
		roots.push_back(scene.CreateEntity("Appended"));
		CHECK(scene.GetSiblingIndex(roots.back()) == 10);
		checkRoots();
		scene.DestroyEntity(roots[3]);
		checkRoots();
		CHECK(scene.SetSiblingIndex(roots[9], 0));
		checkRoots();
		CHECK(scene.SetSiblingIndex(roots[0], 100)); // Clamped to the end
		CHECK(scene.GetRootEntities().back() == roots[0].GetUUID());
		checkRoots();

		// Children of one parent.
		Entity parent = roots[5];
		std::vector<Entity> children;
		for (int index = 0; index < 5; index++)
			children.push_back(scene.CreateChildEntity(parent, "Child" + std::to_string(index)));
		CHECK(scene.SetSiblingIndex(children[4], 1));
		CHECK(scene.GetSiblingIndex(children[4]) == 1);
		CHECK(scene.GetSiblingIndex(children[1]) == 2);
		CHECK(scene.SetParent(children[2], Entity()));
		CHECK(scene.GetSiblingIndex(children[3]) == 3);
		CHECK(scene.GetSiblingIndex(Entity()) == 0);
		checkRoots();
	}

	TEST_CASE("Destroying several entities compacts each sibling list once")
	{
		Scene scene;
		std::vector<Entity> roots;
		for (int index = 0; index < 6; index++)
			roots.push_back(scene.CreateEntity("Root" + std::to_string(index)));
		Entity nested = scene.CreateChildEntity(roots[1], "Nested");
		Entity deeper = scene.CreateChildEntity(nested, "Deeper");
		Entity keptChild = scene.CreateChildEntity(roots[2], "Kept");
		Entity doomedChild = scene.CreateChildEntity(roots[2], "Doomed");
		Scene other;
		Entity foreign = other.CreateEntity("Foreign");

		const uint64_t version = scene.GetHierarchyVersion();
		// Nested entities go with their listed ancestor (listed before or after it); invalid and foreign entities are ignored.
		const std::vector<Entity> doomed = { deeper, roots[1], Entity(), roots[4], doomedChild, foreign, roots[4] };
		scene.DestroyEntities(doomed);
		CHECK(scene.GetHierarchyVersion() == version + 1);
		CHECK_FALSE(roots[1].IsValid());
		CHECK_FALSE(nested.IsValid());
		CHECK_FALSE(deeper.IsValid());
		CHECK_FALSE(roots[4].IsValid());
		CHECK_FALSE(doomedChild.IsValid());
		CHECK(foreign.IsValid());
		CHECK(keptChild.GetParent() == roots[2]);
		CHECK(scene.GetRootEntities() == std::vector<UUID> { roots[0].GetUUID(), roots[2].GetUUID(), roots[3].GetUUID(), roots[5].GetUUID() });
		CHECK(roots[2].GetComponent<RelationshipComponent>().Children == std::vector<UUID> { keptChild.GetUUID() });
		CHECK(scene.GetEntityCount() == 5);
		CheckSceneCaches(scene);

		// Nothing to destroy changes nothing.
		scene.DestroyEntities({});
		CHECK(scene.GetHierarchyVersion() == version + 1);
	}

	TEST_CASE("Running systems hear about every subtree of a batch before any is destroyed")
	{
		DestroyOrderSystem::Announced.clear();
		DestroyOrderSystem::AllListedAlive = true;
		SceneSystemRegistry::Register({ "TestDestroyOrder", false, [](Scene& scene) { return CreateScope<DestroyOrderSystem>(scene); } });
		{
			Scene scene;
			Entity first = scene.CreateEntity("First");
			Entity firstChild = scene.CreateChildEntity(first, "FirstChild");
			Entity second = scene.CreateEntity("Second");
			Entity survivor = scene.CreateEntity("Survivor");
			scene.OnRuntimeStart();
			// The nested and the repeated entity are announced once, with the subtree they belong to.
			const std::vector<Entity> doomed = { firstChild, first, second, second };
			scene.DestroyEntities(doomed);
			CHECK(DestroyOrderSystem::Announced == std::vector<std::string> { "FirstChild", "First", "Second" });
			CHECK(DestroyOrderSystem::AllListedAlive);
			CHECK(scene.GetEntityCount() == 1);
			CHECK(survivor.IsValid());
			CheckSceneCaches(scene);
			scene.OnRuntimeStop();
		}
		SceneSystemRegistry::Unregister("TestDestroyOrder");
	}

	TEST_CASE("Placing entities restores recorded parents and positions")
	{
		std::mt19937 random(7);
		for (int round = 0; round < 20; round++)
		{
			INFO("Round ", round);
			Scene scene;
			std::vector<Entity> entities = CreateRandomHierarchy(scene, 60, random);
			const std::unordered_map<UUID, Placement> recorded = RecordLayout(scene);

			// Scramble: reparent (where no cycle forms) and reorder.
			for (int move = 0; move < 40; move++)
			{
				Entity entity = entities[std::uniform_int_distribution<size_t>(0, entities.size() - 1)(random)];
				if (std::uniform_int_distribution<int>(0, 1)(random) == 0)
				{
					Entity parent = std::uniform_int_distribution<int>(0, 4)(random) == 0 ? Entity() : entities[std::uniform_int_distribution<size_t>(0, entities.size() - 1)(random)];
					scene.SetParent(entity, parent, false);
				}
				else
				{
					scene.SetSiblingIndex(entity, std::uniform_int_distribution<size_t>(0, 10)(random));
				}
			}

			// Like undo: only the entities whose place differs are listed.
			std::vector<Scene::EntityPlacement> placements;
			for (const auto& [id, placement] : recorded)
			{
				Entity entity = scene.GetEntityByUUID(id);
				if (entity.GetComponent<RelationshipComponent>().Parent != placement.Parent || scene.GetSiblingIndex(entity) != placement.SiblingIndex)
					placements.push_back({ id, placement.Parent, placement.SiblingIndex });
			}
			const uint64_t version = scene.GetHierarchyVersion();
			CHECK(scene.PlaceEntities(placements));
			if (!placements.empty())
				CHECK(scene.GetHierarchyVersion() == version + 1);
			CHECK(RecordLayout(scene) == recorded);
			CheckSceneCaches(scene);
		}
	}

	TEST_CASE("Placements that cannot be honored are reported and leave the scene consistent")
	{
		struct Observer
		{
			std::vector<entt::entity> Updated;
			void OnUpdate(entt::registry&, entt::entity entity) { Updated.push_back(entity); }
		};

		Scene scene;
		Entity a = scene.CreateEntity("A");
		Entity b = scene.CreateEntity("B");
		Entity child = scene.CreateChildEntity(a, "Child");
		Entity c = scene.CreateEntity("C");
		Observer observer;
		scene.GetRegistry().on_update<RelationshipComponent>().connect<&Observer::OnUpdate>(observer);

		// Only a change of parent is signaled; a reorder among the same siblings is not.
		CHECK(scene.PlaceEntities(std::vector<Scene::EntityPlacement> { { c.GetUUID(), UUID::Null(), 0 }, { b.GetUUID(), a.GetUUID(), 0 } }));
		CHECK(scene.GetRootEntities() == std::vector<UUID> { c.GetUUID(), a.GetUUID() });
		CHECK(a.GetChildren() == std::vector<Entity> { b, child });
		CHECK(observer.Updated == std::vector<entt::entity> { b.GetHandle() });

		// A cycle: A cannot go below its own child; it stays at the top level, at its position.
		CHECK_FALSE(scene.PlaceEntities(std::vector<Scene::EntityPlacement> { { a.GetUUID(), child.GetUUID(), 0 } }));
		CHECK_FALSE(a.GetParent().IsValid());
		CHECK(scene.GetRootEntities() == std::vector<UUID> { a.GetUUID(), c.GetUUID() });

		// A missing parent leaves the entity at the top level; missing or repeated targets are skipped.
		CHECK_FALSE(scene.PlaceEntities(std::vector<Scene::EntityPlacement> { { child.GetUUID(), UUID(0x1234), 5 } }));
		CHECK_FALSE(child.GetParent().IsValid());
		CHECK(scene.GetRootEntities().back() == child.GetUUID());
		CHECK_FALSE(scene.PlaceEntities(std::vector<Scene::EntityPlacement> { { UUID(0x4321), UUID::Null(), 0 }, { b.GetUUID(), UUID::Null(), 0 }, { b.GetUUID(), c.GetUUID(), 0 } }));
		CHECK(scene.GetRootEntities().front() == b.GetUUID());
		CheckSceneCaches(scene);
	}

	TEST_CASE("Scene copies rebuild the links and keep exact world transforms")
	{
		std::mt19937 random(11);
		Ref<Scene> source = CreateRef<Scene>("Source");
		std::vector<Entity> entities = CreateRandomHierarchy(*source, 200, random);
		entities[17].SetActive(false);
		source->UpdateWorldTransforms();

		Ref<Scene> copy = Scene::Copy(source);
		CheckSceneCaches(*copy);
		CHECK(copy->GetRootEntities() == source->GetRootEntities());
		for (const Entity entity : source->GetEntitiesInHierarchyOrder())
		{
			Entity copied = copy->GetEntityByUUID(entity.GetUUID());
			REQUIRE(copied);
			CHECK(copied.GetComponent<RelationshipComponent>().Children == entity.GetComponent<RelationshipComponent>().Children);
			CHECK(copy->GetDepth(copied) == source->GetDepth(entity));
			CHECK(copy->IsActiveInHierarchy(copied) == source->IsActiveInHierarchy(entity));
			CHECK(copied.GetComponent<WorldTransformComponent>().Matrix == entity.GetComponent<WorldTransformComponent>().Matrix);
		}
		// Current in the source, current in the copy: nothing to recompute.
		copy->UpdateWorldTransforms();
		CHECK(copy->GetTransformUpdateCount() == 0);

		// What is stale in the source is stale in the copy.
		RandomizeTransform(entities[3], random);
		Ref<Scene> staleCopy = Scene::Copy(source);
		staleCopy->UpdateWorldTransforms();
		CHECK(staleCopy->GetTransformUpdateCount() >= 1);
		CheckSceneCaches(*staleCopy);
		source->UpdateWorldTransforms();
		CHECK(staleCopy->GetEntityByUUID(entities[3].GetUUID()).GetComponent<WorldTransformComponent>().Matrix == entities[3].GetComponent<WorldTransformComponent>().Matrix);
	}

	TEST_CASE("Deserialized entities are linked, placed and activated under their parent")
	{
		Scene source;
		Entity root = source.CreateEntity("Root");
		Entity child = source.CreateChildEntity(root, "Child");
		source.CreateChildEntity(child, "Grandchild");
		source.CreateChildEntity(root, "Second");
		const nlohmann::json snapshot = SceneSerializer::SerializeEntities(source, { root });

		Scene scene;
		Entity holder = scene.CreateEntity("Holder");
		scene.CreateChildEntity(holder, "Existing");
		holder.SetActive(false);
		EntityInstantiationOptions options;
		options.Parent = holder;
		const std::vector<Entity> roots = SceneSerializer::DeserializeEntities(scene, snapshot, options);
		REQUIRE(roots.size() == 1);
		CHECK(roots[0].GetParent() == holder);
		CHECK(scene.GetSiblingIndex(roots[0]) == 1);
		CHECK(scene.GetDepth(roots[0].GetChildren()[0].GetChildren()[0]) == 3);
		// Inactive under the inactive holder right away, without an update.
		for (const Entity entity : scene.GetEntitiesInHierarchyOrder())
			CHECK_FALSE(scene.IsActiveInHierarchy(entity));
		CheckSceneCaches(scene);

		holder.SetActive(true);
		for (const Entity entity : scene.GetEntitiesInHierarchyOrder())
			CHECK(scene.IsActiveInHierarchy(entity));
		scene.UpdateWorldTransforms();
		CheckSceneCaches(scene);

		// Children listed before their parents.
		const nlohmann::json reversed = { { "Entities", {
			{ { "ID", "00000000000000C1" }, { "Parent", "00000000000000C2" } },
			{ { "ID", "00000000000000C3" }, { "Parent", "00000000000000C2" } },
			{ { "ID", "00000000000000C2" } }
		} } };
		Scene loaded;
		options = EntityInstantiationOptions();
		options.GenerateNewUUIDs = false;
		SceneSerializer::DeserializeEntities(loaded, reversed, options);
		Entity parent = loaded.GetEntityByUUID(UUID(0xC2));
		CHECK(parent.GetChildren() == std::vector<Entity> { loaded.GetEntityByUUID(UUID(0xC1)), loaded.GetEntityByUUID(UUID(0xC3)) });
		CheckSceneCaches(loaded);
	}

	TEST_CASE("The hierarchy order is computed once per hierarchy version")
	{
		Scene scene;
		Entity a = scene.CreateEntity("A");
		Entity b = scene.CreateChildEntity(a, "B");
		Entity c = scene.CreateEntity("C");
		const uint64_t builds = scene.GetHierarchyOrderBuildCount();
		CHECK(scene.GetEntitiesInHierarchyOrder() == std::vector<Entity> { a, b, c });
		CHECK(scene.GetEntitiesInHierarchyOrder() == std::vector<Entity> { a, b, c });
		CHECK(scene.GetHierarchyOrderBuildCount() == builds + 1);

		// Transforms, activity and components leave the order alone.
		a.GetTransform().Translation.x = 1.0f;
		a.MarkModified<TransformComponent>();
		b.SetActive(false);
		c.AddComponent<TagComponent>("Tagged");
		scene.UpdateWorldTransforms();
		CHECK(scene.GetEntitiesInHierarchyOrder().size() == 3);
		CHECK(scene.GetHierarchyOrderBuildCount() == builds + 1);

		REQUIRE(scene.SetSiblingIndex(c, 0));
		CHECK(scene.GetEntitiesInHierarchyOrder() == std::vector<Entity> { c, a, b });
		CHECK(scene.GetHierarchyOrderBuildCount() == builds + 2);
	}

	TEST_CASE("Moves in the hierarchy are reported since a version")
	{
		Scene scene;
		Entity a = scene.CreateEntity("A");
		Entity b = scene.CreateEntity("B");
		Entity c = scene.CreateEntity("C");
		uint64_t version = scene.GetHierarchyVersion();
		std::vector<entt::entity> moves;
		CHECK(scene.GetHierarchyMoves(version, moves));
		CHECK(moves.empty());

		// Creating and destroying entities moves nothing; reparenting, reordering and placing do (a child created under a
		// parent is reparented).
		scene.DestroyEntity(scene.CreateEntity("Temporary"));
		CHECK(GetMoves(scene, version).empty());
		Entity child = scene.CreateChildEntity(a, "Child");
		REQUIRE(scene.SetParent(b, c));
		REQUIRE(scene.SetSiblingIndex(c, 0));
		CHECK(GetMoves(scene, version) == Handles({ child, b, c }));
		version = scene.GetHierarchyVersion();
		REQUIRE(scene.PlaceEntities(std::vector<Scene::EntityPlacement> { { a.GetUUID(), c.GetUUID(), 0 }, { b.GetUUID(), UUID::Null(), 1 } }));
		CHECK(GetMoves(scene, version) == Handles({ a, b }));
		// Setting the parent an entity already has moves nothing.
		version = scene.GetHierarchyVersion();
		REQUIRE(scene.SetParent(child, a));
		CHECK(GetMoves(scene, version).empty());

		// More moves than the scene remembers: anything may have moved since an older version.
		const uint64_t beforeMany = scene.GetHierarchyVersion();
		for (size_t move = 0; move <= Scene::c_MaxHierarchyMoves; move++)
			REQUIRE(scene.SetSiblingIndex(child, 0));
		moves.clear();
		CHECK_FALSE(scene.GetHierarchyMoves(beforeMany, moves));
		CHECK(moves.empty());
		CHECK(GetMoves(scene, scene.GetHierarchyVersion() - 1) == Handles({ child }));
		// One placement of more entities than the scene remembers.
		std::vector<Scene::EntityPlacement> placements;
		for (size_t index = 0; index <= Scene::c_MaxHierarchyMoves; index++)
			placements.push_back({ scene.CreateEntity("Placed").GetUUID(), UUID::Null(), 0 });
		version = scene.GetHierarchyVersion();
		REQUIRE(scene.PlaceEntities(placements));
		CHECK_FALSE(scene.GetHierarchyMoves(version, moves));
		CHECK(GetMoves(scene, scene.GetHierarchyVersion()).empty());
		CheckSceneCaches(scene);
	}

	TEST_CASE("Destruction requested during a frame is flushed in one batch")
	{
		DeferredDestroySystem::Doomed.clear();
		DeferredDestroySystem::MinimumAliveWhenAnnounced = SIZE_MAX;
		DeferredDestroySystem::Announced = 0;
		SceneSystemRegistry::Register({ "TestDeferredBatch", false, [](Scene& scene) { return CreateScope<DeferredDestroySystem>(scene); } });
		{
			// Every third of 3,000 roots, one with a child, and one nested below another doomed entity.
			Scene scene;
			std::vector<Entity> roots;
			for (int index = 0; index < 3000; index++)
				roots.push_back(scene.CreateEntity("Root" + std::to_string(index)));
			Entity child = scene.CreateChildEntity(roots[3], "Child");
			Entity nested = scene.CreateChildEntity(child, "Nested");
			std::vector<UUID> expectedRoots;
			for (size_t index = 0; index < roots.size(); index++)
			{
				if (index % 3 == 0)
					DeferredDestroySystem::Doomed.push_back(roots[index]);
				else
					expectedRoots.push_back(roots[index].GetUUID());
			}
			DeferredDestroySystem::Doomed.push_back(nested);
			scene.OnRuntimeStart();

			// One compaction of the root list, after every doomed subtree was announced.
			const uint64_t version = scene.GetHierarchyVersion();
			scene.OnUpdateRuntime(0.016f);
			CHECK(scene.GetHierarchyVersion() == version + 1);
			CHECK(DeferredDestroySystem::Announced == 1000 + 2);
			CHECK(DeferredDestroySystem::MinimumAliveWhenAnnounced == DeferredDestroySystem::Doomed.size());
			CHECK_FALSE(child.IsValid());
			CHECK_FALSE(nested.IsValid());
			CHECK(scene.GetRootEntities() == expectedRoots);
			CHECK(scene.GetEntityCount() == 2000);
			CheckSceneCaches(scene);
			scene.OnRuntimeStop();
		}
		SceneSystemRegistry::Unregister("TestDeferredBatch");
		DeferredDestroySystem::Doomed.clear();
	}

	TEST_CASE("Hierarchy order comparisons agree with the hierarchy order")
	{
		std::mt19937 random(3);
		Scene scene;
		CreateRandomHierarchy(scene, 120, random);
		const std::vector<Entity> order = scene.GetEntitiesInHierarchyOrder();
		for (size_t first = 0; first < order.size(); first += 3)
		{
			for (size_t second = 0; second < order.size(); second += 2)
			{
				const int expected = first < second ? -1 : (first > second ? 1 : 0);
				CHECK(scene.CompareHierarchyOrder(order[first], order[second]) == expected);
			}
		}
	}

	TEST_CASE("Name, tag and primary camera lookups use indices, not traversals")
	{
		Scene scene;
		std::vector<Entity> entities;
		for (int index = 0; index < 1000; index++)
		{
			Entity entity = scene.CreateEntity("Entity" + std::to_string(index));
			entity.AddComponent<TagComponent>(index % 2 == 0 ? "Even" : "Odd");
			entities.push_back(entity);
		}
		Entity firstCamera = scene.CreateEntity("FirstCamera");
		firstCamera.AddComponent<CameraComponent>();
		Entity secondCamera = scene.CreateEntity("SecondCamera");
		secondCamera.AddComponent<CameraComponent>();
		Entity idleCamera = scene.CreateChildEntity(entities[0], "IdleCamera");
		idleCamera.AddComponent<CameraComponent>().Primary = false;
		const uint64_t orderBuilds = scene.GetHierarchyOrderBuildCount();

		// The first lookup of each kind builds its index; later ones examine only the matching entities.
		CHECK(scene.FindEntityByName("Entity500") == entities[500]);
		CHECK(scene.GetLookupIndexBuildCount() == 1);
		uint64_t visits = scene.GetLookupVisitCount();
		CHECK(scene.FindEntityByName("Entity700") == entities[700]);
		CHECK(scene.GetLookupVisitCount() - visits == 1);
		CHECK_FALSE(scene.FindEntityByName("Missing").IsValid());
		CHECK(scene.GetLookupVisitCount() - visits == 1);

		std::vector<Entity> evens = scene.FindEntitiesByTag("Even");
		CHECK(scene.GetLookupIndexBuildCount() == 2);
		REQUIRE(evens.size() == 500);
		CHECK(evens.front() == entities[0]);
		CHECK(evens.back() == entities[998]);
		visits = scene.GetLookupVisitCount();
		CHECK(scene.FindEntitiesByTag("Odd").size() == 500);
		CHECK(scene.GetLookupVisitCount() - visits == 500);

		// The primary camera examines the three camera entities.
		visits = scene.GetLookupVisitCount();
		CHECK(scene.GetPrimaryCameraEntity() == firstCamera);
		CHECK(scene.GetLookupVisitCount() - visits == 3);
		CHECK(scene.GetHierarchyOrderBuildCount() == orderBuilds);
		CHECK(scene.GetLookupIndexBuildCount() == 2);
		CheckSceneCaches(scene);

		// Renames, tag changes and removals, deactivation, reordering and destruction keep the answers current.
		entities[10].GetComponent<NameComponent>().Name = "Renamed";
		entities[10].MarkModified<NameComponent>();
		CHECK(scene.FindEntityByName("Renamed") == entities[10]);
		CHECK_FALSE(scene.FindEntityByName("Entity10").IsValid());
		entities[2].GetComponent<TagComponent>().Tag = "Odd";
		entities[2].MarkModified<TagComponent>();
		entities[4].RemoveComponent<TagComponent>();
		CHECK(scene.FindEntitiesByTag("Even").size() == 498);
		CHECK(scene.FindEntitiesByTag("Odd").size() == 501);
		entities[6].SetActive(false);
		CHECK(scene.FindEntitiesByTag("Even").size() == 498); // Lookups include inactive entities
		firstCamera.SetActive(false);
		CHECK(scene.GetPrimaryCameraEntity() == secondCamera);
		firstCamera.SetActive(true);
		REQUIRE(scene.SetSiblingIndex(secondCamera, 0));
		CHECK(scene.GetPrimaryCameraEntity() == secondCamera);
		secondCamera.GetComponent<CameraComponent>().Primary = false; // Read directly: never stale
		CHECK(scene.GetPrimaryCameraEntity() == firstCamera);
		idleCamera.GetComponent<CameraComponent>().Primary = true;
		CHECK(scene.GetPrimaryCameraEntity() == idleCamera); // Below the first root
		scene.DestroyEntity(entities[0]);
		CHECK(scene.GetPrimaryCameraEntity() == firstCamera);
		CHECK(scene.FindEntitiesByTag("Even").size() == 497);
		CHECK_FALSE(scene.FindEntityByName("IdleCamera").IsValid());

		// The first of several entities with one name in hierarchy order.
		Entity late = scene.CreateEntity("Twin");
		Entity early = scene.CreateEntity("Twin");
		CHECK(scene.FindEntityByName("Twin") == late);
		REQUIRE(scene.SetSiblingIndex(early, 0));
		CHECK(scene.FindEntityByName("Twin") == early);
		REQUIRE(scene.SetParent(early, late));
		CHECK(scene.FindEntityByName("Twin") == late); // An ancestor comes first
		CHECK(scene.GetLookupIndexBuildCount() == 2);
		CheckSceneCaches(scene);
	}

	TEST_CASE("Names shared by many entities are removed from the index in constant time")
	{
		Scene scene;
		std::vector<Entity> entities;
		for (int index = 0; index < 2000; index++)
			entities.push_back(scene.CreateEntity("Same"));
		CHECK(scene.FindEntityByName("Same") == entities[0]);

		// Every other one destroyed in one batch, then renamed ones: the bucket stays consistent.
		std::vector<Entity> doomed;
		for (size_t index = 0; index < entities.size(); index += 2)
			doomed.push_back(entities[index]);
		scene.DestroyEntities(doomed);
		CHECK(scene.FindEntityByName("Same") == entities[1]);
		for (size_t index = 1; index < 200; index += 2)
		{
			entities[index].GetComponent<NameComponent>().Name = "Other";
			entities[index].MarkModified<NameComponent>();
		}
		CHECK(scene.FindEntityByName("Same") == entities[201]);
		CHECK(scene.FindEntityByName("Other") == entities[1]);
		CheckSceneCaches(scene);
	}

	TEST_CASE("Creating more entities than an EnTT registry can hold fails cleanly")
	{
		// The registry is filled with plain EnTT entities (they count against its limit like scene entities, and are much
		// quicker to make), then with scene entities up to the limit.
		Scene scene;
		Entity first = scene.CreateEntity("First");
		std::vector<entt::entity> filler(Scene::c_MaxEntities - 3);
		scene.GetRegistry().create(filler.begin(), filler.end());
		Entity second = scene.CreateEntity("Second");
		Entity last = scene.CreateChildEntity(second, "Last");
		REQUIRE(last.IsValid());
		CHECK(scene.GetRegistry().storage<entt::entity>().free_list() == Scene::c_MaxEntities);

		// The 1,048,576th entity: an error and an invalid entity, without an assertion or a corrupted handle.
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		CHECK_FALSE(scene.CreateEntity("OneTooMany").IsValid());
		CHECK_FALSE(scene.CreateChildEntity(first, "ChildTooMany").IsValid());
		CHECK(CountLogMessages(logStart, "maximum of 1048575 entities") == 2);
		CHECK(scene.GetRegistry().storage<entt::entity>().free_list() == Scene::c_MaxEntities);
		CHECK(scene.GetEntityCount() == 3);
		CHECK(scene.GetRootEntities() == std::vector<UUID> { first.GetUUID(), second.GetUUID() });
		CHECK(last.GetParent() == second);
		CheckSceneCaches(scene);

		// Deserialization reports the limit as an error.
		std::string error;
		const nlohmann::json snapshot = { { "Entities", { { { "ID", "0000000000000F01" } } } } };
		CHECK(SceneSerializer::DeserializeEntities(scene, snapshot, {}, &error).empty());
		CHECK(error.find("at most 1048575 entities") != std::string::npos);
		CHECK(scene.GetEntityCount() == 3);

		// Room again after a destruction.
		scene.DestroyEntity(last);
		Entity again = scene.CreateChildEntity(first, "Again");
		REQUIRE(again.IsValid());
		CHECK(scene.GetEntityByUUID(again.GetUUID()) == again);
		CHECK(first.GetChildren() == std::vector<Entity> { again });
		CheckSceneCaches(scene);
	}
}
