#include "stpch.h"
#include "Strata/Reflection/ComponentRegistry.h"

#include "Strata/Core/StringUtils.h"

#include <atomic>
#include <mutex>

namespace Strata
{

	namespace
	{

		enum class RegistryState : uint8_t
		{
			Closed, // Before BeginRegistration: every use is a programmer error
			Open,   // Components may be registered
			Frozen  // The set is final; reads need no lock
		};

		struct RegistryData
		{
			std::mutex Mutex; // Serializes registrations while the registry is open
			std::atomic<RegistryState> State = RegistryState::Closed;
			std::vector<Scope<ComponentInfo>> Storage;
			std::vector<const ComponentInfo*> All;
			std::unordered_map<entt::id_type, const ComponentInfo*> ByTypeId;
		};

		RegistryData& GetData()
		{
			static RegistryData s_Data;
			return s_Data;
		}

		// Reads are valid once registration began: during it (the registering thread) and after Freeze (any thread).
		const RegistryData& GetReadableData()
		{
			const RegistryData& data = GetData();
			ST_CORE_VERIFY(data.State.load(std::memory_order_acquire) != RegistryState::Closed,
				"The component registry is used before Engine::RegisterBuiltinModules() registered the engine's modules");
			return data;
		}

	}

	const PropertyInfo* ComponentInfo::FindProperty(std::string_view name) const
	{
		return Strata::FindProperty(Properties, name);
	}

	void ComponentRegistry::BeginRegistration()
	{
		RegistryData& data = GetData();
		ST_CORE_VERIFY(data.State.load(std::memory_order_acquire) != RegistryState::Frozen, "The component registry cannot be opened again once it is frozen");
		data.State.store(RegistryState::Open, std::memory_order_release);
	}

	void ComponentRegistry::Freeze()
	{
		RegistryData& data = GetData();
		std::scoped_lock<std::mutex> lock(data.Mutex);
		ST_CORE_VERIFY(data.State.load(std::memory_order_acquire) != RegistryState::Closed, "ComponentRegistry::Freeze() is called before BeginRegistration()");
		data.State.store(RegistryState::Frozen, std::memory_order_release);
	}

	bool ComponentRegistry::IsRegistrationOpen()
	{
		return GetData().State.load(std::memory_order_acquire) == RegistryState::Open;
	}

	bool ComponentRegistry::IsFrozen()
	{
		return GetData().State.load(std::memory_order_acquire) == RegistryState::Frozen;
	}

	ComponentInfo* ComponentRegistry::CreateInfo(std::string name, entt::id_type typeId)
	{
		RegistryData& data = GetData();
		std::scoped_lock<std::mutex> lock(data.Mutex);
		const RegistryState state = data.State.load(std::memory_order_acquire);
		ST_CORE_VERIFY(state != RegistryState::Closed, "Component '{}' is registered before Engine::RegisterBuiltinModules() opened the component registry", name);
		if (state == RegistryState::Frozen)
		{
			ST_CORE_ERROR("Component '{}' is not registered: the component registry is frozen once the engine's modules are registered "
				"(register components through Engine::ModuleRegistrationOptions::Extra)", name);
			return nullptr;
		}
		if (name.empty())
		{
			ST_CORE_ERROR("A component cannot be registered without a name");
			return nullptr;
		}
		if (auto existing = data.ByTypeId.find(typeId); existing != data.ByTypeId.end())
		{
			ST_CORE_ERROR("Component '{}' is not registered: its type is registered already, as '{}'", name, existing->second->Name);
			return nullptr;
		}
		for (const ComponentInfo* info : data.All)
		{
			if (StringUtils::EqualsIgnoreCase(info->Name, name))
			{
				ST_CORE_ERROR("Component '{}' is not registered: the name is taken by component '{}'", name, info->Name);
				return nullptr;
			}
		}

		Scope<ComponentInfo>& info = data.Storage.emplace_back(CreateScope<ComponentInfo>());
		info->DisplayName = Utils::PascalCaseToDisplayName(name);
		info->Name = std::move(name);
		info->TypeId = typeId;
		data.All.push_back(info.get());
		data.ByTypeId.emplace(typeId, info.get());
		return info.get();
	}

	const std::vector<const ComponentInfo*>& ComponentRegistry::GetAll()
	{
		return GetReadableData().All;
	}

	const ComponentInfo* ComponentRegistry::Find(std::string_view name)
	{
		for (const ComponentInfo* info : GetAll())
		{
			if (StringUtils::EqualsIgnoreCase(info->Name, name))
				return info;
		}
		return nullptr;
	}

	const ComponentInfo* ComponentRegistry::FindByTypeId(entt::id_type typeId)
	{
		const RegistryData& data = GetReadableData();
		auto it = data.ByTypeId.find(typeId);
		return it != data.ByTypeId.end() ? it->second : nullptr;
	}

}
