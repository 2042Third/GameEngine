#pragma once

#include <Strata/Reflection/Property.h>

#include <nlohmann/json_fwd.hpp>

namespace Strata
{

	struct SettingsField;

	// A simple settings section's fields as the property widgets edit them (DrawPropertyWidget), for the settings pages that
	// draw fields: the generic page and the Graphics Quality page.
	namespace UI
	{

		// The property a field is drawn as: its name, title, description, type and range (an Int's cut to 32 bits, the
		// widgets' numbers: the section still validates the value), a Float's drag speed, an Enum's options in order.
		PropertyInfo MakeFieldProperty(const SettingsField& field);
		// A field's value of a validated section value as its widget edits it (the type's default for a value it cannot read).
		PropertyValue FieldToPropertyValue(const SettingsField& field, const PropertyInfo& property, const nlohmann::json& value);
		// The widget's value in the field's stored form (an Asset: its handle, or null for none).
		nlohmann::json PropertyToFieldValue(const SettingsField& field, const PropertyInfo& property, const PropertyValue& value);

	}

}
