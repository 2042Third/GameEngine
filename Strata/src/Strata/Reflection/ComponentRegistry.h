#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Reflection/Property.h"

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Strata
{

	enum class ComponentFlags : uint32_t
	{
		None = 0,
		Hidden = ST_BIT(0),       // Internal: not shown in the inspector or add-component menus
		NotRemovable = ST_BIT(1), // Present on every entity (e.g. Transform)
		NoCopy = ST_BIT(2),       // Not copied when entering play mode
		NoSerialize = ST_BIT(3)   // Not written as a component (the scene serializer stores it itself, e.g. ID, parent)
	};
	ST_DEFINE_ENUM_FLAG_OPERATORS(ComponentFlags)

	// Reflection metadata and type-erased ECS operations for one component type.
	struct ComponentInfo
	{
		std::string Name;        // Stable identifier used in files and APIs, e.g. "Transform"
		std::string DisplayName; // e.g. "Transform"
		std::string Category;    // Add-component menu grouping, e.g. "Rendering"
		std::string Description;
		ComponentFlags Flags = ComponentFlags::None;
		entt::id_type TypeId = 0;
		std::vector<PropertyInfo> Properties;

		std::function<bool(const entt::registry&, entt::entity)> Has;
		std::function<void*(entt::registry&, entt::entity)> Get;          // nullptr when absent
		std::function<void*(entt::registry&, entt::entity)> Add;          // Adds a default instance if absent
		std::function<void(entt::registry&, entt::entity)> Remove;
		std::function<void(entt::registry&, entt::entity)> MarkModified;  // Emits the registry's on_update signal
		std::function<void(entt::registry& destination, entt::entity destinationEntity, const entt::registry& source, entt::entity sourceEntity)> Copy;

		// Optional serialization of data that is not expressed as properties (e.g. script fields).
		std::function<void(const void* component, nlohmann::json& out)> SerializeExtra;
		std::function<bool(void* component, const nlohmann::json& in, std::string* outError)> DeserializeExtra;

		bool IsHidden() const { return HasFlag(Flags, ComponentFlags::Hidden); }
		bool IsRemovable() const { return !HasFlag(Flags, ComponentFlags::NotRemovable); }
		bool IsCopyable() const { return !HasFlag(Flags, ComponentFlags::NoCopy); }

		const PropertyInfo* FindProperty(std::string_view name) const; // Case-insensitive
	};

	template<typename T>
	class ComponentInfoBuilder;

	// Registry of every reflected component type. Built-in components are registered once, on first use,
	// and the registry is immutable afterwards (safe to read from any thread).
	class ComponentRegistry
	{
	public:
		static const std::vector<const ComponentInfo*>& GetAll();
		static const ComponentInfo* Find(std::string_view name); // Case-insensitive

		template<typename T>
		static const ComponentInfo* Find()
		{
			return FindByTypeId(entt::type_id<T>().hash());
		}

		static const ComponentInfo* FindByTypeId(entt::id_type typeId);

		// Registers a component type. Only valid during built-in registration (see ComponentRegistration.cpp).
		template<typename T>
		static ComponentInfoBuilder<T> Register(std::string name);
	private:
		static ComponentInfo& CreateInfo(std::string name, entt::id_type typeId);
		static void EnsureInitialized();
	};

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

	template<typename T>
	class ComponentInfoBuilder
	{
	public:
		explicit ComponentInfoBuilder(ComponentInfo& info)
			: m_Info(info)
		{
		}

		ComponentInfoBuilder& DisplayName(std::string displayName)
		{
			m_Info.DisplayName = std::move(displayName);
			return *this;
		}

		ComponentInfoBuilder& Category(std::string category)
		{
			m_Info.Category = std::move(category);
			return *this;
		}

		ComponentInfoBuilder& Description(std::string description)
		{
			m_Info.Description = std::move(description);
			return *this;
		}

		ComponentInfoBuilder& Flags(ComponentFlags flags)
		{
			m_Info.Flags = flags;
			return *this;
		}

		template<typename Member>
		ComponentInfoBuilder& Property(std::string name, Member T::* member, const PropertyOptions& options = {})
		{
			PropertyInfo& property = m_Info.Properties.emplace_back();
			Detail::ApplyPropertyOptions(property, name, Detail::DeducePropertyType<Member>(options.Color), options);
			property.Getter = [member](const void* object) -> PropertyValue { return static_cast<const T*>(object)->*member; };
			property.Setter = [member](void* object, const PropertyValue& value) { static_cast<T*>(object)->*member = std::get<Member>(value); };
			return *this;
		}

		template<typename EnumType>
		ComponentInfoBuilder& EnumProperty(std::string name, EnumType T::* member, std::vector<EnumValue> values, const PropertyOptions& options = {})
		{
			static_assert(std::is_enum_v<EnumType>, "EnumProperty requires an enum member");
			PropertyInfo& property = m_Info.Properties.emplace_back();
			Detail::ApplyPropertyOptions(property, name, PropertyType::Enum, options);
			property.EnumValues = std::move(values);
			property.Getter = [member](const void* object) -> PropertyValue { return static_cast<int32_t>(static_cast<const T*>(object)->*member); };
			property.Setter = [member](void* object, const PropertyValue& value) { static_cast<T*>(object)->*member = static_cast<EnumType>(std::get<int32_t>(value)); };
			return *this;
		}

		ComponentInfoBuilder& AssetProperty(std::string name, UUID T::* member, AssetType assetType, const PropertyOptions& options = {})
		{
			PropertyInfo& property = m_Info.Properties.emplace_back();
			Detail::ApplyPropertyOptions(property, name, PropertyType::Asset, options);
			property.AssetFilter = assetType;
			property.Getter = [member](const void* object) -> PropertyValue { return static_cast<const T*>(object)->*member; };
			property.Setter = [member](void* object, const PropertyValue& value) { static_cast<T*>(object)->*member = std::get<UUID>(value); };
			return *this;
		}

		ComponentInfoBuilder& EntityProperty(std::string name, UUID T::* member, const PropertyOptions& options = {})
		{
			PropertyInfo& property = m_Info.Properties.emplace_back();
			Detail::ApplyPropertyOptions(property, name, PropertyType::Entity, options);
			property.Getter = [member](const void* object) -> PropertyValue { return static_cast<const T*>(object)->*member; };
			property.Setter = [member](void* object, const PropertyValue& value) { static_cast<T*>(object)->*member = std::get<UUID>(value); };
			return *this;
		}

		// Property backed by accessor functions instead of a data member.
		ComponentInfoBuilder& CustomProperty(std::string name, PropertyType type, std::function<PropertyValue(const T&)> getter,
			std::function<void(T&, const PropertyValue&)> setter, const PropertyOptions& options = {})
		{
			PropertyInfo& property = m_Info.Properties.emplace_back();
			Detail::ApplyPropertyOptions(property, name, type, options);
			property.Getter = [getter](const void* object) { return getter(*static_cast<const T*>(object)); };
			property.Setter = [setter](void* object, const PropertyValue& value) { setter(*static_cast<T*>(object), value); };
			return *this;
		}

		// Extra (de)serialization for data that properties cannot express.
		ComponentInfoBuilder& Extra(std::function<void(const T&, nlohmann::json&)> serialize, std::function<bool(T&, const nlohmann::json&, std::string*)> deserialize)
		{
			m_Info.SerializeExtra = [serialize](const void* component, nlohmann::json& out) { serialize(*static_cast<const T*>(component), out); };
			m_Info.DeserializeExtra = [deserialize](void* component, const nlohmann::json& in, std::string* outError) { return deserialize(*static_cast<T*>(component), in, outError); };
			return *this;
		}
	private:
		ComponentInfo& m_Info;
	};

	template<typename T>
	ComponentInfoBuilder<T> ComponentRegistry::Register(std::string name)
	{
		ComponentInfo& info = CreateInfo(std::move(name), entt::type_id<T>().hash());

		info.Has = [](const entt::registry& registry, entt::entity entity) { return registry.all_of<T>(entity); };
		if constexpr (std::is_empty_v<T>)
		{
			// Tag components have no storage; a shared instance stands in so callers can treat "non-null" as "present".
			info.Get = [](entt::registry& registry, entt::entity entity) -> void*
			{
				static T s_Instance;
				return registry.all_of<T>(entity) ? &s_Instance : nullptr;
			};
			info.Add = [](entt::registry& registry, entt::entity entity) -> void*
			{
				static T s_Instance;
				if (!registry.all_of<T>(entity))
					registry.emplace<T>(entity);
				return &s_Instance;
			};
			info.MarkModified = [](entt::registry&, entt::entity) {};
		}
		else
		{
			info.Get = [](entt::registry& registry, entt::entity entity) -> void* { return registry.try_get<T>(entity); };
			info.Add = [](entt::registry& registry, entt::entity entity) -> void*
			{
				if (T* existing = registry.try_get<T>(entity))
					return existing;
				return &registry.emplace<T>(entity);
			};
			info.MarkModified = [](entt::registry& registry, entt::entity entity)
			{
				if (registry.all_of<T>(entity))
					registry.patch<T>(entity);
			};
		}
		info.Remove = [](entt::registry& registry, entt::entity entity) { registry.remove<T>(entity); };
		info.Copy = [](entt::registry& destination, entt::entity destinationEntity, const entt::registry& source, entt::entity sourceEntity)
		{
			if constexpr (std::is_empty_v<T>)
			{
				if (source.all_of<T>(sourceEntity))
					destination.emplace_or_replace<T>(destinationEntity);
			}
			else
			{
				if (const T* component = source.try_get<T>(sourceEntity))
					destination.emplace_or_replace<T>(destinationEntity, *component);
			}
		};

		return ComponentInfoBuilder<T>(info);
	}

}
