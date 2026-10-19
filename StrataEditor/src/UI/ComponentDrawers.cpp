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

	void ComponentDrawResult::Replace(std::string_view property)
	{
		if (!Replaces(property) && ReplacedCount < c_MaxReplaced)
			Replaced[ReplacedCount++] = property;
	}

	bool ComponentDrawResult::Replaces(std::string_view property) const
	{
		for (uint32_t index = 0; index < ReplacedCount; index++)
		{
			if (Replaced[index] == property)
				return true;
		}
		return false;
	}

	void ComponentDrawers::Register(entt::id_type type, ComponentDrawer drawer)
	{
		GetDrawers()[type] = std::move(drawer);
	}

	bool ComponentDrawers::Draw(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& info, const ComponentDrawOverrides* overrides,
		ComponentDrawResult& result)
	{
		const auto it = GetDrawers().find(info.TypeId);
		if (it == GetDrawers().end() || entities.empty())
			return false;
		it->second(context, entities, info, overrides, result);
		return true;
	}

}
