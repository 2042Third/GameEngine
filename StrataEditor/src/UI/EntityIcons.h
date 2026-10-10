#pragma once

#include <entt/entt.hpp>

namespace Strata
{

	struct ComponentInfo;

	namespace UI
	{

		// The icon of a component type (UI/Icons.h), shown on its inspector section.
		const char* GetComponentIcon(const ComponentInfo& component);
		// The icon of an entity: that of the component that says most about what it is (a camera, a light, a sky, a sound,
		// text, a mesh...), a package for an otherwise empty entity of a prefab instance, a dot for an entity with only a
		// transform.
		const char* GetEntityIcon(const entt::registry& registry, entt::entity entity);

	}

}
