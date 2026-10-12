#include "UI/AnimationAssetViews.h"

#include "Editor/AssetEdit.h"
#include "Editor/EditorContext.h"
#include "UI/EditorFonts.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/PropertyWidgets.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Animation/AnimationClip.h>
#include <Strata/Animation/Skeleton.h>
#include <Strata/Asset/AssetManager.h>
#include <Strata/Core/StringUtils.h>

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <span>

namespace Strata
{

	namespace
	{

		// At most this many rows of a list show at once; longer lists scroll.
		constexpr float c_MaxListRows = 14.0f;

		std::string_view Trim(std::string_view text)
		{
			while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
				text.remove_prefix(1);
			while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
				text.remove_suffix(1);
			return text;
		}

		std::vector<std::string_view> SplitList(std::string_view text)
		{
			std::vector<std::string_view> parts;
			size_t start = 0;
			while (start <= text.size())
			{
				const size_t end = std::min(text.find(',', start), text.size());
				const std::string_view part = Trim(text.substr(start, end - start));
				if (!part.empty())
					parts.push_back(part);
				start = end + 1;
			}
			return parts;
		}

		bool ParseNumber(std::string_view text, float& outValue)
		{
			text = Trim(text);
			if (text.empty())
				return false;
			float value = 0.0f;
			const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
			if (error != std::errc() || end != text.data() + text.size() || !std::isfinite(value))
				return false;
			outValue = value;
			return true;
		}

		std::string FormatNumber(float value)
		{
			std::string text = fmt::format("{:.3f}", value);
			while (!text.empty() && text.back() == '0')
				text.pop_back();
			if (!text.empty() && text.back() == '.')
				text.pop_back();
			return text == "-0" ? std::string("0") : text;
		}

		PropertyInfo MakeProperty(std::string name, PropertyType type, std::string tooltip = {})
		{
			PropertyInfo property;
			property.Name = name;
			property.DisplayName = std::move(name);
			property.Type = type;
			property.Tooltip = std::move(tooltip);
			return property;
		}

		// A cell's widget: a property widget filling the cell, recorded under `probeKey`.
		PropertyEditResult Cell(const PropertyInfo& property, PropertyValue& value, Scene& scene, const std::string& probeKey)
		{
			PropertyWidgetOptions options;
			options.ProbeKey = probeKey;
			return DrawPropertyWidget(property, value, scene, options);
		}

		void BeginListChild(const char* id, size_t rows)
		{
			const float rowHeight = ImGui::GetFrameHeightWithSpacing();
			const float height = rowHeight * std::min(static_cast<float>(rows) + 1.0f, c_MaxListRows);
			ImGui::BeginChild(id, ImVec2(0.0f, height), ImGuiChildFlags_Borders);
		}

		// A column of an editable table: a field whose least width (in font sizes) keeps its content legible and is also its
		// share of a wider table, or a checkbox or icon button (a square of the frame's height).
		struct EditColumn
		{
			const char* Name = "";
			float Width = 0.0f;
			bool Square = false;
		};

		// The least width of a column: a field's from its font sizes; a square's the frame's height, or its header's width
		// when that is wider ("Loop" over a checkbox), so that a header never runs into the next one.
		float GetLeastColumnWidth(const EditColumn& column, float fontSize, float square)
		{
			if (!column.Square)
				return column.Width * fontSize;
			return std::max(square, ImGui::CalcTextSize(column.Name, nullptr, true).x);
		}

