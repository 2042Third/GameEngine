#pragma once

#include "Strata/Renderer/SceneRenderData.h"

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	// The point and spot lights of a scene as records the GPU culls into the light clusters (RenderScene owns one and syncs
	// it; RenderSceneGPU mirrors the records). A record per light component (an entity with a PointLightComponent and a
	// SpotLightComponent has two), kept from component signals and the scene's world transform changes like the mesh
	// proxies, with a change log the mirror follows. Records of inactive or unlit lights stay, marked not drawn
	// (RenderData::c_LightRecordDrawn), so that lights switched on and off cost no allocation. Main thread.
	// How a light record that casts shadows (RenderData::c_LightRecordShadowed) is shadowed: its component's settings, finite
	// and within their ranges.
	struct LightShadowSettings
	{
		float Bias = 1.0f;
		float NormalBias = 1.5f;
		float FilterRadius = 1.5f;

		bool operator==(const LightShadowSettings&) const = default;
	};

	class RenderLights
	{
	public:
		static constexpr uint32_t c_InvalidIndex = ~0u;
		// Changes kept for readers of the log (GetChanges); more since a reader's version and it reads everything.
		static constexpr size_t c_MaxChanges = 65536;

		// A light's record from its world transform and component values (also what the CPU path draws a light with): the
		// region it reaches (its bounding sphere) and the values shaders light with. Not drawn when it is inactive, unlit, has
		// no range or a value that is not finite.
		static RenderData::LightRecord MakeRecord(const glm::mat4& world, bool active, const glm::vec3& color, float intensity, float range, uint32_t type,
			float innerDegrees, float outerDegrees);
		// The smallest sphere around the region a spot light reaches (apex `position`, unit `direction`): the part of its range
		// sphere inside the cone.
		static glm::vec4 GetSpotBounds(const glm::vec3& position, const glm::vec3& direction, float range, float cosOuter, float sinOuter);

		// Listens to the registry's light components (the connections last as long as the registry: see RenderScene).
		void Connect(entt::registry& registry);
		// Queues every light component the registry has (a new render scene's first sync reads them).
		void QueueAll(const entt::registry& registry);

		// Reads the queued lights, then the lights of `transformChanges` (entities whose world transform or activity changed),
		// or every light when `allTransforms`. Returns the number of records updated.
		uint32_t Sync(const entt::registry& registry, std::span<const entt::entity> transformChanges, bool allTransforms);

		// Records below GetCapacity (free ones are not drawn); the lights the scene has.
		uint32_t GetCapacity() const { return static_cast<uint32_t>(m_Records.size()); }
		uint32_t GetCount() const { return m_Count; }
		std::span<const RenderData::LightRecord> GetRecords() const { return m_Records; }
		// The record of an entity's point (RenderData::c_LightPoint) or spot light, or c_InvalidIndex.
		uint32_t Find(entt::entity entity, uint32_t type) const;
		entt::entity GetEntity(uint32_t record) const { return m_Entities[record]; }
		uint32_t GetType(uint32_t record) const { return m_Types[record]; }
		// The records of drawn lights that cast shadows (SpotAngles.w is RenderData::c_LightRecordShadowed), in no particular
		// order, and a record's shadow settings (those of a record that casts none are the defaults).
		std::span<const uint32_t> GetShadowedRecords() const { return m_Shadowed; }
		const LightShadowSettings& GetShadowSettings(uint32_t record) const { return m_ShadowSettings[record]; }

		// Changes with every change of a record. Readers take it to read the changes after it later (GetChanges).
		uint64_t GetVersion() const { return m_Version; }
		// Appends the records that changed after version `sinceVersion` (a record may appear more than once). Returns false,
		// appending nothing, when more than c_MaxChanges changes happened since: then any record may have changed.
		bool GetChanges(uint64_t sinceVersion, std::vector<uint32_t>& outRecords) const;

		// Compares every record with what the registry's components and transforms give. Returns false with the first
		// difference.
		bool Validate(const entt::registry& registry, std::string* outError) const;
	private:
		template<typename Component>
		void OnLightChanged(entt::registry& registry, entt::entity entity);
		template<typename Component>
		void OnLightDestroyed(entt::registry& registry, entt::entity entity);

		void Queue(entt::entity entity, uint32_t type);
		// Brings an entity's record of `type` up to date with the registry: created, updated or freed.
		bool UpdateRecord(const entt::registry& registry, entt::entity entity, uint32_t type);
		bool ComputeRecord(const entt::registry& registry, entt::entity entity, uint32_t type, RenderData::LightRecord& outRecord,
			LightShadowSettings& outShadow) const;
		void Free(entt::entity entity, uint32_t type);
		// Keeps m_Shadowed in step with a record's SpotAngles.w.
		void UpdateShadowed(uint32_t record);
		void LogChange(uint32_t record);
		static size_t GetKey(entt::entity entity, uint32_t type);
	private:
		std::vector<RenderData::LightRecord> m_Records;
		std::vector<entt::entity> m_Entities;  // Per record (null: free)
		std::vector<uint32_t> m_Types;         // Per record
		std::vector<LightShadowSettings> m_ShadowSettings; // Per record
		std::vector<uint32_t> m_Shadowed;      // The records that cast shadows
		std::vector<uint32_t> m_ShadowedIndex; // Per record: its place in m_Shadowed, or c_InvalidIndex
		std::vector<uint32_t> m_FreeRecords;
		std::vector<uint32_t> m_ByKey;         // Per entity index and type (GetKey): its record
		uint32_t m_Count = 0;

		// Entities whose light components were signaled since the last sync, each once per type.
		struct QueuedLight
		{
			entt::entity Entity;
			uint32_t Type = 0;
		};
		std::vector<QueuedLight> m_Queued;
		std::vector<uint8_t> m_QueuedKeys;     // Per GetKey: queued

		struct Change
		{
			uint64_t Version = 0;
			uint32_t Record = 0;
		};
		std::vector<Change> m_Changes;         // From m_ChangesStart on: the latest changes, at most c_MaxChanges
		size_t m_ChangesStart = 0;
		uint64_t m_ChangesFloor = 0;           // Readers from before this version read everything
		uint64_t m_Version = 0;
	};

}
