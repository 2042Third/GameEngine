#include "UI/ItemProbe.h"

#include <imgui_internal.h>

#include <algorithm>
#include <charconv>
#include <cstring>
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
		RecordRect(key, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), enabled, color, text);
	}

	void ItemProbe::RecordRect(std::string_view key, const ImVec2& min, const ImVec2& max, bool enabled, ImU32 color, std::string_view text)
	{
		const int frame = ImGui::GetFrameCount();
		Item& item = s_Items[HashKey(key)];
		item.Duplicate = item.Frame == frame;
		item.Min = min;
		item.Max = max;
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

	NumberedProbeKey::NumberedProbeKey(std::string_view prefix, size_t number)
	{
		m_Size = std::min(prefix.size(), c_MaxPrefix);
		std::memcpy(m_Text, prefix.data(), m_Size);
		// The buffer has room for every size_t after the prefix.
		const std::to_chars_result result = std::to_chars(m_Text + m_Size, m_Text + sizeof(m_Text), number);
		m_Size = static_cast<size_t>(result.ptr - m_Text);
	}

}
