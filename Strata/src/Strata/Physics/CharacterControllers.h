#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/UUID.h"
#include "Strata/Physics/PhysicsTypes.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneServices.h"

#include <box3d/id.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace Strata
{

	class Scene;
	struct CharacterControllerSetData;

	// A body a character controller touched at the end of its last move (the ground, a wall, a box it pushed). Triggers are
	// not among them: the controller's presence body meets those (see CharacterControllerSet).
	struct CharacterContactReport
	{
		UUID Controller = UUID::Null();      // The controller's entity
		UUID Other = UUID::Null();           // The entity owning the body it touched
		glm::vec3 Point = glm::vec3(0.0f);   // World space
		glm::vec3 Normal = glm::vec3(0.0f);  // From the controller towards the other
	};

	// A presence body (and its shape) that appeared, changed or went (null IDs: none).
	struct CharacterBodyChange
	{
		UUID Entity = UUID::Null();
		b3BodyId OldBody = b3_nullBodyId;
		b3ShapeId OldShape = b3_nullShapeId;
		b3BodyId NewBody = b3_nullBodyId;
		b3ShapeId NewShape = b3_nullShapeId;
		bool Removed = false; // The controller left the simulation (its contacts end)
	};

	// What a step of the controllers did, for the world that owns them.
	struct CharacterStepOutput
	{
		std::vector<entt::entity> Moved;               // Entities whose transform the step wrote, in controller order
		std::vector<CharacterContactReport> Contacts;  // Only when collected (see Step)

		void Clear()
		{
			Moved.clear();
			Contacts.clear();
		}
	};

	// The character controllers of a physics world (internal to the Physics module; see PhysicsWorld and
	// CharacterControllerComponent).
	//
	// Every entity with an active, enabled CharacterControllerComponent and no RigidBodyComponent is a controller: an upright
	// capsule moved by box3d's mover queries (b3World_CollideMover, b3SolvePlanes, b3World_CastMover), not by the solver,
	// whose base is the entity's origin, plus a kinematic "presence" body of 90% of its radius, which lets dynamic bodies land
	// on it, triggers detect it and queries hit it (its shape carries the entity like any body's). The controller collides with
	// the bodies on layers its layer collides with, and with other controllers' presence bodies; never with its own entity's
	// bodies or with triggers.
	//
	// A step (Step, once per fixed step before box3d's) moves every controller with the velocity its script gave it
	// (MoveCharacter), its jump, root motion and gravity, in two passes: across (walking: walkable slopes are climbed, steeper
	// ones and walls block, and steps up to StepHeight are walked up), then up or down (falling: walkable ground holds it,
	// steeper ground lets it slide down); walking down it sticks to the ground within StepHeight. The moves run in parallel on
	// the job system, each reading only the world as it was before the step (box3d's queries do not change it) and writing
	// only its own controller: they do not see each other's new positions in that step. Pushing dynamic bodies happens
	// afterwards, on the main thread, in controller order, together with writing each entity's translation (its rotation and
	// scale stay; Scene::MarkTransformsChanged) and moving the presence bodies. So the results do not depend on the number
	// of threads. An entity moved from outside (a script writing its transform, the editor) teleports its controller there
	// at the next step.
	//
	// Main thread only, except for what Step runs on the job system itself.
	class CharacterControllerSet
	{
	public:
		CharacterControllerSet(Scene& scene, b3WorldId world, const PhysicsSettings& settings);
		~CharacterControllerSet(); // Destroys every presence body

		CharacterControllerSet(const CharacterControllerSet&) = delete;
		CharacterControllerSet& operator=(const CharacterControllerSet&) = delete;

		// The entities of a scene that may become controllers (enabled components), for sizing a world's room.
		static uint32_t CountCandidates(const Scene& scene);

		// Creates, rebuilds and removes controllers after the changes recorded from the registry's signals (components added,
		// edited or removed, entities destroyed, activated or deactivated, rigid bodies added to controller entities) and
		// destructions requested while the scene updates. Appends what happened to presence bodies. Cheap without changes.
		void ApplyChanges(std::vector<CharacterBodyChange>& outChanges);

		// Moves every controller by one fixed step (see the class description). `gravity` is the world's; with
		// collectContacts the bodies each controller touched are reported (otherwise nothing is gathered).
		void Step(float timestep, const glm::vec3& gravity, bool collectContacts, CharacterStepOutput& output);

		// The presence bodies take the world's layer matrix after it changed. Lists the controllers whose presence shapes took
		// a new filter (box3d destroyed those shapes' contacts).
		void RefreshFilters(std::vector<UUID>& outRefiltered);

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

		uint32_t GetCount() const;
		// Wall time of the last Step in milliseconds.
		float GetLastStepTime() const;
	private:
		Scope<CharacterControllerSetData> m_Data;
	};

}
