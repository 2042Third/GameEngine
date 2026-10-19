#include "stpch.h"
#include "Strata/Renderer/RenderLights.h"

#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Scene/Components.h"

#include <entt/entt.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <type_traits>

namespace Strata
{

	namespace
	{

		bool IsFiniteMatrix(const glm::mat4& matrix)
		{
			for (int column = 0; column < 4; column++)
			{
				for (int row = 0; row < 4; row++)
				{
					if (!std::isfinite(matrix[column][row]))
						return false;
				}
			}
			return true;
		}

		bool RecordsEqual(const RenderData::LightRecord& a, const RenderData::LightRecord& b)
		{
			return std::memcmp(&a, &b, sizeof(RenderData::LightRecord)) == 0;
		}

		constexpr uint32_t c_LightTypes = 2;

	}

	glm::vec4 RenderLights::GetSpotBounds(const glm::vec3& position, const glm::vec3& direction, float range, float cosOuter, float sinOuter)
	{
		if (cosOuter >= 0.70710678f)
		{
			// Up to 45 degrees the apex and the rim of the spherical cap lie on the bounding sphere.
			const float radius = range / (2.0f * cosOuter);
			return glm::vec4(position + direction * radius, radius);
		}
		// Wider cones are bounded by the sphere through the rim of the cap.
		return glm::vec4(position + direction * (range * cosOuter), range * sinOuter);
	}

	RenderData::LightRecord RenderLights::MakeRecord(const glm::mat4& world, bool active, const glm::vec3& color, float intensity, float range, uint32_t type,
		float innerDegrees, float outerDegrees)
	{
		RenderData::LightRecord record;
		if (!active || !(intensity > 0.0f) || !(range > 0.0f) || !std::isfinite(intensity) || !std::isfinite(range) || !IsFiniteMatrix(world)
			|| !std::isfinite(color.r) || !std::isfinite(color.g) || !std::isfinite(color.b))
			return record; // Not drawn
		const glm::vec3 position = glm::vec3(world[3]);
		glm::vec3 direction = glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
		const float length = glm::length(direction);
		direction = length > 1e-6f ? direction / length : glm::vec3(0.0f, 0.0f, -1.0f);
		float inner = 0.0f;
		float outer = 0.0f;
		if (type == RenderData::c_LightSpot)
		{
			outer = std::clamp(outerDegrees, 0.1f, 89.9f);
			inner = std::clamp(innerDegrees, 0.0f, outer);
		}
		const float cosInner = std::cos(glm::radians(inner));
		const float cosOuter = std::cos(glm::radians(outer));
		record.PositionRange = glm::vec4(position, range);
		record.Color = glm::vec4(color * intensity, 1.0f / (range * range));
		record.DirectionType = glm::vec4(direction, static_cast<float>(type));
		record.SpotAngles = glm::vec4(cosInner, cosOuter, RenderData::c_LightRecordDrawn, 0.0f);
		record.Bounds = type == RenderData::c_LightSpot ? GetSpotBounds(position, direction, range, cosOuter, std::sin(glm::radians(outer)))
			: glm::vec4(position, range);
		return record;
	}

	size_t RenderLights::GetKey(entt::entity entity, uint32_t type)
	{
		return static_cast<size_t>(entt::to_entity(entity)) * c_LightTypes + type;
	}

	void RenderLights::Connect(entt::registry& registry)
	{
		registry.on_construct<PointLightComponent>().connect<&RenderLights::OnLightChanged<PointLightComponent>>(*this);
		registry.on_update<PointLightComponent>().connect<&RenderLights::OnLightChanged<PointLightComponent>>(*this);
		registry.on_destroy<PointLightComponent>().connect<&RenderLights::OnLightDestroyed<PointLightComponent>>(*this);
		registry.on_construct<SpotLightComponent>().connect<&RenderLights::OnLightChanged<SpotLightComponent>>(*this);
		registry.on_update<SpotLightComponent>().connect<&RenderLights::OnLightChanged<SpotLightComponent>>(*this);
		registry.on_destroy<SpotLightComponent>().connect<&RenderLights::OnLightDestroyed<SpotLightComponent>>(*this);
	}

