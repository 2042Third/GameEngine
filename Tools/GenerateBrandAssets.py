#!/usr/bin/env python3
"""Draws the Strata mark and writes the brand assets of the editor and the runtime.

The outputs are committed (CI never runs this script); run it again only to change the mark:

    python Tools/GenerateBrandAssets.py

The mark: four staggered rounded bands, graded from Sandstone at the top through Ochre and Rust to Umber, on a Basalt
rounded square. At 16 pixels it keeps three thicker bands, so they stay apart. Every image is drawn from signed
distances (the coverage of a pixel is its distance to the shape's edge, clamped to one pixel), so no size is a scaled
copy of another. Only the standard library is used (zlib and struct); a given Python installation writes the same
bytes on every run, though another zlib may compress the PNGs differently. The outputs, in
StrataEditor/Resources/Brand/:
    StrataMark16.png, StrataMark32.png, StrataMark48.png, StrataMark256.png   RGBA PNGs
    StrataMark16.rgba, StrataMark32.rgba, StrataMark48.rgba                     raw RGBA8 rows, top row first, for the
                                                                                window icon (embedded, decoded at no cost)
    StrataMark.ico                                                              Windows icon: 16, 32 and 48 pixels as
                                                                                32-bit bitmaps, 256 pixels as PNG
"""

import os
import struct
import zlib

REPOSITORY_DIRECTORY = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTPUT_DIRECTORY = os.path.join(REPOSITORY_DIRECTORY, "StrataEditor", "Resources", "Brand")

# The 'Bedrock' palette (sRGB).
BASALT = (0x0E, 0x10, 0x13)
SANDSTONE = (0xF2, 0xB8, 0x72)
OCHRE = (0xE0, 0x8A, 0x2E)
RUST = (0xB9, 0x56, 0x2B)
UMBER = (0x6B, 0x3A, 0x22)

PNG_SIZES = (16, 32, 48, 256)
RAW_SIZES = (16, 32, 48)
ICO_BITMAP_SIZES = (16, 32, 48)
ICO_PNG_SIZE = 256


def write_bytes(name, data):
    os.makedirs(OUTPUT_DIRECTORY, exist_ok=True)
    with open(os.path.join(OUTPUT_DIRECTORY, name), "wb") as file:
        file.write(data)
    print(f"{name}: {len(data)} bytes")


################################################################################
# Shapes
################################################################################

def rounded_rectangle_distance(x, y, center_x, center_y, half_width, half_height, radius):
    """Signed distance from (x, y) to a rounded rectangle (negative inside), all in the same units."""
    qx = abs(x - center_x) - (half_width - radius)
    qy = abs(y - center_y) - (half_height - radius)
    outside = (max(qx, 0.0) ** 2 + max(qy, 0.0) ** 2) ** 0.5
    return outside + min(max(qx, qy), 0.0) - radius


def bands_for_size(size):
    """The bands as (color, left, right, top, bottom) in units of the image size (0..1)."""
    if size <= 16:
        colors = (SANDSTONE, OCHRE, RUST)
        thickness, gap = 0.16, 0.10
        width, step = 0.58, 0.08
    else:
        colors = (SANDSTONE, OCHRE, RUST, UMBER)
        thickness, gap = 0.115, 0.06
        width, step = 0.56, 0.06
    count = len(colors)
    total = count * thickness + (count - 1) * gap
    top = (1.0 - total) / 2.0
    bands = []
    for index, color in enumerate(colors):
        # Staggered like layers along a fault: upper bands sit further right, lower ones further left.
        center = 0.5 + ((count - 1) / 2.0 - index) * step
        band_top = top + index * (thickness + gap)
        bands.append((color, center - width / 2.0, center + width / 2.0, band_top, band_top + thickness))
    return bands


def coverage(distance_in_pixels):
    """Fraction of a pixel inside a shape whose edge is this far from the pixel's center (negative: inside)."""
    return min(max(0.5 - distance_in_pixels, 0.0), 1.0)


def draw_mark(size):
    """RGBA8 rows (bytes), top row first."""
    bands = bands_for_size(size)
    corner = 0.22 if size > 16 else 0.18
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            # Pixel centers in units of the image.
            x = (px + 0.5) / size
            y = (py + 0.5) / size
            tile = coverage(rounded_rectangle_distance(x, y, 0.5, 0.5, 0.5, 0.5, corner) * size)
            red, green, blue = (float(channel) for channel in BASALT)
            for color, left, right, top, bottom in bands:
                half_height = (bottom - top) / 2.0
                distance = rounded_rectangle_distance(x, y, (left + right) / 2.0, (top + bottom) / 2.0, (right - left) / 2.0, half_height, half_height)
                weight = coverage(distance * size)
                red += (color[0] - red) * weight
                green += (color[1] - green) * weight
                blue += (color[2] - blue) * weight
            alpha = tile * 255.0
            row.extend((int(red + 0.5), int(green + 0.5), int(blue + 0.5), int(alpha + 0.5)))
        rows.append(bytes(row))
    return rows


################################################################################
# Files
################################################################################

def png_chunk(kind, payload):
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)


def encode_png(size, rows):
    raw = b"".join(b"\x00" + row for row in rows)  # Filter type: none
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)  # 8 bits per channel, RGBA
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) + png_chunk(b"IDAT", zlib.compress(raw, 9))
            + png_chunk(b"IEND", b""))


def encode_ico_bitmap(size, rows):
    """A 32-bit icon bitmap: BITMAPINFOHEADER (height doubled for the mask), BGRA rows bottom-up, then the AND mask."""
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = bytearray()
    for row in reversed(rows):
        for index in range(0, len(row), 4):
            red, green, blue, alpha = row[index:index + 4]
            pixels.extend((blue, green, red, alpha))
    # The alpha channel decides transparency; the 1-bit mask (rows padded to 32 bits) stays all opaque.
    mask_row = ((size + 31) // 32) * 4
    return header + bytes(pixels) + bytes(mask_row * size)


def encode_ico(images):
    """images: (size, data) pairs. 256 is stored as 0 in the directory."""
    directory = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    payload = b""
    for size, data in images:
        dimension = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dimension, dimension, 0, 0, 1, 32, len(data), offset + len(payload))
        payload += data
    return directory + entries + payload


def main():
    marks = {size: draw_mark(size) for size in PNG_SIZES}
    for size in PNG_SIZES:
        write_bytes(f"StrataMark{size}.png", encode_png(size, marks[size]))
    for size in RAW_SIZES:
        write_bytes(f"StrataMark{size}.rgba", b"".join(marks[size]))
    icon_images = [(size, encode_ico_bitmap(size, marks[size])) for size in ICO_BITMAP_SIZES]
    icon_images.append((ICO_PNG_SIZE, encode_png(ICO_PNG_SIZE, marks[ICO_PNG_SIZE])))
    write_bytes("StrataMark.ico", encode_ico(icon_images))


if __name__ == "__main__":
    main()
