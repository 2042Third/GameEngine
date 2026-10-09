#include <doctest/doctest.h>

#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace Strata;

namespace
{
	struct CountingSystem : public SceneSystem
	{
		static inline int Started = 0;
		static inline int Stopped = 0;
		static inline int Updates = 0;
		static inline int FixedUpdates = 0;
		static inline int LateUpdates = 0;
		static inline float LastFixedTimestep = 0.0f;

		static void Reset()
		{
			Started = Stopped = Updates = FixedUpdates = LateUpdates = 0;
			LastFixedTimestep = 0.0f;
		}

		void OnRuntimeStart() override { Started++; }
		void OnRuntimeStop() override { Stopped++; }
		void OnUpdate(Timestep) override { Updates++; }
		void OnFixedUpdate(float timestep) override
		{
			FixedUpdates++;
			LastFixedTimestep = timestep;
		}
		void OnLateUpdate(Timestep) override { LateUpdates++; }
	};

	struct DestroyingSystem : public SceneSystem
	{
		explicit DestroyingSystem(Scene& scene)
			: TargetScene(scene)
		{
		}

		void OnUpdate(Timestep) override
		{
			Entity target = TargetScene.FindEntityByName("Doomed");
			TargetScene.DestroyEntity(target);
			TargetScene.DestroyEntity(target); // Destroying twice is harmless
			StillValidDuringFrame = target.IsValid() && TargetScene.IsPendingDestroy(target);
		}

		Scene& TargetScene;
		static inline bool StillValidDuringFrame = false;
	};

	struct ScopedCountingSystem
	{
		ScopedCountingSystem(bool simulate = false)
		{
			CountingSystem::Reset();
			SceneSystemRegistry::Register({ "TestCounting", simulate, [](Scene&) { return CreateScope<CountingSystem>(); } });
		}

		~ScopedCountingSystem()
		{
			SceneSystemRegistry::Unregister("TestCounting");
		}
	};
}

