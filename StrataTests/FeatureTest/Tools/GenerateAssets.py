#!/usr/bin/env python3
"""Generates the binary and generated assets of the Strata feature test project.

The outputs are committed (CI never runs this script); run it again only to change them:

    python StrataTests/FeatureTest/Tools/GenerateAssets.py

Every asset is tiny and computed without randomness, so a given Python installation writes the same bytes on every
run. Other installations may differ in two places: the PNG's pixel data is compressed by the zlib Python links
(another zlib version or implementation, such as zlib-ng, may produce other compressed bytes for the same pixels), and
the WAV samples come from the platform's math.sin (rounded to 16 bits, so a difference is unlikely but possible). The
other files use integer and string operations and float arithmetic that IEEE 754 makes exact or correctly rounded
everywhere (+, -, *, /, sqrt, frexp, log2 of powers of two, shortest float repr in JSON). After regenerating, commit
only files whose content changed on purpose; the .meta files keep the handles either way. The assets:
    Assets/Textures/Checker.png    8x8 RGBA checkerboard (sRGB color texture)
    Assets/Textures/Sky.hdr        16x8 Radiance HDR sky gradient (equirectangular environment map)
    Assets/Audio/Blip.wav          0.1 s mono 16-bit PCM tone
    Assets/Fonts/FeatureBlocks.ttf TrueType font: every printable ASCII character is a solid block
    Assets/Models/Platform.gltf    glTF 2.0 model: a 4 x 0.5 x 4 platform with a pyramid "Beacon" child node,
                                   two materials (one textured with Checker.png), buffers embedded as a data URI

The .meta sidecars are not generated: they hold the stable asset handles the scene, prefab and material reference.
"""

import base64
import json
import math
import os
import struct
import zlib

PROJECT_DIRECTORY = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSET_DIRECTORY = os.path.join(PROJECT_DIRECTORY, "Assets")


def write_bytes(relative_path, data):
    path = os.path.join(ASSET_DIRECTORY, relative_path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as file:
        file.write(data)
    print(f"{relative_path}: {len(data)} bytes")


################################################################################
# Checker.png
################################################################################

def png_chunk(kind, payload):
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)


def make_checker_png():
    size = 8
    orange = (230, 120, 30, 255)
    cream = (240, 230, 200, 255)
    rows = bytearray()
    for y in range(size):
        rows.append(0)  # Filter type: none
        for x in range(size):
            rows.extend(orange if ((x // 2) + (y // 2)) % 2 == 0 else cream)
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)  # 8 bits per channel, RGBA
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) + png_chunk(b"IDAT", zlib.compress(bytes(rows), 9))
            + png_chunk(b"IEND", b""))


################################################################################
# Sky.hdr
################################################################################

def to_rgbe(color):
    brightest = max(color)
    if brightest < 1e-32:
        return bytes((0, 0, 0, 0))
    mantissa, exponent = math.frexp(brightest)
    scale = mantissa * 256.0 / brightest
    return bytes((int(color[0] * scale), int(color[1] * scale), int(color[2] * scale), exponent + 128))


def make_sky_hdr():
    width, height = 16, 8
    zenith = (0.25, 0.45, 1.2)
    horizon = (1.4, 1.3, 1.1)
    ground = (0.25, 0.2, 0.15)
    pixels = bytearray()
    for y in range(height):
        # Rows 0-3 are the upper hemisphere (zenith to horizon), rows 4-7 the ground.
        if y < height // 2:
            t = y / (height // 2 - 1)
            color = tuple(zenith[i] + (horizon[i] - zenith[i]) * t for i in range(3))
        else:
            color = ground
        for x in range(width):
            # A slightly warmer band around the "sun" columns, so the map is not rotationally symmetric.
            warmth = 1.5 if x in (3, 4) and y == 2 else 1.0
            pixels.extend(to_rgbe((color[0] * warmth, color[1] * warmth, color[2])))
    # Rows narrower than 8 pixels or not starting with the 2, 2 run-length marker are stored flat.
    header = f"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {height} +X {width}\n".encode("ascii")
    return header + bytes(pixels)


################################################################################
# Blip.wav
################################################################################

def make_blip_wav():
    sample_rate = 11025
    sample_count = sample_rate // 10
    samples = bytearray()
    for index in range(sample_count):
        envelope = min(1.0, index / 64.0, (sample_count - index) / 64.0)
        value = 0.4 * envelope * math.sin(2.0 * math.pi * 880.0 * index / sample_rate)
        samples.extend(struct.pack("<h", int(round(value * 32767))))
    fmt = struct.pack("<HHIIHH", 1, 1, sample_rate, sample_rate * 2, 2, 16)  # PCM, mono, 16 bits
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(samples)) + bytes(samples)
    return b"RIFF" + struct.pack("<I", len(body)) + body


