#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Strata
{

	// The desktop entry of an exported Linux game, <File>.desktop next to it: what Linux desktops list applications by, once
	// a person copies it into ~/.local/share/applications (installing it is not the export's). It names the export's
	// absolute paths, so moving the folder means editing it. Written by the rules of the Desktop Entry Specification:
	// every value escaped as its string type (\s for spaces at either end, \n, \t, \r, \\), and Exec's program quoted with
	// ", `, $ and \ escaped and % doubled, so that any path parses back. Platform-independent text, tested everywhere; the
	// export writes it on Linux hosts.
	struct DesktopEntry
	{
		std::string Name;                 // What menus show: the game's title
		std::filesystem::path Executable; // Absolute
		std::filesystem::path Icon;       // Absolute; empty: no icon
		std::string WindowClass;          // StartupWMClass: the class of the game's windows (WindowSpecification::AppId)

		// The file's text (UTF-8, LF line endings); nothing, with the reason, when a value is empty or no valid UTF-8 (the
		// specification's values are UTF-8) or a path is not absolute.
		std::optional<std::string> ToText(std::string* outError = nullptr) const;

		// A value escaped as the specification's string type: backslashes, newlines, tabs and carriage returns, and the spaces
		// at either end, which readers would trim.
		static std::string EscapeString(std::string_view value);
		// An argument of Exec: quoted, with the characters reserved inside quotes escaped and % doubled (a field code
		// otherwise). Exec's whole value is then escaped as a string.
		static std::string QuoteArgument(std::string_view argument);
	};

}
