#pragma once

#include "Strata/Reflection/Property.h"

#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Strata
{

	namespace Detail
	{

		template<typename Member>
		constexpr PropertyType DeducePropertyType(bool color)
		{
			if constexpr (std::is_same_v<Member, bool>)
				return PropertyType::Bool;
			else if constexpr (std::is_same_v<Member, int32_t>)
				return PropertyType::Int;
			else if constexpr (std::is_same_v<Member, uint32_t>)
				return PropertyType::UInt;
			else if constexpr (std::is_same_v<Member, float>)
				return PropertyType::Float;
			else if constexpr (std::is_same_v<Member, glm::vec2>)
				return PropertyType::Vec2;
			else if constexpr (std::is_same_v<Member, glm::vec3>)
				return color ? PropertyType::Color3 : PropertyType::Vec3;
			else if constexpr (std::is_same_v<Member, glm::vec4>)
				return color ? PropertyType::Color4 : PropertyType::Vec4;
			else if constexpr (std::is_same_v<Member, glm::quat>)
				return PropertyType::Quat;
			else if constexpr (std::is_same_v<Member, std::string>)
				return PropertyType::String;
			else
				static_assert(sizeof(Member) == 0, "Unsupported property member type (use EnumProperty, AssetProperty or EntityProperty)");
		}

		void ApplyPropertyOptions(PropertyInfo& property, const std::string& name, PropertyType type, const PropertyOptions& options);

	}

	// Fluent registration of reflected properties of type T into a property list. Derived (CRTP) is the concrete
	// builder returned from each call, so builders can add their own methods (see ComponentInfoBuilder).
	template<typename T, typename Derived>
	class PropertyBuilderBase
	{
	public:
		explicit PropertyBuilderBase(std::vector<PropertyInfo>& properties)
			: m_Properties(properties)
		{
		}

		template<typename Member>
		Derived& Property(std::string name, Member T::* member, const PropertyOptions& options = {})
		{
			PropertyInfo& property = m_Properties.emplace_back();
			Detail::ApplyPropertyOptions(property, name, Detail::DeducePropertyType<Member>(options.Color), options);
			property.Getter = [member](const void* object) -> PropertyValue { return static_cast<const T*>(object)->*member; };
			property.Setter = [member](void* object, const PropertyValue& value) { static_cast<T*>(object)->*member = std::get<Member>(value); };
			return Self();
		}

		template<typename EnumType>
		Derived& EnumProperty(std::string name, EnumType T::* member, std::vector<EnumValue> values, const PropertyOptions& options = {})
		{
			static_assert(std::is_enum_v<EnumType>, "EnumProperty requires an enum member");
			PropertyInfo& property = m_Properties.emplace_back();
			Detail::ApplyPropertyOptions(property, name, PropertyType::Enum, options);
			property.EnumValues = std::move(values);
			property.Getter = [member](const void* object) -> PropertyValue { return static_cast<int32_t>(static_cast<const T*>(object)->*member); };
			property.Setter = [member](void* object, const PropertyValue& value) { static_cast<T*>(object)->*member = static_cast<EnumType>(std::get<int32_t>(value)); };
			return Self();
		}

		Derived& AssetProperty(std::string name, UUID T::* member, AssetType assetType, const PropertyOptions& options = {})
		{
			PropertyInfo& property = m_Properties.emplace_back();
			Detail::ApplyPropertyOptions(property, name, PropertyType::Asset, options);
			property.AssetFilter = assetType;
			property.Getter = [member](const void* object) -> PropertyValue { return static_cast<const T*>(object)->*member; };
			property.Setter = [member](void* object, const PropertyValue& value) { static_cast<T*>(object)->*member = std::get<UUID>(value); };
			return Self();
		}

		Derived& EntityProperty(std::string name, UUID T::* member, const PropertyOptions& options = {})
		{
			PropertyInfo& property = m_Properties.emplace_back();
			Detail::ApplyPropertyOptions(property, name, PropertyType::Entity, options);
			property.Getter = [member](const void* object) -> PropertyValue { return static_cast<const T*>(object)->*member; };
			property.Setter = [member](void* object, const PropertyValue& value) { static_cast<T*>(object)->*member = std::get<UUID>(value); };
			return Self();
		}

		// Property backed by accessor functions instead of a data member.
		Derived& CustomProperty(std::string name, PropertyType type, std::function<PropertyValue(const T&)> getter,
			std::function<void(T&, const PropertyValue&)> setter, const PropertyOptions& options = {})
		{
			PropertyInfo& property = m_Properties.emplace_back();
			Detail::ApplyPropertyOptions(property, name, type, options);
			property.Getter = [getter](const void* object) { return getter(*static_cast<const T*>(object)); };
			property.Setter = [setter](void* object, const PropertyValue& value) { setter(*static_cast<T*>(object), value); };
			return Self();
		}
	private:
		Derived& Self() { return static_cast<Derived&>(*this); }
	private:
		std::vector<PropertyInfo>& m_Properties;
	};

	// Builds a standalone property list for a plain struct (e.g. material parameters).
	template<typename T>
	class PropertyListBuilder : public PropertyBuilderBase<T, PropertyListBuilder<T>>
	{
	public:
		explicit PropertyListBuilder(std::vector<PropertyInfo>& properties)
			: PropertyBuilderBase<T, PropertyListBuilder<T>>(properties)
		{
		}
	};

	// Finds a property by name (case-insensitive) in a property list.
	const PropertyInfo* FindProperty(const std::vector<PropertyInfo>& properties, std::string_view name);

}
