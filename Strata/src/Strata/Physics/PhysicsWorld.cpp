#include "stpch.h"
#include "Strata/Physics/PhysicsWorld.h"

#include "Strata/Core/ErrorThrottle.h"
#include "Strata/Core/Hash.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Math/Math.h"
#include "Strata/Physics/AssetMeshProvider.h"
#include "Strata/Physics/CharacterControllers.h"
#include "Strata/Physics/PhysicsComponents.h"
#include "Strata/Physics/PhysicsFilters.h"
#include "Strata/Physics/PhysicsMeshShapes.h"
#include "Strata/Physics/PhysicsRuntime.h"
#include "Strata/Physics/PhysicsTaskSystem.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneHierarchy.h"

#include <box3d/box3d.h>
#include <box3d/collision.h>
#include <box3d/math_functions.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Strata
{

	namespace
	{

		// Collider dimensions (after scaling) are clamped to at least this size so that box3d never sees degenerate shapes
		// (b3MakeTransformedBoxHull clamps half widths to the same millimeter).
		constexpr float c_MinColliderExtent = 1.0e-3f;
		// Relative change of an entity's world scale that rebuilds its body's shape, and the smallest absolute change that
		// counts (scales this small are degenerate anyway).
		constexpr float c_ScaleChangeTolerance = 1.0e-4f;
		constexpr float c_MinScaleChange = 1.0e-10f;
		constexpr float c_MaxRaycastDistance = 1.0e5f;
		// Bodies within this distance of a static body that is moved are woken up, so that nothing keeps sleeping on top of a
		// body that is no longer there (box3d wakes the bodies touching a body it disables or destroys itself).
		constexpr float c_WakeMargin = 0.1f;
		constexpr float c_MinimumMass = 0.001f;
		// Applying a change can move colliders to another body, which is a change in turn; this many rounds always settle
		// a consistent hierarchy, so hitting the limit means a bug rather than a big change.
		constexpr uint32_t c_MaxChangeRounds = 16;
		// box3d computes in single precision and asserts on coordinates beyond B3_HUGE (1e5 m at one unit per meter): bodies
		// whose world position is farther from the origin wait like bodies with a degenerate transform.
		constexpr float c_MaxBodyCoordinate = 1.0e5f;
		// Solver sub-steps of each box3d step (box3d's recommended count).
		constexpr int c_SolverSubSteps = 4;

		// Warnings are issued once per entity and kind. The transient kinds are reset when the entity's body is built, so
		// that a recurring problem is reported again.
		enum class PhysicsWarning : uint8_t
		{
			MissingCollider = 1,
			DegenerateTransform,
			MissingMesh,
			NoUsableCollider,
			// Kinds below are reported once for the entity's lifetime
			InvalidLayer,
			InvalidType,
			InvalidCollider,
			InvalidMesh,
			UnsupportedMesh,
			NonFiniteState,
			Count
		};

		constexpr PhysicsWarning c_TransientWarnings[] = { PhysicsWarning::MissingCollider, PhysicsWarning::DegenerateTransform, PhysicsWarning::MissingMesh, PhysicsWarning::NoUsableCollider };

		// Automatic room (see PhysicsWorld::ResolveCapacities).
		constexpr uint32_t c_MinAutomaticBodies = 1024;
		constexpr uint32_t c_ContactsPerBody = 2;
		// What a world's room takes once it is used (see PhysicsWorld::EstimateReservedBytes), fitted to box3d's own count of
		// a world's bytes (b3World_GetCounters().byteCount) for worlds of boxes resting on a floor: 2.44 MB for 1,024 bodies and
		// contacts, 38.7 MB for 16,384, 51.1 MB for 16,384 bodies and 65,536 contacts. Per body (box3d's reserved and used
		// storage of a body, its shape, proxy and island), per contact, and the world's fixed part.
		constexpr uint64_t c_ReservedBytesPerBody = 2120;
		constexpr uint64_t c_ReservedBytesPerContact = 250;
		constexpr uint64_t c_ReservedBytesBase = 32ull * 1024;
		static_assert(2 * Scene::c_MaxEntities <= PhysicsCapacitySettings::c_MaxBodiesLimit, "The automatic body capacity must fit a scene full of bodies");
		static_assert(PhysicsCapacitySettings::c_MaxBodiesLimit <= B3_MAX_SHAPES, "box3d must hold a shape for every body of the largest world");

		uint32_t NextPowerOfTwo(uint64_t value)
		{
			return static_cast<uint32_t>(std::min<uint64_t>(std::bit_ceil(std::max<uint64_t>(value, 1)), uint64_t(1) << 31));
		}

		uint32_t ClampCapacity(uint64_t value, uint32_t limit)
		{
			return static_cast<uint32_t>(std::clamp<uint64_t>(value, 1, limit));
		}

		// Why an entity that needs a body has none.
		enum class BuildFailure : uint8_t
		{
			None = 0,
			NoCollider,          // A rigid body without colliders; waits for a component change
			InvalidColliders,    // Every collider has invalid data; waits for a component change
			DegenerateTransform, // Retried when a transform change is signaled (on the entity or an ancestor); also beyond c_MaxBodyCoordinate
			MissingMesh,         // Retried when the mesh provider reports new data or a shape finished cooking
			WaitingForSlot,      // The world is at its growth limit: queued in PhysicsWorldData::SlotWaiters, retried when bodies go
			Count
		};

		//////////////////////////////////////////////////////////////////////////
		// Conversions
		//////////////////////////////////////////////////////////////////////////

		b3Vec3 ToB3(const glm::vec3& value)
		{
			return b3Vec3 { value.x, value.y, value.z };
		}

		b3Quat ToB3(const glm::quat& value)
		{
			return b3Quat { b3Vec3 { value.x, value.y, value.z }, value.w };
		}

		b3Transform ToB3Transform(const glm::vec3& position, const glm::quat& rotation)
		{
			return b3Transform { ToB3(position), ToB3(rotation) };
		}

		glm::vec3 ToGlm(const b3Vec3& value)
		{
			return glm::vec3(value.x, value.y, value.z);
		}

		glm::quat ToGlm(const b3Quat& value)
		{
			return glm::quat(value.s, value.v.x, value.v.y, value.v.z);
		}

		bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		bool IsFinite(const glm::quat& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
		}

		bool IsWithinCoordinateRange(const glm::vec3& position)
		{
			return glm::all(glm::lessThanEqual(glm::abs(position), glm::vec3(c_MaxBodyCoordinate)));
		}

		b3BodyType ToBodyType(RigidBodyType type)
		{
			switch (type)
			{
				case RigidBodyType::Static: return b3_staticBody;
				case RigidBodyType::Dynamic: return b3_dynamicBody;
				case RigidBodyType::Kinematic: return b3_kinematicBody;
			}
			return b3_staticBody;
		}

		// A body's ID without its world, as PhysicsWorld::GetBodyID reports it: the slot and its generation.
		uint64_t GetBodyKey(b3BodyId body)
		{
			return (static_cast<uint64_t>(body.generation) << 32) | static_cast<uint32_t>(body.index1);
		}

		//////////////////////////////////////////////////////////////////////////
		// Hierarchy
		//////////////////////////////////////////////////////////////////////////

		bool HasCollider(const Entity& entity)
		{
			return entity.HasAnyComponent<BoxColliderComponent, SphereColliderComponent, CapsuleColliderComponent, MeshColliderComponent>();
		}

		bool HasPhysicsComponent(const Entity& entity)
		{
			return entity.HasComponent<RigidBodyComponent>() || HasCollider(entity);
		}

		// The entity whose body holds this entity's colliders: the entity itself if it has a rigid body, else (if it has
		// colliders) its nearest ancestor with a rigid body, or the entity itself (a static body of its own). Invalid for
		// entities without physics components.
		Entity FindBodyOwner(Entity entity)
		{
			if (!entity.IsValid())
				return {};
			if (entity.HasComponent<RigidBodyComponent>())
				return entity;
			if (!HasCollider(entity))
				return {};
			for (Entity parent = entity.GetParent(); parent.IsValid(); parent = parent.GetParent())
			{
				if (parent.HasComponent<RigidBodyComponent>())
					return parent;
			}
			return entity;
		}

		bool IsPendingDestroyInHierarchy(const Scene& scene, Entity entity)
		{
			for (Entity current = entity; current.IsValid(); current = current.GetParent())
			{
				if (scene.IsPendingDestroy(current))
					return true;
			}
			return false;
		}

		// Active in the hierarchy and not about to be destroyed: such entities take part in the simulation.
		bool IsSimulated(const Scene& scene, Entity entity)
		{
			return scene.IsActiveInHierarchy(entity) && !IsPendingDestroyInHierarchy(scene, entity);
		}

		// Whether a merged collider entity, a descendant of the owner (as collected by CollectMergedEntities), contributes to
		// its owner's shape: neither it nor an entity between it and the owner is inactive or pending destruction. (The
		// owner's own state decides whether the whole body is simulated.)
		bool IsPartOfOwnerShape(const Scene& scene, Entity entity, Entity owner)
		{
			ST_CORE_ASSERT(scene.IsDescendantOf(entity, owner), "IsPartOfOwnerShape: '{}' is not below its body's entity", entity.GetName());
			for (Entity current = entity; current.IsValid() && current != owner; current = current.GetParent())
			{
				if (current.HasComponent<InactiveComponent>() || scene.IsPendingDestroy(current))
					return false;
			}
			return true;
		}

		// Whether an entity has no children.
		bool IsLeaf(Entity entity)
		{
			const RelationshipComponent* relationship = entity.TryGetComponent<RelationshipComponent>();
			return !relationship || relationship->Children.empty();
		}

		// Visits root and its descendants depth first, parents before children. The visitor returns whether to descend into
		// the visited entity's children. The stack is scratch memory reused between calls (empty when a visit ends), so
		// visitors must not start a visit with the same stack.
		template<typename Visitor>
		void VisitSubtree(const Scene& scene, Entity root, std::vector<Entity>& stack, Visitor&& visitor)
		{
			ST_CORE_ASSERT(stack.empty(), "VisitSubtree: the stack is in use by another visit");
			stack.clear();
			stack.push_back(root);
			while (!stack.empty())
			{
				Entity entity = stack.back();
				stack.pop_back();
				if (!entity.IsValid() || !visitor(entity))
					continue;

				const RelationshipComponent* relationship = entity.TryGetComponent<RelationshipComponent>();
				if (!relationship)
					continue;
				for (auto it = relationship->Children.rbegin(); it != relationship->Children.rend(); ++it)
				{
					if (const Entity child = scene.GetEntityByUUID(*it))
						stack.push_back(child);
				}
			}
		}

		// Collider entities in the subtree of a rigid body that belong to its body (stops at descendants with a rigid body).
		std::vector<entt::entity> CollectMergedEntities(const Scene& scene, Entity owner, std::vector<Entity>& stack)
		{
			std::vector<entt::entity> merged;
			VisitSubtree(scene, owner, stack, [&](Entity entity)
			{
				if (entity == owner)
					return true;
				if (entity.HasComponent<RigidBodyComponent>())
					return false;
				if (HasCollider(entity))
					merged.push_back(entity.GetHandle());
				return true;
			});
			return merged;
		}

		//////////////////////////////////////////////////////////////////////////
		// Transforms
		//////////////////////////////////////////////////////////////////////////

		bool IsInvertible(const glm::mat4& transform)
		{
			return glm::abs(glm::determinant(transform)) > Scene::c_MinInvertibleDeterminant;
		}

		// World poses are written to entities relative to their parent, which must be invertible (not scaled to nearly zero),
		// so dynamic bodies, which write their pose back, cannot be simulated below a parent that is not.
		bool HasInvertibleParent(const Scene& scene, Entity entity)
		{
			const Entity parent = entity.GetParent();
			return !parent || IsInvertible(scene.GetWorldTransform(parent));
		}

		// The rotation that, with the given scale, makes up the basis of a transform; fails if the basis divided by the scale
		// is not a rotation (the transform has another scale, a reflection the scale does not express, or shear).
		bool SolveRotationForScale(const glm::mat4& transform, const glm::vec3& scale, glm::quat& outRotation)
		{
			constexpr float tolerance = 1.0e-4f;
			if (!IsFinite(scale) || glm::any(glm::lessThan(glm::abs(scale), glm::vec3(1.0e-6f))))
				return false;

			const glm::mat3 basis(glm::vec3(transform[0]) / scale.x, glm::vec3(transform[1]) / scale.y, glm::vec3(transform[2]) / scale.z);
			for (int column = 0; column < 3; column++)
			{
				if (!(glm::abs(glm::length(basis[column]) - 1.0f) <= tolerance))
					return false;
			}
			const bool orthogonal = glm::abs(glm::dot(basis[0], basis[1])) <= tolerance && glm::abs(glm::dot(basis[0], basis[2])) <= tolerance
				&& glm::abs(glm::dot(basis[1], basis[2])) <= tolerance;
			if (!orthogonal || !(glm::determinant(basis) > 0.0f))
				return false;

			const glm::quat rotation = glm::normalize(glm::quat_cast(basis));
			if (!IsFinite(rotation))
				return false;
			outRotation = rotation;
			return true;
		}

		// Sets a transform to a simulated local pose. It keeps its authored scale (including mirrored axes) whenever the pose
		// can be expressed with it, so that only its translation and rotation change; otherwise (e.g. below a sheared parent)
		// the local transform is decomposed, which folds mirroring into X. Returns false, leaving the transform unchanged, if
		// the pose is degenerate. Touches nothing else, so it may run on worker threads for distinct entities.
		bool SetLocalPose(TransformComponent& transform, const glm::mat4& localTransform)
		{
			const glm::vec3 translation(localTransform[3]);
			glm::quat rotation;
			if (IsFinite(translation) && SolveRotationForScale(localTransform, transform.Scale, rotation))
			{
				transform.Translation = translation;
				transform.Rotation = rotation;
				return true;
			}
			return transform.SetTransform(localTransform);
		}

		// Sets an entity's world transform to a simulated pose (see SetLocalPose). Returns false, leaving the entity unchanged,
		// if its parent cannot be inverted or the transform is degenerate.
		bool WriteWorldTransform(Scene& scene, Entity entity, const glm::mat4& worldTransform)
		{
			glm::mat4 localTransform = worldTransform;
			if (const Entity parent = entity.GetParent())
			{
				const glm::mat4 parentWorld = scene.GetWorldTransform(parent);
				if (!IsInvertible(parentWorld))
					return false;
				localTransform = glm::inverse(parentWorld) * worldTransform;
			}

			if (!SetLocalPose(entity.GetComponent<TransformComponent>(), localTransform))
				return false;
			// Written without on_update (this world listens to it for edits), so the scene is told directly that the cached world
			// transforms below the entity are stale.
			scene.MarkTransformChanged(entity);
			return true;
		}

		//////////////////////////////////////////////////////////////////////////
		// Contact pairs
		//////////////////////////////////////////////////////////////////////////

		// Contacts are tracked per pair of entities (not bodies or shapes), so that rebuilding a body keeps its contacts.
		struct PairKey
		{
			uint64_t Low = 0;
			uint64_t High = 0;

			bool operator==(const PairKey& other) const { return Low == other.Low && High == other.High; }
			bool operator<(const PairKey& other) const { return Low != other.Low ? Low < other.Low : High < other.High; }
		};

		struct PairKeyHash
		{
			size_t operator()(const PairKey& key) const
			{
				return static_cast<size_t>(Hash::Combine(key.Low, key.High));
			}
		};

		PairKey MakePairKey(UUID a, UUID b)
		{
			const uint64_t first = static_cast<uint64_t>(a);
			const uint64_t second = static_cast<uint64_t>(b);
			return first < second ? PairKey { first, second } : PairKey { second, first };
		}

		// A pair of entities that touch, tracked while someone wants its contacts (see ICollisionEventSource). box3d reports a
		// contact per pair of shapes (and per child of a compound or mesh) and a sensor visit per pair of a sensor shape and a
		// visitor shape, so a pair counts them: it begins when its first contact begins and ends when its last one ends.
		struct TouchingPair
		{
			UUID A = UUID::Null();
			UUID B = UUID::Null();
			// Positions of the pair in the touching pairs of A and of B (PhysicsWorldData::EntityPairs), so that removing it
			// takes constant time.
			uint32_t IndexInA = 0;
			uint32_t IndexInB = 0;
			bool IsTrigger = false;
			// One of the entities' bodies was rebuilt since the pair began: its old shapes' contacts are gone, and the new shapes'
			// contacts of the next step confirm the pair, or it ends then (see ResolveRecheckedPairs).
			bool Unconfirmed = false;
			uint32_t ContactCount = 0;      // Shape contacts and sensor visits of the pair that touch now
			uint64_t CharacterStep = 0;     // Last step in which a character controller's move reported the pair
			uint64_t CheckStep = 0;         // Last step whose refresh visited the pair (see CollectCheckedPairs)
			uint64_t SortKey = 0;           // When the pair began (PhysicsWorldData::NextPairOrder): orders the ends of removals
			b3ContactId Contact = {};       // A contact of the pair, for the point of its End (null for triggers and characters)
			glm::vec3 Point = glm::vec3(0.0f);
			glm::vec3 Normal = glm::vec3(0.0f); // From A towards B
		};

		//////////////////////////////////////////////////////////////////////////
		// World state
		//////////////////////////////////////////////////////////////////////////

		// An entity whose colliders are part of a body's shape, with its transform relative to the body (position and rotation
		// of the body, without its scale) when the shape was built.
		struct ShapeMember
		{
			entt::entity Handle = entt::null;
			glm::mat4 PoseInBody = glm::mat4(1.0f);
		};

		// A mesh used by the colliders of a body, remembered to notice when its data changes, arrives or finishes cooking.
		struct MeshSource
		{
			AssetHandle Mesh = UUID::Null();
			bool Convex = true;
			std::weak_ptr<const PhysicsMeshData> Data; // The data the provider returned (empty if none)
			bool Waiting = false;                      // The data was not available, or its shape was still cooking
		};

		// A baked compound (box3d's form of a triangle mesh placed away from its body's origin), owned by the bodies using it.
		class CompoundData
		{
		public:
			explicit CompoundData(b3CompoundData* compound)
				: m_Compound(compound)
			{
			}
			~CompoundData()
			{
				if (m_Compound)
					b3DestroyCompound(m_Compound);
			}
			CompoundData(const CompoundData&) = delete;
			CompoundData& operator=(const CompoundData&) = delete;

			const b3CompoundData* Get() const { return m_Compound; }
		private:
			b3CompoundData* m_Compound = nullptr;
		};

		// An entity that owns a body (a rigid body, or a collider entity without a rigid body ancestor). The record exists as
		// long as the entity needs a body, also while the body cannot be built (see BuildFailure).
		struct BodyRecord
		{
			UUID EntityID = UUID::Null();
			b3BodyId BodyID = b3_nullBodyId;                // Null while the body cannot be built
			RigidBodyType Type = RigidBodyType::Static;
			BuildFailure Failure = BuildFailure::None;
			uint32_t Layer = 0;                             // Collision layer of its shapes
			bool InSimulation = false;                      // Its body is enabled in box3d
			bool IsTrigger = false;                         // Its shapes are sensors
			bool Suspended = false;                         // Out of the simulation because the world transform is degenerate
			bool WaitsForParent = false;                    // Not built because the parent cannot be inverted (dynamic bodies)
			bool KinematicMoving = false;                   // A target transform gave the body a velocity for the last step
			bool TransformDirty = false;                    // A signaled transform change to apply at the next step
			bool Polled = false;                            // Listed in PhysicsWorldData::PolledBodies
			bool HasPendingForce = false;                   // PendingForce or PendingTorque are set
			uint64_t SlotTicket = 0;                        // Its entry in PhysicsWorldData::SlotWaiters while it waits for a slot
			uint64_t WriteBackPass = 0;                     // Last write-back pass that wrote this body
			uint64_t SyncPass = 0;                          // Last step whose synchronization handled this record
			glm::vec3 ShapeScale = glm::vec3(1.0f);         // World scale baked into the shape
			glm::vec3 WorldScale = glm::vec3(1.0f);         // Entity world scale at the last synchronization (ShapeScale within tolerance)
			glm::mat4 LastWorldTransform = glm::mat4(1.0f); // Entity world transform the body was last synchronized with
			glm::vec3 KinematicTargetPosition = glm::vec3(0.0f);
			glm::quat KinematicTargetRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			// Velocities of a dynamic body while it is out of the simulation, so that it resumes them.
			glm::vec3 SavedLinearVelocity = glm::vec3(0.0f);
			glm::vec3 SavedAngularVelocity = glm::vec3(0.0f);
			// Forces and torques for the next fixed step (box3d clears them after each of its steps, so they are applied before
			// every box3d step of the fixed step, see ApplyPendingForces).
			glm::vec3 PendingForce = glm::vec3(0.0f);
			glm::vec3 PendingTorque = glm::vec3(0.0f);
			std::vector<entt::entity> MergedEntities; // Collider descendants that belong to this body
			std::vector<ShapeMember> ShapeMembers;    // Entities whose colliders are part of the current shape
			std::vector<MeshSource> MeshSources;      // Meshes of the colliders considered by the last build
			std::vector<b3ShapeId> Shapes;            // The body's box3d shapes
			// What the body's shapes point to and must outlive them: cooked meshes and baked compounds.
			std::vector<Ref<const PhysicsCookedShape>> CookedShapes;
			std::vector<Ref<const CompoundData>> Compounds;

			bool HasBody() const { return B3_IS_NON_NULL(BodyID); }
		};

		// What a box3d body slot (its index) leads to: the generation of the body occupying it (to recognize a reused slot),
		// the entity owning it and that entity's record. Records live in a std::map, whose elements stay in place until they
		// are removed, which empties the slot first.
		struct BodySlot
		{
			uint16_t Generation = 0;
			bool Used = false;
			uint64_t AwakeStamp = 0; // PhysicsWorldData::AwakeStamp when the body was last listed awake
			entt::entity Entity = entt::null;
			BodyRecord* Record = nullptr;
		};

		// What a box3d shape slot leads to, for events and queries: the entity whose body (or character controller) the shape
		// belongs to. Kept from the shape's creation to its destruction; events naming a shape that is gone (or a slot reused
		// since, of another generation) resolve to nothing.
		struct ShapeSlot
		{
			uint16_t Generation = 0;
			bool Used = false;
			bool Sensor = false;
			bool Convex = true;                        // Not a triangle mesh (rays starting inside are not reported)
			RigidBodyType BodyType = RigidBodyType::Static;
			UUID Entity = UUID::Null();
			uint64_t BodyKey = 0;
		};

		// An entry of PhysicsWorldData::RecordIndex: the entity recorded at an index (its version included) and its record.
		struct RecordSlot
		{
			entt::entity Handle = entt::null;
			BodyRecord* Record = nullptr;
		};

		// A dynamic body whose simulated pose is written to its entity in parallel with the others: one without a dynamic
		// ancestor written in the same step, so its parent's world transform is known before the writes start (see
		// WriteBackDynamicBodies). Each item touches only its own record and transform.
		struct PoseWrite
		{
			BodyRecord* Record = nullptr;
			entt::entity Handle = entt::null;
			TransformComponent* Transform = nullptr;
			glm::mat4 ParentWorld = glm::mat4(1.0f); // Identity for a root
			bool HasParent = false;
			bool HasChildren = false;
			// Outcome, handled on the main thread afterwards, in order
			bool Written = false;
			bool NonFinite = false;
			bool ScaleChanged = false;
		};

		// A box hull of these half extents and this placement in its body, by the bits of its values: box hulls are made once
		// per size and placement and shared by the bodies of a world (box3d keeps one copy per hull in its world too).
		struct BoxHullKey
		{
			std::array<uint32_t, 10> Bits = {};

			bool operator==(const BoxHullKey& other) const = default;
		};

		struct BoxHullKeyHash
		{
			size_t operator()(const BoxHullKey& key) const
			{
				uint64_t hash = 0;
				for (uint32_t bits : key.Bits)
					hash = Hash::Combine(hash, bits);
				return static_cast<size_t>(hash);
			}
		};

		BoxHullKey MakeBoxHullKey(const glm::vec3& halfExtents, const b3Transform& placement)
		{
			const auto bits = [](float value) { return std::bit_cast<uint32_t>(value); };
			return BoxHullKey { { bits(halfExtents.x), bits(halfExtents.y), bits(halfExtents.z), bits(placement.p.x), bits(placement.p.y), bits(placement.p.z),
				bits(placement.q.v.x), bits(placement.q.v.y), bits(placement.q.v.z), bits(placement.q.s) } };
		}

		// Distinct box hulls a world shares between its bodies; hulls beyond are made per body.
		constexpr size_t c_MaxSharedBoxHulls = 4096;

		// Entities whose changes are applied by the next ApplyPendingChanges, in the order they were recorded, each once.
		// Membership is a slot per entity index holding the entity recorded there (a cell's unload records tens of thousands
		// at once, where a hash set's inserts and clearing cost milliseconds); an index reused by a new entity in the same
		// batch takes the slot over, so both are recorded.
		class ChangeList
		{
		public:
			void Add(entt::entity handle)
			{
				if (handle == entt::null)
					return;
				const size_t index = static_cast<size_t>(entt::to_entity(handle));
				if (index >= m_Recorded.size())
					m_Recorded.resize(std::max(index + 1, m_Recorded.size() * 2), entt::null);
				if (m_Recorded[index] == handle)
					return;
				m_Recorded[index] = handle;
				m_List.push_back(handle);
			}

			bool IsEmpty() const { return m_List.empty(); }

			// Moves the recorded entities into `list` (discarding its contents); the list's buffer is reused for the next
			// changes, so applying changes does not allocate once the buffers have grown.
			const std::vector<entt::entity>& TakeInto(std::vector<entt::entity>& list)
			{
				ForgetRecorded();
				list.clear();
				m_List.swap(list);
				return list;
			}

			void Clear()
			{
				ForgetRecorded();
				m_List.clear();
			}
		private:
			void ForgetRecorded()
			{
				for (const entt::entity handle : m_List)
					m_Recorded[static_cast<size_t>(entt::to_entity(handle))] = entt::null;
			}

			std::vector<entt::entity> m_List;
			std::vector<entt::entity> m_Recorded; // By entity index: the entity recorded there, or null
		};

		struct MeshProviderStorage
		{
			AssetMeshProvider Default;
			Ref<PhysicsMeshProvider> Custom;
			uint64_t Generation = 0; // Changes whenever the provider is replaced
		};

		MeshProviderStorage& GetMeshProviderStorage()
		{
			static MeshProviderStorage s_Storage;
			return s_Storage;
		}

		PhysicsMeshProvider& GetActiveMeshProvider()
		{
			MeshProviderStorage& storage = GetMeshProviderStorage();
			return storage.Custom ? *storage.Custom : static_cast<PhysicsMeshProvider&>(storage.Default);
		}

		// The settings of a world with resolved capacities (see PhysicsWorld::ResolveCapacities), within the engine's limits.
		PhysicsSettings SanitizeSettings(const PhysicsSettings& settings)
		{
			PhysicsSettings result = settings;
			result.MaxBodies = ClampCapacity(settings.MaxBodies, PhysicsCapacitySettings::c_MaxBodiesLimit);
			result.MaxContactConstraints = ClampCapacity(settings.MaxContactConstraints, PhysicsCapacitySettings::c_MaxContactConstraintsLimit);
			result.CollisionSteps = std::clamp(settings.CollisionSteps, 1u, 64u);
			result.MeshWaitTimeout = std::isfinite(settings.MeshWaitTimeout) ? std::max(settings.MeshWaitTimeout, 0.0f) : 0.0f;
			return result;
		}

	}

	struct PhysicsWorldData
	{
		PhysicsWorldData(Scene& scene, const PhysicsSettings& settings)
			: OwnerScene(&scene), Settings(settings), Tasks(&TaskCounters)
		{
		}

		PhysicsRuntimeReference Runtime; // First member: box3d's hooks are installed before anything else uses box3d
		Scene* OwnerScene = nullptr;
		PhysicsSettings Settings;
		PhysicsTaskCounters TaskCounters;
		PhysicsTaskSystem Tasks;  // Outlives the box3d world, which calls it during steps
		b3WorldId World = b3_nullWorldId;
		bool HasStepped = false;  // A box3d step ran: static shapes created from now on must find their pairs themselves
		std::unordered_map<BoxHullKey, Scope<b3BoxHull>, BoxHullKeyHash> BoxHulls; // See GetBoxHull
		Scope<CharacterControllerSet> Characters; // Destroyed (with its presence bodies) before the box3d world it uses
		std::vector<CharacterBodyChange> CharacterChanges;            // Scratch of ApplyCharacterChanges
		CharacterStepOutput CharacterStep;                            // Scratch of StepCharacters

		// Ordered: per-step processing is deterministic. Records are created only by EmplaceRecord and removed only by
		// EraseRecord (or cleared together with RecordIndex and RecordOverflow, as Clear does), never through the map itself:
		// the index below points into its nodes, so a direct erase would leave FindRecord a dangling record, and a direct
		// insert a record FindRecord does not find. Debug and Release builds check the counts (CheckRecordIndex).
		std::map<entt::entity, BodyRecord> Bodies;
		// The records by entity index, for lookups (FindRecord): the map's ordered search costs a cache miss per level, and a
		// cell that loads or unloads looks up tens of thousands of records in a frame. Its nodes keep their addresses. A new
		// entity can take the index of a destroyed one whose record the same changes have not removed yet: the newer record
		// then waits in the overflow (rare, and only until it is removed).
		std::vector<RecordSlot> RecordIndex;
		std::unordered_map<entt::entity, BodyRecord*> RecordOverflow;
		size_t IndexedRecords = 0; // Records RecordIndex holds; the others are in RecordOverflow
		std::unordered_map<entt::entity, entt::entity> MergedOwners;  // Collider entity -> rigid body entity owning its body
		// Records that need attention at the next step besides the awake bodies (see NeedsPolling). Entries are dropped
		// lazily, so the list may hold records that no longer need it (or no longer exist).
		std::vector<entt::entity> PolledBodies;
		std::vector<BodySlot> BodySlots;                              // Per box3d body index, see FindBodyRecord
		std::vector<ShapeSlot> ShapeSlots;                            // Per box3d shape index, see ResolveShape
		uint32_t LiveBodies = 0;                                      // box3d bodies of records and character controllers
		// The bodies that were awake after the last step, and those woken or created since (see MarkAwake): what the next step
		// synchronizes besides the polled records, by body index. Listed once per stamp.
		std::vector<int32_t> AwakeBodies;
		uint64_t AwakeStamp = 1;
		// Bodies created in the current round of changes, static and moving: rounds that created bodies (see AddBatchCount).
		bool RoundCreatedStatic = false;
		bool RoundCreatedMoving = false;
		uint64_t WakeQueryCount = 0;                                  // Tree queries that woke bodies since the world was created
		uint64_t AddBatchCount = 0;                                   // Rounds of changes (per motion kind) that created bodies
		std::unordered_map<PairKey, TouchingPair, PairKeyHash> TouchingPairs;
		std::unordered_map<UUID, std::vector<PairKey>> EntityPairs;   // Touching pairs of each entity (see AddTouchingPair)
		uint64_t PairIndexOperations = 0;                             // Entries of EntityPairs written since the world was created
		uint64_t NextPairOrder = 1;
		bool TrackAll = true;                                         // See SetTrackAllContacts
		std::unordered_set<UUID> ContactInterest;                     // Entities whose contacts are wanted (see SetContactInterest)
		std::vector<UUID> SeededEntities;                             // Wanted since the last step: their touching pairs are seeded after it
		bool SeedAll = false;                                         // Tracking all contacts started: every body's pairs are seeded
		std::vector<UUID> UnwantedEntities;                           // No longer wanted: their pairs nobody wants end after the next step
		bool CheckAllWanted = false;                                  // Tracking all contacts stopped: every pair nobody wants ends
		std::vector<UUID> RecheckedEntities;                          // Rebuilt since the last step: their unconfirmed pairs end unless confirmed
		std::vector<PairKey> CharacterPairs;                          // Pairs the character controllers reported in the last step
		std::set<entt::entity> TriggerBodies;                         // Records whose shapes are sensors (seeding finds what is inside them)
		std::set<entt::entity> MeshBodies;                            // Records with MeshSources (ordered: deterministic rebuilds)
		std::set<entt::entity> WaitingMeshBodies;                     // Records with a MeshSource that waits for data or cooking
		std::unordered_map<AssetHandle, std::vector<entt::entity>> MeshUsers; // Mesh -> records whose MeshSources use it
		// The mesh provider and cook state that the MeshSources of the records were last checked against.
		uint64_t SeenMeshProviderGeneration = 0;
		uint64_t SeenMeshProviderVersion = 0;
		uint64_t SeenCompletedCooks = 0;
		std::vector<CollisionEvent> PendingEvents;
		std::vector<entt::entity> ForcedBodies;                       // Records with forces for the next step (see ApplyPendingForces)
		std::unordered_set<uint64_t> IssuedWarnings;                  // Hash of (entity, PhysicsWarning)

		// Capacities (see PhysicsWorld, "Capacities"): whether the room was sized automatically, the room the world has now,
		// the records waiting at the growth limit (BuildFailure::WaitingForSlot), the first of them since the limit was last
		// reported, and what the world needed beyond the room it was created with.
		bool AutomaticMaxBodies = false;
		bool AutomaticMaxContactConstraints = false;
		uint32_t RoomBodies = 0;
		// What box3d reserved when the world was created: the room, but never more than the automatic room of the scene's
		// bodies (a room given far beyond them, the largest a scene may ask for, would reserve gigabytes box3d grows into
		// anyway when the bodies come).
		uint32_t LibraryRoomBodies = 0;
		uint32_t LibraryRoomContacts = 0;
		uint32_t SlotWaiterCount = 0;
		std::array<uint32_t, static_cast<size_t>(BuildFailure::Count)> FailureCounts {}; // Records per BuildFailure (see SetFailure)
		entt::entity FirstSlotWaiter = entt::null;
		ErrorThrottle SlotWaiterErrors { "Physics" };                 // One aggregated error while bodies wait for a slot
		// The records waiting for a slot, in the order they started waiting, with the ticket each got then: an entry whose
		// record has another ticket (it stopped waiting, or waits again from a later place) or no longer exists is skipped.
		// Waiting records are never polled: bodies that go (see GiveSlotsToWaiters) hand their room to them from the front.
		std::deque<std::pair<entt::entity, uint64_t>> SlotWaiters;
		uint64_t NextSlotTicket = 1;
		PhysicsCapacityDemand Demand;
		uint32_t ReservedBodies = 0;   // See PhysicsWorld::Reserve
		// What updates of the scene built, destroyed and spent applying changes (see UpdateCounters): those of the update
		// Scene::GetFrameIndex was CounterFrame in, and of the one before it.
		struct UpdateCounters
		{
			uint32_t BodiesAdded = 0;
			uint32_t BodiesRemoved = 0;
			float ChangeTime = 0.0f;
		};
		uint64_t CounterFrame = 0;
		UpdateCounters CurrentUpdate;
		UpdateCounters PreviousUpdate;
		uint32_t GrowthCount = 0;
		float LastGrowthMs = 0.0f;
		uint32_t LastGrowthFrom = 0;
		uint32_t LastGrowthTo = 0;
		bool GrewThisRound = false;
		uint64_t StepCount = 0;
		uint64_t WriteBackPass = 0;
		uint64_t SyncPass = 0;
		float LastStepTime = 0.0f;
		float LastLibraryTime = 0.0f;   // The parts of LastStepTime (see PhysicsStats)
		float LastSyncTime = 0.0f;
		float LastWriteBackTime = 0.0f;
		float LastContactTime = 0.0f;
		uint32_t LastSyncedBodies = 0;
		uint32_t LastCheckedPairs = 0;
		uint32_t LastWrittenBodies = 0;
		uint32_t LastOrderedSlots = 0;
		uint64_t BuildCount = 0;
		uint64_t ParallelBuildCount = 0;                              // Rounds of changes that built their bodies on the job threads
		uint64_t MeshCheckCount = 0;
		// The start of the simulation waits for meshes (see HoldForMeshes).
		bool HasStarted = false;
		bool HeldLastStep = false;
		float HeldTime = 0.0f;
		uint64_t HeldStepCount = 0;

		// Scratch buffers reused by every step, so that steps allocate nothing once they have grown.
		std::vector<entt::entity> SyncCandidates;
		std::vector<entt::entity> SyncFollowers;                      // See MarkMovedDescendants
		std::vector<entt::entity> MeshChecks;                         // Scratch of RefreshMeshBodies and HoldForMeshes
		std::vector<AssetHandle> ChangedMeshes;
		std::vector<PairKey> CheckedPairs;
		std::vector<TouchingPair> EndedPairs;
		std::vector<TouchingPair> BegunPairs;
		std::vector<b3ContactData> ContactScratch;
		std::vector<b3ShapeId> ShapeScratch;
		std::vector<std::pair<uint32_t, entt::entity>> NestedWrites;  // Depth and entity of dynamic bodies below written ones
		std::vector<PoseWrite> PoseWrites;                            // See WriteBackDynamicBodies
		std::vector<entt::entity> WrittenEntities;
		// Ordering scratch of the write-back: a bit per body index (grown with the slots, all clear between steps) and the
		// words of it that bits were set in during this step.
		std::vector<uint64_t> MovedBodyBits;
		std::vector<uint32_t> MovedBodyWords;
		std::vector<std::pair<entt::entity, BodyRecord*>> AwakeRecords; // See SyncEntitiesToBodies
		std::vector<Entity> VisitStack;                               // See VisitSubtree
		std::vector<entt::entity> ChangeScratch;                      // See ChangeList::TakeInto
		std::unordered_set<entt::entity> VisitedOwners;

		// A Sliced world's entities with physics components, in hierarchy order, and how many BuildSlice has handed to the
		// rounds of changes so far (see PhysicsWorld::BuildSlice).
		std::vector<entt::entity> PendingBuild;
		size_t PendingBuildIndex = 0;
		uint32_t PreparedSlices = 0; // BuildSlice calls that built bodies
		bool Built = false;

		// Recorded by registry signals, applied by ApplyPendingChanges.
		ChangeList StructureChanges; // Components added, removed or modified; entity destroyed
		ChangeList SubtreeChanges;   // Hierarchy changed: re-evaluate every physics entity of the subtree
		ChangeList ActivityChanges;
		ChangeList TransformChanges;
		ChangeList OwnerRefreshes;   // Bodies to rebuild

		// Declared last: disconnected first, before anything the handlers use is destroyed.
		std::vector<entt::scoped_connection> Connections;
	};

	namespace
	{

		//////////////////////////////////////////////////////////////////////////
		// Records and warnings
		//////////////////////////////////////////////////////////////////////////

		uint64_t MakeWarningKey(UUID entityID, PhysicsWarning warning)
		{
			return Hash::Combine(static_cast<uint64_t>(entityID), static_cast<uint64_t>(warning));
		}

		bool ShouldWarn(PhysicsWorldData& data, UUID entityID, PhysicsWarning warning)
		{
			return data.IssuedWarnings.insert(MakeWarningKey(entityID, warning)).second;
		}

		void ClearWarnings(PhysicsWorldData& data, UUID entityID, std::span<const PhysicsWarning> warnings)
		{
			if (data.IssuedWarnings.empty())
				return; // The usual case: nothing to look up for every body built
			for (PhysicsWarning warning : warnings)
				data.IssuedWarnings.erase(MakeWarningKey(entityID, warning));
		}

		void ClearAllWarnings(PhysicsWorldData& data, UUID entityID)
		{
			if (data.IssuedWarnings.empty())
				return;
			for (uint8_t warning = 1; warning < static_cast<uint8_t>(PhysicsWarning::Count); warning++)
				data.IssuedWarnings.erase(MakeWarningKey(entityID, static_cast<PhysicsWarning>(warning)));
		}

		// Whether a world scale differs from the one baked into a shape enough to rebuild it: by more than
		// c_ScaleChangeTolerance relative to the baked scale on some axis. Relative also for small scales: with an absolute
		// tolerance, edits of centimeter-sized props were neither rebuilt nor matched by the scale written back with the pose.
		bool HasScaleChanged(const glm::vec3& scale, const glm::vec3& previous)
		{
			const glm::vec3 difference = glm::abs(scale - previous);
			const glm::vec3 tolerance = glm::max(glm::abs(previous) * c_ScaleChangeTolerance, glm::vec3(c_MinScaleChange));
			return glm::any(glm::greaterThan(difference, tolerance));
		}

		// The world scale of an entity right now, as DecomposeTransform folds it (the scale a shape would be built with), or
		// the fallback if the world transform is degenerate.
		glm::vec3 GetWorldScale(const Scene& scene, Entity entity, const glm::vec3& fallback)
		{
			glm::vec3 translation;
			glm::quat rotation;
			glm::vec3 scale;
			return Math::DecomposeTransform(scene.GetWorldTransform(entity), translation, rotation, scale) ? scale : fallback;
		}

		template<typename Data>
		auto FindRecord(Data& data, entt::entity handle) -> decltype(&data.Bodies.begin()->second)
		{
			if (handle == entt::null)
				return nullptr;
			const size_t index = static_cast<size_t>(entt::to_entity(handle));
			if (index >= data.RecordIndex.size())
				return nullptr;
			const RecordSlot& slot = data.RecordIndex[index];
			if (slot.Handle == handle)
				return slot.Record;
			if (data.RecordOverflow.empty())
				return nullptr;
			const auto overflow = data.RecordOverflow.find(handle);
			return overflow != data.RecordOverflow.end() ? overflow->second : nullptr;
		}

		// Every record is in the index or the overflow, and nothing else is: a record created or removed through the map
		// itself breaks the counts. Constant time, so that it runs after every change of the records.
		void CheckRecordIndex([[maybe_unused]] const PhysicsWorldData& data)
		{
			ST_CORE_ASSERT(data.Bodies.size() == data.IndexedRecords + data.RecordOverflow.size(),
				"Physics: {} body records, but {} indexed and {} in the overflow (a record was created or removed outside EmplaceRecord and EraseRecord)",
				data.Bodies.size(), data.IndexedRecords, data.RecordOverflow.size());
		}

		// Creates the record of an entity (or finds the one it has) and indexes it for FindRecord.
		std::pair<BodyRecord*, bool> EmplaceRecord(PhysicsWorldData& data, entt::entity handle)
		{
			auto [it, created] = data.Bodies.try_emplace(handle);
			if (created)
			{
				const size_t index = static_cast<size_t>(entt::to_entity(handle));
				// Doubling, but never past the largest entity index (2^20 slots, 16 MiB).
				if (index >= data.RecordIndex.size())
					data.RecordIndex.resize(std::max(index + 1, std::min(data.RecordIndex.size() * 2, static_cast<size_t>(entt::to_entity(entt::entity(entt::null))) + 1)));
				RecordSlot& slot = data.RecordIndex[index];
				if (slot.Handle == entt::null)
				{
					slot = RecordSlot { handle, &it->second };
					data.IndexedRecords++;
				}
				else
				{
					data.RecordOverflow.emplace(handle, &it->second);
				}
				CheckRecordIndex(data);
			}
			return { &it->second, created };
		}

		void EraseRecord(PhysicsWorldData& data, std::map<entt::entity, BodyRecord>::iterator it)
		{
			const entt::entity handle = it->first;
			const size_t index = static_cast<size_t>(entt::to_entity(handle));
			if (index < data.RecordIndex.size() && data.RecordIndex[index].Handle == handle)
			{
				data.RecordIndex[index] = RecordSlot();
				data.IndexedRecords--;
			}
			else
			{
				data.RecordOverflow.erase(handle);
			}
			data.Bodies.erase(it);
			CheckRecordIndex(data);
		}

		// The record of an entity whose body is part of the simulation, or nullptr.
		template<typename Data>
		auto FindSimulatedRecord(Data& data, Entity entity) -> decltype(&data.Bodies.begin()->second)
		{
			if (!entity.IsValid() || entity.GetScene() != data.OwnerScene)
				return nullptr;
			auto record = FindRecord(data, entity.GetHandle());
			return record && record->InSimulation ? record : nullptr;
		}

		template<typename Data>
		auto FindDynamicRecord(Data& data, Entity entity) -> decltype(&data.Bodies.begin()->second)
		{
			auto record = FindSimulatedRecord(data, entity);
			return record && record->Type == RigidBodyType::Dynamic ? record : nullptr;
		}

		// Makes a body known by its index (records' bodies and character controllers' presence bodies). box3d reuses indices,
		// so the slots grow to the largest index in use.
		void RegisterBody(PhysicsWorldData& data, b3BodyId body, entt::entity handle, BodyRecord* record)
		{
			const size_t index = static_cast<size_t>(body.index1 - 1);
			if (index >= data.BodySlots.size())
			{
				data.BodySlots.resize(std::max(index + 1, data.BodySlots.size() * 2));
				data.MovedBodyBits.resize((data.BodySlots.size() + 63) / 64, 0);
			}
			data.BodySlots[index] = BodySlot { body.generation, true, 0, handle, record };
			data.LiveBodies++;
		}

		void UnregisterBody(PhysicsWorldData& data, b3BodyId body)
		{
			const size_t index = static_cast<size_t>(body.index1 - 1);
			if (index < data.BodySlots.size() && data.BodySlots[index].Used && data.BodySlots[index].Generation == body.generation)
			{
				data.BodySlots[index] = BodySlot();
				data.LiveBodies--;
			}
		}

		void RegisterShape(PhysicsWorldData& data, b3ShapeId shape, const ShapeSlot& slot)
		{
			const size_t index = static_cast<size_t>(shape.index1 - 1);
			if (index >= data.ShapeSlots.size())
				data.ShapeSlots.resize(std::max(index + 1, data.ShapeSlots.size() * 2));
			data.ShapeSlots[index] = slot;
			data.ShapeSlots[index].Generation = shape.generation;
			data.ShapeSlots[index].Used = true;
		}

		void UnregisterShape(PhysicsWorldData& data, b3ShapeId shape)
		{
			const size_t index = static_cast<size_t>(shape.index1 - 1);
			if (index < data.ShapeSlots.size() && data.ShapeSlots[index].Generation == shape.generation)
				data.ShapeSlots[index] = ShapeSlot();
		}

		// The entity a shape belongs to as of now, or nullptr for a shape the world does not know (any more): events may name
		// shapes destroyed since, whose slots may hold other shapes now.
		const ShapeSlot* ResolveShape(const PhysicsWorldData& data, b3ShapeId shape)
		{
			const int64_t index = static_cast<int64_t>(shape.index1) - 1;
			if (index < 0 || static_cast<size_t>(index) >= data.ShapeSlots.size())
				return nullptr;
			const ShapeSlot& slot = data.ShapeSlots[static_cast<size_t>(index)];
			return slot.Used && slot.Generation == shape.generation ? &slot : nullptr;
		}

		// Lists a body as awake for the next step's synchronization (see PhysicsWorldData::AwakeBodies).
		void MarkAwake(PhysicsWorldData& data, b3BodyId body)
		{
			const int64_t index = static_cast<int64_t>(body.index1) - 1;
			if (index < 0 || static_cast<size_t>(index) >= data.BodySlots.size())
				return;
			BodySlot& slot = data.BodySlots[static_cast<size_t>(index)];
			if (!slot.Used || slot.Generation != body.generation || slot.AwakeStamp == data.AwakeStamp)
				return;
			slot.AwakeStamp = data.AwakeStamp;
			data.AwakeBodies.push_back(static_cast<int32_t>(index));
		}

		bool IsSlotWaiter(const PhysicsWorldData& data, const std::pair<entt::entity, uint64_t>& entry)
		{
			const BodyRecord* record = FindRecord(data, entry.first);
			return record && record->Failure == BuildFailure::WaitingForSlot && record->SlotTicket == entry.second;
		}

		// Drops the entries of records that stopped waiting from the queue once they are most of it, so that entities that wait
		// and go again (spawns at the growth limit) do not grow it without end. Linear, amortized over the entries dropped.
		void CompactSlotWaiters(PhysicsWorldData& data)
		{
			constexpr size_t c_MinCompactedSize = 1024;
			if (data.SlotWaiters.size() < c_MinCompactedSize || data.SlotWaiters.size() < 2 * static_cast<size_t>(data.SlotWaiterCount))
				return;
			std::erase_if(data.SlotWaiters, [&](const std::pair<entt::entity, uint64_t>& entry) { return !IsSlotWaiter(data, entry); });
		}

		// The counters of the scene's current update (Scene::GetFrameIndex, which advances at the end of each update): those of
		// earlier updates are let go as the frames advance, so no call per frame is needed.
		PhysicsWorldData::UpdateCounters& GetUpdateCounters(PhysicsWorldData& data)
		{
			const uint64_t frame = data.OwnerScene->GetFrameIndex();
			if (frame != data.CounterFrame)
			{
				data.PreviousUpdate = frame == data.CounterFrame + 1 ? data.CurrentUpdate : PhysicsWorldData::UpdateCounters();
				data.CurrentUpdate = PhysicsWorldData::UpdateCounters();
				data.CounterFrame = frame;
			}
			return data.CurrentUpdate;
		}

		// The counters of the scene's last update, the one before the current frame index (see GetUpdateCounters).
		PhysicsWorldData::UpdateCounters GetLastUpdateCounters(const PhysicsWorldData& data)
		{
			const uint64_t frame = data.OwnerScene->GetFrameIndex();
			if (frame == data.CounterFrame)
				return data.PreviousUpdate;
			if (frame == data.CounterFrame + 1)
				return data.CurrentUpdate;
			return {};
		}

		// Sets why a record has no body (BuildFailure::None: it has one), counting the records that wait for a free slot and
		// queuing them in the order they start waiting. A record that fails on a full world again keeps its place.
		void SetFailure(PhysicsWorldData& data, entt::entity handle, BodyRecord& record, BuildFailure failure)
		{
			if (record.Failure == BuildFailure::WaitingForSlot && failure == BuildFailure::WaitingForSlot)
				return;
			if (record.Failure != BuildFailure::None)
				data.FailureCounts[static_cast<size_t>(record.Failure)]--;
			if (failure != BuildFailure::None)
				data.FailureCounts[static_cast<size_t>(failure)]++;
			if (record.Failure == BuildFailure::WaitingForSlot)
			{
				data.SlotWaiterCount--;
				record.SlotTicket = 0;
			}
			if (failure == BuildFailure::WaitingForSlot)
			{
				data.SlotWaiterCount++;
				record.SlotTicket = data.NextSlotTicket++;
				CompactSlotWaiters(data);
				data.SlotWaiters.emplace_back(handle, record.SlotTicket);
				if (data.FirstSlotWaiter == entt::null)
					data.FirstSlotWaiter = handle;
			}
			record.Failure = failure;
		}

		// The bodies of the world (those of records and the presence bodies of character controllers).
		uint32_t GetUsedBodySlots(const PhysicsWorldData& data)
		{
			return data.LiveBodies;
		}

		// The most bodies the world may grow to: PhysicsSettings::GrowthLimit (0: the engine's limit), never below MaxBodies.
		uint32_t GetGrowthLimit(const PhysicsSettings& settings)
		{
			const uint32_t limit = settings.GrowthLimit == 0 ? PhysicsCapacitySettings::c_MaxBodiesLimit : std::min(settings.GrowthLimit, PhysicsCapacitySettings::c_MaxBodiesLimit);
			return std::max(limit, settings.MaxBodies);
		}

		// One error for the entities that found the world at its growth limit, instead of one per entity (thousands at once
		// when a scene outgrows its limit); logged again only once they all got a body and the world reaches its limit again.
		// Records what the world would have needed, so that the scene's next world reserves it.
		void ReportBodyLimit(PhysicsWorldData& data)
		{
			if (data.SlotWaiterCount == 0)
			{
				data.SlotWaiterErrors.Clear();
				data.FirstSlotWaiter = entt::null;
				return;
			}

			const uint64_t needed = static_cast<uint64_t>(GetUsedBodySlots(data)) + data.SlotWaiterCount;
			data.Demand.Bodies = std::max(data.Demand.Bodies, static_cast<uint32_t>(std::min<uint64_t>(needed, PhysicsCapacitySettings::c_MaxBodiesLimit)));
			if (!data.SlotWaiterErrors.GetLastMessage().empty())
				return;

			const Entity first(data.FirstSlotWaiter, data.OwnerScene);
			std::string entities = first.IsValid() ? fmt::format("'{}'", first.GetName()) : std::string("an entity");
			if (data.SlotWaiterCount > 1)
				entities += fmt::format(" and {} more entities", data.SlotWaiterCount - 1);
			const uint32_t limit = GetGrowthLimit(data.Settings);
			const bool engineLimit = limit >= PhysicsCapacitySettings::c_MaxBodiesLimit;
			data.SlotWaiterErrors.Report(fmt::format("cannot create a body for {}: the world is at {} of {} bodies and cannot grow; they get one when bodies "
				"are removed", entities, engineLimit ? "the engine's limit" : "its growth limit (PhysicsSettings::GrowthLimit)", limit));
		}

		// Whether a record without a body waits for a body that can be built later (rather than for a component change).
		bool IsPending(const BodyRecord& record)
		{
			return !record.HasBody() && (record.Failure == BuildFailure::DegenerateTransform || record.Failure == BuildFailure::MissingMesh || record.Failure == BuildFailure::WaitingForSlot);
		}

		// Whether a record needs attention at the next step although its body may be asleep or out of the simulation (awake
		// bodies are synchronized anyway): a signaled transform change, which also retries a build that failed on a
		// degenerate transform and brings back a body suspended for one. Nothing else is polled: activity changes bring
		// bodies back through their signals, mesh data is checked when the mesh provider reports changes (see
		// RefreshMeshBodies), and records waiting for a body slot are handed slots as they free (see GiveSlotsToWaiters), so
		// a step costs nothing per waiting record.
		bool NeedsPolling(const BodyRecord& record)
		{
			return record.TransformDirty;
		}

		const ShapeMember* FindShapeMember(const BodyRecord& record, entt::entity handle)
		{
			auto it = std::find_if(record.ShapeMembers.begin(), record.ShapeMembers.end(), [handle](const ShapeMember& member) { return member.Handle == handle; });
			return it != record.ShapeMembers.end() ? &*it : nullptr;
		}

		// Lists a record for the next step if it needs it. Records that no longer need it are dropped by the step.
		void UpdatePolling(PhysicsWorldData& data, entt::entity handle, BodyRecord& record)
		{
			if (record.Polled || !NeedsPolling(record))
				return;
			record.Polled = true;
			data.PolledBodies.push_back(handle);
		}

		//////////////////////////////////////////////////////////////////////////
		// Contacts
		//////////////////////////////////////////////////////////////////////////

		void QueueEvent(PhysicsWorldData& data, CollisionEventType type, const TouchingPair& pair)
		{
			CollisionEvent event;
			event.Type = type;
			event.IsTrigger = pair.IsTrigger;
			event.AID = pair.A;
			event.BID = pair.B;
			event.Point = pair.Point;
			event.Normal = pair.Normal;
			data.PendingEvents.push_back(event);
		}

		// The touching pairs of each entity are listed in EntityPairs, and each pair knows its position in the lists of both
		// its entities: adding and removing a pair take constant time, however many pairs an entity has (a floor under
		// thousands of boxes). PairIndexOperations counts the list entries written.
		void AddTouchingPair(PhysicsWorldData& data, const PairKey& key, TouchingPair pair)
		{
			std::vector<PairKey>& pairsOfA = data.EntityPairs[pair.A];
			pair.IndexInA = static_cast<uint32_t>(pairsOfA.size());
			pairsOfA.push_back(key);
			std::vector<PairKey>& pairsOfB = data.EntityPairs[pair.B];
			pair.IndexInB = static_cast<uint32_t>(pairsOfB.size());
			pairsOfB.push_back(key);
			data.PairIndexOperations += 2;
			data.TouchingPairs.emplace(key, pair);
		}

		uint32_t& GetIndexIn(TouchingPair& pair, UUID entityID)
		{
			return pair.A == entityID ? pair.IndexInA : pair.IndexInB;
		}

		// Removes a touching pair (still in TouchingPairs) from the list of one of its entities: the list's last pair takes its
		// place.
		void RemovePairFromIndex(PhysicsWorldData& data, UUID entityID, TouchingPair& pair)
		{
			auto it = data.EntityPairs.find(entityID);
			if (it == data.EntityPairs.end())
				return;

			std::vector<PairKey>& keys = it->second;
			const uint32_t index = GetIndexIn(pair, entityID);
			ST_CORE_ASSERT(index < keys.size(), "Physics: the touching pair index of an entity is inconsistent");
			if (index >= keys.size())
				return;
			data.PairIndexOperations++;
			if (index + 1 < keys.size())
			{
				keys[index] = keys.back();
				auto moved = data.TouchingPairs.find(keys[index]);
				ST_CORE_ASSERT(moved != data.TouchingPairs.end(), "Physics: an entity lists a pair that is not touching");
				if (moved != data.TouchingPairs.end())
					GetIndexIn(moved->second, entityID) = index;
				data.PairIndexOperations++;
			}
			keys.pop_back();
			if (keys.empty())
				data.EntityPairs.erase(it);
		}

		// Ends every contact of an entity (its body left the simulation).
		void EndContactsOf(PhysicsWorldData& data, UUID entityID)
		{
			auto indexIt = data.EntityPairs.find(entityID);
			if (indexIt == data.EntityPairs.end())
				return;

			const std::vector<PairKey> keys = std::move(indexIt->second);
			data.EntityPairs.erase(indexIt);

			data.PairIndexOperations += keys.size();
			std::vector<TouchingPair> ended;
			for (const PairKey& key : keys)
			{
				auto pairIt = data.TouchingPairs.find(key);
				if (pairIt == data.TouchingPairs.end())
					continue;

				RemovePairFromIndex(data, pairIt->second.A == entityID ? pairIt->second.B : pairIt->second.A, pairIt->second);
				ended.push_back(pairIt->second);
				data.TouchingPairs.erase(pairIt);
			}

			std::sort(ended.begin(), ended.end(), [](const TouchingPair& a, const TouchingPair& b) { return a.SortKey < b.SortKey; });
			for (const TouchingPair& pair : ended)
				QueueEvent(data, CollisionEventType::End, pair);
		}

		//////////////////////////////////////////////////////////////////////////
		// Simulation membership
		//////////////////////////////////////////////////////////////////////////

		// The bodies a query finds, whatever their layer (every shape's mask holds the query category).
		b3QueryFilter MakeAllShapesFilter()
		{
			return PhysicsFilters::MakeQueryFilter(c_AllPhysicsLayers);
		}

		// Wakes the dynamic and kinematic bodies in a box (static bodies never sleep).
		void WakeBodiesIn(PhysicsWorldData& data, b3AABB bounds)
		{
			bounds.lowerBound = b3Sub(bounds.lowerBound, b3Vec3 { c_WakeMargin, c_WakeMargin, c_WakeMargin });
			bounds.upperBound = b3Add(bounds.upperBound, b3Vec3 { c_WakeMargin, c_WakeMargin, c_WakeMargin });
			// Collected first: waking moves bodies between box3d's solver sets, which a query callback must not do.
			std::vector<b3ShapeId>& shapes = data.ShapeScratch;
			shapes.clear();
			b3World_OverlapAABB(data.World, bounds, MakeAllShapesFilter(), [](b3ShapeId shape, void* context)
			{
				static_cast<std::vector<b3ShapeId>*>(context)->push_back(shape);
				return true;
			}, &shapes);
			data.WakeQueryCount++;
			for (const b3ShapeId shape : shapes)
			{
				const b3BodyId body = b3Shape_GetBody(shape);
				if (b3Body_GetType(body) == b3_staticBody || !b3Body_IsEnabled(body))
					continue;
				b3Body_SetAwake(body, true);
				MarkAwake(data, body);
			}
		}

		// Wakes the bodies touching a body (which may rest on it) before it is moved.
		void WakeTouchingBodies(PhysicsWorldData& data, b3BodyId body)
		{
			const int capacity = b3Body_GetContactCapacity(body);
			if (capacity <= 0)
				return;
			std::vector<b3ContactData>& contacts = data.ContactScratch;
			contacts.resize(static_cast<size_t>(capacity));
			const int count = b3Body_GetContactData(body, contacts.data(), capacity);
			for (int index = 0; index < count; index++)
			{
				const b3BodyId bodyA = b3Shape_GetBody(contacts[index].shapeIdA);
				const b3BodyId other = B3_ID_EQUALS(bodyA, body) ? b3Shape_GetBody(contacts[index].shapeIdB) : bodyA;
				if (b3Body_GetType(other) == b3_staticBody)
					continue;
				b3Body_SetAwake(other, true);
				MarkAwake(data, other);
			}
		}

		void WakeAllBodies(PhysicsWorldData& data)
		{
			for (const auto& [handle, record] : data.Bodies)
			{
				if (record.InSimulation && record.Type != RigidBodyType::Static)
				{
					b3Body_SetAwake(record.BodyID, true);
					MarkAwake(data, record.BodyID);
				}
			}
		}

		// Keeps the motion of a dynamic body that leaves the simulation so that it resumes when the body is back, also when
		// the body is rebuilt in between.
		void SaveVelocities(BodyRecord& record)
		{
			if (!record.HasBody() || !record.InSimulation || record.Type != RigidBodyType::Dynamic)
				return;
			record.SavedLinearVelocity = ToGlm(b3Body_GetLinearVelocity(record.BodyID));
			record.SavedAngularVelocity = ToGlm(b3Body_GetAngularVelocity(record.BodyID));
		}

		// Takes a body out of the simulation: box3d disables it (it keeps its shapes) and wakes the bodies it touched.
		void RemoveFromSimulation(PhysicsWorldData& data, BodyRecord& record)
		{
			if (!record.InSimulation)
				return;

			SaveVelocities(record);
			b3Body_Disable(record.BodyID);
			record.InSimulation = false;
			record.KinematicMoving = false;
			record.HasPendingForce = false;
			EndContactsOf(data, record.EntityID);
		}

		enum class PlacementResult : uint8_t
		{
			Added,
			DegenerateTransform, // The body stays out of the simulation until the transform (or the parent's) is valid
			ScaleChanged         // The shape must be rebuilt
		};

		// Adds a built body back to the simulation at its entity's current pose.
		PlacementResult AddToSimulation(PhysicsWorldData& data, BodyRecord& record, Entity entity)
		{
			if (record.InSimulation)
				return PlacementResult::Added;
			if (record.Type == RigidBodyType::Dynamic && !HasInvertibleParent(*data.OwnerScene, entity))
				return PlacementResult::DegenerateTransform;

			const glm::mat4 worldTransform = data.OwnerScene->GetWorldTransform(entity);
			if (worldTransform != record.LastWorldTransform)
			{
				glm::vec3 position;
				glm::quat rotation;
				glm::vec3 scale;
				if (!Math::DecomposeTransform(worldTransform, position, rotation, scale) || !IsWithinCoordinateRange(position))
					return PlacementResult::DegenerateTransform;
				if (HasScaleChanged(scale, record.ShapeScale))
					return PlacementResult::ScaleChanged;
				record.WorldScale = scale;

				b3Body_SetTransform(record.BodyID, ToB3(position), ToB3(rotation));
				record.LastWorldTransform = worldTransform;
				record.KinematicTargetPosition = position;
				record.KinematicTargetRotation = rotation;
			}

			// Enabling finds the body's pairs again, and its contacts begin at the next step.
			b3Body_Enable(record.BodyID);
			if (record.Type == RigidBodyType::Dynamic)
			{
				// Motion resumes where it stopped.
				b3Body_SetLinearVelocity(record.BodyID, ToB3(record.SavedLinearVelocity));
				b3Body_SetAngularVelocity(record.BodyID, ToB3(record.SavedAngularVelocity));
				record.SavedLinearVelocity = glm::vec3(0.0f);
				record.SavedAngularVelocity = glm::vec3(0.0f);
			}
			if (record.Type != RigidBodyType::Static)
			{
				b3Body_SetAwake(record.BodyID, true);
				MarkAwake(data, record.BodyID);
			}
			if (record.Suspended)
			{
				// The transform problem that suspended the body is solved; it is reported again if it recurs.
				constexpr PhysicsWarning resolved[] = { PhysicsWarning::DegenerateTransform };
				ClearWarnings(data, record.EntityID, resolved);
			}
			record.InSimulation = true;
			record.Suspended = false;
			record.KinematicMoving = false;
			record.TransformDirty = false;
			return PlacementResult::Added;
		}

		// Destroys the box3d body of a record (the record itself stays). box3d wakes the bodies it touched. Contacts are left
		// alone: the caller ends them, or keeps them for a rebuilt body (see InstallBody).
		void DestroyBox3DBody(PhysicsWorldData& data, BodyRecord& record)
		{
			if (!record.HasBody())
				return;

			GetUpdateCounters(data).BodiesRemoved++;
			for (const b3ShapeId shape : record.Shapes)
				UnregisterShape(data, shape);
			UnregisterBody(data, record.BodyID);
			b3DestroyBody(record.BodyID);
			// Only now: the shapes pointed to these.
			record.Shapes.clear();
			record.CookedShapes.clear();
			record.Compounds.clear();
			record.BodyID = b3_nullBodyId;
			record.InSimulation = false;
			record.Suspended = false;
			record.KinematicMoving = false;
			record.HasPendingForce = false;
		}

		// Which pairs of an entity lost their box3d contacts.
		enum class LostContacts : uint8_t
		{
			All,  // A rebuild: the old shapes, their contacts and their sensor visits are gone
			Solid // A new filter (SetLayerCollisionMasks): box3d destroys the shapes' contacts but keeps their sensor visits
		};

		// The pairs of an entity whose shapes lost their contacts wait for the contacts of the next step to confirm them (see
		// ResolveRecheckedPairs): box3d reports the End of each lost contact and, where the shapes still touch, the Begin of a
		// new one with that step, which must neither end nor begin the pair again. A new filter keeps the trigger pairs'
		// counts: box3d checks sensor visits again at its next step and reports only the visits that changed.
		void MarkPairsUnconfirmed(PhysicsWorldData& data, UUID entityID, LostContacts lost = LostContacts::All)
		{
			auto indexIt = data.EntityPairs.find(entityID);
			if (indexIt == data.EntityPairs.end())
				return;
			for (const PairKey& key : indexIt->second)
			{
				auto pairIt = data.TouchingPairs.find(key);
				if (pairIt == data.TouchingPairs.end() || (lost == LostContacts::Solid && pairIt->second.IsTrigger))
					continue;
				pairIt->second.ContactCount = 0;
				pairIt->second.Unconfirmed = true;
			}
			data.RecheckedEntities.push_back(entityID);
		}

		//////////////////////////////////////////////////////////////////////////
		// Shapes
		//////////////////////////////////////////////////////////////////////////

		// One collider's part of a body, in body space.
		struct ShapePart
		{
			enum class Kind : uint8_t
			{
				Hull,       // A box: Hull is the world's or the build's box hull
				Sphere,
				Capsule,
				CookedHull, // A convex mesh collider: Cooked's hull, scaled by Scale, then placed by Placement
				Mesh,       // A triangle mesh collider at its body's origin: Cooked's mesh, scaled by Scale
				OffsetMesh  // A triangle mesh collider elsewhere in its (static) body: a baked compound of the mesh
			};
			Kind Type = Kind::Hull;
			const b3HullData* Hull = nullptr;
			b3Sphere Sphere = {};
			b3Capsule Capsule = {};
			Ref<const PhysicsCookedShape> Cooked;
			b3Transform Placement = b3Transform_identity;
			b3Vec3 Scale = { 1.0f, 1.0f, 1.0f };
		};

		// The box hulls a chunk of a parallel build makes (see RefreshOwnersInParallel), merged into the world's afterwards; the
		// world's are only read there (the main thread changes none while jobs run). Searched linearly: chunks hold a few sizes.
		struct ChunkHulls
		{
			static constexpr size_t c_MaxHulls = 64;
			std::vector<std::pair<BoxHullKey, Scope<b3BoxHull>>> Hulls;
		};

		// What building a body's shape found. Shapes are built on the main thread, which warns about what it cannot use, or on a
		// job thread (OffThread, see RefreshOwnersInParallel), which stops at the first problem or mesh collider and leaves the
		// body to the main thread (NeedsMainThread), so that warnings and mesh data stay on the main thread.
		struct ShapeBuild
		{
			std::vector<ShapePart> Parts;
			std::deque<b3BoxHull> OwnHulls;          // Box hulls beyond the shared ones (box3d copies them into its world)
			std::vector<ShapeMember> ShapeMembers;   // Entities that contributed colliders
			std::vector<MeshSource> MeshSources;     // Meshes of the mesh colliders considered
			bool WaitsForMesh = false;               // A mesh collider was left out until its data arrives or is cooked
			uint32_t ColliderCount = 0;              // Collider components considered, usable or not
			bool OffThread = false;
			bool NeedsMainThread = false;
			ChunkHulls* Chunk = nullptr; // Off the main thread
		};

		void WarnNonFiniteCollider(PhysicsWorldData& data, ShapeBuild& build, const Entity& entity, std::string_view collider)
		{
			if (build.OffThread)
			{
				build.NeedsMainThread = true;
				return;
			}
			if (ShouldWarn(data, entity.GetUUID(), PhysicsWarning::InvalidCollider))
				ST_CORE_WARN("Physics: the {} of '{}' has non-finite dimensions and is ignored", collider, entity.GetName());
		}

		// The box hull of these half extents at this placement: made once per size and placement and shared by the world's
		// bodies (up to c_MaxSharedBoxHulls of them), or by the bodies of a parallel build's chunk.
		const b3HullData* GetBoxHull(PhysicsWorldData& data, ShapeBuild& build, const glm::vec3& halfExtents, const b3Transform& placement)
		{
			const BoxHullKey key = MakeBoxHullKey(halfExtents, placement);
			const auto make = [&]() { return b3MakeTransformedBoxHull(halfExtents.x, halfExtents.y, halfExtents.z, placement); };
			if (const auto shared = data.BoxHulls.find(key); shared != data.BoxHulls.end())
				return &shared->second->base;
			if (build.OffThread)
			{
				ST_CORE_ASSERT(build.Chunk, "A shape built off the main thread belongs to a chunk");
				for (const auto& [chunkKey, hull] : build.Chunk->Hulls)
				{
					if (chunkKey == key)
						return &hull->base;
				}
				if (build.Chunk->Hulls.size() < ChunkHulls::c_MaxHulls)
				{
					build.Chunk->Hulls.emplace_back(key, CreateScope<b3BoxHull>(make()));
					return &build.Chunk->Hulls.back().second->base;
				}
			}
			else if (data.BoxHulls.size() < c_MaxSharedBoxHulls)
			{
				return &data.BoxHulls.emplace(key, CreateScope<b3BoxHull>(make())).first->second->base;
			}
			return &build.OwnHulls.emplace_back(make()).base;
		}

		// The cooked shape of a mesh collider: cooked once per mesh data object (process-wide, see PhysicsMeshShapes), shared
		// by every collider using the data. Returns nullptr if there is none: the mesh data has not arrived or is still
		// cooking (the build waits for it, see RefreshMeshBodies), or it cannot be used.
		Ref<const PhysicsCookedShape> GetMeshShape(PhysicsWorldData& data, const Entity& entity, AssetHandle mesh, bool convex, ShapeBuild& build)
		{
			const Ref<const PhysicsMeshData> meshData = GetActiveMeshProvider().GetMeshData(mesh);
			MeshSource& source = build.MeshSources.emplace_back(MeshSource { mesh, convex, meshData, false });
			if (!meshData)
			{
				if (ShouldWarn(data, entity.GetUUID(), PhysicsWarning::MissingMesh))
					ST_CORE_WARN("Physics: the mesh collider of '{}' waits for mesh {}, which is not available yet", entity.GetName(), mesh.ToString());
				source.Waiting = true;
				build.WaitsForMesh = true;
				return nullptr;
			}

			const PhysicsMeshShapes::Result cooked = PhysicsMeshShapes::Request(meshData, convex);
			if (cooked.CookState == PhysicsMeshShapes::State::Cooking)
			{
				source.Waiting = true;
				build.WaitsForMesh = true;
				return nullptr;
			}
			if (cooked.CookState != PhysicsMeshShapes::State::Ready || !cooked.Shape)
			{
				if (ShouldWarn(data, entity.GetUUID(), PhysicsWarning::InvalidMesh))
					ST_CORE_WARN("Physics: the mesh collider of '{}' cannot use mesh {} and is ignored: {}", entity.GetName(), mesh.ToString(), cooked.Error);
				return nullptr;
			}
			return cooked.Shape;
		}

		bool IsIdentityPlacement(const glm::vec3& translation, const glm::quat& rotation)
		{
			return translation == glm::vec3(0.0f) && rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		}

		// Adds the colliders of an entity whose transform in body space is (translation, rotation, scale).
		void AddColliderParts(PhysicsWorldData& data, const Entity& entity, const glm::mat4& poseInBody, const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale,
			RigidBodyType bodyType, bool trigger, ShapeBuild& build)
		{
			const glm::vec3 absoluteScale = glm::abs(scale);
			const float maxScale = std::max({ absoluteScale.x, absoluteScale.y, absoluteScale.z });
			const auto placeAt = [&](const glm::vec3& offset) { return translation + rotation * (offset * scale); };
			const size_t partCount = build.Parts.size();

			if (const BoxColliderComponent* box = entity.TryGetComponent<BoxColliderComponent>())
			{
				build.ColliderCount++;
				if (IsFinite(box->HalfExtents) && IsFinite(box->Offset))
				{
					// Boxes are symmetric, so mirroring needs nothing: the half extents scale by the absolute scale.
					const glm::vec3 halfExtents = glm::max(glm::abs(box->HalfExtents) * absoluteScale, glm::vec3(c_MinColliderExtent));
					ShapePart part;
					part.Type = ShapePart::Kind::Hull;
					part.Hull = GetBoxHull(data, build, halfExtents, ToB3Transform(placeAt(box->Offset), rotation));
					build.Parts.push_back(std::move(part));
				}
				else
				{
					WarnNonFiniteCollider(data, build, entity, "box collider");
				}
			}

			if (const SphereColliderComponent* sphere = entity.TryGetComponent<SphereColliderComponent>())
			{
				build.ColliderCount++;
				if (std::isfinite(sphere->Radius) && IsFinite(sphere->Offset))
				{
					ShapePart part;
					part.Type = ShapePart::Kind::Sphere;
					part.Sphere = b3Sphere { ToB3(placeAt(sphere->Offset)), std::max(std::abs(sphere->Radius) * maxScale, c_MinColliderExtent) };
					build.Parts.push_back(std::move(part));
				}
				else
				{
					WarnNonFiniteCollider(data, build, entity, "sphere collider");
				}
			}

			if (const CapsuleColliderComponent* capsule = entity.TryGetComponent<CapsuleColliderComponent>())
			{
				build.ColliderCount++;
				if (std::isfinite(capsule->Radius) && std::isfinite(capsule->HalfHeight) && IsFinite(capsule->Offset))
				{
					const float radius = std::max(std::abs(capsule->Radius) * std::max(absoluteScale.x, absoluteScale.z), c_MinColliderExtent);
					const float halfHeight = std::abs(capsule->HalfHeight) * absoluteScale.y;
					const glm::vec3 center = placeAt(capsule->Offset);
					ShapePart part;
					if (halfHeight < c_MinColliderExtent)
					{
						part.Type = ShapePart::Kind::Sphere; // No cylindrical part
						part.Sphere = b3Sphere { ToB3(center), radius };
					}
					else
					{
						// Along the collider's Y axis, through its center.
						const glm::vec3 axis = rotation * glm::vec3(0.0f, halfHeight, 0.0f);
						part.Type = ShapePart::Kind::Capsule;
						part.Capsule = b3Capsule { ToB3(center - axis), ToB3(center + axis), radius };
					}
					build.Parts.push_back(std::move(part));
				}
				else
				{
					WarnNonFiniteCollider(data, build, entity, "capsule collider");
				}
			}

			if (const MeshColliderComponent* meshCollider = entity.TryGetComponent<MeshColliderComponent>())
			{
				if (build.OffThread)
				{
					build.NeedsMainThread = true; // Mesh data comes from the main thread's provider
					return;
				}
				build.ColliderCount++;
				AssetHandle mesh = meshCollider->Mesh;
				if (!mesh.IsValid())
				{
					if (const MeshRendererComponent* renderer = entity.TryGetComponent<MeshRendererComponent>())
						mesh = renderer->Mesh;
				}

				const bool offset = !IsIdentityPlacement(translation, rotation);
				if (!mesh.IsValid())
				{
					if (ShouldWarn(data, entity.GetUUID(), PhysicsWarning::InvalidMesh))
						ST_CORE_WARN("Physics: the mesh collider of '{}' has no mesh (and no Mesh Renderer mesh to fall back to); it is ignored", entity.GetName());
				}
				else if (!meshCollider->Convex && bodyType != RigidBodyType::Static)
				{
					if (ShouldWarn(data, entity.GetUUID(), PhysicsWarning::UnsupportedMesh))
						ST_CORE_WARN("Physics: the non-convex mesh collider of '{}' is ignored: triangle meshes are only supported on static bodies (enable Convex)", entity.GetName());
				}
				else if (!meshCollider->Convex && trigger && offset)
				{
					// A triangle mesh away from its body's origin is a baked compound in box3d, which cannot be a sensor.
					if (ShouldWarn(data, entity.GetUUID(), PhysicsWarning::UnsupportedMesh))
					{
						ST_CORE_WARN("Physics: the non-convex mesh collider of '{}' is ignored: a triangle mesh in a trigger must be at its body's origin (put it on "
							"the trigger's entity, or enable Convex)", entity.GetName());
					}
				}
				else if (Ref<const PhysicsCookedShape> cooked = GetMeshShape(data, entity, mesh, meshCollider->Convex, build))
				{
					ShapePart part;
					part.Cooked = std::move(cooked);
					part.Placement = ToB3Transform(translation, rotation);
					part.Scale = ToB3(scale); // Convex hulls and triangle meshes support any non-zero scale, including mirroring
					part.Type = meshCollider->Convex ? ShapePart::Kind::CookedHull : (offset ? ShapePart::Kind::OffsetMesh : ShapePart::Kind::Mesh);
					build.Parts.push_back(std::move(part));
				}
			}

			if (build.Parts.size() > partCount)
				build.ShapeMembers.push_back({ entity.GetHandle(), poseInBody });
		}

		float SanitizeNonNegative(float value, float fallback)
		{
			return std::isfinite(value) ? std::max(value, 0.0f) : fallback;
		}

		// How a body is made: its definition (without its pose and motion), the definition its shapes share, and its mass.
		struct BodySetup
		{
			b3BodyDef Body = b3DefaultBodyDef();
			b3ShapeDef Shape = b3DefaultShapeDef();
			uint32_t Layer = 0;
			bool Trigger = false;
			float Mass = 0.0f; // Dynamic bodies: the total mass the shapes' mass distribution is scaled to
		};

		BodySetup MakeBodySetup(PhysicsWorldData& data, const Entity& entity, const RigidBodyComponent* rigidBody, RigidBodyType type)
		{
			const RigidBodyComponent defaults;
			BodySetup setup;
			if (rigidBody && rigidBody->Layer < c_PhysicsLayerCount)
			{
				setup.Layer = rigidBody->Layer;
			}
			else if (rigidBody && ShouldWarn(data, entity.GetUUID(), PhysicsWarning::InvalidLayer))
			{
				ST_CORE_WARN("Physics: '{}' uses collision layer {}, but layers range from 0 to {}; layer 0 is used", entity.GetName(), rigidBody->Layer, c_PhysicsLayerCount - 1);
			}

			setup.Body.type = ToBodyType(type);
			setup.Body.userData = nullptr;
			setup.Shape.filter = PhysicsFilters::MakeShapeFilter(setup.Layer, data.Settings.LayerCollisionMasks);
			setup.Shape.baseMaterial.friction = defaults.Friction;
			setup.Shape.baseMaterial.restitution = defaults.Restitution;
			// Every shape is seen by sensors, and every solid shape reports its contacts: box3d fixes a contact's events when the
			// contact is made, so Strata keeps only the pairs someone wants (see ProcessStepEvents).
			setup.Shape.enableSensorEvents = true;
			setup.Shape.enableContactEvents = true;
			// The mass comes from the component, the distribution from the shapes (see InstallBody and ApplyMass).
			setup.Shape.density = 1.0f;
			if (!rigidBody)
				return setup;

			setup.Shape.baseMaterial.friction = SanitizeNonNegative(rigidBody->Friction, defaults.Friction);
			setup.Shape.baseMaterial.restitution = std::isfinite(rigidBody->Restitution) ? std::clamp(rigidBody->Restitution, 0.0f, 1.0f) : defaults.Restitution;
			setup.Trigger = rigidBody->IsTrigger;
			setup.Shape.isSensor = rigidBody->IsTrigger;
			setup.Shape.enableContactEvents = !rigidBody->IsTrigger;
			if (type == RigidBodyType::Static)
				return setup;

			setup.Body.linearDamping = SanitizeNonNegative(rigidBody->LinearDamping, defaults.LinearDamping);
			setup.Body.angularDamping = SanitizeNonNegative(rigidBody->AngularDamping, defaults.AngularDamping);
			setup.Body.gravityScale = std::isfinite(rigidBody->GravityScale) ? rigidBody->GravityScale : defaults.GravityScale;
			if (type == RigidBodyType::Kinematic)
				return setup;

			// box3d sweeps every dynamic body against static ones; a bullet also against kinematic and dynamic bodies.
			setup.Body.isBullet = rigidBody->ContinuousCollision && !rigidBody->IsTrigger;
			setup.Mass = std::isfinite(rigidBody->Mass) ? std::max(rigidBody->Mass, c_MinimumMass) : defaults.Mass;
			// Rotation locks hold the world axes: box3d applies them to the world angular velocity.
			setup.Body.motionLocks.angularX = rigidBody->LockRotationX;
			setup.Body.motionLocks.angularY = rigidBody->LockRotationY;
			setup.Body.motionLocks.angularZ = rigidBody->LockRotationZ;
			return setup;
		}

		// Creates a shape of a body from its part; null if box3d refuses it.
		b3ShapeId CreatePartShape(BodyRecord& record, b3BodyId body, const b3ShapeDef& definition, const ShapePart& part)
		{
			switch (part.Type)
			{
				case ShapePart::Kind::Hull:
					return b3CreateHullShape(body, &definition, part.Hull);
				case ShapePart::Kind::Sphere:
					return b3CreateSphereShape(body, &definition, &part.Sphere);
				case ShapePart::Kind::Capsule:
					return b3CreateCapsuleShape(body, &definition, &part.Capsule);
				case ShapePart::Kind::CookedHull:
				{
					record.CookedShapes.push_back(part.Cooked);
					return b3CreateTransformedHullShape(body, &definition, part.Cooked->GetHull(), part.Placement, part.Scale);
				}
				case ShapePart::Kind::Mesh:
				{
					record.CookedShapes.push_back(part.Cooked);
					return b3CreateMeshShape(body, &definition, part.Cooked->GetMesh(), part.Scale);
				}
				case ShapePart::Kind::OffsetMesh:
				{
					// The compound holds the mesh by pointer (its triangles are shared, not copied): the record keeps both.
					b3CompoundMeshDef meshDefinition = {};
					meshDefinition.meshData = part.Cooked->GetMesh();
					meshDefinition.transform = part.Placement;
					meshDefinition.scale = part.Scale;
					b3CompoundDef compoundDefinition = {};
					compoundDefinition.meshes = &meshDefinition;
					compoundDefinition.meshCount = 1;
					b3CompoundData* compound = b3CreateCompound(&compoundDefinition);
					if (!compound)
						return b3_nullShapeId;
					Ref<const CompoundData> owned = CreateRef<const CompoundData>(compound);
					record.CookedShapes.push_back(part.Cooked);
					record.Compounds.push_back(owned);
					b3ShapeDef compoundShape = definition;
					return b3CreateBakedCompoundShape(body, &compoundShape, owned->Get());
				}
			}
			return b3_nullShapeId;
		}

		// Scales the mass the shapes give a dynamic body (density 1) to the component's mass; the distribution (center and
		// inertia) stays the shapes'.
		void ApplyMass(b3BodyId body, float mass)
		{
			b3Body_ApplyMassFromShapes(body);
			b3MassData massData = b3Body_GetMassData(body);
			if (!(massData.mass > 0.0f) || !std::isfinite(massData.mass))
				return;
			const float factor = mass / massData.mass;
			massData.mass = mass;
			massData.inertia = b3MulSM(factor, massData.inertia);
			b3Body_SetMassData(body, massData);
		}

		//////////////////////////////////////////////////////////////////////////
		// Bodies of entities
		//////////////////////////////////////////////////////////////////////////

		// Records the meshes a body's colliders use, indexed so that changes of a mesh only check the bodies using it (see
		// RefreshMeshBodies).
		void SetMeshSources(PhysicsWorldData& data, entt::entity handle, BodyRecord& record, std::vector<MeshSource> sources)
		{
			// Bodies without mesh colliders (most of them) are in none of the mesh indices before or after.
			if (record.MeshSources.empty() && sources.empty())
				return;
			for (const MeshSource& source : record.MeshSources)
			{
				auto users = data.MeshUsers.find(source.Mesh);
				if (users == data.MeshUsers.end())
					continue;
				std::erase(users->second, handle);
				if (users->second.empty())
					data.MeshUsers.erase(users);
			}

			record.MeshSources = std::move(sources);
			bool waiting = false;
			for (const MeshSource& source : record.MeshSources)
			{
				std::vector<entt::entity>& users = data.MeshUsers[source.Mesh];
				if (std::find(users.begin(), users.end(), handle) == users.end())
					users.push_back(handle);
				waiting |= source.Waiting;
			}

			if (record.MeshSources.empty())
				data.MeshBodies.erase(handle);
			else
				data.MeshBodies.insert(handle);
			if (waiting)
				data.WaitingMeshBodies.insert(handle);
			else
				data.WaitingMeshBodies.erase(handle);
		}

		// Removes the record of an entity that no longer owns a body. Its contacts end, and the colliders that were merged into
		// it are re-evaluated (they belong to another body now, or to none).
		void DestroyRecord(PhysicsWorldData& data, entt::entity handle)
		{
			auto it = data.Bodies.find(handle);
			if (it == data.Bodies.end())
				return;

			BodyRecord& record = it->second;
			DestroyBox3DBody(data, record);
			for (entt::entity merged : record.MergedEntities)
			{
				auto mergedIt = data.MergedOwners.find(merged);
				if (mergedIt != data.MergedOwners.end() && mergedIt->second == handle)
				{
					data.MergedOwners.erase(mergedIt);
					data.StructureChanges.Add(merged);
				}
			}

			const UUID entityID = record.EntityID;
			SetMeshSources(data, handle, record, {});
			data.TriggerBodies.erase(handle);
			SetFailure(data, handle, record, BuildFailure::None);
			EraseRecord(data, it); // A PolledBodies entry is dropped by the next step
			EndContactsOf(data, entityID);
		}

		// Records which collider entities belong to an owner's body, handing entities that left it (or that it took from
		// another body) over to be re-evaluated.
		void UpdateMergedEntities(PhysicsWorldData& data, entt::entity owner, BodyRecord& record, std::vector<entt::entity> merged)
		{
			for (entt::entity previous : record.MergedEntities)
			{
				if (std::find(merged.begin(), merged.end(), previous) != merged.end())
					continue;
				auto it = data.MergedOwners.find(previous);
				if (it != data.MergedOwners.end() && it->second == owner)
				{
					data.MergedOwners.erase(it);
					data.StructureChanges.Add(previous);
				}
			}

			for (entt::entity entity : merged)
			{
				auto it = data.MergedOwners.find(entity);
				if (it != data.MergedOwners.end() && it->second != owner)
					data.OwnerRefreshes.Add(it->second); // Taken from another body
				data.MergedOwners[entity] = owner;
				if (FindRecord(data, entity))
					DestroyRecord(data, entity); // Was a static body of its own
			}

			record.MergedEntities = std::move(merged);
		}

		// Marks a record as unable to have a body for now; the old body (if any) is destroyed and its contacts end. A dynamic
		// body keeps its motion for when it can be built again. Expects record.Type to be the type the body is built with.
		void SetBuildFailure(PhysicsWorldData& data, entt::entity handle, BodyRecord& record, BuildFailure failure, const glm::mat4& worldTransform)
		{
			SaveVelocities(record);
			if (record.Type != RigidBodyType::Dynamic)
			{
				record.SavedLinearVelocity = glm::vec3(0.0f);
				record.SavedAngularVelocity = glm::vec3(0.0f);
			}
			DestroyBox3DBody(data, record);
			SetFailure(data, handle, record, failure);
			record.LastWorldTransform = worldTransform;
			record.ShapeMembers.clear();
			EndContactsOf(data, record.EntityID);
			UpdatePolling(data, handle, record);
		}

		// The first part of a body's (re)build, which only the main thread may do, in record order: the record, the body type
		// and the collider entities merged into it (see UpdateMergedEntities).
		struct BodyBuildStart
		{
			Entity Owner;
			BodyRecord* Record = nullptr;
			const RigidBodyComponent* RigidBody = nullptr;
			RigidBodyType Type = RigidBodyType::Static;
		};

		BodyBuildStart BeginBodyBuild(PhysicsWorldData& data, Entity entity)
		{
			data.BuildCount++;
			BodyBuildStart start;
			start.Owner = entity;
			start.RigidBody = entity.TryGetComponent<RigidBodyComponent>();
			const entt::entity handle = entity.GetHandle();
			auto [record, created] = EmplaceRecord(data, handle);
			start.Record = record;
			if (created)
				start.Record->EntityID = entity.GetUUID();

			start.Type = start.RigidBody ? start.RigidBody->Type : RigidBodyType::Static;
			if (start.Type != RigidBodyType::Static && start.Type != RigidBodyType::Dynamic && start.Type != RigidBodyType::Kinematic)
			{
				if (ShouldWarn(data, start.Record->EntityID, PhysicsWarning::InvalidType))
					ST_CORE_WARN("Physics: '{}' has an invalid rigid body type {}; it is treated as static", entity.GetName(), static_cast<int>(start.Type));
				start.Type = RigidBodyType::Static;
			}

			// Collider descendants only merge into rigid bodies; collider entities without one are static bodies of their own.
			UpdateMergedEntities(data, handle, *start.Record, start.RigidBody ? CollectMergedEntities(*data.OwnerScene, entity, data.VisitStack) : std::vector<entt::entity>());
			return start;
		}

		// The motion a dynamic body (re)built now starts with: a body rebuilt because a property changed keeps moving as before;
		// one that was out of the simulation (or had no body after a failed build) resumes the motion it had.
		void GetBuildVelocities(const BodyRecord& record, RigidBodyType type, glm::vec3& outLinear, glm::vec3& outAngular)
		{
			outLinear = glm::vec3(0.0f);
			outAngular = glm::vec3(0.0f);
			if (type != RigidBodyType::Dynamic)
				return;
			if (record.HasBody() && record.InSimulation)
			{
				outLinear = ToGlm(b3Body_GetLinearVelocity(record.BodyID));
				outAngular = ToGlm(b3Body_GetAngularVelocity(record.BodyID));
				return;
			}
			outLinear = record.SavedLinearVelocity;
			outAngular = record.SavedAngularVelocity;
		}

		// Where a body is built: its entity's world transform and that transform's parts.
		struct BodyPose
		{
			glm::mat4 WorldTransform = glm::mat4(1.0f);
			glm::vec3 Position = glm::vec3(0.0f);
			glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			glm::vec3 Scale = glm::vec3(1.0f);
		};

		// Grows the room of a world to hold `required` bodies: box3d grows its storage itself while bodies are created, and the
		// world counts it as a growth to the next power of two of at least twice the room (and records it as the scene's
		// demand, so that its next world reserves the room at once). Returns whether the room grew.
		bool GrowRoom(PhysicsWorldData& data, uint64_t required)
		{
			if (required <= data.RoomBodies)
				return false;
			const uint32_t from = data.RoomBodies;
			const uint32_t to = static_cast<uint32_t>(std::min<uint64_t>(std::max<uint64_t>(NextPowerOfTwo(std::max<uint64_t>(2ull * from, required)), required),
				PhysicsCapacitySettings::c_MaxBodiesLimit));
			if (to <= from)
				return false;
			data.RoomBodies = to;
			data.GrowthCount++;
			data.LastGrowthFrom = from;
			data.LastGrowthTo = to;
			data.GrewThisRound = true;
			data.Demand.Bodies = std::max(data.Demand.Bodies, to);
			return true;
		}

		// Creates the box3d body of a record (on the main thread, in record order, so that IDs and results do not depend on the
		// number of threads) from its built shape, and makes it the record's body; the old body (a rebuild) goes first, its
		// contacts carried over to be confirmed by the next step. At its growth limit a world leaves a record that has no body
		// waiting for one (reported for all such records at once, see ReportBodyLimit).
		void InstallBody(PhysicsWorldData& data, const BodyBuildStart& start, const BodyPose& pose, ShapeBuild& build, const BodySetup& setup,
			const glm::vec3& linearVelocity, const glm::vec3& angularVelocity)
		{
			BodyRecord& record = *start.Record;
			const entt::entity handle = start.Owner.GetHandle();
			const RigidBodyType type = start.Type;
			if (!record.HasBody() && GetUsedBodySlots(data) >= GetGrowthLimit(data.Settings))
			{
				record.SavedLinearVelocity = linearVelocity;
				record.SavedAngularVelocity = angularVelocity;
				SetBuildFailure(data, handle, record, BuildFailure::WaitingForSlot, pose.WorldTransform);
				return;
			}

			// Contacts are tracked per entity pair and carry over to the new body; the next step confirms or ends them.
			if (record.HasBody())
			{
				DestroyBox3DBody(data, record);
				MarkPairsUnconfirmed(data, record.EntityID);
			}

			const bool simulated = IsSimulated(*data.OwnerScene, start.Owner);
			b3BodyDef bodyDefinition = setup.Body;
			bodyDefinition.position = ToB3(pose.Position);
			bodyDefinition.rotation = ToB3(pose.Rotation);
			// A body that joins the simulation now starts with that motion (one that does not gets it when it is added).
			if (simulated && type == RigidBodyType::Dynamic)
			{
				bodyDefinition.linearVelocity = ToB3(linearVelocity);
				bodyDefinition.angularVelocity = ToB3(angularVelocity);
			}
			bodyDefinition.isEnabled = simulated;
			bodyDefinition.isAwake = true;
			const b3BodyId body = b3CreateBody(data.World, &bodyDefinition);
			RegisterBody(data, body, handle, &record);

			// Before the world's first step the build need not look for pairs of static shapes (the first step finds every
			// pair); afterwards a static shape that does not would miss the bodies resting where it appears until they move.
			b3ShapeDef shapeDefinition = setup.Shape;
			// The entity's UUID, for code that has only box3d's shape (the character controllers' moves).
			shapeDefinition.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint64_t>(record.EntityID)));
			shapeDefinition.invokeContactCreation = data.HasStepped || type != RigidBodyType::Static;
			// A dynamic body's mass is set once its shapes are made (ApplyMass); other bodies have none to compute.
			shapeDefinition.updateBodyMass = type != RigidBodyType::Dynamic;
			ShapeSlot shapeSlot;
			shapeSlot.Sensor = setup.Trigger;
			shapeSlot.BodyType = type;
			shapeSlot.Entity = record.EntityID;
			shapeSlot.BodyKey = GetBodyKey(body);
			for (const ShapePart& part : build.Parts)
			{
				const b3ShapeId shape = CreatePartShape(record, body, shapeDefinition, part);
				if (B3_IS_NULL(shape))
					continue;
				shapeSlot.Convex = part.Type != ShapePart::Kind::Mesh && part.Type != ShapePart::Kind::OffsetMesh;
				RegisterShape(data, shape, shapeSlot);
				record.Shapes.push_back(shape);
			}
			record.BodyID = body;
			if (record.Shapes.empty())
			{
				// box3d refused every part (it should not: the parts were validated); the body waits for a component change.
				if (ShouldWarn(data, record.EntityID, PhysicsWarning::NoUsableCollider))
					ST_CORE_WARN("Physics: '{}' has no usable collider; it is not simulated", start.Owner.GetName());
				record.Type = type;
				SetBuildFailure(data, handle, record, BuildFailure::InvalidColliders, pose.WorldTransform);
				return;
			}
			if (type == RigidBodyType::Dynamic)
				ApplyMass(body, setup.Mass);

			GetUpdateCounters(data).BodiesAdded++;
			(type == RigidBodyType::Static ? data.RoundCreatedStatic : data.RoundCreatedMoving) = true;
			SetFailure(data, handle, record, BuildFailure::None);
			record.Type = type;
			record.Layer = setup.Layer;
			record.IsTrigger = setup.Trigger;
			if (setup.Trigger)
				data.TriggerBodies.insert(handle);
			else
				data.TriggerBodies.erase(handle);
			record.ShapeScale = pose.Scale;
			record.WorldScale = pose.Scale;
			record.LastWorldTransform = pose.WorldTransform;
			record.KinematicTargetPosition = pose.Position;
			record.KinematicTargetRotation = pose.Rotation;
			record.ShapeMembers = std::move(build.ShapeMembers);
			ClearWarnings(data, record.EntityID, c_TransientWarnings);

			if (!simulated)
			{
				// Its motion applies when it joins the simulation (AddToSimulation).
				record.SavedLinearVelocity = type == RigidBodyType::Dynamic ? linearVelocity : glm::vec3(0.0f);
				record.SavedAngularVelocity = type == RigidBodyType::Dynamic ? angularVelocity : glm::vec3(0.0f);
				record.InSimulation = false;
				EndContactsOf(data, record.EntityID);
			}
			else
			{
				record.InSimulation = true;
				record.TransformDirty = false;
				record.SavedLinearVelocity = glm::vec3(0.0f);
				record.SavedAngularVelocity = glm::vec3(0.0f);
				if (type != RigidBodyType::Static)
					MarkAwake(data, body);
			}
			UpdatePolling(data, handle, record);
		}

		// The rest of a body's (re)build on the main thread, after BeginBodyBuild: its shape from the owner's colliders and those
		// merged into it, placed at the owner's world transform; failures and what cannot be used are warned about.
		void CompleteBodyBuild(PhysicsWorldData& data, const BodyBuildStart& start)
		{
			Scene& scene = *data.OwnerScene;
			const Entity& entity = start.Owner;
			const entt::entity handle = entity.GetHandle();
			BodyRecord& record = *start.Record;
			const RigidBodyType type = start.Type;

			BodyPose pose;
			pose.WorldTransform = scene.GetWorldTransform(entity);
			const bool decomposed = Math::DecomposeTransform(pose.WorldTransform, pose.Position, pose.Rotation, pose.Scale);
			const bool inRange = decomposed && IsWithinCoordinateRange(pose.Position);
			if (!inRange || (type == RigidBodyType::Dynamic && !HasInvertibleParent(scene, entity)))
			{
				if (ShouldWarn(data, record.EntityID, PhysicsWarning::DegenerateTransform))
				{
					if (!decomposed)
						ST_CORE_WARN("Physics: '{}' has a degenerate world transform (zero scale or non-finite values); it gets its body once the transform is valid", entity.GetName());
					else if (!inRange)
						ST_CORE_WARN("Physics: '{}' is more than {} m from the origin, beyond what the simulation supports; it gets its body once it is back within", entity.GetName(), c_MaxBodyCoordinate);
					else
						ST_CORE_WARN("Physics: the parent of '{}' is scaled to (nearly) zero, so its simulated pose cannot be written back; it gets its body once the parent's transform is valid", entity.GetName());
				}
				record.Type = type;
				SetBuildFailure(data, handle, record, BuildFailure::DegenerateTransform, pose.WorldTransform);
				record.WaitsForParent = inRange; // The world transform may stay the same when the parent is fixed
				return;
			}
			record.WaitsForParent = false;

			const bool trigger = start.RigidBody && start.RigidBody->IsTrigger;
			ShapeBuild build;
			AddColliderParts(data, entity, glm::mat4(1.0f), glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), pose.Scale, type, trigger, build);

			// Merged colliders are placed with their transform relative to the body (position and rotation, without scale).
			const glm::mat4 worldToBody = glm::inverse(Math::ComposeTransform(pose.Position, pose.Rotation, glm::vec3(1.0f)));
			for (entt::entity mergedHandle : record.MergedEntities)
			{
				const Entity merged(mergedHandle, &scene);
				if (!IsPartOfOwnerShape(scene, merged, entity))
					continue;

				const glm::mat4 poseInBody = worldToBody * scene.GetWorldTransform(merged); // As in HasShapePartChanged
				glm::vec3 partTranslation;
				glm::quat partRotation;
				glm::vec3 partScale;
				if (!Math::DecomposeTransform(poseInBody, partTranslation, partRotation, partScale))
				{
					if (ShouldWarn(data, merged.GetUUID(), PhysicsWarning::DegenerateTransform))
						ST_CORE_WARN("Physics: the colliders of '{}' have a degenerate transform and are left out of the body of '{}'", merged.GetName(), entity.GetName());
					continue;
				}
				AddColliderParts(data, merged, poseInBody, partTranslation, partRotation, partScale, type, trigger, build);
			}

			record.Type = type;
			SetMeshSources(data, handle, record, std::move(build.MeshSources));

			if (build.Parts.empty())
			{
				BuildFailure failure = BuildFailure::InvalidColliders;
				if (build.WaitsForMesh)
				{
					failure = BuildFailure::MissingMesh;
				}
				else if (build.ColliderCount == 0)
				{
					failure = BuildFailure::NoCollider;
					if (start.RigidBody && ShouldWarn(data, record.EntityID, PhysicsWarning::MissingCollider))
						ST_CORE_WARN("Physics: '{}' has a Rigid Body but no collider; it is not simulated", entity.GetName());
				}
				else if (ShouldWarn(data, record.EntityID, PhysicsWarning::NoUsableCollider))
				{
					ST_CORE_WARN("Physics: '{}' has no usable collider; it is not simulated", entity.GetName());
				}
				SetBuildFailure(data, handle, record, failure, pose.WorldTransform);
				return;
			}

			glm::vec3 linearVelocity;
			glm::vec3 angularVelocity;
			GetBuildVelocities(record, type, linearVelocity, angularVelocity);
			const BodySetup setup = MakeBodySetup(data, entity, start.RigidBody, type);
			InstallBody(data, start, pose, build, setup, linearVelocity, angularVelocity);
		}

		// (Re)builds the body of an owner entity from its components, its collider descendants and its world transform.
		void BuildBody(PhysicsWorldData& data, Entity entity)
		{
			ST_PROFILE_FUNCTION();
			CompleteBodyBuild(data, BeginBodyBuild(data, entity));
		}

		// Re-evaluates an entity that may own a body: builds or rebuilds the body, or removes the record if the entity no
		// longer owns one (destroyed, lost its components, or its colliders now belong to an ancestor's rigid body).
		void RefreshOwner(PhysicsWorldData& data, entt::entity handle)
		{
			const Entity entity(handle, data.OwnerScene);
			const Entity owner = FindBodyOwner(entity);
			if (owner == entity && entity.IsValid())
			{
				BuildBody(data, entity);
				return;
			}

			DestroyRecord(data, handle);
			if (owner.IsValid())
				data.OwnerRefreshes.Add(owner.GetHandle()); // Its colliders are part of the ancestor's body now
		}

		// Rounds of changes with at least this many owners to refresh build their bodies' shapes on the job threads.
		constexpr uint32_t c_ParallelBuildThreshold = 256;
		// Owners per job of a parallel build (fixed, so that the result does not depend on the number of threads).
		constexpr uint32_t c_ParallelBuildChunk = 512;

		// A body whose shape is built on the job threads (see RefreshOwnersInParallel).
		struct ParallelBodyBuild
		{
			BodyBuildStart Start;
			// Read on the main thread before the jobs: the world transforms of the owner and of the merged entities that are part
			// of its shape (the scene computes stale ones on demand), the motion the body starts with, and whether its parent
			// can be inverted (dynamic bodies).
			glm::mat4 WorldTransform = glm::mat4(1.0f);
			std::vector<std::pair<entt::entity, glm::mat4>> Parts;
			glm::vec3 LinearVelocity = glm::vec3(0.0f);
			glm::vec3 AngularVelocity = glm::vec3(0.0f);
			bool Eligible = false;
			// Results of the jobs: the shape, the pose and the body's setup, or NeedsMainThread for anything a warning, a failure
			// or mesh data may come from: such owners are built the serial way, in their place.
			BodyPose Pose;
			ShapeBuild Build;
			BodySetup Setup;
			bool NeedsMainThread = true;
		};

		// Builds what the jobs may for one owner: its shape from box, sphere and capsule colliders (box hulls shared within the
		// chunk) and its setup. Reads only components, the transforms read before and the world's settings; creates nothing
		// in box3d, whose creation functions are main-thread only.
		void BuildBodyOffThread(PhysicsWorldData& data, ParallelBodyBuild& item, ChunkHulls& chunkHulls)
		{
			const Entity& entity = item.Start.Owner;
			BodyPose& pose = item.Pose;
			pose.WorldTransform = item.WorldTransform;
			if (!Math::DecomposeTransform(pose.WorldTransform, pose.Position, pose.Rotation, pose.Scale) || !IsWithinCoordinateRange(pose.Position))
				return;

			ShapeBuild& build = item.Build;
			build.OffThread = true;
			build.Chunk = &chunkHulls;
			const bool trigger = item.Start.RigidBody && item.Start.RigidBody->IsTrigger;
			AddColliderParts(data, entity, glm::mat4(1.0f), glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), pose.Scale, item.Start.Type, trigger, build);
			const glm::mat4 worldToBody = glm::inverse(Math::ComposeTransform(pose.Position, pose.Rotation, glm::vec3(1.0f)));
			for (const auto& [mergedHandle, mergedWorld] : item.Parts)
			{
				if (build.NeedsMainThread)
					return;
				const glm::mat4 poseInBody = worldToBody * mergedWorld; // As in HasShapePartChanged
				glm::vec3 partTranslation;
				glm::quat partRotation;
				glm::vec3 partScale;
				if (!Math::DecomposeTransform(poseInBody, partTranslation, partRotation, partScale))
					return;
				AddColliderParts(data, Entity(mergedHandle, data.OwnerScene), poseInBody, partTranslation, partRotation, partScale, item.Start.Type, trigger, build);
			}
			if (build.NeedsMainThread || build.Parts.empty())
				return;

			// MakeBodySetup warns only about invalid layers, which the main thread checked before.
			item.Setup = MakeBodySetup(data, entity, item.Start.RigidBody, item.Start.Type);
			item.NeedsMainThread = false;
		}

		// Refreshes many owners at once, with the results of the serial path (RefreshOwner, in record order): the same body IDs,
		// shapes and settings, so the simulation does not depend on the number of threads. The main thread starts each build in
		// record order (records, merged colliders, transforms, velocities), the job threads build shapes and setups in fixed
		// chunks, and the main thread creates the bodies in record order, building the serial way the owners the jobs left to
		// it.
		void RefreshOwnersInParallel(PhysicsWorldData& data, std::span<const entt::entity> handles)
		{
			ST_PROFILE_FUNCTION();
			data.ParallelBuildCount++;
			Scene& scene = *data.OwnerScene;
			std::vector<ParallelBodyBuild> items;
			items.reserve(handles.size());
			for (const entt::entity handle : handles)
			{
				const Entity entity(handle, &scene);
				const Entity owner = FindBodyOwner(entity);
				if (owner != entity || !entity.IsValid())
				{
					RefreshOwner(data, handle);
					continue;
				}

				ParallelBodyBuild& item = items.emplace_back();
				item.Start = BeginBodyBuild(data, entity);
				const BodyRecord& record = *item.Start.Record;
				const RigidBodyComponent* rigidBody = item.Start.RigidBody;
				item.Eligible = !entity.HasComponent<MeshColliderComponent>() && (!rigidBody || rigidBody->Layer < c_PhysicsLayerCount)
					&& (item.Start.Type != RigidBodyType::Dynamic || HasInvertibleParent(scene, entity));
				if (!item.Eligible)
					continue;
				item.WorldTransform = scene.GetWorldTransform(entity);
				for (const entt::entity mergedHandle : record.MergedEntities)
				{
					const Entity merged(mergedHandle, &scene);
					if (!IsPartOfOwnerShape(scene, merged, entity))
						continue;
					if (merged.HasComponent<MeshColliderComponent>())
					{
						item.Eligible = false;
						break;
					}
					item.Parts.emplace_back(mergedHandle, scene.GetWorldTransform(merged));
				}
				GetBuildVelocities(record, item.Start.Type, item.LinearVelocity, item.AngularVelocity);
			}

			const uint32_t itemCount = static_cast<uint32_t>(items.size());
			const uint32_t chunkCount = (itemCount + c_ParallelBuildChunk - 1) / c_ParallelBuildChunk;
			std::vector<ChunkHulls> chunkHulls(chunkCount);
			const auto build = [&](uint32_t firstChunk, uint32_t lastChunk)
			{
				for (uint32_t chunk = firstChunk; chunk < lastChunk; chunk++)
				{
					for (uint32_t index = chunk * c_ParallelBuildChunk; index < std::min(itemCount, (chunk + 1) * c_ParallelBuildChunk); index++)
					{
						if (items[index].Eligible)
							BuildBodyOffThread(data, items[index], chunkHulls[chunk]);
					}
				}
			};
			if (chunkCount > 1 && JobSystem::IsInitialized())
				JobSystem::ParallelFor(chunkCount, 1, build);
			else
				build(0, chunkCount);

			// The bodies, in record order. The chunks' hulls stay alive until every body is made: the shapes point at them until
			// box3d copied them.
			for (ParallelBodyBuild& item : items)
			{
				if (item.NeedsMainThread)
				{
					CompleteBodyBuild(data, item.Start);
					continue;
				}
				BodyRecord& record = *item.Start.Record;
				record.Type = item.Start.Type;
				record.WaitsForParent = false;
				SetMeshSources(data, item.Start.Owner.GetHandle(), record, {});
				InstallBody(data, item.Start, item.Pose, item.Build, item.Setup, item.LinearVelocity, item.AngularVelocity);
			}

			// Later builds share the chunks' hulls, in chunk order.
			for (ChunkHulls& chunk : chunkHulls)
			{
				for (auto& [key, hull] : chunk.Hulls)
				{
					if (data.BoxHulls.size() >= c_MaxSharedBoxHulls)
						break;
					data.BoxHulls.try_emplace(key, std::move(hull));
				}
			}
		}

		void RefreshActivity(PhysicsWorldData& data, entt::entity handle, BodyRecord& record)
		{
			const Entity entity(handle, data.OwnerScene);
			if (!entity.IsValid())
			{
				data.StructureChanges.Add(handle);
				return;
			}

			if (!IsSimulated(*data.OwnerScene, entity))
			{
				RemoveFromSimulation(data, record);
			}
			else if (record.HasBody() && !record.InSimulation)
			{
				const PlacementResult result = AddToSimulation(data, record, entity);
				if (result == PlacementResult::DegenerateTransform)
					record.Suspended = true;
				else if (result == PlacementResult::ScaleChanged)
					data.OwnerRefreshes.Add(handle);
			}
			UpdatePolling(data, handle, record);
		}

		bool IsNearlyEqualTransform(const glm::mat4& a, const glm::mat4& b)
		{
			constexpr float tolerance = 1.0e-5f;
			for (int column = 0; column < 4; column++)
			{
				for (int row = 0; row < 4; row++)
				{
					if (!(glm::abs(a[column][row] - b[column][row]) <= tolerance * std::max(1.0f, glm::abs(b[column][row]))))
						return false;
				}
			}
			return true;
		}

		// Whether a merged collider entity (staying with the same body) changes its body's shape: it joins or leaves the
		// shape (activity, pending destruction), or it moved relative to the body.
		bool HasShapePartChanged(PhysicsWorldData& data, const BodyRecord& ownerRecord, Entity owner, Entity part)
		{
			const Scene& scene = *data.OwnerScene;
			if (!scene.IsDescendantOf(part, owner))
				return true; // Moved away from the body (re-evaluated as a structure change)
			const ShapeMember* member = FindShapeMember(ownerRecord, part.GetHandle());
			const bool inShape = IsPartOfOwnerShape(scene, part, owner);
			if (!member || !inShape)
				return member || inShape; // Joins or leaves the shape (or a part left out before may fit now)

			glm::vec3 position;
			glm::quat rotation;
			glm::vec3 scale;
			if (!Math::DecomposeTransform(scene.GetWorldTransform(owner), position, rotation, scale))
				return true;
			const glm::mat4 worldToBody = glm::inverse(Math::ComposeTransform(position, rotation, glm::vec3(1.0f)));
			return !IsNearlyEqualTransform(worldToBody * scene.GetWorldTransform(part), member->PoseInBody); // As in BuildBody
		}

		// Rebuilds a body if a change of one of its merged collider entities changed its shape.
		void RefreshOwnerIfShapeChanged(PhysicsWorldData& data, entt::entity owner, Entity part)
		{
			const BodyRecord* ownerRecord = FindRecord(data, owner);
			if (!ownerRecord || HasShapePartChanged(data, *ownerRecord, Entity(owner, data.OwnerScene), part))
				data.OwnerRefreshes.Add(owner);
		}

		// A reparented subtree: physics entities that stay with their body only follow their entity's transform and activity
		// (a merged collider changes its body's shape only if it moved relative to the body, or joined or left the shape);
		// entities that change bodies are re-evaluated like any structure change. This keeps reparenting from rebuilding
		// every body below the moved entity.
		void ApplyHierarchyChange(PhysicsWorldData& data, entt::entity handle)
		{
			Scene& scene = *data.OwnerScene;
			VisitSubtree(scene, Entity(handle, &scene), data.VisitStack, [&](Entity entity)
			{
				if (!HasPhysicsComponent(entity))
					return true;

				const Entity owner = FindBodyOwner(entity);
				if (BodyRecord* record = FindRecord(data, entity.GetHandle()))
				{
					if (owner != entity)
					{
						data.StructureChanges.Add(entity.GetHandle()); // Its colliders join an ancestor's body
						return true;
					}
					RefreshActivity(data, entity.GetHandle(), *record);
					record->TransformDirty = true;
					UpdatePolling(data, entity.GetHandle(), *record);
					return true;
				}

				auto merged = data.MergedOwners.find(entity.GetHandle());
				if (merged == data.MergedOwners.end() || !owner || owner.GetHandle() != merged->second)
				{
					data.StructureChanges.Add(entity.GetHandle()); // Changes bodies
					return true;
				}
				RefreshOwnerIfShapeChanged(data, merged->second, entity);
				return true;
			});
		}

		// Applies the changes of the character controllers (see CharacterControllerSet::ApplyChanges): their presence bodies
		// are known by their slots like records' bodies (events and queries name them), contacts are wanted through them like
		// through bodies and end when a controller leaves the simulation; a rebuilt controller keeps its contacts, which the
		// next step confirms or ends.
		void ApplyCharacterChanges(PhysicsWorldData& data)
		{
			if (!data.Characters)
				return; // The world is being destroyed
			std::vector<CharacterBodyChange>& changes = data.CharacterChanges;
			changes.clear();
			data.Characters->ApplyChanges(changes);
			for (const CharacterBodyChange& change : changes)
			{
				if (B3_IS_NON_NULL(change.OldShape))
					UnregisterShape(data, change.OldShape);
				if (B3_IS_NON_NULL(change.OldBody))
					UnregisterBody(data, change.OldBody);
				if (B3_IS_NON_NULL(change.NewBody))
				{
					RegisterBody(data, change.NewBody, entt::null, nullptr);
					ShapeSlot slot;
					slot.BodyType = RigidBodyType::Kinematic;
					slot.Entity = change.Entity;
					slot.BodyKey = GetBodyKey(change.NewBody);
					RegisterShape(data, change.NewShape, slot);
					MarkAwake(data, change.NewBody);
				}
				if (change.Removed)
					EndContactsOf(data, change.Entity);
				else if (data.EntityPairs.find(change.Entity) != data.EntityPairs.end())
					MarkPairsUnconfirmed(data, change.Entity);
			}
		}

		bool HasChanges(const PhysicsWorldData& data)
		{
			return !data.StructureChanges.IsEmpty() || !data.SubtreeChanges.IsEmpty() || !data.ActivityChanges.IsEmpty() || !data.TransformChanges.IsEmpty() || !data.OwnerRefreshes.IsEmpty();
		}

		// Hands the room of bodies that went (those destroyed in this round of changes count) to the records that waited longest
		// for it at the growth limit, in the order they started waiting: they are rebuilt with the next round. Entries of
		// records that no longer wait are dropped on the way. Returns whether a record was handed room. A record handed room
		// keeps its place until it has a body, so one that loses it to another build of the round stays first.
		bool GiveSlotsToWaiters(PhysicsWorldData& data)
		{
			if (data.SlotWaiterCount == 0)
				return false;
			const uint64_t used = GetUsedBodySlots(data);
			const uint64_t limit = GetGrowthLimit(data.Settings);
			if (used >= limit)
				return false;

			uint64_t free = limit - used;
			while (!data.SlotWaiters.empty() && !IsSlotWaiter(data, data.SlotWaiters.front()))
				data.SlotWaiters.pop_front();

			bool given = false;
			for (size_t index = 0; index < data.SlotWaiters.size() && free > 0; index++)
			{
				if (!IsSlotWaiter(data, data.SlotWaiters[index]))
					continue;
				data.OwnerRefreshes.Add(data.SlotWaiters[index].first);
				free--;
				given = true;
			}
			return given;
		}

		// Applies recorded changes round after round until none are left (a round's changes can record more: colliders move
		// to other bodies). False if c_MaxChangeRounds rounds did not settle them.
		bool ApplyChangeRounds(PhysicsWorldData& data)
		{
			Scene& scene = *data.OwnerScene;
			for (uint32_t round = 0; round < c_MaxChangeRounds && HasChanges(data); round++)
			{
				// Hierarchy changes may move any physics entity of the subtree to another body.
				for (entt::entity handle : data.SubtreeChanges.TakeInto(data.ChangeScratch))
				{
					if (Entity(handle, &scene).IsValid())
						ApplyHierarchyChange(data, handle);
					else
						data.StructureChanges.Add(handle);
				}

				// Activity is inherited: bodies of the subtree enter or leave the simulation; colliders merged into a body above
				// the changed entity may join or leave that body's shape.
				for (entt::entity handle : data.ActivityChanges.TakeInto(data.ChangeScratch))
				{
					const Entity changed(handle, &scene);
					if (!changed.IsValid())
					{
						data.StructureChanges.Add(handle);
						continue;
					}
					// A leaf (the top-level entities of a cell that unloads, most often): nothing to walk, and no owner it could
					// reach twice.
					if (IsLeaf(changed))
					{
						if (BodyRecord* record = FindRecord(data, handle))
						{
							RefreshActivity(data, handle, *record);
						}
						else if (const auto merged = data.MergedOwners.find(handle); merged != data.MergedOwners.end())
						{
							RefreshOwnerIfShapeChanged(data, merged->second, changed);
						}
						continue;
					}

					std::unordered_set<entt::entity>& visitedOwners = data.VisitedOwners;
					visitedOwners.clear();
					VisitSubtree(scene, changed, data.VisitStack, [&](Entity entity)
					{
						if (BodyRecord* record = FindRecord(data, entity.GetHandle()))
						{
							visitedOwners.insert(entity.GetHandle());
							RefreshActivity(data, entity.GetHandle(), *record);
						}
						else
						{
							auto merged = data.MergedOwners.find(entity.GetHandle());
							if (merged != data.MergedOwners.end() && visitedOwners.find(merged->second) == visitedOwners.end())
								RefreshOwnerIfShapeChanged(data, merged->second, entity);
						}
						return true;
					});
				}

				// A transform change moves the bodies of the subtree; colliders merged into a body above the changed entity
				// may have moved relative to that body, which changes its shape.
				for (entt::entity handle : data.TransformChanges.TakeInto(data.ChangeScratch))
				{
					std::unordered_set<entt::entity>& visitedOwners = data.VisitedOwners;
					visitedOwners.clear();
					VisitSubtree(scene, Entity(handle, &scene), data.VisitStack, [&](Entity entity)
					{
						if (BodyRecord* record = FindRecord(data, entity.GetHandle()))
						{
							visitedOwners.insert(entity.GetHandle());
							record->TransformDirty = true;
							UpdatePolling(data, entity.GetHandle(), *record);
						}
						else
						{
							auto merged = data.MergedOwners.find(entity.GetHandle());
							if (merged != data.MergedOwners.end() && visitedOwners.find(merged->second) == visitedOwners.end())
								RefreshOwnerIfShapeChanged(data, merged->second, entity);
						}
						return true;
					});
				}

				// The bodies an entity was part of, and the one it is part of now.
				for (entt::entity handle : data.StructureChanges.TakeInto(data.ChangeScratch))
				{
					if (FindRecord(data, handle))
						data.OwnerRefreshes.Add(handle);
					auto merged = data.MergedOwners.find(handle);
					if (merged != data.MergedOwners.end())
					{
						const entt::entity previousOwner = merged->second;
						if (FindBodyOwner(Entity(handle, &scene)).GetHandle() != previousOwner)
							data.MergedOwners.erase(merged);
						data.OwnerRefreshes.Add(previousOwner);
					}
					if (const Entity owner = FindBodyOwner(Entity(handle, &scene)))
						data.OwnerRefreshes.Add(owner.GetHandle());
				}

				const std::vector<entt::entity>& owners = data.OwnerRefreshes.TakeInto(data.ChangeScratch);
				if (owners.size() >= c_ParallelBuildThreshold)
				{
					RefreshOwnersInParallel(data, owners);
				}
				else
				{
					for (entt::entity handle : owners)
						RefreshOwner(data, handle);
				}
			}
			return !HasChanges(data);
		}

		void ApplyChanges(PhysicsWorldData& data)
		{
			ST_PROFILE_FUNCTION();

			Scene& scene = *data.OwnerScene;
			// Timed only when there is something to do: physics calls apply pending changes before every query.
			const bool timed = HasChanges(data);
			const std::chrono::steady_clock::time_point start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
			data.RoundCreatedStatic = false;
			data.RoundCreatedMoving = false;
			data.GrewThisRound = false;
			bool settled = ApplyChangeRounds(data);
			// Room freed by this call goes to the records waiting for it, which get their bodies in the same call.
			if (settled && GiveSlotsToWaiters(data))
				settled = ApplyChangeRounds(data);
			if (!settled)
			{
				ST_CORE_ERROR("Physics: scene '{}' did not settle after {} rounds of changes; remaining changes are dropped", scene.GetName(), c_MaxChangeRounds);
				data.StructureChanges.Clear();
				data.SubtreeChanges.Clear();
				data.ActivityChanges.Clear();
				data.TransformChanges.Clear();
				data.OwnerRefreshes.Clear();
			}
			// One per motion kind that got new bodies, as the batches of physics engines that add bodies in batches count.
			data.AddBatchCount += (data.RoundCreatedStatic ? 1u : 0u) + (data.RoundCreatedMoving ? 1u : 0u);
			ReportBodyLimit(data);
			ApplyCharacterChanges(data);
			// One growth per round of changes that outgrew the room (or was asked for more, PhysicsWorld::Reserve), however
			// many bodies it created.
			GrowRoom(data, std::max<uint64_t>(GetUsedBodySlots(data), data.ReservedBodies));
			if (timed)
			{
				const float elapsed = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
				GetUpdateCounters(data).ChangeTime += elapsed;
				if (data.GrewThisRound)
				{
					data.LastGrowthMs = elapsed;
					ST_CORE_INFO("Physics: the world of scene '{}' grew from {} to {} bodies; cook the world (world.cook) or raise its physics capacity to start "
						"at that size", scene.GetName(), data.LastGrowthFrom, data.LastGrowthTo);
				}
			}
		}

		//////////////////////////////////////////////////////////////////////////
		// Registry signals
		//////////////////////////////////////////////////////////////////////////

		// Changes are only recorded here and applied later: on_destroy fires before the component is removed, and new
		// components are usually filled in right after they are added.
		void OnStructureChanged(PhysicsWorldData& data, entt::registry&, entt::entity handle)
		{
			data.StructureChanges.Add(handle);
		}

		void OnMeshRendererChanged(PhysicsWorldData& data, entt::registry& registry, entt::entity handle)
		{
			// Mesh colliders without a mesh of their own use the renderer's mesh.
			if (registry.all_of<MeshColliderComponent>(handle))
				data.StructureChanges.Add(handle);
		}

		void OnEntityDestroyed(PhysicsWorldData& data, entt::registry& registry, entt::entity handle)
		{
			data.StructureChanges.Add(handle);
			// The IDComponent is still attached while its on_destroy listeners run.
			if (const IDComponent* id = registry.try_get<IDComponent>(handle))
				ClearAllWarnings(data, id->ID);
		}

		void OnHierarchyChanged(PhysicsWorldData& data, entt::registry&, entt::entity handle)
		{
			data.SubtreeChanges.Add(handle);
		}

		void OnActivityChanged(PhysicsWorldData& data, entt::registry&, entt::entity handle)
		{
			data.ActivityChanges.Add(handle);
		}

		void OnTransformChanged(PhysicsWorldData& data, entt::registry&, entt::entity handle)
		{
			data.TransformChanges.Add(handle);
		}

		template<typename Component, auto Candidate>
		void ConnectChangeSignals(PhysicsWorldData& data, entt::registry& registry)
		{
			data.Connections.emplace_back(registry.on_construct<Component>().template connect<Candidate>(data));
			data.Connections.emplace_back(registry.on_update<Component>().template connect<Candidate>(data));
			data.Connections.emplace_back(registry.on_destroy<Component>().template connect<Candidate>(data));
		}

		void ConnectSignals(PhysicsWorldData& data)
		{
			entt::registry& registry = data.OwnerScene->GetRegistry();
			ConnectChangeSignals<RigidBodyComponent, &OnStructureChanged>(data, registry);
			ConnectChangeSignals<BoxColliderComponent, &OnStructureChanged>(data, registry);
			ConnectChangeSignals<SphereColliderComponent, &OnStructureChanged>(data, registry);
			ConnectChangeSignals<CapsuleColliderComponent, &OnStructureChanged>(data, registry);
			ConnectChangeSignals<MeshColliderComponent, &OnStructureChanged>(data, registry);
			ConnectChangeSignals<MeshRendererComponent, &OnMeshRendererChanged>(data, registry);
			// Reparenting (Scene::SetParent) patches the moved entity's relationship; entity creation and destruction are
			// reported by the other signals.
			data.Connections.emplace_back(registry.on_update<RelationshipComponent>().connect<&OnHierarchyChanged>(data));
			data.Connections.emplace_back(registry.on_update<TransformComponent>().connect<&OnTransformChanged>(data));
			data.Connections.emplace_back(registry.on_construct<InactiveComponent>().connect<&OnActivityChanged>(data));
			data.Connections.emplace_back(registry.on_destroy<InactiveComponent>().connect<&OnActivityChanged>(data));
			// Every entity has an IDComponent until it is destroyed, so its removal reports entity destruction.
			data.Connections.emplace_back(registry.on_destroy<IDComponent>().connect<&OnEntityDestroyed>(data));
		}

		// The state of the mesh provider and of cooking that mesh sources are checked against.
		void RememberMeshState(PhysicsWorldData& data)
		{
			data.SeenMeshProviderGeneration = GetMeshProviderStorage().Generation;
			data.SeenMeshProviderVersion = GetActiveMeshProvider().GetVersion();
			data.SeenCompletedCooks = PhysicsMeshShapes::GetCompletedCount();
		}

		// Rebuilds the bodies whose mesh data changed (hot reload), arrived, or finished cooking. Only the bodies that use a
		// mesh the provider reports as changed (all of them if it cannot tell) and, when a cook finished, the bodies waiting for
		// one are checked, so steps without such changes cost nothing and unrelated asset changes (texture streaming) do not
		// visit mesh colliders. The state is read before the meshes, so a change that happens meanwhile is noticed at the next
		// step.
		void RefreshMeshBodies(PhysicsWorldData& data)
		{
			const uint64_t generation = GetMeshProviderStorage().Generation;
			PhysicsMeshProvider& provider = GetActiveMeshProvider();
			const uint64_t version = provider.GetVersion();
			const uint64_t completedCooks = PhysicsMeshShapes::GetCompletedCount();
			const bool providerChanged = generation != data.SeenMeshProviderGeneration || version != data.SeenMeshProviderVersion;
			const bool cooksFinished = completedCooks != data.SeenCompletedCooks;
			if (!providerChanged && !cooksFinished)
				return;

			std::vector<entt::entity>& checks = data.MeshChecks;
			checks.clear();
			if (providerChanged)
			{
				std::vector<AssetHandle>& changedMeshes = data.ChangedMeshes;
				changedMeshes.clear();
				if (generation == data.SeenMeshProviderGeneration && provider.GetChangedMeshes(data.SeenMeshProviderVersion, changedMeshes))
				{
					for (AssetHandle mesh : changedMeshes)
					{
						auto users = data.MeshUsers.find(mesh);
						if (users != data.MeshUsers.end())
							checks.insert(checks.end(), users->second.begin(), users->second.end());
					}
				}
				else
				{
					checks.assign(data.MeshBodies.begin(), data.MeshBodies.end());
				}
			}
			if (cooksFinished)
				checks.insert(checks.end(), data.WaitingMeshBodies.begin(), data.WaitingMeshBodies.end());
			data.SeenMeshProviderGeneration = generation;
			data.SeenMeshProviderVersion = version;
			data.SeenCompletedCooks = completedCooks;

			// In a fixed order: rebuilds happen in the order they are recorded.
			std::sort(checks.begin(), checks.end());
			checks.erase(std::unique(checks.begin(), checks.end()), checks.end());
			for (entt::entity handle : checks)
			{
				const BodyRecord* record = FindRecord(data, handle);
				if (!record)
					continue;

				data.MeshCheckCount++;
				const bool changed = std::any_of(record->MeshSources.begin(), record->MeshSources.end(), [&](const MeshSource& source)
				{
					// A collider waiting for its mesh wants it loaded; the meshes of built colliders are only looked at, so that
					// a mesh unloaded on purpose is not loaded again. Data that went away leaves the body as it is.
					const Ref<const PhysicsMeshData> current = source.Waiting ? provider.GetMeshData(source.Mesh) : provider.PeekMeshData(source.Mesh);
					if (!current)
						return false;
					if (current != source.Data.lock())
						return true;
					return source.Waiting && PhysicsMeshShapes::Request(current, source.Convex).CookState != PhysicsMeshShapes::State::Cooking;
				});
				if (changed)
					data.OwnerRefreshes.Add(handle);
			}
		}

		// Whether the start of the simulation waits for a body: its entity takes part in the simulation and one of its mesh
		// colliders waits for a shape being cooked or for mesh data the provider is loading. Meshes that are unknown or failed
		// to load are not waited for (they may never arrive).
		bool HoldsStart(PhysicsWorldData& data, PhysicsMeshProvider& provider, entt::entity handle)
		{
			const BodyRecord* record = FindRecord(data, handle);
			const Entity entity(handle, data.OwnerScene);
			if (!record || !entity.IsValid() || !IsSimulated(*data.OwnerScene, entity))
				return false;

			return std::any_of(record->MeshSources.begin(), record->MeshSources.end(), [&](const MeshSource& source)
			{
				// With data, the source waits for its shape to be cooked, which always ends.
				return source.Waiting && (!source.Data.expired() || provider.IsMeshLoading(source.Mesh));
			});
		}

		// Before its first step, the simulation waits for the mesh colliders that exist by then and wait for their mesh data
		// (loading) or shape (cooking), so that bodies do not fall through mesh floors that are not there yet: the step is held
		// (nothing moves, no collision events) while one of them waits, for at most PhysicsSettings::MeshWaitTimeout of
		// simulation time, then the simulation starts anyway and warns. Mesh colliders added later are not waited for. Returns
		// whether to hold this step.
		bool HoldForMeshes(PhysicsWorldData& data, float timestep)
		{
			if (data.HasStarted)
				return false;

			// Mesh data that arrived or finished cooking builds the waiting bodies now.
			RefreshMeshBodies(data);
			ApplyChanges(data);

			PhysicsMeshProvider& provider = GetActiveMeshProvider();
			std::vector<entt::entity>& waiting = data.MeshChecks;
			waiting.clear();
			for (entt::entity handle : data.WaitingMeshBodies)
			{
				if (HoldsStart(data, provider, handle))
					waiting.push_back(handle);
			}
			if (!waiting.empty() && data.HeldTime < data.Settings.MeshWaitTimeout)
			{
				data.HeldTime += timestep;
				return true;
			}

			// Without a wait (MeshWaitTimeout 0), starting without the meshes is what was asked for.
			if (!waiting.empty() && data.Settings.MeshWaitTimeout > 0.0f)
			{
				constexpr size_t maxNames = 5;
				std::string names;
				for (size_t i = 0; i < std::min(waiting.size(), maxNames); i++)
				{
					if (i > 0)
						names += ", ";
					names += fmt::format("'{}'", Entity(waiting[i], data.OwnerScene).GetName());
				}
				if (waiting.size() > maxNames)
					names += fmt::format(" and {} more", waiting.size() - maxNames);
				ST_CORE_WARN("Physics: scene '{}' starts simulating after waiting {} s for meshes, without the mesh colliders of {}",
					data.OwnerScene->GetName(), data.Settings.MeshWaitTimeout, names);
			}
			data.HasStarted = true;
			return false;
		}

		// Bodies of entities about to be destroyed (Scene::DestroyEntity during an update) leave the simulation before the
		// next step; static bodies stay until the entity is destroyed (queries skip them already). Colliders about to be
		// destroyed leave the shape of the body they belong to.
		void RemovePendingDestroys(PhysicsWorldData& data)
		{
			Scene& scene = *data.OwnerScene;
			for (UUID entityID : scene.GetPendingDestroys())
			{
				const Entity root = scene.GetEntityByUUID(entityID);
				if (!root)
					continue;

				VisitSubtree(scene, root, data.VisitStack, [&](Entity entity)
				{
					if (BodyRecord* record = FindRecord(data, entity.GetHandle()))
					{
						if (record->Type != RigidBodyType::Static)
							RemoveFromSimulation(data, *record);
						return true;
					}

					auto merged = data.MergedOwners.find(entity.GetHandle());
					if (merged == data.MergedOwners.end())
						return true;
					const BodyRecord* owner = FindRecord(data, merged->second);
					const bool inShape = owner && FindShapeMember(*owner, entity.GetHandle()) != nullptr;
					if (inShape && !IsPendingDestroyInHierarchy(scene, Entity(merged->second, &scene)))
						data.OwnerRefreshes.Add(merged->second);
					return true;
				});
			}
		}

		// Marks the records below an entity that moved without a signal: their entities moved too, but their bodies may sleep
		// (or be static), so nothing else would notice. They are synchronized in the same step (see SyncEntitiesToBodies).
		void MarkMovedDescendants(PhysicsWorldData& data, Entity moved)
		{
			VisitSubtree(*data.OwnerScene, moved, data.VisitStack, [&](Entity entity)
			{
				if (entity == moved)
					return true;
				BodyRecord* record = FindRecord(data, entity.GetHandle());
				if (record && record->SyncPass != data.SyncPass)
				{
					record->TransformDirty = true;
					data.SyncFollowers.push_back(entity.GetHandle());
				}
				return true;
			});
		}

		//////////////////////////////////////////////////////////////////////////
		// Stepping
		//////////////////////////////////////////////////////////////////////////

		// Brings one body in line with its entity before a step: kinematic target, teleport, degenerate transform, scale
		// change, or a retry of a body that could not be built. Each record is synchronized at most once per step.
		void SyncBody(PhysicsWorldData& data, entt::entity handle, float timestep)
		{
			Scene& scene = *data.OwnerScene;
			BodyRecord* record = FindRecord(data, handle);
			if (!record || record->SyncPass == data.SyncPass)
				return;
			record->SyncPass = data.SyncPass;
			data.LastSyncedBodies++;

			const Entity entity(handle, &scene);
			if (!entity.IsValid())
			{
				data.StructureChanges.Add(handle);
				return;
			}

			if (!record->HasBody())
			{
				// A build whose cause of failure may have gone away is retried.
				bool retry = false;
				if (IsSimulated(scene, entity) && record->Failure == BuildFailure::DegenerateTransform)
					retry = scene.GetWorldTransform(entity) != record->LastWorldTransform || (record->WaitsForParent && HasInvertibleParent(scene, entity));
				if (retry)
					data.OwnerRefreshes.Add(handle);
				record->TransformDirty = false;
				return;
			}

			if (!record->InSimulation)
			{
				// A suspended body comes back once its transform is valid. Bodies of inactive entities or entities about to
				// be destroyed wait: activity changes bring them back, and their pose is taken from the entity then.
				if (record->Suspended && IsSimulated(scene, entity))
				{
					const PlacementResult result = AddToSimulation(data, *record, entity);
					record->Suspended = result == PlacementResult::DegenerateTransform;
					if (result == PlacementResult::ScaleChanged)
						data.OwnerRefreshes.Add(handle);
				}
				record->TransformDirty = false;
				UpdatePolling(data, handle, *record);
				return;
			}

			const glm::mat4 worldTransform = scene.GetWorldTransform(entity);
			if (worldTransform == record->LastWorldTransform)
			{
				// A kinematic body keeps the velocity of its last move; stop it at its target.
				if (record->KinematicMoving)
				{
					b3Body_SetLinearVelocity(record->BodyID, b3Vec3 { 0.0f, 0.0f, 0.0f });
					b3Body_SetAngularVelocity(record->BodyID, b3Vec3 { 0.0f, 0.0f, 0.0f });
					record->KinematicMoving = false;
				}
				record->TransformDirty = false;
				return;
			}

			// Moved without a signal (the entity or an ancestor of this awake body was edited directly): the bodies below it
			// moved as well. (A signaled change marked them already.)
			if (!record->TransformDirty)
				MarkMovedDescendants(data, entity);
			record->TransformDirty = false;

			glm::vec3 position;
			glm::quat rotation;
			glm::vec3 scale;
			const bool decomposed = Math::DecomposeTransform(worldTransform, position, rotation, scale);
			const bool inRange = decomposed && IsWithinCoordinateRange(position);
			if (!inRange || (record->Type == RigidBodyType::Dynamic && !HasInvertibleParent(scene, entity)))
			{
				// Out of the simulation (like an inactive entity) until the transform is valid again; the transform is
				// left as it is.
				if (ShouldWarn(data, record->EntityID, PhysicsWarning::DegenerateTransform))
				{
					if (!decomposed)
						ST_CORE_WARN("Physics: '{}' has a degenerate world transform; its body leaves the simulation until the transform is valid", entity.GetName());
					else if (!inRange)
						ST_CORE_WARN("Physics: '{}' moved more than {} m from the origin, beyond what the simulation supports; its body leaves the simulation until it is back within", entity.GetName(), c_MaxBodyCoordinate);
					else
						ST_CORE_WARN("Physics: the parent of '{}' is scaled to (nearly) zero, so its simulated pose cannot be written back; its body leaves the simulation until the parent's transform is valid", entity.GetName());
				}
				RemoveFromSimulation(data, *record);
				record->Suspended = true;
				UpdatePolling(data, handle, *record);
				return;
			}
			if (HasScaleChanged(scale, record->ShapeScale))
			{
				data.OwnerRefreshes.Add(handle);
				return;
			}
			record->WorldScale = scale;

			switch (record->Type)
			{
				case RigidBodyType::Static:
					// Bodies resting on it at the old and the new place are woken up explicitly: static bodies have no island.
					WakeBodiesIn(data, b3Body_ComputeAABB(record->BodyID));
					b3Body_SetTransform(record->BodyID, ToB3(position), ToB3(rotation));
					WakeBodiesIn(data, b3Body_ComputeAABB(record->BodyID));
					break;
				case RigidBodyType::Kinematic:
					// Moved with a velocity that reaches the target in the step: the bodies it touches, also those resting on it,
					// wake up through their contacts.
					b3Body_SetTargetTransform(record->BodyID, b3WorldTransform { ToB3(position), ToB3(rotation) }, timestep, true);
					MarkAwake(data, record->BodyID);
					record->KinematicMoving = true;
					record->KinematicTargetPosition = position;
					record->KinematicTargetRotation = rotation;
					break;
				case RigidBodyType::Dynamic:
					// Moved from outside physics (gameplay code, editor gizmo, a moving parent): teleport, keeping velocity,
					// and wake the bodies that rested on it.
					WakeTouchingBodies(data, record->BodyID);
					b3Body_SetTransform(record->BodyID, ToB3(position), ToB3(rotation));
					b3Body_SetAwake(record->BodyID, true);
					MarkAwake(data, record->BodyID);
					break;
			}
			record->LastWorldTransform = worldTransform;
		}

		// Brings the bodies in line with their entities before a step. Only awake bodies (whose entities may have been moved
		// without a signal), the records that need attention (see NeedsPolling) and the bodies below entities that moved
		// without a signal are visited, so sleeping and static bodies cost nothing otherwise.
		//
		// Most awake bodies are where their entities are (physics put them there in the last step): those are found in parallel
		// chunks, reading only the scene's current cached world transforms (Scene::FindCurrentWorldTransform) and their own
		// records, and need nothing more. The others are synchronized one by one, in entity order.
		void SyncEntitiesToBodies(PhysicsWorldData& data, float timestep)
		{
			ST_PROFILE_FUNCTION();

			RemovePendingDestroys(data);
			RefreshMeshBodies(data);
			data.SyncPass++;
			data.LastSyncedBodies = 0;
			data.SyncFollowers.clear();

			std::vector<std::pair<entt::entity, BodyRecord*>>& awake = data.AwakeRecords;
			awake.clear();
			for (const int32_t index : data.AwakeBodies)
			{
				const BodySlot& slot = data.BodySlots[static_cast<size_t>(index)];
				if (slot.Used && slot.Record && slot.Record->InSimulation && slot.Record->Type != RigidBodyType::Static)
					awake.emplace_back(slot.Entity, slot.Record);
			}

			const Scene& scene = *data.OwnerScene;
			const uint64_t pass = data.SyncPass;
			std::atomic<uint32_t> unchanged = 0;
			const auto skipUnchanged = [&](uint32_t begin, uint32_t end)
			{
				uint32_t skipped = 0;
				for (uint32_t index = begin; index < end; index++)
				{
					auto& [handle, record] = awake[index];
					if (record->TransformDirty || record->KinematicMoving || !record->InSimulation)
						continue;
					const glm::mat4* world = scene.FindCurrentWorldTransform(handle);
					if (!world || *world != record->LastWorldTransform)
						continue;
					record->SyncPass = pass;
					record = nullptr;
					skipped++;
				}
				unchanged.fetch_add(skipped, std::memory_order_relaxed);
			};
			constexpr uint32_t c_ParallelSyncThreshold = 1024;
			const uint32_t awakeCount = static_cast<uint32_t>(awake.size());
			if (awakeCount >= c_ParallelSyncThreshold && JobSystem::IsInitialized())
				JobSystem::ParallelFor(awakeCount, std::max<uint32_t>(128, awakeCount / ((JobSystem::GetWorkerThreadCount() + 1) * 4)), skipUnchanged);
			else
				skipUnchanged(0, awakeCount);
			data.LastSyncedBodies = unchanged.load(std::memory_order_relaxed);

			std::vector<entt::entity>& candidates = data.SyncCandidates;
			candidates.clear();
			for (const auto& [handle, record] : awake)
			{
				if (record)
					candidates.push_back(handle);
			}

			// The polled records that still need attention; the others, and stale or duplicate entries, are dropped.
			size_t kept = 0;
			for (size_t index = 0; index < data.PolledBodies.size(); index++)
			{
				const entt::entity handle = data.PolledBodies[index];
				BodyRecord* record = FindRecord(data, handle);
				if (!record || !record->Polled)
					continue;
				record->Polled = false; // Set again below for the records that stay listed; skips duplicates meanwhile
				if (!NeedsPolling(*record))
					continue;
				data.PolledBodies[kept++] = handle;
				candidates.push_back(handle);
			}
			data.PolledBodies.resize(kept);
			for (entt::entity handle : data.PolledBodies)
				FindRecord(data, handle)->Polled = true;

			// Processed in a fixed order, whatever the order the bodies woke in; then the bodies below entities that moved
			// without a signal, in the order they were found.
			std::sort(candidates.begin(), candidates.end());
			candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
			for (entt::entity handle : candidates)
				SyncBody(data, handle, timestep);
			for (size_t index = 0; index < data.SyncFollowers.size(); index++)
				SyncBody(data, data.SyncFollowers[index], timestep);

			ApplyChanges(data);
		}

		//////////////////////////////////////////////////////////////////////////
		// Contacts and triggers
		//////////////////////////////////////////////////////////////////////////

		// Whether the contacts of a pair of entities become events (see ICollisionEventSource).
		bool IsWanted(const PhysicsWorldData& data, UUID a, UUID b)
		{
			return data.TrackAll || data.ContactInterest.contains(a) || data.ContactInterest.contains(b);
		}

		// The point and normal of a contact: midway between the two surfaces, averaged over its first manifold's points, and
		// the normal from shape A towards shape B. False if box3d has no touching manifold for it (any more).
		bool ReadContactPoint(b3ContactId contact, glm::vec3& outPoint, glm::vec3& outNormal, b3ShapeId* outShapeA = nullptr)
		{
			if (!b3Contact_IsValid(contact))
				return false;
			const b3ContactData contactData = b3Contact_GetData(contact);
			if (contactData.manifoldCount <= 0 || !contactData.manifolds || contactData.manifolds[0].pointCount <= 0)
				return false;
			const b3Manifold& manifold = contactData.manifolds[0];
			const glm::vec3 centerA = ToGlm(b3Body_GetWorldCenter(b3Shape_GetBody(contactData.shapeIdA)));
			const glm::vec3 centerB = ToGlm(b3Body_GetWorldCenter(b3Shape_GetBody(contactData.shapeIdB)));
			glm::vec3 sum(0.0f);
			for (int index = 0; index < manifold.pointCount; index++)
				sum += centerA + ToGlm(manifold.points[index].anchorA) + centerB + ToGlm(manifold.points[index].anchorB);
			outPoint = sum / (2.0f * static_cast<float>(manifold.pointCount));
			outNormal = ToGlm(manifold.normal);
			if (outShapeA)
				*outShapeA = contactData.shapeIdA;
			return IsFinite(outPoint) && IsFinite(outNormal);
		}

		// Where a trigger and a visitor meet, as far as their shapes' bounds tell (box3d's sensor events carry no point): the
		// center of the intersection of the bounds, and the direction from the trigger's center to the visitor's.
		void ComputeTriggerPoint(b3ShapeId sensor, b3ShapeId visitor, glm::vec3& outPoint, glm::vec3& outNormal)
		{
			const b3AABB a = b3Shape_GetAABB(sensor);
			const b3AABB b = b3Shape_GetAABB(visitor);
			const glm::vec3 lower = glm::max(ToGlm(a.lowerBound), ToGlm(b.lowerBound));
			const glm::vec3 upper = glm::min(ToGlm(a.upperBound), ToGlm(b.upperBound));
			outPoint = 0.5f * (lower + upper);
			const glm::vec3 direction = 0.5f * (ToGlm(b.lowerBound) + ToGlm(b.upperBound)) - 0.5f * (ToGlm(a.lowerBound) + ToGlm(a.upperBound));
			const float length = glm::length(direction);
			outNormal = length > 1.0e-6f && std::isfinite(length) ? direction / length : glm::vec3(0.0f, 1.0f, 0.0f);
		}

		// A pair begins: tracked from now on, its Begin queued in the order the events happened.
		TouchingPair& BeginPair(PhysicsWorldData& data, const PairKey& key, UUID a, UUID b, bool trigger, const glm::vec3& point, const glm::vec3& normal, b3ContactId contact)
		{
			TouchingPair pair;
			pair.A = a;
			pair.B = b;
			pair.IsTrigger = trigger;
			pair.SortKey = data.NextPairOrder++;
			pair.Contact = contact;
			pair.Point = point;
			pair.Normal = normal;
			AddTouchingPair(data, key, pair);
			TouchingPair& added = data.TouchingPairs.find(key)->second;
			QueueEvent(data, CollisionEventType::Begin, added);
			return added;
		}

		void EndPair(PhysicsWorldData& data, std::unordered_map<PairKey, TouchingPair, PairKeyHash>::iterator it)
		{
			TouchingPair pair = it->second;
			RemovePairFromIndex(data, pair.A, it->second);
			RemovePairFromIndex(data, pair.B, it->second);
			data.TouchingPairs.erase(it);
			QueueEvent(data, CollisionEventType::End, pair);
		}

		// One contact or visit of a pair began: the pair begins with it, or counts it (a pair that waited for a rebuilt body's
		// new shapes is confirmed by it).
		void CountBegin(PhysicsWorldData& data, UUID a, UUID b, bool trigger, const glm::vec3& point, const glm::vec3& normal, b3ContactId contact)
		{
			if (a == b || !IsWanted(data, a, b))
				return;
			const PairKey key = MakePairKey(a, b);
			auto it = data.TouchingPairs.find(key);
			TouchingPair& pair = it != data.TouchingPairs.end() ? it->second : BeginPair(data, key, a, b, trigger, point, normal, contact);
			pair.ContactCount++;
			pair.Unconfirmed = false;
			if (B3_IS_NULL(pair.Contact) || !b3Contact_IsValid(pair.Contact))
				pair.Contact = contact;
		}

		// One contact or visit of a pair ended: the pair ends with its last one, unless a character controller's move reported
		// it in this step or it waits for a rebuilt body's new shapes.
		void CountEnd(PhysicsWorldData& data, UUID a, UUID b)
		{
			if (a == b)
				return;
			auto it = data.TouchingPairs.find(MakePairKey(a, b));
			if (it == data.TouchingPairs.end())
				return;
			TouchingPair& pair = it->second;
			if (pair.ContactCount > 0)
				pair.ContactCount--;
			if (pair.ContactCount == 0 && !pair.Unconfirmed && pair.CharacterStep != data.StepCount)
				EndPair(data, it);
		}

		// Marks a moved body for the write-back (see WriteBackDynamicBodies).
		void MarkMoved(PhysicsWorldData& data, b3BodyId body)
		{
			const int64_t index = static_cast<int64_t>(body.index1) - 1;
			if (index < 0 || static_cast<size_t>(index) >= data.BodySlots.size())
				return;
			uint64_t& word = data.MovedBodyBits[static_cast<size_t>(index) / 64];
			if (word == 0)
				data.MovedBodyWords.push_back(static_cast<uint32_t>(index / 64));
			word |= uint64_t(1) << (index % 64);
		}

		// Reads the events of one box3d step (box3d clears them at its next step): the bodies that moved, and contacts and
		// sensor visits that began and ended, folded into entity pairs (each step's End events before its Begin events). Shapes
		// resolve through the world's own slots: End events may name shapes that are gone, whose pairs were ended (a removal)
		// or wait for confirmation (a rebuild) already. With lastStep, the bodies still awake are listed for the next step.
		void ProcessStepEvents(PhysicsWorldData& data, bool lastStep)
		{
			const b3BodyEvents bodyEvents = b3World_GetBodyEvents(data.World);
			if (lastStep)
			{
				data.AwakeStamp++;
				data.AwakeBodies.clear();
			}
			for (int index = 0; index < bodyEvents.moveCount; index++)
			{
				const b3BodyMoveEvent& move = bodyEvents.moveEvents[index];
				MarkMoved(data, move.bodyId);
				if (lastStep && !move.fellAsleep)
					MarkAwake(data, move.bodyId);
			}

			const b3ContactEvents contactEvents = b3World_GetContactEvents(data.World);
			const b3SensorEvents sensorEvents = b3World_GetSensorEvents(data.World);
			for (int index = 0; index < contactEvents.endCount; index++)
			{
				const b3ContactEndTouchEvent& event = contactEvents.endEvents[index];
				const ShapeSlot* a = ResolveShape(data, event.shapeIdA);
				const ShapeSlot* b = ResolveShape(data, event.shapeIdB);
				if (a && b)
					CountEnd(data, a->Entity, b->Entity);
			}
			for (int index = 0; index < sensorEvents.endCount; index++)
			{
				const b3SensorEndTouchEvent& event = sensorEvents.endEvents[index];
				const ShapeSlot* sensor = ResolveShape(data, event.sensorShapeId);
				const ShapeSlot* visitor = ResolveShape(data, event.visitorShapeId);
				if (sensor && visitor && !(sensor->BodyType == RigidBodyType::Static && visitor->BodyType == RigidBodyType::Static))
					CountEnd(data, sensor->Entity, visitor->Entity);
			}
			for (int index = 0; index < contactEvents.beginCount; index++)
			{
				const b3ContactBeginTouchEvent& event = contactEvents.beginEvents[index];
				const ShapeSlot* a = ResolveShape(data, event.shapeIdA);
				const ShapeSlot* b = ResolveShape(data, event.shapeIdB);
				if (!a || !b || a->Entity == b->Entity || !IsWanted(data, a->Entity, b->Entity))
					continue;
				glm::vec3 point(0.0f);
				glm::vec3 normal(0.0f, 1.0f, 0.0f);
				ReadContactPoint(event.contactId, point, normal);
				CountBegin(data, a->Entity, b->Entity, false, point, normal, event.contactId);
			}
			for (int index = 0; index < sensorEvents.beginCount; index++)
			{
				const b3SensorBeginTouchEvent& event = sensorEvents.beginEvents[index];
				const ShapeSlot* sensor = ResolveShape(data, event.sensorShapeId);
				const ShapeSlot* visitor = ResolveShape(data, event.visitorShapeId);
				// Neither body moves (a static trigger and a static collider or static trigger): no pair, as in the physics
				// engines whose triggers only detect what can move into them.
				if (!sensor || !visitor || (sensor->BodyType == RigidBodyType::Static && visitor->BodyType == RigidBodyType::Static))
					continue;
				if (sensor->Entity == visitor->Entity || !IsWanted(data, sensor->Entity, visitor->Entity))
					continue;
				glm::vec3 point;
				glm::vec3 normal;
				ComputeTriggerPoint(event.sensorShapeId, event.visitorShapeId, point, normal);
				CountBegin(data, sensor->Entity, visitor->Entity, true, point, normal, b3ContactId {});
			}
		}

		// The touching pairs that the step may end (those of awake bodies), counted for PhysicsStats::CheckedPairCount; their
		// points are refreshed after the step (see RefreshCheckedPairs). Pairs of sleeping bodies are not visited at all.
		void CollectCheckedPairs(PhysicsWorldData& data)
		{
			ST_PROFILE_FUNCTION();

			data.CheckedPairs.clear();
			const uint64_t step = data.StepCount;
			for (const int32_t index : data.AwakeBodies)
			{
				const BodySlot& slot = data.BodySlots[static_cast<size_t>(index)];
				if (!slot.Used || !slot.Record)
					continue;
				auto indexIt = data.EntityPairs.find(slot.Record->EntityID);
				if (indexIt == data.EntityPairs.end())
					continue;
				for (const PairKey& key : indexIt->second)
				{
					auto pairIt = data.TouchingPairs.find(key);
					if (pairIt == data.TouchingPairs.end() || pairIt->second.CheckStep == step)
						continue;
					pairIt->second.CheckStep = step;
					data.CheckedPairs.push_back(key);
				}
			}
			data.LastCheckedPairs = static_cast<uint32_t>(data.CheckedPairs.size());
		}

		// The points of the checked pairs that still touch, as of the end of the step (their End carries the last one).
		void RefreshCheckedPairs(PhysicsWorldData& data)
		{
			for (const PairKey& key : data.CheckedPairs)
			{
				auto it = data.TouchingPairs.find(key);
				if (it == data.TouchingPairs.end() || it->second.IsTrigger)
					continue;
				TouchingPair& pair = it->second;
				glm::vec3 point;
				glm::vec3 normal;
				b3ShapeId shapeA = b3_nullShapeId;
				if (!ReadContactPoint(pair.Contact, point, normal, &shapeA))
					continue;
				const ShapeSlot* a = ResolveShape(data, shapeA);
				pair.Point = point;
				pair.Normal = a && a->Entity == pair.A ? normal : -normal;
			}
		}

		// Pairs of bodies rebuilt since the last step that the step did not confirm (no contact of the new shapes, no report of
		// a character controller) end now.
		void ResolveRecheckedPairs(PhysicsWorldData& data)
		{
			std::vector<UUID>& entities = data.RecheckedEntities;
			if (entities.empty())
				return;
			std::sort(entities.begin(), entities.end());
			entities.erase(std::unique(entities.begin(), entities.end()), entities.end());
			std::vector<TouchingPair>& ended = data.EndedPairs;
			ended.clear();
			for (const UUID entityID : entities)
			{
				auto indexIt = data.EntityPairs.find(entityID);
				if (indexIt == data.EntityPairs.end())
					continue;
				for (const PairKey& key : indexIt->second)
				{
					auto pairIt = data.TouchingPairs.find(key);
					if (pairIt == data.TouchingPairs.end() || !pairIt->second.Unconfirmed)
						continue;
					if (pairIt->second.ContactCount > 0 || pairIt->second.CharacterStep == data.StepCount)
						pairIt->second.Unconfirmed = false;
					else
						ended.push_back(pairIt->second);
				}
			}
			entities.clear();
			// Ended after the walk (ending changes the lists walked), in the order the pairs began.
			std::sort(ended.begin(), ended.end(), [](const TouchingPair& a, const TouchingPair& b) { return a.SortKey < b.SortKey; });
			for (const TouchingPair& pair : ended)
			{
				auto it = data.TouchingPairs.find(MakePairKey(pair.A, pair.B));
				if (it != data.TouchingPairs.end())
					EndPair(data, it);
			}
		}

		// The pairs the character controllers reported in the previous step but not in this one end, unless box3d's contacts
		// still hold them.
		void ResolveCharacterPairs(PhysicsWorldData& data, std::vector<PairKey>& reported)
		{
			for (const PairKey& key : data.CharacterPairs)
			{
				auto it = data.TouchingPairs.find(key);
				if (it == data.TouchingPairs.end())
					continue;
				const TouchingPair& pair = it->second;
				if (pair.CharacterStep != data.StepCount && pair.ContactCount == 0 && !pair.Unconfirmed)
					EndPair(data, it);
			}
			data.CharacterPairs.swap(reported);
			reported.clear();
		}

		// Pairs nobody wants any more (an entity of interest was let go, or tracking all contacts stopped) end.
		void EndUnwantedPairs(PhysicsWorldData& data)
		{
			if (!data.CheckAllWanted && data.UnwantedEntities.empty())
				return;
			std::vector<TouchingPair>& ended = data.EndedPairs;
			ended.clear();
			const auto collect = [&](const TouchingPair& pair)
			{
				if (!IsWanted(data, pair.A, pair.B))
					ended.push_back(pair);
			};
			if (data.CheckAllWanted)
			{
				for (const auto& [key, pair] : data.TouchingPairs)
					collect(pair);
			}
			else
			{
				for (const UUID entityID : data.UnwantedEntities)
				{
					auto indexIt = data.EntityPairs.find(entityID);
					if (indexIt == data.EntityPairs.end())
						continue;
					for (const PairKey& key : indexIt->second)
					{
						auto pairIt = data.TouchingPairs.find(key);
						if (pairIt != data.TouchingPairs.end())
							collect(pairIt->second);
					}
				}
			}
			data.CheckAllWanted = false;
			data.UnwantedEntities.clear();
			std::sort(ended.begin(), ended.end(), [](const TouchingPair& a, const TouchingPair& b) { return a.SortKey < b.SortKey; });
			ended.erase(std::unique(ended.begin(), ended.end(), [](const TouchingPair& a, const TouchingPair& b) { return a.SortKey == b.SortKey; }), ended.end());
			for (const TouchingPair& pair : ended)
			{
				auto it = data.TouchingPairs.find(MakePairKey(pair.A, pair.B));
				if (it != data.TouchingPairs.end())
					EndPair(data, it);
			}
		}

		// A pair found touching by seeding: its contacts and visits counted now.
		struct SeedCount
		{
			UUID A = UUID::Null();
			UUID B = UUID::Null();
			bool Trigger = false;
			uint32_t Count = 0;
			b3ContactId Contact = {};
			glm::vec3 Point = glm::vec3(0.0f);
			glm::vec3 Normal = glm::vec3(0.0f);
		};

		// Counts the touching contacts of a body's shapes per pair of entities (each contact once, by its ID).
		void CountBodyContacts(PhysicsWorldData& data, b3BodyId body, std::map<PairKey, SeedCount>& counts, std::unordered_set<uint64_t>& seenContacts)
		{
			const int capacity = b3Body_GetContactCapacity(body);
			if (capacity <= 0)
				return;
			std::vector<b3ContactData>& contacts = data.ContactScratch;
			contacts.resize(static_cast<size_t>(capacity));
			const int count = b3Body_GetContactData(body, contacts.data(), capacity);
			for (int index = 0; index < count; index++)
			{
				const b3ContactData& contact = contacts[index];
				if (contact.manifoldCount <= 0)
					continue;
				const uint64_t contactKey = (static_cast<uint64_t>(contact.contactId.generation) << 32) | static_cast<uint32_t>(contact.contactId.index1);
				if (!seenContacts.insert(contactKey).second)
					continue;
				const ShapeSlot* a = ResolveShape(data, contact.shapeIdA);
				const ShapeSlot* b = ResolveShape(data, contact.shapeIdB);
				if (!a || !b || a->Entity == b->Entity || !IsWanted(data, a->Entity, b->Entity))
					continue;
				SeedCount& seed = counts[MakePairKey(a->Entity, b->Entity)];
				if (seed.Count == 0)
				{
					seed.A = a->Entity;
					seed.B = b->Entity;
					seed.Contact = contact.contactId;
					ReadContactPoint(contact.contactId, seed.Point, seed.Normal);
				}
				seed.Count++;
			}
		}

		// Counts the visitors of a trigger record's sensor shapes per pair of entities; with onlyEntity set, only its visits.
		void CountTriggerVisits(PhysicsWorldData& data, const BodyRecord& trigger, UUID onlyEntity, std::map<PairKey, SeedCount>& counts)
		{
			if (!trigger.HasBody() || !trigger.InSimulation)
				return;
			std::vector<b3ShapeId>& visitors = data.ShapeScratch;
			for (const b3ShapeId sensorShape : trigger.Shapes)
			{
				const ShapeSlot* sensor = ResolveShape(data, sensorShape);
				const int capacity = b3Shape_GetSensorCapacity(sensorShape);
				if (!sensor || capacity <= 0)
					continue;
				visitors.resize(static_cast<size_t>(capacity));
				const int count = b3Shape_GetSensorData(sensorShape, visitors.data(), capacity);
				for (int index = 0; index < count; index++)
				{
					const ShapeSlot* visitor = ResolveShape(data, visitors[index]);
					if (!visitor || visitor->Entity == sensor->Entity || (sensor->BodyType == RigidBodyType::Static && visitor->BodyType == RigidBodyType::Static))
						continue;
					if ((onlyEntity != UUID::Null() && visitor->Entity != onlyEntity && sensor->Entity != onlyEntity) || !IsWanted(data, sensor->Entity, visitor->Entity))
						continue;
					SeedCount& seed = counts[MakePairKey(sensor->Entity, visitor->Entity)];
					if (seed.Count == 0)
					{
						seed.A = sensor->Entity;
						seed.B = visitor->Entity;
						seed.Trigger = true;
						ComputeTriggerPoint(sensorShape, visitors[index], seed.Point, seed.Normal);
					}
					seed.Count++;
				}
			}
		}

		// Pairs that became wanted while touching (an entity of interest added, or tracking all contacts started) begin now,
		// counted from box3d's touching contacts and sensor overlaps; tracked pairs among them take the count.
		void SeedWantedPairs(PhysicsWorldData& data)
		{
			if (!data.SeedAll && data.SeededEntities.empty())
				return;
			std::map<PairKey, SeedCount> counts;
			std::unordered_set<uint64_t> seenContacts;
			// A contact is listed by both of its bodies (a character controller's presence body has no record: its contacts are
			// found through the dynamic bodies it touches), so contacts are counted once by their IDs.
			const auto seedRecord = [&](const BodyRecord& record)
			{
				if (!record.HasBody() || !record.InSimulation)
					return;
				if (record.IsTrigger)
					CountTriggerVisits(data, record, UUID::Null(), counts);
				else
					CountBodyContacts(data, record.BodyID, counts, seenContacts);
			};
			if (data.SeedAll)
			{
				for (const auto& [handle, record] : data.Bodies)
					seedRecord(record);
			}
			else
			{
				std::sort(data.SeededEntities.begin(), data.SeededEntities.end());
				data.SeededEntities.erase(std::unique(data.SeededEntities.begin(), data.SeededEntities.end()), data.SeededEntities.end());
				for (const UUID entityID : data.SeededEntities)
				{
					const Entity entity = data.OwnerScene->GetEntityByUUID(entityID);
					const BodyRecord* record = entity.IsValid() ? FindRecord(data, entity.GetHandle()) : nullptr;
					if (record)
						seedRecord(*record);
					if (record && record->IsTrigger)
						continue;
					// The triggers this entity is inside of.
					for (const entt::entity triggerHandle : data.TriggerBodies)
					{
						if (const BodyRecord* trigger = FindRecord(data, triggerHandle))
							CountTriggerVisits(data, *trigger, entityID, counts);
					}
				}
			}
			data.SeedAll = false;
			data.SeededEntities.clear();

			for (const auto& [key, seed] : counts)
			{
				auto it = data.TouchingPairs.find(key);
				if (it != data.TouchingPairs.end())
				{
					it->second.ContactCount = seed.Count;
					it->second.Unconfirmed = false;
					continue;
				}
				TouchingPair& pair = BeginPair(data, key, seed.A, seed.B, seed.Trigger, seed.Point, seed.Normal, seed.Contact);
				pair.ContactCount = seed.Count;
			}
		}

		// Forces and torques of the fixed step act in every box3d step of it (box3d clears them after each).
		void ApplyPendingForces(PhysicsWorldData& data, std::vector<entt::entity>& forced)
		{
			for (const entt::entity handle : forced)
			{
				const BodyRecord* record = FindRecord(data, handle);
				if (!record || !record->HasPendingForce || !record->InSimulation)
					continue;
				if (record->PendingForce != glm::vec3(0.0f))
					b3Body_ApplyForceToCenter(record->BodyID, ToB3(record->PendingForce), true);
				if (record->PendingTorque != glm::vec3(0.0f))
					b3Body_ApplyTorque(record->BodyID, ToB3(record->PendingTorque), true);
			}
		}

		// After an entity was moved by physics: with simulatedMove (written back from the simulation), dynamic descendants
		// simulated on their own keep their world pose (unless they are written themselves in this pass). The bodies of the
		// other descendants (static and kinematic ones, and dynamic ones after a teleport) follow their entities at the next
		// step: physics writes transforms without emitting on_update, so they are marked here instead.
		void UpdateDescendantsOfMovedBody(PhysicsWorldData& data, Entity moved, bool simulatedMove)
		{
			Scene& scene = *data.OwnerScene;
			const RelationshipComponent* relationship = moved.TryGetComponent<RelationshipComponent>();
			if (!relationship || relationship->Children.empty())
				return;

			VisitSubtree(scene, moved, data.VisitStack, [&](Entity entity)
			{
				if (entity == moved)
					return true;

				BodyRecord* record = FindRecord(data, entity.GetHandle());
				if (!record)
					return true;
				if (simulatedMove && record->WriteBackPass == data.WriteBackPass)
					return false; // Written itself, after this one (deeper entities are written later)

				if (simulatedMove && record->HasBody() && record->Type == RigidBodyType::Dynamic && record->InSimulation)
				{
					// Its world pose, and with it the poses of its own descendants, stays where its body is. If the entity
					// cannot take the pose, it moved with its parent; the next step notices and suspends the body.
					if (WriteWorldTransform(scene, entity, record->LastWorldTransform))
					{
						record->LastWorldTransform = scene.GetWorldTransform(entity);
						return false;
					}
				}
				// Also a record waiting for a valid transform: its transform changed.
				record->TransformDirty = true;
				UpdatePolling(data, entity.GetHandle(), *record);
				return true;
			});
		}

		// Moves the character controllers by one step, before box3d's (see CharacterControllerSet::Step): what they touched joins
		// the touching pairs (reported every step while it lasts, see ResolveCharacterPairs); bodies of their entities and of the
		// entities below them follow at the next step, as after a teleport (the write-back emits no on_update).
		void StepCharacters(PhysicsWorldData& data, float timestep, std::vector<PairKey>& reported)
		{
			CharacterStepOutput& output = data.CharacterStep;
			const bool collectContacts = data.TrackAll || !data.ContactInterest.empty();
			data.Characters->Step(timestep, ToGlm(b3World_GetGravity(data.World)), collectContacts, output);

			Scene& scene = *data.OwnerScene;
			for (entt::entity handle : output.Moved)
			{
				if (BodyRecord* record = FindRecord(data, handle))
				{
					record->TransformDirty = true;
					UpdatePolling(data, handle, *record);
				}
				UpdateDescendantsOfMovedBody(data, Entity(handle, &scene), false);
			}

			for (const CharacterContactReport& contact : output.Contacts)
			{
				// Wanted like any contact: every one while tracking all, else those of an entity of interest.
				if (contact.Controller == contact.Other || !IsWanted(data, contact.Controller, contact.Other))
					continue;
				const PairKey key = MakePairKey(contact.Controller, contact.Other);
				auto it = data.TouchingPairs.find(key);
				TouchingPair& pair = it != data.TouchingPairs.end() ? it->second
					: BeginPair(data, key, contact.Controller, contact.Other, false, contact.Point, contact.Normal, b3ContactId {});
				if (pair.CharacterStep != data.StepCount)
					reported.push_back(key);
				pair.CharacterStep = data.StepCount;
				pair.Unconfirmed = false;
				if (pair.ContactCount == 0)
				{
					// Only the controller holds it: its point is the move's.
					pair.Point = contact.Point;
					pair.Normal = pair.A == contact.Controller ? contact.Normal : -contact.Normal;
				}
			}
		}

		// A body whose simulated pose could not be written to its entity (its parent cannot be inverted, which the sync before
		// the step notices only when the transform changed, or the pose is degenerate): the body goes back to its entity's
		// pose and leaves the simulation until the transform is valid again.
		void SuspendUnwritableBody(PhysicsWorldData& data, entt::entity handle, BodyRecord& record)
		{
			if (ShouldWarn(data, record.EntityID, PhysicsWarning::DegenerateTransform))
			{
				ST_CORE_WARN("Physics: the parent of '{}' is scaled to (nearly) zero, so its simulated pose cannot be written back; its body leaves the "
					"simulation until the parent's transform is valid", Entity(handle, data.OwnerScene).GetName());
			}
			glm::vec3 entityPosition;
			glm::quat entityRotation;
			glm::vec3 entityScale;
			if (Math::DecomposeTransform(record.LastWorldTransform, entityPosition, entityRotation, entityScale) && IsWithinCoordinateRange(entityPosition))
				b3Body_SetTransform(record.BodyID, ToB3(entityPosition), ToB3(entityRotation));
			RemoveFromSimulation(data, record);
			record.Suspended = true;
			UpdatePolling(data, handle, record);
		}

		bool ReadBodyPose(b3BodyId body, glm::vec3& outPosition, glm::quat& outRotation)
		{
			const b3WorldTransform transform = b3Body_GetTransform(body);
			outPosition = ToGlm(transform.p);
			outRotation = ToGlm(transform.q);
			return IsFinite(outPosition) && IsFinite(outRotation);
		}

		void WarnNonFinitePose(PhysicsWorldData& data, entt::entity handle, const BodyRecord& record)
		{
			if (ShouldWarn(data, record.EntityID, PhysicsWarning::NonFiniteState))
				ST_CORE_ERROR("Physics: the simulation produced a non-finite pose for '{}'; its transform is not updated", Entity(handle, data.OwnerScene).GetName());
		}

		// Writes one body's simulated pose to its entity, given its parent's world transform. The entity keeps its own scale:
		// the pose is composed with its world scale as of the last synchronization (the shape's within tolerance), or its
		// current one if it was edited without a signal while the body slept, in which case the shape is rebuilt before the
		// next step (ScaleChanged). Reads only its body (box3d's getters do not change the world) and writes only its record
		// and transform, so it may run on worker threads for distinct bodies.
		void WritePose(PoseWrite& item)
		{
			BodyRecord& record = *item.Record;
			glm::vec3 position;
			glm::quat rotation;
			if (!ReadBodyPose(record.BodyID, position, rotation))
			{
				item.NonFinite = true;
				return;
			}

			// The same products as Scene::GetWorldTransform and UpdateWorldTransforms, so that LastWorldTransform equals the
			// matrix the scene will cache (the next step compares them).
			const glm::mat4 currentWorld = item.HasParent ? item.ParentWorld * item.Transform->GetTransform() : item.Transform->GetTransform();
			glm::vec3 worldScale = record.WorldScale;
			if (currentWorld != record.LastWorldTransform)
			{
				glm::vec3 translation;
				glm::quat currentRotation;
				glm::vec3 scale;
				if (Math::DecomposeTransform(currentWorld, translation, currentRotation, scale))
					worldScale = scale;
				item.ScaleChanged = HasScaleChanged(worldScale, record.ShapeScale);
			}

			const glm::mat4 pose = Math::ComposeTransform(position, glm::normalize(rotation), worldScale);
			if (!SetLocalPose(*item.Transform, item.HasParent ? glm::inverse(item.ParentWorld) * pose : pose))
				return;
			record.LastWorldTransform = item.HasParent ? item.ParentWorld * item.Transform->GetTransform() : item.Transform->GetTransform();
			record.WorldScale = worldScale;
			item.Written = true;
		}

		// Writes the poses of the dynamic bodies that moved in the step (box3d's move events, those that fell asleep included)
		// to their entities.
		//
		// Bodies without a dynamic ancestor written in this step (nearly all of them: roots, and bodies below static or
		// kinematic parents) are written in parallel chunks on the job system, with their parents' world transforms read
		// before the writes start; each touches only its own record and transform, through pointers fetched beforehand, never
		// the registry. The scene is then told about every written transform in one batch (Scene::MarkTransformsChanged),
		// and the rare cases (scale edits, non-finite poses, failed writes, descendants to update) are handled in body order
		// on the main thread. Dynamic bodies below written ones (nested dynamics) follow, one by one, parents before
		// children, so that every child's local transform is computed against its parent's final pose.
		void WriteBackDynamicBodies(PhysicsWorldData& data)
		{
			ST_PROFILE_FUNCTION();

			Scene& scene = *data.OwnerScene;
			entt::registry& registry = scene.GetRegistry();

			// The bodies that moved, in body index order, each once: their bits were set in a bitmap over the body indices
			// (MarkMoved), and only the words that got bits are sorted, read and cleared again, so the ordering costs what
			// moved, not the world's size.
			std::vector<uint64_t>& movedBits = data.MovedBodyBits;
			std::vector<uint32_t>& movedWords = data.MovedBodyWords;
			std::sort(movedWords.begin(), movedWords.end());
			data.LastOrderedSlots = static_cast<uint32_t>(movedWords.size() * 64);

			data.WriteBackPass++;
			std::vector<PoseWrite>& writes = data.PoseWrites;
			writes.clear();
			for (const uint32_t word : movedWords)
			{
				const uint64_t wordBits = std::exchange(movedBits[word], 0);
				for (uint64_t bits = wordBits; bits != 0; bits &= bits - 1)
				{
					const BodySlot& slot = data.BodySlots[size_t(word) * 64 + static_cast<size_t>(std::countr_zero(bits))];
					BodyRecord* record = slot.Record;
					if (!slot.Used || !record || record->Type != RigidBodyType::Dynamic || !record->InSimulation)
						continue;
					record->WriteBackPass = data.WriteBackPass;
					writes.push_back(PoseWrite { record, slot.Entity });
				}
			}
			movedWords.clear();
			data.LastWrittenBodies = static_cast<uint32_t>(writes.size());

			// Bodies below another body written in this step wait for its pose (nested dynamics, serial); the others get their
			// transform and their parent's world transform now.
			std::vector<std::pair<uint32_t, entt::entity>>& nested = data.NestedWrites;
			nested.clear();
			const auto isWrittenAbove = [&](Entity entity)
			{
				for (Entity ancestor = entity.GetParent(); ancestor.IsValid(); ancestor = ancestor.GetParent())
				{
					const BodyRecord* record = FindRecord(data, ancestor.GetHandle());
					if (record && record->WriteBackPass == data.WriteBackPass)
						return true;
				}
				return false;
			};
			size_t kept = 0;
			for (PoseWrite& write : writes)
			{
				const Entity entity(write.Handle, &scene);
				const HierarchyComponent& node = registry.get<HierarchyComponent>(write.Handle);
				write.Transform = &registry.get<TransformComponent>(write.Handle);
				write.HasChildren = node.FirstChild != entt::null;
				write.HasParent = node.Parent != entt::null;
				if (write.HasParent)
				{
					if (isWrittenAbove(entity))
					{
						nested.emplace_back(scene.GetDepth(entity), write.Handle);
						continue;
					}
					write.ParentWorld = scene.GetWorldTransform(Entity(node.Parent, &scene));
					if (!IsInvertible(write.ParentWorld))
					{
						SuspendUnwritableBody(data, write.Handle, *write.Record);
						continue;
					}
				}
				writes[kept++] = write;
			}
			writes.resize(kept);

			// The writes: in parallel chunks when there are many.
			constexpr size_t c_ParallelWriteThreshold = 512;
			const uint32_t count = static_cast<uint32_t>(writes.size());
			if (count >= c_ParallelWriteThreshold && JobSystem::IsInitialized())
			{
				const uint32_t batchSize = std::max<uint32_t>(64, count / ((JobSystem::GetWorkerThreadCount() + 1) * 4));
				JobSystem::ParallelFor(count, batchSize, [&](uint32_t begin, uint32_t end)
				{
					for (uint32_t index = begin; index < end; index++)
						WritePose(writes[index]);
				});
			}
			else
			{
				for (PoseWrite& write : writes)
					WritePose(write);
			}

			// The outcomes, in body order on the main thread.
			std::vector<entt::entity>& written = data.WrittenEntities;
			written.clear();
			for (PoseWrite& write : writes)
			{
				if (write.NonFinite)
				{
					WarnNonFinitePose(data, write.Handle, *write.Record);
					continue;
				}
				if (write.ScaleChanged)
					data.OwnerRefreshes.Add(write.Handle);
				if (write.Written)
					written.push_back(write.Handle);
				else
					SuspendUnwritableBody(data, write.Handle, *write.Record);
			}
			scene.MarkTransformsChanged(written);
			for (const PoseWrite& write : writes)
			{
				if (write.Written && write.HasChildren)
					UpdateDescendantsOfMovedBody(data, Entity(write.Handle, &scene), true);
			}

			// Nested dynamics: by depth, then by entity (deterministic, parents first), each against its parent's new pose.
			std::sort(nested.begin(), nested.end());
			for (const auto& [depth, handle] : nested)
			{
				BodyRecord& record = *FindRecord(data, handle); // Listed above
				const Entity entity(handle, &scene);

				glm::vec3 position;
				glm::quat rotation;
				if (!ReadBodyPose(record.BodyID, position, rotation))
				{
					WarnNonFinitePose(data, handle, record);
					continue;
				}

				// As in WritePose.
				glm::vec3 worldScale = record.WorldScale;
				if (scene.GetWorldTransform(entity) != record.LastWorldTransform)
				{
					worldScale = GetWorldScale(scene, entity, record.WorldScale);
					if (HasScaleChanged(worldScale, record.ShapeScale))
						data.OwnerRefreshes.Add(handle);
				}

				if (!WriteWorldTransform(scene, entity, Math::ComposeTransform(position, glm::normalize(rotation), worldScale)))
				{
					SuspendUnwritableBody(data, handle, record);
					continue;
				}
				record.LastWorldTransform = scene.GetWorldTransform(entity);
				record.WorldScale = worldScale;
				UpdateDescendantsOfMovedBody(data, entity, true);
			}
		}

		//////////////////////////////////////////////////////////////////////////
		// Queries
		//////////////////////////////////////////////////////////////////////////

		// Whether a query reports a shape: not a trigger (unless requested), not the ignored entity's, not of an entity that is
		// destroyed or about to be. Queries run on the main thread, so reading the scene is safe.
		const ShapeSlot* AcceptQueryShape(const PhysicsWorldData& data, b3ShapeId shape, UUID ignoredEntity, bool includeTriggers)
		{
			const ShapeSlot* slot = ResolveShape(data, shape);
			if (!slot || (!includeTriggers && slot->Sensor) || (ignoredEntity != UUID::Null() && slot->Entity == ignoredEntity))
				return nullptr;
			const Entity entity = data.OwnerScene->GetEntityByUUID(slot->Entity);
			return entity.IsValid() && !IsPendingDestroyInHierarchy(*data.OwnerScene, entity) ? slot : nullptr;
		}

		bool MakeRay(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, glm::vec3& outTranslation, float& outLength)
		{
			if (!IsFinite(origin) || !IsFinite(direction) || std::isnan(maxDistance) || !(maxDistance > 0.0f))
				return false;

			const float directionLength = glm::length(direction);
			if (!(directionLength > 1.0e-12f) || !std::isfinite(directionLength))
				return false;

			outLength = std::min(maxDistance, c_MaxRaycastDistance);
			outTranslation = direction / directionLength * outLength;
			return true;
		}

		Entity GetBodyEntityOf(const PhysicsWorldData& data, Entity entity)
		{
			if (!entity.IsValid() || entity.GetScene() != data.OwnerScene)
				return {};
			if (FindRecord(data, entity.GetHandle()))
				return entity;
			auto merged = data.MergedOwners.find(entity.GetHandle());
			return merged != data.MergedOwners.end() ? Entity(merged->second, data.OwnerScene) : Entity();
		}

		// The entity whose body (and character controller) a query ignores for ignoreEntity: the owner of its colliders' body.
		UUID GetIgnoredEntity(const PhysicsWorldData& data, Entity ignoreEntity)
		{
			if (!ignoreEntity.IsValid() || ignoreEntity.GetScene() != data.OwnerScene)
				return UUID::Null();
			const Entity owner = GetBodyEntityOf(data, ignoreEntity);
			return owner.IsValid() ? owner.GetUUID() : ignoreEntity.GetUUID();
		}

		struct RayQuery
		{
			const PhysicsWorldData* Data = nullptr;
			UUID Ignored = UUID::Null();
			bool IncludeTriggers = false;
			bool All = false;
			// The closest hit, or (All) the closest hit per entity.
			struct Hit
			{
				UUID Entity = UUID::Null();
				uint64_t BodyKey = 0;
				glm::vec3 Point = glm::vec3(0.0f);
				glm::vec3 Normal = glm::vec3(0.0f);
				float Fraction = 2.0f;
			};
			Hit Closest;
			std::unordered_map<UUID, Hit> PerEntity;
		};

		float RayCallback(b3ShapeId shape, b3Pos point, b3Vec3 normal, float fraction, uint64_t, int, int, void* context)
		{
			RayQuery& query = *static_cast<RayQuery*>(context);
			const ShapeSlot* slot = AcceptQueryShape(*query.Data, shape, query.Ignored, query.IncludeTriggers);
			if (!slot)
				return -1.0f;
			// A ray starting inside a convex collider passes out of it without a hit.
			if (slot->Convex && !(fraction > 0.0f))
				return -1.0f;
			RayQuery::Hit hit { slot->Entity, slot->BodyKey, ToGlm(point), ToGlm(normal), fraction };
			if (!query.All)
			{
				if (fraction < query.Closest.Fraction)
					query.Closest = hit;
				return fraction; // Only closer hits from now on
			}
			auto [it, inserted] = query.PerEntity.try_emplace(slot->Entity, hit);
			if (!inserted && fraction < it->second.Fraction)
				it->second = hit;
			return 1.0f;
		}

		std::optional<RaycastHit> MakeRaycastHit(const PhysicsWorldData& data, const RayQuery::Hit& found, float length)
		{
			RaycastHit hit;
			hit.EntityID = found.Entity;
			hit.HitEntity = data.OwnerScene->GetEntityByUUID(found.Entity);
			hit.Point = found.Point;
			hit.Normal = found.Normal;
			hit.Distance = found.Fraction * length;
			if (!hit.HitEntity.IsValid() || !IsFinite(hit.Point) || !IsFinite(hit.Normal))
				return std::nullopt;
			return hit;
		}

		// The entities whose bodies overlap a convex proxy around `center`, in body order (deterministic).
		std::vector<Entity> CollectOverlaps(const PhysicsWorldData& data, const glm::vec3& center, const b3ShapeProxy& proxy, uint32_t layerMask, bool includeTriggers)
		{
			struct OverlapQuery
			{
				const PhysicsWorldData* Data = nullptr;
				bool IncludeTriggers = false;
				std::vector<std::pair<uint64_t, UUID>> Found;
			} query { &data, includeTriggers, {} };
			b3World_OverlapShape(data.World, ToB3(center), &proxy, PhysicsFilters::MakeQueryFilter(layerMask), [](b3ShapeId shape, void* context)
			{
				OverlapQuery& overlap = *static_cast<OverlapQuery*>(context);
				if (const ShapeSlot* slot = AcceptQueryShape(*overlap.Data, shape, UUID::Null(), overlap.IncludeTriggers))
					overlap.Found.emplace_back(slot->BodyKey, slot->Entity);
				return true;
			}, &query);

			// The body index (the key's low half) orders them as the bodies were made, whatever order the tree visits them in.
			std::sort(query.Found.begin(), query.Found.end(), [](const auto& a, const auto& b)
			{
				const uint32_t indexA = static_cast<uint32_t>(a.first);
				const uint32_t indexB = static_cast<uint32_t>(b.first);
				return indexA != indexB ? indexA < indexB : a.first < b.first;
			});
			query.Found.erase(std::unique(query.Found.begin(), query.Found.end()), query.Found.end());

			std::vector<Entity> entities;
			entities.reserve(query.Found.size());
			std::unordered_set<UUID> seen; // An entity may have a body and a character controller's presence body
			for (const auto& [key, entityID] : query.Found)
			{
				if (!seen.insert(entityID).second)
					continue;
				if (const Entity entity = data.OwnerScene->GetEntityByUUID(entityID))
					entities.push_back(entity);
			}
			return entities;
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// PhysicsWorld
	////////////////////////////////////////////////////////////////////////////////

	namespace
	{

		// The bodies of a scene that can move when a world is created for it: rigid bodies that are not static, and character
		// controllers' presence bodies. box3d reserves its static and moving storage apart.
		uint32_t CountMovingCandidates(const Scene& scene)
		{
			uint64_t count = CharacterControllerSet::CountCandidates(scene);
			const entt::registry& registry = scene.GetRegistry();
			if (const auto* rigidBodies = registry.storage<RigidBodyComponent>())
			{
				for (const entt::entity handle : static_cast<const entt::sparse_set&>(*rigidBodies))
				{
					if (rigidBodies->get(handle).Type != RigidBodyType::Static)
						count++;
				}
			}
			return static_cast<uint32_t>(std::min<uint64_t>(count, UINT32_MAX));
		}

	}

	PhysicsWorld::PhysicsWorld(Scene& scene, const PhysicsSettings& settings, const PhysicsCapacityDemand& demand, PhysicsBuildMode mode)
		: m_Data(CreateScope<PhysicsWorldData>(scene, SanitizeSettings(ResolveCapacities(settings, CountBodyCandidates(scene), demand))))
	{
		ST_PROFILE_FUNCTION();

		PhysicsWorldData& data = *m_Data;
		data.AutomaticMaxBodies = settings.MaxBodies == 0;
		data.AutomaticMaxContactConstraints = settings.MaxContactConstraints == 0;
		data.RoomBodies = data.Settings.MaxBodies;
		ST_CORE_VERIFY(HasFreeWorld(), "PhysicsWorld: box3d holds at most {} worlds at once; check PhysicsWorld::HasFreeWorld first", B3_MAX_WORLDS);

		b3WorldDef definition = b3DefaultWorldDef();
		const glm::vec3 gravity = scene.GetSettings().Gravity;
		if (IsFinite(gravity))
		{
			definition.gravity = ToB3(gravity);
		}
		else
		{
			ST_CORE_WARN("Physics: scene '{}' has a non-finite gravity; the default is used", scene.GetName());
			definition.gravity = ToB3(SceneSettings().Gravity);
		}
		definition.enableSleep = true;
		definition.enableContinuous = true;
		// The room box3d reserves at once (see LibraryRoomBodies), split between its static and moving storage like the
		// scene's bodies.
		const uint32_t candidateCount = CountBodyCandidates(scene);
		const PhysicsSettings automaticRoom = ResolveCapacities(PhysicsSettings(), candidateCount, demand);
		data.LibraryRoomBodies = std::min(data.RoomBodies, automaticRoom.MaxBodies);
		data.LibraryRoomContacts = std::min(data.Settings.MaxContactConstraints, automaticRoom.MaxContactConstraints);
		const uint64_t candidates = std::max<uint64_t>(candidateCount, 1);
		const uint64_t moving = std::min<uint64_t>(CountMovingCandidates(scene), candidates);
		const uint64_t movingRoom = std::min<uint64_t>(data.LibraryRoomBodies, (static_cast<uint64_t>(data.LibraryRoomBodies) * moving + candidates - 1) / candidates);
		definition.capacity.dynamicBodyCount = static_cast<int>(movingRoom);
		definition.capacity.staticBodyCount = static_cast<int>(data.LibraryRoomBodies - movingRoom);
		definition.capacity.dynamicShapeCount = definition.capacity.dynamicBodyCount;
		definition.capacity.staticShapeCount = definition.capacity.staticBodyCount;
		definition.capacity.contactCount = static_cast<int>(data.LibraryRoomContacts);
		data.Tasks.Configure(definition);
		data.World = b3CreateWorld(&definition);

		data.Characters = CreateScope<CharacterControllerSet>(scene, data.World, data.Settings);
		ConnectSignals(data);
		RememberMeshState(data); // Before the bodies read mesh data: later changes are noticed against it
		for (const Entity entity : scene.GetEntitiesInHierarchyOrder())
		{
			if (!HasPhysicsComponent(entity))
				continue;
			if (mode == PhysicsBuildMode::Sliced)
				data.PendingBuild.push_back(entity.GetHandle());
			else
				data.StructureChanges.Add(entity.GetHandle());
		}
		if (mode == PhysicsBuildMode::Immediate)
		{
			ApplyChanges(data);
			data.Built = true;
		}
	}

	bool PhysicsWorld::HasFreeWorld()
	{
		return b3GetWorldCount() < B3_MAX_WORLDS;
	}

	bool PhysicsWorld::BuildSlice(uint32_t maxOwners)
	{
		ST_PROFILE_FUNCTION();
		PhysicsWorldData& data = *m_Data;
		if (data.Built)
			return true;
		// The owners of the next entities (colliders merged into an ancestor's body are built with it), handed to a round of
		// changes with what was recorded meanwhile.
		uint32_t owners = 0;
		for (; data.PendingBuildIndex < data.PendingBuild.size() && owners < maxOwners; data.PendingBuildIndex++)
		{
			const Entity entity(data.PendingBuild[data.PendingBuildIndex], data.OwnerScene);
			if (entity.IsValid() && FindBodyOwner(entity) == entity)
			{
				data.OwnerRefreshes.Add(entity.GetHandle());
				owners++;
			}
		}
		ApplyChanges(data);
		data.PreparedSlices++;
		if (data.PendingBuildIndex < data.PendingBuild.size())
			return false;
		data.PendingBuild = {};
		data.Built = true;
		return true;
	}

	bool PhysicsWorld::IsBuilt() const
	{
		return m_Data->Built;
	}

	PhysicsSettings PhysicsWorld::ResolveCapacities(const PhysicsSettings& settings, uint32_t bodyCount, const PhysicsCapacityDemand& demand)
	{
		PhysicsSettings resolved = settings;
		const uint64_t bodies = std::max(bodyCount, demand.Bodies);
		if (resolved.MaxBodies == 0)
			resolved.MaxBodies = ClampCapacity(std::max<uint64_t>(c_MinAutomaticBodies, NextPowerOfTwo(2 * bodies)), PhysicsCapacitySettings::c_MaxBodiesLimit);
		if (resolved.MaxContactConstraints == 0)
		{
			resolved.MaxContactConstraints = ClampCapacity(std::max({ uint64_t(c_ContactsPerBody) * c_MinAutomaticBodies, c_ContactsPerBody * bodies,
				uint64_t(demand.ContactConstraints) }), PhysicsCapacitySettings::c_MaxContactConstraintsLimit);
		}
		return resolved;
	}

	uint64_t PhysicsWorld::EstimateReservedBytes(const PhysicsSettings& settings)
	{
		const PhysicsSettings resolved = SanitizeSettings(settings);
		return c_ReservedBytesBase + c_ReservedBytesPerBody * resolved.MaxBodies + c_ReservedBytesPerContact * resolved.MaxContactConstraints;
	}

	uint32_t PhysicsWorld::CountBodyCandidates(const Scene& scene)
	{
		const entt::registry& registry = scene.GetRegistry();
		uint64_t count = 0;
		if (const auto* rigidBodies = registry.storage<RigidBodyComponent>())
			count += rigidBodies->size();
		const auto countColliders = [&]<typename Collider, typename... Earlier>()
		{
			const auto* colliders = registry.storage<Collider>();
			if (!colliders)
				return;
			for (const entt::entity handle : static_cast<const entt::sparse_set&>(*colliders))
			{
				// Entities with several colliders count once (with the first collider type they have).
				if (!registry.any_of<RigidBodyComponent, Earlier...>(handle))
					count++;
			}
		};
		countColliders.template operator()<BoxColliderComponent>();
		countColliders.template operator()<SphereColliderComponent, BoxColliderComponent>();
		countColliders.template operator()<CapsuleColliderComponent, BoxColliderComponent, SphereColliderComponent>();
		countColliders.template operator()<MeshColliderComponent, BoxColliderComponent, SphereColliderComponent, CapsuleColliderComponent>();
		// Character controllers have presence bodies.
		count += CharacterControllerSet::CountCandidates(scene);
		return static_cast<uint32_t>(std::min<uint64_t>(count, UINT32_MAX));
	}

	PhysicsWorld::~PhysicsWorld()
	{
		PhysicsWorldData& data = *m_Data;
		data.Connections.clear();
		data.Characters.reset();
		// The world takes every body and shape with it at once; what the shapes pointed to (cooked meshes, compounds) goes
		// with the records afterwards.
		if (B3_IS_NON_NULL(data.World))
			b3DestroyWorld(data.World);
		data.World = b3_nullWorldId;
		data.Bodies.clear();
		data.RecordIndex.clear();
		data.RecordOverflow.clear();
	}

	void PhysicsWorld::SetMeshProvider(Ref<PhysicsMeshProvider> provider)
	{
		MeshProviderStorage& storage = GetMeshProviderStorage();
		storage.Custom = std::move(provider);
		storage.Generation++;
	}

	PhysicsMeshProvider& PhysicsWorld::GetMeshProvider()
	{
		return GetActiveMeshProvider();
	}

	Scene& PhysicsWorld::GetScene() const
	{
		return *m_Data->OwnerScene;
	}

	const PhysicsSettings& PhysicsWorld::GetSettings() const
	{
		return m_Data->Settings;
	}

	PhysicsCapacityDemand PhysicsWorld::GetCapacityDemand() const
	{
		return m_Data->Demand;
	}

	void PhysicsWorld::SetTrackAllContacts(bool trackAll)
	{
		PhysicsWorldData& data = *m_Data;
		if (data.TrackAll == trackAll)
			return;
		data.TrackAll = trackAll;
		// Pairs that touch now begin after the next step; pairs nobody wants any more end then.
		if (trackAll)
			data.SeedAll = true;
		else
			data.CheckAllWanted = true;
	}

	bool PhysicsWorld::IsTrackingAllContacts() const
	{
		return m_Data->TrackAll;
	}

	void PhysicsWorld::SetContactInterest(UUID entityID, bool interested)
	{
		PhysicsWorldData& data = *m_Data;
		if (interested ? !data.ContactInterest.insert(entityID).second : data.ContactInterest.erase(entityID) == 0)
			return;
		if (data.TrackAll)
			return; // Every pair is tracked anyway
		if (interested)
			data.SeededEntities.push_back(entityID);
		else
			data.UnwantedEntities.push_back(entityID);
	}

	void PhysicsWorld::ApplyPendingChanges()
	{
		ApplyChanges(*m_Data);
	}

	PhysicsCapacityInfo PhysicsWorld::GetCapacityInfo() const
	{
		const PhysicsWorldData& data = *m_Data;
		PhysicsCapacityInfo info;
		info.Bodies = GetUsedBodySlots(data);
		info.MaxBodies = data.RoomBodies;
		info.GrowthLimit = GetGrowthLimit(data.Settings);
		info.WaitingForSlot = data.SlotWaiterCount;
		info.PendingOther = data.FailureCounts[static_cast<size_t>(BuildFailure::DegenerateTransform)] + data.FailureCounts[static_cast<size_t>(BuildFailure::MissingMesh)];
		const PhysicsWorldData::UpdateCounters lastUpdate = GetLastUpdateCounters(data);
		info.AddedLastUpdate = lastUpdate.BodiesAdded;
		info.RemovedLastUpdate = lastUpdate.BodiesRemoved;
		info.GrowthCount = data.GrowthCount;
		info.LastGrowthMs = data.LastGrowthMs;
		info.ReservedBytes = GetReservedBytes();
		return info;
	}

	uint64_t PhysicsWorld::GetReservedBytes() const
	{
		// Constant time (the editor reads it every frame): the estimate of what box3d reserved and the most it held since
		// (b3World_GetMaxCapacity keeps them as values); counting its bytes (b3World_GetCounters) walks every island, sensor
		// and hull.
		const PhysicsWorldData& data = *m_Data;
		const b3Capacity held = b3World_GetMaxCapacity(data.World);
		PhysicsSettings room = data.Settings;
		room.MaxBodies = std::max<uint32_t>(data.LibraryRoomBodies, static_cast<uint32_t>(std::max(held.staticBodyCount, 0) + std::max(held.dynamicBodyCount, 0)));
		room.MaxContactConstraints = std::max<uint32_t>(data.LibraryRoomContacts, static_cast<uint32_t>(std::max(held.contactCount, 0)));
		return EstimateReservedBytes(room);
	}

	uint64_t PhysicsWorld::CountLibraryBytes() const
	{
		return static_cast<uint64_t>(std::max<int64_t>(b3World_GetCounters(m_Data->World).byteCount, 0));
	}

	void PhysicsWorld::Reserve(uint32_t bodies)
	{
		m_Data->ReservedBodies = std::min(bodies, PhysicsCapacitySettings::c_MaxBodiesLimit);
	}

	uint32_t PhysicsWorld::GetReservedBodies() const
	{
		return m_Data->ReservedBodies;
	}

	bool PhysicsWorld::Grow()
	{
		PhysicsWorldData& data = *m_Data;
		const uint32_t growths = data.GrowthCount;
		ApplyChanges(data);
		GrowRoom(data, data.ReservedBodies);
		return data.GrowthCount != growths;
	}

	bool PhysicsWorld::HasBody(Entity entity) const
	{
		return FindSimulatedRecord(*m_Data, entity) != nullptr;
	}

	uint64_t PhysicsWorld::GetBodyID(Entity entity) const
	{
		if (!entity.IsValid() || entity.GetScene() != m_Data->OwnerScene)
			return c_NoBodyID;
		const BodyRecord* record = FindRecord(*m_Data, entity.GetHandle());
		return record && record->HasBody() ? GetBodyKey(record->BodyID) : c_NoBodyID;
	}

	Entity PhysicsWorld::GetBodyEntity(Entity entity) const
	{
		return GetBodyEntityOf(*m_Data, entity);
	}

	void PhysicsWorld::Simulate(float timestep)
	{
		ST_PROFILE_FUNCTION();

		if (!(timestep > 0.0f) || !std::isfinite(timestep))
			return;

		PhysicsWorldData& data = *m_Data;
		using Clock = std::chrono::steady_clock;
		const auto millisecondsBetween = [](Clock::time_point start, Clock::time_point end) { return std::chrono::duration<float, std::milli>(end - start).count(); };
		const Clock::time_point startTime = Clock::now();
		ApplyChanges(data);
		// Room reserved for bodies to come (PhysicsWorld::Reserve) counts before the step (see "Capacities").
		GrowRoom(data, data.ReservedBodies);
		data.HeldLastStep = HoldForMeshes(data, timestep);
		if (data.HeldLastStep)
		{
			data.HeldStepCount++;
			data.LastStepTime = millisecondsBetween(startTime, Clock::now());
			return;
		}

		data.StepCount++; // Identifies this step in the contact pairs' marks
		SyncEntitiesToBodies(data, timestep);
		std::vector<PairKey> characterPairs;
		StepCharacters(data, timestep, characterPairs);
		CollectCheckedPairs(data);
		const Clock::time_point stepsStart = Clock::now();

		// CollisionSteps box3d steps of the fixed step; forces act in each, and each one's events are read before the next.
		const uint32_t steps = data.Settings.CollisionSteps;
		const float subStep = timestep / static_cast<float>(steps);
		float libraryTime = 0.0f;
		for (uint32_t step = 0; step < steps; step++)
		{
			ApplyPendingForces(data, data.ForcedBodies);
			const Clock::time_point libraryStart = Clock::now();
			{
				ST_PROFILE_SCOPE("PhysicsWorld::Simulate - box3d");
				b3World_Step(data.World, subStep, c_SolverSubSteps);
			}
			libraryTime += millisecondsBetween(libraryStart, Clock::now());
			data.HasStepped = true;
			ProcessStepEvents(data, step + 1 == steps);
		}
		for (const entt::entity handle : data.ForcedBodies)
		{
			if (BodyRecord* record = FindRecord(data, handle))
			{
				record->PendingForce = glm::vec3(0.0f);
				record->PendingTorque = glm::vec3(0.0f);
				record->HasPendingForce = false;
			}
		}
		data.ForcedBodies.clear();
		const Clock::time_point stepsEnd = Clock::now();

		WriteBackDynamicBodies(data);
		const Clock::time_point writeBackEnd = Clock::now();
		ResolveCharacterPairs(data, characterPairs);
		ResolveRecheckedPairs(data);
		EndUnwantedPairs(data);
		SeedWantedPairs(data);
		RefreshCheckedPairs(data);
		// Contacts beyond the room reserved for them: the scene's next world reserves what this one held.
		const b3Capacity held = b3World_GetMaxCapacity(data.World);
		if (held.contactCount > 0 && static_cast<uint32_t>(held.contactCount) > data.Settings.MaxContactConstraints)
			data.Demand.ContactConstraints = std::max(data.Demand.ContactConstraints, std::min(NextPowerOfTwo(static_cast<uint64_t>(held.contactCount)),
				PhysicsCapacitySettings::c_MaxContactConstraintsLimit));
		const Clock::time_point endTime = Clock::now();

		const float eventTime = millisecondsBetween(stepsStart, stepsEnd) - libraryTime;
		data.LastSyncTime = millisecondsBetween(startTime, stepsStart);
		data.LastLibraryTime = libraryTime;
		data.LastWriteBackTime = millisecondsBetween(stepsEnd, writeBackEnd);
		data.LastContactTime = eventTime + millisecondsBetween(writeBackEnd, endTime);
		data.LastStepTime = millisecondsBetween(startTime, endTime);
	}

	std::vector<CollisionEvent> PhysicsWorld::TakeCollisionEvents()
	{
		std::vector<CollisionEvent> events = std::move(m_Data->PendingEvents);
		m_Data->PendingEvents.clear();
		for (CollisionEvent& event : events)
		{
			event.A = m_Data->OwnerScene->GetEntityByUUID(event.AID);
			event.B = m_Data->OwnerScene->GetEntityByUUID(event.BID);
		}
		return events;
	}

	bool PhysicsWorld::SetGravity(const glm::vec3& gravity)
	{
		if (!IsFinite(gravity))
			return false;

		b3World_SetGravity(m_Data->World, ToB3(gravity));
		WakeAllBodies(*m_Data);
		return true;
	}

	glm::vec3 PhysicsWorld::GetGravity() const
	{
		return ToGlm(b3World_GetGravity(m_Data->World));
	}

	bool PhysicsWorld::DoLayersCollide(uint32_t layerA, uint32_t layerB) const
	{
		return m_Data->Settings.DoLayersCollide(layerA, layerB);
	}

	void PhysicsWorld::SetLayerCollisionMasks(const std::array<uint32_t, c_PhysicsLayerCount>& masks)
	{
		PhysicsWorldData& data = *m_Data;
		if (data.Settings.LayerCollisionMasks == masks)
			return;
		const std::array<uint32_t, c_PhysicsLayerCount> previous = data.Settings.LayerCollisionMasks;
		data.Settings.LayerCollisionMasks = masks;
		// box3d keeps a filter per shape: the shapes of the layers whose masks changed take the new ones (rare, O(bodies)).
		// box3d destroys their contacts and finds them again at the next step, so their pairs wait for it to confirm them,
		// and only the pairs whose layers the change parts end.
		for (auto& [handle, record] : data.Bodies)
		{
			if (!record.HasBody() || previous[record.Layer] == masks[record.Layer])
				continue;
			const b3Filter filter = PhysicsFilters::MakeShapeFilter(record.Layer, masks);
			for (const b3ShapeId shape : record.Shapes)
				b3Shape_SetFilter(shape, filter, true);
			MarkPairsUnconfirmed(data, record.EntityID, LostContacts::Solid);
		}
		std::vector<UUID> refilteredCharacters;
		data.Characters->RefreshFilters(refilteredCharacters);
		for (const UUID entityID : refilteredCharacters)
			MarkPairsUnconfirmed(data, entityID, LostContacts::Solid);
		WakeAllBodies(data);
	}

	glm::vec3 PhysicsWorld::GetLinearVelocity(Entity entity) const
	{
		const BodyRecord* record = FindSimulatedRecord(*m_Data, entity);
		if (!record || record->Type == RigidBodyType::Static)
			return glm::vec3(0.0f);
		return ToGlm(b3Body_GetLinearVelocity(record->BodyID));
	}

	namespace
	{

		void WakeRecord(PhysicsWorldData& data, const BodyRecord& record)
		{
			b3Body_SetAwake(record.BodyID, true);
			MarkAwake(data, record.BodyID);
		}

		// Queues a force or torque for the next fixed step (see ApplyPendingForces).
		void AddPendingForce(PhysicsWorldData& data, Entity entity, BodyRecord& record, const glm::vec3& force, const glm::vec3& torque)
		{
			if (!record.HasPendingForce)
			{
				record.HasPendingForce = true;
				data.ForcedBodies.push_back(entity.GetHandle());
			}
			record.PendingForce += force;
			record.PendingTorque += torque;
			WakeRecord(data, record);
		}

	}

	bool PhysicsWorld::SetLinearVelocity(Entity entity, const glm::vec3& velocity)
	{
		const BodyRecord* record = FindDynamicRecord(*m_Data, entity);
		if (!record || !IsFinite(velocity))
			return false;

		b3Body_SetLinearVelocity(record->BodyID, ToB3(velocity));
		WakeRecord(*m_Data, *record);
		return true;
	}

	glm::vec3 PhysicsWorld::GetAngularVelocity(Entity entity) const
	{
		const BodyRecord* record = FindSimulatedRecord(*m_Data, entity);
		if (!record || record->Type == RigidBodyType::Static)
			return glm::vec3(0.0f);
		return ToGlm(b3Body_GetAngularVelocity(record->BodyID));
	}

	bool PhysicsWorld::SetAngularVelocity(Entity entity, const glm::vec3& velocity)
	{
		const BodyRecord* record = FindDynamicRecord(*m_Data, entity);
		if (!record || !IsFinite(velocity))
			return false;

		b3Body_SetAngularVelocity(record->BodyID, ToB3(velocity));
		WakeRecord(*m_Data, *record);
		return true;
	}

	bool PhysicsWorld::AddForce(Entity entity, const glm::vec3& force)
	{
		BodyRecord* record = FindDynamicRecord(*m_Data, entity);
		if (!record || !IsFinite(force))
			return false;

		AddPendingForce(*m_Data, entity, *record, force, glm::vec3(0.0f));
		return true;
	}

	bool PhysicsWorld::AddForceAtPosition(Entity entity, const glm::vec3& force, const glm::vec3& worldPosition)
	{
		BodyRecord* record = FindDynamicRecord(*m_Data, entity);
		if (!record || !IsFinite(force) || !IsFinite(worldPosition))
			return false;

		// Off its center of mass, a force also turns the body.
		const glm::vec3 center = ToGlm(b3Body_GetWorldCenter(record->BodyID));
		AddPendingForce(*m_Data, entity, *record, force, glm::cross(worldPosition - center, force));
		return true;
	}

	bool PhysicsWorld::AddImpulse(Entity entity, const glm::vec3& impulse)
	{
		const BodyRecord* record = FindDynamicRecord(*m_Data, entity);
		if (!record || !IsFinite(impulse))
			return false;

		b3Body_ApplyLinearImpulseToCenter(record->BodyID, ToB3(impulse), true);
		MarkAwake(*m_Data, record->BodyID);
		return true;
	}

	bool PhysicsWorld::AddImpulseAtPosition(Entity entity, const glm::vec3& impulse, const glm::vec3& worldPosition)
	{
		const BodyRecord* record = FindDynamicRecord(*m_Data, entity);
		if (!record || !IsFinite(impulse) || !IsFinite(worldPosition))
			return false;

		b3Body_ApplyLinearImpulse(record->BodyID, ToB3(impulse), ToB3(worldPosition), true);
		MarkAwake(*m_Data, record->BodyID);
		return true;
	}

	bool PhysicsWorld::AddTorque(Entity entity, const glm::vec3& torque)
	{
		BodyRecord* record = FindDynamicRecord(*m_Data, entity);
		if (!record || !IsFinite(torque))
			return false;

		AddPendingForce(*m_Data, entity, *record, glm::vec3(0.0f), torque);
		return true;
	}

	bool PhysicsWorld::AddAngularImpulse(Entity entity, const glm::vec3& impulse)
	{
		const BodyRecord* record = FindDynamicRecord(*m_Data, entity);
		if (!record || !IsFinite(impulse))
			return false;

		b3Body_ApplyAngularImpulse(record->BodyID, ToB3(impulse), true);
		MarkAwake(*m_Data, record->BodyID);
		return true;
	}

	bool PhysicsWorld::SetGravityScale(Entity entity, float gravityScale)
	{
		const BodyRecord* record = FindSimulatedRecord(*m_Data, entity);
		if (!record || !std::isfinite(gravityScale))
			return false;

		// Applied to the body directly; the component is updated without an on_update notification so that the body is not
		// rebuilt for it.
		if (RigidBodyComponent* rigidBody = entity.TryGetComponent<RigidBodyComponent>())
			rigidBody->GravityScale = gravityScale;
		if (record->Type != RigidBodyType::Static)
		{
			b3Body_SetGravityScale(record->BodyID, gravityScale);
			WakeRecord(*m_Data, *record);
		}
		return true;
	}

	bool PhysicsWorld::IsSleeping(Entity entity) const
	{
		const BodyRecord* record = FindSimulatedRecord(*m_Data, entity);
		if (!record || record->Type == RigidBodyType::Static)
			return false;
		return !b3Body_IsAwake(record->BodyID);
	}

	bool PhysicsWorld::WakeUp(Entity entity)
	{
		const BodyRecord* record = FindSimulatedRecord(*m_Data, entity);
		if (!record || record->Type == RigidBodyType::Static)
			return false;

		WakeRecord(*m_Data, *record);
		return true;
	}

	bool PhysicsWorld::Teleport(Entity entity, const glm::vec3& position, const glm::quat& rotation)
	{
		PhysicsWorldData& data = *m_Data;
		BodyRecord* record = FindSimulatedRecord(data, entity);
		if (!record || !IsFinite(position) || !IsFinite(rotation) || !IsWithinCoordinateRange(position))
			return false;

		const float rotationLength = glm::length(rotation);
		if (!(rotationLength > 1.0e-6f))
			return false;
		const glm::quat normalizedRotation = rotation / rotationLength;

		// The entity keeps its world scale; if it was edited without a signal and no longer matches the shape, the shape is
		// rebuilt before the next step.
		Scene& scene = *data.OwnerScene;
		const glm::vec3 worldScale = GetWorldScale(scene, entity, record->WorldScale);
		if (HasScaleChanged(worldScale, record->ShapeScale))
			data.OwnerRefreshes.Add(entity.GetHandle());
		if (!WriteWorldTransform(scene, entity, Math::ComposeTransform(position, normalizedRotation, worldScale)))
		{
			ST_CORE_WARN("Physics: cannot teleport '{}': its parent is scaled to (nearly) zero", entity.GetName());
			return false;
		}
		record->LastWorldTransform = scene.GetWorldTransform(entity);
		record->WorldScale = worldScale;
		record->KinematicTargetPosition = position;
		record->KinematicTargetRotation = normalizedRotation;
		record->KinematicMoving = false;

		// Bodies resting on it must notice that it left.
		if (record->Type == RigidBodyType::Static)
		{
			WakeBodiesIn(data, b3Body_ComputeAABB(record->BodyID));
			b3Body_SetTransform(record->BodyID, ToB3(position), ToB3(normalizedRotation));
			WakeBodiesIn(data, b3Body_ComputeAABB(record->BodyID));
		}
		else
		{
			WakeTouchingBodies(data, record->BodyID);
			if (record->Type == RigidBodyType::Kinematic)
			{
				b3Body_SetLinearVelocity(record->BodyID, b3Vec3 { 0.0f, 0.0f, 0.0f });
				b3Body_SetAngularVelocity(record->BodyID, b3Vec3 { 0.0f, 0.0f, 0.0f });
			}
			b3Body_SetTransform(record->BodyID, ToB3(position), ToB3(normalizedRotation));
			WakeRecord(data, *record);
		}

		// The entity's descendants moved with it.
		UpdateDescendantsOfMovedBody(data, entity, false);
		return true;
	}

	bool PhysicsWorld::HasCharacterController(Entity entity) const
	{
		return m_Data->Characters->Has(entity);
	}

	bool PhysicsWorld::MoveCharacter(Entity entity, const glm::vec3& velocity)
	{
		return m_Data->Characters->Move(entity, velocity);
	}

	bool PhysicsWorld::JumpCharacter(Entity entity, float speed)
	{
		return m_Data->Characters->Jump(entity, speed);
	}

	bool PhysicsWorld::TeleportCharacter(Entity entity, const glm::vec3& position)
	{
		PhysicsWorldData& data = *m_Data;
		if (!IsFinite(position) || !IsWithinCoordinateRange(position) || !data.Characters->Teleport(entity, position))
			return false;
		// Bodies of the entity (colliders on it) and below it follow, as they follow a teleported body.
		if (BodyRecord* record = FindRecord(data, entity.GetHandle()))
		{
			record->TransformDirty = true;
			UpdatePolling(data, entity.GetHandle(), *record);
		}
		UpdateDescendantsOfMovedBody(data, entity, false);
		return true;
	}

	bool PhysicsWorld::AddCharacterMotion(Entity entity, const glm::vec3& displacement)
	{
		return m_Data->Characters->AddMotion(entity, displacement);
	}

	std::optional<CharacterState> PhysicsWorld::GetCharacterState(Entity entity) const
	{
		return m_Data->Characters->GetState(entity);
	}

	std::optional<RaycastHit> PhysicsWorld::Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t layerMask, Entity ignoreEntity, bool includeTriggers) const
	{
		ST_PROFILE_FUNCTION();

		const PhysicsWorldData& data = *m_Data;
		glm::vec3 translation;
		float length = 0.0f;
		if (!MakeRay(origin, direction, maxDistance, translation, length))
			return std::nullopt;

		RayQuery query;
		query.Data = &data;
		query.Ignored = GetIgnoredEntity(data, ignoreEntity);
		query.IncludeTriggers = includeTriggers;
		b3World_CastRay(data.World, ToB3(origin), ToB3(translation), PhysicsFilters::MakeQueryFilter(layerMask), &RayCallback, &query);
		if (query.Closest.Fraction > 1.0f)
			return std::nullopt;
		return MakeRaycastHit(data, query.Closest, length);
	}

	std::vector<RaycastHit> PhysicsWorld::RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t layerMask, Entity ignoreEntity, bool includeTriggers) const
	{
		ST_PROFILE_FUNCTION();

		const PhysicsWorldData& data = *m_Data;
		std::vector<RaycastHit> hits;
		glm::vec3 translation;
		float length = 0.0f;
		if (!MakeRay(origin, direction, maxDistance, translation, length))
			return hits;

		RayQuery query;
		query.Data = &data;
		query.Ignored = GetIgnoredEntity(data, ignoreEntity);
		query.IncludeTriggers = includeTriggers;
		query.All = true;
		b3World_CastRay(data.World, ToB3(origin), ToB3(translation), PhysicsFilters::MakeQueryFilter(layerMask), &RayCallback, &query);

		std::vector<RayQuery::Hit> results;
		results.reserve(query.PerEntity.size());
		for (const auto& [entityID, hit] : query.PerEntity)
			results.push_back(hit);
		std::sort(results.begin(), results.end(), [](const RayQuery::Hit& a, const RayQuery::Hit& b)
		{
			return a.Fraction != b.Fraction ? a.Fraction < b.Fraction : static_cast<uint64_t>(a.Entity) < static_cast<uint64_t>(b.Entity);
		});

		hits.reserve(results.size());
		for (const RayQuery::Hit& result : results)
		{
			if (std::optional<RaycastHit> hit = MakeRaycastHit(data, result, length))
				hits.push_back(*hit);
		}
		return hits;
	}

	std::vector<Entity> PhysicsWorld::OverlapSphere(const glm::vec3& center, float radius, uint32_t layerMask, bool includeTriggers) const
	{
		ST_PROFILE_FUNCTION();

		if (!IsFinite(center) || !std::isfinite(radius) || !(radius > 0.0f))
			return {};

		const b3Vec3 point = { 0.0f, 0.0f, 0.0f };
		const b3ShapeProxy proxy = { &point, 1, radius };
		return CollectOverlaps(*m_Data, center, proxy, layerMask, includeTriggers);
	}

	std::vector<Entity> PhysicsWorld::OverlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, uint32_t layerMask, bool includeTriggers) const
	{
		ST_PROFILE_FUNCTION();

		if (!IsFinite(center) || !IsFinite(halfExtents) || !IsFinite(rotation) || glm::any(glm::lessThanEqual(halfExtents, glm::vec3(0.0f))))
			return {};

		const float rotationLength = glm::length(rotation);
		if (!(rotationLength > 1.0e-6f))
			return {};
		const glm::quat normalizedRotation = rotation / rotationLength;

		// The box's corners, relative to its center.
		std::array<b3Vec3, 8> corners;
		for (uint32_t index = 0; index < 8; index++)
		{
			const glm::vec3 corner((index & 1u) ? halfExtents.x : -halfExtents.x, (index & 2u) ? halfExtents.y : -halfExtents.y, (index & 4u) ? halfExtents.z : -halfExtents.z);
			corners[index] = ToB3(normalizedRotation * corner);
		}
		const b3ShapeProxy proxy = { corners.data(), 8, 0.0f };
		return CollectOverlaps(*m_Data, center, proxy, layerMask, includeTriggers);
	}

	PhysicsStats PhysicsWorld::GetStats() const
	{
		const PhysicsWorldData& data = *m_Data;
		PhysicsStats stats;
		for (const auto& [handle, record] : data.Bodies)
		{
			if (!record.HasBody())
			{
				if (IsPending(record))
					stats.PendingBodyCount++;
				continue;
			}
			if (!record.InSimulation)
				continue;

			stats.BodyCount++;
			switch (record.Type)
			{
				case RigidBodyType::Static: stats.StaticBodyCount++; break;
				case RigidBodyType::Dynamic: stats.DynamicBodyCount++; break;
				case RigidBodyType::Kinematic: stats.KinematicBodyCount++; break;
			}
		}
		stats.CharacterControllerCount = data.Characters->GetCount();
		stats.CharacterTime = data.Characters->GetLastStepTime();
		stats.ActiveBodyCount = static_cast<uint32_t>(std::max(b3World_GetAwakeBodyCount(data.World), 0));
		stats.ContactPairCount = static_cast<uint32_t>(data.TouchingPairs.size());
		stats.PairIndexOperations = data.PairIndexOperations;
		stats.MaxBodies = data.RoomBodies;
		stats.MaxContactConstraints = data.Settings.MaxContactConstraints;
		stats.GrowthLimit = GetGrowthLimit(data.Settings);
		stats.BodyOverflowCount = data.SlotWaiterCount;
		stats.Demand = data.Demand;
		stats.SyncedBodyCount = data.LastSyncedBodies;
		stats.CheckedPairCount = data.LastCheckedPairs;
		stats.WrittenBodyCount = data.LastWrittenBodies;
		stats.OrderedSlotCount = data.LastOrderedSlots;
		stats.BuildCount = data.BuildCount;
		stats.ParallelBuildCount = data.ParallelBuildCount;
		stats.WakeQueryCount = data.WakeQueryCount;
		stats.PreparedSlices = data.PreparedSlices;
		const PhysicsWorldData::UpdateCounters lastUpdate = GetLastUpdateCounters(data);
		stats.BodiesAddedLastUpdate = lastUpdate.BodiesAdded;
		stats.BodiesRemovedLastUpdate = lastUpdate.BodiesRemoved;
		stats.ChangeTime = lastUpdate.ChangeTime;
		stats.AddBatchCount = data.AddBatchCount;
		stats.SharedShapeCount = static_cast<uint32_t>(data.BoxHulls.size());
		stats.MeshCheckCount = data.MeshCheckCount;
		stats.WaitingForMeshes = data.HeldLastStep;
		stats.HeldStepCount = data.HeldStepCount;
		stats.StepCount = data.StepCount;
		stats.JobCount = data.TaskCounters.Tasks.load(std::memory_order_relaxed);
		stats.WorkerJobCount = data.TaskCounters.WorkerTasks.load(std::memory_order_relaxed);
		stats.LastStepTime = data.LastStepTime;
		stats.LibraryStepTime = data.LastLibraryTime;
		stats.SyncTime = data.LastSyncTime;
		stats.WriteBackTime = data.LastWriteBackTime;
		stats.ContactTime = data.LastContactTime;
		stats.GrowthCount = data.GrowthCount;
		stats.LastGrowthMs = data.LastGrowthMs;
		stats.LastGrowthFrom = data.LastGrowthFrom;
		stats.LastGrowthTo = data.LastGrowthTo;
		return stats;
	}

}
