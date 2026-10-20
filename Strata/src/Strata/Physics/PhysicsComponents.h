#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/UUID.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace Strata
{

	// The physics module's components (registered by RegisterPhysicsComponents).

	// Bodies: an entity with a RigidBodyComponent is one body, made of its own colliders and the colliders of descendants
	// without a RigidBodyComponent of their own (placed by their transform relative to it). Collider entities without a
	// RigidBodyComponent on themselves or any ancestor are static bodies of their own. Collision events and queries report the
	// entity that owns the body.

	enum class RigidBodyType : uint8_t
	{
		Static = 0,
		Dynamic,
		Kinematic
	};

	struct RigidBodyComponent
	{
		RigidBodyType Type = RigidBodyType::Dynamic;
		float Mass = 1.0f;
		float LinearDamping = 0.05f;
		float AngularDamping = 0.05f;
		float GravityScale = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		bool IsTrigger = false; // Reports overlaps without colliding
		bool ContinuousCollision = false; // Also swept against moving bodies (every dynamic body is swept against static ones)
		bool LockRotationX = false;
		bool LockRotationY = false;
		bool LockRotationZ = false;
		uint32_t Layer = 0; // Collision layer (0-31)
	};

	struct BoxColliderComponent
	{
		glm::vec3 HalfExtents = { 0.5f, 0.5f, 0.5f };
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };
	};

	struct SphereColliderComponent
	{
		float Radius = 0.5f;
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };
	};

	// Capsule aligned with the entity's local Y axis.
	struct CapsuleColliderComponent
	{
		float Radius = 0.5f;
		float HalfHeight = 0.5f; // Half height of the cylindrical section
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };
	};

	struct MeshColliderComponent
	{
		AssetHandle Mesh = UUID::Null(); // Uses the MeshRenderer's mesh when unset
		bool Convex = true; // Non-convex meshes are only supported on static bodies
	};

	// A character that gameplay moves (the scripts' CharacterController, root motion) instead of forces: an upright capsule
	// whose bottom is the entity's origin; the entity's rotation and scale do not change its shape. Each fixed step it moves
	// with the velocity it was given, falls with gravity, slides along walls, walks up steps and walkable slopes, sticks to
	// the ground walking down, pushes dynamic bodies and stops at everything else on the layers its layer collides with. It
	// replaces a RigidBody: an entity with both is simulated as the rigid body (see PhysicsWorld).
	struct CharacterControllerComponent
	{
		float Height = 1.8f;           // Total height of the capsule in world units (a sphere when below twice the radius)
		float Radius = 0.3f;           // World units
		float StepHeight = 0.35f;      // Highest step it walks up, and how far it follows the ground down, in world units
		float MaxSlopeDegrees = 45.0f; // Steepest slope it walks up, in degrees from the horizontal
		uint32_t Layer = 0;            // Collision layer (0-31)
		float GravityScale = 1.0f;     // Times the scene's gravity
		float PushStrength = 200.0f;   // Largest force with which it pushes dynamic bodies, in newtons
		bool Enabled = true;           // A disabled controller neither moves nor collides; its entity moves freely
	};

}
