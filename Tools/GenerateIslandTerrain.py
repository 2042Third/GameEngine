#!/usr/bin/env python3
"""Generates the Open World sample's island: its terrain tiles, their palette, and the scatter of its vegetation.

The outputs are committed (CI never runs this script); run it again only to change the island:

    python3 Tools/GenerateIslandTerrain.py terrain --output Samples/OpenWorld/Assets/Terrain
    python3 Tools/GenerateIslandTerrain.py scatter --output <Scatter.json> [--count 20000]

Everything follows from one height function, seeded by SEED, built from the standard library only (struct, zlib, math,
random), so a given Python writes the same files on every run (another zlib may compress the PNG differently):

terrain  writes Island.glb, one glTF 2.0 model with one mesh per tile of a GRID x GRID grid of TILE_SIZE meter tiles
         centered on the origin (tiles that lie wholly below SEA_FLOOR_CUTOFF are left out). A tile is a grid of
         vertices SPACING meters apart whose edge vertices are computed from the same world coordinates as its
         neighbor's, so tiles meet without cracks (import them without LODs: simplified tiles would open seams). Normals
         are 8-bit and texture coordinates 16-bit (KHR_mesh_quantization); every tile shares one index buffer. The
         texture coordinates look up Palette.png by height (u) and slope (v): sand, grass, forest floor, rock and snow.
scatter  writes a command script (editor command steps, StrataEditor --commands or StrataCLI) for an editor that has the
         island's world open: it makes VEGETATION_LAYER the edit layer and loads and pins every cell of that layer over
         the island (world.edit, world.loadCells), so that no instance lands in the persistent scene, then places the
         props with prefab.instantiate steps with instances, one per prefab: seeded Poisson-disc samples on the island,
         each given the prefab that
         suits its height, slope and forest density (palms on beaches, oaks on grass, pines in forests and higher up,
         rocks on slopes, flowers, grass and bushes in clearings), standing on the terrain (the same height function, at
         the exact tile surface: the triangle under the point), turned and scaled at random. The prefabs are named
         PREFAB_DIRECTORY/<name>.stprefab; the script makes them, so it runs in an editor where they exist.
"""

import argparse
import json
import math
import os
import random
import struct
import zlib

SEED = 20261020
GRID = 12                 # Tiles along each axis
TILE_SIZE = 64.0          # Meters: the world's cell size
SPACING = 2.0             # Meters between vertices
TILE_VERTICES = int(TILE_SIZE / SPACING) + 1  # Along one side of a tile (33)
HALF_EXTENT = GRID * TILE_SIZE * 0.5          # 384 m
SEA_FLOOR = -9.0          # Meters: the sea floor far from the island
SEA_FLOOR_CUTOFF = -7.0   # Tiles whose every vertex lies below this are left out
PALETTE_SIZE = (128, 32)  # u: height, v: slope
HEIGHT_RANGE = (-10.0, 80.0)  # Heights the palette's u spans
PREFAB_DIRECTORY = "Prefabs/Nature"
VEGETATION_LAYER = "Vegetation"  # The world layer whose cells hold the scatter

# The vegetation and rocks the scatter places: prefab name, base scale (Nature Kit models are about a meter tall), and
# the scale's random variation.
TREES_BEACH = (("PalmTall", 6.0), ("PalmBend", 6.0), ("PalmDetailed", 5.5))
TREES_GRASS = (("Oak", 6.5), ("TreeDefault", 6.0), ("TreeDetailed", 6.0), ("TreeFat", 5.5), ("TreeTall", 6.0))
TREES_FOREST = (("PineTall", 7.0), ("PineRound", 6.5), ("PineDefault", 6.5), ("TreeCone", 6.0))
TREES_HIGH = (("PineSmall", 6.0), ("PineRound", 5.0))
ROCKS = (("RockLargeA", 4.0), ("RockLargeB", 4.0), ("RockTall", 3.5), ("StoneLarge", 3.5))
GROUND_COVER = (("Grass", 3.0), ("GrassLarge", 3.0), ("GrassLeafs", 3.0), ("FlowerRed", 3.0), ("FlowerYellow", 3.0),
                ("FlowerPurple", 3.0), ("Bush", 3.5), ("BushLarge", 3.0), ("Mushroom", 3.0))


