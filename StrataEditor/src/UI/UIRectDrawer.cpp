#include "UI/ComponentDrawers.h"

#include "Editor/EditorContext.h"
#include "Editor/GameUIPresets.h"
#include "Editor/UIRectEdit.h"
#include "UI/EditorFonts.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/PropertyWidgets.h"
#include "UI/Theme.h"

#include <Strata/Core/Log.h>
#include <Strata/GameUI/GameUIComponents.h>
#include <Strata/Scene/Scene.h>

#include <imgui.h>

#include <optional>
#include <string>

namespace Strata::UI
{

	namespace
	{

		// The grid's toggles, kept while the editor runs: the person's choice applies to the next presets too.
		struct AnchorGridOptions
		{
			bool SetPivot = true;        // A preset sets the pivot to match (top-left: the pivot at the top-left corner)
			bool SnapToAnchors = false;  // A preset moves the rect onto its anchors instead of keeping it in place
		};
		AnchorGridOptions s_GridOptions;

		void Notice(const char* icon, std::string_view text, const ImVec4& color)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::TextUnformatted(icon);
			ImGui::PopStyleColor();
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, GetThemeColors().TextSecondary);
			ImGui::TextWrapped("%.*s", static_cast<int>(text.size()), text.data());
			ImGui::PopStyleColor();
		}

		// A preset's picture: the parent's outline, the rect where the preset puts it and its anchors.
		void DrawPresetGlyph(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, UIAnchorPreset preset, bool current, bool hovered)
		{
			const ThemeColors& colors = GetThemeColors();
			const ImVec2 size(max.x - min.x, max.y - min.y);
			const float inset = size.x * 0.18f;
			const ImVec2 parentMin(min.x + inset, min.y + inset);
			const ImVec2 parentMax(max.x - inset, max.y - inset);
			const ImVec2 parentSize(parentMax.x - parentMin.x, parentMax.y - parentMin.y);
			const float rounding = ImGui::GetStyle().FrameRounding;
			if (current || hovered)
				drawList->AddRectFilled(min, max, ImGui::GetColorU32(current ? colors.AccentMuted : colors.SelectionHovered), rounding);
			drawList->AddRect(parentMin, parentMax, ImGui::GetColorU32(colors.TextDisabled));

			const UIRectEdit::Anchors anchors = UIRectEdit::GetAnchors(preset);
			// Along a stretched axis the rect spans the parent (a little inside); at a point it is a small square there.
			ImVec2 rectMin;
			ImVec2 rectMax;
			const float mark = parentSize.x * 0.32f;
			for (int axis = 0; axis < 2; axis++)
			{
				const float start = axis == 0 ? parentMin.x : parentMin.y;
				const float span = axis == 0 ? parentSize.x : parentSize.y;
				float low = 0.0f;
				float high = 0.0f;
				if (anchors.Min[axis] != anchors.Max[axis])
				{
					low = start + span * 0.12f;
					high = start + span * 0.88f;
				}
				else
				{
					const float at = start + span * anchors.Min[axis];
					low = at - mark * anchors.Pivot[axis];
					high = low + mark;
				}
				(axis == 0 ? rectMin.x : rectMin.y) = low;
				(axis == 0 ? rectMax.x : rectMax.y) = high;
			}
			drawList->AddRectFilled(rectMin, rectMax, ImGui::GetColorU32(current ? colors.Accent : colors.TextSecondary), rounding * 0.5f);
		}

		// The preset grid: rows top, middle, bottom, stretch; columns left, center, right, stretch.
		void DrawAnchorGrid(EditorContext& context, std::span<const Entity> entities, const std::optional<UIAnchorPreset>& current)
		{
			ImGui::Checkbox("Pivot", &s_GridOptions.SetPivot);
			ItemProbe::Record("Inspector.UIRect.Anchor.Pivot");
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				ImGui::SetTooltip("Set the pivot with the anchors (a top-left preset puts the pivot in the top-left corner)");
			ImGui::SameLine();
			ImGui::Checkbox("Position", &s_GridOptions.SnapToAnchors);
			ItemProbe::Record("Inspector.UIRect.Anchor.Position");
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				ImGui::SetTooltip("Move the rect onto its new anchors (else it stays where it is)");

			const float cell = ImGui::GetFrameHeight() * 1.6f;
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			for (uint32_t index = 0; index < c_UIAnchorPresetCount; index++)
			{
				const UIAnchorPreset preset = static_cast<UIAnchorPreset>(index);
				if (index % 4 != 0)
					ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
				ImGui::PushID(static_cast<int>(index));
				const bool clicked = ImGui::InvisibleButton("##Preset", ImVec2(cell, cell));
				const bool hovered = ImGui::IsItemHovered();
				DrawPresetGlyph(drawList, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), preset, current && *current == preset, hovered);
				ItemProbe::Record(std::string("Inspector.UIRect.Anchor.") + UIRectEdit::GetPresetName(preset));
				if (hovered)
					ImGui::SetTooltip("%s", UIRectEdit::GetPresetName(preset));
				ImGui::PopID();
				if (!clicked)
					continue;
				std::string error;
				if (!UIRectEdit::ApplyPresetWithUndo(context, entities, preset, s_GridOptions.SetPivot, s_GridOptions.SnapToAnchors, &error))
					ST_WARN("UI Rect anchors: {}", error);
				ImGui::CloseCurrentPopup();
			}
		}

	}

	void DrawUIRect(EditorContext& context, std::span<const Entity> entities, const ComponentInfo&)
	{
		const ThemeColors& colors = GetThemeColors();
		const Entity primary = entities.back();
		Scene& scene = *primary.GetScene();
		const Entity canvas = GameUIPresets::FindCanvas(primary);
		if (!canvas)
		{
			Notice(Icons::TriangleAlert, "Not on a UI canvas: the game does not draw it. Give it, or one of its parents, a UI Canvas.", colors.Warning);
			return;
		}
		if (canvas == primary)
		{
			if (GameUIPresets::FindCanvas(primary.GetParent()))
				Notice(Icons::Info, "A canvas inside another canvas is an element of the outer one: its scale settings do nothing.", colors.Info);
			else
			{
				Notice(Icons::Info, "A root canvas fills the screen: its rect's values do nothing.", colors.Info);
				return;
			}
		}

		const UIRectComponent& rect = primary.GetComponent<UIRectComponent>();
		const UIScreen screen = UIRectEdit::GetGameScreen(context);
		const std::optional<UIRectEdit::Placement> placement = UIRectEdit::FindPlacement(scene, primary, screen);
		if (!BeginPropertyTable("UIRectDrawer"))
			return;

		PropertyLabel("Anchors", "Where the rect sits on its parent: a corner, an edge's middle, the center, or stretched along an edge or "
			"over the whole parent", "Inspector.UIRect.Anchors");
		const std::optional<UIAnchorPreset> current = UIRectEdit::FindPreset(rect);
		const std::string label = fmt::format("{}  {}", Icons::Anchor, current ? UIRectEdit::GetPresetName(*current) : "custom");
		if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0.0f)))
			ImGui::OpenPopup("UIRectAnchors");
		ItemProbe::Record("Inspector.UIRect.AnchorButton", placement.has_value(), 0, label);
		if (ImGui::BeginPopup("UIRectAnchors"))
		{
			DrawAnchorGrid(context, entities, current);
			ImGui::EndPopup();
		}

		// Along a stretched axis the rect is placed by its distance to the parent's edges.
		if (placement)
		{
			for (int axis = 0; axis < 2; axis++)
			{
				if (!UIRectEdit::IsStretched(rect, axis))
					continue;
				const glm::vec2 margins = UIRectEdit::GetMargins(rect, placement->Box, placement->Parent, axis);
				for (int side = 0; side < 2; side++)
				{
					const char* name = axis == 0 ? (side == 0 ? "Left" : "Right") : (side == 0 ? "Top" : "Bottom");
					const std::string probe = fmt::format("Inspector.UIRect.{}", name);
					PropertyLabel(name, "The distance from the parent's edge, in canvas units (positive: inside)", probe);
					float value = margins[side];
					ImGui::SetNextItemWidth(-FLT_MIN);
					ImGui::PushID(probe.c_str());
					const bool changed = ImGui::DragFloat("##Margin", &value, 0.5f, 0.0f, 0.0f, "%.1f");
					ItemProbe::Record(probe);
					if (changed)
					{
						std::string error;
						if (!UIRectEdit::SetMarginWithUndo(context, entities, axis, side == 1, value, &error))
							ST_WARN("UI Rect {}: {}", name, error);
					}
					if (ImGui::IsItemDeactivatedAfterEdit())
						context.GetUndoStack().BreakMerge();
					ImGui::PopID();
				}
			}
		}

		PropertyLabel("On Screen", "Where the game shows the rect: x, y, width and height in pixels at the game's screen", "Inspector.UIRect.Pixels");
		const std::string pixels = placement
			? fmt::format("{:.0f}, {:.0f}   {:.0f} x {:.0f}", placement->Box.Min.x * placement->Scale, placement->Box.Min.y * placement->Scale,
				placement->Box.Size.x * placement->Scale, placement->Box.Size.y * placement->Scale)
			: std::string("not laid out");
		PushFont(EditorFont::Mono);
		ImGui::TextUnformatted(pixels.c_str());
		ImGui::PopFont();
		ItemProbe::Record("Inspector.UIRect.Pixels", true, 0, pixels);
		EndPropertyTable();
	}

}
