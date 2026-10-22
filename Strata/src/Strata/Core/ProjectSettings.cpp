#include "stpch.h"
#include "Strata/Core/ProjectSettings.h"

#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/SettingsRegistry.h"

#include <atomic>
#include <functional>
#include <limits>
#include <map>
#include <unordered_set>
#include <utility>

namespace Strata
{

	namespace
	{

		// Versions are unique across the process (IProjectSettings::GetVersion).
		std::atomic<uint64_t> s_NextVersion = 1;

		uint64_t MakeVersion()
		{
			return s_NextVersion.fetch_add(1, std::memory_order_relaxed);
		}

	}

	struct ProjectSettingsValues::Data
	{
		struct Entry
		{
			uint32_t Version = 1; // Of the section, as the value was stored
			nlohmann::json Value;
			bool Valid = false;   // Validated by a registry: provided
		};

		std::map<std::string, Entry, std::less<>> Entries;
		uint64_t Version = MakeVersion();

		nlohmann::json ToJson() const
		{
			nlohmann::json settings = nlohmann::json::object();
			for (const auto& [id, entry] : Entries)
				settings[id] = { { "Version", entry.Version }, { "Value", entry.Value } };
			return settings;
		}
	};

	ProjectSettingsValues::ProjectSettingsValues()
		: m_Data(CreateScope<Data>())
	{
	}

	ProjectSettingsValues::~ProjectSettingsValues() = default;

	// A copy is another provider: it gets a version of its own.
	ProjectSettingsValues::ProjectSettingsValues(const ProjectSettingsValues& other)
		: m_Data(CreateScope<Data>(*other.m_Data))
	{
		m_Data->Version = MakeVersion();
	}

	ProjectSettingsValues& ProjectSettingsValues::operator=(const ProjectSettingsValues& other)
	{
		if (this != &other)
		{
			*m_Data = *other.m_Data;
			m_Data->Version = MakeVersion();
		}
		return *this;
	}

	// The source is left empty, with a version of its own.
	ProjectSettingsValues::ProjectSettingsValues(ProjectSettingsValues&& other)
		: m_Data(std::exchange(other.m_Data, CreateScope<Data>()))
	{
	}

	ProjectSettingsValues& ProjectSettingsValues::operator=(ProjectSettingsValues&& other)
	{
		if (this != &other)
			m_Data = std::exchange(other.m_Data, CreateScope<Data>());
		return *this;
	}

	std::optional<ProjectSettingsValues> ProjectSettingsValues::FromJson(const nlohmann::json& settings, std::string* outError)
	{
		auto fail = [outError](std::string message) -> std::optional<ProjectSettingsValues>
		{
			if (outError)
				*outError = std::move(message);
			return std::nullopt;
		};

		if (!settings.is_object())
			return fail("\"Settings\" must be an object of settings sections");
		if (const size_t bytes = JsonUtils::Dump(settings).size(); bytes > c_MaxStoredBytes)
			return fail(fmt::format("the settings take {} bytes, more than the {} MiB they may hold", bytes, c_MaxStoredBytes / (1024 * 1024)));

		ProjectSettingsValues values;
		for (const auto& [id, stored] : settings.items())
		{
			if (!SettingsRegistry::IsValidSectionId(id))
				return fail(fmt::format("'{}' is not a settings section id ('<Module>.<Name>')", id));
			const nlohmann::json* version = JsonUtils::Find(stored, "Version");
			const nlohmann::json* value = JsonUtils::Find(stored, "Value");
			// Parsed numbers are unsigned, numbers made in code signed.
			const bool positive = version && (version->is_number_unsigned() || (version->is_number_integer() && version->get<int64_t>() > 0));
			if (!positive || !value || version->get<uint64_t>() == 0 || version->get<uint64_t>() > std::numeric_limits<uint32_t>::max())
				return fail(fmt::format("settings section '{}' must be {{ \"Version\": <a positive whole number>, \"Value\": ... }}", id));
			Data::Entry& entry = values.m_Data->Entries[id];
			entry.Version = static_cast<uint32_t>(version->get<uint64_t>());
			entry.Value = *value;
		}
		return values;
	}