def hash2(x, y, seed):
    """A deterministic integer hash of a lattice point, in [0, 1)."""
    h = (x * 374761393 + y * 668265263 + seed * 2147483647) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    h ^= h >> 16
    return (h & 0xFFFFFF) / float(0x1000000)


def value_noise(x, y, seed):
    """Smoothly interpolated lattice noise in [0, 1)."""
    x0 = math.floor(x)
    y0 = math.floor(y)
    fx = x - x0
    fy = y - y0
    sx = fx * fx * (3.0 - 2.0 * fx)
    sy = fy * fy * (3.0 - 2.0 * fy)
    a = hash2(x0, y0, seed)
    b = hash2(x0 + 1, y0, seed)
    c = hash2(x0, y0 + 1, seed)
    d = hash2(x0 + 1, y0 + 1, seed)
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy


def fbm(x, y, seed, octaves=5):
    """Fractal sum of value noise in [0, 1)."""
    total = 0.0
    amplitude = 0.5
    norm = 0.0
    for octave in range(octaves):
        total += amplitude * value_noise(x, y, seed + octave * 101)
        norm += amplitude
        x *= 2.03
        y *= 2.03
        amplitude *= 0.5
    return total / norm


def smoothstep(edge0, edge1, x):
    t = max(0.0, min(1.0, (x - edge0) / (edge1 - edge0)))
    return t * t * (3.0 - 2.0 * t)


def height_at(x, z):
    """The island's height in meters at world (x, z): sea level is 0."""
    radius = math.hypot(x, z) / HALF_EXTENT
    # A ragged coast with bays, rolling lowlands, and a ridge of peaks across the middle.
    coast = radius + 0.42 * (fbm(x * 0.0075, z * 0.0075, SEED + 7, 4) - 0.5)
    land = smoothstep(0.86, 0.6, coast)
    rolling = fbm(x / 140.0, z / 140.0, SEED + 1)
    ridged = 1.0 - abs(2.0 * fbm(x / 210.0 + 3.1, z / 210.0 - 1.7, SEED + 2) - 1.0)
    mountains = smoothstep(0.68, 1.0, ridged) * smoothstep(0.62, 0.1, radius)
    land_height = 1.5 + 20.0 * rolling * land + 85.0 * mountains * mountains
    return SEA_FLOOR + (land_height - SEA_FLOOR) * land


def forest_density(x, z):
    """How forested a place is, in [0, 1)."""
    return fbm(x / 90.0 + 11.0, z / 90.0 - 5.0, SEED + 3, 4)


class HeightGrid:
    """The height function sampled at every vertex of the island (shared by the tiles, so their edges agree)."""

    def __init__(self):
        self.count = GRID * (TILE_VERTICES - 1) + 1
        self.heights = [[height_at(self.world(i), self.world(j)) for i in range(self.count)] for j in range(self.count)]

    @staticmethod
    def world(index):
        return index * SPACING - HALF_EXTENT

    def at(self, i, j):
        i = max(0, min(self.count - 1, i))
        j = max(0, min(self.count - 1, j))
        return self.heights[j][i]

    def normal(self, i, j):
        dx = (self.at(i + 1, j) - self.at(i - 1, j)) / (2.0 * SPACING)
        dz = (self.at(i, j + 1) - self.at(i, j - 1)) / (2.0 * SPACING)
        length = math.sqrt(dx * dx + 1.0 + dz * dz)
        return (-dx / length, 1.0 / length, -dz / length)

    def surface(self, x, z):
        """The height of the tile surface at (x, z): the triangle of the grid cell under the point."""
        fi = (x + HALF_EXTENT) / SPACING
        fj = (z + HALF_EXTENT) / SPACING
        i = max(0, min(self.count - 2, int(math.floor(fi))))
        j = max(0, min(self.count - 2, int(math.floor(fj))))
        u = fi - i
        v = fj - j
        h00 = self.at(i, j)
        h10 = self.at(i + 1, j)
        h01 = self.at(i, j + 1)
        h11 = self.at(i + 1, j + 1)
        # The cells are split along the diagonal from (i, j) to (i + 1, j + 1), as the index buffer does.
        if u >= v:
            return h00 + (h10 - h00) * u + (h11 - h10) * v
        return h00 + (h11 - h01) * u + (h01 - h00) * v


