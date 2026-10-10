#include "Editor/SceneEdit.h"

#include <Strata/Core/Log.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/ComponentAccess.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/SceneSerializer.h>

#include <unordered_map>

namespace Strata
{

	namespace
	{

		// Replaces the components that differ from the snapshot. Components are reset to defaults before the
		// snapshot is applied, so properties and extra data match it exactly.
		void ApplyComponents(Scene& scene, Entity entity, const nlohmann::json& components)
		{
			entt::registry& registry = scene.GetRegistry();
			const entt::entity handle = entity.GetHandle();
			for (const ComponentInfo* info : ComponentRegistry::GetAll())
			{
				if (HasFlag(info->Flags, ComponentFlags::NoSerialize))
					continue;

				const bool present = info->Has(registry, handle);
				const auto target = components.is_object() ? components.find(info->Name) : components.end();
				if (target == components.end())
				{
					if (present && info->IsRemovable())
						info->Remove(registry, handle);
					continue;
				}

				if (present && ComponentAccess::Serialize(*info, info->Get(registry, handle)) == *target)
					continue;
				if (present && info->IsRemovable())
					info->Remove(registry, handle);
				void* component = info->Add(registry, handle);
				std::string error;
				std::vector<std::string> warnings;
				if (!ComponentAccess::Deserialize(*info, component, *target, false, &error, &warnings))
					ST_WARN("Restoring component {} of entity {}: {}", info->Name, entity.GetUUID().ToString(), error);
				info->MarkModified(registry, handle);
			}
		}

	}

	namespace SceneEdit
	{

		EntityState CaptureEntity(const Scene& scene, UUID id)
		{
			EntityState state;
			state.ID = id;
			Entity entity = scene.GetEntityByUUID(id);
			if (!entity)
				return state;
			state.Exists = true;
			state.Components = ComponentAccess::SerializeEntityComponents(entity);
			state.Parent = entity.GetComponent<RelationshipComponent>().Parent;
			// Cached by the scene: wide sibling lists are not searched per capture.
			state.SiblingIndex = scene.GetSiblingIndex(entity);
			return state;
		}

		std::vector<EntityState> CaptureEntities(const Scene& scene, const std::vector<UUID>& ids)
		{
			std::vector<EntityState> states;
			states.reserve(ids.size());
			for (UUID id : ids)
				states.push_back(CaptureEntity(scene, id));
			return states;
		}

		std::vector<UUID> CollectSubtree(const Scene& scene, UUID id)
		{
			std::vector<UUID> subtree;
			std::vector<UUID> stack = { id };
			while (!stack.empty())
			{
				const UUID current = stack.back();
				stack.pop_back();
				Entity entity = scene.GetEntityByUUID(current);
				if (!entity)
					continue;
				subtree.push_back(current);
				const std::vector<UUID>& children = entity.GetComponent<RelationshipComponent>().Children;
				stack.insert(stack.end(), children.rbegin(), children.rend());
			}
			return subtree;
		}

		void ApplyEntities(Scene& scene, const std::vector<EntityState>& states)
		{
			// Removals first: descendants that must exist are recreated below. One batch, so that removing many entities from
			// a long sibling list stays linear.
			std::vector<Entity> removed;
			for (const EntityState& state : states)
			{
				if (state.Exists)
					continue;
				if (Entity entity = scene.GetEntityByUUID(state.ID))
					removed.push_back(entity);
			}
			scene.DestroyEntities(removed);

			// Missing entities are recreated in one batch, with their UUIDs, components and the links among them (linear
			// even for large subtrees). States are captured parents first, so siblings keep their order.
			std::unordered_map<UUID, const EntityState*> recreated;
			for (const EntityState& state : states)
			{
				if (state.Exists && !scene.GetEntityByUUID(state.ID))
					recreated.emplace(state.ID, &state);
			}
			if (!recreated.empty())
			{
				nlohmann::json entities = nlohmann::json::array();
				for (const EntityState& state : states)
				{
					if (!recreated.contains(state.ID))
						continue;
					nlohmann::json entity = { { "ID", UUIDToJson(state.ID) }, { "Components", state.Components } };
					if (state.Parent.IsValid() && recreated.contains(state.Parent))
						entity["Parent"] = UUIDToJson(state.Parent);
					entities.push_back(std::move(entity));
				}
				EntityInstantiationOptions options;
				options.GenerateNewUUIDs = false;
				std::string error;
				std::vector<std::string> warnings;
				SceneSerializer::DeserializeEntities(scene, { { "Entities", std::move(entities) } }, options, &error, &warnings);
				if (!error.empty())
					ST_ERROR("Restoring entities failed: {}", error);
				for (const std::string& warning : warnings)
					ST_WARN("Restoring entities: {}", warning);
			}

			for (const EntityState& state : states)
			{
				if (!state.Exists || recreated.contains(state.ID))
					continue;
				if (Entity entity = scene.GetEntityByUUID(state.ID))
					ApplyComponents(scene, entity, state.Components);
			}

			// Hierarchy: only entities whose parent or position differs move, in one batch (detached first so that no
			// intermediate parent link can form a cycle, then each lands at its recorded position), so that restoring many
			// positions in a long sibling list stays linear.
			std::vector<Scene::EntityPlacement> placements;
			for (const EntityState& state : states)
			{
				if (!state.Exists)
					continue;
				Entity entity = scene.GetEntityByUUID(state.ID);
				if (!entity)
					continue;
				if (entity.GetComponent<RelationshipComponent>().Parent == state.Parent && scene.GetSiblingIndex(entity) == state.SiblingIndex)
					continue;
				placements.push_back({ state.ID, state.Parent, state.SiblingIndex });
			}
			if (!scene.PlaceEntities(placements))
				ST_WARN("Restoring the hierarchy: some entities could not be placed under their recorded parent and stay at the top level");
		}

	}

