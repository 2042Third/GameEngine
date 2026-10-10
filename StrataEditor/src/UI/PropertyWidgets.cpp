#include "UI/PropertyWidgets.h"

#include "UI/Icons.h"
#include "UI/Widgets.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Math/Math.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <limits>

namespace Strata
{

	namespace
	{

		// The asset picker's list, in text heights (it scales with the UI).
		constexpr float c_AssetPickerWidthInFontSizes = 23.0f;
		constexpr float c_AssetPickerHeightInFontSizes = 20.0f;

		float GetSpeed(const PropertyInfo& property, float fallback)
		{
			return property.Speed > 0.0f ? property.Speed : fallback;
		}

		bool DrawFloats(const PropertyInfo& property, float* values, int count)
		{
			const float speed = GetSpeed(property, 0.05f);
			const float min = property.HasRange() ? property.Min : -FLT_MAX;
			const float max = property.HasRange() ? property.Max : FLT_MAX;
			if (count == 1 && HasFlag(property.Flags, PropertyFlags::Slider) && property.HasRange())
				return ImGui::SliderFloat("##Value", values, min, max, "%.3f");
			return ImGui::DragScalarN("##Value", ImGuiDataType_Float, values, count, speed, &min, &max, "%.3f");
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

		bool DrawAssetPicker(const PropertyInfo& property, AssetHandle& handle)
		{
			bool changed = false;
			const float clearWidth = ImGui::GetFrameHeight();
			const float width = std::max(ImGui::GetContentRegionAvail().x - clearWidth - ImGui::GetStyle().ItemSpacing.x, 1.0f);
			if (ImGui::Button(GetAssetLabel(handle).c_str(), ImVec2(width, 0.0f)))
				ImGui::OpenPopup("AssetPicker");
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(DragDrop::c_Asset))
				{
					const AssetHandle dropped = *static_cast<const AssetHandle*>(payload->Data);
					if (property.AssetFilter == AssetType::None || AssetManager::GetAssetType(dropped) == property.AssetFilter)
					{
						handle = dropped;
						changed = true;
					}
				}
				ImGui::EndDragDropTarget();
			}
			ImGui::SameLine();
			if (ImGui::Button(Icons::X, ImVec2(clearWidth, 0.0f)) && handle.IsValid())
			{
				handle = UUID::Null();
				changed = true;
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Clear");

			if (ImGui::BeginPopup("AssetPicker"))
			{
				static std::string s_Filter;
				if (ImGui::IsWindowAppearing())
				{
					s_Filter.clear();
					ImGui::SetKeyboardFocusHere();
				}
				ImGui::InputTextWithHint("##Filter", "Search", &s_Filter);
				const std::string filter = StringUtils::ToLower(s_Filter);
				if (ImGui::BeginChild("Assets", ImVec2(ImGui::GetFontSize() * c_AssetPickerWidthInFontSizes, ImGui::GetFontSize() * c_AssetPickerHeightInFontSizes)))
				{
					if (AssetManager::HasActive())
					{
						for (const AssetMetadata& metadata : AssetManager::GetActive()->GetAllMetadata(property.AssetFilter))
						{
							const std::string label = metadata.IsBuiltin() || metadata.Path.empty() ? metadata.Name : fmt::format("{}  ({})", metadata.Name, metadata.Path);
							if (!filter.empty() && StringUtils::ToLower(label).find(filter) == std::string::npos)
								continue;
							ImGui::PushID(metadata.Handle.ToString().c_str());
							const bool current = metadata.Handle == handle;
							UI::PushSelectionColors(current);
							const bool picked = ImGui::Selectable(label.c_str(), current);
							UI::PopSelectionColors();
							if (picked)
							{
								handle = metadata.Handle;
								changed = true;
								ImGui::CloseCurrentPopup();
							}
							ImGui::PopID();
						}
					}
				}
				ImGui::EndChild();
				ImGui::EndPopup();
			}
			return changed;
		}

