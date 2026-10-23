#include <doctest/doctest.h>

#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Renderer/Material.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;

namespace
{

	bool NamesUnit(const std::string& text, std::string_view unit)
	{
		return text.find(unit) != std::string::npos;
	}

	struct LightUnit
	{
		const char* Component = nullptr;
		const char* Unit = nullptr;
	};

}

TEST_SUITE("Renderer.LightUnits")
{
	TEST_CASE("Every light's and the sky's description and intensity tooltip name the unit")
	{
		// The shading computes physical quantities on one scale (DeveloperGuide, "Light units and exposure"): the descriptions
		// agents read and the Inspector's tooltips say which.
		constexpr LightUnit c_Units[] = {
			{ "DirectionalLight", "lux" },
			{ "PointLight", "candela" },
			{ "SpotLight", "candela" },
			{ "SkyLight", "nits" },
		};
		for (const LightUnit& unit : c_Units)
		{
			const std::string component = unit.Component;
			CAPTURE(component);
			const ComponentInfo* info = ComponentRegistry::Find(component);
			REQUIRE(info);
			CHECK(NamesUnit(info->Description, unit.Unit));
			const PropertyInfo* intensity = info->FindProperty("Intensity");
			REQUIRE(intensity);
			CHECK(NamesUnit(intensity->Tooltip, unit.Unit));
		}

		// The sky's other luminances: the ambient color, the procedural colors and the sun's disk.
		const ComponentInfo* sky = ComponentRegistry::Find("SkyLight");
		REQUIRE(sky);
		for (const char* name : { "AmbientColor", "ZenithColor", "HorizonColor", "GroundColor", "SunIntensity" })
		{
			const std::string propertyName = name; // A captured const char* prints as a bool
			CAPTURE(propertyName);
			const PropertyInfo* property = sky->FindProperty(name);
			REQUIRE(property);
			CHECK(NamesUnit(property->Tooltip, "nits"));
		}
	}

	TEST_CASE("The material's emissive properties name their unit")
	{
		const std::vector<PropertyInfo>& properties = Material::GetPropertyInfos();
		for (const char* name : { "EmissiveColor", "EmissiveIntensity" })
		{
			const std::string propertyName = name; // A captured const char* prints as a bool
			CAPTURE(propertyName);
			const auto found = std::find_if(properties.begin(), properties.end(), [name](const PropertyInfo& property) { return property.Name == name; });
			REQUIRE(found != properties.end());
			CHECK(NamesUnit(found->Tooltip, "nits"));
		}
	}
}
