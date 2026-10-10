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
		// The axis labels of vector fields, in frame heights.
		constexpr float c_AxisLabelWidthInFrames = 0.8f;
		// Numbers show three decimals: a value that rounds to zero shows as 0, never as -0.000.
		constexpr const char* c_FloatFormat = "%.3f";
		constexpr float c_ShownZero = 0.0005f;
		// What a field shows when the selected entities' values differ.
		constexpr const char* c_MixedText = "--";
		constexpr const char* c_AxisNames[] = { "X", "Y", "Z", "W" };

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

		// An axis label: a tab in the axis's color before its field. Returns true when clicked (resets the component when
		// there is a default to reset to).
		bool AxisLabel(int axis, float width, bool resettable, std::string_view probeKey)
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
			const char* name = c_AxisNames[axis];
			const ImVec2 textSize = ImGui::CalcTextSize(name);
			drawList->AddText(ImVec2(min.x + (width - textSize.x) * 0.5f, min.y + (height - textSize.y) * 0.5f), ImGui::GetColorU32(colors.TextOnAccent), name);
			ImGui::PopFont();
			if (resettable && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				ImGui::SetTooltip("Reset %s", name);
			if (!probeKey.empty())
				UI::ItemProbe::Record(ProbeKey(probeKey, name), resettable);
			return pressed;
		}

		// One field per component behind its axis label. `defaults`: what the labels reset to (null: they do not reset).
		PropertyEditResult DrawAxisFields(float* values, int count, float speed, float min, float max, const PropertyWidgetOptions& options,
			const float* defaults)
		{
			PropertyEditResult result;
			const ImGuiStyle& style = ImGui::GetStyle();
			const float total = std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight() * static_cast<float>(count));
			const float gap = style.ItemInnerSpacing.x;
			const float slot = (total - gap * static_cast<float>(count - 1)) / static_cast<float>(count);
			const float labelWidth = std::min(std::round(ImGui::GetFrameHeight() * c_AxisLabelWidthInFrames), slot * 0.5f);
			for (int axis = 0; axis < count; axis++)
			{
				if (axis > 0)
					ImGui::SameLine(0.0f, gap);
				const bool mixed = options.MixedComponents[static_cast<size_t>(axis)];
				// Reset: also when the value shown is the default already but other entities' values differ.
				if (AxisLabel(axis, labelWidth, defaults != nullptr, options.ProbeKey) && (values[axis] != defaults[axis] || mixed))
				{
					values[axis] = defaults[axis];
					result.Changed = true;
					result.Finished = true;
					result.ChangedComponents[static_cast<size_t>(axis)] = true;
				}
				ImGui::SameLine(0.0f, 0.0f);
				ImGui::PushID(axis);
				ImGui::SetNextItemWidth(std::max(slot - labelWidth, 1.0f));
				float shown = ToShown(values[axis]);
				// Square left corners against the label.
				ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
				if (ImGui::DragScalar("##Value", ImGuiDataType_Float, &shown, speed, &min, &max, mixed ? c_MixedText : c_FloatFormat))
				{
					values[axis] = shown;
					result.Changed = true;
					result.ChangedComponents[static_cast<size_t>(axis)] = true;
				}
				ImGui::PopStyleVar();
				result.Finished |= ImGui::IsItemDeactivatedAfterEdit();
				ImGui::PopID();
			}
			if (!options.ProbeKey.empty())
				UI::ItemProbe::Record(options.ProbeKey);
			return result;
		}

		PropertyEditResult DrawFloat(const PropertyInfo& property, float& value, const PropertyWidgetOptions& options)
		{
			PropertyEditResult result;
			const float min = property.HasRange() ? property.Min : -FLT_MAX;
			const float max = property.HasRange() ? property.Max : FLT_MAX;
			const char* format = options.Mixed ? c_MixedText : c_FloatFormat;
			float shown = ToShown(value);
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (HasFlag(property.Flags, PropertyFlags::Slider) && property.HasRange())
				result.Changed = ImGui::SliderFloat("##Value", &shown, min, max, format);
			else
				result.Changed = ImGui::DragScalar("##Value", ImGuiDataType_Float, &shown, GetSpeed(property, 0.05f), &min, &max, format);
			if (result.Changed)
				value = shown;
			result.Finished = ImGui::IsItemDeactivatedAfterEdit();
			if (!options.ProbeKey.empty())
				UI::ItemProbe::Record(options.ProbeKey);
			return result;
		}

		// Euler angles are not unique: converting the quaternion back every frame would make a drag past 90 degrees jump. The
		// angles being edited are kept while the rotation is the one they produced.
		PropertyEditResult DrawRotation(const PropertyInfo& property, glm::quat& rotation, const PropertyWidgetOptions& options)
		{
			static ImGuiID s_EulerId = 0;
			static glm::quat s_EulerRotation;
			static glm::vec3 s_Euler;
			const ImGuiID id = ImGui::GetID("##Euler");
			glm::vec3 euler = s_EulerId == id && s_EulerRotation == rotation ? s_Euler : Math::QuatToEulerDegrees(rotation);
			std::optional<glm::vec3> defaults;
			if (options.DefaultValue && std::holds_alternative<glm::quat>(*options.DefaultValue))
				defaults = Math::QuatToEulerDegrees(std::get<glm::quat>(*options.DefaultValue));
			const PropertyEditResult result = DrawAxisFields(&euler.x, 3, GetSpeed(property, 0.5f), -FLT_MAX, FLT_MAX, options,
				defaults ? &defaults->x : nullptr);
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
				result = DrawAxisFields(components, count, GetSpeed(property, 0.05f), min, max, options, defaults);
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
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::BeginCombo("##Value", options.Mixed ? c_MixedText : (current ? current->Name.c_str() : "?")))
				{
					for (const EnumValue& option : property.EnumValues)
					{
						const bool isSelected = option.Value == selected && !options.Mixed;
						UI::PushSelectionColors(isSelected);
						const bool picked = ImGui::Selectable(option.Name.c_str(), isSelected);
						UI::PopSelectionColors();
						if (picked)
						{
							result.Changed = option.Value != selected || options.Mixed;
							selected = option.Value;
						}
					}
					ImGui::EndCombo();
				}
				if (!options.ProbeKey.empty())
					UI::ItemProbe::Record(options.ProbeKey);
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
			if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
				return false;
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.38f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.62f);
			return true;
		}

		void EndPropertyTable()
		{
			ImGui::EndTable();
		}

		void PropertyLabel(std::string_view label, std::string_view tooltip)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::PushStyleColor(ImGuiCol_Text, GetThemeColors().TextSecondary);
			ImGui::TextUnformatted(label.data(), label.data() + label.size());
			ImGui::PopStyleColor();
			if (!tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				ImGui::SetTooltip("%.*s", static_cast<int>(tooltip.size()), tooltip.data());
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

	}

}
