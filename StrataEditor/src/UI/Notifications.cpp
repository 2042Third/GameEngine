#include "UI/Notifications.h"

#include "Editor/EditorNotifications.h"
#include "UI/EditorFonts.h"
#include "UI/EditorShell.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <spdlog/fmt/fmt.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace Strata::UI
{

	namespace
	{

		// The toasts' width, in text heights.
		constexpr float c_ToastWidthInFontSizes = 24.0f;
		// Messages longer than this show cut (the Console and the Activity panel have them whole).
		constexpr size_t c_MaxMessageBytes = 280;

		const char* GetLevelIcon(NotificationLevel level)
		{
			switch (level)
			{
				case NotificationLevel::Info:    return Icons::Info;
				case NotificationLevel::Success: return Icons::CircleCheck;
				case NotificationLevel::Warning: return Icons::TriangleAlert;
				case NotificationLevel::Error:   return Icons::CircleX;
			}
			return Icons::Info;
		}

		ImVec4 GetLevelColor(NotificationLevel level, const ThemeColors& colors)
		{
			switch (level)
			{
				case NotificationLevel::Info:    return colors.Info;
				case NotificationLevel::Success: return colors.Success;
				case NotificationLevel::Warning: return colors.Warning;
				case NotificationLevel::Error:   return colors.Error;
			}
			return colors.Info;
		}

		// The text cut to at most `bytes` (never inside a UTF-8 sequence), with an ellipsis when it was longer.
		std::string Shorten(const std::string& text, size_t bytes)
		{
			if (text.size() <= bytes)
				return text;
			size_t end = bytes;
			while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
				end--;
			return text.substr(0, end) + "...";
		}

	}

	void DrawNotifications(EditorNotifications& notifications, EditorShell& shell, const char* consolePanel, const ImVec2& areaMin, const ImVec2& areaMax)
	{
		if (notifications.IsEmpty())
			return;
		notifications.Expire(std::chrono::steady_clock::now());
		if (notifications.IsEmpty())
			return;

		const ImGuiStyle& style = ImGui::GetStyle();
		const ThemeColors& colors = GetThemeColors();
		const float width = ImGui::GetFontSize() * c_ToastWidthInFontSizes;
		ImGui::SetNextWindowPos(ImVec2(areaMax.x - style.WindowPadding.x, areaMax.y - style.WindowPadding.y), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
		ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(areaMax.x - areaMin.x, areaMax.y - areaMin.y));
		constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
			| ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground
			| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus;
		if (!ImGui::Begin("##Notifications", nullptr, flags))
		{
			ImGui::End();
			return;
		}
		// In front of the panels without taking the focus (a focused window would take the keyboard from the game view).
		ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

		// The actions may dismiss toasts or open panels: they act on a copy of what shows.
		const std::vector<EditorNotification> shown(notifications.GetActive().begin(), notifications.GetActive().end());
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		for (size_t index = 0; index < shown.size(); index++)
		{
			const EditorNotification& notification = shown[index];
			const std::string key = fmt::format("Toast.{}", index);
			const ImVec4 levelColor = GetLevelColor(notification.Level, colors);
			ImGui::PushID(static_cast<int>(notification.Id));

			// The surface goes under the contents: drawn on the lower channel once their size is known.
			drawList->ChannelsSplit(2);
			drawList->ChannelsSetCurrent(1);
			const ImVec2 start = ImGui::GetCursorScreenPos();
			ImGui::SetCursorScreenPos(ImVec2(start.x + style.FramePadding.x, start.y + style.FramePadding.y));
			ImGui::BeginGroup();
			const float innerWidth = width - style.FramePadding.x * 2.0f;
			ImGui::TextColored(levelColor, "%s", GetLevelIcon(notification.Level));
			ImGui::SameLine();
			PushFont(EditorFont::SemiBold);
			ImGui::TextUnformatted(notification.Title.c_str());
			ImGui::PopFont();
			if (notification.Count > 1)
			{
				ImGui::SameLine();
				ImGui::TextColored(colors.TextSecondary, "x%u", notification.Count);
			}
			ImGui::SameLine(innerWidth - ImGui::GetFrameHeight());
			const bool close = IconButton((key + ".Close").c_str(), Icons::X, "Dismiss");

			if (!notification.Message.empty())
			{
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + innerWidth);
				ImGui::TextColored(colors.TextSecondary, "%s", Shorten(notification.Message, c_MaxMessageBytes).c_str());
				ImGui::PopTextWrapPos();
			}
			bool first = true;
			const auto action = [&first](const std::string& id, const char* label)
			{
				if (!first)
					ImGui::SameLine();
				first = false;
				return LinkButton(id.c_str(), label);
			};
			bool showActivity = false;
			bool showConsole = false;
			bool recover = false;
			if (notification.JournalEntry != 0)
				showActivity = action(key + ".Activity", "Show in Activity");
			if (notification.ShowConsole)
				showConsole = action(key + ".Console", "Show in Console");
			if (notification.ShowRecovery)
				recover = action(key + ".Recover", "Recover...");
			ImGui::Dummy(ImVec2(innerWidth, 0.0f));
			ImGui::EndGroup();
			const ImVec2 end(start.x + width, ImGui::GetItemRectMax().y + style.FramePadding.y);
			const bool hovered = ImGui::IsMouseHoveringRect(start, end);

			drawList->ChannelsSetCurrent(0);
			const float rounding = style.FrameRounding;
			drawList->AddRectFilled(start, end, ToColorU32(colors.Raised), rounding);
			drawList->AddRect(start, end, ToColorU32(colors.Border), rounding);
			// A bar in the level's color along the left edge says what kind of news it is at a glance.
			drawList->AddRectFilled(start, ImVec2(start.x + std::max(2.0f, style.FramePadding.x * 0.5f), end.y), ToColorU32(levelColor), rounding,
				ImDrawFlags_RoundCornersLeft);
			drawList->ChannelsMerge();
			ImGui::SetCursorScreenPos(ImVec2(start.x, end.y + style.ItemSpacing.y));
			ImGui::Dummy(ImVec2(width, 0.0f));
			ItemProbe::RecordRect(key, start, end, true, ToColorU32(levelColor), notification.Title + "\n" + notification.Message);
			ImGui::PopID();

			if (hovered)
				notifications.KeepAlive(notification.Id);
			if (showActivity)
				shell.ShowActivity(notification.JournalEntry);
			if (showConsole)
				shell.ShowPanel(consolePanel);
			if (recover)
				shell.ShowRecoveryDialog();
			if (close || showActivity || showConsole || recover)
				notifications.Dismiss(notification.Id);
		}
		ImGui::End();
	}

}
