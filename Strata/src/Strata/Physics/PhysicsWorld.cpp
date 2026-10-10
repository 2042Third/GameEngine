#include "stpch.h"
#include "Strata/Physics/PhysicsWorld.h"

#include "Strata/Core/Hash.h"
#include "Strata/Math/Math.h"
#include "Strata/Physics/AssetMeshProvider.h"
#include "Strata/Physics/PhysicsJobSystem.h"
#include "Strata/Physics/PhysicsMeshShapes.h"
#include "Strata/Physics/PhysicsRuntime.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/StreamIn.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <array>
#include <bit>
#include <cstring>
#include <map>
#include <set>
#include <span>

namespace Strata
{

	namespace
	{

		// Broad phase layers: static bodies never collide with each other, so they live in their own tree that only moving
		// bodies are tested against.
		constexpr JPH::BroadPhaseLayer c_StaticBroadPhaseLayer(0);
		constexpr JPH::BroadPhaseLayer c_MovingBroadPhaseLayer(1);
		constexpr JPH::uint c_BroadPhaseLayerCount = 2;

		// Collider dimensions (after scaling) are clamped to at least this size so that Jolt never sees degenerate shapes.
		constexpr float c_MinColliderExtent = 1.0e-3f;
		// Relative change of an entity's world scale that rebuilds its body's shape, and the smallest absolute change that
		// counts (scales this small are degenerate anyway).
		constexpr float c_ScaleChangeTolerance = 1.0e-4f;
		constexpr float c_MinScaleChange = 1.0e-10f;
		constexpr float c_MaxRaycastDistance = 1.0e5f;
		// Bodies within this distance of a body that is removed or moved are woken up, so that nothing keeps sleeping on
		// top of a body that is no longer there.
		constexpr float c_WakeMargin = 0.1f;
		constexpr float c_MinimumMass = 0.001f;
		// Applying a change can move colliders to another body, which is a change in turn; this many rounds always settle
		// a consistent hierarchy, so hitting the limit means a bug rather than a big change.
		constexpr uint32_t c_MaxChangeRounds = 16;

		// Warnings are issued once per entity and kind. The transient kinds are reset when the entity's body is built, so
		// that a recurring problem is reported again.
		enum class PhysicsWarning : uint8_t
		{
			MissingCollider = 1,
			DegenerateTransform,
			MissingMesh,
			NoUsableCollider,
			BodyLimit,
			// Kinds below are reported once for the entity's lifetime
			InvalidLayer,
			InvalidType,
			InvalidCollider,
			InvalidMesh,
			UnsupportedMesh,
			NonFiniteState,
			Count
		};

		constexpr PhysicsWarning c_TransientWarnings[] = { PhysicsWarning::MissingCollider, PhysicsWarning::DegenerateTransform, PhysicsWarning::MissingMesh, PhysicsWarning::NoUsableCollider, PhysicsWarning::BodyLimit };

		// Why an entity that needs a body has none.
		enum class BuildFailure : uint8_t
		{
			None = 0,
			NoCollider,          // A rigid body without colliders; waits for a component change
			InvalidColliders,    // Every collider has invalid data; waits for a component change
			DegenerateTransform, // Retried when a transform change is signaled (on the entity or an ancestor)
			MissingMesh,         // Retried when the mesh provider reports new data or a shape finished cooking
			BodyLimit            // Retried every step while the world is full
		};

		//////////////////////////////////////////////////////////////////////////
		// Conversions
		//////////////////////////////////////////////////////////////////////////

		JPH::Vec3 ToJolt(const glm::vec3& value)
		{
			return JPH::Vec3(value.x, value.y, value.z);
		}

		JPH::RVec3 ToJoltPosition(const glm::vec3& value)
		{
			return JPH::RVec3(value.x, value.y, value.z);
		}

		JPH::Quat ToJolt(const glm::quat& value)
		{
			return JPH::Quat(value.x, value.y, value.z, value.w);
		}

		glm::vec3 ToGlm(JPH::Vec3Arg value)
		{
			return glm::vec3(value.GetX(), value.GetY(), value.GetZ());
		}

		glm::quat ToGlm(JPH::QuatArg value)
		{
			return glm::quat(value.GetW(), value.GetX(), value.GetY(), value.GetZ());
		}

		bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		bool IsFinite(const glm::quat& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
		}

		JPH::EMotionType ToMotionType(RigidBodyType type)
		{
			switch (type)
			{
				case RigidBodyType::Static: return JPH::EMotionType::Static;
				case RigidBodyType::Dynamic: return JPH::EMotionType::Dynamic;
				case RigidBodyType::Kinematic: return JPH::EMotionType::Kinematic;
			}
			return JPH::EMotionType::Static;
		}

		//////////////////////////////////////////////////////////////////////////
		// Layers
		//////////////////////////////////////////////////////////////////////////

		// Object layers encode the user layer (0-31) and whether the body can move: objectLayer = userLayer * 2 + moving.
		JPH::ObjectLayer MakeObjectLayer(uint32_t userLayer, bool moving)
		{
			return static_cast<JPH::ObjectLayer>(userLayer * 2u + (moving ? 1u : 0u));
		}

		uint32_t GetUserLayer(JPH::ObjectLayer layer)
		{
			return static_cast<uint32_t>(layer) >> 1u;
		}

		bool IsMovingLayer(JPH::ObjectLayer layer)
		{
			return (static_cast<uint32_t>(layer) & 1u) != 0;
		}

		class BroadPhaseLayerMapping final : public JPH::BroadPhaseLayerInterface
		{
		public:
			JPH::uint GetNumBroadPhaseLayers() const override
			{
				return c_BroadPhaseLayerCount;
			}

			JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
			{
				return IsMovingLayer(layer) ? c_MovingBroadPhaseLayer : c_StaticBroadPhaseLayer;
			}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
			const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
			{
				return layer == c_MovingBroadPhaseLayer ? "Moving" : "Static";
			}
#endif
		};

		class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
		{
		public:
			bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhaseLayer) const override
			{
				return IsMovingLayer(layer) || broadPhaseLayer == c_MovingBroadPhaseLayer;
			}
		};

		// Reads the live layer matrix of the world's settings; it is only changed on the main thread between steps.
		class UserLayerPairFilter final : public JPH::ObjectLayerPairFilter
		{
		public:
			explicit UserLayerPairFilter(const PhysicsSettings& settings)
				: m_Settings(settings)
			{
			}

			bool ShouldCollide(JPH::ObjectLayer layerA, JPH::ObjectLayer layerB) const override
			{
				if (!IsMovingLayer(layerA) && !IsMovingLayer(layerB))
					return false;
				return m_Settings.DoLayersCollide(GetUserLayer(layerA), GetUserLayer(layerB));
			}
		private:
			const PhysicsSettings& m_Settings;
		};

		// Query filter accepting the user layers of a layer mask.
		class LayerMaskFilter final : public JPH::ObjectLayerFilter
		{
		public:
			explicit LayerMaskFilter(uint32_t layerMask)
				: m_LayerMask(layerMask)
			{
			}

			bool ShouldCollide(JPH::ObjectLayer layer) const override
			{
				return (m_LayerMask & ST_BIT(GetUserLayer(layer))) != 0;
			}
		private:
			uint32_t m_LayerMask;
		};

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

		// Sets an entity's world transform to a simulated pose. The entity keeps its authored scale (including mirrored axes)
		// whenever the new local transform can be expressed with it, so that only its translation and rotation change;
		// otherwise (e.g. below a sheared parent) the local transform is decomposed, which folds mirroring into X. Returns
		// false, leaving the entity unchanged, if its parent cannot be inverted or the transform is degenerate.
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

