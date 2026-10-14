#pragma once

#include "Editor/UndoStack.h"

#include <Strata/Core/UUID.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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
		// The scene asset of the sub-scene the entity belongs to (a world's cell; null: none, the persistent scene): its own
		// for a top-level entity, its top-level ancestor's for a descendant. A state of an entity that does not exist names
		// the cell the entity is in on the other side of its step (SceneEditAction fills it in), so that undoing a creation
		// or redoing a deletion waits for that cell too: done while it is not loaded, the entity would come back with it.
		AssetHandle Cell = UUID::Null();

		bool operator==(const EntityState& other) const = default;
	};

	// Offered by an open world (EditorWorld) as a scene service of the edited scene: undo steps that need a cell which is not
	// loaded ask it to load (and keep) the cell, and name the cell in their error.
	class ISceneCellLoader
	{
	public:
		virtual ~ISceneCellLoader() = default;

		// How the cell of a scene asset reads in errors ("Props (3, -1)").
		virtual std::string DescribeCell(AssetHandle cell) const = 0;
		// Loads and pins the cell, so that the step can be done once it is in.
		virtual void RequestCell(AssetHandle cell) = 0;
	};

	namespace SceneEdit
	{

		EntityState CaptureEntity(const Scene& scene, UUID id);
		std::vector<EntityState> CaptureEntities(const Scene& scene, const std::vector<UUID>& ids);
		// The entity followed by all its descendants, in hierarchy order.
		std::vector<UUID> CollectSubtree(const Scene& scene, UUID id);
		// How undo steps name what they changed: one entity by its name in quotes ("'Wall'", cut to c_MaxNameInStep
		// characters), several by their number ("3 entities").
		constexpr size_t c_MaxNameInStep = 40;
		std::string QuoteName(std::string_view name);
		std::string DescribeEntities(std::span<const Entity> entities);
		// The name without a copy number: "Crate (2)" -> "Crate" (names that only look like one stay whole: "(1)", "A (x)").
		std::string GetBaseName(std::string_view name);
		// Makes the entity's name unique among its siblings (the other children of its parent, or the other top-level
		// entities): when one of them has its name, it becomes "<base> (n)" with the smallest n from 1 that none has
		// ("Crate" -> "Crate (1)", a copy of "Crate (1)" -> "Crate (2)"). Renames through the name's update signal; returns
		// whether it renamed. Commands naming many entities pass the same `numbers` for all of them: the next number to try
		// per parent and name, so that a thousand copies of one name cost a thousand checks, not half a million.
		using NameNumbers = std::unordered_map<std::string, uint64_t>;
		bool MakeNameUnique(Scene& scene, Entity entity, NameNumbers* numbers = nullptr);

		// Makes the scene match the states: removes entities that must not exist, recreates missing ones (with their
		// UUIDs), replaces the components that differ and restores parents and sibling order. States must be
		// consistent: an entity that is removed has all its descendants in the set.
		void ApplyEntities(Scene& scene, const std::vector<EntityState>& states);
		// Whether every cell the states name is loaded, so that ApplyEntities can put the entities back into them and finds
		// the entities they hold. False with the reason otherwise (changing nothing), after asking the scene's
		// ISceneCellLoader to load the missing cells.
		bool CheckCells(Scene& scene, const std::vector<EntityState>& states, std::string* outError = nullptr);
		// Gives the states of entities that do not exist on one side of a step the Cell of the other side. The two lists name
		// the same entities in the same order (as SceneEditTransaction captures them).
		void LinkCells(std::vector<EntityState>& before, std::vector<EntityState>& after);

	}

	// Undo step for any change to a set of entities, stored as their states before and after the change. The states are
	// shared and never change once recorded (a merge replaces the "after" states), so journals keep them without copies.
	class SceneEditAction final : public EditorAction
	{
	public:
		// The states list the same entities in the same order (as SceneEditTransaction makes them); those of entities that do
		// not exist on one side take the other side's Cell (SceneEdit::LinkCells). mergeKey: consecutive actions with the same
		// non-empty key and the same entities become one undo step.
		SceneEditAction(Scene& scene, std::string name, std::vector<EntityState> before, std::vector<EntityState> after, std::string mergeKey = {});

		const std::string& GetName() const override { return m_Name; }
		bool Execute(std::string* outError) override;
		bool Undo(std::string* outError) override;
		bool MergeWith(const EditorAction& next) override;
		bool IsNoOp() const override { return *m_Before == *m_After; }
		SceneChangeKind GetSceneChange() const override { return SceneChangeKind::Entities; }
		Ref<const std::vector<EntityState>> GetEntityStates(bool undone) const override { return undone ? m_Before : m_After; }

		const std::vector<EntityState>& GetBefore() const { return *m_Before; }
		const std::vector<EntityState>& GetAfter() const { return *m_After; }
	private:
		Scene* m_Scene;
		std::string m_Name;
		Ref<const std::vector<EntityState>> m_Before;
		Ref<const std::vector<EntityState>> m_After;
		std::string m_MergeKey;
	};

	// Undo step of a change to the scene's settings (gravity, fixed step, physics capacities: scene.setSettings).
	class SceneSettingsAction final : public EditorAction
	{
	public:
		SceneSettingsAction(Scene& scene, const SceneSettings& before, const SceneSettings& after);

		const std::string& GetName() const override;
		bool Execute(std::string* outError) override;
		bool Undo(std::string* outError) override;
		SceneChangeKind GetSceneChange() const override { return SceneChangeKind::Settings; }
	private:
		Scene& m_Scene;
		SceneSettings m_Before;
		SceneSettings m_After;
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

		// Names the step after all, once what it made is known (e.g. the names of pasted entities).
		void SetName(std::string name) { m_Name = std::move(name); }
		// The entities the edit tracks, in the order tracked, and whether the edit created the one at an index.
		const std::vector<UUID>& GetEntities() const { return m_Entities; }
		bool WasCreated(size_t index) const { return !m_Before[index].Exists; }
		// Records the edit on the stack. Returns false when nothing changed.
		bool Commit(UndoStack& undoStack);
		// The edit as an action, for a step that holds more (EditorActionGroup); null when nothing changed. Ends the
		// transaction like Commit.
		Scope<EditorAction> TakeAction();
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
