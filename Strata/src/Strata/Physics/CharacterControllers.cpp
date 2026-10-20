#include "stpch.h"
#include "Strata/Physics/CharacterControllers.h"

#include "Strata/Core/JobSystem.h"
#include "Strata/Math/Math.h"
#include "Strata/Physics/PhysicsComponents.h"
#include "Strata/Physics/PhysicsFilters.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

#include <box3d/box3d.h>
#include <box3d/collision.h>
#include <box3d/math_functions.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <unordered_set>

namespace Strata
{

	namespace
	{

		// What a character weighs (kilograms, an adult person): its weight presses on a dynamic body it stands on (PushBodies).
		constexpr float c_CharacterMass = 70.0f;
		// The presence body's radius is this fraction of the character's, so that it stays clear of what the character touches
		// on its sides and meets only what moves into the character; its top is the character's (a crate on its head rests where
		// the character's head is) and its bottom stays above the ground.
		constexpr float c_PresenceRadiusFraction = 0.9f;
		// Smallest radius and height of a controller's capsule (world units), so box3d never sees a degenerate shape, and the
		// shortest segment of its capsule (box3d's capsules need one; a spherical character is this much taller).
		constexpr float c_MinCharacterExtent = 1.0e-3f;
		constexpr float c_MinCapsuleSegment = 0.01f;
		// Moving away from the ground faster than this (meters per second) leaves it: a jump or a launch. Slower, a character
		// on the ground takes on the ground's velocity (a moving platform).
		constexpr float c_LeaveGroundSpeed = 0.1f;
		// An entity whose world position differs from where its controller put it by more than this (relative to the
		// position's magnitude, at least this many meters) was moved from outside: the controller teleports there.
		constexpr float c_ExternalMoveTolerance = 1.0e-4f;
		// How far around its capsule a controller looks for what it touches (the ground below it, walls beside it): box3d's
		// mover keeps a capsule within its linear slop (5 mm) of what it rests on.
		constexpr float c_ContactMargin = 0.02f;
		// Mover iterations of a pass, and the movement below which a pass is done.
		constexpr int c_MoveIterations = 4;
		constexpr float c_MoveTolerance = 1.0e-4f;
		// Planes a controller considers at once (the solver's input); more touching shapes than this are rare.
		constexpr size_t c_MaxPlanes = 32;
		// Controllers moved per job (fixed by the count, so batches do not depend on the threads).
		constexpr uint32_t c_MinBatchSize = 16;
		constexpr uint32_t c_BatchesPerThread = 4;
		static_assert(sizeof(void*) >= sizeof(uint64_t), "Shapes carry their entity's UUID in their user data");

		b3Vec3 ToB3(const glm::vec3& value)
		{
			return b3Vec3 { value.x, value.y, value.z };
		}

