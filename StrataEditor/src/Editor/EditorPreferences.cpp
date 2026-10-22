#include "Editor/EditorPreferences.h"

#include "Editor/ExternalEditor.h"
#include "Editor/SettingsFileWriter.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/SettingsRegistry.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <limits>
#include <map>
#include <utility>

namespace Strata
{

	namespace
	{

		constexpr const char* c_FormatName = "Preferences";
		// 0.5.0's settings file kept the external editor's command as this top-level member.
		constexpr const char* c_ImportedExternalEditorKey = "ExternalEditor";

		// Versions are unique across the process, so a view that keeps one notices preferences replaced by others.
		std::atomic<uint64_t> s_NextVersion = 1;

		uint64_t MakeVersion()
		{
			return s_NextVersion.fetch_add(1, std::memory_order_relaxed);
		}

	}

	struct EditorPreferences::Data
	{
		struct Entry
		{
			uint32_t Version = 1;   // Of the section, as the value was stored
			nlohmann::json Value;
			bool Valid = false;     // Validated by a registered user section, and provided
			std::string Problem;    // Why its section refused it
		};
		std::map<std::string, Entry, std::less<>> Entries;

		nlohmann::json ToFileJson() const
		{
			nlohmann::json settings = nlohmann::json::object();
			for (const auto& [id, entry] : Entries)
				settings[id] = { { "Version", entry.Version }, { "Value", entry.Value } };
			return { { "Strata", { { "Format", c_FormatName }, { "Version", c_FormatVersion } } }, { "Settings", std::move(settings) } };
		}
	};

	std::optional<std::filesystem::path> EditorPreferences::GetDefaultFile()
	{
		const std::optional<std::filesystem::path> directory = Platform::FindUserDataDirectory("Strata");
		if (!directory)
			return std::nullopt;
		return *directory / "Preferences.json";
	}

	void EditorPreferences::RegisterSections(SettingsRegistry& registry)
	{
		registry.Register(ExternalEditor::MakeSettingsSection());
	}

	EditorPreferences::EditorPreferences(const SettingsRegistry& registry, std::filesystem::path file, const std::filesystem::path& importFile,
		std::function<void(const std::string& error)> onWritten)
		: m_Registry(registry), m_File(std::move(file)), m_Data(CreateScope<Data>()), m_Version(MakeVersion()),
		  m_Writer(CreateScope<SettingsFileWriter>(std::move(onWritten)))
	{
		if (m_File.empty())
			return;
		if (FileSystem::Exists(m_File))
			Load();
		else
			Import(importFile);
	}

	EditorPreferences::~EditorPreferences()
	{
		m_Writer->Finish();
	}

