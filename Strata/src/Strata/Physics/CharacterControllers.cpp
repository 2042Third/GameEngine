#include "stpch.h"
#include "Strata/Physics/CharacterControllers.h"

#include "Strata/Core/JobSystem.h"
#include "Strata/Math/Math.h"
#include "Strata/Physics/PhysicsComponents.h"
#include "Strata/Physics/PhysicsLayers.h"
#include "Strata/Physics/PhysicsStateBuffer.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <unordered_set>

namespace Strata
{

	namespace
	{

		// What a character weighs (kilograms, an adult person): its weight presses on a dynamic body it stands on (PushBodies).
		constexpr float c_CharacterMass = 70.0f;
		// The presence body's radius is this fraction of the character's, so that it stays clear of what the character touches
		// on its sides (the character keeps its padding away from geometry) and meets only what moves into the character; its
		// top is the character's (a crate on its head rests where the character's head is) and its bottom stays above the
		// ground.
		constexpr float c_PresenceRadiusFraction = 0.9f;
		// How far Jolt's characters keep their shape from geometry (CharacterVirtualSettings::mCharacterPadding, its default):
		// their shape is lifted by it above their position.
		constexpr float c_CharacterPadding = 0.02f;
		// Smallest radius and height of a controller's capsule (world units), so Jolt never sees a degenerate shape.
		constexpr float c_MinCharacterExtent = 1.0e-3f;
		// Moving away from the ground faster than this (meters per second) leaves it: a jump or a launch. Slower, a character
		// on the ground takes on the ground's velocity (a moving platform).
		constexpr float c_LeaveGroundSpeed = 0.1f;
		// An entity whose world position differs from where its controller put it by more than this (relative to the
		// position's magnitude, at least this many meters) was moved from outside: the controller teleports there.
		constexpr float c_ExternalMoveTolerance = 1.0e-4f;
		// Scratch memory for the collision queries of one batch of controllers (falls back to the heap beyond), and the
		// smallest batch; batches are bounded so that the scratch memory stays bounded however many controllers move.
		constexpr uint32_t c_ScratchBytesPerBatch = 128 * 1024;
		constexpr uint32_t c_MinBatchSize = 16;
		constexpr uint32_t c_BatchesPerThread = 4;

		JPH::Vec3 ToJolt(const glm::vec3& value)
		{
			return JPH::Vec3(value.x, value.y, value.z);
		}

		JPH::RVec3 ToJoltPosition(const glm::vec3& value)
		{
			return JPH::RVec3(value.x, value.y, value.z);
		}

		glm::vec3 ToGlm(JPH::Vec3Arg value)
		{
			return glm::vec3(value.GetX(), value.GetY(), value.GetZ());
		}

		bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		bool IsNearlySamePosition(const glm::vec3& a, const glm::vec3& b)
		{
			const float tolerance = c_ExternalMoveTolerance * std::max(1.0f, std::max({ std::abs(b.x), std::abs(b.y), std::abs(b.z) }));
			return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(tolerance)));
		}

		CharacterGroundState ToGroundState(JPH::CharacterBase::EGroundState state)
		{
			switch (state)
			{
				case JPH::CharacterBase::EGroundState::OnGround: return CharacterGroundState::OnGround;
				case JPH::CharacterBase::EGroundState::OnSteepGround: return CharacterGroundState::OnSteepGround;
				case JPH::CharacterBase::EGroundState::NotSupported: return CharacterGroundState::NotSupported;
				case JPH::CharacterBase::EGroundState::InAir: return CharacterGroundState::InAir;
			}
			return CharacterGroundState::InAir;
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

		// The shape values a controller is built with, from its component (sanitized: finite, within the shapes' limits).
		struct ControllerShape
		{
			float Height = 1.8f;
			float Radius = 0.3f;
			uint32_t Layer = 0;

			bool operator==(const ControllerShape& other) const = default;
		};

		ControllerShape GetShapeOf(const CharacterControllerComponent& component)
		{
			const CharacterControllerComponent defaults;
			ControllerShape shape;
			shape.Radius = std::isfinite(component.Radius) ? std::max(std::abs(component.Radius), c_MinCharacterExtent) : defaults.Radius;
			shape.Height = std::isfinite(component.Height) ? std::max(std::abs(component.Height), c_MinCharacterExtent) : defaults.Height;
			// A capsule shorter than its diameter is a sphere of half its height.
			shape.Radius = std::min(shape.Radius, shape.Height * 0.5f);
			shape.Layer = component.Layer < c_PhysicsLayerCount ? component.Layer : 0;
			return shape;
		}

		// The controller's capsule (a sphere when it is no taller than wide) with its top at `shape.Height` above the origin and
		// its radius times `radiusFraction`: standing on the origin with the full radius.
		JPH::RefConst<JPH::Shape> CreateCapsule(const ControllerShape& shape, float radiusFraction)
		{
			const float radius = shape.Radius * radiusFraction;
			const float halfCylinder = std::max(shape.Height - 2.0f * shape.Radius, 0.0f) * 0.5f;
			JPH::RefConst<JPH::ShapeSettings> core;
			if (halfCylinder > c_MinCharacterExtent)
				core = new JPH::CapsuleShapeSettings(halfCylinder, radius);
			else
				core = new JPH::SphereShapeSettings(radius);
			const float center = shape.Height - halfCylinder - radius;
			const JPH::RotatedTranslatedShapeSettings placed(JPH::Vec3(0.0f, center, 0.0f), JPH::Quat::sIdentity(), core.GetPtr());
			JPH::ShapeSettings::ShapeResult result = placed.Create();
			return result.HasError() ? nullptr : result.Get();
		}

		// Where a controller's presence body is for its feet at `position`: lifted by the padding like the character's shape.
		JPH::RVec3 GetPresencePosition(const glm::vec3& position)
		{
			return JPH::RVec3(position.x, position.y + c_CharacterPadding, position.z);
		}

		// A dynamic body a controller ran into during its move: pushed after every controller moved (see Step).
		struct PushContact
		{
			JPH::BodyID Body;
			JPH::RVec3 Position = JPH::RVec3::sZero();
			JPH::Vec3 Normal = JPH::Vec3::sZero();   // Towards the character
			JPH::Vec3 Velocity = JPH::Vec3::sZero(); // Of the body at the contact point
		};

		struct ControllerRecord
		{
			UUID EntityID = UUID::Null();
			entt::entity Handle = entt::null;
			JPH::Ref<JPH::CharacterVirtual> Character;
			JPH::BodyID PresenceBody;
			ControllerShape Shape;
			// Settings applied without rebuilding (refreshed from the component on every change).
			float GravityScale = 1.0f;
			float PushStrength = 200.0f;
			float StepHeight = 0.35f;
			// Gameplay requests (ICharacterControl).
			glm::vec3 WalkVelocity = glm::vec3(0.0f);
			glm::vec3 Motion = glm::vec3(0.0f);
			float JumpSpeed = 0.0f;
			bool JumpRequested = false;
			// Where the controller put its entity, and the velocity it moved with in its last step (after collisions).
			glm::vec3 Position = glm::vec3(0.0f);
			glm::vec3 Velocity = glm::vec3(0.0f);
			// Step scratch: written by the controller's own move (on any thread), read afterwards on the main thread.
			glm::vec3 NewPosition = glm::vec3(0.0f);
			JPH::Vec3 StepVelocity = JPH::Vec3::sZero();
			std::vector<PushContact> Pushes;
			bool WaitsForPresenceSlot = false; // The world was full when the presence body was made (see Rebind)
			bool HasParent = false;
			bool CanWrite = true;
			glm::mat4 InverseParentWorld = glm::mat4(1.0f);
		};

		// Receives the contacts of the controllers' moves on the job system's threads; it writes only into the record of the
		// character it is called for, which only one thread moves.
		class ControllerContactListener final : public JPH::CharacterContactListener
		{
		public:
			void OnContactAdded(const JPH::CharacterVirtual* character, const JPH::CharacterContact& contact, JPH::CharacterContactSettings& settings) override
			{
				Record(character, contact, settings);
			}

			void OnContactPersisted(const JPH::CharacterVirtual* character, const JPH::CharacterContact& contact, JPH::CharacterContactSettings& settings) override
			{
				Record(character, contact, settings);
			}
		private:
			static void Record(const JPH::CharacterVirtual* character, const JPH::CharacterContact& contact, JPH::CharacterContactSettings& settings)
			{
				// Jolt would push the body from this thread, in an order that depends on the threads; the set pushes afterwards.
				settings.mCanReceiveImpulses = false;
				if (contact.mMotionTypeB != JPH::EMotionType::Dynamic || contact.mIsSensorB || contact.mBodyB.IsInvalid())
					return;
				ControllerRecord* record = reinterpret_cast<ControllerRecord*>(static_cast<uintptr_t>(character->GetUserData()));
				const auto existing = std::find_if(record->Pushes.begin(), record->Pushes.end(), [&](const PushContact& push) { return push.Body == contact.mBodyB; });
				if (existing != record->Pushes.end())
					return; // One push per body and step, from its first contact
				record->Pushes.push_back({ contact.mBodyB, contact.mPosition, contact.mContactNormal, contact.mLinearVelocity });
			}
		};

		// The layers a controller collides with: those its layer collides with in the world's live matrix.
		class ControllerLayerFilter final : public JPH::ObjectLayerFilter
		{
		public:
			ControllerLayerFilter(const PhysicsSettings& settings, uint32_t layer)
				: m_Settings(settings), m_Layer(layer)
			{
			}

			bool ShouldCollide(JPH::ObjectLayer layer) const override
			{
				return m_Settings.DoLayersCollide(m_Layer, PhysicsLayers::GetUserLayer(layer));
			}
		private:
			const PhysicsSettings& m_Settings;
			uint32_t m_Layer;
		};

		// A controller never collides with its own presence body or with the bodies of its own entity (colliders on it).
		class ControllerBodyFilter final : public JPH::BodyFilter
		{
		public:
			ControllerBodyFilter(const JPH::BodyID& presenceBody, UUID entity)
				: m_PresenceBody(presenceBody), m_Entity(static_cast<uint64_t>(entity))
			{
			}

			bool ShouldCollide(const JPH::BodyID& body) const override
			{
				return body != m_PresenceBody;
			}

			bool ShouldCollideLocked(const JPH::Body& body) const override
			{
				return body.GetUserData() != m_Entity;
			}
		private:
			JPH::BodyID m_PresenceBody;
			uint64_t m_Entity;
		};

		// Entities whose controllers ApplyChanges looks at, in the order they were recorded.
		class DirtyList
		{
		public:
			void Add(entt::entity handle)
			{
				if (m_Set.insert(handle).second)
					m_List.push_back(handle);
			}

			bool IsEmpty() const { return m_List.empty(); }

			void TakeInto(std::vector<entt::entity>& list)
			{
				list.clear();
				m_List.swap(list);
				m_Set.clear();
			}
		private:
			std::vector<entt::entity> m_List;
			std::unordered_set<entt::entity> m_Set;
		};

	}

	struct CharacterControllerSetData
	{
		Scene* OwnerScene = nullptr;
		JPH::PhysicsSystem* System = nullptr;
		const PhysicsSettings* Settings = nullptr;
		std::map<entt::entity, ControllerRecord> Controllers; // Ordered: steps and their effects follow entity order
		DirtyList Dirty;
		bool RecheckAll = false;
		size_t CheckedPendingDestroys = 0; // Scene::GetPendingDestroys().size() when last checked
		std::unordered_set<UUID> WarnedEntities; // Controllers ignored because their entity has a RigidBody (warned once)
		std::unordered_set<UUID> FullWorldEntities; // Controllers without a presence body because the world was full (warned once)
		ControllerContactListener Listener;
		// Per batch of a step's parallel moves (see Step), and one for the main thread's queries.
		std::vector<Scope<JPH::TempAllocatorImplWithMallocFallback>> Allocators;
		Scope<JPH::TempAllocatorImplWithMallocFallback> MainAllocator;
		std::vector<ControllerRecord*> StepRecords;
		std::vector<entt::entity> ChangeScratch;
		std::vector<JPH::BodyID> NewPresenceBodies;
		uint32_t WaitingPresenceBodies = 0; // Records with WaitsForPresenceSlot
		float LastStepTime = 0.0f;

		// Declared last: disconnected before anything the handlers use is destroyed.
		std::vector<entt::scoped_connection> Connections;
	};

	namespace
	{

		void OnControllerChanged(CharacterControllerSetData& data, entt::registry&, entt::entity handle)
		{
			data.Dirty.Add(handle);
		}

		void OnRigidBodyChanged(CharacterControllerSetData& data, entt::registry& registry, entt::entity handle)
		{
			// A rigid body on a controller's entity takes over, and a controller whose rigid body goes comes back.
			if (registry.all_of<CharacterControllerComponent>(handle))
				data.Dirty.Add(handle);
		}

		void OnHierarchyChanged(CharacterControllerSetData& data, entt::registry&, entt::entity)
		{
			// Activity and parents can change anywhere below; such changes are rare, and checking every controller is cheap.
			data.RecheckAll = true;
		}

		JPH::TempAllocator& GetMainAllocator(CharacterControllerSetData& data)
		{
			if (!data.MainAllocator)
				data.MainAllocator = CreateScope<JPH::TempAllocatorImplWithMallocFallback>(c_ScratchBytesPerBatch);
			return *data.MainAllocator;
		}

		// Whether an entity is simulated as a controller: an enabled component, no rigid body, active and not about to be
		// destroyed.
		bool IsControllerWanted(CharacterControllerSetData& data, Entity entity)
		{
			if (!entity.IsValid())
				return false;
			const CharacterControllerComponent* component = entity.TryGetComponent<CharacterControllerComponent>();
			if (!component || !component->Enabled)
				return false;
			if (entity.HasComponent<RigidBodyComponent>())
			{
				if (data.WarnedEntities.insert(entity.GetUUID()).second)
					ST_CORE_WARN("Physics: '{}' has both a Rigid Body and a Character Controller; it is simulated as the rigid body (remove one of them)", entity.GetName());
				return false;
			}
			const Scene& scene = *data.OwnerScene;
			return scene.IsActiveInHierarchy(entity) && !IsPendingDestroyInHierarchy(scene, entity);
		}

		void RefreshSettings(ControllerRecord& record, const CharacterControllerComponent& component)
		{
			const CharacterControllerComponent defaults;
			record.GravityScale = std::isfinite(component.GravityScale) ? component.GravityScale : defaults.GravityScale;
			record.PushStrength = std::isfinite(component.PushStrength) ? std::max(component.PushStrength, 0.0f) : defaults.PushStrength;
			record.StepHeight = std::isfinite(component.StepHeight) ? std::max(component.StepHeight, 0.0f) : defaults.StepHeight;
			if (record.Character)
			{
				const float maxSlope = std::isfinite(component.MaxSlopeDegrees) ? std::clamp(component.MaxSlopeDegrees, 0.0f, 89.0f) : defaults.MaxSlopeDegrees;
				record.Character->SetMaxSlopeAngle(glm::radians(maxSlope));
			}
		}

		void DestroyPresenceBody(CharacterControllerSetData& data, ControllerRecord& record)
		{
			if (record.PresenceBody.IsInvalid())
				return;
			JPH::BodyInterface& bodies = data.System->GetBodyInterface();
			bodies.RemoveBody(record.PresenceBody);
			bodies.DestroyBody(record.PresenceBody);
			record.PresenceBody = JPH::BodyID();
		}

		// The Jolt character of a record, with its capsule `shape`, standing at `position` (its feet) in the set's Jolt system.
		JPH::Ref<JPH::CharacterVirtual> CreateCharacter(CharacterControllerSetData& data, ControllerRecord& record, const JPH::Shape* shape, const glm::vec3& position)
		{
			JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
			settings->mShape = shape;
			settings->mUp = JPH::Vec3::sAxisY();
			settings->mCharacterPadding = c_CharacterPadding;
			// Contacts below the center of the capsule's lower sphere can hold it up.
			settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -record.Shape.Radius);
			// The set presses down what the character stands on and pushes bodies itself (see Step), after every move.
			settings->mMass = 0.0f;
			settings->mMaxStrength = 0.0f;
			JPH::Ref<JPH::CharacterVirtual> character = new JPH::CharacterVirtual(settings, ToJoltPosition(position), JPH::Quat::sIdentity(),
				static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&record)), data.System);
			character->SetListener(&data.Listener);
			return character;
		}

		void SetWaitsForPresenceSlot(CharacterControllerSetData& data, ControllerRecord& record, bool waits)
		{
			if (record.WaitsForPresenceSlot == waits)
				return;
			record.WaitsForPresenceSlot = waits;
			if (waits)
				data.WaitingPresenceBodies++;
			else
				data.WaitingPresenceBodies--;
		}

		// The presence body of a record at `position`: kinematic, on the controller's layer. Moving wakes it (triggers only
		// notice awake bodies); standing still it falls asleep like any body, so that its contacts with sleeping bodies (a
		// crate on its head) last. It joins the simulation with the batch of ApplyChanges. A full world leaves the controller
		// without one until the world grows (see Rebind).
		void CreatePresenceBody(CharacterControllerSetData& data, ControllerRecord& record, const Entity& entity, const glm::vec3& position)
		{
			SetWaitsForPresenceSlot(data, record, false);
			const JPH::RefConst<JPH::Shape> presenceShape = CreateCapsule(record.Shape, c_PresenceRadiusFraction);
			if (!presenceShape)
				return;
			if (data.System->GetNumBodies() < data.System->GetMaxBodies())
			{
				JPH::BodyCreationSettings bodySettings(presenceShape, GetPresencePosition(position), JPH::Quat::sIdentity(), JPH::EMotionType::Kinematic,
					PhysicsLayers::MakeObjectLayer(record.Shape.Layer, true));
				bodySettings.mUserData = static_cast<uint64_t>(record.EntityID);
				if (JPH::Body* body = data.System->GetBodyInterface().CreateBody(bodySettings))
				{
					record.PresenceBody = body->GetID();
					data.NewPresenceBodies.push_back(record.PresenceBody);
				}
			}
			SetWaitsForPresenceSlot(data, record, record.PresenceBody.IsInvalid());
			if (record.WaitsForPresenceSlot && data.FullWorldEntities.insert(record.EntityID).second)
			{
				ST_CORE_WARN("Physics: the world has no room for the presence body of '{}' (its body limit is reached): it moves, but bodies, "
					"triggers and queries do not see it until the world grows", entity.GetName());
			}
		}

		// New presence bodies join the simulation in one batch.
		void AddNewPresenceBodies(CharacterControllerSetData& data)
		{
			if (data.NewPresenceBodies.empty())
				return;
			JPH::BodyInterface& bodies = data.System->GetBodyInterface();
			const int count = static_cast<int>(data.NewPresenceBodies.size());
			const JPH::BodyInterface::AddState state = bodies.AddBodiesPrepare(data.NewPresenceBodies.data(), count);
			bodies.AddBodiesFinalize(data.NewPresenceBodies.data(), count, state, JPH::EActivation::Activate);
			data.NewPresenceBodies.clear();
		}

		// Builds the Jolt character and the presence body of a record at `position` (its feet), keeping the record's gameplay
		// state. The presence body joins the simulation with the batch of ApplyChanges.
		bool BuildCharacter(CharacterControllerSetData& data, ControllerRecord& record, Entity entity, const glm::vec3& position)
		{
			const CharacterControllerComponent& component = entity.GetComponent<CharacterControllerComponent>();
			record.Shape = GetShapeOf(component);
			const JPH::RefConst<JPH::Shape> shape = CreateCapsule(record.Shape, 1.0f);
			if (!shape)
			{
				ST_CORE_WARN("Physics: the Character Controller of '{}' has an unusable shape (height {}, radius {}); it is not simulated", entity.GetName(),
					component.Height, component.Radius);
				return false;
			}

			record.Character = CreateCharacter(data, record, shape.GetPtr(), position);
			RefreshSettings(record, component);
			CreatePresenceBody(data, record, entity, position);

			// Where it stands right away: the ground state is valid before its first step.
			const ControllerLayerFilter layers(*data.Settings, record.Shape.Layer);
			const ControllerBodyFilter bodyFilter(record.PresenceBody, record.EntityID);
			record.Character->RefreshContacts(JPH::BroadPhaseLayerFilter(), layers, bodyFilter, JPH::ShapeFilter(), GetMainAllocator(data));
			record.Position = position;
			return true;
		}

		void RemoveController(CharacterControllerSetData& data, std::map<entt::entity, ControllerRecord>::iterator it, std::vector<CharacterBodyChange>& outChanges)
		{
			ControllerRecord& record = it->second;
			const JPH::BodyID body = record.PresenceBody;
			DestroyPresenceBody(data, record);
			SetWaitsForPresenceSlot(data, record, false);
			outChanges.push_back({ record.EntityID, body.GetIndexAndSequenceNumber(), c_NoCharacterBody, true });
			data.Controllers.erase(it);
		}

		// Creates, rebuilds or removes the controller of one entity after a change.
		void RefreshController(CharacterControllerSetData& data, entt::entity handle, std::vector<CharacterBodyChange>& outChanges)
		{
			const Entity entity(handle, data.OwnerScene);
			auto it = data.Controllers.find(handle);
			if (!IsControllerWanted(data, entity))
			{
				if (it != data.Controllers.end())
					RemoveController(data, it, outChanges);
				return;
			}

			const CharacterControllerComponent& component = entity.GetComponent<CharacterControllerComponent>();
			if (it != data.Controllers.end())
			{
				ControllerRecord& record = it->second;
				if (GetShapeOf(component) == record.Shape)
				{
					RefreshSettings(record, component);
					return;
				}
				// A new shape or layer: a new character and presence body where the old one stood, walking on as before.
				const JPH::BodyID oldBody = record.PresenceBody;
				DestroyPresenceBody(data, record);
				const glm::vec3 position = ToGlm(JPH::Vec3(record.Character->GetPosition()));
				const JPH::Vec3 velocity = record.Character->GetLinearVelocity();
				if (!BuildCharacter(data, record, entity, position))
				{
					SetWaitsForPresenceSlot(data, record, false);
					outChanges.push_back({ record.EntityID, oldBody.GetIndexAndSequenceNumber(), c_NoCharacterBody, true });
					data.Controllers.erase(it);
					return;
				}
				record.Character->SetLinearVelocity(velocity);
				outChanges.push_back({ record.EntityID, oldBody.GetIndexAndSequenceNumber(), record.PresenceBody.GetIndexAndSequenceNumber(), false });
				return;
			}

			ControllerRecord& record = data.Controllers[handle];
			record.EntityID = entity.GetUUID();
			record.Handle = handle;
			const glm::vec3 position(data.OwnerScene->GetWorldTransform(entity)[3]);
			if (!IsFinite(position) || !BuildCharacter(data, record, entity, position))
			{
				if (!IsFinite(position))
					ST_CORE_WARN("Physics: the Character Controller of '{}' has a non-finite position; it is not simulated", entity.GetName());
				data.Controllers.erase(handle);
				return;
			}
			outChanges.push_back({ record.EntityID, c_NoCharacterBody, record.PresenceBody.GetIndexAndSequenceNumber(), false });
		}

		// Puts a controller (and its presence body) at a position, as if it had always stood there: no velocity carried over.
		void PlaceController(CharacterControllerSetData& data, ControllerRecord& record, const glm::vec3& position)
		{
			JPH::CharacterVirtual& character = *record.Character;
			character.SetPosition(ToJoltPosition(position));
			JPH::Vec3 velocity = character.GetLinearVelocity();
			velocity.SetY(0.0f);
			character.SetLinearVelocity(velocity);
			record.Position = position;
			record.Velocity = glm::vec3(0.0f);
			if (!record.PresenceBody.IsInvalid())
				data.System->GetBodyInterface().SetPositionAndRotation(record.PresenceBody, GetPresencePosition(position), JPH::Quat::sIdentity(), JPH::EActivation::Activate);
			const ControllerLayerFilter layers(*data.Settings, record.Shape.Layer);
			const ControllerBodyFilter bodyFilter(record.PresenceBody, record.EntityID);
			character.RefreshContacts(JPH::BroadPhaseLayerFilter(), layers, bodyFilter, JPH::ShapeFilter(), GetMainAllocator(data));
		}

		// One fixed step of one controller: its velocity from the gameplay requests, gravity and the ground, then Jolt's move.
		// Reads the world as it was before the step and writes only the record (see CharacterControllerSet::Step).
		void MoveController(const CharacterControllerSetData& data, ControllerRecord& record, float timestep, const glm::vec3& gravity, JPH::TempAllocator& allocator)
		{
			JPH::CharacterVirtual& character = *record.Character;
			record.Pushes.clear();
			character.UpdateGroundVelocity();

			const JPH::Vec3 up = JPH::Vec3::sAxisY();
			const JPH::Vec3 groundVelocity = character.GetGroundVelocity();
			// The vertical velocity it was given last step (after gravity), not the measured one: walking up a step moves it up
			// by the step's height within one step, which must not launch it.
			const float verticalVelocity = character.GetLinearVelocity().GetY();
			const bool onGround = character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
			JPH::Vec3 velocity;
			if (onGround && verticalVelocity - groundVelocity.GetY() < c_LeaveGroundSpeed)
			{
				// Standing: it moves with what it stands on.
				velocity = groundVelocity;
			}
			else
			{
				// In the air, on steep ground or leaving the ground: its vertical motion goes on (sliding down a steep slope
				// speeds up like falling), except that a ceiling stops a rise.
				float vertical = verticalVelocity;
				if (vertical > 0.0f && record.Velocity.y < vertical)
					vertical = std::max(record.Velocity.y, 0.0f);
				velocity = JPH::Vec3(0.0f, vertical, 0.0f);
			}
			if (record.JumpRequested)
				velocity.SetY((onGround ? groundVelocity.GetY() : 0.0f) + record.JumpSpeed);

			const glm::vec3 scaledGravity = gravity * record.GravityScale;
			velocity += ToJolt(scaledGravity) * timestep;
			// Walking is horizontal; the vertical part of the walk velocity counts only without gravity (flying, swimming).
			glm::vec3 walk = record.WalkVelocity;
			if (record.GravityScale != 0.0f)
				walk.y = 0.0f;
			velocity += ToJolt(walk) + ToJolt(record.Motion) / timestep;
			character.SetLinearVelocity(velocity);
			record.StepVelocity = velocity;

			JPH::CharacterVirtual::ExtendedUpdateSettings settings;
			settings.mWalkStairsStepUp = up * record.StepHeight;
			settings.mStickToFloorStepDown = -up * record.StepHeight;
			const ControllerLayerFilter layers(*data.Settings, record.Shape.Layer);
			const ControllerBodyFilter bodies(record.PresenceBody, record.EntityID);
			character.ExtendedUpdate(timestep, ToJolt(scaledGravity), settings, JPH::BroadPhaseLayerFilter(), layers, bodies, JPH::ShapeFilter(), allocator);

			record.NewPosition = ToGlm(JPH::Vec3(character.GetPosition()));
			record.Velocity = (record.NewPosition - record.Position) / timestep;
			record.Motion = glm::vec3(0.0f);
			record.JumpRequested = false;
		}

		// Pushes the dynamic bodies a controller ran into during its move, and presses on a dynamic body it stands on.
		//
		// A push is an inelastic contact (no bounce): the body gets the impulse that makes its point of contact move away from
		// the character as fast as the character closes in on it along the contact normal, so the character keeps its pace
		// instead of pressing into the body. That impulse is the closing speed times the body's mass as felt at that point
		// along the normal, its effective mass 1 / (1/m + (r x n) . I^-1 (r x n)), so a box pushed off its center turns as
		// well as slides; it is limited to the character's push strength (a force) over the step. It never points down: what
		// a character presses down on carries its weight instead. Overlaps get no push of their own: the character's move
		// steps out of them.
		//
		// On a dynamic body, the character's weight (c_CharacterMass under the scene's gravity, scaled like the character's)
		// presses on it at the ground contact: all of it while the character stands (what holds the character in place on a
		// slope holds the body too), and only the part across the surface while it slides down ground too steep to stand on.
		void PushBodies(CharacterControllerSetData& data, ControllerRecord& record, float timestep, const glm::vec3& gravity)
		{
			JPH::BodyInterface& bodies = data.System->GetBodyInterface();
			const float maxImpulse = record.PushStrength * timestep;
			for (const PushContact& push : record.Pushes)
			{
				glm::vec3 impulse(0.0f);
				{
					JPH::BodyLockRead lock(data.System->GetBodyLockInterface(), push.Body);
					if (!lock.SucceededAndIsInBroadPhase() || !lock.GetBody().IsDynamic())
						continue;
					const JPH::Body& body = lock.GetBody();
					// The normal points from the body to the character: the character closes in while the body's point moves
					// towards it faster than the character moves away.
					const float closingSpeed = (push.Velocity - record.StepVelocity).Dot(push.Normal);
					if (!(closingSpeed > 0.0f))
						continue;
					const JPH::Vec3 arm = JPH::Vec3(push.Position - body.GetCenterOfMassPosition());
					const JPH::Vec3 turn = arm.Cross(push.Normal);
					const float inverseMassAtContact = body.GetMotionProperties()->GetInverseMass() + turn.Dot(body.GetInverseInertia().Multiply3x3(turn));
					if (!(inverseMassAtContact > 0.0f))
						continue;
					const float magnitude = std::min(closingSpeed / inverseMassAtContact, maxImpulse);
					impulse = -magnitude * ToGlm(push.Normal);
					impulse.y = std::max(impulse.y, 0.0f);
				}
				// The lock is released first: AddImpulse locks the body itself.
				if (impulse != glm::vec3(0.0f))
					bodies.AddImpulse(push.Body, ToJolt(impulse), push.Position);
			}

			const JPH::CharacterVirtual& character = *record.Character;
			const JPH::BodyID ground = character.GetGroundBodyID();
			const JPH::CharacterBase::EGroundState state = character.GetGroundState();
			const bool standing = state == JPH::CharacterBase::EGroundState::OnGround;
			const bool sliding = state == JPH::CharacterBase::EGroundState::OnSteepGround;
			const glm::vec3 weight = c_CharacterMass * record.GravityScale * gravity;
			if (ground.IsInvalid() || (!standing && !sliding) || !(glm::dot(weight, weight) > 0.0f))
				return;
			glm::vec3 force = weight;
			if (sliding)
			{
				// The ground's normal points out of it: the weight presses into it as far as it points against the normal.
				const glm::vec3 normal = ToGlm(character.GetGroundNormal());
				const float into = -glm::dot(weight, normal);
				if (!(into > 0.0f))
					return;
				force = -into * normal;
			}
			{
				JPH::BodyLockRead lock(data.System->GetBodyLockInterface(), ground);
				if (!lock.SucceededAndIsInBroadPhase() || !lock.GetBody().IsDynamic())
					return;
			}
			bodies.AddImpulse(ground, ToJolt(force * timestep), character.GetGroundPosition());
		}

		// Writes a controller's new position to its entity's translation (its rotation and scale stay).
		bool WriteBack(CharacterControllerSetData& data, ControllerRecord& record)
		{
			if (!record.CanWrite)
				return false;
			TransformComponent& transform = data.OwnerScene->GetRegistry().get<TransformComponent>(record.Handle);
			transform.Translation = record.HasParent ? glm::vec3(record.InverseParentWorld * glm::vec4(record.NewPosition, 1.0f)) : record.NewPosition;
			return true;
		}

		void GatherContacts(const ControllerRecord& record, std::vector<CharacterContactReport>& outContacts)
		{
			const size_t first = outContacts.size();
			for (const JPH::CharacterContact& contact : record.Character->GetActiveContacts())
			{
				if (!contact.mHadCollision || contact.mWasDiscarded || contact.mIsSensorB || contact.mBodyB.IsInvalid() || contact.mUserData == static_cast<uint64_t>(record.EntityID))
					continue;
				// One contact per body (Jolt lists one per sub shape), the first in Jolt's deterministic order.
				const uint32_t other = contact.mBodyB.GetIndexAndSequenceNumber();
				const auto begin = outContacts.begin() + static_cast<std::ptrdiff_t>(first);
				if (std::any_of(begin, outContacts.end(), [other](const CharacterContactReport& report) { return report.OtherBody == other; }))
					continue;
				CharacterContactReport report;
				report.Controller = record.EntityID;
				report.Other = UUID(contact.mUserData);
				report.ControllerBody = record.PresenceBody.IsInvalid() ? c_NoCharacterBody : record.PresenceBody.GetIndexAndSequenceNumber();
				report.OtherBody = other;
				report.Point = ToGlm(JPH::Vec3(contact.mPosition));
				report.Normal = -ToGlm(contact.mContactNormal);
				outContacts.push_back(report);
			}
		}

		// The set's data is reached through a Scope, also from const members, so one non-const lookup serves both.
		ControllerRecord* FindController(CharacterControllerSetData& data, Entity entity)
		{
			if (!entity.IsValid() || entity.GetScene() != data.OwnerScene)
				return nullptr;
			auto it = data.Controllers.find(entity.GetHandle());
			return it != data.Controllers.end() ? &it->second : nullptr;
		}

	}

	CharacterControllerSet::CharacterControllerSet(Scene& scene, JPH::PhysicsSystem& system, const PhysicsSettings& settings)
		: m_Data(CreateScope<CharacterControllerSetData>())
	{
		CharacterControllerSetData& data = *m_Data;
		data.OwnerScene = &scene;
		data.System = &system;
		data.Settings = &settings;

		entt::registry& registry = scene.GetRegistry();
		data.Connections.emplace_back(registry.on_construct<CharacterControllerComponent>().connect<&OnControllerChanged>(data));
		data.Connections.emplace_back(registry.on_update<CharacterControllerComponent>().connect<&OnControllerChanged>(data));
		data.Connections.emplace_back(registry.on_destroy<CharacterControllerComponent>().connect<&OnControllerChanged>(data));
		data.Connections.emplace_back(registry.on_construct<RigidBodyComponent>().connect<&OnRigidBodyChanged>(data));
		data.Connections.emplace_back(registry.on_destroy<RigidBodyComponent>().connect<&OnRigidBodyChanged>(data));
		data.Connections.emplace_back(registry.on_construct<InactiveComponent>().connect<&OnHierarchyChanged>(data));
		data.Connections.emplace_back(registry.on_destroy<InactiveComponent>().connect<&OnHierarchyChanged>(data));
		data.Connections.emplace_back(registry.on_update<RelationshipComponent>().connect<&OnHierarchyChanged>(data));
		// Every entity has an IDComponent until it is destroyed, so its removal reports entity destruction.
		data.Connections.emplace_back(registry.on_destroy<IDComponent>().connect<&OnControllerChanged>(data));

		for (const auto handle : registry.view<CharacterControllerComponent>())
			data.Dirty.Add(handle);
	}

	CharacterControllerSet::~CharacterControllerSet()
	{
		CharacterControllerSetData& data = *m_Data;
		data.Connections.clear();
		for (auto& [handle, record] : data.Controllers)
			DestroyPresenceBody(data, record);
		data.Controllers.clear();
	}

	uint32_t CharacterControllerSet::CountCandidates(const Scene& scene)
	{
		const auto* storage = scene.GetRegistry().storage<CharacterControllerComponent>();
		return storage ? static_cast<uint32_t>(storage->size()) : 0;
	}

	void CharacterControllerSet::ApplyChanges(std::vector<CharacterBodyChange>& outChanges)
	{
		CharacterControllerSetData& data = *m_Data;
		const size_t pendingDestroys = data.OwnerScene->GetPendingDestroys().size();
		const bool newDestroys = pendingDestroys != data.CheckedPendingDestroys;
		if (data.Dirty.IsEmpty() && !data.RecheckAll && !newDestroys)
			return;

		ST_PROFILE_FUNCTION();
		data.CheckedPendingDestroys = pendingDestroys;
		std::vector<entt::entity>& handles = data.ChangeScratch;
		data.Dirty.TakeInto(handles);
		if (data.RecheckAll)
		{
			// Activity or parents changed: every entity with a controller component may join or leave.
			for (const entt::entity handle : data.OwnerScene->GetRegistry().view<CharacterControllerComponent>())
				handles.push_back(handle);
		}
		else if (newDestroys && pendingDestroys > 0)
		{
			for (const auto& [handle, record] : data.Controllers)
				handles.push_back(handle);
		}
		data.RecheckAll = false;

		// In entity order, whatever the order of the signals.
		std::sort(handles.begin(), handles.end());
		handles.erase(std::unique(handles.begin(), handles.end()), handles.end());
		for (const entt::entity handle : handles)
			RefreshController(data, handle, outChanges);

		AddNewPresenceBodies(data);
	}

	void CharacterControllerSet::Rebind(JPH::PhysicsSystem& system, std::vector<CharacterBodyChange>& outChanges)
	{
		ST_PROFILE_FUNCTION();
		CharacterControllerSetData& data = *m_Data;
		data.System = &system;
		PhysicsStateBuffer state;
		for (auto& [handle, record] : data.Controllers)
		{
			// The same capsule in the new system, with everything the old one knew: position, velocity, ground state and the
			// contacts its next move starts from (they name bodies by ID, which the growth kept).
			const JPH::Ref<JPH::CharacterVirtual> previous = record.Character;
			state.Clear();
			previous->SaveState(state);
			record.Character = CreateCharacter(data, record, previous->GetShape(), ToGlm(JPH::Vec3(previous->GetPosition())));
			record.Character->RestoreState(state);
			const Entity entity(handle, data.OwnerScene);
			if (const CharacterControllerComponent* component = entity.TryGetComponent<CharacterControllerComponent>())
				RefreshSettings(record, *component);
			if (state.IsFailed())
				ST_CORE_ERROR("Physics: the character controller of '{}' could not keep its state while its world grew", entity.GetName());

			// Controllers the full world left without a presence body get one now.
			if (record.WaitsForPresenceSlot)
			{
				CreatePresenceBody(data, record, entity, record.Position);
				if (!record.PresenceBody.IsInvalid())
				{
					data.FullWorldEntities.erase(record.EntityID);
					outChanges.push_back({ record.EntityID, c_NoCharacterBody, record.PresenceBody.GetIndexAndSequenceNumber(), false });
				}
			}
		}
		AddNewPresenceBodies(data);
	}

	uint32_t CharacterControllerSet::GetWaitingPresenceBodyCount() const
	{
		return m_Data->WaitingPresenceBodies;
	}

	void CharacterControllerSet::Step(float timestep, const glm::vec3& gravity, bool collectContacts, CharacterStepOutput& output)
	{
		CharacterControllerSetData& data = *m_Data;
		output.Clear();
		if (data.Controllers.empty())
		{
			data.LastStepTime = 0.0f;
			return;
		}

		ST_PROFILE_FUNCTION();
		using Clock = std::chrono::steady_clock;
		const Clock::time_point start = Clock::now();
		Scene& scene = *data.OwnerScene;

		// Before the moves, on the main thread: entities moved from outside teleport their controllers, and the parents' world
		// transforms are read for the write-back.
		std::vector<ControllerRecord*>& records = data.StepRecords;
		records.clear();
		for (auto& [handle, record] : data.Controllers)
		{
			const Entity entity(handle, &scene);
			const glm::vec3 position(scene.GetWorldTransform(entity)[3]);
			if (IsFinite(position) && !IsNearlySamePosition(position, record.Position))
				PlaceController(data, record, position);
			const Entity parent = entity.GetParent();
			record.HasParent = parent.IsValid();
			record.CanWrite = true;
			if (record.HasParent)
			{
				const glm::mat4 parentWorld = scene.GetWorldTransform(parent);
				record.CanWrite = glm::abs(glm::determinant(parentWorld)) > Scene::c_MinInvertibleDeterminant;
				if (record.CanWrite)
					record.InverseParentWorld = glm::inverse(parentWorld);
			}
			records.push_back(&record);
		}

		// The moves: in parallel batches, each with scratch memory of its own.
		const uint32_t count = static_cast<uint32_t>(records.size());
		const uint32_t threads = JobSystem::IsInitialized() ? JobSystem::GetWorkerThreadCount() + 1 : 1;
		const uint32_t batchSize = std::max(c_MinBatchSize, (count + threads * c_BatchesPerThread - 1) / (threads * c_BatchesPerThread));
		const uint32_t batches = (count + batchSize - 1) / batchSize;
		while (data.Allocators.size() < batches)
			data.Allocators.push_back(CreateScope<JPH::TempAllocatorImplWithMallocFallback>(c_ScratchBytesPerBatch));
		const auto move = [&](uint32_t begin, uint32_t end)
		{
			JPH::TempAllocator& allocator = *data.Allocators[begin / batchSize];
			for (uint32_t index = begin; index < end; index++)
				MoveController(data, *records[index], timestep, gravity, allocator);
		};
		if (batches > 1 && JobSystem::IsInitialized())
			JobSystem::ParallelFor(count, batchSize, move);
		else
			move(0, count);

		// After the moves, on the main thread in controller order: write the entities, move the presence bodies, push.
		JPH::BodyInterface& bodies = data.System->GetBodyInterface();
		for (ControllerRecord* record : records)
		{
			if (record->NewPosition != record->Position && IsFinite(record->NewPosition))
			{
				if (WriteBack(data, *record))
				{
					output.Moved.push_back(record->Handle);
					record->Position = record->NewPosition;
				}
				else
				{
					// Its parent cannot be inverted, so its entity cannot be placed: it stays where its entity is.
					record->Character->SetPosition(ToJoltPosition(record->Position));
				}
				if (!record->PresenceBody.IsInvalid())
					bodies.SetPositionAndRotation(record->PresenceBody, GetPresencePosition(record->Position), JPH::Quat::sIdentity(), JPH::EActivation::Activate);
			}
			PushBodies(data, *record, timestep, gravity);
			if (collectContacts)
				GatherContacts(*record, output.Contacts);
			if (record->PresenceBody.IsInvalid())
				output.WithoutPresenceBody.push_back(record->EntityID);
		}
		scene.MarkTransformsChanged(output.Moved);
		data.LastStepTime = std::chrono::duration<float, std::milli>(Clock::now() - start).count();
	}

	bool CharacterControllerSet::Has(Entity entity) const
	{
		return FindController(*m_Data, entity) != nullptr;
	}

	bool CharacterControllerSet::Move(Entity entity, const glm::vec3& velocity)
	{
		ControllerRecord* record = FindController(*m_Data, entity);
		if (!record || !IsFinite(velocity))
			return false;
		record->WalkVelocity = velocity;
		return true;
	}

	bool CharacterControllerSet::Jump(Entity entity, float speed)
	{
		ControllerRecord* record = FindController(*m_Data, entity);
		if (!record || !std::isfinite(speed))
			return false;
		record->JumpRequested = true;
		record->JumpSpeed = speed;
		return true;
	}

	bool CharacterControllerSet::Teleport(Entity entity, const glm::vec3& position)
	{
		CharacterControllerSetData& data = *m_Data;
		ControllerRecord* record = FindController(data, entity);
		if (!record || !IsFinite(position))
			return false;

		Scene& scene = *data.OwnerScene;
		const Entity parent = entity.GetParent();
		record->HasParent = parent.IsValid();
		if (record->HasParent)
		{
			const glm::mat4 parentWorld = scene.GetWorldTransform(parent);
			if (!(glm::abs(glm::determinant(parentWorld)) > Scene::c_MinInvertibleDeterminant))
				return false;
			record->InverseParentWorld = glm::inverse(parentWorld);
		}
		record->CanWrite = true;
		record->NewPosition = position;
		WriteBack(data, *record);
		scene.MarkTransformChanged(entity);
		PlaceController(data, *record, position);
		return true;
	}

	bool CharacterControllerSet::AddMotion(Entity entity, const glm::vec3& displacement)
	{
		ControllerRecord* record = FindController(*m_Data, entity);
		if (!record || !IsFinite(displacement))
			return false;
		record->Motion += displacement;
		return true;
	}

	std::optional<CharacterState> CharacterControllerSet::GetState(Entity entity) const
	{
		const ControllerRecord* record = FindController(*m_Data, entity);
		if (!record)
			return std::nullopt;

		const JPH::CharacterVirtual& character = *record->Character;
		CharacterState state;
		state.Ground = ToGroundState(character.GetGroundState());
		state.Velocity = record->Velocity;
		if (state.Ground != CharacterGroundState::InAir)
		{
			state.GroundNormal = ToGlm(character.GetGroundNormal());
			state.GroundVelocity = ToGlm(character.GetGroundVelocity());
			if (!character.GetGroundBodyID().IsInvalid())
				state.GroundEntity = m_Data->OwnerScene->GetEntityByUUID(UUID(character.GetGroundUserData()));
		}
		return state;
	}

	uint32_t CharacterControllerSet::GetPresenceBody(UUID entity) const
	{
		const Entity found = m_Data->OwnerScene->GetEntityByUUID(entity);
		const ControllerRecord* record = FindController(*m_Data, found);
		return record && !record->PresenceBody.IsInvalid() ? record->PresenceBody.GetIndexAndSequenceNumber() : c_NoCharacterBody;
	}

	uint32_t CharacterControllerSet::GetCount() const
	{
		return static_cast<uint32_t>(m_Data->Controllers.size());
	}

	float CharacterControllerSet::GetLastStepTime() const
	{
		return m_Data->LastStepTime;
	}

}
