#include <doctest/doctest.h>

#include "Audio/AudioTestUtils.h"
#include "Scripting/ScriptTestUtils.h"
#include "Strata/Audio/AudioSource.h"
#include "Strata/Audio/AudioSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/SceneServices.h"
#include "Strata/Scene/SceneSystem.h"
#include "TestHelpers.h"

#include <glm/glm.hpp>
#include <glm/gtc/epsilon.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// The scene the physics test scripts expect (StrataTests/Scripts/API/PhysicsScripts.cpp), with "Tester" running
	// `className`.
	Entity CreatePhysicsScene(Scene& scene, const std::string& className)
	{
		Entity ground = scene.CreateEntity("Ground");
		ground.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
		ground.AddComponent<BoxColliderComponent>().HalfExtents = { 10.0f, 0.5f, 10.0f };

		Entity crate = scene.CreateEntity("Crate");
		crate.GetComponent<TransformComponent>().Translation = { 0.0f, 5.0f, 0.0f };
		RigidBodyComponent& crateBody = crate.AddComponent<RigidBodyComponent>();
		crateBody.GravityScale = 0.0f;
		crateBody.LinearDamping = 0.0f;
		crateBody.AngularDamping = 0.0f;
		crateBody.Layer = 2;
		crate.AddComponent<BoxColliderComponent>();

		Entity platform = scene.CreateEntity("Platform");
		platform.GetComponent<TransformComponent>().Translation = { 5.0f, 2.0f, 0.0f };
		platform.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		platform.AddComponent<BoxColliderComponent>();

		Entity zone = scene.CreateEntity("Zone");
		zone.GetComponent<TransformComponent>().Translation = { -5.0f, 1.0f, 0.0f };
		RigidBodyComponent& zoneBody = zone.AddComponent<RigidBodyComponent>();
		zoneBody.Type = RigidBodyType::Static;
		zoneBody.IsTrigger = true;
		zoneBody.Layer = 3;
		zone.AddComponent<BoxColliderComponent>().HalfExtents = { 1.0f, 1.0f, 1.0f };

		scene.CreateEntity("Plain");
		Entity tester = scene.CreateEntity("Tester");
		AddScriptEntry(tester, className);
		return tester;
	}

	bool Near(const glm::vec3& a, const glm::vec3& b, float epsilon = 1e-3f)
	{
		return glm::all(glm::epsilonEqual(a, b, epsilon));
	}

	// Scenes play without the physics system while this lives, as in a build without the physics module: the systems after
	// it in the update order, and those that name it in their order, are registered again without naming it meanwhile.
	// Everything is registered as it was afterwards.
	class ScopedWithoutPhysicsSystem
	{
	public:
		ScopedWithoutPhysicsSystem()
		{
			const std::vector<SceneSystemDescriptor>& all = SceneSystemRegistry::GetAll();
			const auto physics = std::find_if(all.begin(), all.end(), [](const SceneSystemDescriptor& descriptor) { return descriptor.Name == "Physics"; });
			REQUIRE(physics != all.end());
			for (const SceneSystemDescriptor& descriptor : all)
				m_Order.push_back(descriptor.Name);
			auto namesPhysics = [](const SceneSystemDescriptor& descriptor)
			{
				return std::find(descriptor.After.begin(), descriptor.After.end(), "Physics") != descriptor.After.end()
					|| std::find(descriptor.Before.begin(), descriptor.Before.end(), "Physics") != descriptor.Before.end();
			};
			// Copied before the registry changes: Physics, then the others in update order.
			m_Removed.push_back(*physics);
			for (auto it = all.begin(); it != all.end(); ++it)
			{
				if (it != physics && (it > physics || namesPhysics(*it)))
					m_Removed.push_back(*it);
			}

			// The systems naming Physics go first, then Physics itself.
			for (size_t index = m_Removed.size(); index-- > 1;)
				CHECK(SceneSystemRegistry::Unregister(m_Removed[index].Name));
			CHECK(SceneSystemRegistry::Unregister("Physics"));
			for (size_t index = 1; index < m_Removed.size(); index++)
			{
				SceneSystemDescriptor descriptor = m_Removed[index];
				std::erase(descriptor.After, "Physics");
				std::erase(descriptor.Before, "Physics");
				CHECK(SceneSystemRegistry::Register(std::move(descriptor)));
			}
		}

		~ScopedWithoutPhysicsSystem()
		{
			for (size_t index = m_Removed.size(); index-- > 1;)
				CHECK(SceneSystemRegistry::Unregister(m_Removed[index].Name));
			for (const SceneSystemDescriptor& descriptor : m_Removed)
				CHECK(SceneSystemRegistry::Register(descriptor));
			std::vector<std::string> order;
			for (const SceneSystemDescriptor& descriptor : SceneSystemRegistry::GetAll())
				order.push_back(descriptor.Name);
			CHECK(order == m_Order);
		}

		ScopedWithoutPhysicsSystem(const ScopedWithoutPhysicsSystem&) = delete;
		ScopedWithoutPhysicsSystem& operator=(const ScopedWithoutPhysicsSystem&) = delete;
	private:
		std::vector<SceneSystemDescriptor> m_Removed; // "Physics", then the systems after it or naming it, in the update order
		std::vector<std::string> m_Order;             // Every system's name in the update order
	};

}

