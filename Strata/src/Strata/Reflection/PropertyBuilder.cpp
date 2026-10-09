#include "stpch.h"
#include "Strata/Reflection/PropertyBuilder.h"

#include "Strata/Core/StringUtils.h"

namespace Strata
{

	const PropertyInfo* FindProperty(const std::vector<PropertyInfo>& properties, std::string_view name)
	{
		for (const PropertyInfo& property : properties)
		{
			if (StringUtils::EqualsIgnoreCase(property.Name, name))
				return &property;
		}
		return nullptr;
	}

	namespace Detail
	{

		void ApplyPropertyOptions(PropertyInfo& property, const std::string& name, PropertyType type, const PropertyOptions& options)
		{
			property.Name = name;
			property.DisplayName = options.DisplayName.empty() ? Utils::PascalCaseToDisplayName(name) : options.DisplayName;
			property.Tooltip = options.Tooltip;
			property.Type = type;
			property.Flags = options.Flags;
			property.Min = options.Min;
			property.Max = options.Max;
			property.Speed = options.Speed;
		}

	}

}
