#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/UUID.h"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class SettingsRegistry;

	// The engine service through which modules read the project's settings sections (Strata/Core/SettingsRegistry.h):
	// provided (EngineServices::ScopedProvider) while a program runs a project - the editor while one is open, GameRuntime
	// from its manifest. Main thread only, like every engine service: a module reads its section there and keeps what it
	// made of it until GetVersion changes; worker threads never read it.
	class IProjectSettings
	{
	public:
		virtual ~IProjectSettings() = default;

		// The section's validated value; null when the project does not set it (or its stored value is invalid): the
		// section's default applies. Valid until GetVersion changes.
		virtual const nlohmann::json* FindSection(std::string_view id) const = 0;
		// Changes whenever a section changes. Unique across the process: two providers never share a version, so a module
		// that keeps a value with its version notices a provider replaced by another.
		virtual uint64_t GetVersion() const = 0;
	};

	// A stored section a registry refused, and why.
	struct SettingsProblem
	{
		std::string Section;
		std::string Message;
	};

	// The settings sections a project sets, as the project file and the game manifest store them:
	// { "<Id>": { "Version": n, "Value": ... } }. Keeps every stored section, also those no registered section claims (a
	// build without the module) and those whose value its section refuses: they are written back as they were read.
	// Only validated values are provided (FindSection). The plain implementation of IProjectSettings every program uses.
	class ProjectSettingsValues final : public IProjectSettings
	{
	public:
		// The bound of the stored form (compact JSON): the project file and the manifest's settings hold at most this.
		static constexpr size_t c_MaxStoredBytes = 1024 * 1024;

		ProjectSettingsValues();
		~ProjectSettingsValues() override;
		ProjectSettingsValues(const ProjectSettingsValues& other);
		ProjectSettingsValues& operator=(const ProjectSettingsValues& other);
		ProjectSettingsValues(ProjectSettingsValues&& other);
		ProjectSettingsValues& operator=(ProjectSettingsValues&& other);

		// Reads the stored form as it is, without validating the values (Validate does): nullopt, with the reason, when it is
		// not an object of entries of that shape with valid section ids, or larger than c_MaxStoredBytes.
		static std::optional<ProjectSettingsValues> FromJson(const nlohmann::json& settings, std::string* outError = nullptr);
		nlohmann::json ToJson() const;

		// Validates every stored section the registry has as a project section (SettingsSection::Validate), upgrading the
		// valid ones to their section's version; those are provided from now on. Returns the stored sections it refused (also
		// a user section stored here), which keep their stored values and are not provided. Sections the registry does not
		// have are kept as they are, not provided and not reported.
		std::vector<SettingsProblem> Validate(const SettingsRegistry& registry);

		// Sets a project section of the registry to `value` (validated at its section's version). False, with nothing changed
		// and the reason in outError, when the registry has no such project section, the value is invalid or the stored form
		// would outgrow c_MaxStoredBytes.
		bool Set(std::string_view id, const nlohmann::json& value, const SettingsRegistry& registry, std::string* outError = nullptr);
		// Removes a stored section (its default applies again); false when it is not stored.
		bool Reset(std::string_view id);
		bool IsStored(std::string_view id) const;
		// The ids of the stored sections, sorted.
		std::vector<std::string> GetStoredSections() const;
		bool IsEmpty() const;

		// The asset handles the validated sections name (SettingsSection::FindReferences), each valid handle once.
		std::vector<UUID> GetReferences(const SettingsRegistry& registry) const;

		const nlohmann::json* FindSection(std::string_view id) const override;
		uint64_t GetVersion() const override;
	private:
		struct Data;
		Scope<Data> m_Data; // Never null
	};

}
