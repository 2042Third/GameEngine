#include "UI/ComponentDrawers.h"

#include <Strata/GameUI/GameUIComponents.h>

#include <unordered_map>

namespace Strata::UI
{

	namespace
	{

		std::unordered_map<entt::id_type, ComponentDrawer> MakeBuiltinDrawers()
		{
			std::unordered_map<entt::id_type, ComponentDrawer> drawers;
			drawers[entt::type_id<UIRectComponent>().hash()] = &DrawUIRect;
			return drawers;
		}

		std::unordered_map<entt::id_type, ComponentDrawer>& GetDrawers()
		{
			static std::unordered_map<entt::id_type, ComponentDrawer> s_Drawers = MakeBuiltinDrawers();
			return s_Drawers;
		}

	}

	void ComponentDrawers::Register(entt::id_type type, ComponentDrawer drawer)
	{
		GetDrawers()[type] = std::move(drawer);
	}

	bool ComponentDrawers::Draw(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& info)
	{
		const auto it = GetDrawers().find(info.TypeId);
		if (it == GetDrawers().end() || entities.empty())
			return false;
		it->second(context, entities, info);
		return true;
	}

}