	void EditorPreferences::Load()
	{
		// The preferences name programs the editor runs (the external editor): only a file the user alone could have written is
		// read.
		std::string readError;
		const std::optional<std::string> text = Platform::ReadTrustedFile(m_File, c_MaxFileBytes, &readError);
		const std::optional<nlohmann::json> json = text ? JsonUtils::Parse(*text) : std::nullopt;
		const nlohmann::json* header = json ? JsonUtils::Find(*json, "Strata") : nullptr;
		const nlohmann::json* format = header ? JsonUtils::Find(*header, "Format") : nullptr;
		const nlohmann::json* version = header ? JsonUtils::Find(*header, "Version") : nullptr;
		const nlohmann::json* settings = json ? JsonUtils::Find(*json, "Settings") : nullptr;
		const bool laterFormat = format && *format == c_FormatName && version && version->is_number_unsigned() && version->get<uint64_t>() > c_FormatVersion;
		const std::string problem = [&]() -> std::string
		{
			if (!text)
				return readError;
			if (!json || !json->is_object())
				return "it is not a JSON object";
			if (!format || *format != c_FormatName || !version || !version->is_number_unsigned())
				return "it is not a preferences file ({ \"Strata\": { \"Format\": \"Preferences\", \"Version\": n }, \"Settings\": ... })";
			if (version->get<uint64_t>() > c_FormatVersion)
				return fmt::format("a later editor wrote it (format version {}; this editor reads {})", version->get<uint64_t>(), c_FormatVersion);
			if (!settings || !settings->is_object())
				return "its Settings are not an object";
			for (const auto& [id, stored] : settings->items())
			{
				const nlohmann::json* storedVersion = JsonUtils::Find(stored, "Version");
				const nlohmann::json* value = JsonUtils::Find(stored, "Value");
				if (!SettingsRegistry::IsValidSectionId(id) || !storedVersion || !storedVersion->is_number_unsigned() || storedVersion->get<uint64_t>() == 0
					|| storedVersion->get<uint64_t>() > std::numeric_limits<uint32_t>::max() || !value)
				{
					return fmt::format("its section '{}' is not {{ \"Version\": <a positive whole number>, \"Value\": ... }}", id);
				}
			}
			return {};
		}();
		if (!problem.empty() && laterFormat)
		{
			// A later editor's preferences: replacing the file would discard them for that editor, so it is left as it is (like
			// 0.5.0's file, which this editor only imports from), and this editor uses the defaults without changing them.
			m_ReadOnlyReason = fmt::format("the preferences file '{}' is left as it is, because {}: preferences cannot be changed in "
				"this editor", FileSystem::ToUTF8(m_File), problem);
			ST_WARN("The preferences '{}' are ignored and left as they are: {}. Their defaults apply, and preferences cannot be changed in "
				"this editor", FileSystem::ToUTF8(m_File), problem);
			return;
		}
		if (!problem.empty())
		{
			ST_WARN("The preferences '{}' are ignored: {}. Changing a preference replaces the file", FileSystem::ToUTF8(m_File), problem);
			return;
		}
		for (const auto& [id, stored] : settings->items())
		{
			Data::Entry& entry = m_Data->Entries[id];
			entry.Version = static_cast<uint32_t>(stored["Version"].get<uint64_t>());
			entry.Value = stored["Value"];
		}
		ValidateStored();
	}

	void EditorPreferences::Import(const std::filesystem::path& importFile)
	{
		if (importFile.empty() || !FileSystem::Exists(importFile))
			return;
		// 0.5.0's file is read as 0.5.0 read it (only a file the user alone could have written) and never written: an editor of
		// that version on the same machine keeps its setting, and its later changes stay its own.
		std::string error;
		const std::optional<std::string> text = Platform::ReadTrustedFile(importFile, c_MaxFileBytes, &error);
		const std::optional<nlohmann::json> json = text ? JsonUtils::Parse(*text) : std::nullopt;
		if (!json)
		{
			ST_WARN("The editor settings '{}' are not imported: {}", FileSystem::ToUTF8(importFile), text ? std::string("they are not JSON") : error);
			return;
		}
		const nlohmann::json* command = JsonUtils::Find(*json, c_ImportedExternalEditorKey);
		if (!command || !command->is_string() || command->get_ref<const std::string&>().empty())
			return;
		const nlohmann::json value = { { "Command", *command } };
		const SettingsSection* section = m_Registry.Find(ExternalEditor::c_SettingsSection);
		if (!section || section->Scope != SettingsScope::User)
			return;
		std::optional<nlohmann::json> validated = section->Validate(value, section->Version, &error);
		if (!validated)
		{
			ST_WARN("The external editor of the editor settings '{}' is not imported: {}", FileSystem::ToUTF8(importFile), error);
			return;
		}
		Data::Entry& entry = m_Data->Entries[section->Id];
		entry.Version = section->Version;
		entry.Value = std::move(*validated);
		entry.Valid = true;
		m_Version = MakeVersion();
		ST_INFO("Imported the external editor of '{}' into the preferences '{}'", FileSystem::ToUTF8(importFile), FileSystem::ToUTF8(m_File));
		// Written at once, so that the import happens once: from now on the preferences file is what the editor reads.
		Save();
	}

