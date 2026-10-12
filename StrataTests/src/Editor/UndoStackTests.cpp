#include <doctest/doctest.h>

#include "Editor/UndoStack.h"

#include <string>
#include <vector>

using namespace Strata;

namespace
{

	// Adds a delta to a shared value; merges with following additions to the same value when mergeable.
	class AddAction final : public EditorAction
	{
	public:
		AddAction(int& value, int delta, bool mergeable = false, bool fails = false)
			: m_Value(value), m_Delta(delta), m_Mergeable(mergeable), m_Fails(fails), m_Name("Add " + std::to_string(delta))
		{
		}

		const std::string& GetName() const override { return m_Name; }

		bool Execute(std::string* outError) override
		{
			if (m_Fails)
			{
				if (outError)
					*outError = "Refused";
				return false;
			}
			m_Value += m_Delta;
			return true;
		}

		bool Undo(std::string* outError) override
		{
			if (m_UndoFails)
			{
				if (outError)
					*outError = "Blocked";
				return false;
			}
			m_Value -= m_Delta;
			return true;
		}

		bool MergeWith(const EditorAction& next) override
		{
			const auto* add = dynamic_cast<const AddAction*>(&next);
			if (!m_Mergeable || !add || !add->m_Mergeable || &add->m_Value != &m_Value)
				return false;
			m_Delta += add->m_Delta;
			return true;
		}

		bool IsNoOp() const override { return m_Delta == 0; }

		void SetFails(bool fails) { m_Fails = fails; }
		void SetUndoFails(bool fails) { m_UndoFails = fails; }
	private:
		int& m_Value;
		int m_Delta;
		bool m_Mergeable;
		bool m_Fails;
		bool m_UndoFails = false;
		std::string m_Name;
	};

	// An edit outside the scene (like an asset document's), which saves itself.
	class OutsideAction final : public EditorAction
	{
	public:
		explicit OutsideAction(int& value)
			: m_Value(value)
		{
		}

		const std::string& GetName() const override
		{
			static const std::string s_Name = "Outside";
			return s_Name;
		}

		bool Execute(std::string*) override
		{
			m_Value++;
			return true;
		}

		bool Undo(std::string*) override
		{
			m_Value--;
			return true;
		}
		bool ChangesScene() const override { return false; }
	private:
		int& m_Value;
	};

}

