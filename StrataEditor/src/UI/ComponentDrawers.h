#pragma once

#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Scene/Entity.h>

#include <entt/entt.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace Strata
{

	class EditorContext;

	namespace UI
	{

		// What a drawer drew: the properties it shows in its own way, which the generic rows then leave out.
		struct ComponentDrawResult
		{
			static constexpr uint32_t c_MaxReplaced = 8;
			std::array<std::string_view, c_MaxReplaced> Replaced = {}; // Property names (static strings)
			uint32_t ReplacedCount = 0;

			void Replace(std::string_view property);
			bool Replaces(std::string_view property) const;
		};

		// Draws what a component's generic property rows cannot show (the UIRect's anchor presets and margins): called by the
		// Inspector once per drawn component, above its properties, for the selected entities that all have it.
		using ComponentDrawer = std::function<void(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& info,
			ComponentDrawResult& result)>;

		// The drawers by component type (ComponentInfo::TypeId): the editor's own (the UIRect's) and those registered later.
		class ComponentDrawers
		{
		public:
			static void Register(entt::id_type type, ComponentDrawer drawer);
			// Draws the component's drawer, if it has one. Returns whether it had.
			static bool Draw(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& info, ComponentDrawResult& result);
		};

		// The UIRect's drawer: notices (outside a canvas, a nested canvas), the anchor preset grid with its Pivot and Position
		// toggles, and the rect in pixels at the game's screen. While the selection stretches along an axis it places the
		// rect per axis instead of the generic Position and Size rows: Left and Right (Top and Bottom) margins along a
		// stretched axis, the position and width (height) along the other.
		void DrawUIRect(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& info, ComponentDrawResult& result);

	}

}
