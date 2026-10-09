#include <doctest/doctest.h>

#include "Editor/UndoStack.h"

#include <string>

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

		void Undo() override { m_Value -= m_Delta; }

		bool MergeWith(const EditorAction& next) override
		{
			const auto* add = dynamic_cast<const AddAction*>(&next);
			if (!m_Mergeable || !add || !add->m_Mergeable || &add->m_Value != &m_Value)
				return false;
			m_Delta += add->m_Delta;
			return true;
		}

		void SetFails(bool fails) { m_Fails = fails; }
	private:
		int& m_Value;
		int m_Delta;
		bool m_Mergeable;
		bool m_Fails;
		std::string m_Name;
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
}
