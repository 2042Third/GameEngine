#include <doctest/doctest.h>

#include "Scene/SceneTestUtils.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"

#include <algorithm>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	class ScopedJobSystem
	{
	public:
		explicit ScopedJobSystem(uint32_t workerThreads)
		{
			JobSystemSpecification specification;
			specification.WorkerThreadCount = workerThreads;
			specification.IOThreadCount = 0;
			JobSystem::Init(specification);
		}

		~ScopedJobSystem()
		{
			JobSystem::Shutdown();
		}

		ScopedJobSystem(const ScopedJobSystem&) = delete;
		ScopedJobSystem& operator=(const ScopedJobSystem&) = delete;
	};

	// A tree in which every entity up to `depth` has `branching` children; returns the entities level by level.
	std::vector<std::vector<Entity>> CreateTree(Scene& scene, uint32_t branching, uint32_t depth)
	{
		std::vector<std::vector<Entity>> levels = { { scene.CreateEntity("Root") } };
		for (uint32_t level = 1; level <= depth; level++)
		{
			std::vector<Entity> entities;
			for (const Entity parent : levels.back())
			{
				for (uint32_t child = 0; child < branching; child++)
				{
					Entity entity = scene.CreateChildEntity(parent, "Node");
					entity.GetTransform().Translation = glm::vec3(1.0f, static_cast<float>(child), 0.0f);
					entities.push_back(entity);
				}
			}
			levels.push_back(std::move(entities));
		}
		return levels;
	}

	void Move(Entity entity, const glm::vec3& offset)
	{
		entity.GetTransform().Translation += offset;
		entity.MarkModified<TransformComponent>();
	}

}

