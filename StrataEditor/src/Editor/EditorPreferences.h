#pragma once

#include <Strata/Core/Base.h>

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class SettingsFileWriter;
	class SettingsRegistry;
	enum class SettingsFileState : uint8_t;

	// The person's preferences: the values of the settings registry's user sections (SettingsScope::User), such as the
	// external editor (Editor.ExternalEditor), kept for every project in <user data>/Strata/Preferences.json:
	//   { "Strata": { "Format": "Preferences", "Version": 1 }, "Settings": { "<Id>": { "Version": n, "Value": ... } } }
	// (Docs/DeveloperGuide.md, "Preferences"). Like the project's settings, a stored section is validated by its section,
	// kept as it is when its section refuses it or no registered section claims it, and provided only when valid; a section
	// nobody set has its default. Changes are written in the background (SettingsFileWriter): a failed write keeps the change
	// in memory and leaves the file unwritten until a later write succeeds. Main thread only.
	class EditorPreferences
	{
	public:
		// The preferences file is read and written only up to this size.
		static constexpr size_t c_MaxFileBytes = 64 * 1024;
		static constexpr uint32_t c_FormatVersion = 1;

		// <user data>/Strata/Preferences.json, or nothing where the system has no per-user data directory.
		static std::optional<std::filesystem::path> GetDefaultFile();
		// Registers the editor's preference sections (Editor.ExternalEditor, Editor.Interface) in `registry`: the editor's registration through
		// ModuleRegistrationOptions::Extra, and the tests'.
		static void RegisterSections(SettingsRegistry& registry);

		// Validates with `registry`'s user sections. `file` keeps the preferences (empty: in memory only, and nothing is
		// imported: the tests' pattern). While `file` does not exist, the external editor of `importFile` (0.5.0's
		// EditorSettings.json, read only and never written) is imported, and the preferences file is written at once when it
		// was, so the import happens once. `onWritten` is told how each write ended (an empty error when it succeeded).
		EditorPreferences(const SettingsRegistry& registry, std::filesystem::path file = {}, const std::filesystem::path& importFile = {},
			std::function<void(const std::string& error)> onWritten = {});
		// Finishes the write in flight.
		~EditorPreferences();

		EditorPreferences(const EditorPreferences&) = delete;
		EditorPreferences& operator=(const EditorPreferences&) = delete;

		// The section's validated value; null when it is not set, its stored value is refused or no user section has that id
		// (the default applies). Valid until GetVersion changes.
		const nlohmann::json* FindSection(std::string_view id) const;
		// Why the stored value of a section is refused; nothing when it is valid or not stored.
		std::optional<std::string> FindProblem(std::string_view id) const;
		bool IsStored(std::string_view id) const;
		// Stored sections no registered section claims (kept as stored), sorted.
		std::vector<std::string> GetUnclaimedSections() const;
		// Changes whenever a value changes.
		uint64_t GetVersion() const { return m_Version; }
		// Why the file is left as it is, empty when the editor may write it: a later editor wrote it (a later format, which this
		// editor cannot read), and replacing it would discard that editor's preferences. Every section has its default then,
		// and changes are refused (Set, Reset, Save).
		const std::string& GetReadOnlyReason() const { return m_ReadOnlyReason; }

		// Sets a user section to `value` (validated by its section) and queues a write of the file. False, with nothing
		// changed and the reason, when no user section has that id, the value is invalid, the file would outgrow
		// c_MaxFileBytes or is left as it is (GetReadOnlyReason).
		bool Set(std::string_view id, const nlohmann::json& value, std::string* outError = nullptr);
		// Removes a stored section (its default applies again) and queues a write; false when it was not stored.
		bool Reset(std::string_view id);

		// Queues a write of the file as it is now (preferences.save: after a failed write, once its cause is gone); nothing while
		// the file is left as it is (GetReadOnlyReason).
		void Save();
		// Once per frame: publishes a write that ended and starts the next one.
		void Update();
		// Blocks until no write is in flight (the editor going away).
		void Finish();
		// Where the file stands (Written in memory) and, while it is Unwritten, why its last write failed.
		SettingsFileState GetFileState() const;
		std::string GetFileError() const;
		// The last write failed and none succeeded since: what changed is only in memory.
		bool HasUnwrittenPreferences() const;
		bool IsWriting() const;
		// Empty in memory.
		const std::filesystem::path& GetFile() const { return m_File; }
		const SettingsRegistry& GetRegistry() const { return m_Registry; }
		// Tests (SettingsFileWriter::SetTestHook).
		SettingsFileWriter& GetWriter() { return *m_Writer; }
	private:
		struct Data;

		void Load();
		void Import(const std::filesystem::path& importFile);
		// Validates the stored entries with the registry: valid ones are provided, the others logged.
		void ValidateStored();
	private:
		const SettingsRegistry& m_Registry;
		std::filesystem::path m_File;
		std::string m_ReadOnlyReason;
		Scope<Data> m_Data; // Never null
		uint64_t m_Version;
		Scope<SettingsFileWriter> m_Writer;
	};

}
