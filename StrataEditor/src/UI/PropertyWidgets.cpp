#include "UI/PropertyWidgets.h"

#include "UI/EditorFonts.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Math/Math.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Strata
{

	namespace
	{

		// The pickers' lists, in text heights (they scale with the UI).
		constexpr float c_PickerWidthInFontSizes = 26.0f;
		constexpr float c_PickerHeightInFontSizes = 18.0f;
		// The axis labels of vector fields, in text heights: as wide as their letter and a little room.
		constexpr float c_AxisLabelWidthInFontSizes = 0.95f;
		// Numbers are edited with three decimals; a value that rounds to zero shows as 0, never as -0.000.
		constexpr const char* c_FloatFormat = "%.3f";
		constexpr float c_ShownZero = 0.0005f;
		// What a field shows when the selected entities' values differ.
		constexpr const char* c_MixedText = "--";
		constexpr const char* c_Ellipsis = "\xE2\x80\xA6";
		constexpr const char* c_AxisNames[] = { "X", "Y", "Z", "W" };
		// The property tables' columns: labels and values, as shares of the width (the user resizes them for every table of
		// a panel at once).
		constexpr float c_LabelColumnWeight = 0.42f;
		constexpr float c_ValueColumnWeight = 0.58f;
		// Rotations whose quaternions' dot product is this close to 1 are the same (an angle below a tenth of a degree).
		constexpr float c_SameRotationTolerance = 1.0e-6f;
		// The smallest size limit a PowerOfTwoLimit property offers, in pixels.
		constexpr uint32_t c_SmallestSizeLimit = 32;

		float GetSpeed(const PropertyInfo& property, float fallback)
		{
			return property.Speed > 0.0f ? property.Speed : fallback;
		}

		// The value a field starts from: values that would print as "-0.000" print as 0.
		float ToShown(float value)
		{
			return std::abs(value) < c_ShownZero ? 0.0f : value;
		}

		ImVec4 GetAxisColor(int axis)
		{
			const UI::ThemeColors& colors = UI::GetThemeColors();
			switch (axis)
			{
				case 0: return colors.AxisX;
				case 1: return colors.AxisY;
				case 2: return colors.AxisZ;
				default: return colors.TextDisabled;
			}
		}

		std::string ProbeKey(std::string_view prefix, std::string_view suffix)
		{
			std::string key(prefix);
			if (!suffix.empty())
			{
				key += '.';
				key += suffix;
			}
			return key;
		}

		// A number as people write it: up to three decimals without trailing zeros ("0.8", "-4", "12.375"), never "-0".
		std::string FormatNumber(float value)
		{
			std::string text = fmt::format("{:.3f}", ToShown(value));
			if (text.find('.') != std::string::npos)
			{
				while (text.back() == '0')
					text.pop_back();
				if (text.back() == '.')
					text.pop_back();
			}
			return text == "-0" ? std::string("0") : text;
		}

		// `text` cut at its end with an ellipsis to fit `width` in the current font (ASCII text: cut by bytes, which are
		// characters there; other text is cut at character boundaries).
		std::string FitWidth(std::string_view text, float width)
		{
			if (ImGui::CalcTextSize(text.data(), text.data() + text.size()).x <= width)
				return std::string(text);
			const float ellipsis = ImGui::CalcTextSize(c_Ellipsis).x;
			size_t length = text.size();
			while (length > 0)
			{
				length--;
				while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
					length--;
				if (ImGui::CalcTextSize(text.data(), text.data() + length).x + ellipsis <= width)
					break;
			}
			return std::string(text.substr(0, length)) + c_Ellipsis;
		}

		// A drag field of a float that shows its value as FormatNumber writes it, centered, or "--" when the selected entities'
		// values differ; a value too wide for the field is cut with an ellipsis and shown whole in the field's tooltip. Typing
		// in it (Ctrl+click, double click) shows three decimals. ImGui's own text of the field is drawn transparent (it cannot
		// be formatted this way) except while typing. The probe records the field and the text it shows. Returns true when
		// the value changed; `outFinished` tells when an edit ended.
		bool DragFloatField(const char* id, float& value, float speed, float min, float max, bool mixed, float width, std::string_view probeKey,
			bool& outFinished)
		{
			const ImGuiStyle& style = ImGui::GetStyle();
			const UI::ThemeColors& colors = UI::GetThemeColors();
			const ImGuiID itemId = ImGui::GetID(id);
			const bool typing = ImGui::TempInputIsActive(itemId);
			ImGui::SetNextItemWidth(width);
			float shown = ToShown(value);
			if (!typing)
				ImGui::PushStyleColor(ImGuiCol_Text, UI::WithAlpha(colors.Text, 0.0f));
			const bool changed = ImGui::DragScalar(id, ImGuiDataType_Float, &shown, speed, &min, &max, c_FloatFormat);
			if (!typing)
				ImGui::PopStyleColor();
			if (changed)
				value = shown;
			outFinished = ImGui::IsItemDeactivatedAfterEdit();
			if (ImGui::TempInputIsActive(itemId))
				return changed;

			const ImVec2 itemMin = ImGui::GetItemRectMin();
			const ImVec2 itemMax = ImGui::GetItemRectMax();
			const std::string full = mixed && !changed ? std::string(c_MixedText) : FormatNumber(value);
			const float room = std::max(itemMax.x - itemMin.x - style.FramePadding.x, 1.0f);
			const std::string text = FitWidth(full, room);
			const ImVec2 size = ImGui::CalcTextSize(text.c_str());
			ImGui::GetWindowDrawList()->AddText(ImVec2(itemMin.x + (itemMax.x - itemMin.x - size.x) * 0.5f, itemMin.y + (itemMax.y - itemMin.y - size.y) * 0.5f),
				ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
			if (text != full && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && !ImGui::IsItemActive())
				ImGui::SetTooltip("%s", full.c_str());
			if (!probeKey.empty())
				UI::ItemProbe::Record(probeKey, true, 0, text);
			return changed;
		}

		// A combo for a choice, as wide as its cell, showing `preview`. The probe records it with the preview as its text, with
		// its own rectangle: while it is open, its list's items come after it. Returns true while the list is open (call
		// ImGui::EndCombo then).
		bool BeginChoice(const std::string& preview, std::string_view probeKey)
		{
			ImGui::SetNextItemWidth(-FLT_MIN);
			const ImVec2 min = ImGui::GetCursorScreenPos();
			const float width = ImGui::CalcItemWidth();
			const bool open = ImGui::BeginCombo("##Value", preview.c_str());
			if (!probeKey.empty())
				UI::ItemProbe::RecordRect(probeKey, min, ImVec2(min.x + width, min.y + ImGui::GetFrameHeight()), true, 0, preview);
			return open;
		}

		// An entry of a choice's list (the probe records it under `probeKey` unless that is empty). Returns true when picked.
		bool Choice(const std::string& label, bool selected, int id, const std::string& probeKey)
		{
			ImGui::PushID(id);
			UI::PushSelectionColors(selected);
			const bool picked = ImGui::Selectable(label.c_str(), selected);
			UI::PopSelectionColors();
			ImGui::PopID();
			if (!probeKey.empty())
				UI::ItemProbe::Record(probeKey);
			return picked;
		}

		std::string GetLimitLabel(uint32_t limit)
		{
			return limit == 0 ? std::string("No limit") : std::to_string(limit);
		}

		// A size limit (PropertyFlags::PowerOfTwoLimit): "No limit" (0) or a power of two up to the property's maximum. A
		// value that is no power of two (written by hand) is shown as it is until another is chosen.
		PropertyEditResult DrawPowerOfTwoLimit(const PropertyInfo& property, uint32_t& value, const PropertyWidgetOptions& options)
		{
			PropertyEditResult result;
			const uint64_t max = property.HasRange() ? static_cast<uint64_t>(property.Max) : std::numeric_limits<uint32_t>::max();
			const std::string preview = options.Mixed ? std::string(c_MixedText) : GetLimitLabel(value);
			if (BeginChoice(preview, options.ProbeKey))
			{
				std::vector<uint32_t> choices = { 0 };
				for (uint64_t limit = c_SmallestSizeLimit; limit <= max; limit *= 2)
					choices.push_back(static_cast<uint32_t>(limit));
				for (const uint32_t choice : choices)
				{
					const std::string label = GetLimitLabel(choice);
					const std::string probe = options.ProbeKey.empty() ? std::string() : ProbeKey(options.ProbeKey, choice == 0 ? std::string("NoLimit") : label);
					if (Choice(label, choice == value && !options.Mixed, static_cast<int>(choice), probe))
					{
						result.Changed = choice != value || options.Mixed;
						value = choice;
					}
				}
				ImGui::EndCombo();
			}
			return result;
		}

		float GetAxisLabelWidth()
		{
			return std::round(ImGui::GetFontSize() * c_AxisLabelWidthInFontSizes);
		}

		// An axis label: a tab in the axis's color (GetAxisColor of its index) before its field, holding its name. Returns
		// true when clicked (resets the component when there is a default to reset to).
		bool AxisLabel(int axis, const char* name, float width, bool resettable, std::string_view probeKey)
		{
			const UI::ThemeColors& colors = UI::GetThemeColors();
			const float height = ImGui::GetFrameHeight();
			ImGui::PushID(axis);
			const bool pressed = ImGui::InvisibleButton("##Axis", ImVec2(width, height)) && resettable;
			ImGui::PopID();
			const bool hovered = ImGui::IsItemHovered() && resettable;
			const ImVec2 min = ImGui::GetItemRectMin();
			const ImVec2 max = ImGui::GetItemRectMax();
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const ImVec4 color = GetAxisColor(axis);
			drawList->AddRectFilled(min, max, ImGui::GetColorU32(UI::WithAlpha(color, hovered ? 1.0f : 0.82f)), ImGui::GetStyle().FrameRounding,
				ImDrawFlags_RoundCornersLeft);
			UI::PushFont(UI::EditorFont::SemiBold, UI::TextSize::Caption);
			const ImVec2 textSize = ImGui::CalcTextSize(name);
			drawList->AddText(ImVec2(min.x + (width - textSize.x) * 0.5f, min.y + (height - textSize.y) * 0.5f), ImGui::GetColorU32(colors.TextOnAccent), name);
			ImGui::PopFont();
			if (resettable && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				ImGui::SetTooltip("Reset %s", name);
			if (!probeKey.empty())
				UI::ItemProbe::Record(ProbeKey(probeKey, name), resettable);
			return pressed;
		}

		// One field per component behind its axis label, together `width` wide: X, Y, Z, W for vectors and rotations, R, G,
		// B, A for colors (`names`). `defaults`: what the labels reset to (null: they do not reset). The probe records the row
		// under the options' key, the labels under "<key>.<name>" and the fields with their text under "<key>.<name>.Value".
		PropertyEditResult DrawAxisFields(float* values, int count, const char* const* names, float width, float speed, float min, float max,
			const PropertyWidgetOptions& options, const float* defaults)
		{
			PropertyEditResult result;
			const ImGuiStyle& style = ImGui::GetStyle();
			const float total = std::max(width, ImGui::GetFrameHeight() * static_cast<float>(count));
			const float gap = style.ItemInnerSpacing.x;
			const float slot = (total - gap * static_cast<float>(count - 1)) / static_cast<float>(count);
			const float labelWidth = std::min(GetAxisLabelWidth(), slot * 0.5f);
			ImGui::BeginGroup();
			for (int axis = 0; axis < count; axis++)
			{
				if (axis > 0)
					ImGui::SameLine(0.0f, gap);
				const bool mixed = options.MixedComponents[static_cast<size_t>(axis)];
				// Reset: also when the value shown is the default already but other entities' values differ.
				if (AxisLabel(axis, names[axis], labelWidth, defaults != nullptr, options.ProbeKey) && (values[axis] != defaults[axis] || mixed))
				{
					values[axis] = defaults[axis];
					result.Changed = true;
					result.Finished = true;
					result.ChangedComponents[static_cast<size_t>(axis)] = true;
				}
				ImGui::SameLine(0.0f, 0.0f);
				ImGui::PushID(axis);
				// Square left corners against the label.
				ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
				const std::string probe = options.ProbeKey.empty() ? std::string() : ProbeKey(ProbeKey(options.ProbeKey, names[axis]), "Value");
				bool finished = false;
				if (DragFloatField("##Value", values[axis], speed, min, max, mixed, std::max(slot - labelWidth, 1.0f), probe, finished))
				{
					result.Changed = true;
					result.ChangedComponents[static_cast<size_t>(axis)] = true;
				}
				ImGui::PopStyleVar();
				result.Finished |= finished;
				ImGui::PopID();
			}
			ImGui::EndGroup();
			if (!options.ProbeKey.empty())
				UI::ItemProbe::Record(options.ProbeKey);
			return result;
		}

		PropertyEditResult DrawFloat(const PropertyInfo& property, float& value, const PropertyWidgetOptions& options)
		{
			PropertyEditResult result;
			const float min = property.HasRange() ? property.Min : -FLT_MAX;
			const float max = property.HasRange() ? property.Max : FLT_MAX;
			if (HasFlag(property.Flags, PropertyFlags::Slider) && property.HasRange())
			{
				float shown = ToShown(value);
				ImGui::SetNextItemWidth(-FLT_MIN);
				result.Changed = ImGui::SliderFloat("##Value", &shown, min, max, options.Mixed ? c_MixedText : c_FloatFormat);
				if (result.Changed)
					value = shown;
				result.Finished = ImGui::IsItemDeactivatedAfterEdit();
				if (!options.ProbeKey.empty())
					UI::ItemProbe::Record(options.ProbeKey);
				return result;
			}
			result.Changed = DragFloatField("##Value", value, GetSpeed(property, 0.05f), min, max, options.Mixed, -FLT_MIN, options.ProbeKey, result.Finished);
			return result;
		}

		// Euler angles are not unique: converting the quaternion back every frame would make a drag past 90 degrees jump. The
		// angles being edited are kept while the rotation is the one they produced, up to the rounding of entities that keep
		// their own other angles (several selected entities, PropertyEdit's SetRotationAnglesWithUndo).
		PropertyEditResult DrawRotation(const PropertyInfo& property, glm::quat& rotation, const PropertyWidgetOptions& options)
		{
			static ImGuiID s_EulerId = 0;
			static glm::quat s_EulerRotation;
			static glm::vec3 s_Euler;
			const ImGuiID id = ImGui::GetID("##Euler");
			const bool same = s_EulerId == id && std::abs(glm::dot(s_EulerRotation, rotation)) >= 1.0f - c_SameRotationTolerance;
			glm::vec3 euler = same ? s_Euler : Math::QuatToEulerDegrees(rotation);
			std::optional<glm::vec3> defaults;
			if (options.DefaultValue && std::holds_alternative<glm::quat>(*options.DefaultValue))
				defaults = Math::QuatToEulerDegrees(std::get<glm::quat>(*options.DefaultValue));
			PropertyEditResult result = DrawAxisFields(&euler.x, 3, c_AxisNames, ImGui::GetContentRegionAvail().x, GetSpeed(property, 0.5f), -FLT_MAX, FLT_MAX,
				options, defaults ? &defaults->x : nullptr);
			result.EulerAngles = euler;
			if (result.Changed)
			{
				rotation = Math::EulerDegreesToQuat(euler);
				s_EulerId = id;
				s_EulerRotation = rotation;
				s_Euler = euler;
			}
			return result;
		}

		std::string GetAssetLabel(AssetHandle handle)
		{
			if (!handle.IsValid())
				return "None";
			if (!AssetManager::HasActive())
				return handle.ToString();
			std::optional<AssetMetadata> metadata = AssetManager::GetActive()->GetMetadata(handle);
			if (!metadata)
				return fmt::format("Missing ({})", handle.ToString());
			return metadata->Name.empty() ? metadata->Path : metadata->Name;
		}

		// What the open picker lists: built when it opens and when its search text or (entities) the scene's hierarchy
		// changes, so a frame with the picker open draws only the visible rows of the list.
		struct PickerState
		{
			ImGuiID Popup = 0;
			std::string Search;
			std::string BuiltSearch;
			bool Built = false;
			uint64_t HierarchyVersion = 0;
			std::vector<AssetMetadata> Assets;
			std::vector<std::pair<UUID, std::string>> Entities;
		};
		PickerState s_Picker;

		// Starts the picker's state when its popup appears.
		void BeginPicker(ImGuiID popup)
		{
			if (s_Picker.Popup == popup && !ImGui::IsWindowAppearing())
				return;
			s_Picker = PickerState();
			s_Picker.Popup = popup;
			ImGui::SetKeyboardFocusHere();
		}

		// The search field of a picker; returns true when Enter was pressed in it (pick the first match).
		bool PickerSearchField()
		{
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * c_PickerWidthInFontSizes);
			const std::string hint = std::string(Icons::Search) + "  Search";
			return ImGui::InputTextWithHint("##Search", hint.c_str(), &s_Picker.Search, ImGuiInputTextFlags_EnterReturnsTrue);
		}

		// A row of a picker's list: an icon, the name and dimmer detail text. Returns true when clicked.
		bool PickerRow(const char* icon, std::string_view name, std::string_view detail, bool current)
		{
			const UI::ThemeColors& colors = UI::GetThemeColors();
			UI::PushSelectionColors(current);
			const bool picked = ImGui::Selectable("##Row", current, ImGuiSelectableFlags_None, ImVec2(0.0f, ImGui::GetTextLineHeight()));
			UI::PopSelectionColors();
			const ImVec2 min = ImGui::GetItemRectMin();
			const ImVec2 max = ImGui::GetItemRectMax();
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const float iconWidth = ImGui::CalcTextSize(icon).x;
			const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
			drawList->AddText(min, ImGui::GetColorU32(colors.Accent), icon);
			const float nameX = min.x + iconWidth + gap * 2.0f;
			drawList->PushClipRect(min, max, true);
			drawList->AddText(ImVec2(nameX, min.y), ImGui::GetColorU32(colors.Text), name.data(), name.data() + name.size());
			if (!detail.empty())
			{
				const float detailX = nameX + ImGui::CalcTextSize(name.data(), name.data() + name.size()).x + gap * 2.0f;
				drawList->AddText(ImVec2(detailX, min.y), ImGui::GetColorU32(colors.TextSecondary), detail.data(), detail.data() + detail.size());
			}
			drawList->PopClipRect();
			return picked;
		}

		bool Contains(std::string_view text, const std::string& lowerSearch)
		{
			return lowerSearch.empty() || StringUtils::ToLower(std::string(text)).find(lowerSearch) != std::string::npos;
		}

		// The button of a reference (asset or entity): an icon and the referenced name on a field's surface, leaving room for
		// the clear button after it (ClearButton). Returns true when clicked; drops are accepted by the caller on it.
		bool ReferenceButton(const char* icon, std::string_view label, bool mixed, bool hasValue, std::string_view probeKey)
		{
			const ImGuiStyle& style = ImGui::GetStyle();
			const UI::ThemeColors& colors = UI::GetThemeColors();
			const float clearWidth = ImGui::GetFrameHeight();
			const float width = std::max(ImGui::GetContentRegionAvail().x - clearWidth - style.ItemInnerSpacing.x, ImGui::GetFrameHeight());
			const bool clicked = ImGui::InvisibleButton("##Reference", ImVec2(width, ImGui::GetFrameHeight()));
			const bool hovered = ImGui::IsItemHovered();
			const ImVec2 min = ImGui::GetItemRectMin();
			const ImVec2 max = ImGui::GetItemRectMax();
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			drawList->AddRectFilled(min, max, ImGui::GetColorU32(hovered ? colors.Raised : colors.Chrome), style.FrameRounding);
			drawList->AddRect(min, max, ImGui::GetColorU32(colors.Border), style.FrameRounding);
			const float textY = min.y + style.FramePadding.y;
			const float iconWidth = ImGui::CalcTextSize(icon).x;
			drawList->AddText(ImVec2(min.x + style.FramePadding.x, textY), ImGui::GetColorU32(hasValue ? colors.Accent : colors.TextDisabled), icon);
			const std::string text = mixed ? std::string(c_MixedText) : std::string(label);
			drawList->PushClipRect(min, ImVec2(max.x - style.FramePadding.x, max.y), true);
			drawList->AddText(ImVec2(min.x + style.FramePadding.x + iconWidth + style.ItemInnerSpacing.x, textY),
				ImGui::GetColorU32(hasValue || mixed ? colors.Text : colors.TextSecondary), text.c_str());
			drawList->PopClipRect();
			if (!probeKey.empty())
				UI::ItemProbe::Record(probeKey);
			return clicked;
		}

		// The clear button after a reference button. Returns true when clicked.
		bool ClearButton(bool enabled)
		{
			ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
			ImGui::PushID("Clear");
			const bool cleared = UI::IconButton("##Clear", Icons::X, "Clear", enabled);
			ImGui::PopID();
			return cleared;
		}

		PropertyEditResult DrawAssetPicker(const PropertyInfo& property, AssetHandle& handle, const PropertyWidgetOptions& options)
		{
			PropertyEditResult result;
			const UI::ThemeColors& colors = UI::GetThemeColors();
			const AssetType type = handle.IsValid() ? AssetManager::GetAssetType(handle) : property.AssetFilter;
			const std::string label = GetAssetLabel(handle);
			if (ReferenceButton(UI::GetAssetTypeIcon(type), label, options.Mixed, handle.IsValid(), options.ProbeKey))
				ImGui::OpenPopup("AssetPicker");
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				ImGui::SetTooltip("%s", handle.IsValid() ? label.c_str() : "Click to choose an asset, or drag one here from the Content Browser");
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(DragDrop::c_Asset); payload && payload->DataSize == static_cast<int>(sizeof(AssetHandle)))
				{
					AssetHandle dropped = UUID::Null();
					std::memcpy(&dropped, payload->Data, sizeof(AssetHandle));
					if (property.AssetFilter == AssetType::None || AssetManager::GetAssetType(dropped) == property.AssetFilter)
					{
						result.Changed = dropped != handle || options.Mixed;
						handle = dropped;
					}
				}
				ImGui::EndDragDropTarget();
			}
			if (ClearButton(handle.IsValid() || options.Mixed))
			{
				handle = UUID::Null();
				result.Changed = true;
			}

			if (ImGui::BeginPopup("AssetPicker"))
			{
				BeginPicker(ImGui::GetID("AssetPicker"));
				const bool enter = PickerSearchField();
				if (!s_Picker.Built || s_Picker.BuiltSearch != s_Picker.Search)
				{
					const std::string search = StringUtils::ToLower(s_Picker.Search);
					s_Picker.Assets.clear();
					if (AssetManager::HasActive())
					{
						for (AssetMetadata& metadata : AssetManager::GetActive()->GetAllMetadata(property.AssetFilter))
						{
							if (Contains(metadata.Name, search) || Contains(metadata.Path, search))
								s_Picker.Assets.push_back(std::move(metadata));
						}
					}
					std::sort(s_Picker.Assets.begin(), s_Picker.Assets.end(), [](const AssetMetadata& a, const AssetMetadata& b)
					{
						return a.IsBuiltin() != b.IsBuiltin() ? a.IsBuiltin() : a.Path < b.Path;
					});
					s_Picker.BuiltSearch = s_Picker.Search;
					s_Picker.Built = true;
				}
				if (enter && !s_Picker.Assets.empty())
				{
					handle = s_Picker.Assets.front().Handle;
					result.Changed = true;
					ImGui::CloseCurrentPopup();
				}
				if (ImGui::BeginChild("Assets", ImVec2(ImGui::GetFontSize() * c_PickerWidthInFontSizes, ImGui::GetFontSize() * c_PickerHeightInFontSizes)))
				{
					// Row 0 is "None", then the matching assets.
					ImGuiListClipper clipper;
					clipper.Begin(static_cast<int>(s_Picker.Assets.size()) + 1);
					while (clipper.Step())
					{
						for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
						{
							ImGui::PushID(row);
							bool picked = false;
							AssetHandle choice = UUID::Null();
							if (row == 0)
							{
								picked = PickerRow(Icons::CircleSlash, "None", {}, !handle.IsValid());
							}
							else
							{
								const AssetMetadata& metadata = s_Picker.Assets[static_cast<size_t>(row - 1)];
								choice = metadata.Handle;
								const std::string_view detail = metadata.IsBuiltin() ? std::string_view("Built-in") : std::string_view(metadata.Path);
								picked = PickerRow(UI::GetAssetTypeIcon(metadata.Type), metadata.Name.empty() ? metadata.Path : metadata.Name, detail,
									metadata.Handle == handle);
							}
							if (picked)
							{
								result.Changed = choice != handle || options.Mixed;
								handle = choice;
								ImGui::CloseCurrentPopup();
							}
							ImGui::PopID();
						}
					}
					if (s_Picker.Assets.empty())
						ImGui::TextColored(colors.TextSecondary, "%s", s_Picker.Search.empty() ? "No assets of this type" : "Nothing matches");
				}
				ImGui::EndChild();
				ImGui::EndPopup();
			}
			result.Finished = result.Changed;
			return result;
		}

		PropertyEditResult DrawEntityPicker(UUID& id, Scene& scene, const PropertyWidgetOptions& options)
		{
			PropertyEditResult result;
			const UI::ThemeColors& colors = UI::GetThemeColors();
			const Entity entity = id.IsValid() ? scene.GetEntityByUUID(id) : Entity();
			const std::string label = !id.IsValid() ? "None" : (entity ? entity.GetName() : fmt::format("Missing ({})", id.ToString()));
			if (ReferenceButton(Icons::Box, label, options.Mixed, id.IsValid(), options.ProbeKey))
				ImGui::OpenPopup("EntityPicker");
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				ImGui::SetTooltip("Click to choose an entity, or drag one here from the Hierarchy");
			if (ImGui::BeginDragDropTarget())
			{
				// The first of the dragged entities.
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(DragDrop::c_Entities); payload && payload->DataSize >= static_cast<int>(sizeof(UUID)))
				{
					UUID dropped = UUID::Null();
					std::memcpy(&dropped, payload->Data, sizeof(UUID));
					result.Changed = dropped != id || options.Mixed;
					id = dropped;
				}
				ImGui::EndDragDropTarget();
			}
			if (ClearButton(id.IsValid() || options.Mixed))
			{
				id = UUID::Null();
				result.Changed = true;
			}

			if (ImGui::BeginPopup("EntityPicker"))
			{
				BeginPicker(ImGui::GetID("EntityPicker"));
				const bool enter = PickerSearchField();
				if (!s_Picker.Built || s_Picker.BuiltSearch != s_Picker.Search || s_Picker.HierarchyVersion != scene.GetHierarchyVersion())
				{
					const std::string search = StringUtils::ToLower(s_Picker.Search);
					s_Picker.Entities.clear();
					for (const Entity candidate : scene.GetEntitiesInHierarchyOrder())
					{
						if (Contains(candidate.GetName(), search))
							s_Picker.Entities.emplace_back(candidate.GetUUID(), candidate.GetName());
					}
					s_Picker.BuiltSearch = s_Picker.Search;
					s_Picker.HierarchyVersion = scene.GetHierarchyVersion();
					s_Picker.Built = true;
				}
				if (enter && !s_Picker.Entities.empty())
				{
					result.Changed = s_Picker.Entities.front().first != id || options.Mixed;
					id = s_Picker.Entities.front().first;
					ImGui::CloseCurrentPopup();
				}
				if (ImGui::BeginChild("Entities", ImVec2(ImGui::GetFontSize() * c_PickerWidthInFontSizes, ImGui::GetFontSize() * c_PickerHeightInFontSizes)))
				{
					ImGuiListClipper clipper;
					clipper.Begin(static_cast<int>(s_Picker.Entities.size()) + 1);
					while (clipper.Step())
					{
						for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
						{
							ImGui::PushID(row);
							const UUID choice = row == 0 ? UUID::Null() : s_Picker.Entities[static_cast<size_t>(row - 1)].first;
							const std::string_view name = row == 0 ? std::string_view("None") : std::string_view(s_Picker.Entities[static_cast<size_t>(row - 1)].second);
							if (PickerRow(row == 0 ? Icons::CircleSlash : Icons::Box, name.empty() ? std::string_view("(unnamed)") : name, {}, choice == id))
							{
								result.Changed = choice != id || options.Mixed;
								id = choice;
								ImGui::CloseCurrentPopup();
							}
							ImGui::PopID();
						}
					}
					if (s_Picker.Entities.empty())
						ImGui::TextColored(colors.TextSecondary, "Nothing matches");
				}
				ImGui::EndChild();
				ImGui::EndPopup();
			}
			result.Finished = result.Changed;
			return result;
		}

	}

	PropertyEditResult DrawPropertyWidget(const PropertyInfo& property, PropertyValue& value, Scene& scene, const PropertyWidgetOptions& options)
	{
		PropertyEditResult result;
		ImGui::PushID(property.Name.c_str());
		bool continuous = false;
		switch (property.Type)
		{
			case PropertyType::Bool:
			{
				ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, options.Mixed);
				result.Changed = ImGui::Checkbox("##Value", &std::get<bool>(value));
				ImGui::PopItemFlag();
				if (!options.ProbeKey.empty())
					UI::ItemProbe::Record(options.ProbeKey);
				break;
			}
			case PropertyType::Int:
			{
				int32_t& number = std::get<int32_t>(value);
				const int32_t min = property.HasRange() ? static_cast<int32_t>(property.Min) : std::numeric_limits<int32_t>::min();
				const int32_t max = property.HasRange() ? static_cast<int32_t>(property.Max) : std::numeric_limits<int32_t>::max();
				ImGui::SetNextItemWidth(-FLT_MIN);
				result.Changed = ImGui::DragScalar("##Value", ImGuiDataType_S32, &number, GetSpeed(property, 0.2f), &min, &max, options.Mixed ? c_MixedText : "%d");
				result.Finished = ImGui::IsItemDeactivatedAfterEdit();
				if (!options.ProbeKey.empty())
					UI::ItemProbe::Record(options.ProbeKey);
				continuous = true;
				break;
			}
			case PropertyType::UInt:
			{
				uint32_t& number = std::get<uint32_t>(value);
				if (HasFlag(property.Flags, PropertyFlags::PowerOfTwoLimit))
				{
					result = DrawPowerOfTwoLimit(property, number, options);
					break;
				}
				const uint32_t min = property.HasRange() ? static_cast<uint32_t>(std::max(property.Min, 0.0f)) : 0u;
				const uint32_t max = property.HasRange() ? static_cast<uint32_t>(property.Max) : std::numeric_limits<uint32_t>::max();
				ImGui::SetNextItemWidth(-FLT_MIN);
				result.Changed = ImGui::DragScalar("##Value", ImGuiDataType_U32, &number, GetSpeed(property, 0.2f), &min, &max, options.Mixed ? c_MixedText : "%u");
				result.Finished = ImGui::IsItemDeactivatedAfterEdit();
				if (!options.ProbeKey.empty())
					UI::ItemProbe::Record(options.ProbeKey);
				continuous = true;
				break;
			}
			case PropertyType::Float:
				result = DrawFloat(property, std::get<float>(value), options);
				continuous = true;
				break;
			case PropertyType::Vec2:
			case PropertyType::Vec3:
			case PropertyType::Vec4:
			{
				const int count = property.Type == PropertyType::Vec2 ? 2 : (property.Type == PropertyType::Vec3 ? 3 : 4);
				float* components = property.Type == PropertyType::Vec2 ? &std::get<glm::vec2>(value).x
					: (property.Type == PropertyType::Vec3 ? &std::get<glm::vec3>(value).x : &std::get<glm::vec4>(value).x);
				const float* defaults = nullptr;
				if (options.DefaultValue && options.DefaultValue->index() == value.index())
				{
					defaults = property.Type == PropertyType::Vec2 ? &std::get<glm::vec2>(*options.DefaultValue).x
						: (property.Type == PropertyType::Vec3 ? &std::get<glm::vec3>(*options.DefaultValue).x : &std::get<glm::vec4>(*options.DefaultValue).x);
				}
				const float min = property.HasRange() ? property.Min : -FLT_MAX;
				const float max = property.HasRange() ? property.Max : FLT_MAX;
				result = DrawAxisFields(components, count, c_AxisNames, ImGui::GetContentRegionAvail().x, GetSpeed(property, 0.05f), min, max, options, defaults);
				continuous = true;
				break;
			}
			case PropertyType::Quat:
				result = DrawRotation(property, std::get<glm::quat>(value), options);
				continuous = true;
				break;
			case PropertyType::Color3:
			case PropertyType::Color4:
			{
				ImGui::SetNextItemWidth(-FLT_MIN);
				const ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_AlphaPreviewHalf;
				result.Changed = property.Type == PropertyType::Color3 ? ImGui::ColorEdit3("##Value", &std::get<glm::vec3>(value).x, flags)
					: ImGui::ColorEdit4("##Value", &std::get<glm::vec4>(value).x, flags);
				result.Finished = ImGui::IsItemDeactivatedAfterEdit();
				if (options.Mixed && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
					ImGui::SetTooltip("The selected entities have different colors: this is the first one's");
				if (!options.ProbeKey.empty())
					UI::ItemProbe::Record(options.ProbeKey);
				continuous = true;
				break;
			}
			case PropertyType::String:
			{
				std::string& text = std::get<std::string>(value);
				// Mixed text starts empty; whatever is typed goes to every entity.
				std::string shown = options.Mixed ? std::string() : text;
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (HasFlag(property.Flags, PropertyFlags::MultiLine))
					result.Changed = ImGui::InputTextMultiline("##Value", &shown, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.0f));
				else
					result.Changed = ImGui::InputTextWithHint("##Value", options.Mixed ? c_MixedText : "", &shown);
				if (result.Changed)
					text = std::move(shown);
				result.Finished = ImGui::IsItemDeactivatedAfterEdit();
				if (!options.ProbeKey.empty())
					UI::ItemProbe::Record(options.ProbeKey);
				continuous = true;
				break;
			}
			case PropertyType::Enum:
			{
				int32_t& selected = std::get<int32_t>(value);
				const EnumValue* current = property.FindEnumValue(selected);
				const std::string preview = options.Mixed ? std::string(c_MixedText) : (current ? current->GetDisplayName() : std::string("?"));
				if (BeginChoice(preview, options.ProbeKey))
				{
					for (const EnumValue& option : property.EnumValues)
					{
						const std::string probe = options.ProbeKey.empty() ? std::string() : ProbeKey(options.ProbeKey, option.Name);
						if (Choice(option.GetDisplayName(), option.Value == selected && !options.Mixed, option.Value, probe))
						{
							result.Changed = option.Value != selected || options.Mixed;
							selected = option.Value;
						}
					}
					ImGui::EndCombo();
				}
				break;
			}
			case PropertyType::Asset:
				result = DrawAssetPicker(property, std::get<UUID>(value), options);
				break;
			case PropertyType::Entity:
				result = DrawEntityPicker(std::get<UUID>(value), scene, options);
				break;
		}
		// Discrete widgets (checkboxes, combos, pickers, axis resets) finish with the change itself.
		if (result.Changed && !continuous)
			result.Finished = true;
		ImGui::PopID();
		return result;
	}

	namespace UI
	{

		bool BeginPropertyTable(const char* id)
		{
			// Every property table of a window has one id (whatever the ID stack holds, e.g. the component a section pushed),
			// which makes them instances of one table to ImGui: they share the label column's share of the width, which
			// dragging the divider between the columns (shown while it is hovered) changes for all of them, kept in imgui.ini.
			const ImGuiID tableId = ImHashStr("PropertyTable", 0, ImGui::GetCurrentWindow()->ID);
			const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV
				| ImGuiTableFlags_NoBordersInBodyUntilResize;
			if (!ImGui::BeginTableEx(id, tableId, 2, flags))
				return false;
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, c_LabelColumnWeight);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, c_ValueColumnWeight);
			return true;
		}

		void EndPropertyTable()
		{
			ImGui::EndTable();
		}

		void PropertyLabel(std::string_view label, std::string_view tooltip, std::string_view probeKey)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			// A label wider than its column (which it may fill up to the cell's padding: the clip rectangle ends there) ends in
			// an ellipsis; its tooltip names it whole.
			const std::string shown = FitWidth(label, std::max(ImGui::GetContentRegionAvail().x + ImGui::GetStyle().CellPadding.x, 1.0f));
			const bool cut = std::string_view(shown) != label;
			ImGui::PushStyleColor(ImGuiCol_Text, GetThemeColors().TextSecondary);
			ImGui::TextUnformatted(shown.c_str());
			ImGui::PopStyleColor();
			if ((cut || !tooltip.empty()) && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
			{
				const std::string text = !cut ? std::string(tooltip) : (tooltip.empty() ? std::string(label) : fmt::format("{}\n{}", label, tooltip));
				ImGui::SetTooltip("%s", text.c_str());
			}
			if (!probeKey.empty())
				ItemProbe::Record(ProbeKey(probeKey, "Label"), true, 0, shown);
			ImGui::TableSetColumnIndex(1);
		}

		const char* GetAssetTypeIcon(AssetType type)
		{
			switch (type)
			{
				case AssetType::Scene:     return Icons::Clapperboard;
				case AssetType::Prefab:    return Icons::Package;
				case AssetType::Model:     return Icons::Shapes;
				case AssetType::Mesh:      return Icons::Box;
				case AssetType::Material:  return Icons::Palette;
				case AssetType::Texture:   return Icons::Image;
				case AssetType::AudioClip: return Icons::AudioLines;
				case AssetType::Font:      return Icons::Type;
				case AssetType::None:      break;
			}
			return Icons::File;
		}

		const char* GetAssetTypeDisplayName(AssetType type)
		{
			switch (type)
			{
				case AssetType::Scene:     return "Scene";
				case AssetType::Prefab:    return "Prefab";
				case AssetType::Model:     return "Model";
				case AssetType::Mesh:      return "Mesh";
				case AssetType::Material:  return "Material";
				case AssetType::Texture:   return "Texture";
				case AssetType::AudioClip: return "Audio Clip";
				case AssetType::Font:      return "Font";
				case AssetType::None:      break;
			}
			return "Asset";
		}

	}

}
