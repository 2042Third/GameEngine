#include "Reflection/ComponentSchema.h"

#include "Strata/Reflection/ComponentRegistry.h"

namespace Strata::Tests
{

	nlohmann::json DescribeComponentSchema()
	{
		nlohmann::json components = nlohmann::json::array();
		for (const ComponentInfo* info : ComponentRegistry::GetAll())
		{
			nlohmann::json properties = nlohmann::json::array();
			for (const PropertyInfo& property : info->Properties)
			{
				nlohmann::json enumValues = nlohmann::json::array();
				for (const EnumValue& value : property.EnumValues)
					enumValues.push_back({ { "Name", value.Name }, { "Value", value.Value }, { "DisplayName", value.DisplayName } });
				nlohmann::json described = {
					{ "Name", property.Name },
					{ "DisplayName", property.DisplayName },
					{ "Tooltip", property.Tooltip },
					{ "Type", PropertyTypeToString(property.Type) },
					{ "Flags", static_cast<uint32_t>(property.Flags) },
					{ "Min", property.Min },
					{ "Max", property.Max },
					{ "Speed", property.Speed },
					{ "AssetFilter", static_cast<uint32_t>(property.AssetFilter) },
					{ "EnumValues", enumValues } };
				// Only where a component groups its properties, so the snapshot of the others stays as it was.
				if (!property.Section.empty())
					described["Section"] = property.Section;
				properties.push_back(std::move(described));
			}
			components.push_back({
				{ "Name", info->Name },
				{ "DisplayName", info->DisplayName },
				{ "Category", info->Category },
				{ "Description", info->Description },
				{ "Flags", static_cast<uint32_t>(info->Flags) },
				{ "Extra", static_cast<bool>(info->SerializeExtra) },
				{ "Properties", properties } });
		}
		return nlohmann::json { { "Components", components } };
	}

}