	void RenderLights::QueueAll(const entt::registry& registry)
	{
		for (const entt::entity entity : registry.view<PointLightComponent>())
			Queue(entity, RenderData::c_LightPoint);
		for (const entt::entity entity : registry.view<SpotLightComponent>())
			Queue(entity, RenderData::c_LightSpot);
	}

	template<typename Component>
	void RenderLights::OnLightChanged(entt::registry&, entt::entity entity)
	{
		Queue(entity, std::is_same_v<Component, SpotLightComponent> ? RenderData::c_LightSpot : RenderData::c_LightPoint);
	}

	template<typename Component>
	void RenderLights::OnLightDestroyed(entt::registry&, entt::entity entity)
	{
		// At once: the entity (and its index) may be reused before the next sync.
		constexpr uint32_t c_Type = std::is_same_v<Component, SpotLightComponent> ? RenderData::c_LightSpot : RenderData::c_LightPoint;
		Free(entity, c_Type);
		const size_t key = GetKey(entity, c_Type);
		if (key < m_QueuedKeys.size() && m_QueuedKeys[key])
		{
			m_QueuedKeys[key] = 0;
			std::erase_if(m_Queued, [entity](const QueuedLight& queued) { return queued.Entity == entity && queued.Type == c_Type; });
		}
	}

	void RenderLights::Queue(entt::entity entity, uint32_t type)
	{
		const size_t key = GetKey(entity, type);
		if (key >= m_QueuedKeys.size())
			m_QueuedKeys.resize(key + 1, 0);
		if (m_QueuedKeys[key])
			return;
		m_QueuedKeys[key] = 1;
		m_Queued.push_back(QueuedLight { entity, type });
	}

	uint32_t RenderLights::Find(entt::entity entity, uint32_t type) const
	{
		if (entity == entt::null)
			return c_InvalidIndex;
		const size_t key = GetKey(entity, type);
		if (key >= m_ByKey.size())
			return c_InvalidIndex;
		const uint32_t record = m_ByKey[key];
		return record != c_InvalidIndex && m_Entities[record] == entity ? record : c_InvalidIndex;
	}

	bool RenderLights::ComputeRecord(const entt::registry& registry, entt::entity entity, uint32_t type, RenderData::LightRecord& outRecord) const
	{
		if (!registry.valid(entity))
			return false;
		const WorldTransformComponent* world = registry.try_get<WorldTransformComponent>(entity);
		if (!world)
			return false;
		if (type == RenderData::c_LightSpot)
		{
			const SpotLightComponent* light = registry.try_get<SpotLightComponent>(entity);
			if (!light)
				return false;
			outRecord = MakeRecord(world->Matrix, world->ActiveInHierarchy, light->Color, light->Intensity, light->Range, type, light->InnerConeAngle,
				light->OuterConeAngle);
			return true;
		}
		const PointLightComponent* light = registry.try_get<PointLightComponent>(entity);
		if (!light)
			return false;
		outRecord = MakeRecord(world->Matrix, world->ActiveInHierarchy, light->Color, light->Intensity, light->Range, type, 0.0f, 0.0f);
		return true;
	}

	bool RenderLights::UpdateRecord(const entt::registry& registry, entt::entity entity, uint32_t type)
	{
		RenderData::LightRecord record;
		if (!ComputeRecord(registry, entity, type, record))
		{
			const bool existed = Find(entity, type) != c_InvalidIndex;
			Free(entity, type);
			return existed;
		}
		uint32_t index = Find(entity, type);
		if (index == c_InvalidIndex)
		{
			if (!m_FreeRecords.empty())
			{
				index = m_FreeRecords.back();
				m_FreeRecords.pop_back();
			}
			else
			{
				index = static_cast<uint32_t>(m_Records.size());
				m_Records.emplace_back();
				m_Entities.push_back(entt::null);
				m_Types.push_back(0);
			}
			m_Entities[index] = entity;
			m_Types[index] = type;
			const size_t key = GetKey(entity, type);
			if (key >= m_ByKey.size())
				m_ByKey.resize(key + 1, c_InvalidIndex);
			m_ByKey[key] = index;
			m_Count++;
		}
		else if (RecordsEqual(m_Records[index], record))
		{
			return false;
		}
		m_Records[index] = record;
		LogChange(index);
		return true;
	}