		glm::vec3 ToGlm(const b3Vec3& value)
		{
			return glm::vec3(value.x, value.y, value.z);
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

		bool IsPendingDestroyInHierarchy(const Scene& scene, Entity entity)
		{
			for (Entity current = entity; current.IsValid(); current = current.GetParent())
			{
				if (scene.IsPendingDestroy(current))
					return true;
			}
			return false;
		}

		// The entity whose body a shape belongs to (PhysicsWorld and the presence bodies put its UUID in the shape's user data).
		UUID GetShapeEntity(b3ShapeId shape)
		{
			return UUID(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(b3Shape_GetUserData(shape))));
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

		// The controller's capsule relative to its feet: standing on the origin with the full radius (times radiusFraction, its
		// bottom staying where the full capsule's lower sphere is centered minus that radius), its top at the height.
		b3Capsule MakeCapsule(const ControllerShape& shape, float radiusFraction, float inflate = 0.0f)
		{
			const float radius = shape.Radius * radiusFraction + inflate;
			const float bottom = shape.Radius;
			const float top = std::max(shape.Height - shape.Radius * radiusFraction, bottom + c_MinCapsuleSegment);
			return b3Capsule { b3Vec3 { 0.0f, bottom, 0.0f }, b3Vec3 { 0.0f, top, 0.0f }, radius };
		}

		// A dynamic body a controller walked into during its move: pushed after every controller moved (see Step).
		struct PushContact
		{
			b3BodyId Body = b3_nullBodyId;
			glm::vec3 Position = glm::vec3(0.0f);
			glm::vec3 Normal = glm::vec3(0.0f);   // Towards the character
			glm::vec3 Velocity = glm::vec3(0.0f); // Of the body at the contact point
		};

		// A plane a mover query found, with what it came from (relative to the query's origin).
		struct PlaneSource
		{
			b3ShapeId Shape = b3_nullShapeId;
			glm::vec3 Point = glm::vec3(0.0f);
		};

		// What a controller stands on (see FindGround).
		struct GroundContact
		{
			CharacterGroundState State = CharacterGroundState::InAir;
			glm::vec3 Normal = glm::vec3(0.0f);
			glm::vec3 Point = glm::vec3(0.0f); // World space
			glm::vec3 Velocity = glm::vec3(0.0f);
			b3BodyId Body = b3_nullBodyId;
			UUID Entity = UUID::Null();
		};

		struct ControllerRecord
		{
			UUID EntityID = UUID::Null();
			entt::entity Handle = entt::null;
			b3BodyId PresenceBody = b3_nullBodyId;
			b3ShapeId PresenceShape = b3_nullShapeId;
			ControllerShape Shape;
			// Settings applied without rebuilding (refreshed from the component on every change).
			float GravityScale = 1.0f;
			float PushStrength = 200.0f;
			float StepHeight = 0.35f;
			float CosMaxSlope = std::cos(glm::radians(45.0f));
			// Gameplay requests (ICharacterControl).
			glm::vec3 WalkVelocity = glm::vec3(0.0f);
			glm::vec3 Motion = glm::vec3(0.0f);
			float JumpSpeed = 0.0f;
			bool JumpRequested = false;
			// Where the controller put its entity, the velocity it moved with in its last step, the velocity it was given
			// besides walking (gravity, jumps, the ground's motion, after collisions: what the next step goes on with off the
			// ground) and what it stands on.
			glm::vec3 Position = glm::vec3(0.0f);
			glm::vec3 Velocity = glm::vec3(0.0f);
			glm::vec3 CarriedVelocity = glm::vec3(0.0f);
			GroundContact Ground;
			// Step scratch: written by the controller's own move (on any thread), read afterwards on the main thread.
			glm::vec3 NewPosition = glm::vec3(0.0f);
			glm::vec3 StepVelocity = glm::vec3(0.0f);
			std::vector<PushContact> Pushes;
			std::vector<CharacterContactReport> Touching;
			bool HasParent = false;
			bool CanWrite = true;
			glm::mat4 InverseParentWorld = glm::mat4(1.0f);
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
		b3WorldId World = b3_nullWorldId;
		const PhysicsSettings* Settings = nullptr;
		std::map<entt::entity, ControllerRecord> Controllers; // Ordered: steps and their effects follow entity order
		DirtyList Dirty;
		bool RecheckAll = false;
		size_t CheckedPendingDestroys = 0; // Scene::GetPendingDestroys().size() when last checked
		std::unordered_set<UUID> WarnedEntities; // Controllers ignored because their entity has a RigidBody (warned once)
		std::vector<ControllerRecord*> StepRecords;
		std::vector<entt::entity> ChangeScratch;
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
			const float maxSlope = std::isfinite(component.MaxSlopeDegrees) ? std::clamp(component.MaxSlopeDegrees, 0.0f, 89.0f) : defaults.MaxSlopeDegrees;
			record.CosMaxSlope = std::cos(glm::radians(maxSlope));
		}

		void DestroyPresenceBody(ControllerRecord& record)
		{
			if (B3_IS_NULL(record.PresenceBody))
				return;
			b3DestroyBody(record.PresenceBody);
			record.PresenceBody = b3_nullBodyId;
			record.PresenceShape = b3_nullShapeId;
		}

		// The presence body of a record at `position`: kinematic, on the controller's layer, seen by sensors. Moving wakes it;
		// standing still it falls asleep like any body, so that its contacts with sleeping bodies (a crate on its head) last.
		void CreatePresenceBody(CharacterControllerSetData& data, ControllerRecord& record, const glm::vec3& position)
		{
			b3BodyDef bodyDefinition = b3DefaultBodyDef();
			bodyDefinition.type = b3_kinematicBody;
			bodyDefinition.position = ToB3(position);
			record.PresenceBody = b3CreateBody(data.World, &bodyDefinition);
			b3ShapeDef shapeDefinition = b3DefaultShapeDef();
			shapeDefinition.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint64_t>(record.EntityID)));
			shapeDefinition.filter = PhysicsFilters::MakeShapeFilter(record.Shape.Layer, data.Settings->LayerCollisionMasks);
			shapeDefinition.enableSensorEvents = true;
			shapeDefinition.enableContactEvents = true;
			const b3Capsule capsule = MakeCapsule(record.Shape, c_PresenceRadiusFraction);
			record.PresenceShape = b3CreateCapsuleShape(record.PresenceBody, &shapeDefinition, &capsule);
		}

