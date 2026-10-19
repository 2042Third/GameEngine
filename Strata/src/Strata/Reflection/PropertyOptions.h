#pragma once

#include "Strata/Reflection/Property.h"

#include <string>

namespace Strata
{

	// Shorthands for the options of component properties (ComponentInfoBuilder::Property), shared by every module's
	// registrations, so that ranges, sliders, colors and tooltips read the same everywhere.

	// The bound of a range that is not meant to limit a value (a light's intensity), only to keep it finite.
	constexpr float c_Unbounded = 1.0e6f;

	// A numeric range [min, max] (enforced when max > min) and the drag speed of editors (0: their default).
	inline PropertyOptions Range(float min, float max, float speed = 0.0f)
	{
		PropertyOptions options;
		options.Min = min;
		options.Max = max;
		options.Speed = speed;
		return options;
	}

	// A range edited with a slider.
	inline PropertyOptions Slider(float min, float max)
	{
		PropertyOptions options = Range(min, max);
		options.Flags = PropertyFlags::Slider;
		return options;
	}

	// A vec3 or vec4 that is a color (Color3, Color4).
	inline PropertyOptions Color()
	{
		PropertyOptions options;
		options.Color = true;
		return options;
	}

	// The options with a tooltip for people and agents (component.list shows it).
	inline PropertyOptions WithTooltip(PropertyOptions options, std::string tooltip)
	{
		options.Tooltip = std::move(tooltip);
		return options;
	}

}
