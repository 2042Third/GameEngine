#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/UUID.h"

// The complete type: SettingsField::Default and SettingsSection::Schema are JSON members.
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Settings sections: the project's settings (committed with the project, carried into exported games) and the user's
// preferences, each a section that the module owning it registers. What a section holds is a JSON value that the section
// validates (SettingsSection::Validate); programs keep the values (ProjectSettingsValues, Strata/Core/ProjectSettings.h)
// and modules read the project's through the engine service IProjectSettings. Docs/DeveloperGuide.md, "Project settings".
namespace Strata
{

	// Where a section's value is kept.
	enum class SettingsScope : uint8_t
	{
		Project, // In the project file, and in the manifest of the games it exports
		User     // In the user's preferences file (the editor's)
	};

	enum class SettingsFieldType : uint8_t
	{
		Bool,
		Int,    // A whole number from Min to Max
		Float,  // A number from Min to Max
		String, // At most MaxLength bytes of UTF-8
		Enum,   // One of Options
		Asset   // An asset handle (16 hex digits) of an asset of AssetType, or null for none
	};

	const char* SettingsFieldTypeToString(SettingsFieldType type);

	// A member of a simple section's value: what validates, describes (JSON Schema) and draws it.
	struct SettingsField
	{
		std::string Name;        // The member's name in the section's value: a PascalCase identifier
		std::string Title;       // What a settings page shows
		std::string Description; // Written for agents: what the value changes and when
		SettingsFieldType Type = SettingsFieldType::Bool;
		double Min = 0.0;        // Int, Float: the inclusive range
		double Max = 0.0;
		uint32_t MaxLength = 0;  // String: the most bytes it may hold
		std::vector<std::string> Options; // Enum
		std::string AssetType;   // Asset: the type's name as AssetTypeToString writes it ("Texture")
		nlohmann::json Default;  // A valid value of the field; Register keeps it in the stored form (Float: 2.0, Asset: 16 digits)
	};

	// A section of settings. Its id is "<Module>.<Name>" ("Scene.CollisionLayers"). A simple section lists its Fields: its
	// value is an object with one member per field, and the registry validates, describes and defaults it from them. A
	// complex section has no fields and gives Schema, MakeDefault and Read instead, and References when its value names
	// assets.
	struct SettingsSection
	{
		std::string Id;
		std::string Title;       // What the settings windows show
		std::string Category;    // The group the windows list the section under
		std::string Description; // Written for agents: what the section changes
		SettingsScope Scope = SettingsScope::Project;
		// Stored with the value. A value of an older version is upgraded by Read (simple sections: fields added since take
		// their defaults, members of removed fields are dropped); a newer version than this is refused.
		uint32_t Version = 1;

		std::vector<SettingsField> Fields;

		// Complex sections only.
		nlohmann::json Schema;                    // JSON Schema of the value, for commands and agents
		std::function<nlohmann::json()> MakeDefault;
		// The value checked and upgraded to Version from the stored `version`; nullopt with the reason in `error` (which may be
		// null) when it is invalid. Never asserts: values come from files and agents.
		std::function<std::optional<nlohmann::json>(const nlohmann::json& value, uint32_t version, std::string* error)> Read;
		// The asset handles a valid value names (the project's references: deletes and pruned exports follow them).
		std::function<std::vector<UUID>(const nlohmann::json& value)> References;

		bool IsSimple() const { return !Fields.empty(); }

		// The single validation of a value stored with `version`: its size (c_MaxValueBytes, compact), its version (1 to
		// Version), then the fields or Read. Returns the value at Version (simple sections: with every field), or nullopt with
		// the reason (for a simple section the first bad field's, in the order of Fields).
		std::optional<nlohmann::json> Validate(const nlohmann::json& value, uint32_t version, std::string* outError = nullptr) const;
		// The value of a section nobody set.
		nlohmann::json GetDefault() const;
		// The JSON Schema of the value: Schema, or one made from the fields.
		nlohmann::json GetValueSchema() const;
		// The valid asset handles a validated value names (a simple section's Asset fields, a complex section's References),
		// each once, in the order named.
		std::vector<UUID> FindReferences(const nlohmann::json& value) const;

		static constexpr size_t c_MaxValueBytes = 512 * 1024;
	};

	// The sections a program knows. The process's registry is filled by the modules' registration functions (and games' and
	// the editor's through ModuleRegistrationOptions::Extra) and frozen by Engine::RegisterBuiltinModules; while it is open,
	// only the registering thread may use it, and once frozen it never changes. Code that validates settings takes the
	// registry to use, so tests build local ones.
	class SettingsRegistry
	{
	public:
		static constexpr size_t c_MaxIdSize = 64;

		static SettingsRegistry& GetProcessRegistry();

		SettingsRegistry() = default;
		SettingsRegistry(const SettingsRegistry&) = delete;
		SettingsRegistry& operator=(const SettingsRegistry&) = delete;

		// Adds a section. Refused, with the reason logged, when the registry is frozen, the id is invalid or taken, or the
		// section is incomplete or contradicts itself (a default its own validation refuses, fields with Read...).
		bool Register(SettingsSection section);
		void Freeze() { m_Frozen = true; }
		bool IsFrozen() const { return m_Frozen; }

		// Null when no section has that id. The pointer stays valid as long as the registry.
		const SettingsSection* Find(std::string_view id) const;
		// In the order of registration.
		const std::deque<SettingsSection>& GetAll() const { return m_Sections; }

		// "<Module>.<Name>": two identifiers of ASCII letters and digits, each starting with an upper-case letter, at most
		// c_MaxIdSize bytes in all.
		static bool IsValidSectionId(std::string_view id);
	private:
		std::deque<SettingsSection> m_Sections; // A deque: Find's pointers survive later registrations
		bool m_Frozen = false;
	};

}