		void MovePresenceBody(ControllerRecord& record)
		{
			if (B3_IS_NULL(record.PresenceBody))
				return;
			// Teleported (no velocity), as the character's move put it: a velocity would carry what rests on it by friction and
			// push what it touches harder than the character's push strength.
			b3Body_SetTransform(record.PresenceBody, ToB3(record.Position), b3Quat_identity);
			b3Body_SetAwake(record.PresenceBody, true);
		}

		//////////////////////////////////////////////////////////////////////////
		// Mover queries (read-only: any thread)
		//////////////////////////////////////////////////////////////////////////

		// What a controller's queries need: the world, the layer filter, itself (its own shapes and presence body are skipped),
		// and where the planes go.
		struct MoverQuery
		{
			b3WorldId World = b3_nullWorldId;
			b3QueryFilter Filter = {};
			UUID Self = UUID::Null();
			std::vector<b3CollisionPlane>* Planes = nullptr;
			std::vector<PlaneSource>* Sources = nullptr;
		};

		bool AcceptMoverShape(b3ShapeId shape, void* context)
		{
			const MoverQuery& query = *static_cast<const MoverQuery*>(context);
			return !b3Shape_IsSensor(shape) && GetShapeEntity(shape) != query.Self;
		}

		bool CollectPlanes(b3ShapeId shape, const b3PlaneResult* planes, int planeCount, void* context)
		{
			MoverQuery& query = *static_cast<MoverQuery*>(context);
			if (!AcceptMoverShape(shape, context))
				return true;
			for (int index = 0; index < planeCount && query.Planes->size() < c_MaxPlanes; index++)
			{
				b3CollisionPlane plane = {};
				plane.plane = planes[index].plane;
				plane.pushLimit = FLT_MAX;
				plane.push = 0.0f;
				plane.clipVelocity = true;
				query.Planes->push_back(plane);
				query.Sources->push_back(PlaneSource { shape, ToGlm(planes[index].point) });
			}
			return true;
		}

		// The planes of what a capsule at `position` overlaps (relative to the position).
		void CollidePlanes(MoverQuery& query, const glm::vec3& position, const b3Capsule& capsule)
		{
			query.Planes->clear();
			query.Sources->clear();
			b3World_CollideMover(query.World, ToB3(position), &capsule, query.Filter, &CollectPlanes, &query);
		}

		// How far along `translation` a capsule at `position` moves before it touches something (0 to 1).
		float CastCapsule(MoverQuery& query, const glm::vec3& position, const b3Capsule& capsule, const glm::vec3& translation)
		{
			if (!(glm::dot(translation, translation) > 0.0f))
				return 1.0f;
			return b3World_CastMover(query.World, ToB3(position), &capsule, ToB3(translation), query.Filter, &AcceptMoverShape, &query);
		}

		enum class PassKind : uint8_t
		{
			Across,   // Walking: ground too steep to stand on blocks like a wall, walkable ground is climbed
			Vertical, // Falling or rising: walkable ground holds the capsule, steeper ground lets it slide down
			Standing  // Standing on the ground (an edge of a step included): whatever faces up holds the capsule
		};

		// Adapts the planes to a pass (see PassKind).
		void ShapePlanes(std::vector<b3CollisionPlane>& planes, PassKind pass, float cosMaxSlope)
		{
			for (b3CollisionPlane& plane : planes)
			{
				b3Vec3& normal = plane.plane.normal;
				const bool upward = normal.y > 1.0e-3f;
				const bool walkable = upward && normal.y >= cosMaxSlope;
				if (pass == PassKind::Across && upward && !walkable)
				{
					const float horizontal = std::sqrt(normal.x * normal.x + normal.z * normal.z);
					if (horizontal > 1.0e-4f)
						normal = b3Vec3 { normal.x / horizontal, 0.0f, normal.z / horizontal };
				}
				else if ((pass == PassKind::Vertical && walkable) || (pass == PassKind::Standing && upward))
				{
					normal = b3Vec3 { 0.0f, 1.0f, 0.0f };
				}
			}
		}

