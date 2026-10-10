#pragma once

#include <imgui.h>

#include <optional>
#include <string_view>

namespace Strata::UI
{

	// Where the widget kit's widgets were drawn, by key, so UI tests can find a button and click it (ImGuiHarness). Every
	// widget of UI/Widgets.h records its rectangle under its id: one hash-map write per widget and frame, in every build
	// configuration, so tests see what users get. Keys are the ids the widgets were given (e.g. "Toolbar.Play"); use a
	// fixed set of them, since entries are kept until Clear. Main thread only.
	class ItemProbe
	{
	public:
		struct Item
		{
			ImVec2 Min;
			ImVec2 Max;
			int Frame = -1;           // ImGui::GetFrameCount() when it was recorded
			bool Duplicate = false;   // Recorded more than once in that frame: the key does not name one widget
			bool Enabled = true;      // Clickable (not drawn disabled)
			ImU32 Color = 0;          // The color that carries the widget's meaning (a pill's), packed; 0 for other widgets

			ImVec2 GetCenter() const { return ImVec2((Min.x + Max.x) * 0.5f, (Min.y + Max.y) * 0.5f); }
		};

		// Records the last item ImGui submitted (ImGui::GetItemRectMin/Max) under the key.
		static void Record(std::string_view key, bool enabled = true, ImU32 color = 0);
		// The item recorded under the key in the latest frame of the current context; nothing when it was not drawn then.
		static std::optional<Item> Find(std::string_view key);
		static void Clear();
	};

}
