#pragma once

#include <Strata/Core/Base.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	struct EntityState;

	// What an action's effect on the edited scene consists of, for those that follow the scene's changes (the recovery
	// journal, SceneRecovery, and the activity journal).
	enum class SceneChangeKind : uint8_t
	{
		None = 0, // Changes no scene data (e.g. an asset document edit, which saves its own file)
		Entities, // Entity states (EditorAction::GetEntityStates)
		Settings, // The scene's settings only
		Other     // Something else in the scene, which only a full snapshot of it captures
	};

	// An undoable edit. Execute applies it (the first time and on redo), Undo reverts it. Actions own all state
	// they need: they must work after any number of other actions were undone and redone around them. Either fails,
	// changing nothing and saying why in outError, when the world changed in a way the action cannot be applied or reverted
	// in (another file is where a deleted asset would come back).
	class EditorAction
	{
	public:
		virtual ~EditorAction() = default;

		virtual const std::string& GetName() const = 0;
		virtual bool Execute(std::string* outError) = 0;
		virtual bool Undo(std::string* outError) = 0;

		// Continuous edits (dragging a value) become one undo step: when the stack's latest action accepts the next
		// one, it absorbs its effect (the next action has already been applied) and the next action is dropped.
		virtual bool MergeWith(const EditorAction& /*next*/) { return false; }
		// Whether the action changes nothing (e.g. a merged drag that returned to its start). The stack drops a merged
		// step that became a no-op, so dragging a value back and forth leaves no undo step.
		virtual bool IsNoOp() const { return false; }
		// Whether the action changes the edited scene, which saving the scene saves. Edits of asset documents (a material)
		// write their files themselves: they share the history but never make the scene unsaved (UndoStack::IsModified).
		virtual bool ChangesScene() const { return true; }
		// What the action's effect on the scene consists of. Actions that change the scene describe it as entity states
		// where they can (GetEntityStates): an action of kind Other makes the recovery journal snapshot the whole scene.
		virtual SceneChangeKind GetSceneChange() const { return ChangesScene() ? SceneChangeKind::Other : SceneChangeKind::None; }
		// The states of the entities the action touches as the scene holds them after it (undone false) or before it
		// (undone true); null when the action does not describe its effect as entity states. Shared and never changed, so
		// that journals can keep and read them on other threads.
		virtual Ref<const std::vector<EntityState>> GetEntityStates([[maybe_unused]] bool undone) const { return nullptr; }
	};

	// Told about every change of the scene an UndoStack applies, so it can follow the scene's state (the recovery journal).
	class UndoStackObserver
	{
	public:
		virtual ~UndoStackObserver() = default;

		// `action` was recorded, absorbed a merged action (merged: it describes the merged step, and replaces what the
		// notification of it before said, e.g. every frame of a drag) or was redone (undone false), or it was undone (undone
		// true): the scene now holds what it describes for that direction.
		virtual void OnApplied(const EditorAction& action, bool undone, bool merged) = 0;
	};

	// Several actions as one undo step (e.g. prefab.apply: the prefab's file and the instances it updates): Execute runs them in
	// order, Undo in reverse order; either happens as a whole or not at all (when one fails, those that ran are reverted).
	class EditorActionGroup final : public EditorAction
	{
	public:
		explicit EditorActionGroup(std::string name);

		// Null actions are left out.
		void Add(Scope<EditorAction> action);

		const std::string& GetName() const override { return m_Name; }
		bool Execute(std::string* outError) override;
		bool Undo(std::string* outError) override;
		bool ChangesScene() const override;
		// The scene change of its one member that changes the scene (the others edit files: prefab.apply's prefab, the asset
		// prefab.create writes), whose entity states are the group's; several such members changed the scene together in a
		// way only a snapshot captures (Other).
		SceneChangeKind GetSceneChange() const override;
		Ref<const std::vector<EntityState>> GetEntityStates(bool undone) const override;
	private:
		// The member that changes the scene when exactly one does; null otherwise.
		const EditorAction* GetSceneMember() const;
	private:
		std::string m_Name;
		std::vector<Scope<EditorAction>> m_Actions;
	};

	// Linear undo history with redo, a capacity limit and save-point tracking (for "modified" indicators).
	// Main thread only.
	class UndoStack
	{
	public:
		explicit UndoStack(size_t capacity = 512);

		// Executes the action and records it, discarding the redo history. Returns false (nothing recorded, state
		// unchanged) when the action fails.
		bool Execute(Scope<EditorAction> action, std::string* outError = nullptr);
		// Records an action whose effect has already been applied (e.g. a scene edit made in place).
		void Record(Scope<EditorAction> action);

		// Reverts the latest applied action. When it cannot be reverted, it stays applied (the history keeps its position, so
		// the step can be tried again once what blocks it is gone) and the reason goes to outError and the log.
		bool Undo(std::string* outError = nullptr);
		// Applies the next undone action again. When it cannot be applied, it and the actions after it are dropped (the world
		// they were recorded in is gone) and the reason goes to outError and the log.
		bool Redo(std::string* outError = nullptr);
		bool CanUndo() const { return m_Position > 0; }
		bool CanRedo() const { return m_Position < m_Entries.size(); }
		// Names of the actions undo and redo would apply (empty when unavailable).
		std::string GetUndoName() const;
		std::string GetRedoName() const;
		// Names of all recorded actions, oldest first, and how many of them are applied.
		std::vector<std::string> GetHistory() const;
		size_t GetPosition() const { return m_Position; }
		size_t GetCount() const { return m_Entries.size(); }

		// Every recorded action has a serial number, unique for the stack's lifetime (also across Clear), which a merged
		// action keeps: it names an undo step for as long as the step exists (the activity journal's "Undo to here").
		// The serial of the action at an index (oldest first), and the index of the action with a serial (nullopt when it
		// was trimmed, discarded with the redo history or cleared).
		uint64_t GetSerial(size_t index) const { return m_Entries[index].Serial; }
		std::optional<size_t> FindSerial(uint64_t serial) const;
		const EditorAction& GetAction(size_t index) const { return *m_Entries[index].Action; }
		// The latest applied action and its serial (null and 0 when nothing is applied).
		const EditorAction* GetLatestAction() const { return m_Position > 0 ? m_Entries[m_Position - 1].Action.get() : nullptr; }
		uint64_t GetLatestSerial() const { return m_Position > 0 ? m_Entries[m_Position - 1].Serial : 0; }
		// Counts recorded actions, merges included: a command that changed it recorded (or extended) a step.
		uint64_t GetRecordCount() const { return m_RecordCount; }
		// Counts the steps recorded (serials given out), not the merges into them: a drag recorded in many merges is one.
		uint64_t GetStepCount() const { return m_NextSerial - 1; }
		// Changes whenever what the stack holds, applies or calls saved changes: a record or merge, undo, redo, a save point,
		// Clear. Unchanged, the scene's unsaved changes are the same ones (an answer about them still holds), and so is what
		// the edits made, so caches of it (a prefab instance's "modified" mark) can tell when to look again.
		uint64_t GetVersion() const { return m_Version; }
		// The serial of the step the latest Execute or Record created or merged into; 0 when that step changed nothing in
		// the end and was dropped (a merged drag back to its start).
		uint64_t GetLastRecordedSerial() const { return m_LastRecordedSerial; }
		// Undoes or redoes until `position` actions are applied. False when the position does not exist or an undo or a redo
		// failed on the way (the stack then stays where it stopped).
		bool GoTo(size_t position);

		// The next recorded action starts a new undo step even if it could merge (e.g. the mouse was released).
		void BreakMerge() { m_MergeBroken = true; }
		// Save point: IsModified reports whether the applied actions differ from those at the last MarkSaved by an action
		// that changes the scene (EditorAction::ChangesScene). Costs the number of actions between the two positions.
		void MarkSaved();
		bool IsModified() const;

		// A state of the history, as a save that writes the scene in the background took it (GetSavePoint): the state after
		// the action with Serial was applied (0: before every action the history holds), which stays the same state for as
		// long as the history holds that action, wherever later edits, undos and redos move the history. The edits that
		// continue an action (a drag merging frames) change its state, so whoever takes a point breaks merging first
		// (BreakMerge), and the next edit starts a step of its own.
		struct SavePoint
		{
			size_t Position = 0;        // Applied actions when it was taken
			uint64_t Serial = 0;        // Of the latest of them (0: none)
			uint64_t Generation = 0;    // Of the history (Clear starts another)
			uint64_t SceneTrims = 0;    // Scene changes trimmed from the front of the history before it was taken

			bool operator==(const SavePoint& other) const = default;
		};
		SavePoint GetSavePoint() const;
		// Marks a point saved once what was saved there is on disk (a background save finished): IsModified then compares with
		// it. When the history no longer holds it (undone past and edited since, trimmed, dropped by a failed redo), no point of
		// the history is the saved one and the scene stays modified. A point of a history that was cleared since (another
		// scene) marks nothing.
		void MarkSaved(const SavePoint& point);

		void Clear();

		// Told about every change the stack applies (null: nobody). The observer must outlive the stack or be removed.
		void SetObserver(UndoStackObserver* observer) { m_Observer = observer; }
	private:
		struct Entry
		{
			Scope<EditorAction> Action;
			uint64_t Serial = 0;
		};

		void Push(Scope<EditorAction> action);
		void NotifyApplied(const EditorAction& action, bool undone, bool merged = false);
	private:
		std::deque<Entry> m_Entries;
		size_t m_Position = 0;   // Number of applied actions (index of the next redo)
		size_t m_Capacity;
		bool m_MergeBroken = true;
		// Position of the save point; c_NoSavePoint when it was trimmed or discarded with the redo history.
		size_t m_SavedPosition = 0;
		static constexpr size_t c_NoSavePoint = static_cast<size_t>(-1);
		uint64_t m_NextSerial = 1;
		uint64_t m_RecordCount = 0;
		uint64_t m_LastRecordedSerial = 0;
		uint64_t m_Version = 0;
		uint64_t m_Generation = 0;  // Changes with Clear (SavePoint)
		uint64_t m_SceneTrims = 0;  // Steps that change the scene trimmed from the front (SavePoint)
		UndoStackObserver* m_Observer = nullptr;
	};

}