		// Moves a capsule from `position` by `displacement` as far as it can, sliding along what it meets (box3d's plane
		// solver and mover cast). The planes of the last iteration stay in the query.
		glm::vec3 SlideMove(MoverQuery& query, glm::vec3 position, const glm::vec3& displacement, const b3Capsule& capsule, PassKind pass, float cosMaxSlope)
		{
			const glm::vec3 target = position + displacement;
			for (int iteration = 0; iteration < c_MoveIterations; iteration++)
			{
				CollidePlanes(query, position, capsule);
				ShapePlanes(*query.Planes, pass, cosMaxSlope);
				const b3PlaneSolverResult solved = b3SolvePlanes(ToB3(target - position), query.Planes->data(), static_cast<int>(query.Planes->size()));
				glm::vec3 delta = ToGlm(solved.delta);
				const float fraction = CastCapsule(query, position, capsule, delta);
				delta *= fraction;
				position += delta;
				if (glm::dot(delta, delta) < c_MoveTolerance * c_MoveTolerance)
					break;
			}
			return position;
		}

		// The normal of the surface right below a point, `offset` beside it (box3d's mover planes give the direction from the
		// contact to the capsule, which at an edge is not the surface's), if the surface is about as high as the point.
		std::optional<glm::vec3> FindSurfaceNormal(MoverQuery& query, const glm::vec3& point, const glm::vec2& offset)
		{
			constexpr float c_Height = 0.05f;
			const glm::vec3 origin(point.x + offset.x, point.y + c_Height, point.z + offset.y);
			struct RayQuery
			{
				const MoverQuery* Mover = nullptr;
				float Fraction = 2.0f;
				glm::vec3 Normal = glm::vec3(0.0f);
			} ray { &query, 2.0f, glm::vec3(0.0f) };
			b3World_CastRay(query.World, ToB3(origin), b3Vec3 { 0.0f, -2.0f * c_Height, 0.0f }, query.Filter, [](b3ShapeId shape, b3Pos, b3Vec3 normal, float fraction, uint64_t, int, int, void* context)
			{
				RayQuery& found = *static_cast<RayQuery*>(context);
				MoverQuery mover = *found.Mover;
				if (!AcceptMoverShape(shape, &mover))
					return -1.0f;
				if (fraction < found.Fraction)
				{
					found.Fraction = fraction;
					found.Normal = ToGlm(normal);
				}
				return fraction;
			}, &ray);
			if (ray.Fraction > 1.0f)
				return std::nullopt;
			return ray.Normal;
		}

		// Whether a contact on a capsule's round bottom is on an edge of walkable ground (a step's, or a ledge's): the surface
		// just beside the edge, on either side, is walkable at the contact's height.
		std::optional<glm::vec3> FindWalkableEdge(MoverQuery& query, const glm::vec3& point, const glm::vec3& position, float cosMaxSlope)
		{
			constexpr float c_Inset = 0.02f;
			glm::vec2 inward(position.x - point.x, position.z - point.z);
			const float length = glm::length(inward);
			if (!(length > 1.0e-5f))
				return std::nullopt;
			inward = inward / length * c_Inset;
			for (const glm::vec2& offset : { -inward, inward })
			{
				if (const std::optional<glm::vec3> surface = FindSurfaceNormal(query, point, offset); surface && surface->y >= cosMaxSlope)
					return surface;
			}
			return std::nullopt;
		}

		// What a capsule at `position` stands on: the planes of what it touches within c_ContactMargin, below its lower
		// sphere's center and facing up. Walkable (no steeper than the maximum slope) is OnGround, only steeper is
		// OnSteepGround, touching only beside or above NotSupported, nothing InAir; the ground is the most upward plane.
		GroundContact FindGround(MoverQuery& query, const ControllerRecord& record, const glm::vec3& position)
		{
			GroundContact ground;
			const b3Capsule probe = MakeCapsule(record.Shape, 1.0f, c_ContactMargin);
			CollidePlanes(query, position, probe);
			if (query.Planes->empty())
				return ground;
			ground.State = CharacterGroundState::NotSupported;
			const float lowerCenter = probe.center1.y;
			float best = 1.0e-3f;
			int bestIndex = -1;
			for (size_t index = 0; index < query.Planes->size(); index++)
			{
				const b3Vec3 normal = (*query.Planes)[index].plane.normal;
				const PlaneSource& source = (*query.Sources)[index];
				if (!(source.Point.y < lowerCenter) || !(normal.y > best))
					continue;
				best = normal.y;
				bestIndex = static_cast<int>(index);
			}
			if (bestIndex < 0)
				return ground;
			glm::vec3 normal = ToGlm((*query.Planes)[static_cast<size_t>(bestIndex)].plane.normal);
			const PlaneSource source = (*query.Sources)[static_cast<size_t>(bestIndex)];
			ground.Point = position + source.Point;
			if (normal.y < record.CosMaxSlope)
			{
				// The capsule's round bottom on an edge (a step's) meets it at a slant: the surface just inside the edge says
				// whether it stands on a step or on a slope.
				if (const std::optional<glm::vec3> surface = FindWalkableEdge(query, ground.Point, position, record.CosMaxSlope))
					normal = *surface;
			}
			ground.State = normal.y >= record.CosMaxSlope ? CharacterGroundState::OnGround : CharacterGroundState::OnSteepGround;
			ground.Normal = normal;
			ground.Body = b3Shape_GetBody(source.Shape);
			ground.Entity = GetShapeEntity(source.Shape);
			if (b3Body_GetType(ground.Body) != b3_staticBody)
				ground.Velocity = ToGlm(b3Body_GetWorldPointVelocity(ground.Body, ToB3(ground.Point)));
			return ground;
		}