		bool DrawEntityPicker(UUID& id, Scene& scene)
		{
			bool changed = false;
			Entity entity = id.IsValid() ? scene.GetEntityByUUID(id) : Entity();
			const std::string label = !id.IsValid() ? "None" : (entity ? entity.GetName() : fmt::format("Missing ({})", id.ToString()));
			const float clearWidth = ImGui::GetFrameHeight();
			ImGui::Button(label.c_str(), ImVec2(std::max(ImGui::GetContentRegionAvail().x - clearWidth - ImGui::GetStyle().ItemSpacing.x, 1.0f), 0.0f));
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Drag an entity from the hierarchy here");
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(DragDrop::c_Entity))
				{
					id = *static_cast<const UUID*>(payload->Data);
					changed = true;
				}
				ImGui::EndDragDropTarget();
			}
			ImGui::SameLine();
			if (ImGui::Button(Icons::X, ImVec2(clearWidth, 0.0f)) && id.IsValid())
			{
				id = UUID::Null();
				changed = true;
			}
			return changed;
		}

	}

	PropertyEditResult DrawPropertyWidget(const PropertyInfo& property, PropertyValue& value, Scene& scene)
	{
		PropertyEditResult result;
		ImGui::PushID(property.Name.c_str());
		ImGui::SetNextItemWidth(-FLT_MIN);
		switch (property.Type)
		{
			case PropertyType::Bool:
				result.Changed = ImGui::Checkbox("##Value", &std::get<bool>(value));
				break;
			case PropertyType::Int:
			{
				int32_t& number = std::get<int32_t>(value);
				const int32_t min = property.HasRange() ? static_cast<int32_t>(property.Min) : std::numeric_limits<int32_t>::min();
				const int32_t max = property.HasRange() ? static_cast<int32_t>(property.Max) : std::numeric_limits<int32_t>::max();
				result.Changed = ImGui::DragScalar("##Value", ImGuiDataType_S32, &number, GetSpeed(property, 0.2f), &min, &max);
				break;
			}
			case PropertyType::UInt:
			{
				uint32_t& number = std::get<uint32_t>(value);
				const uint32_t min = property.HasRange() ? static_cast<uint32_t>(std::max(property.Min, 0.0f)) : 0u;
				const uint32_t max = property.HasRange() ? static_cast<uint32_t>(property.Max) : std::numeric_limits<uint32_t>::max();
				result.Changed = ImGui::DragScalar("##Value", ImGuiDataType_U32, &number, GetSpeed(property, 0.2f), &min, &max);
				break;
			}
			case PropertyType::Float:
				result.Changed = DrawFloats(property, &std::get<float>(value), 1);
				break;
			case PropertyType::Vec2:
				result.Changed = DrawFloats(property, &std::get<glm::vec2>(value).x, 2);
				break;
			case PropertyType::Vec3:
				result.Changed = DrawFloats(property, &std::get<glm::vec3>(value).x, 3);
				break;
			case PropertyType::Vec4:
				result.Changed = DrawFloats(property, &std::get<glm::vec4>(value).x, 4);
				break;
			case PropertyType::Quat:
			{
				// Euler angles are not unique: converting the quaternion back every frame would make a drag past 90
				// degrees jump. The angles being edited are kept while the rotation is the one they produced.
				glm::quat& rotation = std::get<glm::quat>(value);
				const ImGuiID id = ImGui::GetID("##Value");
				static ImGuiID s_EulerId = 0;
				static glm::quat s_EulerRotation;
				static glm::vec3 s_Euler;
				glm::vec3 euler = s_EulerId == id && s_EulerRotation == rotation ? s_Euler : Math::QuatToEulerDegrees(rotation);
				if (ImGui::DragFloat3("##Value", &euler.x, GetSpeed(property, 0.5f), 0.0f, 0.0f, "%.2f"))
				{
					rotation = Math::EulerDegreesToQuat(euler);
					s_EulerId = id;
					s_EulerRotation = rotation;
					s_Euler = euler;
					result.Changed = true;
				}
				break;
			}
			case PropertyType::Color3:
				result.Changed = ImGui::ColorEdit3("##Value", &std::get<glm::vec3>(value).x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
				break;
			case PropertyType::Color4:
				result.Changed = ImGui::ColorEdit4("##Value", &std::get<glm::vec4>(value).x,
					ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_AlphaPreviewHalf);
				break;
			case PropertyType::String:
			{
				std::string& text = std::get<std::string>(value);
				if (HasFlag(property.Flags, PropertyFlags::MultiLine))
					result.Changed = ImGui::InputTextMultiline("##Value", &text, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.0f));
				else
					result.Changed = ImGui::InputText("##Value", &text);
				break;
			}
			case PropertyType::Enum:
			{
				int32_t& selected = std::get<int32_t>(value);
				const EnumValue* current = property.FindEnumValue(selected);
				if (ImGui::BeginCombo("##Value", current ? current->Name.c_str() : "?"))
				{
					for (const EnumValue& option : property.EnumValues)
					{
						const bool isSelected = option.Value == selected;
						UI::PushSelectionColors(isSelected);
						const bool picked = ImGui::Selectable(option.Name.c_str(), isSelected);
						UI::PopSelectionColors();
						if (picked)
						{
							result.Changed = option.Value != selected;
							selected = option.Value;
						}
					}
					ImGui::EndCombo();
				}
				break;
			}
			case PropertyType::Asset:
				result.Changed = DrawAssetPicker(property, std::get<UUID>(value));
				break;
			case PropertyType::Entity:
				result.Changed = DrawEntityPicker(std::get<UUID>(value), scene);
				break;
		}
		result.Finished = ImGui::IsItemDeactivatedAfterEdit();
		// Discrete widgets (checkboxes, combos, pickers) finish with the change itself.
		const bool continuous = property.Type == PropertyType::Int || property.Type == PropertyType::UInt || property.Type == PropertyType::Float
			|| property.Type == PropertyType::Vec2 || property.Type == PropertyType::Vec3 || property.Type == PropertyType::Vec4
			|| property.Type == PropertyType::Quat || property.Type == PropertyType::Color3 || property.Type == PropertyType::Color4
			|| property.Type == PropertyType::String;
		if (result.Changed && !continuous)
			result.Finished = true;
		ImGui::PopID();
		return result;
	}

}
