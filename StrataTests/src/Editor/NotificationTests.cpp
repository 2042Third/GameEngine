#include <doctest/doctest.h>

#include "Editor/CommandJournal.h"
#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorNotifications.h"
#include "Editor/HarnessEditor.h"
#include "Panels/ActivityPanel.h"
#include "Panels/ConsolePanel.h"
#include "TestHelpers.h"
#include "UI/EditorActions.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"

#include <Strata/Core/Log.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>

// Notifications (EditorNotifications) and their toasts (UI/Notifications).

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	EditorNotification Make(NotificationLevel level, std::string title, std::string message = {})
	{
		EditorNotification notification;
		notification.Level = level;
		notification.Title = std::move(title);
		notification.Message = std::move(message);
		return notification;
	}

}

TEST_SUITE("Editor.Notifications")
{
	TEST_CASE("Notifications merge repeats, keep the newest few and expire by their level")
	{
		EditorNotifications notifications;
		CHECK(notifications.IsEmpty());
		const uint64_t first = notifications.Post(Make(NotificationLevel::Error, "entity.rename failed", "No entity"));
		const uint64_t version = notifications.GetVersion();
		// The same again: one notification, counted, pointing at the newer journal entry.
		EditorNotification again = Make(NotificationLevel::Error, "entity.rename failed", "No entity");
		again.JournalEntry = 42;
		CHECK(notifications.Post(again) == first);
		REQUIRE(notifications.GetActive().size() == 1);
		CHECK(notifications.GetActive()[0].Count == 2);
		CHECK(notifications.GetActive()[0].JournalEntry == 42);
		CHECK(notifications.GetVersion() > version);
		// Another message is another notification.
		notifications.Post(Make(NotificationLevel::Error, "entity.rename failed", "Other"));
		CHECK(notifications.GetActive().size() == 2);

		// At most c_MaxActive: the oldest go.
		for (int index = 0; index < 6; index++)
			notifications.Post(Make(NotificationLevel::Info, fmt::format("Info {}", index)));
		REQUIRE(notifications.GetActive().size() == EditorNotifications::c_MaxActive);
		CHECK(notifications.GetActive().back().Title == "Info 5");
		CHECK(notifications.GetActive().front().Title == "Info 2");

		// Lifetimes: information goes first, errors last; a notification someone looks at stays.
		notifications.Clear();
		const uint64_t info = notifications.Post(Make(NotificationLevel::Success, "Scripts built"));
		const uint64_t error = notifications.Post(Make(NotificationLevel::Error, "Export failed"));
		const uint64_t kept = notifications.Post(Make(NotificationLevel::Info, "Looked at"));
		const auto now = std::chrono::steady_clock::now();
		notifications.KeepAlive(kept, EditorNotifications::c_ErrorLifetime * 2);
		notifications.Expire(now + EditorNotifications::c_InfoLifetime + std::chrono::milliseconds(100));
		REQUIRE(notifications.GetActive().size() == 2);
		CHECK(notifications.GetActive()[0].Id == error);
		CHECK(notifications.GetActive()[1].Id == kept);
		notifications.Expire(now + EditorNotifications::c_ErrorLifetime + std::chrono::milliseconds(100));
		REQUIRE(notifications.GetActive().size() == 1);
		CHECK(notifications.GetActive()[0].Id == kept);
		notifications.Dismiss(kept);
		CHECK(notifications.IsEmpty());
		notifications.Dismiss(info); // Gone already: nothing happens
		CHECK(notifications.IsEmpty());
	}

	TEST_CASE("Commands the person runs post their failures and warnings; others do not")
	{
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		EditorCommandRunner runner;
		EditorNotifications& notifications = context.GetNotifications();

		// A failure from the UI: an error pointing at the command's journal entry.
		CHECK(RunEditorCommand(context, commands, "entity.rename", { { "entity", "00000000000BAD00" }, { "name", "X" } }).is_null());
		REQUIRE(notifications.GetActive().size() == 1);
		const EditorNotification& failure = notifications.GetActive().back();
		CHECK(failure.Level == NotificationLevel::Error);
		CHECK(failure.Title == "entity.rename failed");
		CHECK_FALSE(failure.Message.empty());
		CHECK(failure.JournalEntry == context.GetJournal().GetLatestId());
		CHECK(context.GetJournal().Find(failure.JournalEntry)->Command == "entity.rename");

		// Successes post nothing; neither do commands of tools and scripts (they get their errors as answers).
		notifications.Clear();
		CHECK_FALSE(RunEditorCommand(context, commands, "entity.create", { { "name", "Fine" } }).is_null());
		CHECK(commands.Execute(context, "entity.rename", { { "entity", "00000000000BAD00" }, { "name", "X" } }).ErrorKind == EditorCommandError::InvalidParameters);
		runner.Run(context, commands, "entity.rename", { { "entity", "00000000000BAD00" }, { "name", "X" } }, nullptr, CommandOrigin { CommandSource::Automation, "Agent" });
		CHECK(notifications.IsEmpty());

		// The runner, for the person: a deferred command's failure when it ends, a warning of an edit of the running game.
		runner.Run(context, commands, "editor.wait", { { "frames", 0 } }, nullptr, CommandOrigin { CommandSource::UI });
		CHECK(notifications.GetActive().size() == 1); // editor.wait refuses 0 frames at once
		notifications.Clear();
		REQUIRE(context.Play());
		runner.Run(context, commands, "entity.create", { { "name", "WhilePlaying" } }, nullptr, CommandOrigin { CommandSource::UI });
		REQUIRE(notifications.GetActive().size() == 1);
		CHECK(notifications.GetActive()[0].Level == NotificationLevel::Warning);
		CHECK(notifications.GetActive()[0].Title == "entity.create");
		context.Stop();
	}
}

