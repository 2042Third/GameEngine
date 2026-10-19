#include <doctest/doctest.h>

#include "Strata/Core/EngineServices.h"
#include "Strata/Core/Log.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneServices.h"
#include "TestHelpers.h"

#if ST_WITH_PHYSICS
	#include "Strata/Physics/PhysicsComponents.h"
#endif
#if ST_WITH_AUDIO
	#include "Strata/Audio/AudioComponents.h"
#endif
#if ST_WITH_ANIMATION
	#include "Strata/Animation/AnimationComponents.h"
#endif

#include <glm/glm.hpp>

#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	class FakeAudio final : public IAudioPlayback
	{
	public:
		bool Play(Entity) override { return true; }
		bool Pause(Entity) override { return true; }
		bool Stop(Entity) override { return true; }
		bool IsPlaying(Entity) override { return false; }
		bool Seek(Entity, float) override { return true; }
		float GetPlaybackPosition(Entity) override { return 0.0f; }
		bool PlayOneShot(AssetHandle, float, float) override { return true; }
		bool PlayOneShotAt(AssetHandle, const glm::vec3&, float, float) override { return true; }
		bool HasSource(Entity) override { return false; }
	};

#if ST_WITH_PHYSICS
	Entity CreateBox(Scene& scene, const char* name, const glm::vec3& position, bool dynamic)
	{
		Entity entity = scene.CreateEntity(name);
		entity.GetTransform().Translation = position;
		if (dynamic)
			entity.AddComponent<RigidBodyComponent>();
		entity.AddComponent<BoxColliderComponent>().HalfExtents = dynamic ? glm::vec3(0.5f) : glm::vec3(10.0f, 0.5f, 10.0f);
		return entity;
	}
#endif

}