	void RenderLights::Free(entt::entity entity, uint32_t type)
	{
		const uint32_t index = Find(entity, type);
		if (index == c_InvalidIndex)
			return;
		m_ByKey[GetKey(entity, type)] = c_InvalidIndex;
		m_Entities[index] = entt::null;
		m_Records[index] = RenderData::LightRecord {};
		m_FreeRecords.push_back(index);
		m_Count--;
		LogChange(index);
	}

	uint32_t RenderLights::Sync(const entt::registry& registry, std::span<const entt::entity> transformChanges, bool allTransforms)
	{
		uint32_t updated = 0;
		for (const QueuedLight& queued : m_Queued)
		{
			m_QueuedKeys[GetKey(queued.Entity, queued.Type)] = 0;
			updated += UpdateRecord(registry, queued.Entity, queued.Type) ? 1u : 0u;
		}
		m_Queued.clear();
		if (m_Count == 0)
			return updated;
		if (allTransforms)
		{
			for (uint32_t index = 0; index < m_Entities.size(); index++)
			{
				if (m_Entities[index] != entt::null)
					updated += UpdateRecord(registry, m_Entities[index], m_Types[index]) ? 1u : 0u;
			}
			return updated;
		}
		for (const entt::entity entity : transformChanges)
		{
			for (uint32_t type = 0; type < c_LightTypes; type++)
			{
				if (Find(entity, type) != c_InvalidIndex)
					updated += UpdateRecord(registry, entity, type) ? 1u : 0u;
			}
		}
		return updated;
	}

	void RenderLights::LogChange(uint32_t record)
	{
		m_Version++;
		const size_t live = m_Changes.size() - m_ChangesStart;
		if (live + 1 > c_MaxChanges)
		{
			// Readers further behind than the oldest change kept read everything.
			m_ChangesStart += live + 1 - c_MaxChanges;
			m_ChangesFloor = m_Changes[m_ChangesStart - 1].Version;
		}
		m_Changes.push_back(Change { m_Version, record });
		if (m_ChangesStart >= c_MaxChanges)
		{
			m_Changes.erase(m_Changes.begin(), m_Changes.begin() + static_cast<ptrdiff_t>(m_ChangesStart));
			m_ChangesStart = 0;
		}
	}

	bool RenderLights::GetChanges(uint64_t sinceVersion, std::vector<uint32_t>& outRecords) const
	{
		if (sinceVersion >= m_Version)
			return true;
		if (sinceVersion < m_ChangesFloor)
			return false;
		for (size_t index = m_Changes.size(); index > m_ChangesStart && m_Changes[index - 1].Version > sinceVersion; index--)
			outRecords.push_back(m_Changes[index - 1].Record);
		return true;
	}

	bool RenderLights::Validate(const entt::registry& registry, std::string* outError) const
	{
		auto fail = [outError](const std::string& message)
		{
			if (outError)
				*outError = message;
			return false;
		};
		uint32_t count = 0;
		for (uint32_t index = 0; index < m_Entities.size(); index++)
		{
			const entt::entity entity = m_Entities[index];
			if (entity == entt::null)
			{
				if (m_Records[index].SpotAngles.z != 0.0f)
					return fail("light record " + std::to_string(index) + " is free but drawn");
				continue;
			}
			count++;
			RenderData::LightRecord expected;
			if (!ComputeRecord(registry, entity, m_Types[index], expected))
				return fail("light record " + std::to_string(index) + " names an entity without its light component");
			if (!RecordsEqual(expected, m_Records[index]))
				return fail("light record " + std::to_string(index) + " differs from its entity's light and transform");
			if (Find(entity, m_Types[index]) != index)
				return fail("light record " + std::to_string(index) + " is not found by its entity");
		}
		if (count != m_Count)
			return fail("the light count differs from the records");
		for (const entt::entity entity : registry.view<PointLightComponent, WorldTransformComponent>())
		{
			if (Find(entity, RenderData::c_LightPoint) == c_InvalidIndex)
				return fail("a point light has no record");
		}
		for (const entt::entity entity : registry.view<SpotLightComponent, WorldTransformComponent>())
		{
			if (Find(entity, RenderData::c_LightSpot) == c_InvalidIndex)
				return fail("a spot light has no record");
		}
		return true;
	}

}