			// Written without on_update (this world listens to it for edits), so the scene is told directly that the cached world
			// transforms below the entity are stale.
			TransformComponent& transform = entity.GetComponent<TransformComponent>();
			const glm::vec3 translation(localTransform[3]);
			glm::quat rotation;
			if (IsFinite(translation) && SolveRotationForScale(localTransform, transform.Scale, rotation))
			{
				transform.Translation = translation;
				transform.Rotation = rotation;
				scene.MarkTransformChanged(entity);
				return true;
			}
			if (!transform.SetTransform(localTransform))
				return false;
			scene.MarkTransformChanged(entity);
			return true;
		}

		//////////////////////////////////////////////////////////////////////////
		// Contact pairs
		//////////////////////////////////////////////////////////////////////////

		// Contacts are tracked per pair of entities (not bodies), so that rebuilding a body keeps its contacts.
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

		struct TouchingPair
		{
			UUID A = UUID::Null();
			UUID B = UUID::Null();
			bool IsTrigger = false;
			uint64_t CheckStep = 0;  // Step during which the pair ends unless it is reported (see CollectCheckedPairs)
			uint64_t ReportStep = 0; // Last step during which the pair was reported
			uint64_t SortKey = 0;
			glm::vec3 Point = glm::vec3(0.0f);
			glm::vec3 Normal = glm::vec3(0.0f); // From A towards B
		};

		//////////////////////////////////////////////////////////////////////////
		// Listeners (called from Jolt's worker threads during a step)
		//////////////////////////////////////////////////////////////////////////

		struct ContactReport
		{
			PairKey Pair;
			// Entity of Jolt's body 1, the body with the lower ID: the contact constraint manager sorts the bodies by ID before
			// calling the contact listener (the order by motion type only applies to collision detection and validation).
			UUID EntityA = UUID::Null();
			UUID EntityB = UUID::Null();
			uint64_t SortKey = 0;        // Both body IDs; orders events independently of thread timing
			uint64_t SubShapeKey = 0;    // Both sub shape IDs; picks a deterministic manifold among several per pair
			glm::vec3 Point = glm::vec3(0.0f);
			glm::vec3 Normal = glm::vec3(0.0f);
			bool IsTrigger = false;
		};

		// A strict total order of the reports of a step that does not depend on the order in which worker threads reported
		// them: by pair, then by sub shape pair, then (several collision steps report the same sub shapes) by contact.
		bool IsReportOrderedBefore(const ContactReport& a, const ContactReport& b)
		{
			if (!(a.Pair == b.Pair))
				return a.Pair < b.Pair;
			if (a.SubShapeKey != b.SubShapeKey)
				return a.SubShapeKey < b.SubShapeKey;
			const auto bits = [](const ContactReport& report)
			{
				return std::array<uint32_t, 6> { std::bit_cast<uint32_t>(report.Point.x), std::bit_cast<uint32_t>(report.Point.y), std::bit_cast<uint32_t>(report.Point.z),
					std::bit_cast<uint32_t>(report.Normal.x), std::bit_cast<uint32_t>(report.Normal.y), std::bit_cast<uint32_t>(report.Normal.z) };
			};
			return bits(a) < bits(b);
		}

		// Records every contact that exists during a step. Contact begin/end is derived from these reports on the main
		// thread after the step (see ProcessContacts), so OnContactRemoved is not needed: it would also fire when a body
		// merely falls asleep, which must not end a contact.
		class ContactCollector final : public JPH::ContactListener
		{
		public:
			void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings&) override
			{
				Record(body1, body2, manifold);
			}

			void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings&) override
			{
				Record(body1, body2, manifold);
			}

			// Moves the reports collected since the last call into `reports` (discarding its contents) and keeps the other
			// buffer for the next step: both keep their capacity, so steps after the first allocate nothing.
			void SwapReports(std::vector<ContactReport>& reports)
			{
				reports.clear();
				std::scoped_lock<std::mutex> lock(m_Mutex);
				m_Reports.swap(reports);
			}
		private:
			void Record(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold)
			{
				ContactReport report;
				report.EntityA = UUID(body1.GetUserData());
				report.EntityB = UUID(body2.GetUserData());
				report.Pair = MakePairKey(report.EntityA, report.EntityB);
				report.SortKey = (static_cast<uint64_t>(body1.GetID().GetIndexAndSequenceNumber()) << 32) | body2.GetID().GetIndexAndSequenceNumber();
				report.SubShapeKey = (static_cast<uint64_t>(manifold.mSubShapeID1.GetValue()) << 32) | manifold.mSubShapeID2.GetValue();
				report.IsTrigger = body1.IsSensor() || body2.IsSensor();
				report.Normal = ToGlm(manifold.mWorldSpaceNormal);

				// Midway between the two surfaces, averaged over the manifold's points.
				const JPH::uint pointCount = static_cast<JPH::uint>(manifold.mRelativeContactPointsOn1.size());
				if (pointCount > 0)
				{
					JPH::Vec3 sum = JPH::Vec3::sZero();
					for (JPH::uint index = 0; index < pointCount; index++)
						sum += manifold.mRelativeContactPointsOn1[index] + manifold.mRelativeContactPointsOn2[index];
					report.Point = ToGlm(manifold.mBaseOffset + sum / (2.0f * static_cast<float>(pointCount)));
				}
				else
				{
					report.Point = ToGlm(0.5f * (body1.GetCenterOfMassPosition() + body2.GetCenterOfMassPosition()));
				}

				std::scoped_lock<std::mutex> lock(m_Mutex);
				m_Reports.push_back(report);
			}
		private:
			std::mutex m_Mutex;
			std::vector<ContactReport> m_Reports;
		};

		// Bodies that fall asleep at the end of a step moved during it; they are written back with the active ones.
		class ActivationCollector final : public JPH::BodyActivationListener
		{
		public:
			void OnBodyActivated(const JPH::BodyID&, JPH::uint64) override
			{
			}

			void OnBodyDeactivated(const JPH::BodyID& bodyID, JPH::uint64) override
			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				m_Deactivated.push_back(bodyID);
			}

			void Clear()
			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				m_Deactivated.clear();
			}

			// Like ContactCollector::SwapReports: double buffered, so that steps allocate nothing.
			void SwapDeactivated(std::vector<JPH::BodyID>& bodies)
			{
				bodies.clear();
				std::scoped_lock<std::mutex> lock(m_Mutex);
				m_Deactivated.swap(bodies);
			}
		private:
			std::mutex m_Mutex;
			std::vector<JPH::BodyID> m_Deactivated;
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

		// An entity that owns a body (a rigid body, or a collider entity without a rigid body ancestor). The record exists as
		// long as the entity needs a body, also while the body cannot be built (see BuildFailure).
		struct BodyRecord
		{
			UUID EntityID = UUID::Null();
			JPH::BodyID BodyID;                             // Invalid while the body cannot be built
			RigidBodyType Type = RigidBodyType::Static;
			BuildFailure Failure = BuildFailure::None;
			bool InSimulation = false;
			bool Suspended = false;                         // Out of the simulation because the world transform is degenerate
			bool WaitsForParent = false;                    // Not built because the parent cannot be inverted (dynamic bodies)
			bool KinematicMoving = false;                   // MoveKinematic gave the body a velocity during the last step
			bool TransformDirty = false;                    // A signaled transform change to apply at the next step
			bool Polled = false;                            // Listed in PhysicsWorldData::PolledBodies
			uint64_t WriteBackPass = 0;                     // Last write-back pass that wrote this body
			uint64_t SyncPass = 0;                          // Last step whose synchronization handled this record
			glm::vec3 ShapeScale = glm::vec3(1.0f);         // World scale baked into the shape
			glm::vec3 WorldScale = glm::vec3(1.0f);         // Entity world scale at the last synchronization (ShapeScale within tolerance)
			glm::mat4 LastWorldTransform = glm::mat4(1.0f); // Entity world transform the body was last synchronized with
			glm::vec3 KinematicTargetPosition = glm::vec3(0.0f);
			glm::quat KinematicTargetRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			// Velocities of a dynamic body while it is out of the simulation (Jolt clears them on removal).
			glm::vec3 SavedLinearVelocity = glm::vec3(0.0f);
			glm::vec3 SavedAngularVelocity = glm::vec3(0.0f);
			std::vector<entt::entity> MergedEntities; // Collider descendants that belong to this body
			std::vector<ShapeMember> ShapeMembers;    // Entities whose colliders are part of the current shape
			std::vector<MeshSource> MeshSources;      // Meshes of the colliders considered by the last build

			bool HasBody() const { return !BodyID.IsInvalid(); }
		};

		// A cooked mesh shape restored in a world, shared by the colliders using the same data.
		struct MeshShapeKey
		{
			const PhysicsMeshData* Data = nullptr;
			bool Convex = false;

			bool operator==(const MeshShapeKey& other) const { return Data == other.Data && Convex == other.Convex; }
		};

		struct MeshShapeKeyHash
		{
			size_t operator()(const MeshShapeKey& key) const
			{
				return static_cast<size_t>(Hash::Combine(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(key.Data)), key.Convex ? 1u : 0u));
			}
		};

		struct RestoredMeshShape
		{
			std::weak_ptr<const PhysicsMeshData> Source; // The entry belongs to its key only while the source is alive
			JPH::RefConst<JPH::Shape> Shape;
		};

		// Entities whose changes are applied by the next ApplyPendingChanges, in the order they were recorded.
		class ChangeList
		{
		public:
			void Add(entt::entity handle)
			{
				if (m_Set.insert(handle).second)
					m_List.push_back(handle);
			}

			bool IsEmpty() const { return m_List.empty(); }

			// Moves the recorded entities into `list` (discarding its contents); the list's buffer is reused for the next
			// changes, so applying changes does not allocate once the buffers have grown.
			const std::vector<entt::entity>& TakeInto(std::vector<entt::entity>& list)
			{
				list.clear();
				m_List.swap(list);
				m_Set.clear();
				return list;
			}

			void Clear()
			{
				m_List.clear();
				m_Set.clear();
			}
		private:
			std::vector<entt::entity> m_List;
			std::unordered_set<entt::entity> m_Set;
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

		PhysicsSettings SanitizeSettings(const PhysicsSettings& settings)
		{
			PhysicsSettings result = settings;
			result.MaxBodies = std::clamp(settings.MaxBodies, 1u, static_cast<uint32_t>(JPH::PhysicsSystem::cMaxBodiesLimit));
			result.MaxBodyPairs = std::clamp(settings.MaxBodyPairs, 1u, static_cast<uint32_t>(JPH::PhysicsSystem::cMaxBodyPairsLimit));
			result.MaxContactConstraints = std::clamp(settings.MaxContactConstraints, 1u, static_cast<uint32_t>(JPH::PhysicsSystem::cMaxContactConstraintsLimit));
			result.CollisionSteps = std::clamp(settings.CollisionSteps, 1u, 64u);
			result.TempAllocatorSize = std::max(settings.TempAllocatorSize, 64u * 1024u);
			result.MeshWaitTimeout = std::isfinite(settings.MeshWaitTimeout) ? std::max(settings.MeshWaitTimeout, 0.0f) : 0.0f;
			return result;
		}

	}

	struct PhysicsWorldData
	{
		PhysicsWorldData(Scene& scene, const PhysicsSettings& settings)
			: OwnerScene(&scene), Settings(settings), LayerPairFilter(Settings)
		{
		}

		PhysicsRuntimeReference Runtime; // First member: Jolt stays initialized until every other member is destroyed
		Scene* OwnerScene = nullptr;
		PhysicsSettings Settings;
		BroadPhaseLayerMapping BroadPhaseLayers;
		ObjectVsBroadPhaseFilter ObjectVsBroadPhase;
		UserLayerPairFilter LayerPairFilter;
		ContactCollector Contacts;
		ActivationCollector Activations;
		PhysicsJobCounters JobCounters;
		Scope<JPH::TempAllocator> Allocator;
		Scope<JPH::JobSystem> Jobs;
		std::unordered_map<MeshShapeKey, RestoredMeshShape, MeshShapeKeyHash> MeshShapes; // See PhysicsMeshShapes
		Scope<JPH::PhysicsSystem> JoltSystem; // Destroyed before the listeners, filters and allocators it references

		std::map<entt::entity, BodyRecord> Bodies;                    // Ordered: per-step processing is deterministic
		std::unordered_map<entt::entity, entt::entity> MergedOwners;  // Collider entity -> rigid body entity owning its body
		// Records that need attention at the next step besides the awake bodies (see NeedsPolling). Entries are dropped
		// lazily, so the list may hold records that no longer need it (or no longer exist).
		std::vector<entt::entity> PolledBodies;
		std::unordered_map<JPH::uint32, entt::entity> BodyEntities;   // Body ID (index and sequence number) -> entity
		std::unordered_map<PairKey, TouchingPair, PairKeyHash> TouchingPairs;
		std::unordered_map<UUID, std::vector<PairKey>> EntityPairs;   // Touching pairs of each entity
		std::vector<UUID> RecheckedEntities;                          // Built or rebuilt since the last step: their pairs are checked
		std::set<entt::entity> MeshBodies;                            // Records with MeshSources (ordered: deterministic rebuilds)
		std::set<entt::entity> WaitingMeshBodies;                     // Records with a MeshSource that waits for data or cooking
		std::unordered_map<AssetHandle, std::vector<entt::entity>> MeshUsers; // Mesh -> records whose MeshSources use it
		// The mesh provider and cook state that the MeshSources of the records were last checked against.
		uint64_t SeenMeshProviderGeneration = 0;
		uint64_t SeenMeshProviderVersion = 0;
		uint64_t SeenCompletedCooks = 0;
		std::vector<CollisionEvent> PendingEvents;
		std::vector<UUID> LeftAfterStep;                              // Bodies removed between the step and contact processing
		std::unordered_set<uint64_t> IssuedWarnings;                  // Hash of (entity, PhysicsWarning)
		JPH::EPhysicsUpdateError ReportedErrors = JPH::EPhysicsUpdateError::None;
		uint64_t StepCount = 0;
		uint64_t WriteBackPass = 0;
		uint64_t SyncPass = 0;
		float LastStepTime = 0.0f;
		uint32_t LastSyncedBodies = 0;
		uint32_t LastCheckedPairs = 0;
		uint32_t LastWrittenBodies = 0;
		uint64_t BuildCount = 0;
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
		std::vector<ContactReport> Reports;
		std::vector<TouchingPair> EndedPairs;
		std::vector<TouchingPair> BegunPairs;
		std::vector<JPH::BodyID> MovedBodies;
		std::vector<JPH::BodyID> DeactivatedBodies;
		std::vector<std::pair<uint32_t, entt::entity>> WriteBacks;   // Hierarchy depth and entity of the bodies to write back
		std::vector<Entity> VisitStack;                               // See VisitSubtree
		std::vector<entt::entity> ChangeScratch;                      // See ChangeList::TakeInto
		std::unordered_set<entt::entity> VisitedOwners;

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
			for (PhysicsWarning warning : warnings)
				data.IssuedWarnings.erase(MakeWarningKey(entityID, warning));
		}

		void ClearAllWarnings(PhysicsWorldData& data, UUID entityID)
		{
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

		uint32_t GetHierarchyDepth(Entity entity)
		{
			uint32_t depth = 0;
			for (Entity parent = entity.GetParent(); parent.IsValid(); parent = parent.GetParent())
				depth++;
			return depth;
		}

		template<typename Data>
		auto FindRecord(Data& data, entt::entity handle) -> decltype(&data.Bodies.begin()->second)
		{
			auto it = data.Bodies.find(handle);
			return it != data.Bodies.end() ? &it->second : nullptr;
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

		// Whether a record without a body waits for a body that can be built later (rather than for a component change).
		bool IsPending(const BodyRecord& record)
		{
			return !record.HasBody() && (record.Failure == BuildFailure::DegenerateTransform || record.Failure == BuildFailure::MissingMesh || record.Failure == BuildFailure::BodyLimit);
		}

		// Whether a record needs attention at the next step although its body may be asleep or out of the simulation (awake
		// bodies are synchronized anyway): a signaled transform change, which also retries a build that failed on a
		// degenerate transform and brings back a body suspended for one, or a build waiting for a free body slot. Nothing
		// else is polled: activity changes bring bodies back through their signals, and mesh data is checked when the mesh
		// provider reports changes (see RefreshMeshBodies).
		bool NeedsPolling(const BodyRecord& record)
		{
			return record.TransformDirty || (!record.HasBody() && record.Failure == BuildFailure::BodyLimit);
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

		void RemovePairFromIndex(PhysicsWorldData& data, UUID entityID, const PairKey& key)
		{
			auto it = data.EntityPairs.find(entityID);
			if (it == data.EntityPairs.end())
				return;

			std::vector<PairKey>& keys = it->second;
			keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
			if (keys.empty())
				data.EntityPairs.erase(it);
		}

		void AddTouchingPair(PhysicsWorldData& data, const PairKey& key, const TouchingPair& pair)
		{
			data.TouchingPairs.emplace(key, pair);
			data.EntityPairs[pair.A].push_back(key);
			data.EntityPairs[pair.B].push_back(key);
		}

		// Ends every contact of an entity (its body left the simulation).
		void EndContactsOf(PhysicsWorldData& data, UUID entityID)
		{
			auto indexIt = data.EntityPairs.find(entityID);
			if (indexIt == data.EntityPairs.end())
				return;

			const std::vector<PairKey> keys = std::move(indexIt->second);
			data.EntityPairs.erase(indexIt);

			std::vector<TouchingPair> ended;
			for (const PairKey& key : keys)
			{
				auto pairIt = data.TouchingPairs.find(key);
				if (pairIt == data.TouchingPairs.end())
					continue;

				const TouchingPair pair = pairIt->second;
				data.TouchingPairs.erase(pairIt);
				RemovePairFromIndex(data, pair.A == entityID ? pair.B : pair.A, key);
				ended.push_back(pair);
			}

			std::sort(ended.begin(), ended.end(), [](const TouchingPair& a, const TouchingPair& b) { return a.SortKey < b.SortKey; });
			for (const TouchingPair& pair : ended)
				QueueEvent(data, CollisionEventType::End, pair);
		}

		//////////////////////////////////////////////////////////////////////////
		// Simulation membership
		//////////////////////////////////////////////////////////////////////////

		// Wakes the bodies around a body (which may rest on it) before it is removed, moved or reshaped.
		void WakeBodiesAround(PhysicsWorldData& data, const JPH::BodyID& bodyID)
		{
			JPH::AABox bounds;
			{
				JPH::BodyLockRead lock(data.JoltSystem->GetBodyLockInterface(), bodyID);
				if (!lock.Succeeded())
					return;
				bounds = lock.GetBody().GetWorldSpaceBounds();
			}

			// The lock must be released first: activation locks the bodies it finds, possibly including this one.
			bounds.ExpandBy(JPH::Vec3::sReplicate(c_WakeMargin));
			const JPH::BroadPhaseLayerFilter broadPhaseFilter;
			const JPH::ObjectLayerFilter layerFilter;
			data.JoltSystem->GetBodyInterface().ActivateBodiesInAABox(bounds, broadPhaseFilter, layerFilter);
		}

		void WakeAllBodies(PhysicsWorldData& data)
		{
			std::vector<JPH::BodyID> bodies;
			for (const auto& [handle, record] : data.Bodies)
			{
				if (record.InSimulation && record.Type != RigidBodyType::Static)
					bodies.push_back(record.BodyID);
			}
			if (!bodies.empty())
				data.JoltSystem->GetBodyInterface().ActivateBodies(bodies.data(), static_cast<int>(bodies.size()));
		}

		// Keeps the motion of a dynamic body that leaves the simulation (Jolt clears it on removal) so that it resumes when
		// the body is back, also when the body is rebuilt in between.
		void SaveVelocities(PhysicsWorldData& data, BodyRecord& record)
		{
			if (!record.HasBody() || !record.InSimulation || record.Type != RigidBodyType::Dynamic)
				return;

			JPH::Vec3 linearVelocity;
			JPH::Vec3 angularVelocity;
			data.JoltSystem->GetBodyInterface().GetLinearAndAngularVelocity(record.BodyID, linearVelocity, angularVelocity);
			record.SavedLinearVelocity = ToGlm(linearVelocity);
			record.SavedAngularVelocity = ToGlm(angularVelocity);
		}

		void RemoveFromSimulation(PhysicsWorldData& data, BodyRecord& record)
		{
			if (!record.InSimulation)
				return;

			SaveVelocities(data, record);
			JPH::BodyInterface& bodies = data.JoltSystem->GetBodyInterface();
			WakeBodiesAround(data, record.BodyID);
			bodies.RemoveBody(record.BodyID);
			record.InSimulation = false;
			record.KinematicMoving = false;
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

			JPH::BodyInterface& bodies = data.JoltSystem->GetBodyInterface();
			const glm::mat4 worldTransform = data.OwnerScene->GetWorldTransform(entity);
			if (worldTransform != record.LastWorldTransform)
			{
				glm::vec3 position;
				glm::quat rotation;
				glm::vec3 scale;
				if (!Math::DecomposeTransform(worldTransform, position, rotation, scale))
					return PlacementResult::DegenerateTransform;
				if (HasScaleChanged(scale, record.ShapeScale))
					return PlacementResult::ScaleChanged;
				record.WorldScale = scale;

				bodies.SetPositionAndRotation(record.BodyID, ToJoltPosition(position), ToJolt(rotation), JPH::EActivation::DontActivate);
				record.LastWorldTransform = worldTransform;
				record.KinematicTargetPosition = position;
				record.KinematicTargetRotation = rotation;
			}

			bodies.AddBody(record.BodyID, record.Type == RigidBodyType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
			if (record.Type == RigidBodyType::Dynamic)
			{
				// Motion resumes where it stopped.
				bodies.SetLinearAndAngularVelocity(record.BodyID, ToJolt(record.SavedLinearVelocity), ToJolt(record.SavedAngularVelocity));
				record.SavedLinearVelocity = glm::vec3(0.0f);
				record.SavedAngularVelocity = glm::vec3(0.0f);
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

		// Destroys the Jolt body of a record (the record itself stays). Contacts are left alone.
		void DestroyJoltBody(PhysicsWorldData& data, BodyRecord& record)
		{
			if (!record.HasBody())
				return;

			JPH::BodyInterface& bodies = data.JoltSystem->GetBodyInterface();
			if (record.InSimulation)
			{
				WakeBodiesAround(data, record.BodyID);
				bodies.RemoveBody(record.BodyID);
				record.InSimulation = false;
			}
			bodies.DestroyBody(record.BodyID);
			data.BodyEntities.erase(record.BodyID.GetIndexAndSequenceNumber());
			record.BodyID = JPH::BodyID();
			record.Suspended = false;
			record.KinematicMoving = false;
		}

		void DestroyAllBodies(PhysicsWorldData& data)
		{
			std::vector<JPH::BodyID> added;
			std::vector<JPH::BodyID> all;
			for (const auto& [handle, record] : data.Bodies)
			{
				if (!record.HasBody())
					continue;
				if (record.InSimulation)
					added.push_back(record.BodyID);
				all.push_back(record.BodyID);
			}

			JPH::BodyInterface& bodies = data.JoltSystem->GetBodyInterface();
			if (!added.empty())
				bodies.RemoveBodies(added.data(), static_cast<int>(added.size()));
			if (!all.empty())
				bodies.DestroyBodies(all.data(), static_cast<int>(all.size()));

			data.Bodies.clear();
			data.MergedOwners.clear();
			data.PolledBodies.clear();
			data.RecheckedEntities.clear();
			data.MeshBodies.clear();
			data.WaitingMeshBodies.clear();
			data.MeshUsers.clear();
			data.BodyEntities.clear();
			data.TouchingPairs.clear();
			data.EntityPairs.clear();
			data.PendingEvents.clear();
		}

		//////////////////////////////////////////////////////////////////////////
		// Shapes
		//////////////////////////////////////////////////////////////////////////

		JPH::RefConst<JPH::Shape> CreateShape(PhysicsWorldData& data, const JPH::ShapeSettings& settings, const Entity& entity, std::string_view what, PhysicsWarning warning)
		{
			JPH::ShapeSettings::ShapeResult result = settings.Create();
			if (result.HasError())
			{
				if (ShouldWarn(data, entity.GetUUID(), warning))
					ST_CORE_WARN("Physics: the {} of '{}' is invalid and is ignored: {}", what, entity.GetName(), result.GetError().c_str());
				return nullptr;
			}
			return result.Get();
		}

		// Reads a shape saved in Jolt's binary state format (see PhysicsMeshShapes).
		class ByteStreamIn final : public JPH::StreamIn
		{
		public:
			explicit ByteStreamIn(std::span<const uint8_t> bytes)
				: m_Bytes(bytes)
			{
			}

			void ReadBytes(void* outData, size_t byteCount) override
			{
				if (byteCount == 0)
					return; // Empty arrays may pass a null destination
				if (byteCount > m_Bytes.size() - m_Offset)
				{
					// Reading past the end: the data is incomplete.
					std::memset(outData, 0, byteCount);
					m_Offset = m_Bytes.size();
					m_PastEnd = true;
					return;
				}
				std::memcpy(outData, m_Bytes.data() + m_Offset, byteCount);
				m_Offset += byteCount;
			}

			bool IsEOF() const override { return m_PastEnd; }
			bool IsFailed() const override { return m_PastEnd; }
		private:
			std::span<const uint8_t> m_Bytes;
			size_t m_Offset = 0;
			bool m_PastEnd = false;
		};

		struct ShapePart
		{
			JPH::RefConst<JPH::Shape> Shape;
			JPH::Vec3 Position = JPH::Vec3::sZero(); // Body space
			JPH::Quat Rotation = JPH::Quat::sIdentity();
		};

		struct ShapeBuild
		{
			std::vector<ShapePart> Parts;
			std::vector<ShapeMember> ShapeMembers;   // Entities that contributed colliders
			std::vector<MeshSource> MeshSources;     // Meshes of the mesh colliders considered
			bool WaitsForMesh = false;               // A mesh collider was left out until its data arrives or is cooked
			uint32_t ColliderCount = 0;              // Collider components considered, usable or not
		};

		void WarnNonFiniteCollider(PhysicsWorldData& data, const Entity& entity, std::string_view collider)
		{
			if (ShouldWarn(data, entity.GetUUID(), PhysicsWarning::InvalidCollider))
				ST_CORE_WARN("Physics: the {} of '{}' has non-finite dimensions and is ignored", collider, entity.GetName());
		}

		// The unscaled shape of a mesh collider: cooked once per mesh data object (process-wide, see PhysicsMeshShapes) and
		// restored once per world, shared by every collider using the data. Returns nullptr if there is no shape: the mesh
		// data has not arrived or is still cooking (the build waits for it, see RefreshMeshBodies), or it cannot be used.
		JPH::RefConst<JPH::Shape> GetMeshShape(PhysicsWorldData& data, const Entity& entity, AssetHandle mesh, bool convex, ShapeBuild& build)
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

			const MeshShapeKey key { meshData.get(), convex };
			auto it = data.MeshShapes.find(key);
			if (it != data.MeshShapes.end() && !it->second.Source.expired())
				return it->second.Shape;

			const PhysicsMeshShapes::Result cooked = PhysicsMeshShapes::Request(meshData, convex);
			if (cooked.CookState == PhysicsMeshShapes::State::Cooking)
			{
				source.Waiting = true;
				build.WaitsForMesh = true;
				return nullptr;
			}

			JPH::RefConst<JPH::Shape> shape;
			std::string error = cooked.Error;
			if (cooked.CookState == PhysicsMeshShapes::State::Ready)
			{
				ByteStreamIn stream(*cooked.Shape);
				JPH::Shape::ShapeResult restored = JPH::Shape::sRestoreFromBinaryState(stream);
				if (restored.HasError())
					error = restored.GetError().c_str();
				else if (stream.IsFailed())
					error = "its cooked shape is incomplete";
				else
					shape = restored.Get();
			}
			if (!shape)
			{
				if (ShouldWarn(data, entity.GetUUID(), PhysicsWarning::InvalidMesh))
					ST_CORE_WARN("Physics: the mesh collider of '{}' cannot use mesh {} and is ignored: {}", entity.GetName(), mesh.ToString(), error);
				return nullptr;
			}

			// Shapes of data that no longer exists go (which includes an entry left at this address).
			std::erase_if(data.MeshShapes, [](const auto& entry) { return entry.second.Source.expired(); });
			data.MeshShapes[key] = RestoredMeshShape { meshData, shape };
			return shape;
		}

		// Adds the colliders of an entity whose transform in body space is (translation, rotation, scale).
		void AddColliderParts(PhysicsWorldData& data, const Entity& entity, const glm::mat4& poseInBody, const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale, RigidBodyType bodyType, ShapeBuild& build)
		{
			const glm::vec3 absoluteScale = glm::abs(scale);
			const float maxScale = std::max({ absoluteScale.x, absoluteScale.y, absoluteScale.z });
			const JPH::Quat partRotation = ToJolt(rotation);
			const auto placeAt = [&](const glm::vec3& offset) { return ToJolt(translation + rotation * (offset * scale)); };
			const size_t partCount = build.Parts.size();

			if (const BoxColliderComponent* box = entity.TryGetComponent<BoxColliderComponent>())
			{
				build.ColliderCount++;
				if (IsFinite(box->HalfExtents) && IsFinite(box->Offset))
				{
					const glm::vec3 halfExtents = glm::max(glm::abs(box->HalfExtents) * absoluteScale, glm::vec3(c_MinColliderExtent));
					if (JPH::RefConst<JPH::Shape> shape = CreateShape(data, JPH::BoxShapeSettings(ToJolt(halfExtents)), entity, "box collider", PhysicsWarning::InvalidCollider))
						build.Parts.push_back({ shape, placeAt(box->Offset), partRotation });
				}
				else
				{
					WarnNonFiniteCollider(data, entity, "box collider");
				}
			}

			if (const SphereColliderComponent* sphere = entity.TryGetComponent<SphereColliderComponent>())
			{
				build.ColliderCount++;
				if (std::isfinite(sphere->Radius) && IsFinite(sphere->Offset))
				{
					const float radius = std::max(std::abs(sphere->Radius) * maxScale, c_MinColliderExtent);
					if (JPH::RefConst<JPH::Shape> shape = CreateShape(data, JPH::SphereShapeSettings(radius), entity, "sphere collider", PhysicsWarning::InvalidCollider))
						build.Parts.push_back({ shape, placeAt(sphere->Offset), partRotation });
				}
				else
				{
					WarnNonFiniteCollider(data, entity, "sphere collider");
				}
			}

			if (const CapsuleColliderComponent* capsule = entity.TryGetComponent<CapsuleColliderComponent>())
			{
				build.ColliderCount++;
				if (std::isfinite(capsule->Radius) && std::isfinite(capsule->HalfHeight) && IsFinite(capsule->Offset))
				{
					const float radius = std::max(std::abs(capsule->Radius) * std::max(absoluteScale.x, absoluteScale.z), c_MinColliderExtent);
					const float halfHeight = std::abs(capsule->HalfHeight) * absoluteScale.y;
					JPH::RefConst<JPH::Shape> shape;
					if (halfHeight < c_MinColliderExtent)
						shape = CreateShape(data, JPH::SphereShapeSettings(radius), entity, "capsule collider", PhysicsWarning::InvalidCollider); // No cylindrical part
					else
						shape = CreateShape(data, JPH::CapsuleShapeSettings(halfHeight, radius), entity, "capsule collider", PhysicsWarning::InvalidCollider);
					if (shape)
						build.Parts.push_back({ shape, placeAt(capsule->Offset), partRotation });
				}
				else
				{
					WarnNonFiniteCollider(data, entity, "capsule collider");
				}
			}

			if (const MeshColliderComponent* meshCollider = entity.TryGetComponent<MeshColliderComponent>())
			{
				build.ColliderCount++;
				AssetHandle mesh = meshCollider->Mesh;
				if (!mesh.IsValid())
				{
					if (const MeshRendererComponent* renderer = entity.TryGetComponent<MeshRendererComponent>())
						mesh = renderer->Mesh;
				}

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
				else if (JPH::RefConst<JPH::Shape> meshShape = GetMeshShape(data, entity, mesh, meshCollider->Convex, build))
				{
					if (scale == glm::vec3(1.0f))
					{
						build.Parts.push_back({ meshShape, ToJolt(translation), partRotation });
					}
					else
					{
						// Convex hulls and triangle meshes support any non-zero scale, including mirroring.
						if (JPH::RefConst<JPH::Shape> shape = CreateShape(data, JPH::ScaledShapeSettings(meshShape.GetPtr(), ToJolt(scale)), entity, "mesh collider", PhysicsWarning::InvalidMesh))
							build.Parts.push_back({ shape, ToJolt(translation), partRotation });
					}
				}
			}

			if (build.Parts.size() > partCount)
				build.ShapeMembers.push_back({ entity.GetHandle(), poseInBody });
		}

		JPH::RefConst<JPH::Shape> CombineParts(PhysicsWorldData& data, const Entity& owner, const std::vector<ShapePart>& parts)
		{
			if (parts.empty())
				return nullptr;

			if (parts.size() == 1)
			{
				const ShapePart& part = parts.front();
				if (part.Position.IsNearZero(0.0f) && part.Rotation == JPH::Quat::sIdentity())
					return part.Shape;

				const JPH::RotatedTranslatedShapeSettings settings(part.Position, part.Rotation, part.Shape.GetPtr());
				return CreateShape(data, settings, owner, "collider placement", PhysicsWarning::InvalidCollider);
			}

			JPH::StaticCompoundShapeSettings compound;
			for (const ShapePart& part : parts)
				compound.AddShape(part.Position, part.Rotation, part.Shape.GetPtr());
			return CreateShape(data, compound, owner, "compound collider", PhysicsWarning::InvalidCollider);
		}

		float SanitizeNonNegative(float value, float fallback)
		{
			return std::isfinite(value) ? std::max(value, 0.0f) : fallback;
		}

		JPH::BodyCreationSettings MakeBodySettings(PhysicsWorldData& data, const Entity& entity, const RigidBodyComponent* rigidBody, RigidBodyType type, const JPH::Shape* shape, const glm::vec3& position, const glm::quat& rotation)
		{
			const RigidBodyComponent defaults;
			uint32_t layer = 0;
			if (rigidBody && rigidBody->Layer < c_PhysicsLayerCount)
			{
				layer = rigidBody->Layer;
			}
			else if (rigidBody && ShouldWarn(data, entity.GetUUID(), PhysicsWarning::InvalidLayer))
			{
				ST_CORE_WARN("Physics: '{}' uses collision layer {}, but layers range from 0 to {}; layer 0 is used", entity.GetName(), rigidBody->Layer, c_PhysicsLayerCount - 1);
			}

			JPH::BodyCreationSettings settings(shape, ToJoltPosition(position), ToJolt(rotation), ToMotionType(type), MakeObjectLayer(layer, type != RigidBodyType::Static));
			settings.mUserData = static_cast<uint64_t>(entity.GetUUID());
			settings.mFriction = defaults.Friction;
			settings.mRestitution = defaults.Restitution;
			if (!rigidBody)
				return settings;

			settings.mFriction = SanitizeNonNegative(rigidBody->Friction, defaults.Friction);
			settings.mRestitution = std::isfinite(rigidBody->Restitution) ? std::clamp(rigidBody->Restitution, 0.0f, 1.0f) : defaults.Restitution;
			settings.mIsSensor = rigidBody->IsTrigger;
			if (type == RigidBodyType::Static)
				return settings;

			settings.mLinearDamping = SanitizeNonNegative(rigidBody->LinearDamping, defaults.LinearDamping);
			settings.mAngularDamping = SanitizeNonNegative(rigidBody->AngularDamping, defaults.AngularDamping);
			settings.mGravityFactor = std::isfinite(rigidBody->GravityScale) ? rigidBody->GravityScale : defaults.GravityScale;
			// Jolt sensors only support discrete collision detection.
			if (rigidBody->ContinuousCollision && !rigidBody->IsTrigger)
				settings.mMotionQuality = JPH::EMotionQuality::LinearCast;

			if (type == RigidBodyType::Kinematic)
			{
				// Static triggers only detect moving bodies; kinematic triggers also detect static colliders.
				settings.mCollideKinematicVsNonDynamic = rigidBody->IsTrigger;
				return settings;
			}

			// The shapes provide the mass distribution, the component the total mass.
			settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
			settings.mMassPropertiesOverride.mMass = std::isfinite(rigidBody->Mass) ? std::max(rigidBody->Mass, c_MinimumMass) : defaults.Mass;

			JPH::EAllowedDOFs allowedDOFs = JPH::EAllowedDOFs::All;
			if (rigidBody->LockRotationX)
				allowedDOFs &= ~JPH::EAllowedDOFs::RotationX;
			if (rigidBody->LockRotationY)
				allowedDOFs &= ~JPH::EAllowedDOFs::RotationY;
			if (rigidBody->LockRotationZ)
				allowedDOFs &= ~JPH::EAllowedDOFs::RotationZ;
			settings.mAllowedDOFs = allowedDOFs;
			return settings;
		}

		//////////////////////////////////////////////////////////////////////////
		// Bodies of entities
		//////////////////////////////////////////////////////////////////////////

		// Records the meshes a body's colliders use, indexed so that changes of a mesh only check the bodies using it (see
		// RefreshMeshBodies).
		void SetMeshSources(PhysicsWorldData& data, entt::entity handle, BodyRecord& record, std::vector<MeshSource> sources)
		{
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
			DestroyJoltBody(data, record);
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
			data.Bodies.erase(it); // A PolledBodies entry is dropped by the next step
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
				if (data.Bodies.find(entity) != data.Bodies.end())
					DestroyRecord(data, entity); // Was a static body of its own
			}

			record.MergedEntities = std::move(merged);
		}

		// Marks a record as unable to have a body for now; the old body (if any) is destroyed and its contacts end. A dynamic
		// body keeps its motion for when it can be built again. Expects record.Type to be the type the body is built with.
		void SetBuildFailure(PhysicsWorldData& data, entt::entity handle, BodyRecord& record, BuildFailure failure, const glm::mat4& worldTransform)
		{
			SaveVelocities(data, record);
			if (record.Type != RigidBodyType::Dynamic)
			{
				record.SavedLinearVelocity = glm::vec3(0.0f);
				record.SavedAngularVelocity = glm::vec3(0.0f);
			}
			DestroyJoltBody(data, record);
			record.Failure = failure;
			record.LastWorldTransform = worldTransform;
			record.ShapeMembers.clear();
			EndContactsOf(data, record.EntityID);
			UpdatePolling(data, handle, record);
		}

		// (Re)builds the body of an owner entity from its components, its collider descendants and its world transform.
		void BuildBody(PhysicsWorldData& data, Entity entity)
		{
			ST_PROFILE_FUNCTION();
			data.BuildCount++;

			Scene& scene = *data.OwnerScene;
			const entt::entity handle = entity.GetHandle();
			const RigidBodyComponent* rigidBody = entity.TryGetComponent<RigidBodyComponent>();

			auto [recordIt, created] = data.Bodies.try_emplace(handle);
			BodyRecord& record = recordIt->second;
			if (created)
				record.EntityID = entity.GetUUID();

			RigidBodyType type = rigidBody ? rigidBody->Type : RigidBodyType::Static;
			if (type != RigidBodyType::Static && type != RigidBodyType::Dynamic && type != RigidBodyType::Kinematic)
			{
				if (ShouldWarn(data, record.EntityID, PhysicsWarning::InvalidType))
					ST_CORE_WARN("Physics: '{}' has an invalid rigid body type {}; it is treated as static", entity.GetName(), static_cast<int>(type));
				type = RigidBodyType::Static;
			}

			// Collider descendants only merge into rigid bodies; collider entities without one are static bodies of their own.
			UpdateMergedEntities(data, handle, record, rigidBody ? CollectMergedEntities(scene, entity, data.VisitStack) : std::vector<entt::entity>());

			const glm::mat4 worldTransform = scene.GetWorldTransform(entity);
			glm::vec3 position;
			glm::quat rotation;
			glm::vec3 scale;
			const bool decomposed = Math::DecomposeTransform(worldTransform, position, rotation, scale);
			if (!decomposed || (type == RigidBodyType::Dynamic && !HasInvertibleParent(scene, entity)))
			{
				if (ShouldWarn(data, record.EntityID, PhysicsWarning::DegenerateTransform))
				{
					if (!decomposed)
						ST_CORE_WARN("Physics: '{}' has a degenerate world transform (zero scale or non-finite values); it gets its body once the transform is valid", entity.GetName());
					else
						ST_CORE_WARN("Physics: the parent of '{}' is scaled to (nearly) zero, so its simulated pose cannot be written back; it gets its body once the parent's transform is valid", entity.GetName());
				}
				record.Type = type;
				SetBuildFailure(data, handle, record, BuildFailure::DegenerateTransform, worldTransform);
				record.WaitsForParent = decomposed; // The world transform may stay the same when the parent is fixed
				return;
			}
			record.WaitsForParent = false;

			ShapeBuild build;
			AddColliderParts(data, entity, glm::mat4(1.0f), glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), scale, type, build);

			// Merged colliders are placed with their transform relative to the body (position and rotation, without scale).
			const glm::mat4 bodyToWorld = Math::ComposeTransform(position, rotation, glm::vec3(1.0f));
			const glm::mat4 worldToBody = glm::inverse(bodyToWorld);
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
				AddColliderParts(data, merged, poseInBody, partTranslation, partRotation, partScale, type, build);
			}

			record.Type = type;
			SetMeshSources(data, handle, record, std::move(build.MeshSources));

			const JPH::RefConst<JPH::Shape> shape = CombineParts(data, entity, build.Parts);
			if (!shape)
			{
				BuildFailure failure = BuildFailure::InvalidColliders;
				if (build.WaitsForMesh)
				{
					failure = BuildFailure::MissingMesh;
				}
				else if (build.ColliderCount == 0)
				{
					failure = BuildFailure::NoCollider;
					if (rigidBody && ShouldWarn(data, record.EntityID, PhysicsWarning::MissingCollider))
						ST_CORE_WARN("Physics: '{}' has a Rigid Body but no collider; it is not simulated", entity.GetName());
				}
				else if (ShouldWarn(data, record.EntityID, PhysicsWarning::NoUsableCollider))
				{
					ST_CORE_WARN("Physics: '{}' has no usable collider; it is not simulated", entity.GetName());
				}
				SetBuildFailure(data, handle, record, failure, worldTransform);
				return;
			}

			JPH::BodyInterface& bodies = data.JoltSystem->GetBodyInterface();
			if (!record.HasBody() && data.JoltSystem->GetNumBodies() >= data.Settings.MaxBodies)
			{
				if (ShouldWarn(data, record.EntityID, PhysicsWarning::BodyLimit))
					ST_CORE_ERROR("Physics: cannot create a body for '{}': the world's limit of {} bodies is reached", entity.GetName(), data.Settings.MaxBodies);
				SetBuildFailure(data, handle, record, BuildFailure::BodyLimit, worldTransform);
				return;
			}

			// A dynamic body rebuilt because a property changed keeps moving as before; one that was out of the simulation (or
			// had no body after a failed build) resumes the motion it had.
			glm::vec3 linearVelocity(0.0f);
			glm::vec3 angularVelocity(0.0f);
			if (type == RigidBodyType::Dynamic)
			{
				if (record.HasBody() && record.InSimulation)
				{
					JPH::Vec3 currentLinear;
					JPH::Vec3 currentAngular;
					bodies.GetLinearAndAngularVelocity(record.BodyID, currentLinear, currentAngular);
					linearVelocity = ToGlm(currentLinear);
					angularVelocity = ToGlm(currentAngular);
				}
				else
				{
					linearVelocity = record.SavedLinearVelocity;
					angularVelocity = record.SavedAngularVelocity;
				}
			}

			const JPH::BodyCreationSettings settings = MakeBodySettings(data, entity, rigidBody, type, shape.GetPtr(), position, rotation);
			JPH::Body* body = bodies.CreateBody(settings);
			if (!body && record.HasBody())
			{
				// The world is full: free the old body's slot for its replacement instead of losing the body (its motion
				// was read above, and its contacts carry over like in any rebuild).
				DestroyJoltBody(data, record);
				body = bodies.CreateBody(settings);
			}
			if (!body)
			{
				if (ShouldWarn(data, record.EntityID, PhysicsWarning::BodyLimit))
					ST_CORE_ERROR("Physics: cannot create a body for '{}': the world's limit of {} bodies is reached", entity.GetName(), data.Settings.MaxBodies);
				record.SavedLinearVelocity = linearVelocity;
				record.SavedAngularVelocity = angularVelocity;
				SetBuildFailure(data, handle, record, BuildFailure::BodyLimit, worldTransform);
				return;
			}

			// Contacts are tracked per entity pair and carry over to the new body; the next step confirms or ends them.
			DestroyJoltBody(data, record);
			record.BodyID = body->GetID();
			record.Failure = BuildFailure::None;
			record.ShapeScale = scale;
			record.WorldScale = scale;
			record.LastWorldTransform = worldTransform;
			record.KinematicTargetPosition = position;
			record.KinematicTargetRotation = rotation;
			record.SavedLinearVelocity = type == RigidBodyType::Dynamic ? linearVelocity : glm::vec3(0.0f); // Applied when added
			record.SavedAngularVelocity = type == RigidBodyType::Dynamic ? angularVelocity : glm::vec3(0.0f);
			record.ShapeMembers = std::move(build.ShapeMembers);
			data.BodyEntities[record.BodyID.GetIndexAndSequenceNumber()] = handle;
			ClearWarnings(data, record.EntityID, c_TransientWarnings);

			if (!IsSimulated(scene, entity) || AddToSimulation(data, record, entity) != PlacementResult::Added)
				EndContactsOf(data, record.EntityID);
			else if (data.EntityPairs.find(record.EntityID) != data.EntityPairs.end())
				data.RecheckedEntities.push_back(record.EntityID); // Its contacts carried over: the next step confirms or ends them
			UpdatePolling(data, handle, record);
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

		void ApplyChanges(PhysicsWorldData& data)
		{
			ST_PROFILE_FUNCTION();

			Scene& scene = *data.OwnerScene;
			const auto hasChanges = [&]()
			{
				return !data.StructureChanges.IsEmpty() || !data.SubtreeChanges.IsEmpty() || !data.ActivityChanges.IsEmpty() || !data.TransformChanges.IsEmpty() || !data.OwnerRefreshes.IsEmpty();
			};

			for (uint32_t round = 0; round < c_MaxChangeRounds && hasChanges(); round++)
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
					if (data.Bodies.find(handle) != data.Bodies.end())
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

				for (entt::entity handle : data.OwnerRefreshes.TakeInto(data.ChangeScratch))
					RefreshOwner(data, handle);
			}

			if (hasChanges())
			{
				ST_CORE_ERROR("Physics: scene '{}' did not settle after {} rounds of changes; remaining changes are dropped", scene.GetName(), c_MaxChangeRounds);
				data.StructureChanges.Clear();
				data.SubtreeChanges.Clear();
				data.ActivityChanges.Clear();
				data.TransformChanges.Clear();
				data.OwnerRefreshes.Clear();
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

		//////////////////////////////////////////////////////////////////////////
		// Stepping
		//////////////////////////////////////////////////////////////////////////

		// The bodies Jolt simulates in the next step (awake dynamic and kinematic bodies). Between steps only the main thread
		// changes the list, so reading it without a lock is safe.
		std::span<const JPH::BodyID> GetAwakeBodies(const PhysicsWorldData& data)
		{
			const JPH::BodyID* bodies = data.JoltSystem->GetActiveBodiesUnsafe(JPH::EBodyType::RigidBody);
			return std::span<const JPH::BodyID>(bodies, data.JoltSystem->GetNumActiveBodies(JPH::EBodyType::RigidBody));
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

		// Brings one body in line with its entity before a step: kinematic target, teleport, degenerate transform, scale
		// change, or a retry of a body that could not be built. Each record is synchronized at most once per step.
		void SyncBody(PhysicsWorldData& data, entt::entity handle, float timestep)
		{
			Scene& scene = *data.OwnerScene;
			JPH::BodyInterface& bodies = data.JoltSystem->GetBodyInterface();
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
				if (IsSimulated(scene, entity))
				{
					switch (record->Failure)
					{
						case BuildFailure::DegenerateTransform:
							retry = scene.GetWorldTransform(entity) != record->LastWorldTransform || (record->WaitsForParent && HasInvertibleParent(scene, entity));
							break;
						case BuildFailure::BodyLimit: retry = data.JoltSystem->GetNumBodies() < data.Settings.MaxBodies; break;
						default: break;
					}
				}
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
					bodies.MoveKinematic(record->BodyID, ToJoltPosition(record->KinematicTargetPosition), ToJolt(record->KinematicTargetRotation), timestep);
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
			if (!decomposed || (record->Type == RigidBodyType::Dynamic && !HasInvertibleParent(scene, entity)))
			{
				// Out of the simulation (like an inactive entity) until the transform is valid again; the transform is
				// left as it is.
				if (ShouldWarn(data, record->EntityID, PhysicsWarning::DegenerateTransform))
				{
					if (!decomposed)
						ST_CORE_WARN("Physics: '{}' has a degenerate world transform; its body leaves the simulation until the transform is valid", entity.GetName());
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
					// Jolt only activates a body that is moved itself, so bodies resting on it at the old and the new place
					// are woken up explicitly.
					WakeBodiesAround(data, record->BodyID);
					bodies.SetPositionAndRotation(record->BodyID, ToJoltPosition(position), ToJolt(rotation), JPH::EActivation::DontActivate);
					WakeBodiesAround(data, record->BodyID);
					break;
				case RigidBodyType::Kinematic:
					// Moved with a velocity: the bodies it touches, also those resting on it, wake up through their contacts.
					bodies.MoveKinematic(record->BodyID, ToJoltPosition(position), ToJolt(rotation), timestep);
					record->KinematicMoving = true;
					record->KinematicTargetPosition = position;
					record->KinematicTargetRotation = rotation;
					break;
				case RigidBodyType::Dynamic:
					// Moved from outside physics (gameplay code, editor gizmo, a moving parent): teleport, keeping velocity,
					// and wake the bodies that rested on it.
					WakeBodiesAround(data, record->BodyID);
					bodies.SetPositionAndRotation(record->BodyID, ToJoltPosition(position), ToJolt(rotation), JPH::EActivation::Activate);
					break;
			}
			record->LastWorldTransform = worldTransform;
		}

		// Brings the bodies in line with their entities before a step. Only awake bodies (whose entities may have been moved
		// without a signal), the records that need attention (see NeedsPolling) and the bodies below entities that moved
		// without a signal are visited, so sleeping and static bodies cost nothing otherwise.
		void SyncEntitiesToBodies(PhysicsWorldData& data, float timestep)
		{
			ST_PROFILE_FUNCTION();

			RemovePendingDestroys(data);
			RefreshMeshBodies(data);

			std::vector<entt::entity>& candidates = data.SyncCandidates;
			candidates.clear();
			for (const JPH::BodyID& bodyID : GetAwakeBodies(data))
			{
				auto it = data.BodyEntities.find(bodyID.GetIndexAndSequenceNumber());
				if (it != data.BodyEntities.end())
					candidates.push_back(it->second);
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

			// Processed in a fixed order, whatever the order of Jolt's active list; then the bodies below entities that moved
			// without a signal, in the order they were found.
			std::sort(candidates.begin(), candidates.end());
			candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
			data.SyncPass++;
			data.LastSyncedBodies = 0;
			data.SyncFollowers.clear();
			for (entt::entity handle : candidates)
				SyncBody(data, handle, timestep);
			for (size_t index = 0; index < data.SyncFollowers.size(); index++)
				SyncBody(data, data.SyncFollowers[index], timestep);

			ApplyChanges(data);
		}

		struct ParticipantState
		{
			bool Simulated = false; // Has a body in the simulation
			bool Static = false;
			bool Active = false;
		};

		ParticipantState GetParticipantState(PhysicsWorldData& data, UUID entityID)
		{
			ParticipantState state;
			const Entity entity = data.OwnerScene->GetEntityByUUID(entityID);
			if (!entity.IsValid())
				return state;

			const BodyRecord* record = FindRecord(data, entity.GetHandle());
			if (!record || !record->InSimulation)
				return state;

			// Between steps only the main thread touches the bodies, so the locks can be skipped.
			state.Simulated = true;
			state.Static = record->Type == RigidBodyType::Static;
			state.Active = !state.Static && data.JoltSystem->GetBodyInterfaceNoLock().IsActive(record->BodyID);
			return state;
		}

		// Whether a touching pair must end unless the next step reports it: a body is awake (Jolt reports the contacts of
		// awake bodies), or the pair can never be reported again (a body left the simulation, or both bodies are static).
		bool MustBeReported(PhysicsWorldData& data, const TouchingPair& pair)
		{
			const ParticipantState a = GetParticipantState(data, pair.A);
			const ParticipantState b = GetParticipantState(data, pair.B);
			return !a.Simulated || !b.Simulated || a.Active || b.Active || (a.Static && b.Static);
		}

		// Collects the touching pairs that end unless the step reports them: those of awake bodies, and those of bodies built
		// since the last step whose contacts carried over (see MustBeReported). Pairs of sleeping bodies stay touching until a
		// body wakes up, and are not visited at all.
		void CollectCheckedPairs(PhysicsWorldData& data)
		{
			ST_PROFILE_FUNCTION();

			data.CheckedPairs.clear();
			const uint64_t step = data.StepCount;
			const auto checkPairsOf = [&](UUID entityID, bool awake)
			{
				auto indexIt = data.EntityPairs.find(entityID);
				if (indexIt == data.EntityPairs.end())
					return;
				for (const PairKey& key : indexIt->second)
				{
					auto pairIt = data.TouchingPairs.find(key);
					if (pairIt == data.TouchingPairs.end() || pairIt->second.CheckStep == step)
						continue;
					if (!awake && !MustBeReported(data, pairIt->second))
						continue;
					pairIt->second.CheckStep = step;
					data.CheckedPairs.push_back(key);
				}
			};

			const JPH::BodyInterface& bodies = data.JoltSystem->GetBodyInterfaceNoLock();
			for (const JPH::BodyID& bodyID : GetAwakeBodies(data))
				checkPairsOf(UUID(bodies.GetUserData(bodyID)), true);
			for (UUID entityID : data.RecheckedEntities)
				checkPairsOf(entityID, false);
			data.RecheckedEntities.clear();
			data.LastCheckedPairs = static_cast<uint32_t>(data.CheckedPairs.size());
		}

		// Turns the contacts reported during the step into events: reported pairs that were not touching begin, checked pairs
		// (see CollectCheckedPairs) that were not reported end.
		void ProcessContacts(PhysicsWorldData& data)
		{
			ST_PROFILE_FUNCTION();

			const uint64_t step = data.StepCount;
			std::vector<ContactReport>& reports = data.Reports;
			data.Contacts.SwapReports(reports);
			// Several sub shape pairs of an entity pair may touch (and several collision steps report them again); the first
			// report of each pair in this order, which does not depend on thread timing, describes the contact.
			std::sort(reports.begin(), reports.end(), IsReportOrderedBefore);

			const auto leftAfterStep = [&](UUID entityID) { return std::find(data.LeftAfterStep.begin(), data.LeftAfterStep.end(), entityID) != data.LeftAfterStep.end(); };
			std::vector<TouchingPair>& begun = data.BegunPairs;
			begun.clear();
			for (size_t index = 0; index < reports.size(); index++)
			{
				const ContactReport& report = reports[index];
				if (index > 0 && reports[index - 1].Pair == report.Pair)
					continue;
				// The contacts of a body that left the simulation after the step ended already.
				if (!data.LeftAfterStep.empty() && (leftAfterStep(report.EntityA) || leftAfterStep(report.EntityB)))
					continue;

				auto it = data.TouchingPairs.find(report.Pair);
				if (it == data.TouchingPairs.end())
				{
					TouchingPair pair;
					pair.A = report.EntityA;
					pair.B = report.EntityB;
					pair.IsTrigger = report.IsTrigger;
					pair.ReportStep = step;
					pair.SortKey = report.SortKey;
					pair.Point = report.Point;
					pair.Normal = report.Normal;
					AddTouchingPair(data, report.Pair, pair);
					begun.push_back(pair);
				}
				else
				{
					// Keep the pair's orientation from when it began (body IDs, and thus Jolt's order, change on rebuilds).
					TouchingPair& pair = it->second;
					pair.IsTrigger = report.IsTrigger;
					pair.ReportStep = step;
					pair.Point = report.Point;
					pair.Normal = report.EntityA == pair.A ? report.Normal : -report.Normal;
				}
			}

			std::vector<TouchingPair>& ended = data.EndedPairs;
			ended.clear();
			for (const PairKey& key : data.CheckedPairs)
			{
				auto it = data.TouchingPairs.find(key);
				if (it == data.TouchingPairs.end() || it->second.ReportStep == step)
					continue;
				ended.push_back(it->second);
				RemovePairFromIndex(data, it->second.A, key);
				RemovePairFromIndex(data, it->second.B, key);
				data.TouchingPairs.erase(it);
			}

			const auto bySortKey = [](const TouchingPair& a, const TouchingPair& b) { return a.SortKey < b.SortKey; };
			std::sort(ended.begin(), ended.end(), bySortKey);
			std::sort(begun.begin(), begun.end(), bySortKey);
			for (const TouchingPair& pair : ended)
				QueueEvent(data, CollisionEventType::End, pair);
			for (const TouchingPair& pair : begun)
				QueueEvent(data, CollisionEventType::Begin, pair);
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

		// Writes the poses of the dynamic bodies that moved (awake, or fell asleep during this step) to their entities, parents
		// before children so that every child's local transform is computed against its parent's final pose.
		void WriteBackDynamicBodies(PhysicsWorldData& data)
		{
			ST_PROFILE_FUNCTION();

			Scene& scene = *data.OwnerScene;
			const JPH::BodyInterface& bodies = data.JoltSystem->GetBodyInterfaceNoLock();
			std::vector<JPH::BodyID>& movedBodies = data.MovedBodies;
			const std::span<const JPH::BodyID> awake = GetAwakeBodies(data);
			movedBodies.assign(awake.begin(), awake.end());
			data.Activations.SwapDeactivated(data.DeactivatedBodies);
			movedBodies.insert(movedBodies.end(), data.DeactivatedBodies.begin(), data.DeactivatedBodies.end());

			std::vector<std::pair<uint32_t, entt::entity>>& writeBacks = data.WriteBacks;
			writeBacks.clear();
			for (const JPH::BodyID& bodyID : movedBodies)
			{
				auto entityIt = data.BodyEntities.find(bodyID.GetIndexAndSequenceNumber());
				if (entityIt == data.BodyEntities.end())
					continue;
				const BodyRecord* record = FindRecord(data, entityIt->second);
				if (!record || record->Type != RigidBodyType::Dynamic || !record->InSimulation)
					continue;
				writeBacks.emplace_back(GetHierarchyDepth(Entity(entityIt->second, &scene)), entityIt->second);
			}
			// By depth, then by entity: deterministic, parents first.
			std::sort(writeBacks.begin(), writeBacks.end());
			writeBacks.erase(std::unique(writeBacks.begin(), writeBacks.end()), writeBacks.end());
			data.LastWrittenBodies = static_cast<uint32_t>(writeBacks.size());

			data.WriteBackPass++;
			for (const auto& [depth, handle] : writeBacks)
				FindRecord(data, handle)->WriteBackPass = data.WriteBackPass;

			for (const auto& [depth, handle] : writeBacks)
			{
				BodyRecord& record = *FindRecord(data, handle); // Filtered above
				const Entity entity(handle, &scene);

				JPH::RVec3 bodyPosition;
				JPH::Quat bodyRotation;
				bodies.GetPositionAndRotation(record.BodyID, bodyPosition, bodyRotation);
				const glm::vec3 position = ToGlm(bodyPosition);
				const glm::quat rotation = ToGlm(bodyRotation);
				if (!IsFinite(position) || !IsFinite(rotation))
				{
					if (ShouldWarn(data, record.EntityID, PhysicsWarning::NonFiniteState))
						ST_CORE_ERROR("Physics: the simulation produced a non-finite pose for '{}'; its transform is not updated", entity.GetName());
					continue;
				}

				// The entity keeps its own scale: the pose is composed with its world scale as of the last synchronization (the
				// shape's within tolerance), or its current one if it was edited without a signal while the body slept, in which
				// case the shape is rebuilt before the next step.
				glm::vec3 worldScale = record.WorldScale;
				if (scene.GetWorldTransform(entity) != record.LastWorldTransform)
				{
					worldScale = GetWorldScale(scene, entity, record.WorldScale);
					if (HasScaleChanged(worldScale, record.ShapeScale))
						data.OwnerRefreshes.Add(handle);
				}

				if (!WriteWorldTransform(scene, entity, Math::ComposeTransform(position, glm::normalize(rotation), worldScale)))
				{
					// The parent cannot be inverted (the sync before the step notices this only when the transform changed):
					// the body goes back to its entity's pose and leaves the simulation until the parent is valid again.
					if (ShouldWarn(data, record.EntityID, PhysicsWarning::DegenerateTransform))
						ST_CORE_WARN("Physics: the parent of '{}' is scaled to (nearly) zero, so its simulated pose cannot be written back; its body leaves the simulation until the parent's transform is valid", entity.GetName());
					glm::vec3 entityPosition;
					glm::quat entityRotation;
					glm::vec3 entityScale;
					if (Math::DecomposeTransform(record.LastWorldTransform, entityPosition, entityRotation, entityScale))
						data.JoltSystem->GetBodyInterface().SetPositionAndRotation(record.BodyID, ToJoltPosition(entityPosition), ToJolt(entityRotation), JPH::EActivation::DontActivate);
					RemoveFromSimulation(data, record);
					record.Suspended = true;
					UpdatePolling(data, handle, record);
					data.LeftAfterStep.push_back(record.EntityID);
					continue;
				}
				record.LastWorldTransform = scene.GetWorldTransform(entity);
				record.WorldScale = worldScale;
				UpdateDescendantsOfMovedBody(data, entity, true);
			}
		}

		void ReportUpdateErrors(PhysicsWorldData& data, JPH::EPhysicsUpdateError errors)
		{
			const auto reportOnce = [&](JPH::EPhysicsUpdateError error, const char* message)
			{
				if ((errors & error) == JPH::EPhysicsUpdateError::None || (data.ReportedErrors & error) != JPH::EPhysicsUpdateError::None)
					return;
				data.ReportedErrors = data.ReportedErrors | error;
				ST_CORE_WARN("Physics: {}; contacts were dropped (reported once per world)", message);
			};

			reportOnce(JPH::EPhysicsUpdateError::ManifoldCacheFull, "the contact manifold cache is full, raise PhysicsSettings::MaxContactConstraints");
			reportOnce(JPH::EPhysicsUpdateError::BodyPairCacheFull, "the body pair cache is full, raise PhysicsSettings::MaxBodyPairs");
			reportOnce(JPH::EPhysicsUpdateError::ContactConstraintsFull, "the contact constraint buffer is full, raise PhysicsSettings::MaxContactConstraints");
		}

		//////////////////////////////////////////////////////////////////////////
		// Queries
		//////////////////////////////////////////////////////////////////////////

		// Query filter skipping an ignored body, triggers (unless requested) and bodies of entities that are destroyed or about
		// to be. Queries run on the main thread, so reading the scene is safe.
		class QueryBodyFilter final : public JPH::BodyFilter
		{
		public:
			QueryBodyFilter(const Scene& scene, const JPH::BodyID& ignoredBody, bool includeTriggers)
				: m_Scene(scene), m_IgnoredBody(ignoredBody), m_IncludeTriggers(includeTriggers)
			{
			}

			bool ShouldCollide(const JPH::BodyID& bodyID) const override
			{
				return bodyID != m_IgnoredBody;
			}

			bool ShouldCollideLocked(const JPH::Body& body) const override
			{
				if (!m_IncludeTriggers && body.IsSensor())
					return false;
				const Entity entity = m_Scene.GetEntityByUUID(UUID(body.GetUserData()));
				return entity.IsValid() && !IsPendingDestroyInHierarchy(m_Scene, entity);
			}
		private:
			const Scene& m_Scene;
			JPH::BodyID m_IgnoredBody;
			bool m_IncludeTriggers;
		};

		bool MakeRay(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, JPH::RRayCast& outRay, float& outLength)
		{
			if (!IsFinite(origin) || !IsFinite(direction) || std::isnan(maxDistance) || !(maxDistance > 0.0f))
				return false;

			const float directionLength = glm::length(direction);
			if (!(directionLength > 1.0e-12f) || !std::isfinite(directionLength))
				return false;

			outLength = std::min(maxDistance, c_MaxRaycastDistance);
			outRay = JPH::RRayCast(ToJoltPosition(origin), ToJolt(direction / directionLength * outLength));
			return true;
		}

		Entity GetBodyEntityOf(const PhysicsWorldData& data, Entity entity)
		{
			if (!entity.IsValid() || entity.GetScene() != data.OwnerScene)
				return {};
			if (data.Bodies.find(entity.GetHandle()) != data.Bodies.end())
				return entity;
			auto merged = data.MergedOwners.find(entity.GetHandle());
			return merged != data.MergedOwners.end() ? Entity(merged->second, data.OwnerScene) : Entity();
		}

		JPH::BodyID GetIgnoredBody(const PhysicsWorldData& data, Entity ignoreEntity)
		{
			const Entity owner = GetBodyEntityOf(data, ignoreEntity);
			const BodyRecord* record = owner.IsValid() ? FindRecord(data, owner.GetHandle()) : nullptr;
			return record ? record->BodyID : JPH::BodyID();
		}

		std::optional<RaycastHit> MakeRaycastHit(const PhysicsWorldData& data, const JPH::RRayCast& ray, float length, const JPH::RayCastResult& result)
		{
			JPH::BodyLockRead lock(data.JoltSystem->GetBodyLockInterface(), result.mBodyID);
			if (!lock.Succeeded())
				return std::nullopt;

			const JPH::Body& body = lock.GetBody();
			const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);

			RaycastHit hit;
			hit.EntityID = UUID(body.GetUserData());
			hit.HitEntity = data.OwnerScene->GetEntityByUUID(hit.EntityID);
			hit.Point = ToGlm(point);
			hit.Normal = ToGlm(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
			hit.Distance = result.mFraction * length;
			if (!hit.HitEntity.IsValid() || !IsFinite(hit.Point) || !IsFinite(hit.Normal))
				return std::nullopt;
			return hit;
		}

		std::vector<Entity> CollectOverlaps(const PhysicsWorldData& data, const JPH::Shape& shape, const glm::vec3& center, const glm::quat& rotation, uint32_t layerMask, bool includeTriggers)
		{
			JPH::ClosestHitPerBodyCollisionCollector<JPH::CollideShapeCollector> collector;
			const JPH::CollideShapeSettings settings;
			const JPH::BroadPhaseLayerFilter broadPhaseFilter;
			const LayerMaskFilter layerFilter(layerMask);
			const QueryBodyFilter bodyFilter(*data.OwnerScene, JPH::BodyID(), includeTriggers);
			const JPH::RMat44 transform = JPH::RMat44::sRotationTranslation(ToJolt(rotation), ToJoltPosition(center));
			data.JoltSystem->GetNarrowPhaseQuery().CollideShape(&shape, JPH::Vec3::sOne(), transform, settings, ToJoltPosition(center), collector, broadPhaseFilter, layerFilter, bodyFilter);

			std::vector<JPH::BodyID> bodies;
			bodies.reserve(collector.mHits.size());
			for (const JPH::CollideShapeResult& hit : collector.mHits)
				bodies.push_back(hit.mBodyID2);
			std::sort(bodies.begin(), bodies.end());
			bodies.erase(std::unique(bodies.begin(), bodies.end()), bodies.end());

			std::vector<Entity> entities;
			entities.reserve(bodies.size());
			const JPH::BodyInterface& bodyInterface = data.JoltSystem->GetBodyInterface();
			for (const JPH::BodyID& bodyID : bodies)
			{
				if (const Entity entity = data.OwnerScene->GetEntityByUUID(UUID(bodyInterface.GetUserData(bodyID))))
					entities.push_back(entity);
			}
			return entities;
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// PhysicsWorld
	////////////////////////////////////////////////////////////////////////////////

	PhysicsWorld::PhysicsWorld(Scene& scene, const PhysicsSettings& settings)
		: m_Data(CreateScope<PhysicsWorldData>(scene, SanitizeSettings(settings)))
	{
		ST_PROFILE_FUNCTION();

		PhysicsWorldData& data = *m_Data;
		data.Allocator = CreateScope<JPH::TempAllocatorImplWithMallocFallback>(data.Settings.TempAllocatorSize);
		data.Jobs = CreatePhysicsJobSystem(&data.JobCounters);
		data.JoltSystem = CreateScope<JPH::PhysicsSystem>();
		data.JoltSystem->Init(data.Settings.MaxBodies, 0, data.Settings.MaxBodyPairs, data.Settings.MaxContactConstraints, data.BroadPhaseLayers, data.ObjectVsBroadPhase, data.LayerPairFilter);
		data.JoltSystem->SetContactListener(&data.Contacts);
		data.JoltSystem->SetBodyActivationListener(&data.Activations);

		const glm::vec3 gravity = scene.GetSettings().Gravity;
		if (IsFinite(gravity))
		{
			data.JoltSystem->SetGravity(ToJolt(gravity));
		}
		else
		{
			ST_CORE_WARN("Physics: scene '{}' has a non-finite gravity; the default is used", scene.GetName());
			data.JoltSystem->SetGravity(ToJolt(SceneSettings().Gravity));
		}

		ConnectSignals(data);
		RememberMeshState(data); // Before the bodies read mesh data: later changes are noticed against it
		for (const Entity entity : scene.GetEntitiesInHierarchyOrder())
		{
			if (HasPhysicsComponent(entity))
				data.StructureChanges.Add(entity.GetHandle());
		}
		ApplyChanges(data);

		// Bodies were inserted one by one; rebuilding the broad phase trees once makes the first queries and steps fast.
		data.JoltSystem->OptimizeBroadPhase();
	}

	PhysicsWorld::~PhysicsWorld()
	{
		m_Data->Connections.clear();
		DestroyAllBodies(*m_Data);
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

	void PhysicsWorld::ApplyPendingChanges()
	{
		ApplyChanges(*m_Data);
	}

	bool PhysicsWorld::HasBody(Entity entity) const
	{
		return FindSimulatedRecord(*m_Data, entity) != nullptr;
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
		const auto startTime = std::chrono::steady_clock::now();
		ApplyChanges(data);
		data.HeldLastStep = HoldForMeshes(data, timestep);
		if (data.HeldLastStep)
		{
			data.HeldStepCount++;
			data.LastStepTime = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - startTime).count();
			return;
		}

		data.StepCount++; // Identifies this step in the contact pairs' check and report marks
		SyncEntitiesToBodies(data, timestep);

		CollectCheckedPairs(data);
		data.Activations.Clear();
		{
			ST_PROFILE_SCOPE("PhysicsWorld::Simulate - Jolt");
			const JPH::EPhysicsUpdateError errors = data.JoltSystem->Update(timestep, static_cast<int>(data.Settings.CollisionSteps), data.Allocator.get(), data.Jobs.get());
			if (errors != JPH::EPhysicsUpdateError::None)
				ReportUpdateErrors(data, errors);
		}

		data.LeftAfterStep.clear();
		WriteBackDynamicBodies(data);
		ProcessContacts(data);

		data.LastStepTime = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - startTime).count();
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

		m_Data->JoltSystem->SetGravity(ToJolt(gravity));
		WakeAllBodies(*m_Data);
		return true;
	}

	glm::vec3 PhysicsWorld::GetGravity() const
	{
		return ToGlm(m_Data->JoltSystem->GetGravity());
	}

	void PhysicsWorld::SetLayersCollide(uint32_t layerA, uint32_t layerB, bool collide)
	{
		if (layerA >= c_PhysicsLayerCount || layerB >= c_PhysicsLayerCount)
		{
			ST_CORE_WARN("Physics: cannot change the collision of layers {} and {}: layers range from 0 to {}", layerA, layerB, c_PhysicsLayerCount - 1);
			return;
		}

		m_Data->Settings.SetLayersCollide(layerA, layerB, collide);
		WakeAllBodies(*m_Data);
	}

	bool PhysicsWorld::DoLayersCollide(uint32_t layerA, uint32_t layerB) const
	{
		return m_Data->Settings.DoLayersCollide(layerA, layerB);
	}

	glm::vec3 PhysicsWorld::GetLinearVelocity(Entity entity) const
	{
		const BodyRecord* record = FindSimulatedRecord(*m_Data, entity);
		if (!record || record->Type == RigidBodyType::Static)
			return glm::vec3(0.0f);
		return ToGlm(m_Data->JoltSystem->GetBodyInterface().GetLinearVelocity(record->BodyID));
	}

	bool PhysicsWorld::SetLinearVelocity(Entity entity, const glm::vec3& velocity)
	{
		const BodyRecord* record = FindDynamicRecord(*m_Data, entity);
		if (!record || !IsFinite(velocity))
			return false;

		JPH::BodyInterface& bodies = m_Data->JoltSystem->GetBodyInterface();
		bodies.SetLinearVelocity(record->BodyID, ToJolt(velocity));
		bodies.ActivateBody(record->BodyID);
		return true;
	}

	glm::vec3 PhysicsWorld::GetAngularVelocity(Entity entity) const
	{
		const BodyRecord* record = FindSimulatedRecord(*m_Data, entity);
		if (!record || record->Type == RigidBodyType::Static)
			return glm::vec3(0.0f);
		return ToGlm(m_Data->JoltSystem->GetBodyInterface().GetAngularVelocity(record->BodyID));
	}

	bool PhysicsWorld::SetAngularVelocity(Entity entity, const glm::vec3& velocity)
	{
		const BodyRecord* record = FindDynamicRecord(*m_Data, entity);
		if (!record || !IsFinite(velocity))
			return false;

		JPH::BodyInterface& bodies = m_Data->JoltSystem->GetBodyInterface();
		bodies.SetAngularVelocity(record->BodyID, ToJolt(velocity));
		bodies.ActivateBody(record->BodyID);
		return true;
	}

	bool PhysicsWorld::AddForce(Entity entity, const glm::vec3& force)
	{
		const BodyRecord* record = FindDynamicRecord(*m_Data, entity);
		if (!record || !IsFinite(force))
			return false;

		m_Data->JoltSystem->GetBodyInterface().AddForce(record->BodyID, ToJolt(force), JPH::EActivation::Activate);
		return true;
	}

	bool PhysicsWorld::AddForceAtPosition(Entity entity, const glm::vec3& force, const glm::vec3& worldPosition)
	{
		const BodyRecord* record = FindDynamicRecord(*m_Data, entity);
		if (!record || !IsFinite(force) || !IsFinite(worldPosition))
			return false;

		m_Data->JoltSystem->GetBodyInterface().AddForce(record->BodyID, ToJolt(force), ToJoltPosition(worldPosition), JPH::EActivation::Activate);
		return true;
	}

	bool PhysicsWorld::AddImpulse(Entity entity, const glm::vec3& impulse)
	{
		const BodyRecord* record = FindDynamicRecord(*m_Data, entity);
		if (!record || !IsFinite(impulse))
			return false;

		m_Data->JoltSystem->GetBodyInterface().AddImpulse(record->BodyID, ToJolt(impulse));
		return true;
	}

	bool PhysicsWorld::AddImpulseAtPosition(Entity entity, const glm::vec3& impulse, const glm::vec3& worldPosition)
	{
		const BodyRecord* record = FindDynamicRecord(*m_Data, entity);
		if (!record || !IsFinite(impulse) || !IsFinite(worldPosition))
			return false;

		m_Data->JoltSystem->GetBodyInterface().AddImpulse(record->BodyID, ToJolt(impulse), ToJoltPosition(worldPosition));
		return true;
	}

	bool PhysicsWorld::AddTorque(Entity entity, const glm::vec3& torque)
	{
		const BodyRecord* record = FindDynamicRecord(*m_Data, entity);
		if (!record || !IsFinite(torque))
			return false;

		m_Data->JoltSystem->GetBodyInterface().AddTorque(record->BodyID, ToJolt(torque), JPH::EActivation::Activate);
		return true;
	}

	bool PhysicsWorld::AddAngularImpulse(Entity entity, const glm::vec3& impulse)
	{
		const BodyRecord* record = FindDynamicRecord(*m_Data, entity);
		if (!record || !IsFinite(impulse))
			return false;

		m_Data->JoltSystem->GetBodyInterface().AddAngularImpulse(record->BodyID, ToJolt(impulse));
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
			JPH::BodyInterface& bodies = m_Data->JoltSystem->GetBodyInterface();
			bodies.SetGravityFactor(record->BodyID, gravityScale);
			bodies.ActivateBody(record->BodyID);
		}
		return true;
	}

	bool PhysicsWorld::IsSleeping(Entity entity) const
	{
		const BodyRecord* record = FindSimulatedRecord(*m_Data, entity);
		if (!record || record->Type == RigidBodyType::Static)
			return false;
		return !m_Data->JoltSystem->GetBodyInterface().IsActive(record->BodyID);
	}

	bool PhysicsWorld::WakeUp(Entity entity)
	{
		const BodyRecord* record = FindSimulatedRecord(*m_Data, entity);
		if (!record || record->Type == RigidBodyType::Static)
			return false;

		m_Data->JoltSystem->GetBodyInterface().ActivateBody(record->BodyID);
		return true;
	}

	bool PhysicsWorld::Teleport(Entity entity, const glm::vec3& position, const glm::quat& rotation)
	{
		PhysicsWorldData& data = *m_Data;
		BodyRecord* record = FindSimulatedRecord(data, entity);
		if (!record || !IsFinite(position) || !IsFinite(rotation))
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

		// Bodies resting on it must notice that it left: Jolt only activates the moved body itself.
		WakeBodiesAround(data, record->BodyID);
		JPH::BodyInterface& bodies = data.JoltSystem->GetBodyInterface();
		if (record->Type == RigidBodyType::Static)
		{
			bodies.SetPositionAndRotation(record->BodyID, ToJoltPosition(position), ToJolt(normalizedRotation), JPH::EActivation::DontActivate);
			WakeBodiesAround(data, record->BodyID);
		}
		else
		{
			if (record->Type == RigidBodyType::Kinematic)
				bodies.SetLinearAndAngularVelocity(record->BodyID, JPH::Vec3::sZero(), JPH::Vec3::sZero());
			bodies.SetPositionAndRotation(record->BodyID, ToJoltPosition(position), ToJolt(normalizedRotation), JPH::EActivation::Activate);
		}

		// The entity's descendants moved with it.
		UpdateDescendantsOfMovedBody(data, entity, false);
		return true;
	}

	std::optional<RaycastHit> PhysicsWorld::Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t layerMask, Entity ignoreEntity, bool includeTriggers) const
	{
		ST_PROFILE_FUNCTION();

		const PhysicsWorldData& data = *m_Data;
		JPH::RRayCast ray;
		float length = 0.0f;
		if (!MakeRay(origin, direction, maxDistance, ray, length))
			return std::nullopt;

		JPH::RayCastSettings settings;
		settings.mTreatConvexAsSolid = false; // A ray starting inside a convex collider passes out of it without a hit
		JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
		const JPH::BroadPhaseLayerFilter broadPhaseFilter;
		const LayerMaskFilter layerFilter(layerMask);
		const QueryBodyFilter bodyFilter(*data.OwnerScene, GetIgnoredBody(data, ignoreEntity), includeTriggers);
		data.JoltSystem->GetNarrowPhaseQuery().CastRay(ray, settings, collector, broadPhaseFilter, layerFilter, bodyFilter);
		if (!collector.HadHit())
			return std::nullopt;
		return MakeRaycastHit(data, ray, length, collector.mHit);
	}

	std::vector<RaycastHit> PhysicsWorld::RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t layerMask, Entity ignoreEntity, bool includeTriggers) const
	{
		ST_PROFILE_FUNCTION();

		const PhysicsWorldData& data = *m_Data;
		std::vector<RaycastHit> hits;
		JPH::RRayCast ray;
		float length = 0.0f;
		if (!MakeRay(origin, direction, maxDistance, ray, length))
			return hits;

		JPH::RayCastSettings settings;
		settings.mTreatConvexAsSolid = false;
		JPH::ClosestHitPerBodyCollisionCollector<JPH::CastRayCollector> collector;
		const JPH::BroadPhaseLayerFilter broadPhaseFilter;
		const LayerMaskFilter layerFilter(layerMask);
		const QueryBodyFilter bodyFilter(*data.OwnerScene, GetIgnoredBody(data, ignoreEntity), includeTriggers);
		data.JoltSystem->GetNarrowPhaseQuery().CastRay(ray, settings, collector, broadPhaseFilter, layerFilter, bodyFilter);

		std::vector<JPH::RayCastResult> results(collector.mHits.begin(), collector.mHits.end());
		std::sort(results.begin(), results.end(), [](const JPH::RayCastResult& a, const JPH::RayCastResult& b)
		{
			return a.mFraction != b.mFraction ? a.mFraction < b.mFraction : a.mBodyID < b.mBodyID;
		});

		hits.reserve(results.size());
		for (const JPH::RayCastResult& result : results)
		{
			if (std::optional<RaycastHit> hit = MakeRaycastHit(data, ray, length, result))
				hits.push_back(*hit);
		}
		return hits;
	}

	std::vector<Entity> PhysicsWorld::OverlapSphere(const glm::vec3& center, float radius, uint32_t layerMask, bool includeTriggers) const
	{
		ST_PROFILE_FUNCTION();

		if (!IsFinite(center) || !std::isfinite(radius) || !(radius > 0.0f))
			return {};

		JPH::SphereShape sphere(radius);
		sphere.SetEmbedded(); // Stack object: never deleted through its reference count
		return CollectOverlaps(*m_Data, sphere, center, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), layerMask, includeTriggers);
	}

	std::vector<Entity> PhysicsWorld::OverlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, uint32_t layerMask, bool includeTriggers) const
	{
		ST_PROFILE_FUNCTION();

		if (!IsFinite(center) || !IsFinite(halfExtents) || !IsFinite(rotation) || glm::any(glm::lessThanEqual(halfExtents, glm::vec3(0.0f))))
			return {};

		const float rotationLength = glm::length(rotation);
		if (!(rotationLength > 1.0e-6f))
			return {};

		const float convexRadius = std::min(JPH::cDefaultConvexRadius, std::min({ halfExtents.x, halfExtents.y, halfExtents.z }));
		JPH::BoxShape box(ToJolt(halfExtents), convexRadius);
		box.SetEmbedded();
		return CollectOverlaps(*m_Data, box, center, rotation / rotationLength, layerMask, includeTriggers);
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
		stats.ActiveBodyCount = data.JoltSystem->GetNumActiveBodies(JPH::EBodyType::RigidBody);
		stats.ContactPairCount = static_cast<uint32_t>(data.TouchingPairs.size());
		stats.SyncedBodyCount = data.LastSyncedBodies;
		stats.CheckedPairCount = data.LastCheckedPairs;
		stats.WrittenBodyCount = data.LastWrittenBodies;
		stats.BuildCount = data.BuildCount;
		stats.MeshCheckCount = data.MeshCheckCount;
		stats.WaitingForMeshes = data.HeldLastStep;
		stats.HeldStepCount = data.HeldStepCount;
		stats.StepCount = data.StepCount;
		stats.JobCount = data.JobCounters.Jobs.load(std::memory_order_relaxed);
		stats.WorkerJobCount = data.JobCounters.WorkerJobs.load(std::memory_order_relaxed);
		stats.LastStepTime = data.LastStepTime;
		return stats;
	}

}
