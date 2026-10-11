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

	void UndoStack::NotifyApplied(const EditorAction& action, bool undone)
	{
		if (m_Observer)
			m_Observer->OnApplied(action, undone);
	}

	void UndoStack::Push(Scope<EditorAction> action)
	{
		m_RecordCount++;
		// Recording after undoing discards the undone actions; a save point among them becomes unreachable.
		if (m_Position < m_Entries.size())
		{
			m_Entries.erase(m_Entries.begin() + static_cast<std::ptrdiff_t>(m_Position), m_Entries.end());
			if (m_SavedPosition != c_NoSavePoint && m_SavedPosition > m_Position)
				m_SavedPosition = c_NoSavePoint;
			m_MergeBroken = true;
		}

		if (!m_MergeBroken && m_Position > 0 && m_SavedPosition != m_Position && m_Entries.back().Action->MergeWith(*action))
		{
			// Absorbed into the latest step (never across the save point, so saving ends a drag's step). A step that now
			// changes nothing is dropped, and the next edit starts a step of its own. Either way the scene holds what the
			// latest step describes now (a dropped one: what it started from).
			NotifyApplied(*m_Entries.back().Action, false);
			m_LastRecordedSerial = m_Entries.back().Serial;
			if (m_Entries.back().Action->IsNoOp())
			{
				m_LastRecordedSerial = 0;
				m_Entries.pop_back();
				m_Position--;
				m_MergeBroken = true;
			}
			return;
		}

		m_LastRecordedSerial = m_NextSerial;
		m_Entries.push_back({ std::move(action), m_NextSerial++ });
		m_Position = m_Entries.size();
		m_MergeBroken = false;
		NotifyApplied(*m_Entries.back().Action, false);
		if (m_Entries.size() > m_Capacity)
		{
			// A save point before the dropped step stays at the front when that step did not change the scene (a material
			// edit): the scene there is the saved one. After a dropped scene edit, the saved scene can no longer be reached.
			const bool dropsSceneChange = m_Entries.front().Action->ChangesScene();
			m_Entries.pop_front();
			m_Position--;
			if (m_SavedPosition == 0)
				m_SavedPosition = dropsSceneChange ? c_NoSavePoint : 0;
			else if (m_SavedPosition != c_NoSavePoint)
				m_SavedPosition--;
		}
	}

	bool UndoStack::Undo()
	{
		if (!CanUndo())
			return false;
		m_Position--;
		m_Entries[m_Position].Action->Undo();
		m_MergeBroken = true;
		NotifyApplied(*m_Entries[m_Position].Action, true);
		return true;
	}

	bool UndoStack::Redo()
	{
		if (!CanRedo())
			return false;
		std::string error;
		if (!m_Entries[m_Position].Action->Execute(&error))
		{
			// The world changed in a way the action cannot be reapplied to: drop it and everything after it.
			ST_ERROR("Redo of '{}' failed: {}", m_Entries[m_Position].Action->GetName(), error);
			m_Entries.erase(m_Entries.begin() + static_cast<std::ptrdiff_t>(m_Position), m_Entries.end());
			if (m_SavedPosition != c_NoSavePoint && m_SavedPosition > m_Position)
				m_SavedPosition = c_NoSavePoint;
			return false;
		}
		m_Position++;
		m_MergeBroken = true;
		NotifyApplied(*m_Entries[m_Position - 1].Action, false);
		return true;
	}

	bool UndoStack::GoTo(size_t position)
	{
		if (position > m_Entries.size())
			return false;
		while (m_Position > position)
			Undo();
		while (m_Position < position)
		{
			if (!Redo())
				return false;
		}
		return true;
	}

	std::optional<size_t> UndoStack::FindSerial(uint64_t serial) const
	{
		// Serials grow with the index: a binary search finds one in long histories.
		const auto it = std::lower_bound(m_Entries.begin(), m_Entries.end(), serial, [](const Entry& entry, uint64_t value) { return entry.Serial < value; });
		if (it == m_Entries.end() || it->Serial != serial)
			return std::nullopt;
		return static_cast<size_t>(it - m_Entries.begin());
	}

	std::string UndoStack::GetUndoName() const
	{
		return CanUndo() ? m_Entries[m_Position - 1].Action->GetName() : std::string();
	}

	std::string UndoStack::GetRedoName() const
	{
		return CanRedo() ? m_Entries[m_Position].Action->GetName() : std::string();
	}

	std::vector<std::string> UndoStack::GetHistory() const
	{
		std::vector<std::string> names;
		names.reserve(m_Entries.size());
		for (const Entry& entry : m_Entries)
			names.push_back(entry.Action->GetName());
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
			if (m_Entries[index].Action->ChangesScene())
				return true;
		}
		return false;
	}

	void UndoStack::Clear()
	{
		m_Entries.clear();
		m_Position = 0;
		m_SavedPosition = 0;
		m_MergeBroken = true;
	}

}