def palette_color(u, v):
    """The terrain's color (sRGB, 0-255) at palette coordinates: u the height, v the slope."""
    height = HEIGHT_RANGE[0] + (HEIGHT_RANGE[1] - HEIGHT_RANGE[0]) * u
    stops = ((-10.0, (110, 120, 105)), (-1.0, (196, 178, 128)), (2.5, (222, 202, 146)), (4.5, (110, 150, 62)),
             (18.0, (82, 128, 50)), (34.0, (70, 98, 52)), (48.0, (118, 110, 98)), (62.0, (140, 136, 130)),
             (68.0, (236, 240, 244)), (80.0, (246, 248, 250)))
    color = stops[-1][1]
    for (h0, c0), (h1, c1) in zip(stops, stops[1:]):
        if height <= h1:
            t = max(0.0, min(1.0, (height - h0) / (h1 - h0)))
            color = tuple(c0[k] + (c1[k] - c0[k]) * t for k in range(3))
            break
    rock = (112, 106, 98)
    steep = smoothstep(0.35, 0.8, v)
    return tuple(int(round(color[k] + (rock[k] - color[k]) * steep)) for k in range(3))


def write_png(path, width, height, rows):
    """Writes RGB rows as an 8-bit PNG."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(value for pixel in row for value in pixel) for row in rows)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as file:
        file.write(png)


def write_terrain(output):
    grid = HeightGrid()
    os.makedirs(output, exist_ok=True)

    width, height = PALETTE_SIZE
    rows = [[palette_color((x + 0.5) / width, (y + 0.5) / height) for x in range(width)] for y in range(height)]
    write_png(os.path.join(output, "Palette.png"), width, height, rows)

    n = TILE_VERTICES
    indices = []
    for j in range(n - 1):
        for i in range(n - 1):
            a = j * n + i
            b = a + 1
            c = a + n
            d = c + 1
            # Counter-clockwise seen from above (+Y), split along a-d as HeightGrid.surface assumes.
            indices += [a, c, d, a, d, b]
    buffer = bytearray(struct.pack("<%dH" % len(indices), *indices))
    views = [{"buffer": 0, "byteOffset": 0, "byteLength": len(buffer), "target": 34963}]
    accessors = [{"bufferView": 0, "componentType": 5123, "count": len(indices), "type": "SCALAR", "max": [n * n - 1], "min": [0]}]
    meshes = []
    nodes = [{"name": "Island", "children": []}]

    def align():
        while len(buffer) % 4:
            buffer.append(0)

    for tz in range(GRID):
        for tx in range(GRID):
            base_i = tx * (n - 1)
            base_j = tz * (n - 1)
            tile_heights = [grid.at(base_i + i, base_j + j) for j in range(n) for i in range(n)]
            if max(tile_heights) < SEA_FLOOR_CUTOFF:
                continue
            positions = bytearray()
            normals = bytearray()
            uvs = bytearray()
            lo = [math.inf] * 3
            hi = [-math.inf] * 3
            for j in range(n):
                for i in range(n):
                    gi = base_i + i
                    gj = base_j + j
                    p = (HeightGrid.world(gi), grid.at(gi, gj), HeightGrid.world(gj))
                    for k in range(3):
                        lo[k] = min(lo[k], p[k])
                        hi[k] = max(hi[k], p[k])
                    positions += struct.pack("<3f", *p)
                    normal = grid.normal(gi, gj)
                    normals += struct.pack("<4b", *(max(-127, min(127, int(round(c * 127.0)))) for c in normal), 0)
                    u = (p[1] - HEIGHT_RANGE[0]) / (HEIGHT_RANGE[1] - HEIGHT_RANGE[0])
                    v = (1.0 - normal[1]) * 2.5
                    uvs += struct.pack("<2H", *(int(round(max(0.0, min(1.0, c)) * 65535.0)) for c in (u, v)))
            attributes = {}
            for name, data, component, kind, normalized, stride in (("POSITION", positions, 5126, "VEC3", False, 12),
                                                                   ("NORMAL", normals, 5120, "VEC3", True, 4),
                                                                   ("TEXCOORD_0", uvs, 5123, "VEC2", True, 4)):
                align()
                views.append({"buffer": 0, "byteOffset": len(buffer), "byteLength": len(data), "byteStride": stride, "target": 34962})
                buffer.extend(data)
                accessor = {"bufferView": len(views) - 1, "componentType": component, "count": n * n, "type": kind}
                if normalized:
                    accessor["normalized"] = True
                if name == "POSITION":
                    accessor["min"] = lo
                    accessor["max"] = hi
                accessors.append(accessor)
                attributes[name] = len(accessors) - 1
            name = "Tile_%d_%d" % (tx - GRID // 2, tz - GRID // 2)
            meshes.append({"name": name, "primitives": [{"attributes": attributes, "indices": 0, "material": 0}]})
            nodes.append({"name": name, "mesh": len(meshes) - 1})
            nodes[0]["children"].append(len(nodes) - 1)
    align()

    document = {
        "asset": {"version": "2.0", "generator": "Strata Tools/GenerateIslandTerrain.py"},
        "extensionsUsed": ["KHR_mesh_quantization"],
        "extensionsRequired": ["KHR_mesh_quantization"],
        "scene": 0,
        "scenes": [{"name": "Island", "nodes": [0]}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": [{"name": "Terrain", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 0.92}}],
        "textures": [{"sampler": 0, "source": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 33071, "wrapT": 33071}],
        "images": [{"uri": "Palette.png"}],
        "buffers": [{"byteLength": len(buffer)}],
        "bufferViews": views,
        "accessors": accessors,
    }
    text = json.dumps(document, separators=(",", ":")).encode("utf-8")
    text += b" " * ((4 - len(text) % 4) % 4)
    glb = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(buffer))
    glb += struct.pack("<II", len(text), 0x4E4F534A) + text + struct.pack("<II", len(buffer), 0x004E4942) + bytes(buffer)
    with open(os.path.join(output, "Island.glb"), "wb") as file:
        file.write(glb)
    print("Island.glb: %d tiles, %d bytes; Palette.png %dx%d" % (len(meshes), len(glb), width, height))


def pick(rng, choices):
    return choices[rng.randrange(len(choices))]


def write_scatter(output, count):
    grid = HeightGrid()
    rng = random.Random(SEED)
    # Poisson-disc samples by dart throwing on a background grid: no two samples closer than MIN_DISTANCE.
    min_distance = 2.2
    cell = min_distance / math.sqrt(2.0)
    cells = int(math.ceil(2.0 * HALF_EXTENT / cell))
    occupied = {}
    placements = {}
    placed = 0
    attempts = 0
    max_attempts = count * 40
    while placed < count and attempts < max_attempts:
        attempts += 1
        x = rng.uniform(-HALF_EXTENT + 4.0, HALF_EXTENT - 4.0)
        z = rng.uniform(-HALF_EXTENT + 4.0, HALF_EXTENT - 4.0)
        h = grid.surface(x, z)
        if h < 0.6:
            continue  # In the sea or at the water's edge
        ci = int((x + HALF_EXTENT) / cell)
        cj = int((z + HALF_EXTENT) / cell)
        if any((ci + di, cj + dj) in occupied and math.dist(occupied[(ci + di, cj + dj)], (x, z)) < min_distance
               for di in (-2, -1, 0, 1, 2) for dj in (-2, -1, 0, 1, 2)):
            continue
        fi = int(round((x + HALF_EXTENT) / SPACING))
        fj = int(round((z + HALF_EXTENT) / SPACING))
        slope = 1.0 - grid.normal(fi, fj)[1]
        forest = forest_density(x, z)
        roll = rng.random()
        if slope > 0.32:
            choice = pick(rng, ROCKS) if roll < 0.5 else None
        elif h < 3.0:
            choice = pick(rng, TREES_BEACH) if roll < 0.18 else (pick(rng, ROCKS[2:]) if roll < 0.22 else None)
        elif h > 46.0:
            choice = pick(rng, TREES_HIGH) if roll < 0.3 else (pick(rng, ROCKS) if roll < 0.45 else None)
        elif forest > 0.55:
            choice = pick(rng, TREES_FOREST) if roll < 0.45 else (pick(rng, GROUND_COVER[6:]) if roll < 0.75 else None)
        else:
            choice = pick(rng, TREES_GRASS) if roll < 0.12 else (pick(rng, GROUND_COVER) if roll < 0.9 else None)
        if choice is None:
            continue
        occupied[(ci, cj)] = (x, z)
        name, base_scale = choice
        scale = round(base_scale * rng.uniform(0.8, 1.25), 3)
        yaw = round(rng.uniform(0.0, 360.0), 1)
        # Sunk a little so that slopes never show a gap under a trunk or rock.
        y = round(h - 0.05 * scale, 3)
        placements.setdefault(name, []).append({"components": {"Transform": {"Translation": [round(x, 3), y, round(z, 3)],
                                                                             "Rotation": [0.0, yaw, 0.0], "Scale": [scale, scale, scale]}}})
        placed += 1
    # New top-level entities go into the edit layer's cells, which must be loaded (and stay so) while the props arrive.
    steps = [{"command": "world.edit", "parameters": {"editLayer": VEGETATION_LAYER},
              "expect": {"/editLayer": {"equals": VEGETATION_LAYER}}},
             {"command": "world.loadCells", "parameters": {"region": {"min": [-HALF_EXTENT, -HALF_EXTENT], "max": [HALF_EXTENT, HALF_EXTENT]},
                                                           "layers": [VEGETATION_LAYER], "pin": True, "wait": True}}]
    steps += [{"command": "prefab.instantiate", "parameters": {"prefab": "%s/%s.stprefab" % (PREFAB_DIRECTORY, name), "instances": instances}}
              for name, instances in sorted(placements.items())]
    with open(output, "w", encoding="utf-8", newline="\n") as file:
        json.dump(steps, file, separators=(",", ":"))
        file.write("\n")
    print("%s: %d instances of %d prefabs (%d attempts)" % (output, placed, len(placements), attempts))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    terrain = commands.add_parser("terrain", help="Write Island.glb and Palette.png")
    terrain.add_argument("--output", required=True, help="The directory to write them into")
    scatter = commands.add_parser("scatter", help="Write the vegetation's command script, for an editor with the island's world open")
    scatter.add_argument("--output", required=True, help="The command script to write (.json)")
    scatter.add_argument("--count", type=int, default=20000, help="Instances to place (default 20,000)")
    arguments = parser.parse_args()
    if arguments.command == "terrain":
        write_terrain(arguments.output)
    else:
        if arguments.count < 1 or arguments.count > 100000:
            parser.error("--count must be from 1 to 100,000")
        write_scatter(arguments.output, arguments.count)


if __name__ == "__main__":
    main()