################################################################################
# FeatureBlocks.ttf
################################################################################

def table_checksum(data):
    padded = data + b"\0" * (-len(data) % 4)
    return sum(struct.unpack(f">{len(padded) // 4}I", padded)) & 0xFFFFFFFF


def make_cmap():
    # Format 4: space maps to glyph 1, '!'..'~' to glyph 2 (through the glyph id array), plus the final 0xFFFF segment.
    first, last = 33, 126
    seg_count = 3
    end_codes = [32, last, 0xFFFF]
    start_codes = [32, first, 0xFFFF]
    id_deltas = [1 - 32, 0, 1]
    id_range_offsets = [0, 2 * (seg_count - 1), 0]  # Segment 1 points at the start of the glyph id array
    glyph_ids = [2] * (last - first + 1)
    search_range = 2 * (2 ** int(math.log2(seg_count)))
    entry_selector = int(math.log2(search_range // 2))
    range_shift = 2 * seg_count - search_range
    body = struct.pack(">HHHH", 2 * seg_count, search_range, entry_selector, range_shift)
    body += struct.pack(f">{seg_count}H", *end_codes) + struct.pack(">H", 0)
    body += struct.pack(f">{seg_count}H", *start_codes)
    body += struct.pack(f">{seg_count}h", *id_deltas)
    body += struct.pack(f">{seg_count}H", *id_range_offsets)
    body += struct.pack(f">{len(glyph_ids)}H", *glyph_ids)
    subtable = struct.pack(">HHH", 4, 6 + len(body), 0) + body
    return struct.pack(">HH", 0, 1) + struct.pack(">HHI", 3, 1, 12) + subtable  # Windows, Unicode BMP


def make_block_glyph():
    # One clockwise contour of four on-curve points; coordinates are 16-bit deltas.
    points = [(50, 0), (50, 700), (550, 700), (550, 0)]
    data = struct.pack(">hhhhh", 1, 50, 0, 550, 700)
    data += struct.pack(">H", len(points) - 1) + struct.pack(">H", 0)  # End point of the contour, no instructions
    data += bytes([0x01] * len(points))  # On curve, long coordinates
    previous = (0, 0)
    x_deltas, y_deltas = [], []
    for point in points:
        x_deltas.append(point[0] - previous[0])
        y_deltas.append(point[1] - previous[1])
        previous = point
    data += struct.pack(f">{len(points)}h", *x_deltas) + struct.pack(f">{len(points)}h", *y_deltas)
    return data


def make_name_table():
    names = {
        1: "Strata Feature Blocks",
        2: "Regular",
        4: "Strata Feature Blocks Regular",
        6: "StrataFeatureBlocks-Regular",
    }
    records = b""
    strings = b""
    for name_id, text in names.items():
        encoded = text.encode("utf-16-be")
        records += struct.pack(">HHHHHH", 3, 1, 0x409, name_id, len(encoded), len(strings))
        strings += encoded
    return struct.pack(">HHH", 0, len(names), 6 + len(records)) + records + strings


def make_font():
    block = make_block_glyph()
    assert len(block) % 2 == 0
    glyph_count = 3  # .notdef and space are empty, glyph 2 is the block
    tables = {
        b"cmap": make_cmap(),
        b"glyf": block,
        b"head": struct.pack(">HHIIIHHqqhhhhHHhhh", 1, 0, 0x00010000, 0, 0x5F0F3CF5, 0x000B, 1000, 0, 0,
                             50, 0, 550, 700, 0, 8, 2, 0, 0),
        b"hhea": struct.pack(">HHhhhHhhhhhhhhhhhH", 1, 0, 800, -200, 0, 600, 50, 50, 550, 1, 0, 0, 0, 0, 0, 0, 0,
                             glyph_count),
        b"hmtx": struct.pack(">HhHhHh", 600, 0, 600, 0, 600, 50),
        b"loca": struct.pack(">4H", 0, 0, 0, len(block) // 2),
        b"maxp": struct.pack(">I14H", 0x00010000, glyph_count, 4, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0),
        b"name": make_name_table(),
        b"post": struct.pack(">IIhhIIIII", 0x00030000, 0, -100, 50, 1, 0, 0, 0, 0),
    }

    tags = sorted(tables)
    entry_selector = int(math.log2(len(tags)))
    search_range = 16 * (2 ** entry_selector)
    directory = struct.pack(">IHHHH", 0x00010000, len(tags), search_range, entry_selector, 16 * len(tags) - search_range)
    offset = len(directory) + 16 * len(tags)
    records = b""
    body = b""
    for tag in tags:
        data = tables[tag]
        records += tag + struct.pack(">III", table_checksum(data), offset + len(body), len(data))
        body += data + b"\0" * (-len(data) % 4)
    font = bytearray(directory + records + body)

    # head.checkSumAdjustment makes the whole file sum to 0xB1B0AFBA.
    head_offset = struct.unpack(">I", records[tags.index(b"head") * 16 + 8:tags.index(b"head") * 16 + 12])[0]
    adjustment = (0xB1B0AFBA - table_checksum(bytes(font))) & 0xFFFFFFFF
    font[head_offset + 8:head_offset + 12] = struct.pack(">I", adjustment)
    return bytes(font)


################################################################################
# Platform.gltf
################################################################################

def box_faces(half):
    hx, hy, hz = half
    # (normal, four corners counter-clockwise seen from outside)
    return [
        ((0, 1, 0), [(-hx, hy, hz), (hx, hy, hz), (hx, hy, -hz), (-hx, hy, -hz)]),
        ((0, -1, 0), [(-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz), (-hx, -hy, hz)]),
        ((0, 0, 1), [(-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz)]),
        ((0, 0, -1), [(hx, -hy, -hz), (-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz)]),
        ((1, 0, 0), [(hx, -hy, hz), (hx, -hy, -hz), (hx, hy, -hz), (hx, hy, hz)]),
        ((-1, 0, 0), [(-hx, -hy, -hz), (-hx, -hy, hz), (-hx, hy, hz), (-hx, hy, -hz)]),
    ]


def make_platform_mesh():
    positions, normals, uvs, indices = [], [], [], []
    for normal, corners in box_faces((2.0, 0.25, 2.0)):
        base = len(positions)
        positions.extend(corners)
        normals.extend([normal] * 4)
        uvs.extend([(0.0, 2.0), (2.0, 2.0), (2.0, 0.0), (0.0, 0.0)])
        indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])
    return positions, normals, uvs, indices


def normalize(vector):
    length = math.sqrt(sum(component * component for component in vector))
    return tuple(component / length for component in vector)


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def subtract(a, b):
    return tuple(a[i] - b[i] for i in range(3))


def make_beacon_mesh():
    half, height = 0.3, 0.75
    apex = (0.0, height, 0.0)
    base_corners = [(-half, 0.0, half), (half, 0.0, half), (half, 0.0, -half), (-half, 0.0, -half)]
    positions, normals, uvs, indices = [], [], [], []
    for index in range(4):
        a, b = base_corners[index], base_corners[(index + 1) % 4]
        normal = normalize(cross(subtract(b, a), subtract(apex, a)))  # Counter-clockwise a, b, apex
        start = len(positions)
        positions.extend([a, b, apex])
        normals.extend([normal] * 3)
        uvs.extend([(0.0, 1.0), (1.0, 1.0), (0.5, 0.0)])
        indices.extend([start, start + 1, start + 2])
    start = len(positions)
    positions.extend(reversed(base_corners))  # Seen from below
    normals.extend([(0.0, -1.0, 0.0)] * 4)
    uvs.extend([(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)])
    indices.extend([start, start + 1, start + 2, start, start + 2, start + 3])
    return positions, normals, uvs, indices


def make_platform_gltf():
    buffer = bytearray()
    buffer_views = []
    accessors = []

    def add_view(data, target):
        while len(buffer) % 4:
            buffer.append(0)
        buffer_views.append({"buffer": 0, "byteOffset": len(buffer), "byteLength": len(data), "target": target})
        buffer.extend(data)
        return len(buffer_views) - 1

    def add_vectors(values, kind, with_bounds=False):
        width = len(values[0])
        data = b"".join(struct.pack(f"<{width}f", *value) for value in values)
        accessor = {"bufferView": add_view(data, 34962), "componentType": 5126, "count": len(values), "type": kind}
        if with_bounds:
            accessor["min"] = [min(value[i] for value in values) for i in range(width)]
            accessor["max"] = [max(value[i] for value in values) for i in range(width)]
        accessors.append(accessor)
        return len(accessors) - 1

    def add_indices(values):
        data = struct.pack(f"<{len(values)}H", *values)
        accessors.append({"bufferView": add_view(data, 34963), "componentType": 5123, "count": len(values), "type": "SCALAR"})
        return len(accessors) - 1

    meshes = []
    for name, (positions, normals, uvs, indices), material in (("Platform", make_platform_mesh(), 0), ("Beacon", make_beacon_mesh(), 1)):
        attributes = {
            "POSITION": add_vectors(positions, "VEC3", with_bounds=True),
            "NORMAL": add_vectors(normals, "VEC3"),
            "TEXCOORD_0": add_vectors(uvs, "VEC2"),
        }
        meshes.append({"name": name, "primitives": [{"attributes": attributes, "indices": add_indices(indices), "material": material}]})

    document = {
        "asset": {"version": "2.0", "generator": "Strata FeatureTest GenerateAssets.py"},
        "scene": 0,
        "scenes": [{"name": "Platform", "nodes": [0]}],
        "nodes": [
            {"name": "Platform", "mesh": 0, "children": [1]},
            {"name": "Beacon", "mesh": 1, "translation": [0.0, 0.25, 0.0]},
        ],
        "meshes": meshes,
        "materials": [
            {
                "name": "PlatformMaterial",
                "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 0.8},
            },
            {
                "name": "BeaconMaterial",
                "pbrMetallicRoughness": {"baseColorFactor": [0.2, 0.6, 1.0, 1.0], "metallicFactor": 0.5, "roughnessFactor": 0.3},
                "emissiveFactor": [0.1, 0.3, 0.6],
            },
        ],
        "textures": [{"source": 0, "sampler": 0}],
        "images": [{"uri": "../Textures/Checker.png"}],
        "samplers": [{"magFilter": 9728, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{
            "byteLength": len(buffer),
            "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buffer)).decode("ascii"),
        }],
    }
    return (json.dumps(document, indent="\t") + "\n").encode("utf-8")


def main():
    write_bytes("Textures/Checker.png", make_checker_png())
    write_bytes("Textures/Sky.hdr", make_sky_hdr())
    write_bytes("Audio/Blip.wav", make_blip_wav())
    write_bytes("Fonts/FeatureBlocks.ttf", make_font())
    write_bytes("Models/Platform.gltf", make_platform_gltf())


if __name__ == "__main__":
    main()