	SceneEditAction::SceneEditAction(Scene& scene, std::string name, std::vector<EntityState> before, std::vector<EntityState> after, std::string mergeKey)
		: m_Scene(&scene), m_Name(std::move(name)), m_Before(std::move(before)), m_After(std::move(after)), m_MergeKey(std::move(mergeKey))
	{
	}

	bool SceneEditAction::Execute(std::string*)
	{
		SceneEdit::ApplyEntities(*m_Scene, m_After);
		return true;
	}

	void SceneEditAction::Undo()
	{
		SceneEdit::ApplyEntities(*m_Scene, m_Before);
	}

	bool SceneEditAction::MergeWith(const EditorAction& next)
	{
		const auto* edit = dynamic_cast<const SceneEditAction*>(&next);
		if (!edit || m_MergeKey.empty() || edit->m_MergeKey != m_MergeKey || edit->m_Scene != m_Scene || edit->m_After.size() != m_After.size())
			return false;
		for (size_t index = 0; index < m_After.size(); index++)
		{
			if (edit->m_After[index].ID != m_After[index].ID)
				return false;
		}
		m_After = edit->m_After;
		return true;
	}

	SceneEditTransaction::SceneEditTransaction(Scene& scene, std::string name, const std::vector<UUID>& entities, std::string mergeKey)
		: m_Scene(scene), m_Name(std::move(name)), m_MergeKey(std::move(mergeKey))
	{
		std::vector<UUID> tracked;
		for (UUID id : entities)
		{
			if (id.IsValid() && m_Tracked.insert(id).second)
				tracked.push_back(id);
		}
		for (EntityState& state : SceneEdit::CaptureEntities(m_Scene, tracked))
			m_Before.push_back(std::move(state));
		m_Entities = std::move(tracked);
	}

	void SceneEditTransaction::Track(UUID id)
	{
		if (!id.IsValid() || !m_Tracked.insert(id).second)
			return;
		m_Entities.push_back(id);
		m_Before.push_back(SceneEdit::CaptureEntity(m_Scene, id));
	}

	void SceneEditTransaction::TrackSubtree(UUID id)
	{
		std::vector<UUID> untracked;
		for (UUID entity : SceneEdit::CollectSubtree(m_Scene, id))
		{
			if (m_Tracked.insert(entity).second)
				untracked.push_back(entity);
		}
		for (EntityState& state : SceneEdit::CaptureEntities(m_Scene, untracked))
			m_Before.push_back(std::move(state));
		m_Entities.insert(m_Entities.end(), untracked.begin(), untracked.end());
	}

	void SceneEditTransaction::TrackCreated(UUID id)
	{
		for (UUID entity : SceneEdit::CollectSubtree(m_Scene, id))
		{
			if (!m_Tracked.insert(entity).second)
				continue;
			m_Entities.push_back(entity);
			EntityState state;
			state.ID = entity;
			m_Before.push_back(std::move(state));
		}
	}

	void SceneEditTransaction::Rollback()
	{
		SceneEdit::ApplyEntities(m_Scene, m_Before);
		m_Before.clear();
		m_Entities.clear();
		m_Tracked.clear();
	}

	bool SceneEditTransaction::Commit(UndoStack& undoStack)
	{
		std::vector<EntityState> after = SceneEdit::CaptureEntities(m_Scene, m_Entities);
		if (after == m_Before)
			return false;
		undoStack.Record(CreateScope<SceneEditAction>(m_Scene, m_Name, std::move(m_Before), std::move(after), m_MergeKey));
		m_Before.clear();
		m_Entities.clear();
		m_Tracked.clear();
		return true;
	}

}
