"""Generates StrataEditor/src/UI/Icons.h from the Lucide icon font and its codepoint map.

The editor merges the Lucide icon font (StrataEditor/Resources/Fonts/Lucide.ttf) into its UI font, so icons are text:
each icon becomes a UTF-8 string constant (Icons::Play) that ImGui draws like any other glyph. Icon names are Lucide's
kebab-case names in PascalCase ("mouse-pointer-2" -> MousePointer2). The header also lists every icon with its
codepoint (Icons::c_All), so tests can check that the font has a glyph for each.

Lucide's codepoint map (codepoints.json in its font release) can name icons the font no longer has (removed brand
icons); the font's own character map decides, and those names are left out (and reported).

Run it after replacing the font and its codepoint map with another Lucide release (standard library only):

    python StrataEditor/Tools/GenerateIconHeader.py --version <release>

and commit the regenerated header together with the font.
"""

import argparse
import json
import pathlib
import re
import struct
import sys

EDITOR_DIR = pathlib.Path(__file__).resolve().parent.parent
FONTS_DIR = EDITOR_DIR / "Resources" / "Fonts"
DEFAULT_MAP = FONTS_DIR / "Lucide-codepoints.json"
DEFAULT_FONT = FONTS_DIR / "Lucide.ttf"
DEFAULT_OUTPUT = EDITOR_DIR / "src" / "UI" / "Icons.h"

# Unicode's Private Use Area of the Basic Multilingual Plane, where icon fonts place their glyphs. The editor merges only
# this range from the icon font (UI/EditorFonts.cpp), so every icon must lie inside it.
PRIVATE_USE_FIRST = 0xE000
PRIVATE_USE_LAST = 0xF8FF

NAME_PATTERN = re.compile(r"^[a-z0-9]+(-[a-z0-9]+)*$")


class FontError(Exception):
    pass


def read_u16(data, offset):
    if offset + 2 > len(data):
        raise FontError("truncated font")
    return struct.unpack_from(">H", data, offset)[0]


def read_u32(data, offset):
    if offset + 4 > len(data):
        raise FontError("truncated font")
    return struct.unpack_from(">I", data, offset)[0]


def read_mapped_codepoints(path):
    """The codepoints a TrueType font maps to a glyph (its 'cmap' table, subtable formats 4 and 12)."""
    try:
        data = pathlib.Path(path).read_bytes()
    except OSError as error:
        raise FontError(str(error))
    table_count = read_u16(data, 4)
    cmap = None
    for index in range(table_count):
        record = 12 + 16 * index
        if data[record:record + 4] == b"cmap":
            cmap = read_u32(data, record + 8)
    if cmap is None:
        raise FontError("no 'cmap' table")

    codepoints = set()
    for index in range(read_u16(data, cmap + 2)):
        subtable = cmap + read_u32(data, cmap + 4 + 8 * index + 4)
        subtable_format = read_u16(data, subtable)
        if subtable_format == 4:
            segments = read_u16(data, subtable + 6) // 2
            ends = subtable + 14
            starts = ends + 2 * segments + 2
            deltas = starts + 2 * segments
            range_offsets = deltas + 2 * segments
            for segment in range(segments):
                first = read_u16(data, starts + 2 * segment)
                last = read_u16(data, ends + 2 * segment)
                delta = read_u16(data, deltas + 2 * segment)
                range_offset = read_u16(data, range_offsets + 2 * segment)
                for codepoint in range(first, last + 1):
                    if codepoint == 0xFFFF:
                        continue
                    if range_offset == 0:
                        glyph = (codepoint + delta) & 0xFFFF
                    else:
                        glyph = read_u16(data, range_offsets + 2 * segment + range_offset + 2 * (codepoint - first))
                        if glyph != 0:
                            glyph = (glyph + delta) & 0xFFFF
                    if glyph != 0:
                        codepoints.add(codepoint)
        elif subtable_format == 12:
            for group in range(read_u32(data, subtable + 12)):
                record = subtable + 16 + 12 * group
                first, last, glyph = read_u32(data, record), read_u32(data, record + 4), read_u32(data, record + 8)
                if glyph != 0 or first != last:
                    codepoints.update(range(first, last + 1))
    return codepoints


def to_identifier(name):
    """'mouse-pointer-2' -> 'MousePointer2'. Digits on both sides of a dash keep an underscore ('arrow-down-0-1' ->
    'ArrowDown0_1', unlike 'arrow-down-01' -> 'ArrowDown01'); names that would start with a digit get an 'Icon' prefix."""
    identifier = ""
    for part in name.split("-"):
        if identifier[-1:].isdigit() and part[:1].isdigit():
            identifier += "_"
        identifier += part[:1].upper() + part[1:]
    if identifier[:1].isdigit():
        identifier = "Icon" + identifier
    return identifier


