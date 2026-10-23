#include <doctest/doctest.h>

#include "Core/SettingsTestUtils.h"
#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorInterface.h"
#include "Editor/EditorPreferences.h"
#include "Editor/ExternalEditor.h"
#include "Editor/HarnessEditor.h"
#include "Editor/SettingsFileWriter.h"
#include "Editor/UndoStack.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/SettingsRegistry.h>
#include <Strata/Core/Timestep.h>
#include <Strata/ImGui/ImGuiLayer.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	EditorContextSpecification MakeSpecification(const std::filesystem::path& preferencesFile, const SettingsRegistry* settings = nullptr)
	{
		EditorContextSpecification specification;
		specification.WatchAssetFiles = false;
		specification.PreferencesFile = preferencesFile;
		specification.Settings = settings;
		return specification;
	}

	struct PreferencesHarness
	{
		EditorContext Context;
		EditorCommandRegistry Commands;

		explicit PreferencesHarness(const std::filesystem::path& preferencesFile = {}, const SettingsRegistry* settings = nullptr)
			: Context(MakeSpecification(preferencesFile, settings))
		{
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return RunCommandToCompletion(Context, Commands, name, parameters);
		}

		EditorCommandResult Execute(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return CompleteCommand(Context, Commands.Execute(Context, name, parameters));
		}

		// Editor frames until no write of the preferences is in flight; false when one still is after `timeout`.
		bool WaitForWrites(std::chrono::milliseconds timeout = std::chrono::seconds(30))
		{
			const auto deadline = std::chrono::steady_clock::now() + timeout;
			while (Context.GetPreferences().IsWriting())
			{
				if (std::chrono::steady_clock::now() > deadline)
					return false;
				Context.Update(Timestep(1.0f / 60.0f));
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return true;
		}
	};

	std::string GetCommand(const EditorPreferences& preferences)
	{
		const nlohmann::json* value = preferences.FindSection(ExternalEditor::c_SettingsSection);
		return value ? JsonUtils::GetString(*value, "Command") : std::string();
	}

	bool Logged(const LogCapture& log, std::string_view text)
	{
		for (const LogEntry& entry : log.GetEntries())
		{
			if (entry.Message.find(text) != std::string::npos)
				return true;
		}
		return false;
	}

	// A registry with the editor's sections, the test sections (project) and a user test section.
	struct PreferencesRegistry
	{
		SettingsRegistry Registry;

		PreferencesRegistry()
		{
			EditorPreferences::RegisterSections(Registry);
			RegisterTestSections(Registry);
			REQUIRE(Registry.Register(MakeUserTestSection()));
			Registry.Freeze();
		}
	};

}

