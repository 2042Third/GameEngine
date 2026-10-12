#include <doctest/doctest.h>

#include "Physics/PhysicsTestUtils.h"
#include "Scene/SceneTestUtils.h"
#include "Scripting/ScriptTestUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Scene/SceneServices.h"
#include "TestHelpers.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A character controller standing at `position` (its feet): 1.8 m tall, 0.3 m in radius.
	Entity CreateCharacter(Scene& scene, const std::string& name, const glm::vec3& position, float stepHeight = 0.35f)
	{
		Entity entity = scene.CreateEntity(name);
		entity.GetTransform().Translation = position;
		CharacterControllerComponent& controller = entity.AddComponent<CharacterControllerComponent>();
		controller.Height = 1.8f;
		controller.Radius = 0.3f;
		controller.StepHeight = stepHeight;
		return entity;
	}

	ICharacterControl& GetCharacters(Scene& scene)
	{
		ICharacterControl* characters = scene.GetService<ICharacterControl>();
		REQUIRE(characters != nullptr);
		return *characters;
	}

	CharacterState GetState(Scene& scene, Entity character)
	{
		const std::optional<CharacterState> state = GetCharacters(scene).GetCharacterState(character);
		REQUIRE(state.has_value());
		return *state;
	}

	// A walkable (or not) ramp rising along +X from (startX, 0): a long thin box tilted by `degrees` around Z.
	Entity CreateRamp(Scene& scene, const std::string& name, float startX, float degrees)
	{
		const float angle = glm::radians(degrees);
		const glm::vec3 halfExtents(5.0f, 0.25f, 3.0f);
		Entity ramp = scene.CreateEntity(name);
		// The top face's low edge lies on the ground at x = startX.
		ramp.GetTransform().Translation = glm::vec3(startX + halfExtents.x * std::cos(angle) + halfExtents.y * std::sin(angle),
			halfExtents.x * std::sin(angle) - halfExtents.y * std::cos(angle), 0.0f);
		ramp.GetTransform().Rotation = glm::angleAxis(angle, glm::vec3(0.0f, 0.0f, 1.0f));
		ramp.AddComponent<BoxColliderComponent>().HalfExtents = halfExtents;
		return ramp;
	}

	// Walks a character along +X at `speed` for `steps` fixed steps and returns where it ends.
	glm::vec3 Walk(Scene& scene, Entity character, float speed, uint32_t steps)
	{
		REQUIRE(GetCharacters(scene).MoveCharacter(character, glm::vec3(speed, 0.0f, 0.0f)));
		StepScene(scene, steps);
		return GetWorldPosition(scene, character);
	}

}

