#include "UI/SettingsFieldProperty.h"

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Core/SettingsRegistry.h>
#include <Strata/Reflection/PropertyJson.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace Strata::UI
{

	PropertyInfo MakeFieldProperty(const SettingsField& field)
	{
		PropertyInfo property;
		property.Name = field.Name;
		property.DisplayName = field.Title;
		property.Tooltip = field.Description;
		switch (field.Type)
		{
			case SettingsFieldType::Bool:
				property.Type = PropertyType::Bool;
				break;
			case SettingsFieldType::Int:
				property.Type = PropertyType::Int;
				// The widget edits 32-bit numbers: a wider range is cut to theirs (the section still validates the value).
				property.Min = static_cast<float>(std::max(field.Min, static_cast<double>(std::numeric_limits<int32_t>::min())));
				property.Max = static_cast<float>(std::min(field.Max, static_cast<double>(std::numeric_limits<int32_t>::max())));
				break;
			case SettingsFieldType::Float:
				property.Type = PropertyType::Float;
				property.Min = static_cast<float>(field.Min);
				property.Max = static_cast<float>(field.Max);
				property.Speed = static_cast<float>(field.DragSpeed);
				break;
			case SettingsFieldType::String:
				property.Type = PropertyType::String;
				break;
			case SettingsFieldType::Enum:
				property.Type = PropertyType::Enum;
				for (size_t index = 0; index < field.Options.size(); index++)
					property.EnumValues.push_back({ field.Options[index], static_cast<int32_t>(index), {} });
				break;
			case SettingsFieldType::Asset:
				property.Type = PropertyType::Asset;
				property.AssetFilter = AssetTypeFromString(field.AssetType).value_or(AssetType::None);
				break;
		}
		return property;
	}

	PropertyValue FieldToPropertyValue(const SettingsField& field, const PropertyInfo& property, const nlohmann::json& value)
	{
		if (field.Type == SettingsFieldType::Asset && value.is_null())
			return UUID::Null();
		if (field.Type == SettingsFieldType::Int && value.is_number())
		{
			const double number = value.get<double>();
			return static_cast<int32_t>(std::clamp(number, static_cast<double>(std::numeric_limits<int32_t>::min()),
				static_cast<double>(std::numeric_limits<int32_t>::max())));
		}
		if (std::optional<PropertyValue> parsed = PropertyValueFromJson(property, value))
			return std::move(*parsed);
		return GetDefaultPropertyValue(property.Type);
	}

	nlohmann::json PropertyToFieldValue(const SettingsField& field, const PropertyInfo& property, const PropertyValue& value)
	{
		if (field.Type == SettingsFieldType::Asset)
		{
			const UUID handle = std::get<UUID>(value);
			return handle.IsValid() ? UUIDToJson(handle) : nlohmann::json(nullptr);
		}
		return PropertyValueToJson(property, value);
	}

}
