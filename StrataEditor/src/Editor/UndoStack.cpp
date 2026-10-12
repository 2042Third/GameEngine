#include "Editor/UndoStack.h"

#include <Strata/Core/Log.h>

#include <algorithm>

namespace Strata
{

	UndoStack::UndoStack(size_t capacity)
		: m_Capacity(std::max<size_t>(capacity, 1))
	{
	}

	bool UndoStack::Execute(Scope<EditorAction> action, std::string* outError)
	{
		if (!action)
			return false;
		if (!action->Execute(outError))
			return false;
		Push(std::move(action));
		return true;
	}

	void UndoStack::Record(Scope<EditorAction> action)
	{
		if (action)
			Push(std::move(action));
	}

	void UndoStack::Push(Scope<EditorAction> action)
	{
		m_ChangeCount++;
		// Recording after undoing discards the undone actions; a save point among them becomes unreachable.
		if (m_Position < m_Actions.size())
		{
			m_Actions.erase(m_Actions.begin() + static_cast<std::ptrdiff_t>(m_Position), m_Actions.end());
			if (m_SavedPosition != c_NoSavePoint && m_SavedPosition > m_Position)
				m_SavedPosition = c_NoSavePoint;
			m_MergeBroken = true;
		}

		if (!m_MergeBroken && m_Position > 0 && m_SavedPosition != m_Position && m_Actions.back()->MergeWith(*action))
		{
			// Absorbed into the latest step (never across the save point, so saving ends a drag's step). A step that now
			// changes nothing is dropped, and the next edit starts a step of its own.
			if (m_Actions.back()->IsNoOp())
			{
				m_Actions.pop_back();
				m_Position--;
				m_MergeBroken = true;
			}
			return;
		}

		m_Actions.push_back(std::move(action));
		m_Position = m_Actions.size();
		m_MergeBroken = false;
		if (m_Actions.size() > m_Capacity)
		{
			// A save point before the dropped step stays at the front when that step did not change the scene (a material
			// edit): the scene there is the saved one. After a dropped scene edit, the saved scene can no longer be reached.
			const bool dropsSceneChange = m_Actions.front()->ChangesScene();
			m_Actions.pop_front();
			m_Position--;
			if (m_SavedPosition == 0)
				m_SavedPosition = dropsSceneChange ? c_NoSavePoint : 0;
			else if (m_SavedPosition != c_NoSavePoint)
				m_SavedPosition--;
		}
	}

	bool UndoStack::Undo(std::string* outError)
	{
		if (!CanUndo())
		{
			if (outError)
				*outError = "Nothing to undo";
			return false;
		}
		std::string error;
		if (!m_Actions[m_Position - 1]->Undo(&error))
		{
			// The step stays applied: nothing changed, and undoing it again works once what blocks it is gone.
			const std::string message = fmt::format("Undo of '{}' failed: {}", m_Actions[m_Position - 1]->GetName(), error);
			ST_ERROR("{}", message);
			if (outError)
				*outError = message;
			m_MergeBroken = true;
			return false;
		}
		m_Position--;
		m_ChangeCount++;
		m_MergeBroken = true;
		return true;
	}

	bool UndoStack::Redo(std::string* outError)
	{
		if (!CanRedo())
		{
			if (outError)
				*outError = "Nothing to redo";
			return false;
		}
		std::string error;
		m_ChangeCount++;
		if (!m_Actions[m_Position]->Execute(&error))
		{
			// The world changed in a way the action cannot be reapplied to: drop it and everything after it.
			const std::string message = fmt::format("Redo of '{}' failed: {}", m_Actions[m_Position]->GetName(), error);
			ST_ERROR("{}", message);
			if (outError)
				*outError = message;
			m_Actions.erase(m_Actions.begin() + static_cast<std::ptrdiff_t>(m_Position), m_Actions.end());
			if (m_SavedPosition != c_NoSavePoint && m_SavedPosition > m_Position)
				m_SavedPosition = c_NoSavePoint;
			return false;
		}
		m_Position++;
		m_MergeBroken = true;
		return true;
	}

	std::string UndoStack::GetUndoName() const
	{
		return CanUndo() ? m_Actions[m_Position - 1]->GetName() : std::string();
	}

	std::string UndoStack::GetRedoName() const
	{
		return CanRedo() ? m_Actions[m_Position]->GetName() : std::string();
	}

	std::vector<std::string> UndoStack::GetHistory() const
	{
		std::vector<std::string> names;
		names.reserve(m_Actions.size());
		for (const Scope<EditorAction>& action : m_Actions)
			names.push_back(action->GetName());
		return names;
	}

	void UndoStack::MarkSaved()
	{
		m_SavedPosition = m_Position;
		m_MergeBroken = true;
	}

	bool UndoStack::IsModified() const
	{
		// A save point that was trimmed or discarded can no longer be reached: whatever lies between is unknown.
		if (m_SavedPosition == c_NoSavePoint)
			return true;
		const size_t first = std::min(m_SavedPosition, m_Position);
		const size_t last = std::max(m_SavedPosition, m_Position);
		for (size_t index = first; index < last; index++)
		{
			if (m_Actions[index]->ChangesScene())
				return true;
		}
		return false;
	}

	void UndoStack::Clear()
	{
		m_ChangeCount++;
		m_Actions.clear();
		m_Position = 0;
		m_SavedPosition = 0;
		m_MergeBroken = true;
	}

	////////////////////////////////////////////////////////////////////////////////
	// EditorActionGroup
	////////////////////////////////////////////////////////////////////////////////

	EditorActionGroup::EditorActionGroup(std::string name)
		: m_Name(std::move(name))
	{
	}

	void EditorActionGroup::Add(Scope<EditorAction> action)
	{
		if (action)
			m_Actions.push_back(std::move(action));
	}

	bool EditorActionGroup::Execute(std::string* outError)
	{
		for (size_t index = 0; index < m_Actions.size(); index++)
		{
			if (!m_Actions[index]->Execute(outError))
			{
				// The group happens as a whole or not at all. The actions that ran revert in the world they just made, so they
				// can; should one not, the log says so.
				for (size_t undone = index; undone > 0; undone--)
				{
					std::string error;
					if (!m_Actions[undone - 1]->Undo(&error))
						ST_ERROR("Reverting '{}' of '{}' failed: {}", m_Actions[undone - 1]->GetName(), m_Name, error);
				}
				return false;
			}
		}
		return true;
	}

	bool EditorActionGroup::Undo(std::string* outError)
	{
		for (size_t index = m_Actions.size(); index > 0; index--)
		{
			if (!m_Actions[index - 1]->Undo(outError))
			{
				// As a whole or not at all: the actions undone already are applied again.
				for (size_t redone = index; redone < m_Actions.size(); redone++)
				{
					std::string error;
					if (!m_Actions[redone]->Execute(&error))
						ST_ERROR("Applying '{}' of '{}' again failed: {}", m_Actions[redone]->GetName(), m_Name, error);
				}
				return false;
			}
		}
		return true;
	}

	bool EditorActionGroup::ChangesScene() const
	{
		for (const Scope<EditorAction>& action : m_Actions)
		{
			if (action->ChangesScene())
				return true;
		}
		return false;
	}

}