TEST_SUITE("Physics.Character")
{
	TEST_CASE("A character controller stands on the ground and falls without it")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		Entity standing = CreateCharacter(scene, "Standing", glm::vec3(0.0f, 0.0f, 0.0f));
		Entity falling = CreateCharacter(scene, "Falling", glm::vec3(80.0f, 5.0f, 0.0f)); // Beyond the ground
		scene.OnRuntimeStart();
		ICharacterControl& characters = GetCharacters(scene);
		CHECK(characters.HasCharacterController(standing));
		CHECK(GetPhysics(scene).GetStats().CharacterControllerCount == 2);

		StepScene(scene, 60);
		const glm::vec3 position = GetWorldPosition(scene, standing);
		CHECK(std::abs(position.y) < 0.01f);
		CHECK(std::abs(position.x) < 1.0e-4f);
		const CharacterState state = GetState(scene, standing);
		CHECK(state.Ground == CharacterGroundState::OnGround);
		CHECK(Math::IsNearlyEqual(state.GroundNormal, glm::vec3(0.0f, 1.0f, 0.0f), 1.0e-3f));
		CHECK(state.GroundEntity == ground);
		CHECK(glm::length(state.Velocity) < 0.01f);

		// One second of free fall from 5 m: 5 - g/2 = 0.1 m.
		CHECK(GetState(scene, falling).Ground == CharacterGroundState::InAir);
		CHECK(GetWorldPosition(scene, falling).y < 0.5f);
		CHECK(GetState(scene, falling).Velocity.y < -9.0f);
		CheckSceneCaches(scene);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Characters walk up steps up to their step height and not higher ones")
	{
		Scene scene;
		CreateGround(scene);
		// Steps across the walk from x = 2 to x = 6: a 0.3 m step, and a 0.5 m step for a second character 10 m further along Z.
		CreateStaticBox(scene, "Low Step", glm::vec3(4.0f, 0.15f, 0.0f), glm::vec3(2.0f, 0.15f, 2.0f));
		CreateStaticBox(scene, "High Step", glm::vec3(4.0f, 0.25f, 10.0f), glm::vec3(2.0f, 0.25f, 2.0f));
		Entity climber = CreateCharacter(scene, "Climber", glm::vec3(0.0f, 0.0f, 0.0f), 0.35f);
		Entity blocked = CreateCharacter(scene, "Blocked", glm::vec3(0.0f, 0.0f, 10.0f), 0.35f);
		scene.OnRuntimeStart();
		REQUIRE(GetCharacters(scene).MoveCharacter(blocked, glm::vec3(2.0f, 0.0f, 0.0f)));

		const glm::vec3 up = Walk(scene, climber, 2.0f, 120);
		CHECK(up.x > 3.0f);
		CHECK(std::abs(up.y - 0.3f) < 0.02f);
		CHECK(GetState(scene, climber).Ground == CharacterGroundState::OnGround);

		const glm::vec3 stopped = GetWorldPosition(scene, blocked);
		CHECK(stopped.x < 2.0f - 0.3f + 0.05f);
		CHECK(std::abs(stopped.y) < 0.02f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Characters walk up walkable slopes and stop at steeper ones")
	{
		Scene scene;
		CreateGround(scene);
		CreateRamp(scene, "Gentle", 2.0f, 30.0f);
		Entity steep = CreateRamp(scene, "Steep", 2.0f, 60.0f);
		steep.GetTransform().Translation.z = 10.0f;
		Entity walker = CreateCharacter(scene, "Walker", glm::vec3(0.0f, 0.0f, 0.0f));
		Entity stopped = CreateCharacter(scene, "Stopped", glm::vec3(0.0f, 0.0f, 10.0f));
		for (Entity character : { walker, stopped })
			character.GetComponent<CharacterControllerComponent>().MaxSlopeDegrees = 45.0f;
		scene.OnRuntimeStart();
		REQUIRE(GetCharacters(scene).MoveCharacter(stopped, glm::vec3(2.0f, 0.0f, 0.0f)));

		const glm::vec3 climbed = Walk(scene, walker, 2.0f, 120);
		CHECK(climbed.y > 0.5f); // Two meters along a 30 degree slope rise more than a meter
		CHECK(GetState(scene, walker).Ground == CharacterGroundState::OnGround);
		CHECK(std::abs(GetState(scene, walker).GroundNormal.y - std::cos(glm::radians(30.0f))) < 0.02f);

		const glm::vec3 held = GetWorldPosition(scene, stopped);
		CHECK(held.y < 0.2f);
		CHECK(held.x < 2.3f);

		// Put on the steep slope, it cannot stand: it slides down.
		Entity slider = CreateCharacter(scene, "Slider", glm::vec3(0.0f, 0.0f, 10.0f));
		slider.GetComponent<CharacterControllerComponent>().MaxSlopeDegrees = 45.0f;
		const float startX = 2.0f + 1.5f;
		const float surfaceY = 1.5f * std::tan(glm::radians(60.0f));
		slider.GetTransform().Translation = glm::vec3(startX, surfaceY + 0.05f, 10.0f);
		slider.MarkModified<TransformComponent>();
		StepScene(scene, 2);
		const CharacterState onSlope = GetState(scene, slider);
		CHECK(onSlope.Ground == CharacterGroundState::OnSteepGround);
		CHECK(std::abs(onSlope.GroundNormal.y - std::cos(glm::radians(60.0f))) < 0.05f);
		StepScene(scene, 30);
		CHECK(GetWorldPosition(scene, slider).y < surfaceY - 0.3f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Jumps leave the ground and land again")
	{
		Scene scene;
		CreateGround(scene);
		Entity jumper = CreateCharacter(scene, "Jumper", glm::vec3(0.0f, 0.0f, 0.0f));
		scene.OnRuntimeStart();
		StepScene(scene, 10);
		REQUIRE(GetState(scene, jumper).Ground == CharacterGroundState::OnGround);

		REQUIRE(GetCharacters(scene).JumpCharacter(jumper, 5.0f));
		StepScene(scene, 1);
		CHECK(GetState(scene, jumper).Ground == CharacterGroundState::InAir);
		CHECK(GetState(scene, jumper).Velocity.y > 4.0f);
		StepScene(scene, 20); // A third of a second: still rising (5 - 9.81 / 3 > 0)
		CHECK(GetWorldPosition(scene, jumper).y > 1.0f);
		CHECK(GetState(scene, jumper).GroundNormal == glm::vec3(0.0f));
		CHECK_FALSE(GetState(scene, jumper).GroundEntity.IsValid());
		StepScene(scene, 60);
		CHECK(GetState(scene, jumper).Ground == CharacterGroundState::OnGround);
		CHECK(std::abs(GetWorldPosition(scene, jumper).y) < 0.01f);

		// A ceiling stops the rise.
		CreateStaticBox(scene, "Ceiling", glm::vec3(0.0f, 2.3f, 0.0f), glm::vec3(2.0f, 0.2f, 2.0f));
		StepScene(scene, 1);
		REQUIRE(GetCharacters(scene).JumpCharacter(jumper, 8.0f));
		StepScene(scene, 30);
		CHECK(GetWorldPosition(scene, jumper).y < 0.3f); // Bumped its head at 2.1 - 1.8 = 0.3 m, then fell back
		scene.OnRuntimeStop();
	}

	TEST_CASE("Characters push dynamic bodies, as hard as their push strength")
	{
		Scene scene;
		CreateGround(scene);
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(1.5f, 0.5f, 0.0f));
		box.GetComponent<RigidBodyComponent>().Mass = 5.0f;
		Entity heavy = CreateDynamicBox(scene, "Heavy", glm::vec3(1.5f, 0.5f, 10.0f));
		heavy.GetComponent<RigidBodyComponent>().Mass = 5.0f;
		Entity pusher = CreateCharacter(scene, "Pusher", glm::vec3(0.0f, 0.0f, 0.0f));
		Entity weak = CreateCharacter(scene, "Weak", glm::vec3(0.0f, 0.0f, 10.0f));
		weak.GetComponent<CharacterControllerComponent>().PushStrength = 0.0f;
		scene.OnRuntimeStart();
		REQUIRE(GetCharacters(scene).MoveCharacter(weak, glm::vec3(2.0f, 0.0f, 0.0f)));

		Walk(scene, pusher, 2.0f, 120);
		CHECK(GetWorldPosition(scene, box).x > 3.0f);
		CHECK(GetWorldPosition(scene, pusher).x > 2.0f);
		// Without strength it walks against the box like against a wall.
		CHECK(std::abs(GetWorldPosition(scene, heavy).x - 1.5f) < 0.05f);
		CHECK(GetWorldPosition(scene, weak).x < 1.5f - 0.5f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A character presses on a dynamic body it stands on with its weight, and across the surface where it slides")
	{
		// Heavy rafts that gravity does not move and that cannot turn: only the characters' weight moves them.
		constexpr float c_CharacterMass = 70.0f; // What characters weigh
		constexpr float c_RaftMass = 700.0f;
		constexpr float c_SteepDegrees = 60.0f;
		Scene scene;
		const auto createRaft = [&scene](const std::string& name, const glm::vec3& position, float degrees)
		{
			Entity raft = CreateDynamicBox(scene, name, position, glm::vec3(5.0f, 0.25f, 3.0f));
			raft.GetTransform().Rotation = glm::angleAxis(glm::radians(degrees), glm::vec3(0.0f, 0.0f, 1.0f));
			RigidBodyComponent& body = raft.GetComponent<RigidBodyComponent>();
			body.Mass = c_RaftMass;
			body.GravityScale = 0.0f;
			body.LinearDamping = 0.0f;
			body.LockRotationX = true;
			body.LockRotationY = true;
			body.LockRotationZ = true;
			return raft;
		};
		Entity flat = createRaft("Flat", glm::vec3(0.0f, -0.25f, 0.0f), 0.0f);
		Entity steep = createRaft("Steep", glm::vec3(0.0f, 0.0f, 20.0f), c_SteepDegrees);
		Entity rider = CreateCharacter(scene, "Rider", glm::vec3(0.0f, 0.0f, 0.0f));
		// Above the steep raft's top face, its sphere clear of the face (the face's normal tilts the gap).
		const float angle = glm::radians(c_SteepDegrees);
		const glm::vec3 normal(-std::sin(angle), std::cos(angle), 0.0f);
		const glm::vec3 face = glm::vec3(0.0f, 0.0f, 20.0f) + normal * 0.25f;
		Entity slider = CreateCharacter(scene, "Slider", face + glm::vec3(0.0f, (0.3f + 0.05f) / std::cos(angle) - 0.3f, 0.0f));
		// The slider's contact pushes nothing: what moves its raft is its weight alone.
		slider.GetComponent<CharacterControllerComponent>().PushStrength = 0.0f;
		slider.GetComponent<CharacterControllerComponent>().MaxSlopeDegrees = 45.0f;
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		const float gravity = 9.81f;
		REQUIRE(scene.GetSettings().Gravity == glm::vec3(0.0f, -gravity, 0.0f));

		// Standing, all of its weight presses down: the raft gains 70 kg * g / 700 kg of downward speed per second.
		StepScene(scene, 10);
		REQUIRE(GetState(scene, rider).Ground == CharacterGroundState::OnGround);
		const glm::vec3 flatBefore = physics.GetLinearVelocity(flat);
		constexpr uint32_t c_Steps = 30;
		const float seconds = static_cast<float>(c_Steps) * scene.GetSettings().FixedTimestep;
		StepScene(scene, c_Steps);
		CHECK(GetState(scene, rider).Ground == CharacterGroundState::OnGround);
		const glm::vec3 flatGained = physics.GetLinearVelocity(flat) - flatBefore;
		CHECK(flatGained.y == doctest::Approx(-c_CharacterMass * gravity / c_RaftMass * seconds).epsilon(0.05));
		CHECK(std::abs(flatGained.x) < 1.0e-4f);

		// Sliding down ground too steep to stand on, only the part of its weight across the surface presses, into it: the
		// raft moves along the surface's normal, sideways as well as down.
		const CharacterState sliding = GetState(scene, slider);
		REQUIRE(sliding.Ground == CharacterGroundState::OnSteepGround);
		CHECK(std::abs(sliding.GroundNormal.y - std::cos(angle)) < 0.05f);
		const glm::vec3 steepBefore = physics.GetLinearVelocity(steep);
		StepScene(scene, 10);
		REQUIRE(GetState(scene, slider).Ground == CharacterGroundState::OnSteepGround);
		const glm::vec3 steepGained = physics.GetLinearVelocity(steep) - steepBefore;
		const float expected = c_CharacterMass * gravity * std::cos(angle) / c_RaftMass * 10.0f * scene.GetSettings().FixedTimestep;
		CHECK(glm::length(steepGained) == doctest::Approx(expected).epsilon(0.1));
		CHECK(glm::dot(glm::normalize(steepGained), -normal) > 0.99f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Characters collide only with layers their layer collides with")
	{
		Scene scene;
		CreateGround(scene);
		Entity wall = CreateStaticBox(scene, "Wall", glm::vec3(2.0f, 1.0f, 0.0f), glm::vec3(0.2f, 1.0f, 20.0f));
		wall.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		wall.GetComponent<RigidBodyComponent>().Layer = 2;
		Entity ghost = CreateCharacter(scene, "Ghost", glm::vec3(0.0f, 0.0f, 0.0f));
		ghost.GetComponent<CharacterControllerComponent>().Layer = 1;
		Entity solid = CreateCharacter(scene, "Solid", glm::vec3(0.0f, 0.0f, 5.0f));
		solid.GetComponent<CharacterControllerComponent>().Layer = 3;
		scene.OnRuntimeStart();
		GetPhysics(scene).SetLayersCollide(1, 2, false);
		REQUIRE(GetCharacters(scene).MoveCharacter(solid, glm::vec3(2.0f, 0.0f, 0.0f)));

		CHECK(Walk(scene, ghost, 2.0f, 120).x > 3.0f);
		CHECK(GetWorldPosition(scene, solid).x < 2.0f - 0.2f - 0.3f + 0.05f);

		// The layer applies to its presence body too: queries of the layer find it, others do not.
		const glm::vec3 above = GetWorldPosition(scene, solid) + glm::vec3(0.0f, 5.0f, 0.0f);
		const std::optional<RaycastHit> hit = GetPhysics(scene).Raycast(above, glm::vec3(0.0f, -1.0f, 0.0f), 10.0f, ST_BIT(3));
		REQUIRE(hit.has_value());
		CHECK(hit->HitEntity == solid);
		CHECK_FALSE(GetPhysics(scene).Raycast(above, glm::vec3(0.0f, -1.0f, 0.0f), 10.0f, ST_BIT(1)).has_value());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Contacts of characters reach collision listeners; triggers and bodies meet their presence body")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		Entity wall = CreateStaticBox(scene, "Wall", glm::vec3(3.0f, 1.0f, 0.0f), glm::vec3(0.2f, 1.0f, 2.0f));
		Entity trigger = CreateStaticBox(scene, "Trigger", glm::vec3(1.5f, 1.0f, 0.0f), glm::vec3(0.3f, 1.0f, 2.0f));
		trigger.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		trigger.GetComponent<RigidBodyComponent>().IsTrigger = true;
		Entity character = CreateCharacter(scene, "Character", glm::vec3(0.0f, 0.0f, 0.0f));
		scene.OnRuntimeStart();
		CollisionRecorder recorder(GetPhysics(scene));

		StepScene(scene, 10);
		CHECK(recorder.Count(CollisionEventType::Begin, character.GetUUID(), ground.GetUUID()) == 1);
		Walk(scene, character, 2.0f, 90);
		CHECK(recorder.Count(CollisionEventType::Begin, character.GetUUID(), wall.GetUUID()) == 1);
		const CollisionEvent* entered = recorder.Find(CollisionEventType::Begin, character.GetUUID(), trigger.GetUUID());
		REQUIRE(entered != nullptr);
		CHECK(entered->IsTrigger);
		CHECK(recorder.Count(CollisionEventType::End, character.GetUUID(), trigger.GetUUID()) == 1); // Walked through it
		CHECK(recorder.Count(CollisionEventType::End, character.GetUUID(), ground.GetUUID()) == 0);

		// A box dropped on its head lands on it.
		REQUIRE(GetCharacters(scene).MoveCharacter(character, glm::vec3(0.0f)));
		REQUIRE(GetCharacters(scene).TeleportCharacter(character, glm::vec3(-5.0f, 0.0f, 0.0f)));
		CHECK(Math::IsNearlyEqual(GetWorldPosition(scene, character), glm::vec3(-5.0f, 0.0f, 0.0f)));
		Entity falling = CreateDynamicBox(scene, "Falling", glm::vec3(-5.0f, 4.0f, 0.0f), glm::vec3(0.2f));
		StepScene(scene, 90);
		CHECK(recorder.Count(CollisionEventType::Begin, character.GetUUID(), falling.GetUUID()) == 1);
		CHECK(GetWorldPosition(scene, falling).y > 1.5f);

		// Removing the controller ends its contacts.
		character.RemoveComponent<CharacterControllerComponent>();
		StepScene(scene, 1);
		CHECK_FALSE(GetCharacters(scene).HasCharacterController(character));
		CHECK(recorder.Count(CollisionEventType::End, character.GetUUID(), ground.GetUUID()) == 1);
		CHECK(recorder.Count(CollisionEventType::Begin) == recorder.Count(CollisionEventType::End));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Controllers follow their component, activity and entity")
	{
		Scene scene;
		CreateGround(scene);
		Entity parent = scene.CreateEntity("Parent");
		parent.GetTransform().Translation = glm::vec3(10.0f, 0.0f, 0.0f);
		parent.GetTransform().Scale = glm::vec3(2.0f);
		Entity character = CreateCharacter(scene, "Character", glm::vec3(0.0f));
		scene.SetParent(character, parent, false);
		scene.OnRuntimeStart();
		ICharacterControl& characters = GetCharacters(scene);
		REQUIRE(characters.HasCharacterController(character));

		// Below a scaled parent it writes its local translation so that its world position is where it walked.
		Walk(scene, character, 1.0f, 60);
		CHECK(std::abs(GetWorldPosition(scene, character).x - 11.0f) < 0.02f);
		CHECK(std::abs(character.GetTransform().Translation.x - 0.5f) < 0.01f);
		CheckSceneCaches(scene);

		// Moved from outside: it continues from there.
		REQUIRE(characters.MoveCharacter(character, glm::vec3(0.0f)));
		character.GetTransform().Translation = glm::vec3(-2.0f, 0.0f, 0.0f);
		character.MarkModified<TransformComponent>();
		StepScene(scene, 5);
		CHECK(Math::IsNearlyEqual(GetWorldPosition(scene, character), glm::vec3(6.0f, 0.0f, 0.0f), 0.01f));

		// Disabled, deactivated or without its component it is no controller, and its entity stays where it is.
		character.GetComponent<CharacterControllerComponent>().Enabled = false;
		character.MarkModified<CharacterControllerComponent>();
		StepScene(scene, 1);
		CHECK_FALSE(characters.HasCharacterController(character));
		CHECK_FALSE(characters.MoveCharacter(character, glm::vec3(1.0f, 0.0f, 0.0f)));
		CHECK_FALSE(characters.GetCharacterState(character).has_value());
		character.GetComponent<CharacterControllerComponent>().Enabled = true;
		character.MarkModified<CharacterControllerComponent>();
		StepScene(scene, 1);
		CHECK(characters.HasCharacterController(character));

		parent.AddComponent<InactiveComponent>();
		StepScene(scene, 1);
		CHECK_FALSE(characters.HasCharacterController(character));
		parent.RemoveComponent<InactiveComponent>();
		StepScene(scene, 1);
		CHECK(characters.HasCharacterController(character));
		CHECK(Math::IsNearlyEqual(GetWorldPosition(scene, character), glm::vec3(6.0f, 0.0f, 0.0f), 0.01f));

		// A rigid body takes over (with a warning).
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		character.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		StepScene(scene, 1);
		CHECK_FALSE(characters.HasCharacterController(character));
		CHECK(CountLogMessages(logStart, "has both a Rigid Body and a Character Controller") == 1);
		character.RemoveComponent<RigidBodyComponent>();
		StepScene(scene, 1);
		CHECK(characters.HasCharacterController(character));

		// Shape edits rebuild it in place.
		character.GetComponent<CharacterControllerComponent>().Height = 1.2f;
		character.MarkModified<CharacterControllerComponent>();
		StepScene(scene, 5);
		CHECK(characters.HasCharacterController(character));
		CHECK(Math::IsNearlyEqual(GetWorldPosition(scene, character), glm::vec3(6.0f, 0.0f, 0.0f), 0.01f));

		scene.DestroyEntity(character);
		StepScene(scene, 1);
		CHECK(GetPhysics(scene).GetStats().CharacterControllerCount == 0);
		CheckSceneCaches(scene);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Invalid requests are refused")
	{
		Scene scene;
		CreateGround(scene);
		Entity character = CreateCharacter(scene, "Character", glm::vec3(0.0f));
		Entity other = scene.CreateEntity("Other");
		scene.OnRuntimeStart();
		ICharacterControl& characters = GetCharacters(scene);
		const float nan = std::nanf("");
		CHECK_FALSE(characters.MoveCharacter(character, glm::vec3(nan, 0.0f, 0.0f)));
		CHECK_FALSE(characters.JumpCharacter(character, std::numeric_limits<float>::infinity()));
		CHECK_FALSE(characters.TeleportCharacter(character, glm::vec3(0.0f, nan, 0.0f)));
		CHECK_FALSE(characters.AddCharacterMotion(character, glm::vec3(0.0f, 0.0f, nan)));
		CHECK_FALSE(characters.MoveCharacter(other, glm::vec3(1.0f)));
		CHECK_FALSE(characters.JumpCharacter(Entity(), 1.0f));
		StepScene(scene, 5);
		CHECK(std::abs(GetWorldPosition(scene, character).y) < 0.01f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Root motion moves characters like walking")
	{
		Scene scene;
		CreateGround(scene);
		CreateStaticBox(scene, "Wall", glm::vec3(1.0f, 1.0f, 0.0f), glm::vec3(0.2f, 1.0f, 2.0f));
		Entity character = CreateCharacter(scene, "Character", glm::vec3(0.0f));
		scene.OnRuntimeStart();
		ICharacterControl& characters = GetCharacters(scene);
		StepScene(scene, 2);
		for (int step = 0; step < 30; step++)
		{
			REQUIRE(characters.AddCharacterMotion(character, glm::vec3(0.05f, 0.0f, 0.0f)));
			StepScene(scene, 1);
		}
		// 1.5 m of motion, stopped by the wall at 1 - 0.2 - 0.3.
		CHECK(std::abs(GetWorldPosition(scene, character).x - 0.5f) < 0.05f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Scripts walk, jump and teleport characters through the SDK")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		CreateGround(scene);
		Entity hero = CreateCharacter(scene, "Hero", glm::vec3(0.0f));
		scene.CreateEntity("Plain");
		Entity tester = scene.CreateEntity("Tester");
		AddScriptEntry(tester, "CharacterAPI");
		Entity misuser = scene.CreateEntity("Misuser");
		AddScriptEntry(misuser, "CharacterMisuse");
		scene.OnRuntimeStart();
		StepScene(scene, 110);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, tester, "CharacterAPI", "Done"));
		CheckScriptChecks(system, tester, "CharacterAPI", 17);
		CHECK(GetField<bool>(system, misuser, "CharacterMisuse", "Done"));
		CheckScriptChecks(system, misuser, "CharacterMisuse", 16);
		CHECK_FALSE(engine->IsFaulted());
		CHECK(Math::IsNearlyEqual(GetWorldPosition(scene, hero), glm::vec3(0.0f, 0.0f, 5.0f), 0.02f));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Characters move the same with any number of threads")
	{
		const auto simulate = [](uint32_t workers)
		{
			Scope<ScopedJobSystem> jobs = workers > 0 ? CreateScope<ScopedJobSystem>(workers) : nullptr;
			Scene scene;
			CreateGround(scene);
			CreateStaticBox(scene, "Step", glm::vec3(6.0f, 0.15f, 5.0f), glm::vec3(2.0f, 0.15f, 8.0f));
			CreateRamp(scene, "Ramp", 10.0f, 20.0f);
			for (uint32_t index = 0; index < 12; index++)
				CreateDynamicBox(scene, "Crate " + std::to_string(index), glm::vec3(3.0f + static_cast<float>(index % 4), 0.5f, static_cast<float>(index / 4) * 3.0f), glm::vec3(0.3f));
			std::vector<Entity> characters;
			for (uint32_t index = 0; index < 48; index++)
				characters.push_back(CreateCharacter(scene, "Character " + std::to_string(index), glm::vec3(0.0f, 0.0f, static_cast<float>(index) * 0.75f)));
			scene.OnRuntimeStart();
			ICharacterControl& control = GetCharacters(scene);
			for (uint32_t index = 0; index < characters.size(); index++)
				REQUIRE(control.MoveCharacter(characters[index], glm::vec3(2.0f, 0.0f, (index % 3 == 0) ? 0.5f : -0.25f)));
			StepScene(scene, 60);
			REQUIRE(control.JumpCharacter(characters[5], 4.0f));
			StepScene(scene, 120);
			std::vector<glm::mat4> poses;
			for (const Entity entity : scene.GetEntitiesInHierarchyOrder())
				poses.push_back(scene.GetWorldTransform(entity));
			scene.OnRuntimeStop();
			return poses;
		};

		const std::vector<glm::mat4> serial = simulate(0);
		const std::vector<glm::mat4> parallel = simulate(4);
		REQUIRE(serial.size() == parallel.size());
		size_t different = 0;
		for (size_t index = 0; index < serial.size(); index++)
		{
			if (serial[index] != parallel[index])
				different++;
		}
		CHECK(different == 0);
	}
}
