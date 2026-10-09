#include "stpch.h"
#include "Strata/Reflection/ComponentRegistry.h"

#include <cctype>
#include <mutex>

namespace Strata
{

	// Defined in Scene/ComponentRegistration.cpp: registers every built-in component.
	void RegisterBuiltinComponents();

	namespace
	{

		struct RegistryData
		{
			std::vector<Scope<ComponentInfo>> Storage;
			std::vector<const ComponentInfo*> All;
			std::unordered_map<entt::id_type, const ComponentInfo*> ByTypeId;
			bool Registering = false;
		};

		RegistryData& GetData()
		{
			static RegistryData s_Data;
			return s_Data;
		}

		bool EqualsIgnoreCase(std::string_view a, std::string_view b)
		{
			if (a.size() != b.size())
				return false;
			for (size_t index = 0; index < a.size(); index++)
			{
				if (std::tolower(static_cast<unsigned char>(a[index])) != std::tolower(static_cast<unsigned char>(b[index])))
					return false;
			}
			return true;
		}

	}

	const PropertyInfo* ComponentInfo::FindProperty(std::string_view name) const
	{
		for (const PropertyInfo& property : Properties)
		{
			if (EqualsIgnoreCase(property.Name, name))
				return &property;
		}
		return nullptr;
	}

	void ComponentRegistry::EnsureInitialized()
	{
		static std::once_flag s_Once;
		std::call_once(s_Once, []()
		{
			RegistryData& data = GetData();
			data.Registering = true;
			RegisterBuiltinComponents();
			data.Registering = false;
		});
	}

	ComponentInfo& ComponentRegistry::CreateInfo(std::string name, entt::id_type typeId)
	{
		RegistryData& data = GetData();
		ST_CORE_VERIFY(data.Registering, "Components can only be registered from RegisterBuiltinComponents()");
		ST_CORE_VERIFY(data.ByTypeId.find(typeId) == data.ByTypeId.end(), "Component '{}' registered twice", name);

		Scope<ComponentInfo>& info = data.Storage.emplace_back(CreateScope<ComponentInfo>());
		info->DisplayName = Utils::PascalCaseToDisplayName(name);
		info->Name = std::move(name);
		info->TypeId = typeId;
		data.All.push_back(info.get());
		data.ByTypeId.emplace(typeId, info.get());
		return *info;
	}

	const std::vector<const ComponentInfo*>& ComponentRegistry::GetAll()
	{
		EnsureInitialized();
		return GetData().All;
	}

	const ComponentInfo* ComponentRegistry::Find(std::string_view name)
	{
		for (const ComponentInfo* info : GetAll())
		{
			if (EqualsIgnoreCase(info->Name, name))
				return info;
		}
		return nullptr;
	}

	const ComponentInfo* ComponentRegistry::FindByTypeId(entt::id_type typeId)
	{
		EnsureInitialized();
		const RegistryData& data = GetData();
		auto it = data.ByTypeId.find(typeId);
		return it != data.ByTypeId.end() ? it->second : nullptr;
	}

	namespace Detail
	{

		void ApplyPropertyOptions(PropertyInfo& property, const std::string& name, PropertyType type, const PropertyOptions& options)
		{
			property.Name = name;
			property.DisplayName = options.DisplayName.empty() ? Utils::PascalCaseToDisplayName(name) : options.DisplayName;
			property.Tooltip = options.Tooltip;
			property.Type = type;
			property.Flags = options.Flags;
			property.Min = options.Min;
			property.Max = options.Max;
			property.Speed = options.Speed;
		}

	}

}