		// Remembers the dynamic bodies a pass ran into, once per body and step, for the pushes after the moves.
		void GatherPushes(MoverQuery& query, ControllerRecord& record, const glm::vec3& position)
		{
			for (size_t index = 0; index < query.Planes->size(); index++)
			{
				const PlaneSource& source = (*query.Sources)[index];
				const b3BodyId body = b3Shape_GetBody(source.Shape);
				if (b3Body_GetType(body) != b3_dynamicBody)
					continue;
				const auto existing = std::find_if(record.Pushes.begin(), record.Pushes.end(), [&](const PushContact& push) { return B3_ID_EQUALS(push.Body, body); });
				if (existing != record.Pushes.end())
					continue; // One push per body and step, from its first contact
				const glm::vec3 point = position + source.Point;
				record.Pushes.push_back({ body, point, ToGlm((*query.Planes)[index].plane.normal), ToGlm(b3Body_GetWorldPointVelocity(body, ToB3(point))) });
			}
		}

		// Collects what the controller touches at the end of its move (each body once: the first of its planes in box3d's
		// deterministic order).
		void GatherTouching(MoverQuery& query, ControllerRecord& record, const glm::vec3& position)
		{
			record.Touching.clear();
			const b3Capsule probe = MakeCapsule(record.Shape, 1.0f, c_ContactMargin);
			CollidePlanes(query, position, probe);
			for (size_t index = 0; index < query.Planes->size(); index++)
			{
				const PlaneSource& source = (*query.Sources)[index];
				const UUID other = GetShapeEntity(source.Shape);
				if (other == UUID::Null() || std::any_of(record.Touching.begin(), record.Touching.end(), [other](const CharacterContactReport& report) { return report.Other == other; }))
					continue;
				CharacterContactReport report;
				report.Controller = record.EntityID;
				report.Other = other;
				report.Point = position + source.Point;
				report.Normal = -ToGlm((*query.Planes)[index].plane.normal);
				record.Touching.push_back(report);
			}
		}