TEST_SUITE("Editor.Undo")
{
	TEST_CASE("Actions are undone and redone in order")
	{
		int value = 0;
		UndoStack stack;
		CHECK_FALSE(stack.CanUndo());
		CHECK(stack.Execute(CreateScope<AddAction>(value, 1)));
		CHECK(stack.Execute(CreateScope<AddAction>(value, 10)));
		CHECK(value == 11);
		CHECK(stack.GetUndoName() == "Add 10");

		CHECK(stack.Undo());
		CHECK(value == 1);
		CHECK(stack.GetRedoName() == "Add 10");
		CHECK(stack.Undo());
		CHECK(value == 0);
		CHECK_FALSE(stack.Undo());

		CHECK(stack.Redo());
		CHECK(value == 1);
		// A new action discards the redo history.
		CHECK(stack.Execute(CreateScope<AddAction>(value, 100)));
		CHECK_FALSE(stack.CanRedo());
		CHECK(stack.GetHistory() == std::vector<std::string> { "Add 1", "Add 100" });
		CHECK(value == 101);
	}

	TEST_CASE("Failed actions are not recorded")
	{
		int value = 0;
		UndoStack stack;
		std::string error;
		CHECK_FALSE(stack.Execute(CreateScope<AddAction>(value, 5, false, true), &error));
		CHECK(error == "Refused");
		CHECK_FALSE(stack.CanUndo());
		CHECK(value == 0);
	}

	TEST_CASE("Continuous edits merge into one step until the merge is broken")
	{
		int value = 0;
		UndoStack stack;
		stack.Execute(CreateScope<AddAction>(value, 1, true));
		stack.Execute(CreateScope<AddAction>(value, 2, true));
		stack.Execute(CreateScope<AddAction>(value, 3, true));
		CHECK(stack.GetHistory().size() == 1);
		stack.BreakMerge(); // Mouse released
		stack.Execute(CreateScope<AddAction>(value, 4, true));
		CHECK(stack.GetHistory().size() == 2);
		CHECK(value == 10);
		stack.Undo();
		CHECK(value == 6);
		stack.Undo();
		CHECK(value == 0);
	}

	TEST_CASE("A continuous edit that returns to its start leaves no step")
	{
		int value = 0;
		UndoStack stack;
		stack.Execute(CreateScope<AddAction>(value, 5));
		stack.MarkSaved();
		stack.Execute(CreateScope<AddAction>(value, 2, true));
		CHECK(stack.IsModified());
		stack.Execute(CreateScope<AddAction>(value, -2, true));
		CHECK(value == 5);
		CHECK(stack.GetHistory() == std::vector<std::string> { "Add 5" });
		CHECK(stack.GetPosition() == 1);
		CHECK_FALSE(stack.IsModified());

		// The next edit is a step of its own, even though it could have merged.
		stack.Execute(CreateScope<AddAction>(value, 3, true));
		stack.Execute(CreateScope<AddAction>(value, 1, true));
		CHECK(stack.GetHistory() == std::vector<std::string> { "Add 5", "Add 3" });
		REQUIRE(stack.Undo());
		CHECK(value == 5);
	}

	TEST_CASE("The save point tracks modifications")
	{
		int value = 0;
		UndoStack stack;
		CHECK_FALSE(stack.IsModified());
		stack.Execute(CreateScope<AddAction>(value, 1, true));
		CHECK(stack.IsModified());
		stack.MarkSaved();
		CHECK_FALSE(stack.IsModified());

		// Saving ends a merge: the next edit is its own step, so undo returns to the saved state.
		stack.Execute(CreateScope<AddAction>(value, 2, true));
		CHECK(stack.GetHistory().size() == 2);
		CHECK(stack.IsModified());
		stack.Undo();
		CHECK_FALSE(stack.IsModified());
		CHECK(value == 1);
		stack.Undo();
		CHECK(stack.IsModified());

		// A save point in a discarded redo history can never be reached again.
		stack.Redo();
		stack.Undo();
		stack.Execute(CreateScope<AddAction>(value, 5));
		CHECK(stack.IsModified());
		stack.Undo();
		CHECK(stack.IsModified());
	}

	TEST_CASE("Steps that do not change the scene never make it unsaved")
	{
		int value = 0;
		int outside = 0;
		UndoStack stack;
		stack.Execute(CreateScope<OutsideAction>(outside));
		CHECK_FALSE(stack.IsModified());
		stack.Execute(CreateScope<AddAction>(value, 1));
		CHECK(stack.IsModified());
		stack.Execute(CreateScope<OutsideAction>(outside));
		stack.MarkSaved();
		CHECK_FALSE(stack.IsModified());

		// Undoing and redoing outside steps on either side of the save point changes nothing saving the scene would save.
		stack.Execute(CreateScope<OutsideAction>(outside));
		CHECK_FALSE(stack.IsModified());
		stack.Undo();
		stack.Undo();
		CHECK(outside == 1);
		CHECK_FALSE(stack.IsModified());
		stack.Undo();
		CHECK(value == 0);
		CHECK(stack.IsModified());
		stack.Redo();
		CHECK_FALSE(stack.IsModified());
		stack.Redo();
		stack.Redo();
		CHECK(outside == 3);
		CHECK_FALSE(stack.IsModified());
	}

	TEST_CASE("The capacity drops the oldest steps")
	{
		int value = 0;
		UndoStack stack(3);
		stack.MarkSaved();
		for (int step = 1; step <= 5; step++)
			stack.Execute(CreateScope<AddAction>(value, step));
		CHECK(stack.GetHistory() == std::vector<std::string> { "Add 3", "Add 4", "Add 5" });
		while (stack.Undo())
		{
		}
		CHECK(value == 3); // 1 + 2 cannot be undone any more
		CHECK(stack.IsModified()); // The saved state (0) was trimmed away
	}

	TEST_CASE("Trimming steps that do not change the scene keeps the save point")
	{
		int value = 0;
		int outside = 0;
		UndoStack stack(3);
		stack.MarkSaved();
		// Many material edits after a save: the oldest are dropped, the scene is still as saved.
		for (int step = 0; step < 5; step++)
			stack.Execute(CreateScope<OutsideAction>(outside));
		CHECK(stack.GetHistory().size() == 3);
		CHECK_FALSE(stack.IsModified());
		// A scene edit after them makes it modified, and undoing it returns to the saved scene.
		stack.Execute(CreateScope<AddAction>(value, 1));
		CHECK(stack.IsModified());
		REQUIRE(stack.Undo());
		CHECK_FALSE(stack.IsModified());
		// Once a scene edit right after the save point is dropped, the saved state cannot be reached any more.
		stack.Clear();
		stack.MarkSaved();
		stack.Execute(CreateScope<AddAction>(value, 1));
		for (int step = 0; step < 3; step++)
			stack.Execute(CreateScope<OutsideAction>(outside));
		CHECK(stack.IsModified());
		while (stack.Undo())
		{
		}
		CHECK(stack.IsModified());
	}

	TEST_CASE("A redo that fails drops the redo history and the save point in it")
	{
		int value = 0;
		UndoStack stack;
		stack.Execute(CreateScope<AddAction>(value, 1));
		Scope<AddAction> second = CreateScope<AddAction>(value, 2);
		AddAction* secondAction = second.get();
		stack.Execute(std::move(second));
		stack.Execute(CreateScope<AddAction>(value, 3));
		stack.MarkSaved();
		stack.Undo();
		stack.Undo();
		CHECK(value == 1);

		secondAction->SetFails(true); // The world changed so that it cannot be applied again
		CHECK_FALSE(stack.Redo());
		CHECK(value == 1);
		CHECK_FALSE(stack.CanRedo());
		CHECK(stack.GetHistory() == std::vector<std::string> { "Add 1" });
		CHECK(stack.IsModified()); // The saved state (all three applied) is unreachable now
	}

	TEST_CASE("An undo that fails keeps the step applied, says why, and works once nothing blocks it")
	{
		int value = 0;
		UndoStack stack;
		stack.Execute(CreateScope<AddAction>(value, 1));
		Scope<AddAction> second = CreateScope<AddAction>(value, 2);
		AddAction* secondAction = second.get();
		stack.Execute(std::move(second));
		stack.MarkSaved();
		const uint64_t changes = stack.GetChangeCount();

		secondAction->SetUndoFails(true);
		std::string error;
		CHECK_FALSE(stack.Undo(&error));
		CHECK(error == "Undo of 'Add 2' failed: Blocked");
		CHECK(value == 3);
		CHECK(stack.GetPosition() == 2);
		CHECK_FALSE(stack.CanRedo());
		CHECK_FALSE(stack.IsModified());
		CHECK(stack.GetChangeCount() == changes);

		secondAction->SetUndoFails(false);
		CHECK(stack.Undo());
		CHECK(value == 1);
		CHECK(stack.Redo());
		CHECK(value == 3);

		// With nothing to undo or redo, the reason says so.
		UndoStack empty;
		CHECK_FALSE(empty.Undo(&error));
		CHECK(error == "Nothing to undo");
		CHECK_FALSE(empty.Redo(&error));
		CHECK(error == "Nothing to redo");
	}

	TEST_CASE("A group whose member cannot be undone stays applied as a whole")
	{
		int value = 0;
		UndoStack stack;
		Scope<EditorActionGroup> group = CreateScope<EditorActionGroup>("Three");
		Scope<AddAction> first = CreateScope<AddAction>(value, 1);
		AddAction* firstAction = first.get();
		group->Add(std::move(first));
		group->Add(CreateScope<AddAction>(value, 2));
		group->Add(CreateScope<AddAction>(value, 4));
		REQUIRE(stack.Execute(std::move(group)));
		REQUIRE(value == 7);

		// The members after the first are undone before the first refuses: they are applied again.
		firstAction->SetUndoFails(true);
		std::string error;
		CHECK_FALSE(stack.Undo(&error));
		CHECK(error == "Undo of 'Three' failed: Blocked");
		CHECK(value == 7);
		CHECK(stack.GetPosition() == 1);
		firstAction->SetUndoFails(false);
		CHECK(stack.Undo());
		CHECK(value == 0);
	}

	TEST_CASE("Clearing resets history and modification state")
	{
		int value = 0;
		UndoStack stack;
		stack.Execute(CreateScope<AddAction>(value, 1));
		stack.Clear();
		CHECK_FALSE(stack.CanUndo());
		CHECK_FALSE(stack.CanRedo());
		CHECK_FALSE(stack.IsModified());
	}

	TEST_CASE("A group is one step: it runs as a whole or not at all, and undoes in reverse order")
	{
		int value = 0;
		int outside = 0;
		UndoStack stack;
		Scope<EditorActionGroup> group = CreateScope<EditorActionGroup>("Both");
		group->Add(CreateScope<OutsideAction>(outside));
		group->Add(nullptr); // Left out
		group->Add(CreateScope<AddAction>(value, 2));
		CHECK(group->ChangesScene()); // One of its actions does
		const uint64_t changes = stack.GetChangeCount();
		CHECK(stack.Execute(std::move(group)));
		CHECK(stack.GetChangeCount() > changes);
		CHECK(value == 2);
		CHECK(outside == 1);
		CHECK(stack.GetHistory() == std::vector<std::string> { "Both" });
		CHECK(stack.Undo());
		CHECK(value == 0);
		CHECK(outside == 0);
		CHECK(stack.Redo());
		CHECK(value == 2);

		// A failing member undoes those that ran before it.
		Scope<EditorActionGroup> failing = CreateScope<EditorActionGroup>("Refused");
		failing->Add(CreateScope<AddAction>(value, 5));
		failing->Add(CreateScope<AddAction>(value, 7, false, true));
		std::string error;
		CHECK_FALSE(stack.Execute(std::move(failing), &error));
		CHECK(error == "Refused");
		CHECK(value == 2);
		CHECK(stack.GetHistory().size() == 1);

		// Only actions outside the scene: the group does not change it either.
		EditorActionGroup files("Files");
		files.Add(CreateScope<OutsideAction>(outside));
		CHECK_FALSE(files.ChangesScene());
	}
}