def to_utf8_literal(codepoint):
    return "".join(f"\\x{byte:02x}" for byte in chr(codepoint).encode("utf-8"))


def load_icons(map_path, font_codepoints):
    try:
        with open(map_path, encoding="utf-8") as file:
            codepoints = json.load(file)
    except (OSError, json.JSONDecodeError) as error:
        sys.exit(f"Cannot read the codepoint map '{map_path}': {error}")
    if not isinstance(codepoints, dict) or not codepoints:
        sys.exit(f"'{map_path}' must be a JSON object mapping icon names to codepoints")

    icons = []
    identifiers = {}
    without_glyph = []
    for name, codepoint in sorted(codepoints.items()):
        if not NAME_PATTERN.match(name):
            sys.exit(f"Unexpected icon name '{name}' (expected kebab-case)")
        if not isinstance(codepoint, int) or not PRIVATE_USE_FIRST <= codepoint <= PRIVATE_USE_LAST:
            sys.exit(f"Icon '{name}' has the codepoint {codepoint!r}, outside the Private Use Area U+E000-U+F8FF")
        if codepoint not in font_codepoints:
            without_glyph.append(name)
            continue
        identifier = to_identifier(name)
        if identifier in identifiers:
            sys.exit(f"Icons '{identifiers[identifier]}' and '{name}' both become Icons::{identifier}")
        identifiers[identifier] = name
        icons.append((identifier, name, codepoint))
    if not icons:
        sys.exit("The font has none of the map's icons")
    return icons, without_glyph


def render(icons, source, version):
    first = min(codepoint for _, _, codepoint in icons)
    last = max(codepoint for _, _, codepoint in icons)
    lines = [
        "#pragma once",
        "",
        f"// Generated by StrataEditor/Tools/GenerateIconHeader.py from {source} (Lucide {version}) - do not edit.",
        "//",
        "// Icons of the Lucide icon font as UTF-8 strings. The editor's fonts merge the icon font into the UI font",
        "// (UI/EditorFonts.h), so icons are text: ImGui::Button(Icons::Play), or fmt::format(\"{} Play\", Icons::Play).",
        "",
        "#include <array>",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace Strata::Icons",
        "{",
        "",
    ]
    for identifier, name, codepoint in icons:
        lines.append(f"\tinline constexpr const char* {identifier} = \"{to_utf8_literal(codepoint)}\"; // U+{codepoint:04X} {name}")
    lines += [
        "",
        "\tstruct IconGlyph",
        "\t{",
        "\t\tstd::string_view Name; // Lucide's name, e.g. \"mouse-pointer-2\"",
        "\t\tuint32_t Codepoint = 0;",
        "\t};",
        "",
        "\t// Every icon above, by name.",
        f"\tinline constexpr std::array<IconGlyph, {len(icons)}> c_All = {{ {{",
    ]
    for identifier, name, codepoint in icons:
        lines.append(f"\t\t{{ \"{name}\", 0x{codepoint:04X} }},")
    lines += [
        "\t} };",
        "",
        "\t// The codepoints the icons occupy (inside the Private Use Area, U+E000 to U+F8FF).",
        f"\tinline constexpr uint32_t c_FirstCodepoint = 0x{first:04X};",
        f"\tinline constexpr uint32_t c_LastCodepoint = 0x{last:04X};",
        "",
        "}",
        "",
    ]
    return "\n".join(lines)


def relative_to_editor(path):
    try:
        return path.resolve().relative_to(EDITOR_DIR).as_posix()
    except ValueError:
        return path.name


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--map", type=pathlib.Path, default=DEFAULT_MAP, help="Lucide's codepoints.json")
    parser.add_argument("--font", type=pathlib.Path, default=DEFAULT_FONT, help="The Lucide font (lucide.ttf)")
    parser.add_argument("--output", type=pathlib.Path, default=DEFAULT_OUTPUT, help="The header to write")
    parser.add_argument("--version", default="1.47.0", help="The Lucide release of the font and the map")
    arguments = parser.parse_args()

    try:
        font_codepoints = read_mapped_codepoints(arguments.font)
    except FontError as error:
        sys.exit(f"Cannot read the font '{arguments.font}': {error}")
    icons, without_glyph = load_icons(arguments.map, font_codepoints)
    source = f"{relative_to_editor(arguments.map)} and {relative_to_editor(arguments.font)}"
    text = render(icons, source, arguments.version)
    with open(arguments.output, "w", encoding="utf-8", newline="\n") as file:
        file.write(text)
    print(f"Wrote {len(icons)} icons to {arguments.output}")
    if without_glyph:
        print(f"Left out {len(without_glyph)} names the font has no glyph for: {', '.join(without_glyph)}")


if __name__ == "__main__":
    main()