	void EditorPreferences::ValidateStored()
	{
		for (auto& [id, entry] : m_Data->Entries)
		{
			const SettingsSection* section = m_Registry.Find(id);
			entry.Valid = false;
			entry.Problem.clear();
			if (!section)
				continue;
			std::optional<nlohmann::json> value;
			if (section->Scope != SettingsScope::User)
				entry.Problem = "it is a project setting, not a preference";
			else
				value = section->Validate(entry.Value, entry.Version, &entry.Problem);
			if (!value)
			{
				ST_WARN("The preference '{}' of '{}' is ignored (its default applies; the stored value is kept until it is changed): {}", id,
					FileSystem::ToUTF8(m_File), entry.Problem);
				continue;
			}
			entry.Value = std::move(*value);
			entry.Version = section->Version;
			entry.Valid = true;
		}
		m_Version = MakeVersion();
	}

	const nlohmann::json* EditorPreferences::FindSection(std::string_view id) const
	{
		const auto it = m_Data->Entries.find(id);
		return it != m_Data->Entries.end() && it->second.Valid ? &it->second.Value : nullptr;
	}

	std::optional<std::string> EditorPreferences::FindProblem(std::string_view id) const
	{
		const auto it = m_Data->Entries.find(id);
		if (it == m_Data->Entries.end() || it->second.Valid || it->second.Problem.empty())
			return std::nullopt;
		return it->second.Problem;
	}

	bool EditorPreferences::IsStored(std::string_view id) const
	{
		return m_Data->Entries.find(id) != m_Data->Entries.end();
	}

	std::vector<std::string> EditorPreferences::GetUnclaimedSections() const
	{
		std::vector<std::string> ids;
		for (const auto& [id, entry] : m_Data->Entries)
		{
			if (!m_Registry.Find(id))
				ids.push_back(id);
		}
		return ids;
	}

	bool EditorPreferences::Set(std::string_view id, const nlohmann::json& value, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};
		const SettingsSection* section = m_Registry.Find(id);
		if (!section)
			return fail(fmt::format("there is no preferences section '{}'", id));
		if (section->Scope != SettingsScope::User)
			return fail(fmt::format("'{}' is a project setting, not a preference", id));
		if (!m_ReadOnlyReason.empty())
			return fail(m_ReadOnlyReason);
		std::string error;
		std::optional<nlohmann::json> validated = section->Validate(value, section->Version, &error);
		if (!validated)
			return fail(std::move(error));

		// Checked on a copy: nothing changes when the file could not hold the change.
		Data changed = *m_Data;
		Data::Entry& entry = changed.Entries[std::string(id)];
		entry.Version = section->Version;
		entry.Value = std::move(*validated);
		entry.Valid = true;
		entry.Problem.clear();
		if (const size_t bytes = JsonUtils::Dump(changed.ToFileJson(), 1, '\t').size() + 1; bytes > c_MaxFileBytes)
			return fail(fmt::format("the preferences file would take {} bytes, more than the {} KiB it may hold", bytes, c_MaxFileBytes / 1024));
		*m_Data = std::move(changed);
		m_Version = MakeVersion();
		Save();
		return true;
	}

	bool EditorPreferences::Reset(std::string_view id)
	{
		const auto it = m_Data->Entries.find(id);
		if (it == m_Data->Entries.end() || !m_ReadOnlyReason.empty())
			return false;
		m_Data->Entries.erase(it);
		m_Version = MakeVersion();
		Save();
		return true;
	}

	void EditorPreferences::Save()
	{
		if (m_File.empty() || !m_ReadOnlyReason.empty())
			return;
		// The snapshot: a copy of the small values, serialized and written on the writer's job.
		m_Writer->Submit([file = m_File, json = m_Data->ToFileJson()](std::string* outError)
		{
			return Platform::WritePrivateFile(file, JsonUtils::Dump(json, 1, '\t') + "\n", outError);
		});
	}

	void EditorPreferences::Update()
	{
		m_Writer->Update();
	}

	void EditorPreferences::Finish()
	{
		m_Writer->Finish();
	}

	SettingsFileState EditorPreferences::GetFileState() const
	{
		return m_Writer->GetState();
	}

	std::string EditorPreferences::GetFileError() const
	{
		return HasUnwrittenPreferences() ? m_Writer->GetLastError() : std::string();
	}

	bool EditorPreferences::HasUnwrittenPreferences() const
	{
		return m_Writer->GetState() == SettingsFileState::Unwritten;
	}

	bool EditorPreferences::IsWriting() const
	{
		return m_Writer->IsBusy();
	}

}