TEST_SUITE("Scripting.Physics")
{
	TEST_CASE("Scripts push bodies, teleport them and query the physics world")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		const Entity tester = CreatePhysicsScene(scene, "PhysicsAPI");
		scene.OnRuntimeStart();
		RunFrames(scene, 4);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, tester, "PhysicsAPI", "Done"));
		CheckScriptChecks(system, tester, "PhysicsAPI", 39);
		CHECK_FALSE(engine->IsFaulted());

		// The engine sees what the script did: the platform where it was teleported, the crate moving as last pushed.
		PhysicsSystem* physics = scene.GetSystem<PhysicsSystem>();
		REQUIRE(physics != nullptr);
		const Entity platform = scene.FindEntityByName("Platform");
		CHECK(Near(glm::vec3(scene.GetWorldTransform(platform)[3]), glm::vec3(5.0f, 3.0f, 0.0f)));
		const Entity crate = scene.FindEntityByName("Crate");
		CHECK(Near(physics->GetLinearVelocity(crate), glm::vec3(0.0f, 0.0f, 1.0f)));
		CHECK(Near(physics->GetAngularVelocity(crate), glm::vec3(0.0f, -6.0f, 0.0f), 1e-2f));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Physics host functions refuse misuse harmlessly")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		const Entity tester = CreatePhysicsScene(scene, "PhysicsMisuse");
		scene.OnRuntimeStart();
		RunFrames(scene, 1);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, tester, "PhysicsMisuse", "Done"));
		CheckScriptChecks(system, tester, "PhysicsMisuse", 50);
		CHECK_FALSE(engine->IsFaulted());
		CHECK(Near(glm::vec3(scene.GetWorldTransform(scene.FindEntityByName("Crate"))[3]), glm::vec3(0.0f, 5.0f, 0.0f)));
		scene.OnRuntimeStop();
	}

	TEST_CASE("The SDK's physics functions degrade gracefully on engines without them")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		const Entity tester = CreatePhysicsScene(scene, "OlderEnginePhysics");
		scene.OnRuntimeStart();
		RunFrames(scene, 1);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, tester, "OlderEnginePhysics", "Done"));
		CheckScriptChecks(system, tester, "OlderEnginePhysics", 6);
		CHECK_FALSE(engine->IsFaulted());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Scripts use physics from OnCreate, also on bodies they spawn while the scene starts")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		const Entity tester = CreatePhysicsScene(scene, "PhysicsAtStart");
		scene.OnRuntimeStart();

		const ScriptSystem& system = GetScriptSystem(scene);
		CheckScriptChecks(system, tester, "PhysicsAtStart", 5);
		const Entity spawned = scene.FindEntityByName("Spawned");
		REQUIRE(spawned.IsValid());
		CHECK(GetField<UUID>(system, tester, "PhysicsAtStart", "Spawned") == spawned.GetUUID());
		CheckScriptChecks(system, spawned, "SpawnedBody", 1);
		PhysicsSystem* physics = scene.GetSystem<PhysicsSystem>();
		REQUIRE(physics != nullptr);
		CHECK(Near(physics->GetLinearVelocity(scene.FindEntityByName("Crate")), glm::vec3(1.0f, 0.0f, 0.0f)));
		CHECK(Near(physics->GetLinearVelocity(spawned), glm::vec3(0.0f, 0.0f, 2.0f)));
		CHECK_FALSE(engine->IsFaulted());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Scenes play without the physics system: scripts, contact watches and Doppler do without its services")
	{
		ScopedAudioEngine audioEngine;
		REQUIRE(audioEngine.Initialized);
		AudioProject project;
		const AssetHandle clip = project.AddClip(2.0f);
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		{
			ScopedWithoutPhysicsSystem withoutPhysics;
			Scene scene;
			const Entity tester = CreatePhysicsScene(scene, "PhysicsWithoutSystem");
			// The crate has what makes other systems use physics: a script with contact callbacks (which watches its contacts)
			// and a spatial sound on its rigid body (whose Doppler velocity is the body's).
			Entity crate = scene.FindEntityByName("Crate");
			AddScriptEntry(crate, "ContactRecorder");
			AudioSourceComponent& source = crate.AddComponent<AudioSourceComponent>();
			source.Clip = clip;
			source.Loop = true;

			const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
			scene.OnRuntimeStart();
			CHECK(scene.GetSystem<PhysicsSystem>() == nullptr);
			CHECK(scene.GetService<ICollisionEventSource>() == nullptr);
			CHECK(scene.GetService<IBodyMotionQuery>() == nullptr);
			CHECK(scene.GetService<IBodyControl>() == nullptr);
			CHECK(scene.GetService<IPhysicsQueries>() == nullptr);
			CHECK(scene.GetService<IAudioPlayback>() != nullptr);

			// Nothing falls: the crate moves only where it is put, and its sound's velocity comes from those moves.
			const float timestep = scene.GetSettings().FixedTimestep;
			for (int frame = 0; frame < 60; frame++)
			{
				crate.GetTransform().Translation.x += 0.1f;
				crate.MarkModified<TransformComponent>();
				RunFrames(scene, 1, timestep);
			}
			CHECK(crate.GetTransform().Translation.y == 5.0f);

			const ScriptSystem& system = GetScriptSystem(scene);
			CHECK(GetField<bool>(system, tester, "PhysicsWithoutSystem", "Done"));
			CheckScriptChecks(system, tester, "PhysicsWithoutSystem", 10);
			CHECK(GetField<int32_t>(system, crate, "ContactRecorder", "CollisionEnters") == 0);
			CHECK(CountLogMessages(logStart, "'Crate' has no body in the physics simulation") > 0);
			CHECK_FALSE(engine->IsFaulted());

			AudioSystem* audio = scene.GetSystem<AudioSystem>();
			REQUIRE(audio != nullptr);
			CHECK(audio->IsPlaying(crate));
			const AudioSource* voice = audio->GetAudioSource(crate);
			REQUIRE(voice != nullptr);
			CHECK(voice->GetVelocity().x == doctest::Approx(0.1f / timestep).epsilon(0.01));
			CHECK(voice->GetVelocity().y == 0.0f);
			scene.OnRuntimeStop();
			CHECK_FALSE(scene.IsRunning());
		}

		// Registered again, the physics system runs in the scenes that play next.
		Scene scene;
		scene.OnRuntimeStart();
		CHECK(scene.GetSystem<PhysicsSystem>() != nullptr);
		CHECK(scene.GetService<ICollisionEventSource>() != nullptr);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Physics functions find nothing in scenes without physics, until a physics component appears")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity tester = scene.CreateEntity("Tester");
		AddScriptEntry(tester, "PhysicsWithoutWorld");
		scene.OnRuntimeStart();
		REQUIRE(scene.GetSystem<PhysicsSystem>() != nullptr);
		CHECK(scene.GetSystem<PhysicsSystem>()->GetWorld() == nullptr);
		RunFrames(scene, 1);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, tester, "PhysicsWithoutWorld", "Done"));
		CheckScriptChecks(system, tester, "PhysicsWithoutWorld", 13);
		CHECK(scene.GetSystem<PhysicsSystem>()->GetWorld() != nullptr);
		CHECK(Near(glm::vec3(scene.GetWorldTransform(tester)[3]), glm::vec3(0.0f)));
		scene.OnRuntimeStop();
	}
}
