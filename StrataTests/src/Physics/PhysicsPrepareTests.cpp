#include <doctest/doctest.h>

#include "Scene/SceneLoaderTestUtils.h"
#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Physics/PhysicsWorld.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneLoader.h"
#include "Strata/Scene/SceneWorkBudget.h"
#include "TestHelpers.h"

#include <chrono>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A scene document of `count` static boxes in a grid, and a dynamic box resting above the first.
	std::vector<uint8_t> MakeBoxSceneDocument(uint32_t count)
	{
		Scene scene("Boxes");
		for (uint32_t index = 0; index < count; index++)
		{
			Entity box = scene.CreateEntity("Box");
			box.GetTransform().Translation = glm::vec3(static_cast<float>(index % 200) * 2.0f, 0.0f, static_cast<float>(index / 200) * 2.0f);
			box.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.5f);
		}
		Entity crate = scene.CreateEntity("Crate");
		crate.GetTransform().Translation = glm::vec3(0.0f, 1.0f, 0.0f);
		crate.AddComponent<RigidBodyComponent>();
		crate.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.5f);
		return SceneToBytes(scene);
	}

}

TEST_SUITE("Physics.Capacity")
{
	TEST_CASE("A loaded scene's bodies are built while it is prepared, a slice per frame, and its first frame builds none")
	{
#if defined(ST_DEBUG)
		constexpr uint32_t c_Boxes = 20'000;
#else
		constexpr uint32_t c_Boxes = 40'000;
#endif
		SceneLoaderFixture fixture(MakeBoxSceneDocument(c_Boxes));
		SceneLoadOptions options;
		options.PrepareSystems = true;
		SceneLoadRequest request = SceneLoader::LoadAsync(fixture.SceneHandle, options);

		// Frames without time left: each prepares one slice of bodies at most.
		uint64_t built = 0;
		uint32_t preparingFrames = 0;
		REQUIRE(WaitUntil([&]()
		{
			fixture.Manager->Update();
			const Scene* preparing = request.GetPreparedScene();
			SceneWorkBudget budget = SceneWorkBudget::ForFrame(preparing ? preparing->GetSettings() : SceneSettings());
			budget.Deadline = std::chrono::steady_clock::now();
			request.Update(&budget);
			if (const Scene* scene = request.GetPreparedScene())
			{
				const PreparedPhysicsWorld* prepared = scene->GetPreparedState<PreparedPhysicsWorld>();
				REQUIRE(prepared);
				const uint64_t now = prepared->World->GetStats().BuildCount;
				CHECK(now - built <= PhysicsSystem::c_PrepareBodiesPerSlice);
				built = now;
				preparingFrames++;
			}
			return request.IsFinished();
		}, std::chrono::seconds(60)));
		REQUIRE(request.GetState() == SceneLoadState::Ready);
		CHECK(built == c_Boxes + 1);
		CHECK(preparingFrames >= (c_Boxes + 1 + PhysicsSystem::c_PrepareBodiesPerSlice - 1) / PhysicsSystem::c_PrepareBodiesPerSlice);

		// Started, the scene's physics adopts the prepared world: its first frames build no body, and the crate rests.
		const Ref<Scene> scene = request.TakeScene();
		REQUIRE(scene);
		scene->OnRuntimeStart();
		CHECK(scene->GetPreparedState<PreparedPhysicsWorld>() == nullptr);
		PhysicsSystem* physics = scene->GetSystem<PhysicsSystem>();
		REQUIRE(physics);
		REQUIRE(physics->GetWorld());
		CHECK(physics->GetStats().BuildCount == built);
		CHECK(physics->GetStats().BodyCount == c_Boxes + 1);
		CHECK(physics->GetStats().PreparedSlices >= (c_Boxes + 1) / PhysicsSystem::c_PrepareBodiesPerSlice);
		for (uint32_t frame = 0; frame < 30; frame++)
			scene->OnUpdateRuntime(1.0f / 60.0f);
		CHECK(physics->GetStats().BuildCount == built);
		CHECK(scene->FindEntityByName("Crate").GetTransform().Translation.y == doctest::Approx(1.0f).epsilon(0.01));
		scene->OnRuntimeStop();
	}

	TEST_CASE("A load that does not prepare its systems leaves the bodies to the scene's start")
	{
		SceneLoaderFixture fixture(MakeBoxSceneDocument(100));
		SceneLoadRequest request = SceneLoader::LoadAsync(fixture.SceneHandle, SceneLoadOptions());
		fixture.Finish(request);
		REQUIRE(request.GetState() == SceneLoadState::Ready);
		const Ref<Scene> scene = request.TakeScene();
		REQUIRE(scene);
		CHECK(scene->GetPreparedState<PreparedPhysicsWorld>() == nullptr);
		scene->OnRuntimeStart();
		CHECK(scene->GetSystem<PhysicsSystem>()->GetStats().BodyCount == 101);
		CHECK(scene->GetSystem<PhysicsSystem>()->GetStats().PreparedSlices == 0);
		scene->OnRuntimeStop();
	}

	TEST_CASE("A prepared scene let go before it runs drops its prepared world on the main thread")
	{
		SceneLoaderFixture fixture(MakeBoxSceneDocument(100));
		SceneLoadOptions options;
		options.PrepareSystems = true;
		SceneLoadRequest request = SceneLoader::LoadAsync(fixture.SceneHandle, options);
		fixture.Finish(request);
		REQUIRE(request.GetState() == SceneLoadState::Ready);
		REQUIRE(request.GetPreparedScene());
		CHECK(request.GetPreparedScene()->GetPreparedState<PreparedPhysicsWorld>());

		// Scene::ReleaseInBackground drops it before the background job takes the scene (held here, the job never destroys
		// it): the world, listening to the registry, never meets a registry being destroyed on another thread.
		const Ref<Scene> scene = request.TakeScene();
		REQUIRE(scene);
		const Ref<Scene> held = scene;
		Scene::ReleaseInBackground(scene);
		CHECK(held->GetPreparedState<PreparedPhysicsWorld>() == nullptr);
	}

	TEST_CASE("A cancelled load drops the world its systems prepared")
	{
		SceneLoaderFixture fixture(MakeBoxSceneDocument(100));
		SceneLoadOptions options;
		options.PrepareSystems = true;
		SceneLoadRequest request = SceneLoader::LoadAsync(fixture.SceneHandle, options);
		fixture.Finish(request);
		REQUIRE(request.GetPreparedScene());
		request.Cancel();
		CHECK(request.GetPreparedScene() == nullptr);
	}
}