		// One fixed step of one controller: its velocity from the gameplay requests, gravity and the ground, then its moves.
		// Reads the world as it was before the step and writes only the record (see CharacterControllerSet::Step).
		void MoveController(const CharacterControllerSetData& data, ControllerRecord& record, float timestep, const glm::vec3& gravity, bool collectContacts)
		{
			record.Pushes.clear();
			std::vector<b3CollisionPlane> planes;
			std::vector<PlaneSource> sources;
			planes.reserve(c_MaxPlanes);
			sources.reserve(c_MaxPlanes);
			MoverQuery query { data.World, PhysicsFilters::MakeLayerQueryFilter(record.Shape.Layer, data.Settings->LayerCollisionMasks), record.EntityID, &planes, &sources };

			// The velocity it was given last step (after gravity and collisions), not the measured one: walking up a step moves
			// it up by the step's height within one step, which must not launch it.
			const GroundContact& previousGround = record.Ground;
			const bool onGround = previousGround.State == CharacterGroundState::OnGround;
			// Standing, it moves with what it stands on; in the air or on steep ground its motion goes on (sliding down a steep
			// slope speeds up like falling).
			glm::vec3 carried = onGround ? previousGround.Velocity : record.CarriedVelocity;
			const bool jumping = record.JumpRequested;
			if (jumping)
				carried.y = (onGround ? previousGround.Velocity.y : 0.0f) + record.JumpSpeed;
			const glm::vec3 scaledGravity = gravity * record.GravityScale;
			carried += scaledGravity * timestep;
			// Moving away from the ground faster than c_LeaveGroundSpeed leaves it (a jump or a launch).
			const bool leaving = carried.y - (onGround ? previousGround.Velocity.y : 0.0f) > c_LeaveGroundSpeed;

			// Walking is horizontal; the vertical part of the walk velocity counts only without gravity (flying, swimming).
			glm::vec3 walk = record.WalkVelocity;
			if (record.GravityScale != 0.0f)
				walk.y = 0.0f;
			const glm::vec3 velocity = carried + walk + record.Motion / timestep;
			record.StepVelocity = velocity;

			const glm::vec3 start = record.Position;
			const b3Capsule capsule = MakeCapsule(record.Shape, 1.0f);
			const glm::vec3 across(velocity.x * timestep, 0.0f, velocity.z * timestep);

			// Across: slide along walls and up walkable slopes.
			glm::vec3 position = SlideMove(query, start, across, capsule, PassKind::Across, record.CosMaxSlope);
			GatherPushes(query, record, position);
			// What blocked the carried motion across stops it (a wall stops a slide or a flight).
			glm::vec3 horizontal(carried.x, 0.0f, carried.z);
			horizontal = ToGlm(b3ClipVector(ToB3(horizontal), planes.data(), static_cast<int>(planes.size())));
			horizontal.y = 0.0f;

			// Steps: blocked while standing, it tries the same move from up to a step higher and keeps it if it lands on walkable
			// ground further along.
			const float wanted = glm::length(glm::vec2(across.x, across.z));
			const float made = glm::length(glm::vec2(position.x - start.x, position.z - start.z));
			if (onGround && !jumping && record.StepHeight > 0.0f && wanted > c_MoveTolerance && made < wanted * 0.99f)
			{
				const glm::vec3 up(0.0f, record.StepHeight, 0.0f);
				const glm::vec3 lifted = start + up * CastCapsule(query, start, capsule, up);
				glm::vec3 stepped = SlideMove(query, lifted, across, capsule, PassKind::Across, record.CosMaxSlope);
				const glm::vec3 down(0.0f, -(lifted.y - start.y) - c_ContactMargin, 0.0f);
				stepped += down * CastCapsule(query, stepped, capsule, down);
				const float steppedMade = glm::length(glm::vec2(stepped.x - start.x, stepped.z - start.z));
				// It must stand on walkable ground no higher above its feet than the step height (on a higher step's edge it
				// would only hang on).
				const GroundContact landed = FindGround(query, record, stepped);
				if (steppedMade > made + c_MoveTolerance && landed.State == CharacterGroundState::OnGround && landed.Point.y - start.y <= record.StepHeight + c_ContactMargin)
					position = stepped;
			}

			// Vertical: fall, rise or stand.
			const glm::vec3 vertical(0.0f, velocity.y * timestep, 0.0f);
			position = SlideMove(query, position, vertical, capsule, onGround && !leaving ? PassKind::Standing : PassKind::Vertical, record.CosMaxSlope);
			// What the collisions left of the carried motion: a floor stops a fall, a ceiling a rise, steep ground turns a fall
			// into a slide down along it.
			glm::vec3 next = ToGlm(b3ClipVector(ToB3(glm::vec3(horizontal.x, carried.y, horizontal.z)), planes.data(), static_cast<int>(planes.size())));

			// Stick to the ground: walking down a slope or a step within the step height, it stays on the ground.
			if (onGround && !leaving && record.StepHeight > 0.0f)
			{
				const GroundContact ground = FindGround(query, record, position);
				if (ground.State == CharacterGroundState::InAir || ground.State == CharacterGroundState::NotSupported)
				{
					const glm::vec3 down(0.0f, -record.StepHeight, 0.0f);
					const float fraction = CastCapsule(query, position, capsule, down);
					if (fraction < 1.0f)
					{
						const glm::vec3 snapped = position + down * fraction;
						if (FindGround(query, record, snapped).State == CharacterGroundState::OnGround)
							position = snapped;
					}
				}
			}

			record.Ground = FindGround(query, record, position);
			if (leaving && record.Ground.State == CharacterGroundState::OnGround)
				record.Ground.State = CharacterGroundState::InAir; // Rising off the ground (a jump)
			if (record.Ground.State == CharacterGroundState::InAir)
			{
				record.Ground.Normal = glm::vec3(0.0f);
				record.Ground.Velocity = glm::vec3(0.0f);
				record.Ground.Body = b3_nullBodyId;
				record.Ground.Entity = UUID::Null();
			}
			// Standing, it carries what it stands on (so that it keeps the motion of a platform it walks off).
			record.CarriedVelocity = record.Ground.State == CharacterGroundState::OnGround ? record.Ground.Velocity : next;
			if (collectContacts)
				GatherTouching(query, record, position);
			else
				record.Touching.clear();

			record.NewPosition = IsFinite(position) ? position : start;
			record.Velocity = (record.NewPosition - start) / timestep;
			record.Motion = glm::vec3(0.0f);
			record.JumpRequested = false;
		}