		// Begins an editable table (and draws its header) whose fields never get narrower than their least widths: in a
		// narrower panel the table scrolls sideways rather than squeezing names and numbers into ellipses.
		bool BeginEditTable(const char* id, std::span<const EditColumn> columns, size_t rows)
		{
			const ImGuiStyle& style = ImGui::GetStyle();
			const float fontSize = ImGui::GetFontSize();
			const float square = ImGui::GetFrameHeight();
			float leastWidth = 0.0f;
			for (const EditColumn& column : columns)
				leastWidth += GetLeastColumnWidth(column, fontSize, square) + style.CellPadding.x * 2.0f;
			ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg;
			ImVec2 outerSize(0.0f, 0.0f);
			float innerWidth = 0.0f;
			if (ImGui::GetContentRegionAvail().x < leastWidth)
			{
				// A table that scrolls is a window of its own, as tall as its header, its rows of fields and its scrollbar.
				flags |= ImGuiTableFlags_ScrollX;
				const float header = ImGui::GetTextLineHeight() + style.CellPadding.y * 2.0f;
				const float row = square + style.CellPadding.y * 2.0f;
				outerSize.y = header + row * static_cast<float>(rows) + style.ScrollbarSize + style.ItemSpacing.y;
				innerWidth = leastWidth;
			}
			if (!ImGui::BeginTable(id, static_cast<int>(columns.size()), flags, outerSize, innerWidth))
				return false;
			// Stretch weights are the least widths: a table at its least width gives every field exactly its own.
			for (const EditColumn& column : columns)
			{
				if (column.Square)
					ImGui::TableSetupColumn(column.Name, ImGuiTableColumnFlags_WidthFixed, GetLeastColumnWidth(column, fontSize, square));
				else
					ImGui::TableSetupColumn(column.Name, ImGuiTableColumnFlags_WidthStretch, column.Width);
			}
			ImGui::TableHeadersRow();
			return true;
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// Skeletons and clips
	////////////////////////////////////////////////////////////////////////////////

	void AnimationAssetViews::DrawSkeleton(AssetHandle handle)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const Ref<Skeleton> skeleton = AssetManager::GetAsset<Skeleton>(handle);
		if (!skeleton)
		{
			ImGui::TextColored(colors.TextSecondary, "Loading the skeleton...");
			return;
		}
		const uint32_t joints = skeleton->GetJointCount();
		ImGui::TextColored(colors.TextSecondary, "%u joints \xC2\xB7 root motion from %s", joints, skeleton->GetJointNames()[skeleton->GetRootMotionJoint()].c_str());
		UI::ItemProbe::Record("AssetInspector.Skeleton.Summary", true, 0, std::to_string(joints));

		// Depths, once per skeleton (parents come before their children).
		if (m_DepthSkeleton.lock() != skeleton)
		{
			m_DepthSkeleton = skeleton;
			m_Depths.assign(joints, 0);
			for (uint32_t joint = 0; joint < joints; joint++)
			{
				const JointIndex parent = skeleton->GetParents()[joint];
				m_Depths[joint] = parent == c_NoJoint ? 0 : m_Depths[parent] + 1;
			}
		}

		BeginListChild("AssetInspector.Skeleton.Joints", joints);
		const float indent = ImGui::GetStyle().IndentSpacing * 0.75f;
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(joints));
		while (clipper.Step())
		{
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
			{
				const uint32_t joint = static_cast<uint32_t>(row);
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent * static_cast<float>(m_Depths[joint]));
				ImGui::TextColored(joint == skeleton->GetRootMotionJoint() ? colors.Accent : colors.TextSecondary, "%s", Icons::Bone);
				ImGui::SameLine();
				ImGui::TextUnformatted(skeleton->GetJointNames()[joint].c_str());
			}
		}
		ImGui::EndChild();
	}

	void AnimationAssetViews::DrawClip(AssetHandle handle)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const Ref<AnimationClip> clip = AssetManager::GetAsset<AnimationClip>(handle);
		if (!clip)
		{
			ImGui::TextColored(colors.TextSecondary, "Loading the clip...");
			return;
		}
		const std::vector<AnimationChannel>& channels = clip->GetChannels();
		ImGui::TextColored(colors.TextSecondary, "%.2f s \xC2\xB7 %zu channels \xC2\xB7 %llu keys \xC2\xB7 %zu joints", clip->GetDuration(), channels.size(),
			static_cast<unsigned long long>(clip->GetKeyCount()), clip->GetTargetNames().size());
		UI::ItemProbe::Record("AssetInspector.Clip.Summary", true, 0, fmt::format("{}", channels.size()));
		if (channels.empty())
			return;

		BeginListChild("AssetInspector.Clip.Channels", channels.size());
		if (ImGui::BeginTable("Channels", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Joint", ImGuiTableColumnFlags_WidthStretch, 3.0f);
			ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 2.0f);
			ImGui::TableSetupColumn("Interpolation", ImGuiTableColumnFlags_WidthStretch, 2.0f);
			ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableHeadersRow();
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(channels.size()));
			while (clipper.Step())
			{
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
				{
					const AnimationChannel& channel = channels[static_cast<size_t>(row)];
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(clip->GetTargetNames()[channel.Target].c_str());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(AnimationPathToString(channel.Path));
					ImGui::TableNextColumn();
					ImGui::TextColored(colors.TextSecondary, "%s", AnimationInterpolationToString(channel.Interpolation));
					ImGui::TableNextColumn();
					UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Body);
					ImGui::Text("%u", channel.KeyCount);
					ImGui::PopFont();
				}
			}
			ImGui::EndTable();
		}
		ImGui::EndChild();
	}

	////////////////////////////////////////////////////////////////////////////////
	// Controllers
	////////////////////////////////////////////////////////////////////////////////

	std::string AnimationAssetViews::FormatConditions(const AnimationControllerData& data, const AnimationTransition& transition)
	{
		std::string text;
		for (const AnimationCondition& condition : transition.Conditions)
		{
			if (condition.Parameter >= data.Parameters.size())
				continue;
			const std::string& name = data.Parameters[condition.Parameter].Name;
			if (!text.empty())
				text += ", ";
			switch (condition.Mode)
			{
				case AnimationConditionMode::Greater: text += fmt::format("{} > {}", name, FormatNumber(condition.Threshold)); break;
				case AnimationConditionMode::Less:    text += fmt::format("{} < {}", name, FormatNumber(condition.Threshold)); break;
				case AnimationConditionMode::True:
				case AnimationConditionMode::Set:     text += name; break;
				case AnimationConditionMode::False:   text += "!" + name; break;
			}
		}
		return text;
	}

	bool AnimationAssetViews::ParseConditions(const AnimationControllerData& data, std::string_view text, std::vector<AnimationCondition>& outConditions,
		std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};
		auto findParameter = [&data](std::string_view name) -> uint32_t
		{
			for (uint32_t index = 0; index < data.Parameters.size(); index++)
			{
				if (data.Parameters[index].Name == name)
					return index;
			}
			return ~0u;
		};

		std::vector<AnimationCondition> conditions;
		for (const std::string_view part : SplitList(text))
		{
			AnimationCondition condition;
			const size_t comparison = part.find_first_of("<>");
			if (comparison != std::string_view::npos)
			{
				const std::string_view name = Trim(part.substr(0, comparison));
				condition.Parameter = findParameter(name);
				if (condition.Parameter == ~0u)
					return fail(fmt::format("There is no parameter '{}'", name));
				if (data.Parameters[condition.Parameter].Type != AnimationParameterType::Float)
					return fail(fmt::format("'{}' is not a float: compare floats only", name));
				if (!ParseNumber(part.substr(comparison + 1), condition.Threshold))
					return fail(fmt::format("'{}' does not compare with a number", part));
				condition.Mode = part[comparison] == '>' ? AnimationConditionMode::Greater : AnimationConditionMode::Less;
			}
			else
			{
				const bool negated = part.front() == '!';
				const std::string_view name = Trim(negated ? part.substr(1) : part);
				condition.Parameter = findParameter(name);
				if (condition.Parameter == ~0u)
					return fail(fmt::format("There is no parameter '{}'", name));
				const AnimationParameterType type = data.Parameters[condition.Parameter].Type;
				if (type == AnimationParameterType::Float)
					return fail(fmt::format("'{}' is a float: compare it ('{} > 0.5')", name, name));
				if (type == AnimationParameterType::Trigger && negated)
					return fail(fmt::format("'{}' is a trigger: transitions wait for it to be set", name));
				condition.Mode = type == AnimationParameterType::Trigger ? AnimationConditionMode::Set : (negated ? AnimationConditionMode::False : AnimationConditionMode::True);
			}
			conditions.push_back(condition);
		}
		if (conditions.size() > AnimationController::c_MaxConditions)
			return fail(fmt::format("A transition has at most {} conditions", AnimationController::c_MaxConditions));
		outConditions = std::move(conditions);
		return true;
	}

	std::string AnimationAssetViews::FormatEvents(const AnimationState& state)
	{
		std::string text;
		for (const AnimationEvent& event : state.Events)
			text += fmt::format("{}{}@{}", text.empty() ? "" : ", ", event.Name, FormatNumber(event.Time));
		return text;
	}

	bool AnimationAssetViews::ParseEvents(std::string_view text, std::vector<AnimationEvent>& outEvents, std::string* outError)
	{
		std::vector<AnimationEvent> events;
		for (const std::string_view part : SplitList(text))
		{
			const size_t at = part.rfind('@');
			AnimationEvent event;
			if (at == std::string_view::npos || Trim(part.substr(0, at)).empty() || !ParseNumber(part.substr(at + 1), event.Time) || event.Time < 0.0f)
			{
				if (outError)
					*outError = fmt::format("'{}' is not an event: write Name@seconds", part);
				return false;
			}
			event.Name = std::string(Trim(part.substr(0, at)));
			events.push_back(std::move(event));
		}
		outEvents = std::move(events);
		return true;
	}

	void AnimationAssetViews::Reset()
	{
		m_Pending.reset();
		m_Queued.reset();
		m_Source.reset();
		m_Error.clear();
	}

	void AnimationAssetViews::Edit(AnimationControllerData data, bool finished, const std::string& name)
	{
		if (m_Queued && m_Queued->Finished && !finished)
			return;
		m_Queued = QueuedEdit { std::move(data), finished, name };
	}

	void AnimationAssetViews::ApplyEdit(EditorContext& context, QueuedEdit edit)
	{
		if (!edit.Finished)
		{
			m_Pending = std::move(edit.Data);
			return;
		}
		m_Pending.reset();
		std::string error;
		if (AssetEdit::SetControllerData(context, m_Controller, edit.Data, edit.Name, {}, &error))
			m_Error.clear();
		else
			m_Error = std::move(error);
	}

	void AnimationAssetViews::DrawController(EditorContext& context, AssetHandle handle)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const Ref<AnimationController> controller = AssetManager::GetAsset<AnimationController>(handle);
		if (!controller)
		{
			ImGui::TextColored(colors.TextSecondary, "Loading the controller...");
			return;
		}
		// Another controller, or this one changed under the edit (saved, undone, reloaded): the edit in progress ends.
		if (handle != m_Controller || m_Source.lock() != controller)
		{
			if (handle != m_Controller)
				m_Error.clear();
			m_Controller = handle;
			m_Source = controller;
			m_Pending.reset();
		}
		const AnimationControllerData& data = m_Pending ? *m_Pending : controller->GetData();
		if (!m_Error.empty())
			UI::Banner("AssetInspector.Controller.Error", Icons::TriangleAlert, m_Error, colors.Error);

		m_Queued.reset();
		DrawParameters(context, data);
		DrawStates(context, data);
		DrawTransitions(context, data);
		// Nothing draws from `data` any more: the frame's edit may replace or end the pending one now.
		if (m_Queued)
		{
			QueuedEdit edit = std::move(*m_Queued);
			m_Queued.reset();
			ApplyEdit(context, std::move(edit));
		}
	}

	void AnimationAssetViews::DrawParameters(EditorContext& context, const AnimationControllerData& data)
	{
		const std::string summary = fmt::format("{}", data.Parameters.size());
		if (!UI::SectionHeader("AssetInspector.Controller.ParametersSection", (std::string(Icons::SlidersHorizontal) + "  Parameters").c_str(),
			ImGuiTreeNodeFlags_DefaultOpen, summary))
			return;
		Scene& scene = *context.GetActiveScene();
		static const PropertyInfo c_Name = MakeProperty("Name", PropertyType::String, "Transitions test the parameter by this name");
		static const PropertyInfo c_Type = []()
		{
			PropertyInfo property = MakeProperty("Type", PropertyType::Enum, "Float: compared with thresholds; Bool: true or false; Trigger: set until a transition consumes it");
			property.EnumValues = { { "Float", 0, {} }, { "Bool", 1, {} }, { "Trigger", 2, {} } };
			return property;
		}();
		static const PropertyInfo c_FloatDefault = MakeProperty("Default", PropertyType::Float, "The value the animator starts with");
		static const PropertyInfo c_BoolDefault = MakeProperty("DefaultBool", PropertyType::Bool, "The value the animator starts with");

		std::optional<size_t> removed;
		static constexpr EditColumn c_Columns[] = { { "Name", 6.0f }, { "Type", 5.75f }, { "Default", 3.75f }, { "##Remove", 0.0f, true } };
		if (!data.Parameters.empty() && BeginEditTable("Parameters", c_Columns, data.Parameters.size()))
		{
			for (size_t index = 0; index < data.Parameters.size(); index++)
			{
				const AnimationParameter& parameter = data.Parameters[index];
				const std::string probe = fmt::format("AssetInspector.Controller.Parameter{}", index);
				ImGui::PushID(static_cast<int>(index));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				PropertyValue name = parameter.Name;
				if (const PropertyEditResult result = Cell(c_Name, name, scene, probe + ".Name"); result.Changed || result.Finished)
				{
					AnimationControllerData edited = data;
					edited.Parameters[index].Name = std::get<std::string>(name);
					Edit(std::move(edited), result.Finished, "Rename Parameter");
				}
				ImGui::TableNextColumn();
				PropertyValue type = static_cast<int32_t>(parameter.Type);
				if (const PropertyEditResult result = Cell(c_Type, type, scene, probe + ".Type"); result.Finished)
				{
					AnimationControllerData edited = data;
					edited.Parameters[index].Type = static_cast<AnimationParameterType>(std::get<int32_t>(type));
					Edit(std::move(edited), true, "Change Parameter Type");
				}
				ImGui::TableNextColumn();
				if (parameter.Type == AnimationParameterType::Float)
				{
					PropertyValue value = parameter.DefaultFloat;
					if (const PropertyEditResult result = Cell(c_FloatDefault, value, scene, probe + ".Default"); result.Changed || result.Finished)
					{
						AnimationControllerData edited = data;
						edited.Parameters[index].DefaultFloat = std::get<float>(value);
						Edit(std::move(edited), result.Finished, "Edit Parameter Default");
					}
				}
				else if (parameter.Type == AnimationParameterType::Bool)
				{
					PropertyValue value = parameter.DefaultBool;
					if (const PropertyEditResult result = Cell(c_BoolDefault, value, scene, probe + ".Default"); result.Finished)
					{
						AnimationControllerData edited = data;
						edited.Parameters[index].DefaultBool = std::get<bool>(value);
						Edit(std::move(edited), true, "Edit Parameter Default");
					}
				}
				else
				{
					ImGui::TextColored(UI::GetThemeColors().TextDisabled, "cleared");
				}
				ImGui::TableNextColumn();
				if (UI::IconButton((probe + ".Remove").c_str(), Icons::X, "Remove the parameter and the conditions that test it"))
					removed = index;
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		if (removed)
		{
			// Conditions refer to parameters by position: those of the removed one go, the later ones move up.
			AnimationControllerData edited = data;
			edited.Parameters.erase(edited.Parameters.begin() + static_cast<std::ptrdiff_t>(*removed));
			for (AnimationTransition& transition : edited.Transitions)
			{
				std::erase_if(transition.Conditions, [&](const AnimationCondition& condition) { return condition.Parameter == *removed; });
				for (AnimationCondition& condition : transition.Conditions)
				{
					if (condition.Parameter > *removed)
						condition.Parameter--;
				}
			}
			Edit(std::move(edited), true, "Remove Parameter");
		}
		if (UI::ToolbarButton("AssetInspector.Controller.AddParameter", Icons::Plus, "Add Parameter", "Add a float parameter"))
		{
			AnimationControllerData edited = data;
			std::string name = "Parameter";
			for (uint32_t number = 1; std::any_of(edited.Parameters.begin(), edited.Parameters.end(), [&](const AnimationParameter& parameter) { return parameter.Name == name; }); number++)
				name = fmt::format("Parameter{}", number);
			edited.Parameters.push_back(AnimationParameter { name, AnimationParameterType::Float, 0.0f, false });
			Edit(std::move(edited), true, "Add Parameter");
		}
	}

	void AnimationAssetViews::DrawStates(EditorContext& context, const AnimationControllerData& data)
	{
		const std::string summary = fmt::format("{}", data.States.size());
		if (!UI::SectionHeader("AssetInspector.Controller.StatesSection", (std::string(Icons::Film) + "  States").c_str(), ImGuiTreeNodeFlags_DefaultOpen, summary))
			return;
		Scene& scene = *context.GetActiveScene();
		static const PropertyInfo c_Default = MakeProperty("Default", PropertyType::Bool, "The state the animator starts in");
		static const PropertyInfo c_Name = MakeProperty("Name", PropertyType::String, "Scripts and transitions name the state by this");
		static const PropertyInfo c_Clip = []()
		{
			PropertyInfo property = MakeProperty("Clip", PropertyType::Asset, "The animation clip the state plays (none: the rest pose)");
			property.AssetFilter = AssetType::AnimationClip;
			return property;
		}();
		static const PropertyInfo c_Speed = []()
		{
			PropertyInfo property = MakeProperty("Speed", PropertyType::Float, "Playback rate; negative plays backwards");
			property.Min = -AnimationController::c_MaxSpeed;
			property.Max = AnimationController::c_MaxSpeed;
			property.Speed = 0.01f;
			return property;
		}();
		static const PropertyInfo c_Loop = MakeProperty("Loop", PropertyType::Bool, "Whether the clip repeats");
		static const PropertyInfo c_Events = MakeProperty("Events", PropertyType::String, "Events scripts receive (OnAnimationEvent): Name@seconds, ...");

		std::optional<size_t> removed;
		// Events take 6.5 font sizes ("SwingEnd@0.9" whole), the room Loop's header needs beside its checkbox, so the table
		// still fits the Asset tab of the default layout at 100% and 150% without scrolling.
		static constexpr EditColumn c_Columns[] = { { "##Default", 0.0f, true }, { "Name", 6.0f }, { "Clip", 7.5f }, { "Speed", 4.0f }, { "Loop", 0.0f, true },
			{ "Events", 6.5f }, { "##Remove", 0.0f, true } };
		if (BeginEditTable("States", c_Columns, data.States.size()))
		{
			for (size_t index = 0; index < data.States.size(); index++)
			{
				const AnimationState& state = data.States[index];
				const std::string probe = fmt::format("AssetInspector.Controller.State{}", index);
				ImGui::PushID(static_cast<int>(index));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				PropertyValue isDefault = data.DefaultState == index;
				if (const PropertyEditResult result = Cell(c_Default, isDefault, scene, probe + ".Default"); result.Finished && std::get<bool>(isDefault))
				{
					AnimationControllerData edited = data;
					edited.DefaultState = static_cast<uint32_t>(index);
					Edit(std::move(edited), true, "Set Default State");
				}
				ImGui::TableNextColumn();
				PropertyValue name = state.Name;
				if (const PropertyEditResult result = Cell(c_Name, name, scene, probe + ".Name"); result.Changed || result.Finished)
				{
					AnimationControllerData edited = data;
					edited.States[index].Name = std::get<std::string>(name);
					Edit(std::move(edited), result.Finished, "Rename State");
				}
				ImGui::TableNextColumn();
				PropertyValue clip = UUID(state.Clip);
				if (const PropertyEditResult result = Cell(c_Clip, clip, scene, probe + ".Clip"); result.Finished)
				{
					AnimationControllerData edited = data;
					edited.States[index].Clip = std::get<UUID>(clip);
					Edit(std::move(edited), true, "Change State Clip");
				}
				ImGui::TableNextColumn();
				PropertyValue speed = state.Speed;
				if (const PropertyEditResult result = Cell(c_Speed, speed, scene, probe + ".Speed"); result.Changed || result.Finished)
				{
					AnimationControllerData edited = data;
					edited.States[index].Speed = std::get<float>(speed);
					Edit(std::move(edited), result.Finished, "Edit State Speed");
				}
				ImGui::TableNextColumn();
				PropertyValue loop = state.Loop;
				if (const PropertyEditResult result = Cell(c_Loop, loop, scene, probe + ".Loop"); result.Finished)
				{
					AnimationControllerData edited = data;
					edited.States[index].Loop = std::get<bool>(loop);
					Edit(std::move(edited), true, "Edit State Loop");
				}
				ImGui::TableNextColumn();
				PropertyValue events = FormatEvents(state);
				if (const PropertyEditResult result = Cell(c_Events, events, scene, probe + ".Events"); result.Finished)
				{
					AnimationControllerData edited = data;
					std::string error;
					if (ParseEvents(std::get<std::string>(events), edited.States[index].Events, &error))
						Edit(std::move(edited), true, "Edit State Events");
					else
						m_Error = std::move(error);
				}
				ImGui::TableNextColumn();
				if (UI::IconButton((probe + ".Remove").c_str(), Icons::X, "Remove the state and its transitions", data.States.size() > 1))
					removed = index;
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		if (removed)
		{
			// Transitions refer to states by position: those to or from the removed one go, later states move up.
			AnimationControllerData edited = data;
			const uint32_t gone = static_cast<uint32_t>(*removed);
			edited.States.erase(edited.States.begin() + static_cast<std::ptrdiff_t>(gone));
			std::erase_if(edited.Transitions, [gone](const AnimationTransition& transition) { return transition.From == gone || transition.To == gone; });
			for (AnimationTransition& transition : edited.Transitions)
			{
				if (transition.From != AnimationTransition::c_AnyState && transition.From > gone)
					transition.From--;
				if (transition.To > gone)
					transition.To--;
			}
			if (edited.DefaultState == gone)
				edited.DefaultState = 0;
			else if (edited.DefaultState > gone)
				edited.DefaultState--;
			Edit(std::move(edited), true, "Remove State");
		}
		if (UI::ToolbarButton("AssetInspector.Controller.AddState", Icons::Plus, "Add State", "Add a state without a clip"))
		{
			AnimationControllerData edited = data;
			std::string name = "State";
			for (uint32_t number = 1; std::any_of(edited.States.begin(), edited.States.end(), [&](const AnimationState& state) { return state.Name == name; }); number++)
				name = fmt::format("State{}", number);
			AnimationState state;
			state.Name = std::move(name);
			edited.States.push_back(std::move(state));
			Edit(std::move(edited), true, "Add State");
		}
	}

	void AnimationAssetViews::DrawTransitions(EditorContext& context, const AnimationControllerData& data)
	{
		const std::string summary = fmt::format("{}", data.Transitions.size());
		if (!UI::SectionHeader("AssetInspector.Controller.TransitionsSection", (std::string(Icons::ArrowRightLeft) + "  Transitions").c_str(),
			ImGuiTreeNodeFlags_DefaultOpen, summary))
			return;
		Scene& scene = *context.GetActiveScene();
		// From offers "Any" (any state) first, then the states; To the states.
		PropertyInfo from = MakeProperty("From", PropertyType::Enum, "The state the transition leaves, or Any: whatever state the animator is in");
		PropertyInfo to = MakeProperty("To", PropertyType::Enum, "The state it cross-fades to");
		from.EnumValues.push_back({ "Any", -1, "Any State" });
		for (size_t index = 0; index < data.States.size(); index++)
		{
			from.EnumValues.push_back({ data.States[index].Name, static_cast<int32_t>(index), {} });
			to.EnumValues.push_back({ data.States[index].Name, static_cast<int32_t>(index), {} });
		}
		static const PropertyInfo c_Conditions = MakeProperty("Conditions", PropertyType::String,
			"All must hold: \"Speed > 0.5\", \"Speed < 0.1\" (floats), \"Grounded\" or \"!Grounded\" (bools), \"Jump\" (a trigger, consumed when it fires)");
		static const PropertyInfo c_HasExit = MakeProperty("HasExit", PropertyType::Bool, "Wait for the source state's exit time");
		static const PropertyInfo c_Exit = []()
		{
			PropertyInfo property = MakeProperty("ExitTime", PropertyType::Float, "The source state's normalized time to wait for (1: its end)");
			property.Min = 0.0f;
			property.Max = 100.0f;
			property.Speed = 0.01f;
			return property;
		}();
		static const PropertyInfo c_Duration = []()
		{
			PropertyInfo property = MakeProperty("Duration", PropertyType::Float, "Cross-fade seconds (0: switch at once)");
			property.Min = 0.0f;
			property.Max = AnimationController::c_MaxDuration;
			property.Speed = 0.01f;
			return property;
		}();

		std::optional<size_t> removed;
		static constexpr EditColumn c_Columns[] = { { "From", 6.5f }, { "To", 5.5f }, { "Conditions", 7.5f }, { "##HasExit", 0.0f, true }, { "Exit", 3.0f },
			{ "Fade", 3.0f }, { "##Remove", 0.0f, true } };
		if (!data.Transitions.empty() && BeginEditTable("Transitions", c_Columns, data.Transitions.size()))
		{
			for (size_t index = 0; index < data.Transitions.size(); index++)
			{
				const AnimationTransition& transition = data.Transitions[index];
				const std::string probe = fmt::format("AssetInspector.Controller.Transition{}", index);
				ImGui::PushID(static_cast<int>(index));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				PropertyValue source = transition.From == AnimationTransition::c_AnyState ? -1 : static_cast<int32_t>(transition.From);
				if (const PropertyEditResult result = Cell(from, source, scene, probe + ".From"); result.Finished)
				{
					AnimationControllerData edited = data;
					const int32_t value = std::get<int32_t>(source);
					edited.Transitions[index].From = value < 0 ? AnimationTransition::c_AnyState : static_cast<uint32_t>(value);
					Edit(std::move(edited), true, "Change Transition Source");
				}
				ImGui::TableNextColumn();
				PropertyValue target = static_cast<int32_t>(transition.To);
				if (const PropertyEditResult result = Cell(to, target, scene, probe + ".To"); result.Finished)
				{
					AnimationControllerData edited = data;
					edited.Transitions[index].To = static_cast<uint32_t>(std::get<int32_t>(target));
					Edit(std::move(edited), true, "Change Transition Target");
				}
				ImGui::TableNextColumn();
				PropertyValue conditions = FormatConditions(data, transition);
				if (const PropertyEditResult result = Cell(c_Conditions, conditions, scene, probe + ".Conditions"); result.Finished)
				{
					AnimationControllerData edited = data;
					std::string error;
					if (ParseConditions(data, std::get<std::string>(conditions), edited.Transitions[index].Conditions, &error))
						Edit(std::move(edited), true, "Edit Transition Conditions");
					else
						m_Error = std::move(error);
				}
				ImGui::TableNextColumn();
				PropertyValue hasExit = transition.ExitTime.has_value();
				if (const PropertyEditResult result = Cell(c_HasExit, hasExit, scene, probe + ".HasExit"); result.Finished)
				{
					AnimationControllerData edited = data;
					edited.Transitions[index].ExitTime = std::get<bool>(hasExit) ? std::optional<float>(1.0f) : std::nullopt;
					Edit(std::move(edited), true, "Edit Transition Exit Time");
				}
				ImGui::TableNextColumn();
				if (transition.ExitTime)
				{
					PropertyValue exit = *transition.ExitTime;
					if (const PropertyEditResult result = Cell(c_Exit, exit, scene, probe + ".ExitTime"); result.Changed || result.Finished)
					{
						AnimationControllerData edited = data;
						edited.Transitions[index].ExitTime = std::get<float>(exit);
						Edit(std::move(edited), result.Finished, "Edit Transition Exit Time");
					}
				}
				else
				{
					ImGui::TextColored(UI::GetThemeColors().TextDisabled, "none");
				}
				ImGui::TableNextColumn();
				PropertyValue duration = transition.Duration;
				if (const PropertyEditResult result = Cell(c_Duration, duration, scene, probe + ".Duration"); result.Changed || result.Finished)
				{
					AnimationControllerData edited = data;
					edited.Transitions[index].Duration = std::get<float>(duration);
					Edit(std::move(edited), result.Finished, "Edit Transition Fade");
				}
				ImGui::TableNextColumn();
				if (UI::IconButton((probe + ".Remove").c_str(), Icons::X, "Remove the transition"))
					removed = index;
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		if (removed)
		{
			AnimationControllerData edited = data;
			edited.Transitions.erase(edited.Transitions.begin() + static_cast<std::ptrdiff_t>(*removed));
			Edit(std::move(edited), true, "Remove Transition");
		}
		if (UI::ToolbarButton("AssetInspector.Controller.AddTransition", Icons::Plus, "Add Transition", "Add a transition between the first two states"))
		{
			AnimationControllerData edited = data;
			AnimationTransition transition;
			transition.From = 0;
			transition.To = data.States.size() > 1 ? 1u : 0u;
			edited.Transitions.push_back(std::move(transition));
			Edit(std::move(edited), true, "Add Transition");
		}
	}

}
