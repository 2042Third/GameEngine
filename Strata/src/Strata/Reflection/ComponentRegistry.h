#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Reflection/Property.h"
#include "Strata/Reflection/PropertyBuilder.h"

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
		NoSerialize = ST_BIT(3),  // Not written as a component (the scene serializer stores it itself, e.g. ID, parent)
		EngineAdded = ST_BIT(4)   // Added only by the engine (e.g. prefab links): visible and removable, never added by users or tools
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
		bool IsAddable() const { return !IsHidden() && !HasFlag(Flags, ComponentFlags::EngineAdded) && !HasFlag(Flags, ComponentFlags::NoSerialize); }
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

	template<typename T>
	class ComponentInfoBuilder : public PropertyBuilderBase<T, ComponentInfoBuilder<T>>
	{
	public:
		explicit ComponentInfoBuilder(ComponentInfo& info)
			: PropertyBuilderBase<T, ComponentInfoBuilder<T>>(info.Properties), m_Info(info)
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