TEST_SUITE("Scene")
{
	TEST_CASE("Entities are created with core components")
	{
		Scene scene("Test");
		Entity entity = scene.CreateEntity("Player");
		CHECK(entity.IsValid());
		CHECK(entity.GetName() == "Player");
		CHECK(entity.GetUUID().IsValid());
		CHECK(entity.HasComponent<TransformComponent>());
		CHECK(entity.HasComponent<RelationshipComponent>());
		CHECK(scene.GetEntityByUUID(entity.GetUUID()) == entity);
		CHECK(scene.GetEntityCount() == 1);
		CHECK(scene.CreateEntity().GetName() == "Entity");

		const UUID fixed(1234);
		CHECK(scene.CreateEntityWithUUID(fixed, "Fixed").GetUUID() == fixed);
		// Duplicate UUIDs are rejected by assigning a fresh one.
		CHECK(scene.CreateEntityWithUUID(fixed, "Again").GetUUID() != fixed);
	}

	TEST_CASE("Destroying an entity destroys its descendants")
	{
		Scene scene;
		Entity parent = scene.CreateEntity("Parent");
		Entity child = scene.CreateChildEntity(parent, "Child");
		Entity grandchild = scene.CreateChildEntity(child, "Grandchild");
		Entity other = scene.CreateEntity("Other");

		scene.DestroyEntity(child);
		CHECK_FALSE(child.IsValid());
		CHECK_FALSE(grandchild.IsValid());
		CHECK(parent.IsValid());
		CHECK(parent.GetChildren().empty());
		CHECK(scene.GetEntityCount() == 2);

		scene.DestroyEntity(parent);
		CHECK(scene.GetRootEntities() == std::vector<UUID> { other.GetUUID() });
		scene.DestroyEntity(Entity()); // No-op
	}

	TEST_CASE("Hierarchy manipulation keeps order and rejects cycles")
	{
		Scene scene;
		Entity a = scene.CreateEntity("A");
		Entity b = scene.CreateEntity("B");
		Entity c = scene.CreateEntity("C");

		CHECK(scene.SetParent(b, a));
		CHECK(scene.SetParent(c, a));
		CHECK(b.GetParent() == a);
		CHECK(a.GetChildren() == std::vector<Entity> { b, c });
		CHECK(scene.GetRootEntities() == std::vector<UUID> { a.GetUUID() });

		CHECK_FALSE(scene.SetParent(a, c)); // Cycle
		CHECK_FALSE(scene.SetParent(a, a));
		CHECK(scene.IsDescendantOf(c, a));
		CHECK_FALSE(scene.IsDescendantOf(a, c));

		CHECK(scene.SetSiblingIndex(c, 0));
		CHECK(a.GetChildren() == std::vector<Entity> { c, b });

		CHECK(scene.SetParent(b, Entity()));
		CHECK(scene.GetRootEntities() == std::vector<UUID> { a.GetUUID(), b.GetUUID() });

		const std::vector<Entity> order = scene.GetEntitiesInHierarchyOrder();
		CHECK(order == std::vector<Entity> { a, c, b });
	}

	TEST_CASE("Reparenting notifies the moved entity's relationship listeners")
	{
		struct Observer
		{
			std::vector<entt::entity> Updated;
			std::vector<UUID> SeenParents;
			glm::vec3 SeenTranslation = glm::vec3(0.0f);

			void OnUpdate(entt::registry& registry, entt::entity entity)
			{
				Updated.push_back(entity);
				SeenParents.push_back(registry.get<RelationshipComponent>(entity).Parent);
				SeenTranslation = registry.get<TransformComponent>(entity).Translation;
			}
		};

		Scene scene;
		Entity parent = scene.CreateEntity("Parent");
		parent.GetTransform().Translation = glm::vec3(5.0f, 0.0f, 0.0f);
		Entity child = scene.CreateEntity("Child");
		Entity other = scene.CreateEntity("Other");
		Observer observer;
		scene.GetRegistry().on_update<RelationshipComponent>().connect<&Observer::OnUpdate>(observer);

		// Listeners see the final hierarchy and the transform that keeps the world pose.
		CHECK(scene.SetParent(child, parent));
		REQUIRE(observer.Updated.size() == 1);
		CHECK(observer.Updated[0] == child.GetHandle());
		CHECK(observer.SeenParents[0] == parent.GetUUID());
		CHECK(observer.SeenTranslation == glm::vec3(-5.0f, 0.0f, 0.0f));

		// No change, rejected requests and sibling reordering emit nothing.
		CHECK(scene.SetParent(child, parent));
		CHECK_FALSE(scene.SetParent(parent, child));
		CHECK(scene.SetParent(other, parent));
		CHECK(scene.SetSiblingIndex(other, 0));
		CHECK(observer.Updated.size() == 2);

		CHECK(scene.SetParent(child, Entity()));
		REQUIRE(observer.Updated.size() == 3);
		CHECK(observer.Updated[2] == child.GetHandle());
		CHECK_FALSE(observer.SeenParents[2].IsValid());
	}

	TEST_CASE("World transforms follow the hierarchy")
	{
		Scene scene;
		Entity parent = scene.CreateEntity("Parent");
		Entity child = scene.CreateChildEntity(parent, "Child");
		parent.GetTransform().Translation = { 10.0f, 0.0f, 0.0f };
		parent.GetTransform().Scale = { 2.0f, 2.0f, 2.0f };
		child.GetTransform().Translation = { 1.0f, 0.0f, 0.0f };

		const glm::vec3 worldPosition = glm::vec3(scene.GetWorldTransform(child)[3]);
		CHECK(Math::IsNearlyEqual(worldPosition, glm::vec3(12.0f, 0.0f, 0.0f)));

		scene.UpdateWorldTransforms();
		CHECK(Math::IsNearlyEqual(glm::vec3(child.GetComponent<WorldTransformComponent>().Matrix[3]), glm::vec3(12.0f, 0.0f, 0.0f)));

		// Re-parenting keeps the world position by default.
		CHECK(scene.SetParent(child, Entity(), true));
		CHECK(Math::IsNearlyEqual(child.GetTransform().Translation, glm::vec3(12.0f, 0.0f, 0.0f), 1e-4f));
		CHECK(scene.SetParent(child, parent, true));
		CHECK(Math::IsNearlyEqual(child.GetTransform().Translation, glm::vec3(1.0f, 0.0f, 0.0f), 1e-4f));

		scene.SetWorldTransform(child, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 4.0f, 0.0f)));
		CHECK(Math::IsNearlyEqual(glm::vec3(scene.GetWorldTransform(child)[3]), glm::vec3(0.0f, 4.0f, 0.0f), 1e-4f));
	}

	TEST_CASE("Activity propagates down the hierarchy")
	{
		Scene scene;
		Entity parent = scene.CreateEntity("Parent");
		Entity child = scene.CreateChildEntity(parent, "Child");
		CHECK(scene.IsActiveInHierarchy(child));

		parent.SetActive(false);
		CHECK_FALSE(parent.IsActive());
		CHECK(child.IsActive());
		CHECK_FALSE(scene.IsActiveInHierarchy(child));
		scene.UpdateWorldTransforms();
		CHECK_FALSE(child.GetComponent<WorldTransformComponent>().ActiveInHierarchy);

		parent.SetActive(true);
		scene.UpdateWorldTransforms();
		CHECK(child.GetComponent<WorldTransformComponent>().ActiveInHierarchy);
	}

	TEST_CASE("Find by name and tag")
	{
		Scene scene;
		scene.CreateEntity("Alpha");
		Entity enemy1 = scene.CreateEntity("Enemy");
		Entity enemy2 = scene.CreateEntity("Enemy");
		enemy1.AddComponent<TagComponent>("Enemy");
		enemy2.AddComponent<TagComponent>("Enemy");

		CHECK(scene.FindEntityByName("Enemy") == enemy1);
		CHECK_FALSE(scene.FindEntityByName("Missing").IsValid());
		CHECK(scene.FindEntitiesByTag("Enemy").size() == 2);
		CHECK(scene.FindEntitiesByTag("Friend").empty());
	}

	TEST_CASE("Duplicating copies descendants and remaps internal references")
	{
		Scene scene;
		Entity root = scene.CreateEntity("Root");
		Entity child = scene.CreateChildEntity(root, "Child");
		Entity outside = scene.CreateEntity("Outside");
		root.AddComponent<MeshRendererComponent>().CastShadows = false;

		ScriptComponent& scripts = root.AddComponent<ScriptComponent>();
		ScriptEntry& script = scripts.Scripts.emplace_back();
		script.ClassName = "Follower";
		script.Fields.push_back({ "Inner", PropertyType::Entity, child.GetUUID() });
		script.Fields.push_back({ "Outer", PropertyType::Entity, outside.GetUUID() });

		Entity copy = scene.DuplicateEntity(root);
		REQUIRE(copy.IsValid());
		CHECK(copy != root);
		CHECK(copy.GetUUID() != root.GetUUID());
		CHECK(copy.GetName() == "Root");
		CHECK_FALSE(copy.GetComponent<MeshRendererComponent>().CastShadows);

		const std::vector<Entity> copyChildren = copy.GetChildren();
		REQUIRE(copyChildren.size() == 1);
		CHECK(copyChildren[0].GetUUID() != child.GetUUID());

		const ScriptEntry* copiedScript = copy.GetComponent<ScriptComponent>().FindScript("Follower");
		REQUIRE(copiedScript);
		CHECK(std::get<UUID>(copiedScript->FindField("Inner")->Value) == copyChildren[0].GetUUID());
		CHECK(std::get<UUID>(copiedScript->FindField("Outer")->Value) == outside.GetUUID());

		// The copy is placed right after the original.
		CHECK(scene.GetRootEntities() == std::vector<UUID> { root.GetUUID(), copy.GetUUID(), outside.GetUUID() });
	}

	TEST_CASE("Scene copies preserve UUIDs, data and hierarchy")
	{
		Ref<Scene> source = CreateRef<Scene>("Source");
		Entity parent = source->CreateEntity("Parent");
		Entity child = source->CreateChildEntity(parent, "Child");
		child.GetTransform().Translation = { 1.0f, 2.0f, 3.0f };
		parent.AddComponent<CameraComponent>().PerspectiveFOV = 75.0f;
		source->GetSettings().Gravity = { 0.0f, -3.0f, 0.0f };

		Ref<Scene> copy = Scene::Copy(source);
		CHECK(copy->GetName() == "Source");
		CHECK(copy->GetEntityCount() == 2);
		CHECK(copy->GetSettings().Gravity.y == doctest::Approx(-3.0f));

		Entity copiedChild = copy->GetEntityByUUID(child.GetUUID());
		REQUIRE(copiedChild.IsValid());
		CHECK(copiedChild.GetTransform().Translation == glm::vec3(1.0f, 2.0f, 3.0f));
		CHECK(copiedChild.GetParent().GetUUID() == parent.GetUUID());
		CHECK(copy->GetEntityByUUID(parent.GetUUID()).GetComponent<CameraComponent>().PerspectiveFOV == doctest::Approx(75.0f));

		// The copy is independent of the source.
		copiedChild.GetTransform().Translation.x = 100.0f;
		CHECK(child.GetTransform().Translation.x == doctest::Approx(1.0f));
	}

	TEST_CASE("Runtime update runs systems at the fixed timestep")
	{
		ScopedCountingSystem system;
		Scene scene;
		scene.GetSettings().FixedTimestep = 0.01f;

		scene.OnRuntimeStart();
		CHECK(scene.IsRunning());
		CHECK(CountingSystem::Started == 1);

		scene.OnUpdateRuntime(0.035f);
		CHECK(CountingSystem::Updates == 1);
		CHECK(CountingSystem::LateUpdates == 1);
		CHECK(CountingSystem::FixedUpdates == 3);
		CHECK(CountingSystem::LastFixedTimestep == doctest::Approx(0.01f));

		scene.OnUpdateRuntime(0.006f); // Accumulated 0.005 + 0.006 -> one more step
		CHECK(CountingSystem::FixedUpdates == 4);

		scene.SetPaused(true);
		scene.OnUpdateRuntime(1.0f);
		CHECK(CountingSystem::Updates == 2);
		scene.Step(2);
		scene.OnUpdateRuntime(1.0f);
		scene.OnUpdateRuntime(1.0f);
		scene.OnUpdateRuntime(1.0f);
		CHECK(CountingSystem::Updates == 4);
		CHECK(CountingSystem::FixedUpdates == 6);
		scene.SetPaused(false);

		// Long frames are capped to MaxFixedStepsPerFrame.
		scene.OnUpdateRuntime(10.0f);
		CHECK(CountingSystem::FixedUpdates == 6 + static_cast<int>(scene.GetSettings().MaxFixedStepsPerFrame));

		scene.SetTimeScale(0.0f);
		scene.OnUpdateRuntime(1.0f);
		CHECK(CountingSystem::FixedUpdates == 6 + static_cast<int>(scene.GetSettings().MaxFixedStepsPerFrame) + 1); // Only the capped backlog remains

		scene.OnRuntimeStop();
		CHECK(CountingSystem::Stopped == 1);
		CHECK_FALSE(scene.IsRunning());
	}

	TEST_CASE("Simulate mode only creates systems that support it")
	{
		ScopedCountingSystem system(false);
		Scene scene;
		scene.OnRuntimeStart(SceneRuntimeMode::Simulate);
		CHECK(CountingSystem::Started == 0);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Destruction during an update is deferred to the end of the frame")
	{
		SceneSystemRegistry::Register({ "TestDestroying", false, [](Scene& scene) { return CreateScope<DestroyingSystem>(scene); } });
		{
			Scene scene;
			Entity doomed = scene.CreateEntity("Doomed");
			scene.OnRuntimeStart();
			scene.OnUpdateRuntime(0.016f);
			CHECK(DestroyingSystem::StillValidDuringFrame);
			CHECK_FALSE(doomed.IsValid());
			scene.OnRuntimeStop();
		}
		SceneSystemRegistry::Unregister("TestDestroying");
	}

	TEST_CASE("Primary camera lookup")
	{
		Scene scene;
		CHECK_FALSE(scene.GetPrimaryCameraEntity().IsValid());
		Entity secondary = scene.CreateEntity("Secondary");
		secondary.AddComponent<CameraComponent>().Primary = false;
		Entity primary = scene.CreateEntity("Primary");
		primary.AddComponent<CameraComponent>();
		CHECK(scene.GetPrimaryCameraEntity() == primary);
		primary.SetActive(false);
		CHECK_FALSE(scene.GetPrimaryCameraEntity().IsValid());
	}

	TEST_CASE("Camera projections follow the engine conventions")
	{
		CameraComponent camera;
		const glm::mat4 projection = camera.GetProjection(16.0f / 9.0f);
		const glm::vec4 nearPoint = projection * glm::vec4(0.0f, 0.0f, -camera.PerspectiveNear, 1.0f);
		CHECK(nearPoint.z / nearPoint.w == doctest::Approx(1.0f));

		camera.Projection = ProjectionType::Orthographic;
		camera.OrthographicSize = 10.0f;
		const glm::vec4 top = camera.GetProjection(1.0f) * glm::vec4(0.0f, 5.0f, -1.0f, 1.0f);
		CHECK(top.y / top.w == doctest::Approx(1.0f));

		// Degenerate settings never produce invalid matrices.
		camera.OrthographicFar = camera.OrthographicNear;
		const glm::mat4 degenerate = camera.GetProjection(0.0f);
		CHECK(std::isfinite(degenerate[2][2]));
	}
}