TEST_SUITE("Editor.Preferences")
{
	TEST_CASE("A 0.5.0 EditorSettings.json is imported once and left as it was")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("PreferencesImport") / "Strata";
		const std::filesystem::path legacy = directory / "EditorSettings.json";
		const std::filesystem::path file = directory / "Preferences.json";
		const std::string legacyText = "{\n\t\"ExternalEditor\": \"code -g {file}:{line}\",\n\t\"Other\": 1\n}\n";
		REQUIRE(Platform::WritePrivateFile(legacy, legacyText));
		{
			EditorPreferences preferences(SettingsRegistry::GetProcessRegistry(), file, legacy);
			CHECK(GetCommand(preferences) == "code -g {file}:{line}");
		}
		// The preferences file was written at once, so the import is not repeated; 0.5.0's file is left as it was.
		REQUIRE(FileSystem::Exists(file));
		CHECK(FileSystem::ReadText(legacy) == legacyText);
		const std::optional<nlohmann::json> written = JsonUtils::Parse(FileSystem::ReadText(file).value_or(""));
		REQUIRE(written);
		CHECK((*written)["Strata"] == nlohmann::json { { "Format", "Preferences" }, { "Version", 1 } });
		CHECK((*written)["Settings"][ExternalEditor::c_SettingsSection] == nlohmann::json { { "Version", 1 }, { "Value", { { "Command", "code -g {file}:{line}" } } } });

		// A 0.5.0 editor that changes its file afterwards changes nothing here, and a change here never reaches that file.
		const std::string laterText = "{ \"ExternalEditor\": \"clion --line {line} {file}\" }\n";
		REQUIRE(Platform::WritePrivateFile(legacy, laterText));
		{
			EditorPreferences preferences(SettingsRegistry::GetProcessRegistry(), file, legacy);
			CHECK(GetCommand(preferences) == "code -g {file}:{line}");
			REQUIRE(preferences.Set(ExternalEditor::c_SettingsSection, { { "Command", "" } }));
		}
		CHECK(FileSystem::ReadText(legacy) == laterText);
		EditorPreferences reopened(SettingsRegistry::GetProcessRegistry(), file, legacy);
		CHECK(GetCommand(reopened).empty());
		CHECK(reopened.IsStored(ExternalEditor::c_SettingsSection));

		// Nothing to import (no external editor, or one that cannot be used) writes nothing: the next start looks again.
		const std::filesystem::path other = CreateTemporaryDirectory("PreferencesNoImport") / "Strata";
		REQUIRE(Platform::WritePrivateFile(other / "EditorSettings.json", "{ \"ExternalEditor\": \"edit.bat {file}\" }\n"));
		{
			LogCapture log;
			EditorPreferences preferences(SettingsRegistry::GetProcessRegistry(), other / "Preferences.json", other / "EditorSettings.json");
			CHECK(GetCommand(preferences).empty());
			CHECK(Logged(log, "is not imported"));
		}
		CHECK_FALSE(FileSystem::Exists(other / "Preferences.json"));
	}

	TEST_CASE("The file is private and read bounded")
	{
		PreferencesRegistry settings;
		const std::filesystem::path file = CreateTemporaryDirectory("PreferencesPrivate") / "Strata" / "Preferences.json";
		{
			EditorPreferences preferences(settings.Registry, file);
			REQUIRE(preferences.Set("Test.Preferences", { { "Note", "Hello" }, { "Size", 3 } }));
			// A change the file could not hold is refused, naming the bound, and changes nothing.
			std::string error;
			CHECK_FALSE(preferences.Set("Test.Preferences", { { "Note", std::string(70 * 1024, 'x') }, { "Size", 3 } }, &error));
			CHECK(error.find("64 KiB") != std::string::npos);
			CHECK((*preferences.FindSection("Test.Preferences"))["Note"] == "Hello");
		}
		CHECK(Platform::IsTrustedFile(file));
		{
			EditorPreferences preferences(settings.Registry, file);
			REQUIRE(preferences.FindSection("Test.Preferences"));
			CHECK((*preferences.FindSection("Test.Preferences"))["Size"] == 3);
		}

		// A file over 64 KiB is not read: its preferences are ignored, with a warning.
		const nlohmann::json large = { { "Strata", { { "Format", "Preferences" }, { "Version", 1 } } },
			{ "Settings", { { "Test.Preferences", { { "Version", 1 }, { "Value", { { "Note", std::string(65 * 1024, 'x') }, { "Size", 4 } } } } } } } };
		REQUIRE(Platform::WritePrivateFile(file, JsonUtils::Dump(large)));
		{
			LogCapture log;
			EditorPreferences preferences(settings.Registry, file);
			CHECK_FALSE(preferences.FindSection("Test.Preferences"));
			CHECK(Logged(log, "are ignored"));
		}
		// Nor is one that is not JSON, or one of a later format.
		for (const std::string& text : { std::string("not json"), std::string("{ \"Strata\": { \"Format\": \"Preferences\", \"Version\": 2 }, \"Settings\": {} }") })
		{
			REQUIRE(Platform::WritePrivateFile(file, text));
			LogCapture log;
			EditorPreferences preferences(settings.Registry, file);
			CHECK_FALSE(preferences.FindSection("Test.Preferences"));
			CHECK(Logged(log, "are ignored"));
		}
#if !defined(ST_PLATFORM_WINDOWS)
		// Nor one others could have written: the preferences name a program the editor runs.
		REQUIRE(Platform::WritePrivateFile(file, JsonUtils::Dump({ { "Strata", { { "Format", "Preferences" }, { "Version", 1 } } },
			{ "Settings", { { ExternalEditor::c_SettingsSection, { { "Version", 1 }, { "Value", { { "Command", "code {file}" } } } } } } } })));
		std::error_code permissionError;
		std::filesystem::permissions(file, std::filesystem::perms::others_write, std::filesystem::perm_options::add, permissionError);
		REQUIRE_FALSE(permissionError);
		{
			LogCapture log;
			EditorPreferences preferences(settings.Registry, file);
			CHECK(GetCommand(preferences).empty());
			CHECK(Logged(log, "are ignored"));
		}
#endif
	}

	TEST_CASE("Stored preferences a section refuses or no section claims are kept as stored")
	{
		PreferencesRegistry settings;
		const std::filesystem::path file = CreateTemporaryDirectory("PreferencesKept") / "Strata" / "Preferences.json";
		const nlohmann::json refused = { { "Version", 1 }, { "Value", { { "Command", "edit.bat {file}" } } } };
		const nlohmann::json unclaimed = { { "Version", 3 }, { "Value", { { "Anything", true } } } };
		REQUIRE(Platform::WritePrivateFile(file, JsonUtils::Dump({ { "Strata", { { "Format", "Preferences" }, { "Version", 1 } } },
			{ "Settings", { { ExternalEditor::c_SettingsSection, refused }, { "Later.Feature", unclaimed } } } })));
		{
			LogCapture log;
			EditorPreferences preferences(settings.Registry, file);
			CHECK(GetCommand(preferences).empty());
			REQUIRE(preferences.FindProblem(ExternalEditor::c_SettingsSection));
			CHECK(preferences.FindProblem(ExternalEditor::c_SettingsSection)->find("batch") != std::string::npos);
			CHECK(Logged(log, "is ignored"));
			CHECK(preferences.GetUnclaimedSections() == std::vector<std::string> { "Later.Feature" });
			REQUIRE(preferences.Set("Test.Preferences", { { "Note", "" }, { "Size", 1 } }));
		}
		const std::optional<nlohmann::json> written = JsonUtils::Parse(FileSystem::ReadText(file).value_or(""));
		REQUIRE(written);
		CHECK((*written)["Settings"][ExternalEditor::c_SettingsSection] == refused);
		CHECK((*written)["Settings"]["Later.Feature"] == unclaimed);
	}

	TEST_CASE("A preferences file of a later format is left as it is")
	{
		PreferencesRegistry settings;
		const std::filesystem::path file = CreateTemporaryDirectory("PreferencesLater") / "Strata" / "Preferences.json";
		const std::string later = JsonUtils::Dump({ { "Strata", { { "Format", "Preferences" }, { "Version", 2 } } },
			{ "Settings", { { ExternalEditor::c_SettingsSection, { { "Version", 1 }, { "Value", { { "Command", "code {file}" } } } } } } } });
		REQUIRE(Platform::WritePrivateFile(file, later));
		PreferencesHarness harness(file, &settings.Registry);
		const EditorPreferences& preferences = harness.Context.GetPreferences();
		// This editor cannot read it: the defaults apply, and it says why nothing can be changed.
		CHECK(GetCommand(preferences).empty());
		REQUIRE_FALSE(preferences.GetReadOnlyReason().empty());
		CHECK(preferences.GetReadOnlyReason().find("later editor") != std::string::npos);
		CHECK(harness.Run("preferences.get")["readOnly"] == preferences.GetReadOnlyReason());

		// Changes are refused, through every path, rather than replacing the later editor's preferences.
		const EditorCommandResult set = harness.Execute("preferences.set", { { "section", "Test.Preferences" }, { "value", { { "Size", 3 } } } });
		CHECK_FALSE(set.Success);
		CHECK(set.Error.find("later editor") != std::string::npos);
		CHECK_FALSE(harness.Execute("preferences.set", { { "section", ExternalEditor::c_SettingsSection }, { "reset", true } }).Success);
		std::string error;
		CHECK_FALSE(harness.Context.GetExternalEditor().SetUserCommand("", &error));
		CHECK_FALSE(harness.Execute("preferences.save").Success);
		CHECK_FALSE(preferences.FindSection("Test.Preferences"));
		REQUIRE(harness.WaitForWrites());
		CHECK(harness.Context.GetPreferences().GetWriter().GetWriteCount() == 0);
		CHECK(FileSystem::ReadText(file) == later);
	}

	TEST_CASE("STRATA_EXTERNAL_EDITOR still wins")
	{
		ScopedEnvironmentVariable noOverride("STRATA_EXTERNAL_EDITOR", "");
		PreferencesHarness harness(CreateTemporaryDirectory("PreferencesEnvironment") / "Preferences.json");
		const nlohmann::json set = harness.Run("preferences.set", { { "section", ExternalEditor::c_SettingsSection },
			{ "value", { { "Command", "myeditor -g {file}:{line}" } } } });
		CHECK(set["value"]["Command"] == "myeditor -g {file}:{line}");
		CHECK(harness.Run("script.status")["externalEditor"] == nlohmann::json { { "command", "myeditor -g {file}:{line}" }, { "source", "user" } });
		{
			ScopedEnvironmentVariable environment("STRATA_EXTERNAL_EDITOR", "subl {file}:{line}");
			CHECK(harness.Run("script.status")["externalEditor"] == nlohmann::json { { "command", "subl {file}:{line}" }, { "source", "environment" } });
			// The preference stays the person's.
			CHECK(harness.Context.GetExternalEditor().GetUserCommand() == "myeditor -g {file}:{line}");
		}
		// A template that cannot be one is refused by the section, as script.setExternalEditor refuses it.
		const EditorCommandResult refused = harness.Execute("preferences.set", { { "section", ExternalEditor::c_SettingsSection },
			{ "value", { { "Command", "edit.cmd {file}" } } } });
		CHECK(refused.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Context.GetExternalEditor().GetUserCommand() == "myeditor -g {file}:{line}");
	}

	TEST_CASE("An empty path keeps preferences in memory")
	{
		PreferencesHarness harness;
		CHECK(harness.Context.GetPreferences().GetFile().empty());
		harness.Run("preferences.set", { { "section", ExternalEditor::c_SettingsSection }, { "value", { { "Command", "code {file}" } } } });
		const nlohmann::json described = harness.Run("preferences.get", { { "section", ExternalEditor::c_SettingsSection } });
		CHECK(described["file"].is_null());
		CHECK(described["fileState"] == "written");
		REQUIRE(described["sections"].size() == 1);
		CHECK(described["sections"][0]["value"]["Command"] == "code {file}");
		CHECK(described["sections"][0]["isDefault"] == false);
		CHECK(harness.Run("preferences.save")["file"].is_null());
		CHECK(harness.Context.GetPreferences().GetWriter().GetWriteCount() == 0);
		CHECK_FALSE(harness.Context.HasSavesInFlight());
		harness.Run("preferences.set", { { "section", ExternalEditor::c_SettingsSection }, { "reset", true } });
		CHECK(harness.Run("preferences.get")["sections"][0]["isDefault"] == true);
	}

	TEST_CASE("Scale applies through the host and --ui-scale wins")
	{
		{
			HarnessEditor editor;
			// The layer gives the host the preferences' factor from its first frame: Automatic, 100% of the display's scale.
			editor.Host->UIScaleFactor = 0.5f;
			editor.Frames(1);
			CHECK(editor.Host->UIScaleFactor == 1.0f);
			const size_t steps = editor.Context().GetUndoStack().GetCount();
			editor.Run("preferences.set", { { "section", EditorInterface::c_SettingsSection }, { "value", { { "Scale", "125%" } } } });
			editor.Frames(1);
			CHECK(editor.Host->UIScaleFactor == doctest::Approx(1.25f));
			CHECK(editor.Context().GetUndoStack().GetCount() == steps);
			// A percentage the section does not offer changes nothing (100% is Automatic).
			CHECK(editor.Layer->GetCommands().Execute(editor.Context(), "preferences.set",
				{ { "section", EditorInterface::c_SettingsSection }, { "value", { { "Scale", "100%" } } } }).ErrorKind == EditorCommandError::InvalidParameters);
			editor.Frames(1);
			CHECK(editor.Host->UIScaleFactor == doctest::Approx(1.25f));
			editor.Run("preferences.set", { { "section", EditorInterface::c_SettingsSection }, { "value", { { "Scale", "75%" } } } });
			editor.Frames(1);
			CHECK(editor.Host->UIScaleFactor == doctest::Approx(0.75f));
			editor.Run("preferences.set", { { "section", EditorInterface::c_SettingsSection }, { "reset", true } });
			editor.Frames(1);
			CHECK(editor.Host->UIScaleFactor == 1.0f);
		}

		// The editor's host gives the factor to the ImGui layer: it scales the display's scale on every display, and the
		// override --ui-scale sets wins over it.
		ImGuiLayer layer;
		layer.SetContentScale(2.0f);
		layer.SetContentScaleFactor(1.25f);
		CHECK(layer.GetUIScale() == doctest::Approx(2.5f));
		layer.SetContentScale(1.0f);
		CHECK(layer.GetUIScale() == doctest::Approx(1.25f));
		layer.SetContentScaleOverride(1.5f);
		CHECK(layer.GetUIScale() == doctest::Approx(1.5f));
		layer.SetContentScaleFactor(2.0f);
		CHECK(layer.GetUIScale() == doctest::Approx(1.5f));
		layer.SetContentScaleOverride(0.0f);
		CHECK(layer.GetUIScale() == doctest::Approx(2.0f));
		// The product stays within the layer's range, and factors that are no scale are ignored.
		layer.SetContentScale(3.0f);
		CHECK(layer.GetUIScale() == ImGuiLayer::c_MaxScale);
		layer.SetContentScaleFactor(0.0f);
		layer.SetContentScaleFactor(-1.0f);
		CHECK(layer.GetUIScale() == ImGuiLayer::c_MaxScale);
		layer.SetContentScaleFactor(0.75f);
		CHECK(layer.GetUIScale() == doctest::Approx(2.25f));
	}

	TEST_CASE("Preference commands are Session commands and record no undo")
	{
		const EditorCommandRegistry registry;
		REQUIRE(registry.Find("preferences.get"));
		CHECK(registry.Find("preferences.get")->Kind == EditorCommandKind::Query);
		CHECK(registry.Find("preferences.get")->Results == EditorCommandResults::Session);
		CHECK(registry.Find("preferences.set")->Kind == EditorCommandKind::Session);
		CHECK(registry.Find("preferences.save")->Kind == EditorCommandKind::Session);

		PreferencesRegistry settings;
		PreferencesHarness harness({}, &settings.Registry);
		const std::filesystem::path directory = CreateTemporaryDirectory("PreferencesNoUndo") / "Game";
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" }, { "template", "empty" } });
		const size_t steps = harness.Context.GetUndoStack().GetCount();
		const bool modified = harness.Context.IsSceneModified();
		harness.Run("preferences.set", { { "section", "Test.Preferences" }, { "value", { { "Size", 4 } } } });
		CHECK(harness.Context.GetUndoStack().GetCount() == steps);
		CHECK(harness.Context.IsSceneModified() == modified);
		// Fields left out take their defaults.
		CHECK(*harness.Context.GetPreferences().FindSection("Test.Preferences") == nlohmann::json { { "Note", "" }, { "Size", 4 } });

		// What cannot be set changes nothing: a bad value, a project section, an unknown one.
		CHECK(harness.Execute("preferences.set", { { "section", "Test.Preferences" }, { "value", { { "Size", 9 } } } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		const EditorCommandResult project = harness.Execute("preferences.set", { { "section", "Test.Simple" }, { "value", nlohmann::json::object() } });
		CHECK(project.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(project.Error.find("project setting") != std::string::npos);
		CHECK(harness.Execute("preferences.set", { { "section", "Test.Missing" }, { "reset", true } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Execute("preferences.set", { { "section", "Test.Preferences" } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Execute("project.setSettings", { { "section", "Test.Preferences" }, { "value", nlohmann::json::object() } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		CHECK(*harness.Context.GetPreferences().FindSection("Test.Preferences") == nlohmann::json { { "Note", "" }, { "Size", 4 } });
		CHECK(harness.Context.GetUndoStack().GetCount() == steps);
	}

	TEST_CASE("editor.quit after a failed preferences write fails unless force, and preferences.save writes it again")
	{
		const std::filesystem::path file = CreateTemporaryDirectory("PreferencesUnwritten") / "Strata" / "Preferences.json";
		// A directory in the file's place makes its writes fail.
		REQUIRE(FileSystem::CreateDirectories(file));
		PreferencesHarness harness(file);
		harness.Run("preferences.set", { { "section", ExternalEditor::c_SettingsSection }, { "value", { { "Command", "code {file}" } } } });
		REQUIRE(harness.WaitForWrites());
		REQUIRE(harness.Context.HasUnwrittenPreferences());
		// The change stays in memory, and the failure is reported.
		CHECK(harness.Context.GetExternalEditor().GetUserCommand() == "code {file}");
		const nlohmann::json described = harness.Run("preferences.get");
		CHECK(described["fileState"] == "unwritten");
		CHECK(described["fileError"].is_string());

		const EditorCommandResult refused = harness.Execute("editor.quit");
		CHECK_FALSE(refused.Success);
		CHECK(refused.Error.find("preferences.save") != std::string::npos);
		CHECK_FALSE(harness.Context.IsQuitRequested());
		CHECK_FALSE(harness.Execute("preferences.save").Success);

		REQUIRE(FileSystem::Remove(file));
		CHECK(harness.Run("preferences.save")["file"] == FileSystem::ToUTF8(file));
		CHECK_FALSE(harness.Context.HasUnwrittenPreferences());
		EditorPreferences reread(SettingsRegistry::GetProcessRegistry(), file);
		CHECK(GetCommand(reread) == "code {file}");

		// With force, quitting discards what could not be written, and says so.
		REQUIRE(FileSystem::Remove(file));
		REQUIRE(FileSystem::CreateDirectories(file));
		harness.Run("preferences.set", { { "section", ExternalEditor::c_SettingsSection }, { "reset", true } });
		REQUIRE(harness.WaitForWrites());
		REQUIRE(harness.Context.HasUnwrittenPreferences());
		const nlohmann::json quit = harness.Run("editor.quit", { { "force", true } });
		CHECK(quit["discardedPreferences"] == true);
		CHECK(harness.Context.IsQuitRequested());
	}
}
