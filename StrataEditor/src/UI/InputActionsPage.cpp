#include "UI/InputActionsPage.h"

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "UI/EditorFonts.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/PropertyWidgets.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Core/ProjectSettings.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Input/InputNames.h>
#include <Strata/Project/Project.h>

#include <imgui.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace Strata
{

	namespace
	{

		// The widest the binding popup's lists get, in text heights.
		constexpr float c_PopupWidthInFontSizes = 16.0f;

		std::string ProbeKey(std::string_view item)
		{
			return std::string(InputActionsPage::c_ProbePrefix) + std::string(item);
		}

		void Caption(const std::string& text, const ImVec4& color, float width)
		{
			UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Caption);
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
			ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			ImGui::PopFont();
		}

		// The controls a binding or a composite's part can be, as binding texts in the order InputNames lists them: keys, mouse
		// buttons, gamepad buttons and half axes (what Buttons and composites take), whole axes (Axis actions) and sticks.
		struct ControlLists
		{
			std::vector<std::string> ButtonLike;
			std::vector<std::string> AxisControls; // ButtonLike and the whole axes
			std::vector<std::string> Sticks;
		};

		const ControlLists& GetControlLists()
		{
			static const ControlLists s_Lists = []()
			{
				ControlLists lists;
				for (const char* key : InputNames::GetKeyNames())
					lists.ButtonLike.push_back(std::string("Key/") + key);
				for (const char* button : InputNames::GetMouseButtonNames())
					lists.ButtonLike.push_back(std::string("Mouse/") + button);
				for (const char* button : InputNames::GetGamepadButtonNames())
					lists.ButtonLike.push_back(std::string("Gamepad/") + button);
				std::vector<std::string> wholeAxes;
				for (uint32_t axis = 0; axis < c_GamepadAxisCount; axis++)
				{
					const std::string name = std::string("Gamepad/") + InputNames::GetGamepadAxisName(static_cast<GamepadAxis>(axis));
					lists.ButtonLike.push_back(name + "+");
					if (GetGamepadAxisMinimum(static_cast<GamepadAxis>(axis)) < 0.0f)
						lists.ButtonLike.push_back(name + "-");
					wholeAxes.push_back(name);
				}
				lists.AxisControls = lists.ButtonLike;
				lists.AxisControls.insert(lists.AxisControls.end(), wholeAxes.begin(), wholeAxes.end());
				lists.Sticks = { "Gamepad/LeftStick", "Gamepad/RightStick" };
				return lists;
			}();
			return s_Lists;
		}

		// The controls a part of a binding of this shape can be, for an action of this kind.
		const std::vector<std::string>& GetPartControls(InputActionKind kind, InputBinding::Shape shape)
		{
			const ControlLists& lists = GetControlLists();
			if (shape != InputBinding::Shape::Control || kind == InputActionKind::Button)
				return lists.ButtonLike;
			return kind == InputActionKind::Axis ? lists.AxisControls : lists.Sticks;
		}

		const char* GetShapeName(InputBinding::Shape shape)
		{
			switch (shape)
			{
				case InputBinding::Shape::Control: return "Control";
				case InputBinding::Shape::Composite1D: return "1D";
				case InputBinding::Shape::Composite2D: return "2D";
			}
			return "Control";
		}

		const char* GetDeviceIcon(const InputBinding& binding)
		{
			switch (binding.Controls[0].Type)
			{
				case InputControlType::Key: return Icons::Keyboard;
				case InputControlType::MouseButton: return Icons::Mouse;
				default: return Icons::Gamepad;
			}
		}

		// A list of binding texts to choose one from, with a probe for the list and each entry it shows. Long lists are clipped
		// to what shows.
		std::optional<std::string> ControlCombo(const std::string& key, const std::string& current, const std::vector<std::string>& options, float width)
		{
			const ImVec2 min = ImGui::GetCursorScreenPos();
			ImGui::SetNextItemWidth(width);
			const bool open = ImGui::BeginCombo(("##" + key).c_str(), current.c_str(), ImGuiComboFlags_HeightLarge);
			// Its own rectangle: while the list is open, its entries come after it.
			UI::ItemProbe::RecordRect(key, min, ImVec2(min.x + width, min.y + ImGui::GetFrameHeight()), true, 0, current);
			if (!open)
				return std::nullopt;
			std::optional<std::string> picked;
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(options.size()));
			while (clipper.Step())
			{
				for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; index++)
				{
					const std::string& option = options[static_cast<size_t>(index)];
					const bool selected = option == current;
					UI::PushSelectionColors(selected);
					if (ImGui::Selectable(option.c_str(), selected))
						picked = option;
					UI::PopSelectionColors();
					UI::ItemProbe::Record(key + "." + option, true, 0, option);
				}
			}
			ImGui::EndCombo();
			return picked;
		}

		// A name for something new that `taken` does not refuse: "<base>", else "<base> 2" and so on.
		template<typename Taken>
		std::string MakeNewName(std::string_view base, size_t limit, const Taken& taken)
		{
			for (size_t number = 1; number <= limit + 1; number++)
			{
				std::string name = number == 1 ? std::string(base) : fmt::format("{} {}", base, number);
				if (!taken(name))
					return name;
			}
			return std::string(base);
		}

	}

	void InputActionsPage::Refresh(const EditorContext& context)
	{
		const ProjectSettingsValues& values = context.GetProject()->GetSettings();
		if (m_Read && values.GetVersion() == m_SettingsVersion)
			return;
		m_Read = true;
		m_SettingsVersion = values.GetVersion();
		std::optional<InputActionSet> set;
		if (const nlohmann::json* value = values.FindSection(InputActionSet::c_SectionId))
			set = InputActionSet::FromJson(*value);
		m_Set = set ? std::move(*set) : InputActionSet::MakeDefault();
		// The selection follows the map by name; a game's first map, else the UI map, when it is gone.
		if (!FindSelectedMap())
		{
			m_SelectedMap = m_Set.Maps.front().Name;
			for (const InputActionMap& map : m_Set.Maps)
			{
				if (!StringUtils::EqualsIgnoreCase(map.Name, InputActionSet::c_UIMapName))
				{
					m_SelectedMap = map.Name;
					break;
				}
			}
		}
	}

	const InputActionMap* InputActionsPage::FindSelectedMap() const
	{
		const std::optional<uint32_t> index = m_Set.FindMap(m_SelectedMap);
		return index ? &m_Set.Maps[*index] : nullptr;
	}

	bool InputActionsPage::Run(EditorPanelContext& context, const char* command, const nlohmann::json& parameters)
	{
		// While the project is not ready (its assets scanned, its start scene opened, a 0.5.0 project migrated) the command would
		// wait, and nothing on a page runs it later: it is refused, saying why, as RunEditorCommand refuses what the UI runs.
		if (const std::optional<std::string> waiting = context.Commands.DescribeAssetScanWait(context.Context, command))
		{
			m_Error = fmt::format("Not yet: {}", *waiting);
			return false;
		}
		const EditorCommandResult result = context.Commands.Execute(context.Context, command, parameters, CommandOrigin { CommandSource::UI });
		m_Error = result.Success ? std::string() : result.Error;
		return result.Success;
	}

	std::optional<std::string> InputActionsPage::NameField(const std::string& key, const std::string& name, bool readOnly)
	{
		std::string local = name;
		std::string* text = m_EditKey == key ? &m_EditText : &local;
		const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | (readOnly ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None);
		const bool entered = ImGui::InputTextWithHint("##Name", "Name", text, flags);
		UI::ItemProbe::Record(key, !readOnly, 0, *text);
		if (ImGui::IsItemActivated())
		{
			m_EditKey = key;
			m_EditText = *text;
		}
		const std::string typed = *text;
		const bool commit = !readOnly && (entered || ImGui::IsItemDeactivatedAfterEdit()) && typed != name;
		if (!ImGui::IsItemActive() && m_EditKey == key)
			m_EditKey.clear();
		if (!commit)
			return std::nullopt;
		return typed;
	}

	void InputActionsPage::Draw(EditorPanelContext& context, float width)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		if (!context.Context.HasProject())
			return;
		Refresh(context.Context);
		Caption("The game's named controls: scripts read actions by name (\"<Map>/<Action>\", or a name no other map has) and players can rebind "
			"them. A Button is down or up, an Axis is one value from -1 to 1, a Vector2 two (+Y up). The map UI holds the actions the game UI "
			"navigates with.", colors.TextSecondary, width);
		ImGui::Spacing();
		const std::string summary = fmt::format("{} {} \xC2\xB7 {} {}", m_Set.Maps.size(), m_Set.Maps.size() == 1 ? "map" : "maps", m_Set.GetActionCount(),
			m_Set.GetActionCount() == 1 ? "action" : "actions");
		ImGui::TextUnformatted(summary.c_str());
		UI::ItemProbe::Record(ProbeKey("Summary"), true, 0, summary);
		ImGui::Spacing();
		// Each part runs at most one command, which replaces the project's settings: the next frame draws the new set.
		DrawMaps(context);
		ImGui::Spacing();
		DrawActions(context);
		if (!m_Error.empty())
		{
			Caption(m_Error, colors.Error, width);
			UI::ItemProbe::Record(ProbeKey("Error"), true, 0, m_Error);
		}
	}

	void InputActionsPage::DrawMaps(EditorPanelContext& context)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		// Copies: a command run below replaces the set.
		const InputActionSet set = m_Set;
		UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Caption);
		ImGui::TextColored(colors.TextSecondary, "Maps");
		ImGui::PopFont();
		for (uint32_t index = 0; index < set.Maps.size(); index++)
		{
			const InputActionMap& map = set.Maps[index];
			const bool selected = StringUtils::EqualsIgnoreCase(map.Name, m_SelectedMap);
			const bool reserved = StringUtils::EqualsIgnoreCase(map.Name, InputActionSet::c_UIMapName);
			std::string label = map.Name;
			if (reserved)
				label += "  \xC2\xB7  game UI navigation";
			if (!map.Enabled)
				label += "  \xC2\xB7  off at start";
			ImGui::PushID(static_cast<int>(index));
			UI::PushSelectionColors(selected);
			if (ImGui::Selectable(label.c_str(), selected))
				m_SelectedMap = map.Name;
			UI::PopSelectionColors();
			UI::ItemProbe::Record(ProbeKey(fmt::format("Map.{}", index)), true, 0, map.Name);
			ImGui::PopID();
		}
		UI::ButtonStyle addStyle;
		addStyle.Enabled = set.Maps.size() < InputActionSet::c_MaxMaps;
		const std::string addKey = ProbeKey("AddMap");
		if (UI::ToolbarButton(addKey.c_str(), Icons::Plus, "Add Map", "Add a map of actions (a set the game enables and disables together)", addStyle))
		{
			const std::string name = MakeNewName("New Map", InputActionSet::c_MaxMaps, [&set](const std::string& candidate) { return set.FindMap(candidate).has_value(); });
			if (Run(context, "inputActions.setMap", { { "map", name } }))
				m_SelectedMap = name;
			return;
		}

		const InputActionMap* map = FindSelectedMap();
		if (!map)
			return;
		const std::string mapName = map->Name;
		const bool mapEnabled = map->Enabled;
		const bool reserved = StringUtils::EqualsIgnoreCase(mapName, InputActionSet::c_UIMapName);
		const ImGuiStyle& style = ImGui::GetStyle();
		std::optional<nlohmann::json> edit;
		if (UI::BeginPropertyTable("InputActionMap"))
		{
			const std::string nameKey = ProbeKey("MapName");
			UI::PropertyLabel("Map", reserved ? "The game UI reads this map's actions by name: it keeps its name" : "The map's name: scripts name actions <Map>/<Action>",
				nameKey);
			ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - style.ItemSpacing.x, ImGui::GetFrameHeight()));
			if (const std::optional<std::string> renamed = NameField(nameKey, mapName, reserved))
				edit = nlohmann::json { { "map", mapName }, { "rename", *renamed } };
			ImGui::SameLine();
			const std::string removeKey = ProbeKey("RemoveMap");
			if (UI::IconButton(removeKey.c_str(), Icons::Trash, reserved ? "The game UI navigates with the map UI: it stays" : "Remove the map and its actions", !reserved))
				edit = nlohmann::json { { "map", mapName }, { "remove", true } };

			const std::string enabledKey = ProbeKey("MapEnabled");
			UI::PropertyLabel("Enabled at start", "Whether the map's actions work when the game starts; scripts enable and disable maps while it runs",
				enabledKey);
			bool enabled = mapEnabled;
			if (ImGui::Checkbox("##Enabled", &enabled))
				edit = nlohmann::json { { "map", mapName }, { "enabled", enabled } };
			UI::ItemProbe::Record(enabledKey, true, 0, enabled ? "on" : "off");
			UI::EndPropertyTable();
		}
		if (edit && Run(context, "inputActions.setMap", *edit) && edit->contains("rename"))
			m_SelectedMap = (*edit)["rename"].get<std::string>();
	}

	void InputActionsPage::DrawActions(EditorPanelContext& context)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const InputActionMap* selected = FindSelectedMap();
		if (!selected)
			return;
		// Copies: a command run below replaces the set.
		const InputActionMap map = *selected;
		const std::optional<uint32_t> mapIndex = m_Set.FindMap(map.Name);
		const ImGuiStyle& style = ImGui::GetStyle();
		UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Caption);
		ImGui::TextColored(colors.TextSecondary, "Actions of %s", map.Name.c_str());
		ImGui::PopFont();
		std::optional<nlohmann::json> edit;
		for (uint32_t index = 0; index < map.Actions.size() && !edit; index++)
		{
			const InputAction& action = map.Actions[index];
			const std::string qualified = fmt::format("{}/{}", map.Name, action.Name);
			const bool reserved = mapIndex && m_Set.IsReservedAction({ *mapIndex, index });
			const std::string prefix = fmt::format("Action.{}.", index);
			ImGui::PushID(static_cast<int>(index));
			if (UI::BeginPropertyTable("InputAction"))
			{
				const std::string nameKey = ProbeKey(prefix + "Name");
				UI::PropertyLabel("Action", reserved ? "The game UI navigates with this action and reads it by name: it keeps its name and kind"
					: "Unique in its map; scripts read it as <Map>/<Action>", nameKey);
				ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - style.ItemSpacing.x, ImGui::GetFrameHeight()));
				if (const std::optional<std::string> renamed = NameField(nameKey, action.Name, reserved))
					edit = nlohmann::json { { "action", qualified }, { "rename", *renamed } };
				ImGui::SameLine();
				const std::string removeKey = ProbeKey(prefix + "Remove");
				if (UI::IconButton(removeKey.c_str(), Icons::Trash, reserved ? "The game UI navigates with this action: it stays" : "Remove the action", !reserved))
					edit = nlohmann::json { { "action", qualified }, { "remove", true } };
				if (reserved)
				{
					UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Caption);
					ImGui::TextColored(colors.TextSecondary, "Used by game UI navigation");
					ImGui::PopFont();
					UI::ItemProbe::Record(ProbeKey(prefix + "Reserved"), true, 0, "Used by game UI navigation");
				}

				const std::string kindKey = ProbeKey(prefix + "Kind");
				UI::PropertyLabel("Kind", "Button: down or up. Axis: one value from -1 to 1. Vector2: two values, +Y up", kindKey);
				ImGui::BeginDisabled(reserved);
				const ImVec2 kindMin = ImGui::GetCursorScreenPos();
				const float kindWidth = ImGui::GetContentRegionAvail().x;
				ImGui::SetNextItemWidth(kindWidth);
				const bool open = ImGui::BeginCombo("##Kind", InputActionKindToString(action.Kind));
				UI::ItemProbe::RecordRect(kindKey, kindMin, ImVec2(kindMin.x + kindWidth, kindMin.y + ImGui::GetFrameHeight()), !reserved, 0,
					InputActionKindToString(action.Kind));
				if (open)
				{
					for (const InputActionKind kind : { InputActionKind::Button, InputActionKind::Axis, InputActionKind::Vector2 })
					{
						UI::PushSelectionColors(kind == action.Kind);
						if (ImGui::Selectable(InputActionKindToString(kind), kind == action.Kind) && kind != action.Kind)
						{
							// The bindings that fit the new kind stay; the others go with the change.
							nlohmann::json bindings = nlohmann::json::array();
							for (const InputBinding& binding : action.Bindings)
							{
								if (binding.FitsKind(kind))
									bindings.push_back(binding.ToString());
							}
							edit = nlohmann::json { { "action", qualified }, { "kind", InputActionKindToString(kind) }, { "bindings", std::move(bindings) } };
						}
						UI::PopSelectionColors();
						UI::ItemProbe::Record(kindKey + "." + InputActionKindToString(kind), true, 0, InputActionKindToString(kind));
					}
					ImGui::EndCombo();
				}
				ImGui::EndDisabled();

				if (action.Kind != InputActionKind::Button)
				{
					const std::string zoneKey = ProbeKey(prefix + "DeadZone");
					UI::PropertyLabel("Dead zone", "Values within it read 0; the rest is rescaled to reach 1 (round for sticks)", zoneKey);
					float zone = m_EditKey == zoneKey ? m_EditDeadZone : action.DeadZone;
					ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
					ImGui::SliderFloat("##DeadZone", &zone, 0.0f, static_cast<float>(InputActionSet::c_MaxDeadZone), "%.2f", ImGuiSliderFlags_AlwaysClamp);
					UI::ItemProbe::Record(zoneKey, true, 0, fmt::format("{:.2f}", zone));
					if (ImGui::IsItemActive())
					{
						m_EditKey = zoneKey;
						m_EditDeadZone = zone;
					}
					if (ImGui::IsItemDeactivatedAfterEdit())
					{
						m_EditKey.clear();
						edit = nlohmann::json { { "action", qualified }, { "deadZone", zone } };
					}
				}

				UI::PropertyLabel("Bindings", "The controls that drive the action (players can rebind them)", ProbeKey(prefix + "Bindings"));
				if (!edit)
					DrawBindings(context, index, ImGui::GetContentRegionAvail().x);
				UI::EndPropertyTable();
			}
			ImGui::PopID();
			ImGui::Spacing();
		}
		if (edit)
		{
			Run(context, "inputActions.setAction", *edit);
			return;
		}
		UI::ButtonStyle addStyle;
		addStyle.Enabled = m_Set.GetActionCount() < InputActionSet::c_MaxActions;
		const std::string addKey = ProbeKey("AddAction");
		if (UI::ToolbarButton(addKey.c_str(), Icons::Plus, "Add Action", "Add a Button action to this map", addStyle))
		{
			const std::string name = MakeNewName("New Action", InputActionSet::c_MaxActions, [&map](const std::string& candidate)
			{
				return std::any_of(map.Actions.begin(), map.Actions.end(), [&candidate](const InputAction& action) { return StringUtils::EqualsIgnoreCase(action.Name, candidate); });
			});
			Run(context, "inputActions.setAction", { { "action", fmt::format("{}/{}", map.Name, name) } });
		}
	}

	void InputActionsPage::DrawBindings(EditorPanelContext& context, uint32_t action, float width)
	{
		const InputActionMap* map = FindSelectedMap();
		if (!map || action >= map->Actions.size())
			return;
		// Copies: a command run below replaces the set.
		const InputAction current = map->Actions[action];
		const std::string qualified = fmt::format("{}/{}", map->Name, current.Name);
		const std::string prefix = fmt::format("Action.{}.", action);
		const ImGuiStyle& style = ImGui::GetStyle();
		std::optional<size_t> removed;
		float used = 0.0f;
		for (size_t index = 0; index < current.Bindings.size(); index++)
		{
			const InputBinding& binding = current.Bindings[index];
			const std::string text = binding.ToString();
			const char* icon = GetDeviceIcon(binding);
			const float chipWidth = UI::ChipWidth(icon, text.c_str()) + style.ItemSpacing.x + ImGui::GetFrameHeight();
			if (index > 0 && used + style.ItemSpacing.x + chipWidth <= width)
			{
				ImGui::SameLine();
				used += style.ItemSpacing.x;
			}
			else
			{
				used = 0.0f;
			}
			ImGui::PushID(static_cast<int>(index));
			const std::string chipKey = ProbeKey(fmt::format("{}Binding.{}", prefix, index));
			UI::Chip(chipKey.c_str(), icon, text.c_str(), "A control bound to the action");
			ImGui::SameLine();
			const std::string removeKey = ProbeKey(fmt::format("{}RemoveBinding.{}", prefix, index));
			if (UI::IconButton(removeKey.c_str(), Icons::X, "Remove this binding"))
				removed = index;
			ImGui::PopID();
			used += chipWidth;
		}
		const bool full = current.Bindings.size() >= InputActionSet::c_MaxBindings;
		if (!current.Bindings.empty() && used + style.ItemSpacing.x + ImGui::GetFrameHeight() <= width)
			ImGui::SameLine();
		const std::string addKey = ProbeKey(prefix + "AddBinding");
		if (UI::IconButton(addKey.c_str(), Icons::Plus, full ? "An action has at most 8 bindings" : "Add a binding", !full))
		{
			m_AddAction = qualified;
			m_AddShape = InputBinding::Shape::Control;
			m_AddParts[0] = GetPartControls(current.Kind, m_AddShape).front();
			for (size_t part = 1; part < m_AddParts.size(); part++)
				m_AddParts[part] = GetControlLists().ButtonLike.front();
			ImGui::OpenPopup("AddBinding");
		}
		DrawAddBinding(context, action);
		if (removed)
		{
			nlohmann::json bindings = nlohmann::json::array();
			for (size_t index = 0; index < current.Bindings.size(); index++)
			{
				if (index != *removed)
					bindings.push_back(current.Bindings[index].ToString());
			}
			Run(context, "inputActions.setAction", { { "action", qualified }, { "bindings", std::move(bindings) } });
		}
	}

	void InputActionsPage::DrawAddBinding(EditorPanelContext& context, uint32_t action)
	{
		if (!ImGui::BeginPopup("AddBinding"))
			return;
		const InputActionMap* map = FindSelectedMap();
		if (!map || action >= map->Actions.size() || !StringUtils::EqualsIgnoreCase(fmt::format("{}/{}", map->Name, map->Actions[action].Name), m_AddAction))
		{
			// The action changed under the popup (an undo, another client): there is nothing to add to.
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		const InputAction current = map->Actions[action];
		const float width = ImGui::GetFontSize() * c_PopupWidthInFontSizes;

		// The shapes this kind takes: one control (a stick for a Vector2), or a composite put together part by part.
		std::vector<InputBinding::Shape> shapes = { InputBinding::Shape::Control };
		if (current.Kind == InputActionKind::Axis)
			shapes.push_back(InputBinding::Shape::Composite1D);
		else if (current.Kind == InputActionKind::Vector2)
			shapes.push_back(InputBinding::Shape::Composite2D);
		if (shapes.size() > 1)
		{
			ImGui::TextUnformatted("Shape");
			const std::string shapeKey = ProbeKey("AddBinding.Shape");
			const ImVec2 min = ImGui::GetCursorScreenPos();
			ImGui::SetNextItemWidth(width);
			const bool open = ImGui::BeginCombo("##Shape", GetShapeName(m_AddShape));
			UI::ItemProbe::RecordRect(shapeKey, min, ImVec2(min.x + width, min.y + ImGui::GetFrameHeight()), true, 0, GetShapeName(m_AddShape));
			if (open)
			{
				for (const InputBinding::Shape shape : shapes)
				{
					UI::PushSelectionColors(shape == m_AddShape);
					if (ImGui::Selectable(GetShapeName(shape), shape == m_AddShape) && shape != m_AddShape)
					{
						m_AddShape = shape;
						m_AddParts[0] = GetPartControls(current.Kind, shape).front();
					}
					UI::PopSelectionColors();
					UI::ItemProbe::Record(shapeKey + "." + GetShapeName(shape), true, 0, GetShapeName(shape));
				}
				ImGui::EndCombo();
			}
		}

		InputBinding parts;
		parts.Type = m_AddShape;
		static constexpr const char* c_1DParts[] = { "Negative", "Positive" };
		static constexpr const char* c_2DParts[] = { "Up", "Down", "Left", "Right" };
		for (uint32_t part = 0; part < parts.GetControlCount(); part++)
		{
			const char* label = m_AddShape == InputBinding::Shape::Composite1D ? c_1DParts[part]
				: (m_AddShape == InputBinding::Shape::Composite2D ? c_2DParts[part] : (current.Kind == InputActionKind::Vector2 ? "Stick" : "Control"));
			ImGui::TextUnformatted(label);
			ImGui::PushID(static_cast<int>(part));
			if (std::optional<std::string> picked = ControlCombo(ProbeKey(fmt::format("AddBinding.Part.{}", part)), m_AddParts[part],
				GetPartControls(current.Kind, m_AddShape), width))
			{
				m_AddParts[part] = std::move(*picked);
			}
			ImGui::PopID();
		}

		std::string text;
		if (m_AddShape != InputBinding::Shape::Control)
			text = m_AddShape == InputBinding::Shape::Composite1D ? "1D:" : "2D:";
		for (uint32_t part = 0; part < parts.GetControlCount(); part++)
			text += (part > 0 ? "," : "") + m_AddParts[part];
		ImGui::Spacing();
		UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
		ImGui::TextUnformatted(text.c_str());
		ImGui::PopFont();
		const std::string addKey = ProbeKey("AddBinding.Add");
		if (UI::DialogButton(addKey.c_str(), "Add", true))
		{
			nlohmann::json bindings = nlohmann::json::array();
			for (const InputBinding& binding : current.Bindings)
				bindings.push_back(binding.ToString());
			bindings.push_back(text);
			if (Run(context, "inputActions.setAction", { { "action", m_AddAction }, { "bindings", std::move(bindings) } }))
				ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

}
