#include "UI/ItemProbe.h"

#include <imgui_internal.h>

#include <unordered_map>

namespace Strata::UI
{

	namespace
	{

		std::unordered_map<ImGuiID, ItemProbe::Item> s_Items;

		ImGuiID HashKey(std::string_view key)
		{
			return ImHashStr(key.data(), key.size());
		}

	}

	void ItemProbe::Record(std::string_view key, bool enabled, ImU32 color, std::string_view text)
	{
		const int frame = ImGui::GetFrameCount();
		Item& item = s_Items[HashKey(key)];
		item.Duplicate = item.Frame == frame;
		item.Min = ImGui::GetItemRectMin();
		item.Max = ImGui::GetItemRectMax();
		item.Frame = frame;
		item.Enabled = enabled;
		item.Color = color;
		// Reuses the item's storage: recording the same widget every frame allocates nothing once its text fits.
		item.Text.assign(text);
	}

	std::optional<ItemProbe::Item> ItemProbe::Find(std::string_view key)
	{
		if (!ImGui::GetCurrentContext())
			return std::nullopt;
		const auto it = s_Items.find(HashKey(key));
		if (it == s_Items.end() || it->second.Frame != ImGui::GetFrameCount())
			return std::nullopt;
		return it->second;
	}

	void ItemProbe::Clear()
	{
		s_Items.clear();
	}

}
