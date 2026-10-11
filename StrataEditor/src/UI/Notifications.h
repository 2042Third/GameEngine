#pragma once

#include <imgui.h>

namespace Strata
{
	class EditorNotifications;
	class EditorShell;
}

namespace Strata::UI
{

	// The editor's notifications (EditorNotifications) as toasts stacked at the bottom right of the area (the main window's
	// work area, above the status bar), newest at the bottom: an icon in the level's color, the title, the message (cut to
	// a few lines), how often it repeated, its actions (Show in Activity, Show Asset, Show in Console, Recover...) and a close
	// button. Hovering a toast keeps it. Draws nothing, and does no work, while there are none. Toasts never take the keyboard
	// focus: their window does not take it when it appears and stays in front of the panels without being focused. Probe
	// keys: "Toast.<n>" (n counts from 0 at the top), with ".Activity", ".Asset", ".Console", ".Recover" and ".Close" for its
	// buttons.
	void DrawNotifications(EditorNotifications& notifications, EditorShell& shell, const char* consolePanel, const ImVec2& areaMin, const ImVec2& areaMax);

}