	nlohmann::json ProjectSettingsValues::ToJson() const
	{
		return m_Data->ToJson();
	}

	std::vector<SettingsProblem> ProjectSettingsValues::Validate(const SettingsRegistry& registry)
	{
		std::vector<SettingsProblem> problems;
		bool changed = false;
		for (auto& [id, entry] : m_Data->Entries)
		{
			const SettingsSection* section = registry.Find(id);
			std::string error;
			std::optional<nlohmann::json> value;
			if (section && section->Scope != SettingsScope::Project)
				error = "it is a preference of the user, not a project setting";
			else if (section)
				value = section->Validate(entry.Value, entry.Version, &error);

			if (!value)
			{
				// Kept as stored: written back unchanged, and its default applies.
				if (section)
					problems.push_back({ id, std::move(error) });
				changed |= entry.Valid;
				entry.Valid = false;
				continue;
			}
			changed |= !entry.Valid || entry.Version != section->Version || *value != entry.Value;
			entry.Value = std::move(*value);
			entry.Version = section->Version;
			entry.Valid = true;
		}
		if (changed)
			m_Data->Version = MakeVersion();
		return problems;
	}

	bool ProjectSettingsValues::Set(std::string_view id, const nlohmann::json& value, const SettingsRegistry& registry, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		const SettingsSection* section = registry.Find(id);
		if (!section)
			return fail(fmt::format("there is no settings section '{}'", id));
		if (section->Scope != SettingsScope::Project)
			return fail(fmt::format("'{}' is a preference of the user, not a project setting", id));
		std::string error;
		std::optional<nlohmann::json> validated = section->Validate(value, section->Version, &error);
		if (!validated)
			return fail(fmt::format("settings section '{}': {}", id, error));

		Data changed = *m_Data;
		Data::Entry& entry = changed.Entries[std::string(id)];
		entry.Version = section->Version;
		entry.Value = std::move(*validated);
		entry.Valid = true;
		if (const size_t bytes = JsonUtils::Dump(changed.ToJson()).size(); bytes > c_MaxStoredBytes)
		{
			return fail(fmt::format("the project's settings would take {} bytes, more than the {} MiB they may hold", bytes,
				c_MaxStoredBytes / (1024 * 1024)));
		}
		changed.Version = MakeVersion();
		*m_Data = std::move(changed);
		return true;
	}

	bool ProjectSettingsValues::Reset(std::string_view id)
	{
		const auto it = m_Data->Entries.find(id);
		if (it == m_Data->Entries.end())
			return false;
		m_Data->Entries.erase(it);
		m_Data->Version = MakeVersion();
		return true;
	}

	bool ProjectSettingsValues::IsStored(std::string_view id) const
	{
		return m_Data->Entries.find(id) != m_Data->Entries.end();
	}

	std::vector<std::string> ProjectSettingsValues::GetStoredSections() const
	{
		std::vector<std::string> ids;
		ids.reserve(m_Data->Entries.size());
		for (const auto& [id, entry] : m_Data->Entries)
			ids.push_back(id);
		return ids;
	}

	bool ProjectSettingsValues::IsEmpty() const
	{
		return m_Data->Entries.empty();
	}

	std::vector<UUID> ProjectSettingsValues::GetReferences(const SettingsRegistry& registry) const
	{
		std::vector<UUID> handles;
		std::unordered_set<UUID> seen;
		for (const auto& [id, entry] : m_Data->Entries)
		{
			const SettingsSection* section = entry.Valid ? registry.Find(id) : nullptr;
			if (!section)
				continue;
			for (UUID handle : section->FindReferences(entry.Value))
			{
				if (handle.IsValid() && seen.insert(handle).second)
					handles.push_back(handle);
			}
		}
		return handles;
	}

	const nlohmann::json* ProjectSettingsValues::FindSection(std::string_view id) const
	{
		const auto it = m_Data->Entries.find(id);
		return it != m_Data->Entries.end() && it->second.Valid ? &it->second.Value : nullptr;
	}

	uint64_t ProjectSettingsValues::GetVersion() const
	{
		return m_Data->Version;
	}

}