		// Puts a controller (and its presence body) at a position, as if it had always stood there: no vertical velocity
		// carried over.
		void PlaceController(CharacterControllerSetData& data, ControllerRecord& record, const glm::vec3& position)
		{
			record.Position = position;
			record.Velocity = glm::vec3(0.0f);
			record.CarriedVelocity = glm::vec3(0.0f);
			MovePresenceBody(record);
			std::vector<b3CollisionPlane> planes;
			std::vector<PlaneSource> sources;
			MoverQuery query { data.World, PhysicsFilters::MakeLayerQueryFilter(record.Shape.Layer, data.Settings->LayerCollisionMasks), record.EntityID, &planes, &sources };
			record.Ground = FindGround(query, record, position);
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
		void PushBodies(ControllerRecord& record, float timestep, const glm::vec3& gravity)
		{
			const float maxImpulse = record.PushStrength * timestep;
			for (const PushContact& push : record.Pushes)
			{
				if (!b3Body_IsValid(push.Body) || b3Body_GetType(push.Body) != b3_dynamicBody || !b3Body_IsEnabled(push.Body))
					continue;
				// The normal points from the body to the character: the character closes in while the body's point moves
				// towards it faster than the character moves away.
				const float closingSpeed = glm::dot(push.Velocity - record.StepVelocity, push.Normal);
				if (!(closingSpeed > 0.0f))
					continue;
				const glm::vec3 arm = push.Position - ToGlm(b3Body_GetWorldCenter(push.Body));
				const glm::vec3 turn = glm::cross(arm, push.Normal);
				const b3Vec3 inverseInertiaTurn = b3MulMV(b3Body_GetWorldInverseRotationalInertia(push.Body), ToB3(turn));
				const float inverseMassAtContact = b3Body_GetInverseMass(push.Body) + glm::dot(turn, ToGlm(inverseInertiaTurn));
				if (!(inverseMassAtContact > 0.0f))
					continue;
				const float magnitude = std::min(closingSpeed / inverseMassAtContact, maxImpulse);
				glm::vec3 impulse = -magnitude * push.Normal;
				impulse.y = std::max(impulse.y, 0.0f);
				if (impulse != glm::vec3(0.0f))
					b3Body_ApplyLinearImpulse(push.Body, ToB3(impulse), ToB3(push.Position), true);
			}

			const GroundContact& ground = record.Ground;
			const bool standing = ground.State == CharacterGroundState::OnGround;
			const bool sliding = ground.State == CharacterGroundState::OnSteepGround;
			const glm::vec3 weight = c_CharacterMass * record.GravityScale * gravity;
			if (B3_IS_NULL(ground.Body) || (!standing && !sliding) || !(glm::dot(weight, weight) > 0.0f))
				return;
			if (!b3Body_IsValid(ground.Body) || b3Body_GetType(ground.Body) != b3_dynamicBody || !b3Body_IsEnabled(ground.Body))
				return;
			glm::vec3 force = weight;
			if (sliding)
			{
				// The ground's normal points out of it: the weight presses into it as far as it points against the normal.
				const float into = -glm::dot(weight, ground.Normal);
				if (!(into > 0.0f))
					return;
				force = -into * ground.Normal;
			}
			b3Body_ApplyLinearImpulse(ground.Body, ToB3(force * timestep), ToB3(ground.Point), true);
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

		void RemoveController(std::map<entt::entity, ControllerRecord>& controllers, std::map<entt::entity, ControllerRecord>::iterator it, std::vector<CharacterBodyChange>& outChanges)
		{
			ControllerRecord& record = it->second;
			CharacterBodyChange change;
			change.Entity = record.EntityID;
			change.OldBody = record.PresenceBody;
			change.OldShape = record.PresenceShape;
			change.Removed = true;
			DestroyPresenceBody(record);
			outChanges.push_back(change);
			controllers.erase(it);
		}

		// Creates, rebuilds or removes the controller of one entity after a change.
		void RefreshController(CharacterControllerSetData& data, entt::entity handle, std::vector<CharacterBodyChange>& outChanges)
		{
			const Entity entity(handle, data.OwnerScene);
			auto it = data.Controllers.find(handle);
			if (!IsControllerWanted(data, entity))
			{
				if (it != data.Controllers.end())
					RemoveController(data.Controllers, it, outChanges);
				return;
			}

			const CharacterControllerComponent& component = entity.GetComponent<CharacterControllerComponent>();
			if (it != data.Controllers.end())
			{
				ControllerRecord& record = it->second;
				RefreshSettings(record, component);
				if (GetShapeOf(component) == record.Shape)
					return;
				// A new shape or layer: a new presence body where the old one stood, walking on as before.
				CharacterBodyChange change;
				change.Entity = record.EntityID;
				change.OldBody = record.PresenceBody;
				change.OldShape = record.PresenceShape;
				DestroyPresenceBody(record);
				record.Shape = GetShapeOf(component);
				CreatePresenceBody(data, record, record.Position);
				change.NewBody = record.PresenceBody;
				change.NewShape = record.PresenceShape;
				outChanges.push_back(change);
				return;
			}

			const glm::vec3 position(data.OwnerScene->GetWorldTransform(entity)[3]);
			if (!IsFinite(position))
			{
				ST_CORE_WARN("Physics: the Character Controller of '{}' has a non-finite position; it is not simulated", entity.GetName());
				return;
			}
			ControllerRecord& record = data.Controllers[handle];
			record.EntityID = entity.GetUUID();
			record.Handle = handle;
			record.Shape = GetShapeOf(component);
			RefreshSettings(record, component);
			CreatePresenceBody(data, record, position);
			// Where it stands right away: the ground state is valid before its first step.
			PlaceController(data, record, position);
			CharacterBodyChange change;
			change.Entity = record.EntityID;
			change.NewBody = record.PresenceBody;
			change.NewShape = record.PresenceShape;
			outChanges.push_back(change);
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

	CharacterControllerSet::CharacterControllerSet(Scene& scene, b3WorldId world, const PhysicsSettings& settings)
		: m_Data(CreateScope<CharacterControllerSetData>())
	{
		CharacterControllerSetData& data = *m_Data;
		data.OwnerScene = &scene;
		data.World = world;
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
			DestroyPresenceBody(record);
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
	}

	void CharacterControllerSet::RefreshFilters(std::vector<UUID>& outRefiltered)
	{
		CharacterControllerSetData& data = *m_Data;
		for (auto& [handle, record] : data.Controllers)
		{
			if (B3_IS_NULL(record.PresenceShape))
				continue;
			const b3Filter filter = PhysicsFilters::MakeShapeFilter(record.Shape.Layer, data.Settings->LayerCollisionMasks);
			const b3Filter current = b3Shape_GetFilter(record.PresenceShape);
			if (current.categoryBits == filter.categoryBits && current.maskBits == filter.maskBits && current.groupIndex == filter.groupIndex)
				continue;
			b3Shape_SetFilter(record.PresenceShape, filter, true);
			outRefiltered.push_back(record.EntityID);
		}
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

		// The moves: in parallel batches (box3d's queries only read the world).
		const uint32_t count = static_cast<uint32_t>(records.size());
		const uint32_t threads = JobSystem::IsInitialized() ? JobSystem::GetWorkerThreadCount() + 1 : 1;
		const uint32_t batchSize = std::max(c_MinBatchSize, (count + threads * c_BatchesPerThread - 1) / (threads * c_BatchesPerThread));
		const uint32_t batches = (count + batchSize - 1) / batchSize;
		const auto move = [&](uint32_t begin, uint32_t end)
		{
			for (uint32_t index = begin; index < end; index++)
				MoveController(data, *records[index], timestep, gravity, collectContacts);
		};
		if (batches > 1 && JobSystem::IsInitialized())
			JobSystem::ParallelFor(count, batchSize, move);
		else
			move(0, count);

		// After the moves, on the main thread in controller order: write the entities, move the presence bodies, push.
		for (ControllerRecord* record : records)
		{
			if (record->NewPosition != record->Position && IsFinite(record->NewPosition))
			{
				if (WriteBack(data, *record))
				{
					output.Moved.push_back(record->Handle);
					record->Position = record->NewPosition;
				}
				MovePresenceBody(*record);
			}
			PushBodies(*record, timestep, gravity);
			if (collectContacts)
				output.Contacts.insert(output.Contacts.end(), record->Touching.begin(), record->Touching.end());
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

		CharacterState state;
		state.Ground = record->Ground.State;
		state.Velocity = record->Velocity;
		if (state.Ground != CharacterGroundState::InAir)
		{
			state.GroundNormal = record->Ground.Normal;
			state.GroundVelocity = record->Ground.Velocity;
			if (record->Ground.Entity != UUID::Null())
				state.GroundEntity = m_Data->OwnerScene->GetEntityByUUID(record->Ground.Entity);
		}
		return state;
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
