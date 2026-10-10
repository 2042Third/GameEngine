#pragma once

#include "Editor/UndoStack.h"

#include <Strata/Core/UUID.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_set>
#include <vector>

namespace Strata
{

	// Everything that defines one entity in the edited scene: its components, its parent and its position among its
	// siblings. Restoring a state recreates the entity with the same UUID, so references to it stay valid.
	struct EntityState
	{
		UUID ID = UUID::Null();
		bool Exists = false;
		nlohmann::json Components; // ComponentAccess::SerializeEntityComponents plus the UnknownComponentsComponent's entries
		UUID Parent = UUID::Null();
		size_t SiblingIndex = 0;

		bool operator==(const EntityState& other) const = default;
	};

	namespace SceneEdit
	{

		EntityState CaptureEntity(const Scene& scene, UUID id);
		std::vector<EntityState> CaptureEntities(const Scene& scene, const std::vector<UUID>& ids);
		// The entity followed by all its descendants, in hierarchy order.
		std::vector<UUID> CollectSubtree(const Scene& scene, UUID id);

		// Makes the scene match the states: removes entities that must not exist, recreates missing ones (with their
		// UUIDs), replaces the components that differ and restores parents and sibling order. States must be
		// consistent: an entity that is removed has all its descendants in the set.
		void ApplyEntities(Scene& scene, const std::vector<EntityState>& states);

	}

	// Undo step for any change to a set of entities, stored as their states before and after the change.
	class SceneEditAction final : public EditorAction
	{
	public:
		// mergeKey: consecutive actions with the same non-empty key and the same entities become one undo step.
		SceneEditAction(Scene& scene, std::string name, std::vector<EntityState> before, std::vector<EntityState> after, std::string mergeKey = {});

		const std::string& GetName() const override { return m_Name; }
		bool Execute(std::string* outError) override;
		void Undo() override;
		bool MergeWith(const EditorAction& next) override;
		bool IsNoOp() const override { return m_Before == m_After; }

		const std::vector<EntityState>& GetBefore() const { return m_Before; }
		const std::vector<EntityState>& GetAfter() const { return m_After; }
	private:
		Scene* m_Scene;
		std::string m_Name;
		std::vector<EntityState> m_Before;
		std::vector<EntityState> m_After;
		std::string m_MergeKey;
	};

	// Records an edit that is made directly on the scene: construct it with the entities the edit will touch, make
	// the changes, Track any entities created along the way, then Commit to record one undo step. An edit that
	// changed nothing records nothing.
	class SceneEditTransaction
	{
	public:
		SceneEditTransaction(Scene& scene, std::string name, const std::vector<UUID>& entities, std::string mergeKey = {});

		// Adds an entity to the edit; call before changing it.
		void Track(UUID id);
		// Adds an entity and all its descendants; call before changing or destroying them.
		void TrackSubtree(UUID id);
		// Adds an entity (and its descendants) the edit created; call after creating it.
		void TrackCreated(UUID id);

		// Records the edit on the stack. Returns false when nothing changed.
		bool Commit(UndoStack& undoStack);
		// Restores the tracked entities to their state before the edit (a failed edit) and ends the transaction.
		void Rollback();
	private:
		Scene& m_Scene;
		std::string m_Name;
		std::string m_MergeKey;
		std::vector<UUID> m_Entities;
		std::unordered_set<UUID> m_Tracked;
		std::vector<EntityState> m_Before;
	};

}
