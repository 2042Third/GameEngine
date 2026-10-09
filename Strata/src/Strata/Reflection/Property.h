#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Core/UUID.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Strata
{

	// Reflected property types. Reflection metadata drives serialization, the editor inspector, the automation
	// API, undo/redo and script field access, so every authored component field is described by one of these.
	enum class PropertyType : uint8_t
	{
		Bool,
		Int,     // int32_t
		UInt,    // uint32_t
		Float,
		Vec2,
		Vec3,
		Vec4,
		Quat,    // Rotation; edited as Euler angles in degrees
		Color3,  // Linear RGB stored in glm::vec3
		Color4,  // Linear RGBA stored in glm::vec4
		String,
		Enum,    // int32_t value with named options
		Asset,   // AssetHandle (UUID)
		Entity   // Entity reference by UUID
	};

	const char* PropertyTypeToString(PropertyType type);
	std::optional<PropertyType> PropertyTypeFromString(std::string_view text);

	// Storage for any property value. Color3/Color4 use vec3/vec4, Enum uses int32_t, Asset/Entity use UUID.
	using PropertyValue = std::variant<bool, int32_t, uint32_t, float, glm::vec2, glm::vec3, glm::vec4, glm::quat, std::string, UUID>;

	// Index of the PropertyValue alternative that holds values of the given type.
	size_t GetPropertyValueIndex(PropertyType type);
	PropertyValue GetDefaultPropertyValue(PropertyType type);

	enum class PropertyFlags : uint32_t
	{
		None = 0,
		ReadOnly = ST_BIT(0),  // Visible but not editable through the inspector, automation or scripts
		Hidden = ST_BIT(1),    // Not shown in the inspector (still serialized)
		Transient = ST_BIT(2), // Not serialized (runtime state)
		MultiLine = ST_BIT(3), // String edited in a multi-line text box
		Slider = ST_BIT(4)     // Numeric value edited with a slider over [Min, Max]
	};
	ST_DEFINE_ENUM_FLAG_OPERATORS(PropertyFlags)

	struct EnumValue
	{
		std::string Name;
		int32_t Value = 0;
	};

	struct PropertyInfo
	{
		std::string Name;        // Stable identifier used in files and APIs, e.g. "Translation"
		std::string DisplayName; // Inspector label, e.g. "Translation"
		std::string Tooltip;
		PropertyType Type = PropertyType::Float;
		PropertyFlags Flags = PropertyFlags::None;
		float Min = 0.0f;
		float Max = 0.0f;   // Numeric range is enforced when Max > Min
		float Speed = 0.0f; // Drag speed hint for editors (0 picks a default)
		AssetType AssetFilter = AssetType::None; // Asset properties: the accepted asset type
		std::vector<EnumValue> EnumValues;       // Enum properties: the named options

		std::function<PropertyValue(const void* object)> Getter;
		std::function<void(void* object, const PropertyValue& value)> Setter;

		bool HasRange() const { return Max > Min; }
		bool IsReadOnly() const { return HasFlag(Flags, PropertyFlags::ReadOnly); }
		bool IsHidden() const { return HasFlag(Flags, PropertyFlags::Hidden); }
		bool IsTransient() const { return HasFlag(Flags, PropertyFlags::Transient); }

		PropertyValue GetValue(const void* object) const { return Getter(object); }
		// Validates the value (type, enum membership), clamps numeric ranges and assigns it.
		// Returns false (leaving the object unchanged) if the value is not valid for this property.
		bool SetValue(void* object, const PropertyValue& value, std::string* outError = nullptr) const;

		const EnumValue* FindEnumValue(std::string_view name) const; // Case-insensitive
		const EnumValue* FindEnumValue(int32_t value) const;
	};

	// Options accepted when registering a property.
	struct PropertyOptions
	{
		std::string DisplayName; // Defaults to the name split into words ("HalfExtents" -> "Half Extents")
		std::string Tooltip;
		PropertyFlags Flags = PropertyFlags::None;
		float Min = 0.0f;
		float Max = 0.0f;
		float Speed = 0.0f;
		bool Color = false; // vec3/vec4 members are colors (Color3/Color4)
	};

	namespace Utils
	{
		// "PerspectiveFOV" -> "Perspective FOV", "SSAOEnabled" -> "SSAO Enabled".
		std::string PascalCaseToDisplayName(std::string_view name);
	}

}