TEST_SUITE("Scene.Transforms")
{
	TEST_CASE("Updates without changes visit nothing")
	{
		Scene scene;
		CreateTree(scene, 10, 3); // 1111 entities
		scene.UpdateWorldTransforms();
		const uint64_t updated = scene.GetTransformUpdateCount();
		CHECK(updated == 1111);
		const uint64_t version = scene.GetTransformsVersion();

		scene.UpdateWorldTransforms();
		scene.OnUpdateEditor(0.016f);
		CHECK(scene.GetTransformUpdateCount() == updated);
		CHECK(scene.GetTransformsVersion() == version);
		std::vector<entt::entity> changes;
		CHECK(scene.GetWorldTransformChanges(version, changes));
		CHECK(changes.empty());
	}

	TEST_CASE("Only the subtrees of changed entities are recomputed")
	{
		Scene scene;
		const std::vector<std::vector<Entity>> levels = CreateTree(scene, 10, 3);
		scene.UpdateWorldTransforms();

		uint64_t updated = scene.GetTransformUpdateCount();
		Move(levels[3][123], glm::vec3(0.0f, 0.0f, 1.0f));
		scene.UpdateWorldTransforms();
		CHECK(scene.GetTransformUpdateCount() - updated == 1);

		// A node and two of its descendants: the node's subtree once.
		updated = scene.GetTransformUpdateCount();
		Move(levels[2][7].GetChildren()[3], glm::vec3(1.0f));
		Move(levels[2][7], glm::vec3(1.0f));
		Move(levels[2][7].GetChildren()[4], glm::vec3(1.0f));
		scene.UpdateWorldTransforms();
		CHECK(scene.GetTransformUpdateCount() - updated == 11);

		// Reparenting recomputes the moved subtree; activity changes recompute nothing.
		updated = scene.GetTransformUpdateCount();
		REQUIRE(scene.SetParent(levels[1][2], levels[3][999], false));
		Entity deactivated = levels[1][5];
		deactivated.SetActive(false);
		scene.UpdateWorldTransforms();
		CHECK(scene.GetTransformUpdateCount() - updated == 111);

		updated = scene.GetTransformUpdateCount();
		scene.InvalidateAllTransforms();
		scene.UpdateWorldTransforms();
		CHECK(scene.GetTransformUpdateCount() - updated == 1111);
		CheckSceneCaches(scene);
	}

	TEST_CASE("Entities changed directly after their creation need no signal")
	{
		Scene scene;
		Entity parent = scene.CreateEntity("Parent");
		parent.GetTransform().Translation = glm::vec3(1.0f, 2.0f, 3.0f);
		Entity child = scene.CreateChildEntity(parent, "Child");
		child.GetTransform().Scale = glm::vec3(2.0f);
		scene.UpdateWorldTransforms();
		CHECK(child.GetComponent<WorldTransformComponent>().Matrix[3] == glm::vec4(1.0f, 2.0f, 3.0f, 1.0f));
		CheckSceneCaches(scene);
	}

	TEST_CASE("World transforms read between updates equal what the next update caches")
	{
		std::mt19937 random(5);
		Scene scene;
		std::vector<Entity> entities = CreateRandomHierarchy(scene, 300, random);
		scene.UpdateWorldTransforms();

		for (int round = 0; round < 10; round++)
		{
			for (int change = 0; change < 15; change++)
				RandomizeTransform(entities[std::uniform_int_distribution<size_t>(0, entities.size() - 1)(random)], random);
			std::vector<glm::mat4> before;
			for (const Entity entity : entities)
				before.push_back(scene.GetWorldTransform(entity));
			scene.UpdateWorldTransforms();
			for (size_t index = 0; index < entities.size(); index++)
			{
				// Exactly: physics compares these matrices to notice moves.
				CHECK(before[index] == entities[index].GetComponent<WorldTransformComponent>().Matrix);
				CHECK(scene.GetWorldTransform(entities[index]) == before[index]);
			}
		}
		CheckSceneCaches(scene);
	}

	TEST_CASE("Large updates run on the job system with the serial results")
	{
		// Wide sets of independent changes, and one changed root whose subtree fans out: computed with and without workers.
		const auto build = [](Scene& scene)
		{
			std::vector<Entity> roots;
			for (int index = 0; index < 3000; index++)
			{
				Entity root = scene.CreateEntity("Root");
				root.GetTransform().Rotation = glm::angleAxis(0.001f * static_cast<float>(index), glm::vec3(0.0f, 1.0f, 0.0f));
				for (int child = 0; child < 3; child++)
					scene.CreateChildEntity(root, "Child").GetTransform().Translation = glm::vec3(static_cast<float>(child), 1.0f, 0.0f);
				roots.push_back(root);
			}
			Entity hub = scene.CreateEntity("Hub");
			for (int index = 0; index < 5000; index++)
				scene.CreateChildEntity(hub, "Spoke").GetTransform().Translation = glm::vec3(0.0f, 0.0f, static_cast<float>(index));
			scene.UpdateWorldTransforms();
			for (const Entity root : roots)
				Move(root, glm::vec3(1.0f, 0.0f, 0.0f));
			Move(hub, glm::vec3(0.0f, 2.0f, 0.0f));
			scene.UpdateWorldTransforms();
		};

		Scene serial;
		build(serial);
		Scene parallel;
		{
			ScopedJobSystem jobs(4);
			build(parallel);
		}
		CHECK(parallel.GetTransformUpdateCount() == serial.GetTransformUpdateCount());
		CheckSceneCaches(parallel);
		const std::vector<Entity> serialOrder = serial.GetEntitiesInHierarchyOrder();
		const std::vector<Entity> parallelOrder = parallel.GetEntitiesInHierarchyOrder();
		REQUIRE(serialOrder.size() == parallelOrder.size());
		for (size_t index = 0; index < serialOrder.size(); index++)
			CHECK(serialOrder[index].GetComponent<WorldTransformComponent>().Matrix == parallelOrder[index].GetComponent<WorldTransformComponent>().Matrix);
	}

	TEST_CASE("Many changed entities below unchanged deep ancestors are found in linear time")
	{
		// A chain with a leaf on every link; every leaf changes. Looking for changed ancestors walks each link once.
		Scene scene;
		std::vector<Entity> leaves;
		Entity link = scene.CreateEntity("Link");
		for (int depth = 0; depth < 3000; depth++)
		{
			leaves.push_back(scene.CreateChildEntity(link, "Leaf"));
			link = scene.CreateChildEntity(link, "Link");
			link.GetTransform().Translation.y = 1.0f;
		}
		scene.UpdateWorldTransforms();
		const uint64_t updated = scene.GetTransformUpdateCount();
		for (const Entity leaf : leaves)
			Move(leaf, glm::vec3(1.0f, 0.0f, 0.0f));
		scene.UpdateWorldTransforms();
		CHECK(scene.GetTransformUpdateCount() - updated == leaves.size());
		CHECK(leaves.back().GetComponent<WorldTransformComponent>().Matrix[3] == glm::vec4(1.0f, 2999.0f, 0.0f, 1.0f));
		CheckSceneCaches(scene);
	}

	TEST_CASE("Random edits keep every cached transform equal to a full recomputation")
	{
		// Reparenting, moving, (de)activating and destroying, checked against a full recomputation after every operation.
		// Unoptimized Debug builds compute each full recomputation about ten times slower, so they run a shorter sequence
		// on fewer entities; optimized builds run 10,000 operations on 5,000 entities.
#ifdef ST_DEBUG
		constexpr size_t c_Entities = 1000;
		constexpr int c_Operations = 1000;
#else
		constexpr size_t c_Entities = 5000;
		constexpr int c_Operations = 10000;
#endif
		std::mt19937 random(2024);
		Scene scene;
		std::vector<Entity> entities = CreateRandomHierarchy(scene, c_Entities, random);
		scene.UpdateWorldTransforms();

		const auto pick = [&]() -> Entity
		{
			while (true)
			{
				Entity entity = entities[std::uniform_int_distribution<size_t>(0, entities.size() - 1)(random)];
				if (entity.IsValid())
					return entity;
			}
		};

		size_t destroyed = 0;
		for (int operation = 0; operation < c_Operations; operation++)
		{
			const int kind = std::uniform_int_distribution<int>(0, 99)(random);
			if (kind < 30)
			{
				scene.SetParent(pick(), std::uniform_int_distribution<int>(0, 9)(random) == 0 ? Entity() : pick(), std::uniform_int_distribution<int>(0, 1)(random) == 0);
			}
			else if (kind < 75)
			{
				RandomizeTransform(pick(), random);
			}
			else if (kind < 95)
			{
				Entity entity = pick();
				entity.SetActive(!entity.IsActive());
			}
			else if (scene.GetEntityCount() > c_Entities / 2)
			{
				// Mostly leaves and small subtrees: the scene keeps most of its entities.
				Entity entity = pick();
				while (!entity.GetChildren().empty())
					entity = entity.GetChildren().front();
				scene.DestroyEntity(entity);
				destroyed++;
			}

			scene.UpdateWorldTransforms();
			std::string error;
			const bool valid = scene.ValidateWorldTransforms(&error);
			if (!valid)
			{
				FAIL_CHECK("Operation ", operation, ": ", error);
				break;
			}
		}
		CHECK(destroyed > 0);
		CheckSceneCaches(scene);
	}

	TEST_CASE("Activity in the hierarchy is exact at once")
	{
		Scene scene;
		Entity parent = scene.CreateEntity("Parent");
		Entity child = scene.CreateChildEntity(parent, "Child");
		Entity grandchild = scene.CreateChildEntity(child, "Grandchild");
		Entity other = scene.CreateEntity("Other");
		const uint64_t version = scene.GetTransformsVersion();

		parent.SetActive(false);
		CHECK_FALSE(scene.IsActiveInHierarchy(grandchild));
		CHECK_FALSE(grandchild.GetComponent<WorldTransformComponent>().ActiveInHierarchy);
		std::vector<entt::entity> changes;
		REQUIRE(scene.GetWorldTransformChanges(version, changes));
		CHECK(std::unordered_set<entt::entity>(changes.begin(), changes.end()) == std::unordered_set<entt::entity> { parent.GetHandle(), child.GetHandle(), grandchild.GetHandle() });

		REQUIRE(scene.SetParent(child, other));
		CHECK(scene.IsActiveInHierarchy(grandchild));
		other.SetActive(false);
		child.SetActive(false);
		other.SetActive(true);
		CHECK_FALSE(scene.IsActiveInHierarchy(grandchild));
		child.RemoveComponent<InactiveComponent>();
		CHECK(scene.IsActiveInHierarchy(grandchild));
		REQUIRE(scene.SetParent(child, parent));
		CHECK_FALSE(scene.IsActiveInHierarchy(child));
		CHECK_FALSE(scene.IsActiveInHierarchy(Entity()));
		CheckSceneCaches(scene);
	}

	TEST_CASE("World transform changes are reported since a version")
	{
		Scene scene;
		const std::vector<std::vector<Entity>> levels = CreateTree(scene, 4, 2); // 21 entities
		scene.UpdateWorldTransforms();
		const uint64_t version = scene.GetTransformsVersion();

		Move(levels[1][0], glm::vec3(1.0f));
		Move(levels[2][15], glm::vec3(1.0f));
		scene.UpdateWorldTransforms();
		CHECK(scene.GetTransformsVersion() == version + 1);
		std::vector<entt::entity> changes;
		REQUIRE(scene.GetWorldTransformChanges(version, changes));
		std::unordered_set<entt::entity> expected = { levels[1][0].GetHandle(), levels[2][15].GetHandle() };
		for (const Entity child : levels[1][0].GetChildren())
			expected.insert(child.GetHandle());
		CHECK(std::unordered_set<entt::entity>(changes.begin(), changes.end()) == expected);

		// Changes too many to remember: the caller must assume anything changed.
		Scene large;
		std::vector<Entity> roots;
		for (size_t index = 0; index <= Scene::c_MaxTransformChanges; index++)
			roots.push_back(large.CreateEntity("Root"));
		large.UpdateWorldTransforms();
		const uint64_t before = large.GetTransformsVersion();
		Move(roots[0], glm::vec3(1.0f));
		large.UpdateWorldTransforms();
		const uint64_t afterOne = large.GetTransformsVersion();
		large.InvalidateAllTransforms();
		large.UpdateWorldTransforms();
		changes.clear();
		CHECK_FALSE(large.GetWorldTransformChanges(before, changes));
		CHECK_FALSE(large.GetWorldTransformChanges(afterOne, changes));
		CHECK(changes.empty());
		CHECK(large.GetWorldTransformChanges(large.GetTransformsVersion(), changes));
		CHECK(changes.empty());
	}

	TEST_CASE("Validation finds transforms written without a signal")
	{
		Scene scene;
		Entity parent = scene.CreateEntity("Parent");
		Entity child = scene.CreateChildEntity(parent, "Child");
		scene.UpdateWorldTransforms();

		parent.GetTransform().Translation.x = 5.0f;
		scene.UpdateWorldTransforms();
		std::string error;
		CHECK_FALSE(scene.ValidateWorldTransforms(&error));
		CHECK(error.find("'Parent'") != std::string::npos);
		CHECK(error.find("MarkModified") != std::string::npos);

		// Told without an on_update signal, as physics does.
		scene.MarkTransformChanged(parent);
		CHECK(scene.GetWorldTransform(child)[3].x == doctest::Approx(5.0f));
		scene.UpdateWorldTransforms();
		CHECK(scene.ValidateWorldTransforms(&error));
		CHECK(child.GetComponent<WorldTransformComponent>().Matrix[3].x == doctest::Approx(5.0f));
	}

	TEST_CASE("Instantiated prefabs and duplicates get their world transforms below moved parents")
	{
		Scene source;
		Entity root = source.CreateEntity("Turret");
		root.GetTransform().Translation = glm::vec3(0.0f, 1.0f, 0.0f);
		source.CreateChildEntity(root, "Barrel").GetTransform().Translation = glm::vec3(0.0f, 0.0f, 2.0f);
		Ref<Prefab> prefab = Prefab::CreateFromEntities(source, { root });

		Scene scene;
		Entity holder = scene.CreateEntity("Holder");
		scene.UpdateWorldTransforms();
		Move(holder, glm::vec3(10.0f, 0.0f, 0.0f));
		const std::vector<Entity> instances = prefab->Instantiate(scene, holder);
		REQUIRE(instances.size() == 1);
		Entity copy = scene.DuplicateEntity(instances[0]);
		REQUIRE(copy);
		scene.UpdateWorldTransforms();
		for (const Entity turret : { instances[0], copy })
		{
			const Entity barrel = turret.GetChildren().front();
			CHECK(barrel.GetComponent<WorldTransformComponent>().Matrix[3] == glm::vec4(10.0f, 1.0f, 2.0f, 1.0f));
		}
		CheckSceneCaches(scene);
	}
}
