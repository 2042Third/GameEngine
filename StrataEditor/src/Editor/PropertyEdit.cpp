#include "Editor/PropertyEdit.h"

#include "Editor/EditorContext.h"

#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/ComponentAccess.h>

#include <glm/glm.hpp>

#include <type_traits>
#include <vector>

namespace Strata
{

	namespace
	{

		// Calls `function` with the vector a value holds (glm::vec2/3/4) and returns true, or returns false for other values.
		template<typename Function>
		bool VisitVector(PropertyValue& value, Function&& function)
		{
			return std::visit([&function](auto& held)
			{
				using Held = std::decay_t<decltype(held)>;
				if constexpr (std::is_same_v<Held, glm::vec2> || std::is_same_v<Held, glm::vec3> || std::is_same_v<Held, glm::vec4>)
				{
					function(held);
					return true;
				}
				else
				{
					return false;
				}
			}, value);
		}

	}

	bool SetPropertyWithUndo(EditorContext& context, Entity entity, const ComponentInfo& component, const PropertyInfo& property, const PropertyValue& value,
		std::string* outError)
	{
		if (!entity)
		{
			if (outError)
				*outError = "The entity does not exist";
			return false;
		}
		const std::string mergeKey = fmt::format("Property/{}/{}/{}", entity.GetUUID().ToString(), component.Name, property.Name);
		SceneEditTransaction transaction(*entity.GetScene(), fmt::format("Edit {} {}", component.DisplayName, property.DisplayName), { entity.GetUUID() }, mergeKey);
		if (!ComponentAccess::SetProperty(entity, component, property, value, outError))
		{
			transaction.Rollback();
			return false;
		}
		context.CommitEdit(transaction);
		return true;
	}

	std::optional<SharedPropertyValue> GetSharedProperty(std::span<const Entity> entities, const ComponentInfo& component, const PropertyInfo& property)
	{
		if (entities.empty())
			return std::nullopt;
		std::optional<PropertyValue> first = ComponentAccess::GetProperty(entities.front(), component, property);
		if (!first)
			return std::nullopt;
		SharedPropertyValue shared;
		shared.Value = *first;
		for (size_t index = 1; index < entities.size(); index++)
		{
			std::optional<PropertyValue> other = ComponentAccess::GetProperty(entities[index], component, property);
			if (!other)
				return std::nullopt;
			if (*other == shared.Value)
				continue;
			shared.Mixed = true;
			VisitVector(*other, [&shared](const auto& vector)
			{
				const auto& firstVector = std::get<std::decay_t<decltype(vector)>>(shared.Value);
				for (glm::length_t axis = 0; axis < vector.length(); axis++)
					shared.MixedComponents[static_cast<size_t>(axis)] = shared.MixedComponents[static_cast<size_t>(axis)] || vector[axis] != firstVector[axis];
			});
		}
		return shared;
	}

	bool SetPropertyWithUndo(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& component, const PropertyInfo& property,
		const PropertyValue& shown, const PropertyValue& value, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		if (entities.empty())
			return fail("No entities to edit");
		if (shown.index() != value.index())
			return fail(fmt::format("Property '{}': the shown value and the new one have different types", property.Name));
		std::vector<UUID> ids;
		for (const Entity entity : entities)
		{
			if (!entity || entity.GetScene() != entities.front().GetScene())
				return fail("The entities do not exist in the scene");
			ids.push_back(entity.GetUUID());
		}

		// The same entities in the same order merge (SceneEditAction compares them); the key names the edit.
		const std::string mergeKey = fmt::format("Property/{}+{}/{}/{}", ids.front().ToString(), ids.size(), component.Name, property.Name);
		const std::string name = entities.size() == 1 ? fmt::format("Edit {} {}", component.DisplayName, property.DisplayName)
			: fmt::format("Edit {} {} of {} Entities", component.DisplayName, property.DisplayName, entities.size());
		SceneEditTransaction transaction(*entities.front().GetScene(), name, ids, mergeKey);
		for (const Entity entity : entities)
		{
			PropertyValue target = value;
			// Vectors and colors: the components the edit did not touch keep each entity's own value.
			if (shown != value)
			{
				const std::optional<PropertyValue> current = ComponentAccess::GetProperty(entity, component, property);
				if (!current)
				{
					transaction.Rollback();
					return fail(fmt::format("'{}' has no {} component", entity.GetName(), component.Name));
				}
				VisitVector(target, [&](auto& vector)
				{
					using Vector = std::decay_t<decltype(vector)>;
					const Vector& before = std::get<Vector>(shown);
					const Vector& own = std::get<Vector>(*current);
					for (glm::length_t axis = 0; axis < vector.length(); axis++)
					{
						if (vector[axis] == before[axis])
							vector[axis] = own[axis];
					}
				});
			}
			std::string error;
			if (!ComponentAccess::SetProperty(entity, component, property, target, &error))
			{
				transaction.Rollback();
				return fail(error);
			}
		}
		context.CommitEdit(transaction);
		return true;
	}

	nlohmann::json GetDefaultComponentValues(const ComponentInfo& component)
	{
		// A component made on a registry of its own: nothing listens to it there.
		entt::registry registry;
		const entt::entity entity = registry.create();
		const void* instance = component.Add(registry, entity);
		nlohmann::json values = nlohmann::json::object();
		for (const PropertyInfo& property : component.Properties)
		{
			if (!property.IsReadOnly() && !property.IsTransient())
				values[property.Name] = PropertyValueToJson(property, property.GetValue(instance));
		}
		return values;
	}

}
