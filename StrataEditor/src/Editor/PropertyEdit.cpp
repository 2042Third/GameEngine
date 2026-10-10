#include "Editor/PropertyEdit.h"

#include "Editor/EditorContext.h"

#include <Strata/Math/Math.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/ComponentAccess.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <vector>

namespace Strata
{

	namespace
	{

		// Euler angles (degrees) closer than this are the same: converting rotations back and forth is not exact.
		constexpr float c_AngleEpsilon = 0.001f;

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
			if (std::holds_alternative<glm::quat>(*other))
			{
				const glm::vec3 firstAngles = Math::QuatToEulerDegrees(std::get<glm::quat>(shared.Value));
				const glm::vec3 ownAngles = Math::QuatToEulerDegrees(std::get<glm::quat>(*other));
				for (glm::length_t axis = 0; axis < 3; axis++)
				{
					shared.MixedComponents[static_cast<size_t>(axis)] = shared.MixedComponents[static_cast<size_t>(axis)]
						|| std::abs(ownAngles[axis] - firstAngles[axis]) > c_AngleEpsilon;
				}
			}
		}
		return shared;
	}

	namespace
	{

		// Sets `value` on every entity in one undo step; with `components`, only those components of vectors and colors
		// (Euler angles of rotations) and each entity's own values for the others.
		bool SetPropertyOnEntities(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& component, const PropertyInfo& property,
			const PropertyValue& value, const std::optional<PropertyComponentMask>& components, std::string* outError)
		{
			auto fail = [outError](std::string message)
			{
				if (outError)
					*outError = std::move(message);
				return false;
			};

			if (entities.empty())
				return fail("No entities to edit");
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
				if (components)
				{
					const std::optional<PropertyValue> current = ComponentAccess::GetProperty(entity, component, property);
					if (!current || current->index() != value.index())
					{
						transaction.Rollback();
						return fail(fmt::format("'{}' has no {} component", entity.GetName(), component.Name));
					}
					// Vectors and colors: the components the edit did not touch keep each entity's own value.
					VisitVector(target, [&](auto& vector)
					{
						using Vector = std::decay_t<decltype(vector)>;
						const Vector& own = std::get<Vector>(*current);
						for (glm::length_t axis = 0; axis < vector.length(); axis++)
						{
							if (!(*components)[static_cast<size_t>(axis)])
								vector[axis] = own[axis];
						}
					});
					// Rotations: the Euler angles (as the inspector edits them) the edit changed replace the entity's own.
					if (std::holds_alternative<glm::quat>(target))
					{
						const glm::vec3 angles = Math::QuatToEulerDegrees(std::get<glm::quat>(value));
						glm::vec3 own = Math::QuatToEulerDegrees(std::get<glm::quat>(*current));
						for (glm::length_t axis = 0; axis < 3; axis++)
						{
							if ((*components)[static_cast<size_t>(axis)])
								own[axis] = angles[axis];
						}
						target = Math::EulerDegreesToQuat(own);
					}
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

	}

	bool SetPropertyWithUndo(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& component, const PropertyInfo& property,
		const PropertyValue& shown, const PropertyValue& value, std::string* outError)
	{
		if (shown.index() != value.index())
		{
			if (outError)
				*outError = fmt::format("Property '{}': the shown value and the new one have different types", property.Name);
			return false;
		}
		// The components that differ between what the widget showed and the new value; equal values are written whole.
		std::optional<PropertyComponentMask> components;
		if (shown != value)
		{
			PropertyComponentMask changed = {};
			bool partial = false;
			PropertyValue edited = value;
			VisitVector(edited, [&](const auto& vector)
			{
				using Vector = std::decay_t<decltype(vector)>;
				const Vector& before = std::get<Vector>(shown);
				for (glm::length_t axis = 0; axis < vector.length(); axis++)
					changed[static_cast<size_t>(axis)] = vector[axis] != before[axis];
				partial = true;
			});
			if (std::holds_alternative<glm::quat>(value))
			{
				const glm::vec3 before = Math::QuatToEulerDegrees(std::get<glm::quat>(shown));
				const glm::vec3 after = Math::QuatToEulerDegrees(std::get<glm::quat>(value));
				for (glm::length_t axis = 0; axis < 3; axis++)
					changed[static_cast<size_t>(axis)] = std::abs(after[axis] - before[axis]) > c_AngleEpsilon;
				partial = true;
			}
			if (partial)
				components = changed;
		}
		return SetPropertyOnEntities(context, entities, component, property, value, components, outError);
	}

	bool SetPropertyComponentsWithUndo(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& component, const PropertyInfo& property,
		const PropertyValue& value, const PropertyComponentMask& components, std::string* outError)
	{
		return SetPropertyOnEntities(context, entities, component, property, value, components, outError);
	}

	bool IsSettableProperty(const PropertyInfo& property)
	{
		return !property.IsReadOnly() && !property.IsTransient();
	}

	bool HasSettableProperties(const ComponentInfo& component)
	{
		return std::any_of(component.Properties.begin(), component.Properties.end(), [](const PropertyInfo& property) { return IsSettableProperty(property); });
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
			if (IsSettableProperty(property))
				values[property.Name] = PropertyValueToJson(property, property.GetValue(instance));
		}
		return values;
	}

	std::vector<std::optional<PropertyValue>> GetDefaultPropertyValues(const ComponentInfo& component)
	{
		entt::registry registry;
		const entt::entity entity = registry.create();
		const void* instance = component.Add(registry, entity);
		std::vector<std::optional<PropertyValue>> values;
		values.reserve(component.Properties.size());
		for (const PropertyInfo& property : component.Properties)
		{
			if (IsSettableProperty(property))
				values.emplace_back(property.GetValue(instance));
			else
				values.emplace_back(std::nullopt);
		}
		return values;
	}

}