TEST_SUITE("Scene.Services")
{
	TEST_CASE("A scene has one provider per service, found by its interface")
	{
		Scene scene;
		FakeAudio first;
		FakeAudio second;
		CHECK(scene.GetService<IAudioPlayback>() == nullptr);

		CHECK(scene.RegisterService<IAudioPlayback>(first));
		CHECK(scene.GetService<IAudioPlayback>() == &first);
		CHECK(scene.RegisterService<IAudioPlayback>(first)); // Again: no change
		CHECK(scene.GetService<IBodyMotionQuery>() == nullptr);

		// A second provider is refused with an error and changes nothing.
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		CHECK_FALSE(scene.RegisterService<IAudioPlayback>(second));
		CHECK(CountLogMessages(logStart, "already provided by another system") == 1);
		CHECK(scene.GetService<IAudioPlayback>() == &first);

		// Only the provider removes its registration.
		scene.UnregisterService<IAudioPlayback>(second);
		CHECK(scene.GetService<IAudioPlayback>() == &first);
		scene.UnregisterService<IAudioPlayback>(first);
		CHECK(scene.GetService<IAudioPlayback>() == nullptr);
		CHECK(scene.RegisterService<IAudioPlayback>(second));
		CHECK(scene.GetService<IAudioPlayback>() == &second);
		scene.UnregisterService<IAudioPlayback>(second);
	}

	TEST_CASE("The built-in systems offer their services while the scene plays")
	{
		// An engine without a module plays scenes without its services.
		Scene scene;
		scene.OnRuntimeStart(SceneRuntimeMode::Play);
		CHECK((scene.GetService<ICollisionEventSource>() != nullptr) == ST_WITH_PHYSICS);
		CHECK((scene.GetService<IBodyMotionQuery>() != nullptr) == ST_WITH_PHYSICS);
		CHECK((scene.GetService<IBodyControl>() != nullptr) == ST_WITH_PHYSICS);
		CHECK((scene.GetService<IPhysicsQueries>() != nullptr) == ST_WITH_PHYSICS);
		CHECK((scene.GetService<ICharacterControl>() != nullptr) == ST_WITH_PHYSICS);
		CHECK((scene.GetService<IAudioPlayback>() != nullptr) == ST_WITH_AUDIO);
		CHECK((scene.GetService<IAnimatorControl>() != nullptr) == ST_WITH_ANIMATION);
		scene.OnRuntimeStop();
		CHECK(scene.GetService<ICollisionEventSource>() == nullptr);
		CHECK(scene.GetService<IAudioPlayback>() == nullptr);

		// Simulate mode has physics but no audio.
		scene.OnRuntimeStart(SceneRuntimeMode::Simulate);
		CHECK((scene.GetService<IPhysicsQueries>() != nullptr) == ST_WITH_PHYSICS);
		CHECK(scene.GetService<IAudioPlayback>() == nullptr);
		scene.OnRuntimeStop();
		CHECK(scene.GetService<IPhysicsQueries>() == nullptr);
	}

#if ST_TESTS_HAVE_EVERY_MODULE
	TEST_CASE("Modules learn from the services what moves an entity and what it plays, without reading other modules' components")
	{
		Scene scene;
		Entity ground = CreateBox(scene, "Ground", glm::vec3(0.0f, -0.5f, 0.0f), false);
		Entity box = CreateBox(scene, "Box", glm::vec3(0.0f, 2.0f, 0.0f), true);
		Entity rider = scene.CreateEntity("Rider");
		scene.SetParent(rider, box);
		Entity lift = CreateBox(scene, "Lift", glm::vec3(4.0f, 2.0f, 0.0f), true);
		lift.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		Entity speaker = scene.CreateEntity("Speaker");
		speaker.AddComponent<AudioSourceComponent>();
		Entity dancer = scene.CreateEntity("Dancer");
		dancer.AddComponent<AnimatorComponent>();
		scene.OnRuntimeStart(SceneRuntimeMode::Play);

		// Physics: the body that moves an entity is its own or its nearest ancestor's; colliders alone are static bodies.
		IBodyMotionQuery* motion = scene.GetService<IBodyMotionQuery>();
		REQUIRE(motion != nullptr);
		CHECK(motion->GetSimulatingBody(box) == box);
		CHECK(motion->GetSimulatingBody(rider) == box);
		CHECK_FALSE(motion->GetSimulatingBody(ground));
		CHECK_FALSE(motion->GetSimulatingBody(speaker));
		CHECK(motion->IsDynamicBody(box));
		CHECK_FALSE(motion->IsDynamicBody(lift));
		CHECK_FALSE(motion->IsDynamicBody(ground));
		CHECK_FALSE(motion->IsDynamicBody(rider));

		// Audio and animation: what has a source and what has an animator.
		IAudioPlayback* audio = scene.GetService<IAudioPlayback>();
		REQUIRE(audio != nullptr);
		CHECK(audio->HasSource(speaker));
		CHECK_FALSE(audio->HasSource(box));
		IAnimatorControl* animation = scene.GetService<IAnimatorControl>();
		REQUIRE(animation != nullptr);
		CHECK(animation->HasAnimator(dancer));
		CHECK_FALSE(animation->HasAnimator(speaker));
		scene.OnRuntimeStop();
	}
#endif

	TEST_CASE("The audio module offers the engine's audio output to the whole process")
	{
		CHECK(EngineServices::Get<ICollisionEventSource>() == nullptr); // A scene service, never an engine one
#if ST_WITH_AUDIO
		// Registered by the composition root (the test process runs it before the suites).
		IAudioOutput* output = EngineServices::Get<IAudioOutput>();
		REQUIRE(output != nullptr);
		const float previous = output->GetMasterVolume();
		output->SetMasterVolume(0.5f);
		CHECK(output->GetMasterVolume() == 0.5f);
		output->SetMasterVolume(previous);
#else
		CHECK(EngineServices::Get<IAudioOutput>() == nullptr);
#endif
	}

#if ST_WITH_PHYSICS
	TEST_CASE("Listeners of watched entities hear only the contacts of watched entities")
	{
		Scene scene;
		Entity ground = CreateBox(scene, "Ground", glm::vec3(0.0f, -0.5f, 0.0f), false);
		Entity watchedBox = CreateBox(scene, "Watched", glm::vec3(-3.0f, 0.55f, 0.0f), true);
		Entity plainBox = CreateBox(scene, "Plain", glm::vec3(3.0f, 0.55f, 0.0f), true);
		scene.OnRuntimeStart(SceneRuntimeMode::Simulate);

		ICollisionEventSource* contacts = scene.GetService<ICollisionEventSource>();
		REQUIRE(contacts != nullptr);
		std::vector<CollisionEvent> all;
		std::vector<CollisionEvent> watched;
		const CollisionListenerID allListener = contacts->AddCollisionListener([&](const CollisionEvent& event) { all.push_back(event); },
			CollisionListenerScope::AllEntities);
		const CollisionListenerID watchedListener = contacts->AddCollisionListener([&](const CollisionEvent& event) { watched.push_back(event); },
			CollisionListenerScope::WatchedEntities);
		// Watches add up: one of two watches ending leaves the entity watched.
		contacts->WatchCollisions(watchedBox.GetUUID());
		contacts->WatchCollisions(watchedBox.GetUUID());
		contacts->UnwatchCollisions(watchedBox.GetUUID());
		contacts->UnwatchCollisions(plainBox.GetUUID()); // Never watched: no effect

		for (int frame = 0; frame < 10; frame++)
			scene.OnUpdateRuntime(scene.GetSettings().FixedTimestep);

		REQUIRE(all.size() == 2);
		REQUIRE(watched.size() == 1);
		CHECK(watched[0].Type == CollisionEventType::Begin);
		CHECK(watched[0].Involves(watchedBox.GetUUID()));
		CHECK(watched[0].Involves(ground.GetUUID()));

		CHECK(contacts->RemoveCollisionListener(allListener));
		CHECK(contacts->RemoveCollisionListener(watchedListener));
		CHECK_FALSE(contacts->RemoveCollisionListener(watchedListener));
		contacts->UnwatchCollisions(watchedBox.GetUUID());
		scene.OnRuntimeStop();
	}
#endif
}
