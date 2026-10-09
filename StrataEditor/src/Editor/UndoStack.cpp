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
			m_Actions.pop_front();
			m_Position--;
			if (m_SavedPosition != c_NoSavePoint)
				m_SavedPosition = m_SavedPosition == 0 ? c_NoSavePoint : m_SavedPosition - 1;
		}
	}

	bool UndoStack::Undo()
	{
		if (!CanUndo())
			return false;
		m_Position--;
		m_Actions[m_Position]->Undo();
		m_MergeBroken = true;
		return true;
	}

	bool UndoStack::Redo()
	{
		if (!CanRedo())
			return false;
		std::string error;
		if (!m_Actions[m_Position]->Execute(&error))
		{
			// The world changed in a way the action cannot be reapplied to: drop it and everything after it.
			ST_ERROR("Redo of '{}' failed: {}", m_Actions[m_Position]->GetName(), error);
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
		return m_SavedPosition != m_Position;
	}

	void UndoStack::Clear()
	{
		m_Actions.clear();
		m_Position = 0;
		m_SavedPosition = 0;
		m_MergeBroken = true;
	}

}
