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

	void UndoStack::AddObserver(UndoStackObserver* observer)
	{
		if (observer && std::find(m_Observers.begin(), m_Observers.end(), observer) == m_Observers.end())
			m_Observers.push_back(observer);
	}

	void UndoStack::RemoveObserver(UndoStackObserver* observer)
	{
		std::erase(m_Observers, observer);
	}

	void UndoStack::NotifyApplied(const EditorAction& action, bool undone, bool merged)
	{
		for (UndoStackObserver* observer : m_Observers)
			observer->OnApplied(action, undone, merged);
	}

	void UndoStack::Push(Scope<EditorAction> action)
	{
		m_RecordCount++;
		m_Version++;
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
			NotifyApplied(*m_Entries.back().Action, false, true);
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

	bool UndoStack::Undo(std::string* outError)
	{
		if (!CanUndo())
		{
			if (outError)
				*outError = "Nothing to undo";
			return false;
		}
		std::string error;
		if (!m_Entries[m_Position - 1].Action->Undo(&error))
		{
			// The step stays applied: nothing changed, and undoing it again works once what blocks it is gone.
			const std::string message = fmt::format("Undo of '{}' failed: {}", m_Entries[m_Position - 1].Action->GetName(), error);
			ST_ERROR("{}", message);
			if (outError)
				*outError = message;
			m_MergeBroken = true;
			return false;
		}
		m_Version++;
		m_Position--;
		m_MergeBroken = true;
		NotifyApplied(*m_Entries[m_Position].Action, true);
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
		// Also a failed redo changes the history (it drops the steps it cannot apply).
		m_Version++;
		std::string error;
		if (!m_Entries[m_Position].Action->Execute(&error))
		{
			// The world changed in a way the action cannot be reapplied to: drop it and everything after it.
			const std::string message = fmt::format("Redo of '{}' failed: {}", m_Entries[m_Position].Action->GetName(), error);
			ST_ERROR("{}", message);
			if (outError)
				*outError = message;
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
		{
			if (!Undo())
				return false;
		}
		while (m_Position < position)
		{
			if (!Redo())
				return false;
		}
		return true;
	}

	std::optional<size_t> UndoStack::FindSerial(uint64_t serial) const
	{
		// The step just recorded is the latest: found without a search through the history.
		if (!m_Entries.empty() && m_Entries.back().Serial == serial)
			return m_Entries.size() - 1;
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
		m_Version++;
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
		m_Version++;
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

	const EditorAction* EditorActionGroup::GetSceneMember() const
	{
		const EditorAction* member = nullptr;
		for (const Scope<EditorAction>& action : m_Actions)
		{
			if (action->GetSceneChange() == SceneChangeKind::None)
				continue;
			if (member)
				return nullptr;
			member = action.get();
		}
		return member;
	}

	SceneChangeKind EditorActionGroup::GetSceneChange() const
	{
		if (const EditorAction* member = GetSceneMember())
			return member->GetSceneChange();
		const bool any = std::any_of(m_Actions.begin(), m_Actions.end(), [](const Scope<EditorAction>& action)
		{
			return action->GetSceneChange() != SceneChangeKind::None;
		});
		return any ? SceneChangeKind::Other : SceneChangeKind::None;
	}

	Ref<const std::vector<EntityState>> EditorActionGroup::GetEntityStates(bool undone) const
	{
		const EditorAction* member = GetSceneMember();
		return member ? member->GetEntityStates(undone) : nullptr;
	}

}
