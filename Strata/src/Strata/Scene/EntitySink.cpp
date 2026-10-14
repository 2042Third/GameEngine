#include "stpch.h"
#include "Strata/Scene/EntitySink.h"

#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Scene/ComponentAccess.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/EntityPrototype.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/UnknownComponents.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <utility>

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_NotLinked = std::numeric_limits<uint32_t>::max();
		// Entities between two progress reports of a sink's Finish: a few milliseconds of work.
		constexpr size_t c_FinishProgressInterval = 4096;

		void ReportFinish(const EntityReadProgress& progress, float start, float share, size_t index, size_t count)
		{
			if (progress && index > 0 && index % c_FinishProgressInterval == 0)
				progress(start + share * static_cast<float>(static_cast<double>(index) / static_cast<double>(count)));
		}

		// The ID of an entity object ("ID"; a new one when it has none), or nullopt when it is invalid.
		std::optional<UUID> ReadEntityID(const nlohmann::json& entity)
		{
			const nlohmann::json* id = JsonUtils::Find(entity, "ID");
			const std::optional<UUID> parsed = id ? UUIDFromJson(*id) : std::optional<UUID>(UUID());
			if (!parsed || !parsed->IsValid())
				return std::nullopt;
			return parsed;
		}

		// Reads the "Components" of an entity object as SceneSerializer::DeserializeEntities does: each registered component
		// into the object `add` returns for it (ComponentAccess::Deserialize, not strict: what it cannot read is reported),
		// components this build does not register into `outUnknown`, counted in `tally`.
		template<typename AddComponent>
		void ReadComponents(const nlohmann::json& entity, UUID id, AddComponent&& add, nlohmann::json& outUnknown, Detail::UnknownComponentTally& tally,
			std::vector<std::string>* outWarnings)
		{
			const nlohmann::json* components = JsonUtils::Find(entity, "Components");
			if (!components)
				return;
			if (!components->is_object())
			{
				if (outWarnings)
					outWarnings->push_back(fmt::format("Entity {}: 'Components' must be an object", id.ToString()));
				return;
			}
			for (const auto& [componentName, componentJson] : components->items())
			{
				const ComponentInfo* info = ComponentRegistry::Find(componentName);
				if (!info)
				{
					outUnknown[componentName] = componentJson;
					tally.Add(componentName);
					continue;
				}
				if (HasFlag(info->Flags, ComponentFlags::NoSerialize))
				{
					if (outWarnings)
						outWarnings->push_back(fmt::format("Entity {}: component '{}' is not read from entity data; skipped", id.ToString(), componentName));
					continue;
				}
				void* component = add(*info);
				std::string error;
				if (!ComponentAccess::Deserialize(*info, component, componentJson, false, &error, outWarnings) && outWarnings)
					outWarnings->push_back(fmt::format("Entity {}: {}", id.ToString(), error));
			}
		}

		// The parent each entity is linked to (its index in document order, or c_NotLinked: the top level), as
		// SceneSerializer::DeserializeEntities links them: in document order, a parent that is not an ID (`invalidParents`,
		// ascending), is not among the entities (`indexOf` gives c_NotLinked) or would close a cycle leaves the entity at the
		// top level, with a warning. Accepted links form a forest in which each child is still a root when its link is
		// considered (IDs are unique), so a link closes a cycle exactly when both ends are already connected: union-find
		// answers that in near-constant time, keeping deep hierarchies linear to load.
		template<typename IdOf, typename IndexOf>
		std::vector<uint32_t> LinkParents(std::span<const UUID> parents, std::span<const uint32_t> invalidParents, IdOf&& idOf, IndexOf&& indexOf,
			std::vector<std::string>* outWarnings, const EntityReadProgress& progress, float progressStart, float progressShare)
		{
			const size_t count = parents.size();
			std::vector<uint32_t> linked(count, c_NotLinked);
			std::vector<uint32_t> connected(count);
			std::iota(connected.begin(), connected.end(), 0u);
			auto findRepresentative = [&connected](uint32_t index)
			{
				uint32_t representative = index;
				while (connected[representative] != representative)
					representative = connected[representative];
				while (connected[index] != representative) // Path compression
					index = std::exchange(connected[index], representative);
				return representative;
			};

			size_t nextInvalid = 0;
			for (uint32_t index = 0; index < count; index++)
			{
				ReportFinish(progress, progressStart, progressShare, index, count);
				if (nextInvalid < invalidParents.size() && invalidParents[nextInvalid] == index)
				{
					nextInvalid++;
					if (outWarnings)
						outWarnings->push_back(fmt::format("Entity {}: invalid 'Parent'; attached at the top level", idOf(index).ToString()));
					continue;
				}
				const UUID parent = parents[index];
				if (!parent.IsValid())
					continue;
				const uint32_t parentIndex = indexOf(parent);
				if (parentIndex == c_NotLinked)
				{
					if (outWarnings)
						outWarnings->push_back(fmt::format("Entity {}: parent {} not found; attached at the top level", idOf(index).ToString(), parent.ToString()));
					continue;
				}
				const uint32_t childRepresentative = findRepresentative(index);
				const uint32_t parentRepresentative = findRepresentative(parentIndex);
				if (childRepresentative == parentRepresentative)
				{
					if (outWarnings)
						outWarnings->push_back(fmt::format("Entity {}: parent {} would create a cycle; attached at the top level", idOf(index).ToString(), parent.ToString()));
					continue;
				}
				connected[childRepresentative] = parentRepresentative;
				linked[index] = parentIndex;
			}
			return linked;
		}

		bool Fail(std::string* outError, std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		}

	}

	namespace Detail
	{

		void UnknownComponentTally::Add(const std::string& name)
		{
			auto [count, firstSeen] = Counts.try_emplace(name, 0);
			if (firstSeen)
				Names.push_back(name);
			count->second++;
		}

		void UnknownComponentTally::AppendWarnings(std::vector<std::string>& outWarnings) const
		{
			for (const std::string& name : Names)
			{
				const size_t count = Counts.at(name);
				outWarnings.push_back(fmt::format("Unknown component '{}' ({} {}) kept unchanged: no module of this build registers it", name, count,
					count == 1 ? "entity" : "entities"));
			}
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// SceneBuilder
	////////////////////////////////////////////////////////////////////////////////

	namespace
	{

		// The shares of SceneBuilder::Finish its steps take: the parents, the links, the notifications.
		constexpr float c_FinishParentsShare = 0.2f;
		constexpr float c_FinishLinksShare = 0.3f;

	}

	SceneBuilder::SceneBuilder(Scene& scene, std::vector<std::string>* outWarnings, const EntityReadProgress& linkProgress)
		: m_Scene(scene), m_Warnings(outWarnings), m_LinkProgress(linkProgress), m_WasEmpty(scene.GetEntityCount() == 0)
	{
	}

	void SceneBuilder::ReserveEntities(uint64_t count)
	{
		const size_t entities = static_cast<size_t>(std::min<uint64_t>(count, Scene::c_MaxEntities));
		m_Handles.reserve(entities);
		m_Parents.reserve(entities);
		m_Scene.m_EntityMap.reserve(entities);
	}

	void SceneBuilder::ReadField(std::string_view key, const nlohmann::json& value)
	{
		if (key == "Name")
		{
			m_Scene.SetName(value.is_string() ? value.get<std::string>() : std::string("Untitled"));
			m_HasName = true;
		}
		else if (key == "Settings" && value.is_object())
		{
			SceneSerializer::DeserializeSettings(value, m_Scene.GetSettings(), m_Warnings ? &m_SettingsWarnings : nullptr);
		}
	}

	bool SceneBuilder::ReadEntity(const nlohmann::json& entity, std::string* outError)
	{
		if (!m_WasEmpty)
			return Fail(outError, "Scenes can only be deserialized into an empty scene");
		if (!entity.is_object())
			return Fail(outError, "Entity entries must be objects");
		const std::optional<UUID> id = ReadEntityID(entity);
		if (!id)
			return Fail(outError, "Entity has an invalid 'ID'");
		if (m_Scene.m_EntityMap.contains(*id))
			return Fail(outError, fmt::format("Duplicate entity ID {} in entity data", id->ToString()));

		// Created directly (not through CreateEntity), as DeserializeEntities does, so building large hierarchies stays linear:
		// they are linked once all exist.
		entt::registry& registry = m_Scene.m_Registry;
		const entt::entity handle = registry.create();
		registry.emplace<IDComponent>(handle, *id);
		registry.emplace<NameComponent>(handle, "Entity");
		registry.emplace<TransformComponent>(handle);
		registry.emplace<RelationshipComponent>(handle);
		registry.emplace<WorldTransformComponent>(handle);
		m_Scene.m_EntityMap.emplace(*id, handle);

		UUID parent = UUID::Null();
		if (const nlohmann::json* parentJson = JsonUtils::Find(entity, "Parent"))
		{
			if (const std::optional<UUID> parsed = UUIDFromJson(*parentJson))
				parent = *parsed;
			else
				m_InvalidParents.push_back(static_cast<uint32_t>(m_Handles.size()));
		}
		m_Handles.push_back(handle);
		m_Parents.push_back(parent);

		nlohmann::json unknown = nlohmann::json::object();
		ReadComponents(entity, *id, [&](const ComponentInfo& info) { return info.Add(registry, handle); }, unknown, m_Unknown,
			m_Warnings ? &m_EntityWarnings : nullptr);
		if (!unknown.empty())
			registry.emplace<UnknownComponentsComponent>(handle, UnknownComponentsComponent { std::move(unknown) });
		return true;
	}

	bool SceneBuilder::Finish(std::string* outError)
	{
		if (!m_WasEmpty)
			return Fail(outError, "Scenes can only be deserialized into an empty scene");
		if (!m_HasName)
			m_Scene.SetName("Untitled");

		// Where each entity lies in the document by its EnTT index.
		entt::registry& registry = m_Scene.m_Registry;
		const size_t count = m_Handles.size();
		size_t indexSpan = 0;
		for (const entt::entity handle : m_Handles)
			indexSpan = std::max(indexSpan, static_cast<size_t>(entt::to_entity(handle)) + 1);
		std::vector<uint32_t> indexOfSlot(indexSpan, c_NotLinked);
		for (size_t index = 0; index < count; index++)
			indexOfSlot[entt::to_entity(m_Handles[index])] = static_cast<uint32_t>(index);

		std::vector<std::string> parentWarnings;
		const std::vector<uint32_t> linked = LinkParents(m_Parents, m_InvalidParents,
			[&](uint32_t index) { return registry.get<IDComponent>(m_Handles[index]).ID; },
			[&](UUID id)
			{
				const auto it = m_Scene.m_EntityMap.find(id);
				return it != m_Scene.m_EntityMap.end() ? indexOfSlot[entt::to_entity(it->second)] : c_NotLinked;
			},
			m_Warnings ? &parentWarnings : nullptr, m_LinkProgress, 0.0f, c_FinishParentsShare);
		m_Parents = {};
		m_InvalidParents = {};

		// The runtime links (HierarchyComponent) are built now that every entity exists: children may be listed before their
		// parents. Linking also fills the RelationshipComponents and the root list.
		for (const entt::entity handle : m_Handles)
			registry.emplace<HierarchyComponent>(handle);
		std::vector<entt::entity> rootHandles;
		for (size_t index = 0; index < count; index++)
		{
			ReportFinish(m_LinkProgress, c_FinishParentsShare, c_FinishLinksShare, index, count);
			const entt::entity parent = linked[index] != c_NotLinked ? m_Handles[linked[index]] : entt::null;
			if (parent == entt::null)
				rootHandles.push_back(m_Handles[index]);
			m_Scene.LinkLast(m_Handles[index], parent);
		}
		// Depths, activity and the (stale) world transforms of the new subtrees.
		m_Scene.FinishLinking(rootHandles);
		m_Scene.m_HierarchyVersion++;

		// Components were constructed with default values and filled in afterwards; now that data and hierarchy are complete,
		// systems hear of them (on_update) and see the final state.
		const std::vector<const ComponentInfo*>& infos = ComponentRegistry::GetAll();
		for (size_t index = 0; index < count; index++)
		{
			ReportFinish(m_LinkProgress, c_FinishParentsShare + c_FinishLinksShare, 1.0f - c_FinishParentsShare - c_FinishLinksShare, index, count);
			for (const ComponentInfo* info : infos)
			{
				if (info->Has(registry, m_Handles[index]))
					info->MarkModified(registry, m_Handles[index]);
			}
		}
		m_Handles = {};

		if (m_Warnings)
		{
			for (std::string& warning : m_SettingsWarnings)
				m_Warnings->push_back(std::move(warning));
			for (std::string& warning : m_EntityWarnings)
				m_Warnings->push_back(std::move(warning));
			m_Unknown.AppendWarnings(*m_Warnings);
			for (std::string& warning : parentWarnings)
				m_Warnings->push_back(std::move(warning));
		}
		if (m_LinkProgress)
			m_LinkProgress(1.0f);
		return true;
	}

}