TEST_SUITE("Editor.UI.Notifications")
{
	TEST_CASE("A failing UI command shows a toast and a Console entry; the toast leads to the Activity panel")
	{
		HarnessEditor editor({}, WithFeatureProject("NotificationToasts"));
		editor.Frames(2);
		REQUIRE(editor.Context().HasProject());
		CHECK_FALSE(UI::ItemProbe::Find("Toast.0").has_value());

		CHECK(RunEditorCommand(editor.Context(), editor.Layer->GetCommands(), "entity.rename", { { "entity", "00000000000BAD00" }, { "name", "X" } }).is_null());
		const uint64_t entry = editor.Context().GetJournal().GetLatestId();
		editor.Frames(2);
		const std::optional<UI::ItemProbe::Item> toast = UI::ItemProbe::Find("Toast.0");
		REQUIRE(toast.has_value());
		CHECK(toast->Text.find("entity.rename failed") == 0);
		CHECK(toast->Color == UI::ToColorU32(UI::GetThemeColors().Error));
		// The Console has the error too.
		ConsolePanel* console = editor.Layer->GetPanels().Get<ConsolePanel>(EditorPanels::c_Console);
		REQUIRE(console);
		CHECK(console->GetUnreadErrors() >= 1);
		// The toast does not take the keyboard focus.
		CHECK_FALSE(ImGui::GetIO().WantTextInput);

		// Show in Activity: the panel, with the command's entry selected; the toast goes.
		REQUIRE(editor.Click("Toast.0.Activity"));
		editor.Frames(2);
		ActivityPanel* activity = editor.Layer->GetPanels().Get<ActivityPanel>(EditorPanels::c_Activity);
		REQUIRE(activity);
		CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Activity));
		CHECK(activity->GetSelected() == entry);
		CHECK_FALSE(UI::ItemProbe::Find("Toast.0").has_value());

		// Toasts stack, and close.
		RunEditorCommand(editor.Context(), editor.Layer->GetCommands(), "entity.rename", { { "entity", "00000000000BAD00" }, { "name", "Y" } });
		RunEditorCommand(editor.Context(), editor.Layer->GetCommands(), "scene.open", { { "scene", "Scenes/Missing.stscene" } });
		editor.Frames(2);
		REQUIRE(UI::ItemProbe::Find("Toast.1").has_value());
		CHECK(UI::ItemProbe::Find("Toast.0")->Max.y <= UI::ItemProbe::Find("Toast.1")->Min.y);
		REQUIRE(editor.Click("Toast.1.Close"));
		editor.Frames(2);
		CHECK(UI::ItemProbe::Find("Toast.0").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Toast.1").has_value());
		REQUIRE(editor.Click("Toast.0.Console"));
		editor.Frames(2);
		CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Console));
		CHECK_FALSE(UI::ItemProbe::Find("Toast.0").has_value());
		CHECK(editor.Context().GetNotifications().IsEmpty());
	}
}
