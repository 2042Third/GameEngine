#include "stpch.h"
#include "Strata/Core/SettingsRegistry.h"

#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Log.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace Strata
{

	namespace
	{

		// The largest whole number a double holds exactly (2^53): Int ranges stay within it.
		constexpr uint64_t c_MaxExactWholeNumber = uint64_t { 1 } << 53;
		constexpr double c_MaxExactInteger = static_cast<double>(c_MaxExactWholeNumber);

		bool IsUpper(char character) { return character >= 'A' && character <= 'Z'; }
		bool IsAlphanumeric(char character)
		{
			return IsUpper(character) || (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
		}

		// A PascalCase identifier: an upper-case ASCII letter, then ASCII letters and digits.
		bool IsPascalIdentifier(std::string_view text)
		{
			return !text.empty() && IsUpper(text.front()) && std::all_of(text.begin(), text.end(), IsAlphanumeric);
		}

		std::string JoinNames(const std::vector<std::string>& names)
		{
			std::string joined;
			for (const std::string& name : names)
				joined += (joined.empty() ? "'" : ", '") + name + "'";
			return joined;
		}

		// What a field accepts, for messages: "<name> must be <this>".
		std::string DescribeExpected(const SettingsField& field)
		{
			switch (field.Type)
			{
				case SettingsFieldType::Bool:   return "true or false";
				case SettingsFieldType::Int:    return fmt::format("a whole number from {} to {}", static_cast<int64_t>(field.Min), static_cast<int64_t>(field.Max));
				case SettingsFieldType::Float:  return fmt::format("a number from {} to {}", field.Min, field.Max);
				case SettingsFieldType::String: return fmt::format("a string of at most {} bytes", field.MaxLength);
				case SettingsFieldType::Enum:   return fmt::format("one of {}", JoinNames(field.Options));
				case SettingsFieldType::Asset:  return fmt::format("the handle of a {} asset (16 hex digits), or null for none", field.AssetType);
			}
			return "valid";
		}

		// The field's value checked and in its stored form (Float as a floating-point number, Asset as the canonical handle or
		// null), or nullopt when the field does not accept it.
		std::optional<nlohmann::json> ValidateField(const SettingsField& field, const nlohmann::json& value)
		{
			switch (field.Type)
			{
				case SettingsFieldType::Bool:
					if (value.is_boolean())
						return value;
					return std::nullopt;
				case SettingsFieldType::Int:
				{
					// Ranges lie within +-2^53 (CheckField), so larger whole numbers are refused before the conversion to double,
					// which would round the next ones onto a bound of that size; the rest convert exactly.
					const bool exact = value.is_number_unsigned() ? value.get<uint64_t>() <= c_MaxExactWholeNumber
						: value.is_number_integer() && value.get<int64_t>() >= -static_cast<int64_t>(c_MaxExactWholeNumber)
							&& value.get<int64_t>() <= static_cast<int64_t>(c_MaxExactWholeNumber);
					if (!exact)
						return std::nullopt;
					const double number = value.get<double>();
					if (number < field.Min || number > field.Max)
						return std::nullopt;
					return value;
				}
				case SettingsFieldType::Float:
				{
					if (!value.is_number())
						return std::nullopt;
					const double number = value.get<double>();
					if (!std::isfinite(number) || number < field.Min || number > field.Max)
						return std::nullopt;
					return nlohmann::json(number);
				}
				case SettingsFieldType::String:
					if (value.is_string() && value.get_ref<const std::string&>().size() <= field.MaxLength)
						return value;
					return std::nullopt;
				case SettingsFieldType::Enum:
				{
					if (!value.is_string())
						return std::nullopt;
					const std::string& text = value.get_ref<const std::string&>();
					if (std::find(field.Options.begin(), field.Options.end(), text) == field.Options.end())
						return std::nullopt;
					return value;
				}
				case SettingsFieldType::Asset:
				{
					if (value.is_null() || (value.is_string() && value.get_ref<const std::string&>().empty()))
						return nlohmann::json(nullptr);
					if (!value.is_string())
						return std::nullopt;
					const std::optional<UUID> handle = UUID::FromString(value.get_ref<const std::string&>());
					if (!handle)
						return std::nullopt;
					return handle->IsValid() ? nlohmann::json(handle->ToString()) : nlohmann::json(nullptr);
				}
			}
			return std::nullopt;
		}

		// Why a field cannot be registered, or empty when it can.
		std::string CheckField(const SettingsField& field)
		{
			if (!IsPascalIdentifier(field.Name))
				return fmt::format("field name '{}' is not a PascalCase identifier", field.Name);
			if (field.Title.empty() || field.Description.empty())
				return fmt::format("field '{}' needs a title and a description", field.Name);
			switch (field.Type)
			{
				case SettingsFieldType::Bool:
					break;
				case SettingsFieldType::Int:
					if (std::floor(field.Min) != field.Min || std::floor(field.Max) != field.Max || field.Min < -c_MaxExactInteger || field.Max > c_MaxExactInteger)
						return fmt::format("field '{}' needs a range of whole numbers within +-2^53", field.Name);
					[[fallthrough]];
				case SettingsFieldType::Float:
					if (!std::isfinite(field.Min) || !std::isfinite(field.Max) || field.Min > field.Max)
						return fmt::format("field '{}' needs a finite range whose Min is not above its Max", field.Name);
					break;
				case SettingsFieldType::String:
					if (field.MaxLength == 0)
						return fmt::format("field '{}' needs a MaxLength", field.Name);
					break;
				case SettingsFieldType::Enum:
				{
					std::unordered_set<std::string> options(field.Options.begin(), field.Options.end());
					if (field.Options.empty() || options.size() != field.Options.size() || options.contains(""))
						return fmt::format("field '{}' needs distinct, non-empty options", field.Name);
					break;
				}
				case SettingsFieldType::Asset:
					if (field.AssetType.empty())
						return fmt::format("field '{}' needs the type of its assets", field.Name);
					break;
			}
			if (!ValidateField(field, field.Default))
				return fmt::format("the default of field '{}' is not {}", field.Name, DescribeExpected(field));
			return {};
		}

		void AddUnique(std::vector<UUID>& handles, std::unordered_set<UUID>& seen, UUID handle)
		{
			if (handle.IsValid() && seen.insert(handle).second)
				handles.push_back(handle);
		}

	}

	const char* SettingsFieldTypeToString(SettingsFieldType type)
	{
		switch (type)
		{
			case SettingsFieldType::Bool:   return "Bool";
			case SettingsFieldType::Int:    return "Int";
			case SettingsFieldType::Float:  return "Float";
			case SettingsFieldType::String: return "String";
			case SettingsFieldType::Enum:   return "Enum";
			case SettingsFieldType::Asset:  return "Asset";
		}
		return "Unknown";
	}

	std::optional<nlohmann::json> SettingsSection::Validate(const nlohmann::json& value, uint32_t version, std::string* outError) const
	{
		auto fail = [outError](std::string message) -> std::optional<nlohmann::json>
		{
			if (outError)
				*outError = std::move(message);
			return std::nullopt;
		};

		if (version == 0)
			return fail("version 0 is no version of the section");
		if (version > Version)
			return fail(fmt::format("the value is of version {} of the section, newer than this engine reads (up to {})", version, Version));
		if (const size_t bytes = JsonUtils::Dump(value).size(); bytes > c_MaxValueBytes)
			return fail(fmt::format("the value takes {} bytes, more than the {} KiB a section may hold", bytes, c_MaxValueBytes / 1024));

		if (!IsSimple())
		{
			if (!Read)
				return fail("the section cannot read values");
			std::string error;
			std::optional<nlohmann::json> result = Read(value, version, &error);
			if (!result)
				return fail(error.empty() ? std::string("the value is invalid") : error);
			if (const size_t bytes = JsonUtils::Dump(*result).size(); bytes > c_MaxValueBytes)
				return fail(fmt::format("the value takes {} bytes, more than the {} KiB a section may hold", bytes, c_MaxValueBytes / 1024));
			return result;
		}

		if (!value.is_object())
			return fail("the value must be an object of the section's fields");
		nlohmann::json result = nlohmann::json::object();
		for (const SettingsField& field : Fields)
		{
			const nlohmann::json* member = JsonUtils::Find(value, field.Name);
			if (!member)
			{
				result[field.Name] = field.Default;
				continue;
			}
			std::optional<nlohmann::json> fieldValue = ValidateField(field, *member);
			if (!fieldValue)
				return fail(fmt::format("'{}' must be {}", field.Name, DescribeExpected(field)));
			result[field.Name] = std::move(*fieldValue);
		}
		// Members that name no field: a typo in this version, a field removed since in older ones (dropped).
		if (version == Version)
		{
			for (const auto& [name, member] : value.items())
			{
				if (!result.contains(name))
				{
					std::vector<std::string> names;
					for (const SettingsField& field : Fields)
						names.push_back(field.Name);
					return fail(fmt::format("'{}' is no field of the section (its fields: {})", name, JoinNames(names)));
				}
			}
		}
		return result;
	}

	nlohmann::json SettingsSection::GetDefault() const
	{
		if (!IsSimple())
			return MakeDefault ? MakeDefault() : nlohmann::json(nullptr);
		nlohmann::json value = nlohmann::json::object();
		for (const SettingsField& field : Fields)
			value[field.Name] = field.Default;
		return value;
	}

	nlohmann::json SettingsSection::GetValueSchema() const
	{
		if (!IsSimple())
			return Schema;
		nlohmann::json properties = nlohmann::json::object();
		for (const SettingsField& field : Fields)
		{
			nlohmann::json property = { { "title", field.Title }, { "description", field.Description }, { "default", field.Default } };
			switch (field.Type)
			{
				case SettingsFieldType::Bool:
					property["type"] = "boolean";
					break;
				case SettingsFieldType::Int:
					property["type"] = "integer";
					property["minimum"] = static_cast<int64_t>(field.Min);
					property["maximum"] = static_cast<int64_t>(field.Max);
					break;
				case SettingsFieldType::Float:
					property["type"] = "number";
					property["minimum"] = field.Min;
					property["maximum"] = field.Max;
					break;
				case SettingsFieldType::String:
					property["type"] = "string";
					property["maxLength"] = field.MaxLength;
					break;
				case SettingsFieldType::Enum:
					property["type"] = "string";
					property["enum"] = field.Options;
					break;
				case SettingsFieldType::Asset:
					property["type"] = nlohmann::json::array({ "string", "null" });
					property["pattern"] = "^[0-9A-Fa-f]{1,16}$";
					property["description"] = fmt::format("{} (the handle of a {} asset, or null for none)", field.Description, field.AssetType);
					break;
			}
			properties[field.Name] = std::move(property);
		}
		return { { "type", "object" }, { "title", Title }, { "description", Description }, { "properties", std::move(properties) },
			{ "additionalProperties", false } };
	}

	std::vector<UUID> SettingsSection::FindReferences(const nlohmann::json& value) const
	{
		std::vector<UUID> handles;
		std::unordered_set<UUID> seen;
		if (IsSimple())
		{
			for (const SettingsField& field : Fields)
			{
				const nlohmann::json* member = field.Type == SettingsFieldType::Asset ? JsonUtils::Find(value, field.Name) : nullptr;
				if (member && member->is_string())
				{
					if (const std::optional<UUID> handle = UUID::FromString(member->get_ref<const std::string&>()))
						AddUnique(handles, seen, *handle);
				}
			}
		}
		else if (References)
		{
			for (UUID handle : References(value))
				AddUnique(handles, seen, handle);
		}
		return handles;
	}

	SettingsRegistry& SettingsRegistry::GetProcessRegistry()
	{
		static SettingsRegistry s_Registry;
		return s_Registry;
	}

	bool SettingsRegistry::IsValidSectionId(std::string_view id)
	{
		const size_t dot = id.find('.');
		if (id.size() > c_MaxIdSize || dot == std::string_view::npos)
			return false;
		return IsPascalIdentifier(id.substr(0, dot)) && IsPascalIdentifier(id.substr(dot + 1));
	}

	bool SettingsRegistry::Register(SettingsSection section)
	{
		auto refuse = [&section](const std::string& reason)
		{
			ST_CORE_ERROR("Settings section '{}' is not registered: {}", section.Id, reason);
			return false;
		};

		if (m_Frozen)
			return refuse("the registry is frozen (sections register while the modules register)");
		if (!IsValidSectionId(section.Id))
			return refuse(fmt::format("its id is not '<Module>.<Name>' (two PascalCase identifiers, at most {} bytes)", c_MaxIdSize));
		if (Find(section.Id))
			return refuse("the id is taken");
		if (section.Title.empty() || section.Category.empty() || section.Description.empty())
			return refuse("a section needs a title, a category and a description");
		if (section.Version == 0)
			return refuse("versions start at 1");

		if (section.IsSimple())
		{
			if (section.Read || section.MakeDefault || section.References || !section.Schema.is_null())
				return refuse("a section with fields is described by them: it gives no Schema, MakeDefault, Read or References");
			std::unordered_set<std::string> names;
			for (SettingsField& field : section.Fields)
			{
				if (std::string problem = CheckField(field); !problem.empty())
					return refuse(problem);
				if (!names.insert(field.Name).second)
					return refuse(fmt::format("two fields are named '{}'", field.Name));
				// Kept in the stored form, which CheckField found it has: a missing member then validates to what a given one does.
				field.Default = *ValidateField(field, field.Default);
			}
		}
		else
		{
			if (!section.Schema.is_object() || !section.MakeDefault || !section.Read)
				return refuse("a section without fields needs a Schema (an object), MakeDefault and Read");
			std::string error;
			if (!section.Validate(section.MakeDefault(), section.Version, &error))
				return refuse(fmt::format("its own Read refuses its default: {}", error));
		}

		m_Sections.push_back(std::move(section));
		return true;
	}

	const SettingsSection* SettingsRegistry::Find(std::string_view id) const
	{
		const auto it = std::find_if(m_Sections.begin(), m_Sections.end(), [id](const SettingsSection& section) { return section.Id == id; });
		return it != m_Sections.end() ? &*it : nullptr;
	}

}
