#pragma once

#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Scene/Entity.h>

#include <entt/entt.hpp>

#include <functional>
#include <span>

namespace Strata
{

	class EditorContext;

	namespace UI
	{

		// Draws what a component's generic property rows cannot show (the UIRect's anchor presets and margins): called by the
		// Inspector once per drawn component, above its properties, for the selected entities that all have it.
		using ComponentDrawer = std::function<void(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& info)>;

		// The drawers by component type (ComponentInfo::TypeId): the editor's own (the UIRect's) and those registered later.
		class ComponentDrawers
		{
		public:
			static void Register(entt::id_type type, ComponentDrawer drawer);
			// Draws the component's drawer, if it has one. Returns whether it had.
			static bool Draw(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& info);
		};

		// The UIRect's drawer: notices (outside a canvas, a nested canvas), the anchor preset grid with its Pivot and Position
		// toggles, the margins along stretched axes and the rect in pixels at the game's screen.
		void DrawUIRect(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& info);

	}

}
