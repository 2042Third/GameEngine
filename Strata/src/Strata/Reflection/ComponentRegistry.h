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

	// Registry of every reflected component type.
	//
	// Lifecycle (driven by Engine::RegisterBuiltinModules at startup): BeginRegistration opens the registry, the engine's
	// modules - and games and tests, through ModuleRegistrationOptions::Extra - register their components, and Freeze fixes
	// the set. Using the registry before BeginRegistration fails ST_CORE_VERIFY. While it is open, only the registering
	// thread may use it; once frozen it never changes, so reads need no lock and are safe from any thread.
	class ComponentRegistry
	{
	public:
		static void BeginRegistration();
		static void Freeze();
		static bool IsRegistrationOpen();
		static bool IsFrozen();

		static const std::vector<const ComponentInfo*>& GetAll();
		static const ComponentInfo* Find(std::string_view name); // Case-insensitive

		template<typename T>
		static const ComponentInfo* Find()
		{
			return FindByTypeId(entt::type_id<T>().hash());
		}

		static const ComponentInfo* FindByTypeId(entt::id_type typeId);

		// Registers a component type under a stable name (used in files and APIs) while registration is open. A refused
		// registration - the registry is frozen, the type is registered already or the name is taken (ignoring case) - logs
		// an error, and the returned builder converts to false and discards what it is given.
		template<typename T>
		static ComponentInfoBuilder<T> Register(std::string name);
	private:
		// Null (with the reason logged) when the registration is refused.
		static ComponentInfo* CreateInfo(std::string name, entt::id_type typeId);
	};

	namespace Detail
	{

		// What a component builder writes into: the registered info, or a scratch info that is discarded when the registry
		// refused the registration (so the builder chain that follows a refused Register runs without effect). A separate
		// base class, so that it is constructed before PropertyBuilderBase, which keeps a reference into the info.
		class ComponentInfoTarget
		{
		protected:
			explicit ComponentInfoTarget(ComponentInfo* registered)
				: m_Registered(registered), m_Scratch(registered ? nullptr : CreateScope<ComponentInfo>())
			{
			}

			ComponentInfo& GetTarget() { return m_Registered ? *m_Registered : *m_Scratch; }

			ComponentInfo* m_Registered;
			Scope<ComponentInfo> m_Scratch;
		};

	}

	template<typename T>
	class ComponentInfoBuilder : private Detail::ComponentInfoTarget, public PropertyBuilderBase<T, ComponentInfoBuilder<T>>
	{
	public:
		// `info` is the registered info, or null when the registration was refused.
		explicit ComponentInfoBuilder(ComponentInfo* info)
			: Detail::ComponentInfoTarget(info), PropertyBuilderBase<T, ComponentInfoBuilder<T>>(GetTarget().Properties)
		{
		}

		// False when the registry refused the registration (see ComponentRegistry::Register).
		bool IsRegistered() const { return m_Registered != nullptr; }
		explicit operator bool() const { return IsRegistered(); }

		ComponentInfoBuilder& DisplayName(std::string displayName)
		{
			GetTarget().DisplayName = std::move(displayName);
			return *this;
		}

		ComponentInfoBuilder& Category(std::string category)
		{
			GetTarget().Category = std::move(category);
			return *this;
		}

		ComponentInfoBuilder& Description(std::string description)
		{
			GetTarget().Description = std::move(description);
			return *this;
		}

		ComponentInfoBuilder& Flags(ComponentFlags flags)
		{
			GetTarget().Flags = flags;
			return *this;
		}

		// Extra (de)serialization for data that properties cannot express.
		ComponentInfoBuilder& Extra(std::function<void(const T&, nlohmann::json&)> serialize, std::function<bool(T&, const nlohmann::json&, std::string*)> deserialize)
		{
			ComponentInfo& info = GetTarget();
			info.SerializeExtra = [serialize](const void* component, nlohmann::json& out) { serialize(*static_cast<const T*>(component), out); };
			info.DeserializeExtra = [deserialize](void* component, const nlohmann::json& in, std::string* outError) { return deserialize(*static_cast<T*>(component), in, outError); };
			return *this;
		}
	};

	template<typename T>
	ComponentInfoBuilder<T> ComponentRegistry::Register(std::string name)
	{
		ComponentInfo* registered = CreateInfo(std::move(name), entt::type_id<T>().hash());
		if (!registered)
			return ComponentInfoBuilder<T>(nullptr);

		ComponentInfo& info = *registered;
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

		return ComponentInfoBuilder<T>(registered);
	}

}
