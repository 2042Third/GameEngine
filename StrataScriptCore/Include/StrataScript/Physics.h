#pragma once

#include "StrataScript/Entity.h"
#include "StrataScript/Host.h"
#include "StrataScript/Value.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace Strata
{

	// A hit of a physics ray (see Physics::Raycast).
	struct RaycastHit
	{
		Entity HitEntity;                   // The entity owning the body (its RigidBody entity, also for colliders on descendants)
		glm::vec3 Point = glm::vec3(0.0f);  // World space
		glm::vec3 Normal = glm::vec3(0.0f); // Surface normal at Point
		float Distance = 0.0f;              // From the ray's origin along its direction
	};

	// A contact passed to the collision and trigger callbacks of Script.
	struct Collision
	{
		// The entity on the other side. Entities destroyed during the frame stay valid until it ends; a contact that ended
		// because the other entity was destroyed names an entity that no longer exists.
		Entity Other;
		glm::vec3 Point = glm::vec3(0.0f);  // World space (the last known one when the contact ends)
		glm::vec3 Normal = glm::vec3(0.0f); // From this script's entity towards the other
	};

	// The physics body of an entity while the scene plays: an active entity with a RigidBody component and colliders.
	// Vectors are in world space, angular values in radians. Velocities can be read from every body, but only dynamic
	// bodies accept velocities, forces and impulses; calls on other bodies or on entities without a body fail (the engine
	// logs why). Forces and torques act during the next fixed step only (apply them in OnFixedUpdate for as long as they
	// should act); impulses change the velocity at once. Body settings (type, mass, friction, layer, ...) are properties
	// of the "RigidBody" component (Entity::GetProperty/SetProperty).
	class RigidBody
	{
	public:
		explicit RigidBody(Entity entity)
			: m_Entity(entity)
		{
		}

		Entity GetEntity() const { return m_Entity; }

		// Zero when the entity has no body.
		glm::vec3 GetLinearVelocity() const
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(GetLinearVelocity);
			return host ? Read(host->GetLinearVelocity) : glm::vec3(0.0f);
		}

		bool SetLinearVelocity(const glm::vec3& velocity)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(SetLinearVelocity);
			return host && Apply(host->SetLinearVelocity, velocity);
		}

		// Radians per second around each world axis; zero when the entity has no body.
		glm::vec3 GetAngularVelocity() const
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(GetAngularVelocity);
			return host ? Read(host->GetAngularVelocity) : glm::vec3(0.0f);
		}

		bool SetAngularVelocity(const glm::vec3& velocity)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(SetAngularVelocity);
			return host && Apply(host->SetAngularVelocity, velocity);
		}

		bool AddForce(const glm::vec3& force)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AddForce);
			return host && Apply(host->AddForce, force);
		}

		// A force acting at a world space point (it also turns the body).
		bool AddForceAtPosition(const glm::vec3& force, const glm::vec3& worldPosition)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AddForceAtPosition);
			return host && ApplyAt(host->AddForceAtPosition, force, worldPosition);
		}

		bool AddImpulse(const glm::vec3& impulse)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AddImpulse);
			return host && Apply(host->AddImpulse, impulse);
		}

		bool AddImpulseAtPosition(const glm::vec3& impulse, const glm::vec3& worldPosition)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AddImpulseAtPosition);
			return host && ApplyAt(host->AddImpulseAtPosition, impulse, worldPosition);
		}

		bool AddTorque(const glm::vec3& torque)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AddTorque);
			return host && Apply(host->AddTorque, torque);
		}

		bool AddAngularImpulse(const glm::vec3& impulse)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AddAngularImpulse);
			return host && Apply(host->AddAngularImpulse, impulse);
		}

		// Moves the body (of any type) and its entity to a world position and rotation at once, keeping its velocities;
		// queries see it there right away. Writing the entity's transform moves the body too, but only at the next fixed
		// step. Without a rotation the body keeps its world rotation.
		bool Teleport(const glm::vec3& position, const glm::quat& rotation)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(Teleport);
			if (!host || m_Entity.GetID() == 0)
				return false;
			float abiPosition[3];
			float abiRotation[4];
			Detail::ToABIVector3(position, abiPosition);
			Detail::ToABIQuat(rotation, abiRotation);
			return host->Teleport(Detail::GetContext(), m_Entity.GetID(), abiPosition, abiRotation);
		}

		bool Teleport(const glm::vec3& position) { return Teleport(position, m_Entity.GetTransform().GetWorldRotation()); }
	private:
		using GetFunction = bool (*)(StrataScriptContext*, StrataScriptEntityID, float*);
		using ApplyFunction = bool (*)(StrataScriptContext*, StrataScriptEntityID, const float*);
		using ApplyAtFunction = bool (*)(StrataScriptContext*, StrataScriptEntityID, const float*, const float*);

		glm::vec3 Read(GetFunction function) const
		{
			float value[3] = { 0.0f, 0.0f, 0.0f };
			if (m_Entity.GetID() == 0 || !function(Detail::GetContext(), m_Entity.GetID(), value))
				return glm::vec3(0.0f);
			return Detail::FromABIVector3(value);
		}

		bool Apply(ApplyFunction function, const glm::vec3& vector)
		{
			float value[3];
			Detail::ToABIVector3(vector, value);
			return m_Entity.GetID() != 0 && function(Detail::GetContext(), m_Entity.GetID(), value);
		}

		bool ApplyAt(ApplyAtFunction function, const glm::vec3& vector, const glm::vec3& position)
		{
			float value[3];
			float point[3];
			Detail::ToABIVector3(vector, value);
			Detail::ToABIVector3(position, point);
			return m_Entity.GetID() != 0 && function(Detail::GetContext(), m_Entity.GetID(), value, point);
		}
	private:
		Entity m_Entity;
	};

	// Physics queries against the bodies of the scene as of the last fixed step (or RigidBody::Teleport). A layer mask
	// selects RigidBody layers (bit n: layer n). Triggers are skipped unless includeTriggers is set. Results name the
	// entity owning each body (its RigidBody entity, also for colliders on descendants).
	class Physics
	{
	public:
		static constexpr uint32_t c_AllLayers = 0xFFFFFFFFu;

		// The closest hit of a ray. The direction need not be normalized; `ignore` is skipped (e.g. the caster's own body),
		// and a ray starting inside a convex collider does not hit it.
		static std::optional<RaycastHit> Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance = std::numeric_limits<float>::infinity(),
			uint32_t layerMask = c_AllLayers, Entity ignore = Entity(), bool includeTriggers = false)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(Raycast);
			if (!host)
				return std::nullopt;
			float abiOrigin[3];
			float abiDirection[3];
			Detail::ToABIVector3(origin, abiOrigin);
			Detail::ToABIVector3(direction, abiDirection);
			StrataScriptRaycastHit hit = {};
			if (!host->Raycast(Detail::GetContext(), abiOrigin, abiDirection, maxDistance, layerMask, ignore.GetID(), includeTriggers, &hit))
				return std::nullopt;
			return ToHit(hit);
		}

		// The closest hit on every body along the ray, sorted by distance.
		static std::vector<RaycastHit> RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance = std::numeric_limits<float>::infinity(),
			uint32_t layerMask = c_AllLayers, Entity ignore = Entity(), bool includeTriggers = false)
		{
			std::vector<RaycastHit> hits;
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(RaycastAll);
			if (!host)
				return hits;
			float abiOrigin[3];
			float abiDirection[3];
			Detail::ToABIVector3(origin, abiOrigin);
			Detail::ToABIVector3(direction, abiDirection);
			const auto read = [&](StrataScriptRaycastHit* buffer, uint32_t capacity)
			{
				return host->RaycastAll(Detail::GetContext(), abiOrigin, abiDirection, maxDistance, layerMask, ignore.GetID(), includeTriggers, buffer, capacity);
			};

			StrataScriptRaycastHit stackBuffer[16];
			constexpr uint32_t c_StackCapacity = static_cast<uint32_t>(sizeof(stackBuffer) / sizeof(stackBuffer[0]));
			uint32_t count = read(stackBuffer, c_StackCapacity);
			const StrataScriptRaycastHit* results = stackBuffer;
			std::vector<StrataScriptRaycastHit> largeBuffer;
			if (count > c_StackCapacity)
			{
				largeBuffer.resize(count);
				const uint32_t secondCount = read(largeBuffer.data(), count);
				count = secondCount < count ? secondCount : count;
				results = largeBuffer.data();
			}
			hits.reserve(count);
			for (uint32_t index = 0; index < count; index++)
				hits.push_back(ToHit(results[index]));
			return hits;
		}

		// The entities whose bodies overlap a sphere or an oriented box, in a deterministic order.
		static std::vector<Entity> OverlapSphere(const glm::vec3& center, float radius, uint32_t layerMask = c_AllLayers, bool includeTriggers = false)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(OverlapSphere);
			if (!host)
				return {};
			float abiCenter[3];
			Detail::ToABIVector3(center, abiCenter);
			return ToEntities(Detail::ReadHostIDs([&](uint64_t* buffer, uint32_t capacity)
			{
				return host->OverlapSphere(Detail::GetContext(), abiCenter, radius, layerMask, includeTriggers, buffer, capacity);
			}));
		}

		static std::vector<Entity> OverlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
			uint32_t layerMask = c_AllLayers, bool includeTriggers = false)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(OverlapBox);
			if (!host)
				return {};
			float abiCenter[3];
			float abiHalfExtents[3];
			float abiRotation[4];
			Detail::ToABIVector3(center, abiCenter);
			Detail::ToABIVector3(halfExtents, abiHalfExtents);
			Detail::ToABIQuat(rotation, abiRotation);
			return ToEntities(Detail::ReadHostIDs([&](uint64_t* buffer, uint32_t capacity)
			{
				return host->OverlapBox(Detail::GetContext(), abiCenter, abiHalfExtents, abiRotation, layerMask, includeTriggers, buffer, capacity);
			}));
		}
	private:
		static RaycastHit ToHit(const StrataScriptRaycastHit& hit)
		{
			RaycastHit result;
			result.HitEntity = Entity(hit.Entity);
			result.Point = Detail::FromABIVector3(hit.Point);
			result.Normal = Detail::FromABIVector3(hit.Normal);
			result.Distance = hit.Distance;
			return result;
		}

		static std::vector<Entity> ToEntities(const std::vector<uint64_t>& ids)
		{
			std::vector<Entity> entities;
			entities.reserve(ids.size());
			for (uint64_t id : ids)
				entities.emplace_back(id);
			return entities;
		}
	};

	inline RigidBody Entity::GetRigidBody() const
	{
		return RigidBody(*this);
	}

}
