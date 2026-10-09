#include "stpch.h"
#include "Strata/Scene/ComponentAccess.h"

#include "Strata/Scene/Entity.h"

namespace Strata
{

	nlohmann::json ComponentAccess::Serialize(const ComponentInfo& info, const void* component)
	{
		nlohmann::json json = nlohmann::json::object();
		if (!component)
			return json;

		for (const PropertyInfo& property : info.Properties)
		{
			if (property.IsTransient())
				continue;
			json[property.Name] = PropertyValueToJson(property, property.GetValue(component));
		}

		if (info.SerializeExtra)
			info.SerializeExtra(component, json);
		return json;
	}

	bool ComponentAccess::Deserialize(const ComponentInfo& info, void* component, const nlohmann::json& json, bool strict, std::string* outError, std::vector<std::string>* outWarnings)
	{
		if (!json.is_object())
		{
			if (outError)
				*outError = fmt::format("Component '{}' data must be a JSON object", info.Name);
			return false;
		}
		if (!component)
			return true; // Tag component: presence is all there is

		for (const auto& [key, value] : json.items())
		{
			const PropertyInfo* property = info.FindProperty(key);
			if (!property)
			{
				// Keys consumed by DeserializeExtra are not properties.
				if (info.DeserializeExtra)
					continue;

				const std::string message = fmt::format("Component '{}' has no property '{}'", info.Name, key);
				if (strict)
				{
					if (outError)
						*outError = message;
					return false;
				}
				if (outWarnings)
					outWarnings->push_back(message);
				continue;
			}

			if (property->IsTransient())
				continue;

			std::string error;
			std::optional<PropertyValue> parsed = PropertyValueFromJson(*property, value, &error);
			if (parsed && property->SetValue(component, *parsed, &error))
				continue;

			if (strict)
			{
				if (outError)
					*outError = fmt::format("{}: {}", info.Name, error);
				return false;
			}
			if (outWarnings)
				outWarnings->push_back(fmt::format("{}: {}", info.Name, error));
		}

		if (info.DeserializeExtra)
		{
			std::string error;
			if (!info.DeserializeExtra(component, json, &error))
			{
				if (strict)
				{
					if (outError)
						*outError = fmt::format("{}: {}", info.Name, error);
					return false;
				}
				if (outWarnings)
					outWarnings->push_back(fmt::format("{}: {}", info.Name, error));
			}
		}
		return true;
	}

	std::optional<PropertyValue> ComponentAccess::GetProperty(Entity entity, const ComponentInfo& info, const PropertyInfo& property)
	{
		if (!entity.IsValid())
			return std::nullopt;

		void* component = info.Get(entity.GetScene()->GetRegistry(), entity.GetHandle());
		if (!component)
			return std::nullopt;
		return property.GetValue(component);
	}

	bool ComponentAccess::SetProperty(Entity entity, const ComponentInfo& info, const PropertyInfo& property, const PropertyValue& value, std::string* outError)
	{
		if (!entity.IsValid())
		{
			if (outError)
				*outError = "Invalid entity";
			return false;
		}
		if (property.IsReadOnly())
		{
			if (outError)
				*outError = fmt::format("Property '{}.{}' is read-only", info.Name, property.Name);
			return false;
		}

		entt::registry& registry = entity.GetScene()->GetRegistry();
		void* component = info.Get(registry, entity.GetHandle());
		if (!component)
		{
			if (outError)
				*outError = fmt::format("Entity '{}' has no {} component", entity.GetName(), info.Name);
			return false;
		}

		if (!property.SetValue(component, value, outError))
			return false;

		info.MarkModified(registry, entity.GetHandle());
		return true;
	}

	bool ComponentAccess::AddComponent(Entity entity, const ComponentInfo& info, std::string* outError)
	{
		if (!entity.IsValid())
		{
			if (outError)
				*outError = "Invalid entity";
			return false;
		}
		if (info.IsHidden())
		{
			if (outError)
				*outError = fmt::format("Component '{}' is internal and cannot be added directly", info.Name);
			return false;
		}

		info.Add(entity.GetScene()->GetRegistry(), entity.GetHandle());
		return true;
	}

	bool ComponentAccess::RemoveComponent(Entity entity, const ComponentInfo& info, std::string* outError)
	{
		if (!entity.IsValid())
		{
			if (outError)
				*outError = "Invalid entity";
			return false;
		}
		if (!info.IsRemovable() || info.IsHidden())
		{
			if (outError)
				*outError = fmt::format("Component '{}' cannot be removed", info.Name);
			return false;
		}

		entt::registry& registry = entity.GetScene()->GetRegistry();
		if (!info.Has(registry, entity.GetHandle()))
		{
			if (outError)
				*outError = fmt::format("Entity '{}' has no {} component", entity.GetName(), info.Name);
			return false;
		}

		info.Remove(registry, entity.GetHandle());
		return true;
	}

	nlohmann::json ComponentAccess::SerializeEntityComponents(Entity entity)
	{
		nlohmann::json components = nlohmann::json::object();
		if (!entity.IsValid())
			return components;

		entt::registry& registry = entity.GetScene()->GetRegistry();
		for (const ComponentInfo* info : ComponentRegistry::GetAll())
		{
			if (HasFlag(info->Flags, ComponentFlags::NoSerialize))
				continue;
			if (!info->Has(registry, entity.GetHandle()))
				continue;
			components[info->Name] = Serialize(*info, info->Get(registry, entity.GetHandle()));
		}
		return components;
	}

}
