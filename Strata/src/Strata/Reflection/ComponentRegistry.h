#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/UUID.h"
#include "Strata/Reflection/ComponentGizmo.h"
#include "Strata/Reflection/Property.h"
#include "Strata/Reflection/PropertyBuilder.h"

#include <entt/entt.hpp>
#include <nlohmann/json_fwd.hpp>

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

	// Hears of changes of a component type in a registry (ComponentInfo::Observe): caches and filters that depend on which
	// entities have a component, or on its values, without knowing the type.
	class ComponentObserver
	{
	public:
		virtual ~ComponentObserver() = default;
		// The component was added to, changed on (when observed with updates) or is being removed from the entity.
		virtual void OnComponentChanged(entt::registry& registry, entt::entity entity) = 0;
	};

	// Values of one component type kept outside any registry, in one array of that type (EntityPrototype keeps a prefab's
	// components this way: an EnTT storage allocates its elements a page of 1,024 at a time, too much for small prefabs).
	// Exact copies, read by address and copied into registries with ComponentInfo::EmplaceCopy.
	class ComponentValueArray
	{
	public:
		virtual ~ComponentValueArray() = default;
		// Room for `count` values in all: the addresses of values appended within it never change.
		virtual void Reserve(size_t count) = 0;
		// Appends a copy of a component of the type (as ComponentInfo::Get returns it; tag components may pass null) and
		// returns the copy's address (a shared instance for tag components, which carry no data).
		virtual const void* Append(const void* component) = 0;
		// The bytes the values occupy (not what they own on the heap).
		virtual uint64_t GetMemoryUsage() const = 0;
	};

	namespace Detail
	{

		template<typename T>
		class TypedComponentValueArray final : public ComponentValueArray
		{
		public:
			void Reserve(size_t count) override
			{
				if constexpr (!std::is_empty_v<T>)
					m_Values.reserve(count);
			}

			const void* Append([[maybe_unused]] const void* component) override
			{
				if constexpr (std::is_empty_v<T>)
				{
					static T s_Instance;
					return &s_Instance;
				}
				else
				{
					return &m_Values.emplace_back(*static_cast<const T*>(component));
				}
			}

			uint64_t GetMemoryUsage() const override
			{
				if constexpr (std::is_empty_v<T>)
					return 0;
				else
					return m_Values.capacity() * sizeof(T);
			}
		private:
			// Unused for tag components.
			std::vector<std::conditional_t<std::is_empty_v<T>, char, T>> m_Values;
		};

	}

	// Entity and asset references in what a component holds beyond its properties: its extra data (a script's field
	// values, which SerializeExtra writes). Code that works on any component - remapping entity references, linking a
	// prefab's copies to each other, comparing prefab instances with their prefab, finding the assets a document
	// references - reaches them through these and never names the component's type. A reference is addressed by two indices
	// the component chooses (for scripts: the script and its field).
	struct ComponentExtraReferences
	{
		using Visitor = std::function<void(uint32_t slot, uint32_t subslot, PropertyType type, UUID value)>;
		using JsonVisitor = std::function<void(PropertyType type, UUID value)>;
		using Remap = std::function<UUID(UUID)>;

		// Calls the visitor for every entity or asset reference the component holds (PropertyType::Entity or Asset).
		std::function<void(const void* component, const Visitor& visitor)> Visit;
		// Replaces the reference Visit reported at (slot, subslot).
		std::function<void(void* component, uint32_t slot, uint32_t subslot, UUID value)> Set;
		// Whether the extra data of `a`, its entity references mapped through `remap` (instance to prefab), equals that of `b`.
		std::function<bool(const void* a, const void* b, const Remap& remap)> Equals;
		// The references the component's serialized extra data holds (the JSON object SerializeExtra writes into), as Visit
		// reports them, without making a component: the assets of documents that are not loaded.
		std::function<void(const nlohmann::json& extra, const JsonVisitor& visitor)> VisitJson;

		explicit operator bool() const { return Visit != nullptr; }
	};

	// Reflection metadata and type-erased ECS operations for one component type.
	struct ComponentInfo
	{
		std::string Name;        // Stable identifier used in files and APIs, e.g. "Transform"
		std::string DisplayName; // e.g. "Transform"
		std::string Category;    // Add-component menu grouping, e.g. "Rendering"
		std::string Description;
		ComponentFlags Flags = ComponentFlags::None;
		entt::id_type TypeId = 0;
		size_t Size = 0;         // sizeof the component; 0 for tag components, which have no storage per entity
		std::vector<PropertyInfo> Properties;

		std::function<bool(const entt::registry&, entt::entity)> Has;
		std::function<void*(entt::registry&, entt::entity)> Get;          // nullptr when absent
		std::function<void*(entt::registry&, entt::entity)> Add;          // Adds a default instance if absent
		std::function<void(entt::registry&, entt::entity)> Remove;
		std::function<void(entt::registry&, entt::entity)> MarkModified;  // Emits the registry's on_update signal
		std::function<void(entt::registry& destination, entt::entity destinationEntity, const entt::registry& source, entt::entity sourceEntity)> Copy;
		// Whole storages, for copies of a registry whose entities have the same identifiers in both (Scene::Copy). AssureStorage
		// creates the type's storage; CopyStorage then copies every component of the type `source` has to the same entities of
		// `destination`, which lack it there (they need not exist yet: storages do not check). CopyStorage touches only this
		// type's storages, so copies of different types may run on different threads at once once every storage exists.
		std::function<void(entt::registry& registry)> AssureStorage;
		std::function<void(entt::registry& destination, const entt::registry& source)> CopyStorage;
		// Components kept outside registries (ComponentValueArray): a new array of this type, and a copy of one of its values
		// added to an entity (replacing a component it has; tag components need no value).
		std::function<Scope<ComponentValueArray>()> CreateValueArray;
		std::function<void(entt::registry& registry, entt::entity entity, const void* component)> EmplaceCopy;
		// Connects the observer to the registry's signals of this type: additions and removals, and with `updates` the
		// on_update signal too (value changes that are signaled, see Entity::MarkModified). Release the returned connections
		// (entt::connection::release) while the registry exists, at the latest when the observer goes; a registry that is
		// destroyed ends them itself, and releasing them afterwards would touch freed memory.
		std::function<std::vector<entt::connection>(entt::registry& registry, ComponentObserver& observer, bool updates)> Observe;

		// Optional serialization of data that is not expressed as properties (e.g. script fields).
		std::function<void(const void* component, nlohmann::json& out)> SerializeExtra;
		std::function<bool(void* component, const nlohmann::json& in, std::string* outError)> DeserializeExtra;
		// The entity and asset references of that data (empty for components without extra data or without references in it).
		ComponentExtraReferences ExtraReferences;

		// The editor gizmo the component is drawn as (null: none), and with which gizmos (Renderer/SceneGizmos.h).
		ComponentGizmoGroup GizmoGroup = ComponentGizmoGroup::None;
		ComponentGizmoFunction Gizmo = nullptr;

		// Names of the components this one needs on its entity (a UI image needs its UIRect). The paths that add components by
		// name (ComponentAccess::AddComponent: component.add, entity.create, the inspector's Add Component, scripts) add the
		// missing ones first, in the same edit. Removing a required component is allowed: the dependent one then has no effect.
		std::vector<std::string> Requires;

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

		// A component the entity needs for this one to work (see ComponentInfo::Requires), by its registered name.
		ComponentInfoBuilder& Requires(std::string componentName)
		{
			GetTarget().Requires.push_back(std::move(componentName));
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

		// The editor gizmo of the component: `gizmo` fills its shape (see ComponentGizmoFunction), drawn with `group`.
		ComponentInfoBuilder& Gizmo(ComponentGizmoGroup group, ComponentGizmoFunction gizmo)
		{
			ComponentInfo& info = GetTarget();
			info.GizmoGroup = group;
			info.Gizmo = gizmo;
			return *this;
		}

		// The entity and asset references of the extra data (see ComponentExtraReferences): all four functions.
		ComponentInfoBuilder& ExtraReferences(std::function<void(const T&, const ComponentExtraReferences::Visitor&)> visit,
			std::function<void(T&, uint32_t slot, uint32_t subslot, UUID value)> set,
			std::function<bool(const T&, const T&, const ComponentExtraReferences::Remap&)> equals,
			std::function<void(const nlohmann::json&, const ComponentExtraReferences::JsonVisitor&)> visitJson)
		{
			ComponentExtraReferences& references = GetTarget().ExtraReferences;
			references.Visit = [visit](const void* component, const ComponentExtraReferences::Visitor& visitor) { visit(*static_cast<const T*>(component), visitor); };
			references.Set = [set](void* component, uint32_t slot, uint32_t subslot, UUID value) { set(*static_cast<T*>(component), slot, subslot, value); };
			references.Equals = [equals](const void* a, const void* b, const ComponentExtraReferences::Remap& remap)
			{
				return equals(*static_cast<const T*>(a), *static_cast<const T*>(b), remap);
			};
			references.VisitJson = std::move(visitJson);
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
		info.Size = std::is_empty_v<T> ? 0 : sizeof(T);
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
		info.AssureStorage = [](entt::registry& registry) { registry.storage<T>(); };
		info.CopyStorage = [](entt::registry& destination, const entt::registry& source)
		{
			const auto* from = source.storage<T>();
			if (!from || from->empty())
				return;
			auto& to = destination.storage<T>();
			to.reserve(to.size() + from->size());
			if constexpr (std::is_empty_v<T>)
			{
				for (const entt::entity entity : static_cast<const entt::sparse_set&>(*from))
					to.emplace(entity);
			}
			else
			{
				for (const auto [entity, component] : from->each())
					to.emplace(entity, component);
			}
		};
		info.CreateValueArray = []() -> Scope<ComponentValueArray> { return CreateScope<Detail::TypedComponentValueArray<T>>(); };
		info.EmplaceCopy = [](entt::registry& registry, entt::entity entity, [[maybe_unused]] const void* component)
		{
			if constexpr (std::is_empty_v<T>)
				registry.emplace_or_replace<T>(entity);
			else
				registry.emplace_or_replace<T>(entity, *static_cast<const T*>(component));
		};
		info.Observe = [](entt::registry& registry, ComponentObserver& observer, bool updates)
		{
			std::vector<entt::connection> connections;
			connections.emplace_back(registry.on_construct<T>().template connect<&ComponentObserver::OnComponentChanged>(observer));
			connections.emplace_back(registry.on_destroy<T>().template connect<&ComponentObserver::OnComponentChanged>(observer));
			if (updates)
				connections.emplace_back(registry.on_update<T>().template connect<&ComponentObserver::OnComponentChanged>(observer));
			return connections;
		};

		return ComponentInfoBuilder<T>(registered);
	}

}
