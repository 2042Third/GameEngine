#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/UUID.h"
#include "Strata/Physics/PhysicsTypes.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneServices.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace JPH
{
	class PhysicsSystem;
}

namespace Strata
{

	class Scene;
	struct CharacterControllerSetData;

	// A Jolt body ID (index and sequence number) that names no body.
	constexpr uint32_t c_NoCharacterBody = 0xFFFFFFFFu;

	// A body a character controller touched during its last move (the ground, a wall, a box it pushed). Triggers are not
	// among them: the controller's presence body meets those (see CharacterControllerSet).
	struct CharacterContactReport
	{
		UUID Controller = UUID::Null();      // The controller's entity
		UUID Other = UUID::Null();           // The entity owning the body it touched
		uint32_t ControllerBody = c_NoCharacterBody; // Its presence body (Jolt body ID; c_NoCharacterBody: it has none)
		uint32_t OtherBody = c_NoCharacterBody;
		glm::vec3 Point = glm::vec3(0.0f);   // World space
		glm::vec3 Normal = glm::vec3(0.0f);  // From the controller towards the other
	};

	// A presence body that appeared, changed or went (Jolt body IDs; c_NoCharacterBody: none).
	struct CharacterBodyChange
	{
		UUID Entity = UUID::Null();
		uint32_t OldBody = c_NoCharacterBody;
		uint32_t NewBody = c_NoCharacterBody;
		bool Removed = false; // The controller left the simulation (its contacts end)
	};

	// What a step of the controllers did, for the world that owns them.
	struct CharacterStepOutput
	{
		std::vector<entt::entity> Moved;               // Entities whose transform the step wrote, in controller order
		std::vector<CharacterContactReport> Contacts;  // Only when collected (see Step)
		std::vector<UUID> WithoutPresenceBody;         // Controllers without a presence body: their contacts are checked every step

		void Clear()
		{
			Moved.clear();
			Contacts.clear();
			WithoutPresenceBody.clear();
		}
	};

	// The character controllers of a physics world (internal to the Physics module; see PhysicsWorld and
	// CharacterControllerComponent).
	//
	// Every entity with an active, enabled CharacterControllerComponent and no RigidBodyComponent is a controller: a Jolt
	// CharacterVirtual (an upright capsule moved by collision queries, not by the solver) whose base is the entity's origin,
	// plus a kinematic "presence" body of 90% of its size, which lets dynamic bodies land on it, triggers detect it and
	// queries hit it (it carries the entity's UUID like any body). The controller collides with the bodies on layers its
	// layer collides with, and with other controllers' presence bodies; never with its own entity's bodies.
	//
	// A step (Step, once per fixed step before Jolt's) moves every controller with the velocity its script gave it
	// (MoveCharacter), its jump, root motion and gravity: Jolt's ExtendedUpdate slides it along walls, walks it up steps up
	// to StepHeight and keeps it on the ground walking down. The moves run in parallel on the job system, each reading only
	// the world as it was before the step and writing only its own controller: they do not see each other's new positions
	// in that step, and pushing dynamic bodies is not done during the moves (Jolt would add the impulses from many threads in
	// any order) but afterwards, on the main thread, in controller order, together with writing each entity's translation
	// (its rotation and scale stay; Scene::MarkTransformsChanged) and moving the presence bodies. So the results do not depend
	// on the number of threads. An entity moved from outside (a script writing its transform, the editor) teleports its
	// controller there at the next step.
	//
	// Main thread only, except for what Step runs on the job system itself.
	class CharacterControllerSet
	{
	public:
		CharacterControllerSet(Scene& scene, JPH::PhysicsSystem& system, const PhysicsSettings& settings);
		~CharacterControllerSet(); // Destroys every controller and presence body

		CharacterControllerSet(const CharacterControllerSet&) = delete;
		CharacterControllerSet& operator=(const CharacterControllerSet&) = delete;

		// The entities of a scene that may become controllers (enabled components), for sizing a world's capacities.
		static uint32_t CountCandidates(const Scene& scene);

		// Creates, rebuilds and removes controllers after the changes recorded from the registry's signals (components added,
		// edited or removed, entities destroyed, activated or deactivated, rigid bodies added to controller entities) and
		// destructions requested while the scene updates. Appends what happened to presence bodies. Cheap without changes.
		void ApplyChanges(std::vector<CharacterBodyChange>& outChanges);

		// Moves every controller by one fixed step (see the class description). `gravity` is the world's; with
		// collectContacts the bodies each controller touched are reported (otherwise nothing is gathered).
		void Step(float timestep, const glm::vec3& gravity, bool collectContacts, CharacterStepOutput& output);

		//////////////////////////////////////////////////////////////////////////
		// Gameplay (see ICharacterControl; entities without a controller get false or nothing)
		//////////////////////////////////////////////////////////////////////////

		bool Has(Entity entity) const;
		bool Move(Entity entity, const glm::vec3& velocity);
		bool Jump(Entity entity, float speed);
		// Moves the controller, its presence body and the entity's translation at once (the caller tells the scene's other
		// listeners); false when the entity's parent cannot be inverted.
		bool Teleport(Entity entity, const glm::vec3& position);
		bool AddMotion(Entity entity, const glm::vec3& displacement);
		std::optional<CharacterState> GetState(Entity entity) const;

		// The presence body of an entity's controller (c_NoCharacterBody: none).
		uint32_t GetPresenceBody(UUID entity) const;
		uint32_t GetCount() const;
		// Wall time of the last Step in milliseconds.
		float GetLastStepTime() const;
	private:
		Scope<CharacterControllerSetData> m_Data;
	};

}
