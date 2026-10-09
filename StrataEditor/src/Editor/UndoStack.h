#pragma once

#include <Strata/Core/Base.h>

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace Strata
{

	// An undoable edit. Execute applies it (the first time and on redo), Undo reverts it. Actions own all state
	// they need: they must work after any number of other actions were undone and redone around them.
	class EditorAction
	{
	public:
		virtual ~EditorAction() = default;

		virtual const std::string& GetName() const = 0;
		virtual bool Execute(std::string* outError) = 0;
		virtual void Undo() = 0;

		// Continuous edits (dragging a value) become one undo step: when the stack's latest action accepts the next
		// one, it absorbs its effect (the next action has already been applied) and the next action is dropped.
		virtual bool MergeWith(const EditorAction& /*next*/) { return false; }
		// Whether the action changes nothing (e.g. a merged drag that returned to its start). The stack drops a merged
		// step that became a no-op, so dragging a value back and forth leaves no undo step.
		virtual bool IsNoOp() const { return false; }
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

		bool Undo();
		bool Redo();
		bool CanUndo() const { return m_Position > 0; }
		bool CanRedo() const { return m_Position < m_Actions.size(); }
		// Names of the actions undo and redo would apply (empty when unavailable).
		std::string GetUndoName() const;
		std::string GetRedoName() const;
		// Names of all recorded actions, oldest first, and how many of them are applied.
		std::vector<std::string> GetHistory() const;
		size_t GetPosition() const { return m_Position; }

		// The next recorded action starts a new undo step even if it could merge (e.g. the mouse was released).
		void BreakMerge() { m_MergeBroken = true; }

		// Save point: IsModified reports whether the applied actions differ from those at the last MarkSaved.
		void MarkSaved();
		bool IsModified() const;

		void Clear();
	private:
		void Push(Scope<EditorAction> action);
	private:
		std::deque<Scope<EditorAction>> m_Actions;
		size_t m_Position = 0;   // Number of applied actions (index of the next redo)
		size_t m_Capacity;
		bool m_MergeBroken = true;
		// Position of the save point; c_NoSavePoint when it was trimmed or discarded with the redo history.
		size_t m_SavedPosition = 0;
		static constexpr size_t c_NoSavePoint = static_cast<size_t>(-1);
	};

}
