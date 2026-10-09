#include "stpch.h"
#include "Strata/Physics/PhysicsWorld.h"

#include "Strata/Core/Hash.h"
#include "Strata/Math/Math.h"
#include "Strata/Physics/PhysicsJobSystem.h"
#include "Strata/Physics/PhysicsRuntime.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystem.h>
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
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <map>

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
		// Relative change of an entity's world scale that rebuilds its body's shape.
		constexpr float c_ScaleChangeTolerance = 1.0e-4f;
		constexpr float c_MaxRaycastDistance = 1.0e5f;
		// Bodies within this distance of a body that is removed or teleported are woken up, so that nothing keeps sleeping
		// on top of a body that is no longer there.
		constexpr float c_WakeMargin = 0.1f;
		constexpr float c_MinimumMass = 0.001f;

		enum class PhysicsWarning : uint8_t
		{
			MissingCollider = 1,
			InvalidLayer,
			DegenerateTransform,
			NonFiniteState
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

		// Query filter skipping an ignored body, triggers (unless requested) and bodies whose entity was destroyed but whose
		// destruction the world has not processed yet. Queries run on the main thread, so reading the scene is safe.
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
				return m_Scene.GetEntityByUUID(UUID(body.GetUserData())).IsValid();
			}
		private:
			const Scene& m_Scene;
			JPH::BodyID m_IgnoredBody;
			bool m_IncludeTriggers;
		};

		//////////////////////////////////////////////////////////////////////////
		// Listeners (called from Jolt's worker threads during a step)
		//////////////////////////////////////////////////////////////////////////

		struct ContactReport
		{
			UUID EntityA = UUID::Null(); // Entity of Jolt's body 1 (the body with the lower ID)
			UUID EntityB = UUID::Null();
			uint64_t SortKey = 0;        // Both body IDs; orders events independently of thread timing
			uint64_t SubShapeKey = 0;    // Both sub shape IDs; picks a deterministic manifold among several per pair
			glm::vec3 Point = glm::vec3(0.0f);
			glm::vec3 Normal = glm::vec3(0.0f);
			bool IsTrigger = false;
		};

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

			std::vector<ContactReport> TakeReports()
			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				std::vector<ContactReport> reports = std::move(m_Reports);
				m_Reports.clear();
				return reports;
			}
		private:
			void Record(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold)
			{
				ContactReport report;
				report.EntityA = UUID(body1.GetUserData());
				report.EntityB = UUID(body2.GetUserData());
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

			std::vector<JPH::BodyID> Take()
			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				std::vector<JPH::BodyID> bodies = std::move(m_Deactivated);
				m_Deactivated.clear();
				return bodies;
			}
		private:
			std::mutex m_Mutex;
			std::vector<JPH::BodyID> m_Deactivated;
		};

		//////////////////////////////////////////////////////////////////////////
		// World state
		//////////////////////////////////////////////////////////////////////////

		struct BodyRecord
		{
			UUID EntityID = UUID::Null();
			JPH::BodyID BodyID;
			RigidBodyType Type = RigidBodyType::Static;
			bool InSimulation = false;
			bool KinematicMoving = false;                   // MoveKinematic gave the body a velocity during the last step
			glm::vec3 ShapeScale = glm::vec3(1.0f);         // World scale baked into the shape
			glm::mat4 LastWorldTransform = glm::mat4(1.0f); // Entity world transform the body was last synchronized with
			glm::vec3 KinematicTargetPosition = glm::vec3(0.0f);
			glm::quat KinematicTargetRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			// Velocities of a dynamic body while it is out of the simulation (Jolt clears them on removal).
			glm::vec3 SavedLinearVelocity = glm::vec3(0.0f);
			glm::vec3 SavedAngularVelocity = glm::vec3(0.0f);
		};

		// Contacts are tracked per pair of entities (not bodies), so that rebuilding a body keeps its contacts.
		struct PairKey
		{
			uint64_t Low = 0;
			uint64_t High = 0;

			bool operator==(const PairKey& other) const { return Low == other.Low && High == other.High; }
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
			bool Awake = true; // At least one body could report the contact during the current step
			uint64_t SortKey = 0;
			glm::vec3 Point = glm::vec3(0.0f);
			glm::vec3 Normal = glm::vec3(0.0f); // From A towards B
		};

		struct MeshShapeCacheEntry
		{
			Ref<const PhysicsMeshData> Data;
			JPH::RefConst<JPH::Shape> Shape;
		};

		PhysicsMeshProvider& GetMeshProviderStorage()
		{
			static PhysicsMeshProvider s_MeshProvider;
			return s_MeshProvider;
		}

		PhysicsSettings SanitizeSettings(const PhysicsSettings& settings)
		{
			PhysicsSettings result = settings;
			result.MaxBodies = std::clamp(settings.MaxBodies, 1u, static_cast<uint32_t>(JPH::PhysicsSystem::cMaxBodiesLimit));
			result.MaxBodyPairs = std::clamp(settings.MaxBodyPairs, 1u, static_cast<uint32_t>(JPH::PhysicsSystem::cMaxBodyPairsLimit));
			result.MaxContactConstraints = std::clamp(settings.MaxContactConstraints, 1u, static_cast<uint32_t>(JPH::PhysicsSystem::cMaxContactConstraintsLimit));
			result.CollisionSteps = std::clamp(settings.CollisionSteps, 1u, 64u);
			result.TempAllocatorSize = std::max(settings.TempAllocatorSize, 64u * 1024u);
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
		Scope<JPH::TempAllocator> Allocator;
		Scope<JPH::JobSystem> Jobs;
		std::unordered_map<AssetHandle, MeshShapeCacheEntry> ConvexMeshShapes;
		std::unordered_map<AssetHandle, MeshShapeCacheEntry> TriangleMeshShapes;
		Scope<JPH::PhysicsSystem> JoltSystem; // Destroyed before the listeners, filters and allocators it references

		std::map<entt::entity, BodyRecord> Bodies;                  // Ordered: per-step processing is deterministic
		std::unordered_map<JPH::uint32, entt::entity> BodyEntities; // Body ID (index and sequence number) -> entity
		std::unordered_map<PairKey, TouchingPair, PairKeyHash> TouchingPairs;
		std::vector<CollisionEvent> PendingEvents;
		std::unordered_set<uint64_t> IssuedWarnings;                // Hash of (entity, PhysicsWarning)
		JPH::EPhysicsUpdateError ReportedErrors = JPH::EPhysicsUpdateError::None;
		uint64_t StepCount = 0;
		float LastStepTime = 0.0f;
	};

	namespace
	{

		bool ShouldWarn(PhysicsWorldData& data, UUID entityID, PhysicsWarning warning)
		{
			return data.IssuedWarnings.insert(Hash::Combine(static_cast<uint64_t>(entityID), static_cast<uint64_t>(warning))).second;
		}

		bool HasScaleChanged(const glm::vec3& scale, const glm::vec3& previous)
		{
			const glm::vec3 difference = glm::abs(scale - previous);
			const glm::vec3 tolerance = glm::max(glm::abs(previous), glm::vec3(1.0f)) * c_ScaleChangeTolerance;
			return glm::any(glm::greaterThan(difference, tolerance));
		}

		uint32_t GetHierarchyDepth(Entity entity)
		{
			uint32_t depth = 0;
			for (Entity parent = entity.GetParent(); parent.IsValid(); parent = parent.GetParent())
				depth++;
			return depth;
		}

		BodyRecord* FindRecord(PhysicsWorldData& data, entt::entity handle)
		{
			auto it = data.Bodies.find(handle);
			return it != data.Bodies.end() ? &it->second : nullptr;
		}

		// The record of an entity whose body is part of the simulation, or nullptr.
		BodyRecord* FindSimulatedRecord(PhysicsWorldData& data, Entity entity)
		{
			if (!entity.IsValid() || entity.GetScene() != data.OwnerScene)
				return nullptr;
			auto it = data.Bodies.find(entity.GetHandle());
			if (it == data.Bodies.end() || !it->second.InSimulation)
				return nullptr;
			return &it->second;
		}

		const BodyRecord* FindSimulatedRecord(const PhysicsWorldData& data, Entity entity)
		{
			return FindSimulatedRecord(const_cast<PhysicsWorldData&>(data), entity);
		}

		const BodyRecord* FindDynamicRecord(const PhysicsWorldData& data, Entity entity)
		{
			const BodyRecord* record = FindSimulatedRecord(data, entity);
			return record && record->Type == RigidBodyType::Dynamic ? record : nullptr;
		}

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

		// Ends every contact of an entity (its body left the simulation).
		void EndContactsOf(PhysicsWorldData& data, UUID entityID)
		{
			std::vector<TouchingPair> ended;
			for (auto it = data.TouchingPairs.begin(); it != data.TouchingPairs.end();)
			{
				if (it->second.A == entityID || it->second.B == entityID)
				{
					ended.push_back(it->second);
					it = data.TouchingPairs.erase(it);
				}
				else
				{
					++it;
				}
			}

			std::sort(ended.begin(), ended.end(), [](const TouchingPair& a, const TouchingPair& b) { return a.SortKey < b.SortKey; });
			for (const TouchingPair& pair : ended)
				QueueEvent(data, CollisionEventType::End, pair);
		}

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

		void RemoveFromSimulation(PhysicsWorldData& data, BodyRecord& record)
		{
			if (!record.InSimulation)
				return;

			JPH::BodyInterface& bodies = data.JoltSystem->GetBodyInterface();
			if (record.Type == RigidBodyType::Dynamic)
			{
				JPH::Vec3 linearVelocity;
				JPH::Vec3 angularVelocity;
				bodies.GetLinearAndAngularVelocity(record.BodyID, linearVelocity, angularVelocity);
				record.SavedLinearVelocity = ToGlm(linearVelocity);
				record.SavedAngularVelocity = ToGlm(angularVelocity);
			}

			WakeBodiesAround(data, record.BodyID);
			bodies.RemoveBody(record.BodyID);
			record.InSimulation = false;
			record.KinematicMoving = false;
			EndContactsOf(data, record.EntityID);
		}

		// Adds the body back at its entity's current pose. Returns false if the body cannot be placed as it is (the world
		// scale changed or the transform is degenerate) and must be rebuilt instead.
		bool AddToSimulation(PhysicsWorldData& data, BodyRecord& record, Entity entity)
		{
			if (record.InSimulation)
				return true;

			JPH::BodyInterface& bodies = data.JoltSystem->GetBodyInterface();
			const glm::mat4 worldTransform = data.OwnerScene->GetWorldTransform(entity);
			if (worldTransform != record.LastWorldTransform)
			{
				glm::vec3 position;
				glm::quat rotation;
				glm::vec3 scale;
				if (!Math::DecomposeTransform(worldTransform, position, rotation, scale) || HasScaleChanged(scale, record.ShapeScale))
					return false;

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
			record.InSimulation = true;
			record.KinematicMoving = false;
			return true;
		}

		// Destroys the Jolt body of a record (the record itself stays). Contacts are left alone.
		void DestroyJoltBody(PhysicsWorldData& data, BodyRecord& record)
		{
			JPH::BodyInterface& bodies = data.JoltSystem->GetBodyInterface();
			if (record.InSimulation)
			{
				WakeBodiesAround(data, record.BodyID);
				bodies.RemoveBody(record.BodyID);
				record.InSimulation = false;
			}
			bodies.DestroyBody(record.BodyID);
			data.BodyEntities.erase(record.BodyID.GetIndexAndSequenceNumber());
		}

		void DestroyAllBodies(PhysicsWorldData& data)
		{
			std::vector<JPH::BodyID> added;
			std::vector<JPH::BodyID> all;
			for (const auto& [handle, record] : data.Bodies)
			{
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
			data.BodyEntities.clear();
			data.TouchingPairs.clear();
			data.PendingEvents.clear();
		}

		//////////////////////////////////////////////////////////////////////////
		// Shapes
		//////////////////////////////////////////////////////////////////////////

		JPH::RefConst<JPH::Shape> CreateShape(const JPH::ShapeSettings& settings, const Entity& entity, std::string_view what)
		{
			JPH::ShapeSettings::ShapeResult result = settings.Create();
			if (result.HasError())
			{
				ST_CORE_WARN("Physics: the {} of '{}' is invalid and is ignored: {}", what, entity.GetName(), result.GetError().c_str());
				return nullptr;
			}
			return result.Get();
		}

		JPH::RefConst<JPH::Shape> CreateConvexHullShape(const PhysicsMeshData& mesh, const Entity& entity)
		{
			JPH::Array<JPH::Vec3> points;
			points.reserve(mesh.Positions.size());
			for (const glm::vec3& position : mesh.Positions)
			{
				if (!IsFinite(position))
				{
					ST_CORE_WARN("Physics: the mesh collider of '{}' has non-finite vertex positions and is ignored", entity.GetName());
					return nullptr;
				}
				points.push_back(ToJolt(position));
			}

			const JPH::ConvexHullShapeSettings settings(points, JPH::cDefaultConvexRadius);
			return CreateShape(settings, entity, "convex mesh collider");
		}

		JPH::RefConst<JPH::Shape> CreateTriangleMeshShape(const PhysicsMeshData& mesh, const Entity& entity)
		{
			if (mesh.Indices.empty() || mesh.Indices.size() % 3 != 0)
			{
				ST_CORE_WARN("Physics: the mesh collider of '{}' needs a triangle list (index count {} is not a positive multiple of 3); it is ignored", entity.GetName(), mesh.Indices.size());
				return nullptr;
			}

			JPH::VertexList vertices;
			vertices.reserve(mesh.Positions.size());
			for (const glm::vec3& position : mesh.Positions)
			{
				if (!IsFinite(position))
				{
					ST_CORE_WARN("Physics: the mesh collider of '{}' has non-finite vertex positions and is ignored", entity.GetName());
					return nullptr;
				}
				vertices.push_back(JPH::Float3(position.x, position.y, position.z));
			}

			JPH::IndexedTriangleList triangles;
			triangles.reserve(mesh.Indices.size() / 3);
			for (size_t index = 0; index < mesh.Indices.size(); index += 3)
			{
				const uint32_t i0 = mesh.Indices[index];
				const uint32_t i1 = mesh.Indices[index + 1];
				const uint32_t i2 = mesh.Indices[index + 2];
				if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
				{
					ST_CORE_WARN("Physics: the mesh collider of '{}' references vertex indices beyond its {} vertices; it is ignored", entity.GetName(), vertices.size());
					return nullptr;
				}
				triangles.push_back(JPH::IndexedTriangle(i0, i1, i2));
			}

			// The settings remove degenerate and duplicate triangles; a mesh without any valid triangle fails to build.
			const JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
			return CreateShape(settings, entity, "mesh collider");
		}

		// Unscaled mesh shape for an asset, built once per asset data and reused (also across entities).
		JPH::RefConst<JPH::Shape> GetMeshShape(PhysicsWorldData& data, const Entity& entity, AssetHandle mesh, bool convex)
		{
			const PhysicsMeshProvider& provider = GetMeshProviderStorage();
			if (!provider)
			{
				ST_CORE_WARN("Physics: the mesh collider of '{}' is ignored: no physics mesh provider is registered", entity.GetName());
				return nullptr;
			}

			Ref<const PhysicsMeshData> meshData = provider(mesh);
			if (!meshData)
			{
				ST_CORE_WARN("Physics: the mesh collider of '{}' is ignored: mesh {} is not available", entity.GetName(), mesh.ToString());
				return nullptr;
			}

			std::unordered_map<AssetHandle, MeshShapeCacheEntry>& cache = convex ? data.ConvexMeshShapes : data.TriangleMeshShapes;
			auto it = cache.find(mesh);
			if (it != cache.end() && it->second.Data == meshData)
				return it->second.Shape;

			JPH::RefConst<JPH::Shape> shape = convex ? CreateConvexHullShape(*meshData, entity) : CreateTriangleMeshShape(*meshData, entity);
			if (!shape)
				return nullptr;

			cache[mesh] = MeshShapeCacheEntry { meshData, shape };
			return shape;
		}

		struct ShapePart
		{
			JPH::RefConst<JPH::Shape> Shape;
			JPH::Vec3 Position = JPH::Vec3::sZero(); // Body space position of the collider (offset scaled with the entity)
		};

		void WarnNonFiniteCollider(const Entity& entity, std::string_view collider)
		{
			ST_CORE_WARN("Physics: the {} of '{}' has non-finite dimensions and is ignored", collider, entity.GetName());
		}

		// Builds the body shape of an entity from its colliders with the entity's world scale baked in. Returns nullptr if
		// no collider is usable.
		JPH::RefConst<JPH::Shape> BuildBodyShape(PhysicsWorldData& data, const Entity& entity, const glm::vec3& scale, RigidBodyType type)
		{
			const glm::vec3 absoluteScale = glm::abs(scale);
			const float maxScale = std::max({ absoluteScale.x, absoluteScale.y, absoluteScale.z });
			std::vector<ShapePart> parts;

			if (const BoxColliderComponent* box = entity.TryGetComponent<BoxColliderComponent>())
			{
				if (IsFinite(box->HalfExtents) && IsFinite(box->Offset))
				{
					const glm::vec3 halfExtents = glm::max(glm::abs(box->HalfExtents) * absoluteScale, glm::vec3(c_MinColliderExtent));
					const JPH::BoxShapeSettings settings(ToJolt(halfExtents));
					if (JPH::RefConst<JPH::Shape> shape = CreateShape(settings, entity, "box collider"))
						parts.push_back({ shape, ToJolt(box->Offset * scale) });
				}
				else
				{
					WarnNonFiniteCollider(entity, "box collider");
				}
			}

			if (const SphereColliderComponent* sphere = entity.TryGetComponent<SphereColliderComponent>())
			{
				if (std::isfinite(sphere->Radius) && IsFinite(sphere->Offset))
				{
					const float radius = std::max(std::abs(sphere->Radius) * maxScale, c_MinColliderExtent);
					const JPH::SphereShapeSettings settings(radius);
					if (JPH::RefConst<JPH::Shape> shape = CreateShape(settings, entity, "sphere collider"))
						parts.push_back({ shape, ToJolt(sphere->Offset * scale) });
				}
				else
				{
					WarnNonFiniteCollider(entity, "sphere collider");
				}
			}

			if (const CapsuleColliderComponent* capsule = entity.TryGetComponent<CapsuleColliderComponent>())
			{
				if (std::isfinite(capsule->Radius) && std::isfinite(capsule->HalfHeight) && IsFinite(capsule->Offset))
				{
					const float radius = std::max(std::abs(capsule->Radius) * std::max(absoluteScale.x, absoluteScale.z), c_MinColliderExtent);
					const float halfHeight = std::abs(capsule->HalfHeight) * absoluteScale.y;
					JPH::RefConst<JPH::Shape> shape;
					if (halfHeight < c_MinColliderExtent)
						shape = CreateShape(JPH::SphereShapeSettings(radius), entity, "capsule collider"); // No cylindrical part
					else
						shape = CreateShape(JPH::CapsuleShapeSettings(halfHeight, radius), entity, "capsule collider");
					if (shape)
						parts.push_back({ shape, ToJolt(capsule->Offset * scale) });
				}
				else
				{
					WarnNonFiniteCollider(entity, "capsule collider");
				}
			}

			if (const MeshColliderComponent* meshCollider = entity.TryGetComponent<MeshColliderComponent>())
			{
				AssetHandle mesh = meshCollider->Mesh;
				if (!mesh.IsValid())
				{
					if (const MeshRendererComponent* renderer = entity.TryGetComponent<MeshRendererComponent>())
						mesh = renderer->Mesh;
				}

				if (!mesh.IsValid())
				{
					ST_CORE_WARN("Physics: the mesh collider of '{}' has no mesh (and no Mesh Renderer mesh to fall back to); it is ignored", entity.GetName());
				}
				else if (!meshCollider->Convex && type != RigidBodyType::Static)
				{
					ST_CORE_WARN("Physics: the non-convex mesh collider of '{}' is ignored: triangle meshes are only supported on static bodies (enable Convex)", entity.GetName());
				}
				else if (JPH::RefConst<JPH::Shape> meshShape = GetMeshShape(data, entity, mesh, meshCollider->Convex))
				{
					if (scale == glm::vec3(1.0f))
					{
						parts.push_back({ meshShape, JPH::Vec3::sZero() });
					}
					else
					{
						// Convex hulls and triangle meshes support any non-zero scale, including mirroring.
						const JPH::ScaledShapeSettings settings(meshShape.GetPtr(), ToJolt(scale));
						if (JPH::RefConst<JPH::Shape> shape = CreateShape(settings, entity, "mesh collider"))
							parts.push_back({ shape, JPH::Vec3::sZero() });
					}
				}
			}

			if (parts.empty())
				return nullptr;

			if (parts.size() == 1)
			{
				if (parts.front().Position.IsNearZero(0.0f))
					return parts.front().Shape;

				const JPH::RotatedTranslatedShapeSettings settings(parts.front().Position, JPH::Quat::sIdentity(), parts.front().Shape.GetPtr());
				return CreateShape(settings, entity, "collider offset");
			}

			JPH::StaticCompoundShapeSettings compound;
			for (const ShapePart& part : parts)
				compound.AddShape(part.Position, JPH::Quat::sIdentity(), part.Shape.GetPtr());
			return CreateShape(compound, entity, "compound collider");
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
		// Stepping
		//////////////////////////////////////////////////////////////////////////

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

			state.Simulated = true;
			state.Static = record->Type == RigidBodyType::Static;
			state.Active = !state.Static && data.JoltSystem->GetBodyInterface().IsActive(record->BodyID);
			return state;
		}

		// Jolt only reports contacts of awake bodies. Before a step, flag the touching pairs that Jolt is able to report;
		// pairs that are not reported during the step end only if they were able to be reported. Pairs of sleeping bodies stay
		// touching until a body wakes up, and pairs of two static bodies (which can never touch) end.
		void FlagReportablePairs(PhysicsWorldData& data)
		{
			for (auto& [key, pair] : data.TouchingPairs)
			{
				const ParticipantState a = GetParticipantState(data, pair.A);
				const ParticipantState b = GetParticipantState(data, pair.B);
				pair.Awake = !a.Simulated || !b.Simulated || a.Active || b.Active || (a.Static && b.Static);
			}
		}

		void ProcessContacts(PhysicsWorldData& data)
		{
			ST_PROFILE_FUNCTION();

			// Several sub shape pairs of the same entity pair may touch; keep the lowest sub shape key so that the reported
			// point does not depend on the order in which worker threads reported them.
			std::unordered_map<PairKey, ContactReport, PairKeyHash> current;
			for (const ContactReport& report : data.Contacts.TakeReports())
			{
				auto [it, inserted] = current.try_emplace(MakePairKey(report.EntityA, report.EntityB), report);
				if (!inserted && report.SubShapeKey < it->second.SubShapeKey)
					it->second = report;
			}

			std::vector<TouchingPair> ended;
			for (auto it = data.TouchingPairs.begin(); it != data.TouchingPairs.end();)
			{
				if (it->second.Awake && current.find(it->first) == current.end())
				{
					ended.push_back(it->second);
					it = data.TouchingPairs.erase(it);
				}
				else
				{
					++it;
				}
			}

			std::vector<TouchingPair> begun;
			for (const auto& [key, report] : current)
			{
				auto it = data.TouchingPairs.find(key);
				if (it == data.TouchingPairs.end())
				{
					TouchingPair pair;
					pair.A = report.EntityA;
					pair.B = report.EntityB;
					pair.IsTrigger = report.IsTrigger;
					pair.SortKey = report.SortKey;
					pair.Point = report.Point;
					pair.Normal = report.Normal;
					data.TouchingPairs.emplace(key, pair);
					begun.push_back(pair);
				}
				else
				{
					// Keep the pair's orientation from when it began (body IDs, and thus Jolt's order, change on rebuilds).
					TouchingPair& pair = it->second;
					pair.IsTrigger = report.IsTrigger;
					pair.Point = report.Point;
					pair.Normal = report.EntityA == pair.A ? report.Normal : -report.Normal;
				}
			}

			const auto bySortKey = [](const TouchingPair& a, const TouchingPair& b) { return a.SortKey < b.SortKey; };
			std::sort(ended.begin(), ended.end(), bySortKey);
			std::sort(begun.begin(), begun.end(), bySortKey);
			for (const TouchingPair& pair : ended)
				QueueEvent(data, CollisionEventType::End, pair);
			for (const TouchingPair& pair : begun)
				QueueEvent(data, CollisionEventType::Begin, pair);
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

		JPH::BodyID GetIgnoredBody(const PhysicsWorldData& data, Entity ignoreEntity)
		{
			if (!ignoreEntity.IsValid() || ignoreEntity.GetScene() != data.OwnerScene)
				return JPH::BodyID();
			auto it = data.Bodies.find(ignoreEntity.GetHandle());
			return it != data.Bodies.end() ? it->second.BodyID : JPH::BodyID();
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
		PhysicsWorldData& data = *m_Data;
		data.Allocator = CreateScope<JPH::TempAllocatorImplWithMallocFallback>(data.Settings.TempAllocatorSize);
		data.Jobs = CreatePhysicsJobSystem();
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
	}

	PhysicsWorld::~PhysicsWorld()
	{
		DestroyAllBodies(*m_Data);
	}

	void PhysicsWorld::SetMeshProvider(PhysicsMeshProvider provider)
	{
		GetMeshProviderStorage() = std::move(provider);
	}

	bool PhysicsWorld::HasMeshProvider()
	{
		return static_cast<bool>(GetMeshProviderStorage());
	}

	Scene& PhysicsWorld::GetScene() const
	{
		return *m_Data->OwnerScene;
	}

	const PhysicsSettings& PhysicsWorld::GetSettings() const
	{
		return m_Data->Settings;
	}

	void PhysicsWorld::CreateAllBodies()
	{
		ST_PROFILE_FUNCTION();

		for (const Entity entity : m_Data->OwnerScene->GetEntitiesInHierarchyOrder())
		{
			if (entity.HasAnyComponent<RigidBodyComponent, BoxColliderComponent, SphereColliderComponent, CapsuleColliderComponent, MeshColliderComponent>())
				RefreshBody(entity.GetHandle());
		}

		// Bodies were inserted one by one; rebuilding the broad phase trees once makes the first queries and steps fast.
		m_Data->JoltSystem->OptimizeBroadPhase();
	}

	bool PhysicsWorld::RefreshBody(entt::entity handle)
	{
		ST_PROFILE_FUNCTION();

		PhysicsWorldData& data = *m_Data;
		Scene& scene = *data.OwnerScene;
		const Entity entity(handle, &scene);
		if (!entity.IsValid())
		{
			DestroyBody(handle);
			return false;
		}

		const RigidBodyComponent* rigidBody = entity.TryGetComponent<RigidBodyComponent>();
		if (!entity.HasAnyComponent<BoxColliderComponent, SphereColliderComponent, CapsuleColliderComponent, MeshColliderComponent>())
		{
			if (rigidBody && ShouldWarn(data, entity.GetUUID(), PhysicsWarning::MissingCollider))
				ST_CORE_WARN("Physics: '{}' has a Rigid Body but no collider; it is not simulated", entity.GetName());
			DestroyBody(handle);
			return false;
		}

		const glm::mat4 worldTransform = scene.GetWorldTransform(entity);
		glm::vec3 position;
		glm::quat rotation;
		glm::vec3 scale;
		if (!Math::DecomposeTransform(worldTransform, position, rotation, scale))
		{
			ST_CORE_WARN("Physics: '{}' has a degenerate world transform (zero scale or non-finite values); it is not simulated", entity.GetName());
			DestroyBody(handle);
			return false;
		}

		RigidBodyType type = rigidBody ? rigidBody->Type : RigidBodyType::Static;
		if (type != RigidBodyType::Static && type != RigidBodyType::Dynamic && type != RigidBodyType::Kinematic)
		{
			ST_CORE_WARN("Physics: '{}' has an invalid rigid body type {}; it is treated as static", entity.GetName(), static_cast<int>(type));
			type = RigidBodyType::Static;
		}

		const JPH::RefConst<JPH::Shape> shape = BuildBodyShape(data, entity, scale, type);
		if (!shape)
		{
			ST_CORE_WARN("Physics: '{}' has no usable collider; it is not simulated", entity.GetName());
			DestroyBody(handle);
			return false;
		}

		JPH::BodyInterface& bodies = data.JoltSystem->GetBodyInterface();
		JPH::BodyCreationSettings settings = MakeBodySettings(data, entity, rigidBody, type, shape.GetPtr(), position, rotation);

		// A dynamic body rebuilt because a property changed keeps moving as before.
		BodyRecord* existing = FindRecord(data, handle);
		glm::vec3 linearVelocity(0.0f);
		glm::vec3 angularVelocity(0.0f);
		if (existing && existing->Type == RigidBodyType::Dynamic && type == RigidBodyType::Dynamic)
		{
			if (existing->InSimulation)
			{
				JPH::Vec3 currentLinear;
				JPH::Vec3 currentAngular;
				bodies.GetLinearAndAngularVelocity(existing->BodyID, currentLinear, currentAngular);
				linearVelocity = ToGlm(currentLinear);
				angularVelocity = ToGlm(currentAngular);
			}
			else
			{
				linearVelocity = existing->SavedLinearVelocity;
				angularVelocity = existing->SavedAngularVelocity;
			}
		}

		JPH::Body* body = bodies.CreateBody(settings);
		if (!body)
		{
			ST_CORE_ERROR("Physics: cannot create a body for '{}': the world's limit of {} bodies is reached", entity.GetName(), data.Settings.MaxBodies);
			DestroyBody(handle);
			return false;
		}

		BodyRecord record;
		record.EntityID = entity.GetUUID();
		record.BodyID = body->GetID();
		record.Type = type;
		record.ShapeScale = scale;
		record.LastWorldTransform = worldTransform;
		record.KinematicTargetPosition = position;
		record.KinematicTargetRotation = rotation;
		record.SavedLinearVelocity = linearVelocity; // Applied when the body enters the simulation
		record.SavedAngularVelocity = angularVelocity;

		if (existing)
		{
			// Contacts are tracked per entity pair and carry over to the new body; the next step confirms or ends them.
			DestroyJoltBody(data, *existing);
			*existing = record;
		}
		else
		{
			existing = &data.Bodies.emplace(handle, record).first->second;
		}
		data.BodyEntities[record.BodyID.GetIndexAndSequenceNumber()] = handle;

		if (scene.IsActiveInHierarchy(entity))
			AddToSimulation(data, *existing, entity);
		else
			EndContactsOf(data, record.EntityID);
		return true;
	}

	void PhysicsWorld::RefreshActivity(entt::entity handle)
	{
		PhysicsWorldData& data = *m_Data;
		BodyRecord* record = FindRecord(data, handle);
		if (!record)
			return;

		const Entity entity(handle, data.OwnerScene);
		if (!entity.IsValid())
		{
			DestroyBody(handle);
			return;
		}

		if (!data.OwnerScene->IsActiveInHierarchy(entity))
			RemoveFromSimulation(data, *record);
		else if (!record->InSimulation && !AddToSimulation(data, *record, entity))
			RefreshBody(handle);
	}

	void PhysicsWorld::DestroyBody(entt::entity handle)
	{
		PhysicsWorldData& data = *m_Data;
		auto it = data.Bodies.find(handle);
		if (it == data.Bodies.end())
			return;

		const UUID entityID = it->second.EntityID;
		DestroyJoltBody(data, it->second);
		data.Bodies.erase(it);
		EndContactsOf(data, entityID);
	}

	bool PhysicsWorld::HasBody(Entity entity) const
	{
		return FindSimulatedRecord(*m_Data, entity) != nullptr;
	}

	void PhysicsWorld::Simulate(float timestep)
	{
		ST_PROFILE_FUNCTION();

		if (!(timestep > 0.0f) || !std::isfinite(timestep))
			return;

		PhysicsWorldData& data = *m_Data;
		Scene& scene = *data.OwnerScene;
		JPH::BodyInterface& bodies = data.JoltSystem->GetBodyInterface();
		const auto startTime = std::chrono::steady_clock::now();

		// Entities -> bodies: activity, teleports, kinematic targets and scale changes.
		std::vector<entt::entity> destroyed;
		std::vector<entt::entity> rebuild;
		for (auto& [handle, record] : data.Bodies)
		{
			const Entity entity(handle, &scene);
			if (!entity.IsValid())
			{
				destroyed.push_back(handle);
				continue;
			}

			if (!scene.IsActiveInHierarchy(entity))
			{
				RemoveFromSimulation(data, record);
				continue;
			}
			if (!record.InSimulation)
			{
				if (!AddToSimulation(data, record, entity))
					rebuild.push_back(handle);
				continue;
			}

			const glm::mat4 worldTransform = scene.GetWorldTransform(entity);
			if (worldTransform == record.LastWorldTransform)
			{
				// A kinematic body keeps the velocity of its last move; stop it at its target.
				if (record.KinematicMoving)
				{
					bodies.MoveKinematic(record.BodyID, ToJoltPosition(record.KinematicTargetPosition), ToJolt(record.KinematicTargetRotation), timestep);
					record.KinematicMoving = false;
				}
				continue;
			}

			glm::vec3 position;
			glm::quat rotation;
			glm::vec3 scale;
			if (!Math::DecomposeTransform(worldTransform, position, rotation, scale))
			{
				if (ShouldWarn(data, record.EntityID, PhysicsWarning::DegenerateTransform))
					ST_CORE_WARN("Physics: '{}' has a degenerate world transform; its body keeps its last pose", entity.GetName());
				continue;
			}
			if (HasScaleChanged(scale, record.ShapeScale))
			{
				rebuild.push_back(handle);
				continue;
			}

			switch (record.Type)
			{
				case RigidBodyType::Static:
					WakeBodiesAround(data, record.BodyID);
					bodies.SetPositionAndRotation(record.BodyID, ToJoltPosition(position), ToJolt(rotation), JPH::EActivation::DontActivate);
					WakeBodiesAround(data, record.BodyID);
					break;
				case RigidBodyType::Kinematic:
					bodies.MoveKinematic(record.BodyID, ToJoltPosition(position), ToJolt(rotation), timestep);
					record.KinematicMoving = true;
					record.KinematicTargetPosition = position;
					record.KinematicTargetRotation = rotation;
					break;
				case RigidBodyType::Dynamic:
					// Moved from outside physics (gameplay code, editor gizmo, a moving parent): teleport, keeping velocity.
					bodies.SetPositionAndRotation(record.BodyID, ToJoltPosition(position), ToJolt(rotation), JPH::EActivation::Activate);
					break;
			}
			record.LastWorldTransform = worldTransform;
		}

		for (entt::entity handle : destroyed)
			DestroyBody(handle);
		for (entt::entity handle : rebuild)
			RefreshBody(handle);

		// Step.
		FlagReportablePairs(data);
		data.Activations.Clear();
		{
			ST_PROFILE_SCOPE("PhysicsWorld::Simulate - Jolt");
			const JPH::EPhysicsUpdateError errors = data.JoltSystem->Update(timestep, static_cast<int>(data.Settings.CollisionSteps), data.Allocator.get(), data.Jobs.get());
			if (errors != JPH::EPhysicsUpdateError::None)
				ReportUpdateErrors(data, errors);
		}

		// Bodies -> entities: dynamic bodies that moved (awake, or fell asleep during this step), parents before children so
		// that every child's local transform is computed against its parent's final pose.
		JPH::BodyIDVector movedBodies;
		data.JoltSystem->GetActiveBodies(JPH::EBodyType::RigidBody, movedBodies);
		for (const JPH::BodyID& bodyID : data.Activations.Take())
			movedBodies.push_back(bodyID);

		struct WriteBack
		{
			uint32_t Depth = 0;
			entt::entity Handle = entt::null;
		};
		std::vector<WriteBack> writeBacks;
		writeBacks.reserve(movedBodies.size());
		for (const JPH::BodyID& bodyID : movedBodies)
		{
			auto entityIt = data.BodyEntities.find(bodyID.GetIndexAndSequenceNumber());
			if (entityIt == data.BodyEntities.end())
				continue;
			const BodyRecord* record = FindRecord(data, entityIt->second);
			if (!record || record->Type != RigidBodyType::Dynamic || !record->InSimulation)
				continue;
			writeBacks.push_back({ GetHierarchyDepth(Entity(entityIt->second, &scene)), entityIt->second });
		}
		std::sort(writeBacks.begin(), writeBacks.end(), [](const WriteBack& a, const WriteBack& b)
		{
			return a.Depth != b.Depth ? a.Depth < b.Depth : a.Handle < b.Handle;
		});
		writeBacks.erase(std::unique(writeBacks.begin(), writeBacks.end(), [](const WriteBack& a, const WriteBack& b) { return a.Handle == b.Handle; }), writeBacks.end());

		for (const WriteBack& writeBack : writeBacks)
		{
			BodyRecord& record = *FindRecord(data, writeBack.Handle); // Filtered above
			const Entity entity(writeBack.Handle, &scene);

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

			scene.SetWorldTransform(entity, Math::ComposeTransform(position, glm::normalize(rotation), record.ShapeScale));
			record.LastWorldTransform = scene.GetWorldTransform(entity);
		}

		ProcessContacts(data);

		data.StepCount++;
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
		BodyRecord* simulatedRecord = FindSimulatedRecord(data, entity);
		if (!simulatedRecord || !IsFinite(position) || !IsFinite(rotation))
			return false;

		const float rotationLength = glm::length(rotation);
		if (!(rotationLength > 1.0e-6f))
			return false;
		const glm::quat normalizedRotation = rotation / rotationLength;

		BodyRecord& record = *simulatedRecord;
		Scene& scene = *data.OwnerScene;
		if (!scene.SetWorldTransform(entity, Math::ComposeTransform(position, normalizedRotation, record.ShapeScale)))
			return false;
		record.LastWorldTransform = scene.GetWorldTransform(entity);
		record.KinematicTargetPosition = position;
		record.KinematicTargetRotation = normalizedRotation;
		record.KinematicMoving = false;

		JPH::BodyInterface& bodies = data.JoltSystem->GetBodyInterface();
		if (record.Type == RigidBodyType::Static)
		{
			WakeBodiesAround(data, record.BodyID);
			bodies.SetPositionAndRotation(record.BodyID, ToJoltPosition(position), ToJolt(normalizedRotation), JPH::EActivation::DontActivate);
			WakeBodiesAround(data, record.BodyID);
		}
		else
		{
			if (record.Type == RigidBodyType::Kinematic)
				bodies.SetLinearAndAngularVelocity(record.BodyID, JPH::Vec3::sZero(), JPH::Vec3::sZero());
			bodies.SetPositionAndRotation(record.BodyID, ToJoltPosition(position), ToJolt(normalizedRotation), JPH::EActivation::Activate);
		}
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
		stats.StepCount = data.StepCount;
		stats.LastStepTime = data.LastStepTime;
		return stats;
	}

}
