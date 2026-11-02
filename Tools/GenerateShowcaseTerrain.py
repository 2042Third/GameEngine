#!/usr/bin/env python3
"""Generates the Showcase sample's ground (High Desert Outpost) and checks the design that its shots rest on.

The outputs are committed and a CTest (StrataTools.ShowcaseTerrain) keeps them equal to what this script writes:

    python3 -I Tools/GenerateShowcaseTerrain.py terrain --output Samples/Showcase/Assets/Generated/Terrain
    python3 -I Tools/GenerateShowcaseTerrain.py check
    python3 -I Tools/GenerateShowcaseTerrain.py stations [--json <file>]
    python3 -I Tools/GenerateShowcaseTerrain.py selftest --committed Samples/Showcase/Assets/Generated/Terrain --work <dir>

Everything follows from one height function, seeded by SEED and built from the standard library only. It uses only
+ - * /, floor, sqrt and integer hashing (value noise with smoothstep polynomials), never sin, cos, exp or atan, and every
vertex attribute is quantized before it is packed (positions to 1/4096 m, normals to 8-bit SNORM, texture coordinates to
1/65536), so runs on every platform write the same bytes. Meters, +Y up, the yard's ground at y = 0; azimuths are degrees
from +X toward +Z, as lighting.applySkyPreset's sunAzimuth. The valley's axes are u = (x + z) / sqrt(2), down the valley
toward the golden-hour sun (azimuth 45), and v = (z - x) / sqrt(2) across it (+v is azimuth 135, the dawn side).

terrain   runs check, then writes four glTF 2.0 models (nothing on a failure):
          Terrain.glb  the inner terrain, 36 tiles of 50 m (nodes Ground_<i>_<j>) over -150..150 at 1 m spacing, without
                       the triangles the apron covers, with a 1.5 m skirt down the outer edges; and the ring, 80 tiles of
                       300 m (nodes Ring_<k>) over -1350..1350 at 10 m spacing around it. The inner terrain blends into
                       the ring's surface over its last 20 m, so the two meet exactly. Import it without LODs: simplified
                       tiles would open seams.
          Apron.glb    the asphalt apron (node Apron, a 0.5 m grid) with its potholes and shallow puddles (node
                       ApronFine, a 0.25 m grid over the cells around them, flat where it meets Apron so the T-junctions
                       do not crack), a 6 cm bevel and a skirt down to 5 cm under the terrain. It drains toward +X.
          Water.glb    one flat disc per pothole and puddle (nodes Puddle_<n>) at its water level, the lowest point of its
                       rim minus 1.5 cm, 10% wider than its hole: the asphalt above the water hides the rest.
          Mud.glb      eight mud patches (nodes Mud_<n>) along the apron's edge and under the bus's front, draped 1.5 cm
                       above the surface under them.
          Every model has factor-only materials (Ground, Ring, Asphalt, Water, Mud) that the sample's .stmat files
          replace; texture coordinates are in meters (u = x, v = z).
check     evaluates the design's rules on the generated surfaces and exits 1 when one fails:
          1. from every apron point (a 2 m grid, 0.3 m above it) the terrain stays at most 4.0 degrees high toward
             azimuths 30-60 (the golden sun is 6 degrees up), 2. from shot 1's camera at most 3.5 degrees toward
             azimuths 135-145 (the dawn sun is 5), 3. every pad's footprint is flat (at most 3 cm and 2 degrees),
             4. the inner terrain meets the ring within 2 cm, 5. within 140 m slopes above 45 degrees lie only in the
             cliff band (reported, not fatal), 6. the apron's edge stays at least 2 cm above the terrain and its skirt
             reaches at least 3 cm below it.
stations  prints the three times of day (Dawn, Golden, Dusk) and, per shot of the tour, its cameras (key, from, to),
          look target, field of view, where its subjects fall in a 16:9 picture and the mirror points of shots 2-4
          (where the camera sees the sun reflected); --json writes the same as JSON.
selftest  check, terrain twice (byte-equal), each model byte-equal to the committed one (--committed), and shots 2-4's
          mirror relations within 1 cm and 0.1 degree. Exit codes: 0 done, 1 a check failed, 2 usage.
"""

import argparse
import json
import math
import os
import random
import struct
import sys

SEED = 20261101
SQRT_HALF = math.sqrt(0.5)

# Inner terrain: 6 x 6 tiles of 50 m at 1 m spacing; the ring: 9 x 9 tiles of 300 m at 10 m spacing without the center.
INNER_TILES = 6
INNER_TILE_SIZE = 50
INNER_HALF = INNER_TILES * INNER_TILE_SIZE // 2   # 150 m
INNER_SPACING = 1.0
RING_TILES = 9
RING_TILE_SIZE = 300
RING_HALF = RING_TILES * RING_TILE_SIZE // 2      # 1350 m
RING_SPACING = 10.0
RING_TILE_VERTICES = int(RING_TILE_SIZE / RING_SPACING) + 1  # 31
SEAM_BLEND = (130.0, 150.0)  # The inner terrain turns into the ring's surface between these (max(|x|, |z|))
SKIRT_DEPTH = 1.5

# The apron (asphalt): a rounded rectangle with a noisy outline, its top draining toward +X.
APRON_X = (-14.0, 26.0)
APRON_Z = (-16.0, 24.0)
APRON_CORNER = 6.0
APRON_NOISE = 0.8
APRON_SPACING = 0.5
APRON_FINE_SPACING = 0.25
APRON_BEVEL_WIDTH = 0.06
APRON_BEVEL_DROP = 0.015
APRON_SKIRT_BELOW = 0.05

# Pads: footprints the terrain is flat under (rule 3). The terrain is 0 within PAD_MARGIN of a footprint (the check
# samples the grid bilinearly, so a footprint's edge reads grid points up to sqrt(2) m away) and blends to the height
# function over PAD_BLEND beyond.
PAD_MARGIN = 1.5
PAD_BLEND = 6.0
APRON_PAD_GROWTH = 1.0  # The apron's footprint is its outline grown by 1 m

# The layout's anchors (the showcase spec's D3; the props generator and the layout use the same numbers).
BUS = {"center": (11.8, 0.1), "half": (1.22, 5.4), "yaw": 0}
OIL_TANK = {"center": (2.7, 17.2), "radius": 4.0, "pad": 4.35, "pad_height": 0.3, "shell": 7.2}
FUEL_TANK = {"center": (-10.0, -20.0), "half": (1.4, 3.9), "yaw": -45, "axis_height": 1.75}
HUT = {"center": (-22.0, -18.0), "half": (2.6, 2.2), "yaw": 45}
LEAN_TO = {"local": (3.9, 0.0), "half": (1.5, 2.6)}
FIRE_PIT = {"center": (-15.0, -14.0), "radius": 0.75}
DRUM_CLUSTERS = (((-2.0, 20.0), 1.5), ((-17.5, -21.5), 1.2), ((9.5, 7.5), 1.2))
LANTERN_A = (-20.76, 2.0, -15.63)
POTHOLE_ORIGIN = (-10.0, -10.0)
CLIFF_BAND = {"u": (-60.0, -36.0), "v": (-80.0, 60.0)}
WALK_RADIUS = 140.0

# Bus letterboard: the vertical band of the +X side where shot 2's highlight runs (the props generator builds it).
LETTERBOARD = {"x": BUS["center"][0] + BUS["half"][0], "y": (2.25, 2.62), "z": (BUS["center"][1] - BUS["half"][1], BUS["center"][1] + BUS["half"][1])}

# The three times of day (the showcase spec's D4): name, photo hour, sun elevation, elevation the light is drawn at,
# azimuth, light scale, fog density, exposure compensation (EV), shadow distance (m), local lights on.
STATIONS = (
    ("Dawn", 6.5, 5.0, 5.0, 140.0, 1.0, 0.006, 0.0, 600.0, False),
    ("Golden", 17.75, 6.0, 6.0, 45.0, 1.0, 0.0006, 0.0, 200.0, False),
    ("Dusk", 19.0, 1.0, 2.0, 32.0, 0.5, 0.0015, -0.7, 150.0, True),
)
SUN_LUX = 120000.0


class CheckFailed(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------------------
# Noise and shaping (no transcendental functions: these feed the vertex positions)

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
    top = a + (b - a) * sx
    return top + ((c + (d - c) * sx) - top) * sy


def fbm(x, y, seed, octaves=4):
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


def periodic_noise(s, period, seed):
    """1D value noise in [-1, 1) with an integer period (in lattice cells), for closed outlines."""
    i0 = math.floor(s)
    f = s - i0
    t = f * f * (3.0 - 2.0 * f)
    a = hash2(i0 % period, 0, seed)
    b = hash2((i0 + 1) % period, 0, seed)
    return 2.0 * (a + (b - a) * t) - 1.0


def smoothstep(edge0, edge1, x):
    """0 at edge0, 1 at edge1 (reversed when edge0 > edge1), smooth in between."""
    t = (x - edge0) / (edge1 - edge0)
    t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
    return t * t * (3.0 - 2.0 * t)


def quantize(value, steps):
    return round(value * steps) / steps


def yaw_axes(degrees):
    """(cos, sin) of the yaws the layout uses, without trigonometry (they feed the height function)."""
    table = {0: (1.0, 0.0), 45: (SQRT_HALF, SQRT_HALF), -45: (SQRT_HALF, -SQRT_HALF), 90: (0.0, 1.0)}
    return table[degrees]


def local_to_world(center, yaw, lx, lz):
    """A point of an object's local frame (x right, z forward) in the world, for a yaw about +Y."""
    c, s = yaw_axes(yaw)
    return (center[0] + lx * c + lz * s, center[1] - lx * s + lz * c)


# ---------------------------------------------------------------------------------------------------------------------
# Shapes: the apron's outline and the pads' footprints, as signed distances (negative inside)

def build_apron_outline():
    """The apron's outline: a rounded rectangle (rational arcs, no trigonometry) pushed out and in by periodic noise."""
    x0, x1 = APRON_X
    z0, z1 = APRON_Z
    r = APRON_CORNER
    base = []  # (x, z, nx, nz)

    def edge(ax, az, bx, bz, nx, nz):
        length = math.sqrt((bx - ax) ** 2 + (bz - az) ** 2)
        steps = int(math.ceil(length / 1.0))
        for k in range(steps):
            t = k / steps
            base.append((ax + (bx - ax) * t, az + (bz - az) * t, nx, nz))

    def arc(cx, cz, na, nb):
        steps = 12
        for k in range(steps):
            t = k / steps
            c = (1.0 - t * t) / (1.0 + t * t)
            s = 2.0 * t / (1.0 + t * t)
            nx = c * na[0] + s * nb[0]
            nz = c * na[1] + s * nb[1]
            base.append((cx + r * nx, cz + r * nz, nx, nz))

    edge(x0 + r, z0, x1 - r, z0, 0.0, -1.0)
    arc(x1 - r, z0 + r, (0.0, -1.0), (1.0, 0.0))
    edge(x1, z0 + r, x1, z1 - r, 1.0, 0.0)
    arc(x1 - r, z1 - r, (1.0, 0.0), (0.0, 1.0))
    edge(x1 - r, z1, x0 + r, z1, 0.0, 1.0)
    arc(x0 + r, z1 - r, (0.0, 1.0), (-1.0, 0.0))
    edge(x0, z1 - r, x0, z0 + r, -1.0, 0.0)
    arc(x0 + r, z0 + r, (-1.0, 0.0), (0.0, -1.0))

    lengths = [0.0]
    for i in range(1, len(base)):
        lengths.append(lengths[-1] + math.sqrt((base[i][0] - base[i - 1][0]) ** 2 + (base[i][1] - base[i - 1][1]) ** 2))
    total = lengths[-1] + math.sqrt((base[0][0] - base[-1][0]) ** 2 + (base[0][1] - base[-1][1]) ** 2)
    outline = []
    for (x, z, nx, nz), s in zip(base, lengths):
        f = 0.7 * periodic_noise(s / total * 18.0, 18, SEED + 11) + 0.3 * periodic_noise(s / total * 45.0, 45, SEED + 12)
        outline.append((x + APRON_NOISE * f * nx, z + APRON_NOISE * f * nz))
    return outline


class Polygon:
    """A closed polygon in (x, z) with its signed distance (negative inside)."""

    def __init__(self, points):
        self.points = points
        self.segments = []
        n = len(points)
        for i in range(n):
            ax, az = points[i]
            bx, bz = points[(i + 1) % n]
            dx = bx - ax
            dz = bz - az
            self.segments.append((ax, az, dx, dz, dx * dx + dz * dz))
        xs = [p[0] for p in points]
        zs = [p[1] for p in points]
        self.bounds = (min(xs), min(zs), max(xs), max(zs))
        self.center = (sum(xs) / n, sum(zs) / n)

    def inside(self, px, pz):
        crossings = False
        for ax, az, dx, dz, _ in self.segments:
            bz = az + dz
            if (az > pz) != (bz > pz):
                if px < ax + (pz - az) * dx / dz:
                    crossings = not crossings
        return crossings

    def distance(self, px, pz):
        best = math.inf
        for ax, az, dx, dz, length2 in self.segments:
            t = ((px - ax) * dx + (pz - az) * dz) / length2
            t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
            ex = ax + t * dx - px
            ez = az + t * dz - pz
            d2 = ex * ex + ez * ez
            if d2 < best:
                best = d2
        return math.sqrt(best)

    def signed_distance(self, px, pz, cap=math.inf):
        """Negative inside; at least `cap` (returned as is) for points farther than cap from the bounds."""
        x0, z0, x1, z1 = self.bounds
        ox = max(x0 - px, 0.0, px - x1)
        oz = max(z0 - pz, 0.0, pz - z1)
        if ox * ox + oz * oz >= cap * cap:
            return cap
        d = self.distance(px, pz)
        return -d if self.inside(px, pz) else d

    def ray_to_outline(self, px, pz):
        """The point of the outline on the segment from the center to (px, pz) nearest to (px, pz)."""
        cx, cz = self.center
        rx = px - cx
        rz = pz - cz
        best = None
        for ax, az, dx, dz, _ in self.segments:
            denominator = rx * dz - rz * dx
            if abs(denominator) < 1e-12:
                continue
            qx = ax - cx
            qz = az - cz
            t = (qx * dz - qz * dx) / denominator   # Along the ray
            s = (qx * rz - qz * rx) / denominator   # Along the segment
            if 0.0 <= s <= 1.0 and 0.0 <= t <= 1.0 and (best is None or t > best):
                best = t
        if best is None:
            return (px, pz)
        return (cx + rx * best, cz + rz * best)


def box_distance(center, half, yaw, px, pz):
    c, s = yaw_axes(yaw)
    dx = px - center[0]
    dz = pz - center[1]
    lx = abs(dx * c - dz * s) - half[0]
    lz = abs(dx * s + dz * c) - half[1]
    outside = math.sqrt(max(lx, 0.0) ** 2 + max(lz, 0.0) ** 2)
    return outside + min(max(lx, lz), 0.0)


def circle_distance(center, radius, px, pz):
    return math.sqrt((px - center[0]) ** 2 + (pz - center[1]) ** 2) - radius


class Layout:
    """The apron's outline and the pads, shared by the terrain, the apron and the checks."""

    def __init__(self):
        self.outline = Polygon(build_apron_outline())
        lean_center = local_to_world(HUT["center"], HUT["yaw"], *LEAN_TO["local"])
        x0, z0, x1, z1 = self.outline.bounds
        apron_box = ((x0 + x1) * 0.5, (z0 + z1) * 0.5, max(x1 - x0, z1 - z0) * 0.5 + APRON_PAD_GROWTH)
        # name, signed distance to the footprint (negative inside), the footprint's bounds as (center x, center z, half
        # extent). Distances are only needed up to the reach of the detail noise's fade (PAD_MARGIN + 30 m).
        self.pads = [
            ("Apron", lambda x, z: self.outline.signed_distance(x, z, 34.0) - APRON_PAD_GROWTH, apron_box),
            ("Bus", lambda x, z: box_distance(BUS["center"], BUS["half"], BUS["yaw"], x, z), BUS["center"] + (5.5,)),
            ("OilTank", lambda x, z: circle_distance(OIL_TANK["center"], OIL_TANK["pad"], x, z), OIL_TANK["center"] + (4.5,)),
            ("FuelTank", lambda x, z: box_distance(FUEL_TANK["center"], FUEL_TANK["half"], FUEL_TANK["yaw"], x, z),
             FUEL_TANK["center"] + (4.0,)),
            ("Hut", lambda x, z: box_distance(HUT["center"], HUT["half"], HUT["yaw"], x, z), HUT["center"] + (3.5,)),
            ("LeanTo", lambda x, z: box_distance(lean_center, LEAN_TO["half"], HUT["yaw"], x, z), lean_center + (3.0,)),
            ("FirePit", lambda x, z: circle_distance(FIRE_PIT["center"], FIRE_PIT["radius"], x, z), FIRE_PIT["center"] + (1.0,)),
        ]
        for index, (center, radius) in enumerate(DRUM_CLUSTERS):
            self.pads.append(("Drums%d" % index, lambda x, z, c=center, r=radius: circle_distance(c, r, x, z), center + (radius + 0.5,)))

    def pad_distance(self, x, z):
        """Distance from (x, z) to the flattened ground around the pads (0 on it)."""
        best = min(distance(x, z) for _, distance, _ in self.pads)
        return max(best - PAD_MARGIN, 0.0)


# ---------------------------------------------------------------------------------------------------------------------
# The height function

def valley_axes(x, z):
    return ((x + z) * SQRT_HALF, (z - x) * SQRT_HALF)


def base_height(x, z):
    """The valley without pads: floor, walls and noise (meters)."""
    u, v = valley_axes(x, z)
    floor_height = -0.02 * max(u - 30.0, 0.0)
    rim = 26.0 * smoothstep(-28.0, -68.0, u)
    right = 18.0 * smoothstep(55.0, 110.0, v)
    left = 12.0 * smoothstep(-70.0, -130.0, v)
    # The far hills and the mountains vary along their length, so the skyline has peaks and saddles, not a flat top.
    far = 45.0 * smoothstep(700.0, 1150.0, u)
    if far > 0.0:
        far *= 0.45 + 1.1 * fbm(v / 260.0, u / 260.0, SEED + 6, 3)
    behind = 140.0 * smoothstep(300.0, 1100.0, -u)
    if behind > 0.0:
        behind *= 0.6 + 0.8 * fbm(v / 320.0, u / 320.0, SEED + 7, 3)
    side = 70.0 * smoothstep(260.0, 900.0, abs(v))
    if side > 0.0:
        side *= 0.55 + 0.9 * fbm(u / 300.0, v / 300.0, SEED + 8, 3)
    mountains = max(behind, side)
    height = floor_height + max(rim, right, left, far, mountains)
    extent = max(abs(x), abs(z))
    ring_weight = smoothstep(170.0, 260.0, extent)
    if ring_weight > 0.0:
        height += 8.0 * (fbm(x / 300.0, z / 300.0, SEED + 2) - 0.5) * ring_weight
    return height


def height(layout, x, z):
    """The terrain's height at (x, z): base_height, with the detail noise and the pads (flat at 0, blended over 6 m)."""
    h = base_height(x, z)
    extent = max(abs(x), abs(z))
    if extent > 160.0:
        return h
    pad = layout.pad_distance(x, z)
    weight = smoothstep(8.0, 30.0, pad) * smoothstep(145.0, 120.0, extent)
    if weight > 0.0:
        h += 1.6 * (fbm(x / 40.0, z / 40.0, SEED + 1) - 0.5) * weight
    return h * smoothstep(0.0, PAD_BLEND, pad)


def normal_from_differences(dx, dz, spacing):
    nx = -dx / (2.0 * spacing)
    nz = -dz / (2.0 * spacing)
    length = math.sqrt(nx * nx + 1.0 + nz * nz)
    return (nx / length, 1.0 / length, nz / length)


class Ground:
    """The terrain sampled where its meshes have vertices: the ring's lattice (10 m) and the inner grid (1 m)."""

    def __init__(self, layout):
        self.layout = layout
        # The ring's lattice over the whole extent (the inner terrain blends into its surface).
        self.ring_count = int(2 * RING_HALF / RING_SPACING) + 1
        self.ring = [[base_height(self.ring_world(i), self.ring_world(j)) for i in range(self.ring_count)]
                     for j in range(self.ring_count)]
        self.ring_normals = [[self._ring_normal(i, j) for i in range(self.ring_count)] for j in range(self.ring_count)]
        # The inner grid, one sample beyond its edges for the normals.
        self.inner_count = int(2 * INNER_HALF / INNER_SPACING) + 1
        self.inner = []
        self.blend = []
        for j in range(-1, self.inner_count + 1):
            row = []
            weights = []
            for i in range(-1, self.inner_count + 1):
                x = self.inner_world(i)
                z = self.inner_world(j)
                w = smoothstep(SEAM_BLEND[0], SEAM_BLEND[1], max(abs(x), abs(z)))
                h = height(layout, x, z) if w < 1.0 else 0.0
                if w > 0.0:
                    h = h + (self.ring_surface(x, z) - h) * w
                row.append(h)
                weights.append(w)
            self.inner.append(row)
            self.blend.append(weights)

    @staticmethod
    def ring_world(index):
        return index * RING_SPACING - RING_HALF

    @staticmethod
    def inner_world(index):
        return index * INNER_SPACING - INNER_HALF

    def ring_at(self, i, j):
        i = max(0, min(self.ring_count - 1, i))
        j = max(0, min(self.ring_count - 1, j))
        return self.ring[j][i]

    def _ring_normal(self, i, j):
        return normal_from_differences(self.ring_at(i + 1, j) - self.ring_at(i - 1, j),
                                       self.ring_at(i, j + 1) - self.ring_at(i, j - 1), RING_SPACING)

    def inner_at(self, i, j):
        """Inner height at grid index (i, j), -1..count (the extra samples)."""
        return self.inner[j + 1][i + 1]

    @staticmethod
    def _triangle(h00, h10, h01, h11, u, v):
        # Cells are split along the diagonal from (i, j) to (i + 1, j + 1), as the index buffers are.
        if u >= v:
            return h00 + (h10 - h00) * u + (h11 - h10) * v
        return h00 + (h11 - h01) * u + (h01 - h00) * v

    def ring_surface(self, x, z):
        fi = (x + RING_HALF) / RING_SPACING
        fj = (z + RING_HALF) / RING_SPACING
        i = max(0, min(self.ring_count - 2, int(math.floor(fi))))
        j = max(0, min(self.ring_count - 2, int(math.floor(fj))))
        return self._triangle(self.ring[j][i], self.ring[j][i + 1], self.ring[j + 1][i], self.ring[j + 1][i + 1], fi - i, fj - j)

    def ring_normal_at(self, x, z):
        fi = (x + RING_HALF) / RING_SPACING
        fj = (z + RING_HALF) / RING_SPACING
        i = max(0, min(self.ring_count - 2, int(math.floor(fi))))
        j = max(0, min(self.ring_count - 2, int(math.floor(fj))))
        n = [self._triangle(self.ring_normals[j][i][k], self.ring_normals[j][i + 1][k], self.ring_normals[j + 1][i][k],
                            self.ring_normals[j + 1][i + 1][k], fi - i, fj - j) for k in range(3)]
        length = math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2])
        return (n[0] / length, n[1] / length, n[2] / length)

    def inner_normal(self, i, j):
        fine = normal_from_differences(self.inner_at(i + 1, j) - self.inner_at(i - 1, j),
                                       self.inner_at(i, j + 1) - self.inner_at(i, j - 1), INNER_SPACING)
        w = self.blend[j + 1][i + 1]
        if w <= 0.0:
            return fine
        coarse = self.ring_normal_at(self.inner_world(i), self.inner_world(j))
        n = [fine[k] + (coarse[k] - fine[k]) * w for k in range(3)]
        length = math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2])
        return (n[0] / length, n[1] / length, n[2] / length)

    def surface(self, x, z):
        """The meshes' surface at (x, z): the inner grid's triangle inside, the ring's outside."""
        if max(abs(x), abs(z)) <= INNER_HALF:
            fi = (x + INNER_HALF) / INNER_SPACING
            fj = (z + INNER_HALF) / INNER_SPACING
            i = max(0, min(self.inner_count - 2, int(math.floor(fi))))
            j = max(0, min(self.inner_count - 2, int(math.floor(fj))))
            return self._triangle(self.inner_at(i, j), self.inner_at(i + 1, j), self.inner_at(i, j + 1),
                                  self.inner_at(i + 1, j + 1), fi - i, fj - j)
        return self.ring_surface(x, z)


# ---------------------------------------------------------------------------------------------------------------------
# The apron: holes, the top surface, the meshes

class Hole:
    """A pothole or a shallow puddle: a smooth bowl with a slightly irregular outline (a polynomial in the direction)."""

    def __init__(self, center, radius, depth, shape):
        self.center = center
        self.radius = radius
        self.depth = depth
        self.shape = shape  # Coefficients of cos, sin, cos 2, sin 2 of the direction
        self.reach = radius * (1.0 + sum(abs(c) for c in shape))
        self.water_level = None

    def radius_toward(self, dx, dz):
        a, b, c, d = self.shape
        return self.radius * (1.0 + a * dx + b * dz + c * (dx * dx - dz * dz) + d * 2.0 * dx * dz)

    def depth_at(self, x, z):
        dx = x - self.center[0]
        dz = z - self.center[1]
        d2 = dx * dx + dz * dz
        if d2 >= self.reach * self.reach:
            return 0.0
        d = math.sqrt(d2)
        if d < 1e-9:
            return self.depth
        r = self.radius_toward(dx / d, dz / d)
        return self.depth * smoothstep(r, 0.0, d)


def apron_plane(x):
    """The apron's top without holes: 4 cm above the ground at x 26, rising 2 mm a meter toward -X (drains toward +X)."""
    return 0.04 + 0.002 * (26.0 - x)


def place_holes(layout):
    """The shallow puddles on azimuth 45 from POTHOLE_ORIGIN (shot 4's glint runs over them), then 14 potholes in the
    wedge of azimuths 18-72 at 3-22 m, apart from each other and from what stands on the apron."""
    rng = random.Random(SEED + 3)
    holes = []
    keep_out = [lambda x, z, r: box_distance(BUS["center"], BUS["half"], BUS["yaw"], x, z) < r + 0.6,
                lambda x, z, r: circle_distance(OIL_TANK["center"], OIL_TANK["pad"], x, z) < r + 0.6]
    keep_out += [lambda x, z, r, c=c, cr=cr: circle_distance(c, cr, x, z) < r + 0.4 for c, cr in DRUM_CLUSTERS]

    def shape():
        return tuple(round(rng.uniform(-0.07, 0.07), 4) for _ in range(4))

    def fits(hole, gap):
        x, z = hole.center
        # The fine cells reach 1 m beyond a hole (and up to a cell more); they must stay clear of the outline.
        if layout.outline.signed_distance(x, z) > -(hole.reach + 3.0):
            return False
        if any(blocked(x, z, hole.reach) for blocked in keep_out):
            return False
        return all(math.sqrt((x - h.center[0]) ** 2 + (z - h.center[1]) ** 2) > hole.reach + h.reach + gap for h in holes)

    # The first puddle holds the glint's mirror point (about 6.3 m out from shot 4's camera at POTHOLE_ORIGIN); the
    # others lie further along azimuth 45, beside the line where they would overlap.
    across = (-SQRT_HALF, SQRT_HALF)
    for near, far, side in ((5.9, 6.5, 0.3), (8.5, 11.5, 3.5), (11.5, 14.0, 3.5)):
        for _ in range(2000):
            d = rng.uniform(near, far)
            offset = rng.uniform(-side, side)
            x = round(POTHOLE_ORIGIN[0] + d * SQRT_HALF + offset * across[0], 3)
            z = round(POTHOLE_ORIGIN[1] + d * SQRT_HALF + offset * across[1], 3)
            hole = Hole((x, z), round(rng.uniform(2.0, 3.5), 3), round(rng.uniform(0.03, 0.04), 4), shape())
            if fits(hole, 0.4):
                holes.append(hole)
                break
        else:
            raise CheckFailed("no room for the shallow puddle at %.1f-%.1f m" % (near, far))
    potholes = 0
    attempts = 0
    while potholes < 14:
        attempts += 1
        if attempts > 20000:
            raise CheckFailed("no room for 14 potholes")
        azimuth = math.radians(rng.uniform(18.0, 72.0))
        d = rng.uniform(3.0, 22.0)
        x = round(POTHOLE_ORIGIN[0] + d * math.cos(azimuth), 3)
        z = round(POTHOLE_ORIGIN[1] + d * math.sin(azimuth), 3)
        # Mostly small holes, a few large ones (0.7-2.6 m, squared toward the small end), so fourteen fit the wedge.
        size = rng.random()
        hole = Hole((x, z), round(0.7 + 1.9 * size * size, 3), round(rng.uniform(0.05, 0.12), 4), shape())
        if fits(hole, 0.35):
            holes.append(hole)
            potholes += 1
    for hole in holes:
        # The lowest point of the rim (the plane falls toward +X), less 1.5 cm.
        rim = min(apron_plane(hole.center[0] + hole.radius_toward(math.cos(a), math.sin(a)) * math.cos(a))
                  for a in (k * math.pi / 32.0 for k in range(64)))
        hole.water_level = quantize(rim - 0.015, 4096)
    return holes


class MeshData:
    def __init__(self):
        self.positions = []
        self.normals = []
        self.uvs = []
        self.indices = []
        self.lookup = {}

    def add(self, position, normal, uv=None, key=None):
        if key is not None and key in self.lookup:
            return self.lookup[key]
        p = tuple(quantize(c, 4096) for c in position)
        self.positions.append(p)
        self.normals.append(normal)
        self.uvs.append(uv if uv is not None else (p[0], p[2]))
        index = len(self.positions) - 1
        if key is not None:
            self.lookup[key] = index
        return index

    def triangle(self, a, b, c, up=None):
        """Adds a triangle, wound so that its normal points along `up` (counter-clockwise seen from there); degenerate
        triangles are left out."""
        pa, pb, pc = self.positions[a], self.positions[b], self.positions[c]
        e1 = (pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2])
        e2 = (pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2])
        n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        if n[0] * n[0] + n[1] * n[1] + n[2] * n[2] < 1e-12:
            return
        if up is not None and n[0] * up[0] + n[1] * up[1] + n[2] * up[2] < 0.0:
            b, c = c, b
        self.indices += [a, b, c]


class Apron:
    def __init__(self, layout, ground):
        self.layout = layout
        self.ground = ground
        self.holes = place_holes(layout)
        outline = layout.outline
        x0, z0, x1, z1 = outline.bounds
        s = APRON_SPACING
        self.i0 = int(math.floor(x0 / s)) - 1
        self.j0 = int(math.floor(z0 / s)) - 1
        self.i1 = int(math.ceil(x1 / s)) + 1
        self.j1 = int(math.ceil(z1 / s)) + 1
        self.inside = {}
        for j in range(self.j0, self.j1 + 1):
            for i in range(self.i0, self.i1 + 1):
                self.inside[(i, j)] = outline.inside(i * s, j * s)
        # Cells near a hole are fine cells (0.25 m); every corner of theirs is well inside the outline.
        self.fine_cells = set()
        for hole in self.holes:
            reach = hole.reach + 1.0
            for j in range(int(math.floor((hole.center[1] - reach) / s)), int(math.ceil((hole.center[1] + reach) / s))):
                for i in range(int(math.floor((hole.center[0] - reach) / s)), int(math.ceil((hole.center[0] + reach) / s))):
                    self.fine_cells.add((i, j))
        for i, j in self.fine_cells:
            for ci, cj in ((i, j), (i + 1, j), (i, j + 1), (i + 1, j + 1)):
                if outline.signed_distance(ci * s, cj * s) > -0.5:
                    raise CheckFailed("a hole's fine cells reach the apron's edge at (%.2f, %.2f)" % (ci * s, cj * s))
        self.boundary = []  # Outline vertices of the top (x, z, top height), for rule 6
        self.skirt_bottom = []  # (x, z, bottom height) of the skirt
        self.top_mesh, self.fine_mesh = self._build()

    def top(self, x, z):
        return apron_plane(x) - sum(hole.depth_at(x, z) for hole in self.holes)

    def top_normal(self, x, z):
        e = 0.05
        return normal_from_differences(self.top(x + e, z) - self.top(x - e, z), self.top(x, z + e) - self.top(x, z - e), e)

    def _build(self):
        s = APRON_SPACING
        outline = self.layout.outline
        coarse = MeshData()
        kept = set()
        for j in range(self.j0, self.j1):
            for i in range(self.i0, self.i1):
                if (i, j) in self.fine_cells:
                    continue
                corners = ((i, j), (i + 1, j), (i, j + 1), (i + 1, j + 1))
                if any(self.inside[c] for c in corners):
                    kept.add((i, j))

        def vertex(ci, cj):
            x = ci * s
            z = cj * s
            if not self.inside[(ci, cj)]:
                x, z = outline.ray_to_outline(x, z)
            return coarse.add((x, self.top(x, z), z), self.top_normal(x, z), key=(ci, cj))

        up = (0.0, 1.0, 0.0)
        for i, j in sorted(kept, key=lambda c: (c[1], c[0])):
            a = vertex(i, j)
            b = vertex(i + 1, j)
            c = vertex(i, j + 1)
            d = vertex(i + 1, j + 1)
            coarse.triangle(a, c, d, up)
            coarse.triangle(a, d, b, up)

        # The outer edges: a cell edge whose neighbor across is neither kept nor fine. Every such edge joins two outline
        # vertices (an edge with a vertex inside the outline is shared by two kept cells).
        boundary_edges = []
        for i, j in kept:
            for (ai, aj), (bi, bj), (ni, nj) in ((((i, j), (i + 1, j), (i, j - 1))), (((i + 1, j), (i + 1, j + 1), (i + 1, j))),
                                                 (((i + 1, j + 1), (i, j + 1), (i, j + 1))), (((i, j + 1), (i, j), (i - 1, j)))):
                if (ni, nj) not in kept and (ni, nj) not in self.fine_cells:
                    boundary_edges.append((coarse.lookup[(ai, aj)], coarse.lookup[(bi, bj)], (i + 0.5) * s, (j + 0.5) * s))
        self._add_edge(coarse, boundary_edges)

        fine = MeshData()
        f = APRON_FINE_SPACING
        for i, j in sorted(self.fine_cells, key=lambda c: (c[1], c[0])):
            for sj in range(2):
                for si in range(2):
                    fi = 2 * i + si
                    fj = 2 * j + sj
                    ids = []
                    for ci, cj in ((fi, fj), (fi + 1, fj), (fi, fj + 1), (fi + 1, fj + 1)):
                        x = ci * f
                        z = cj * f
                        ids.append(fine.add((x, self.top(x, z), z), self.top_normal(x, z), key=(ci, cj)))
                    fine.triangle(ids[0], ids[2], ids[3], up)
                    fine.triangle(ids[0], ids[3], ids[1], up)
        return coarse, fine

    def _add_edge(self, mesh, edges):
        """The bevel (6 cm out, 1.5 cm down) and the skirt (down to 5 cm under the terrain) along the outer edges."""
        outward = {}
        for a, b, cx, cz in edges:
            pa = mesh.positions[a]
            pb = mesh.positions[b]
            ex = pb[0] - pa[0]
            ez = pb[2] - pa[2]
            length = math.sqrt(ex * ex + ez * ez)
            if length < 1e-4:
                continue
            nx, nz = ez / length, -ex / length
            mx = (pa[0] + pb[0]) * 0.5
            mz = (pa[2] + pb[2]) * 0.5
            if nx * (mx - cx) + nz * (mz - cz) < 0.0:
                nx, nz = -nx, -nz
            for v in (a, b):
                ox, oz = outward.get(v, (0.0, 0.0))
                outward[v] = (ox + nx * length, oz + nz * length)
        rings = {}
        for v, (ox, oz) in outward.items():
            length = math.sqrt(ox * ox + oz * oz)
            ox /= length
            oz /= length
            px, py, pz = mesh.positions[v]
            bx = px + ox * APRON_BEVEL_WIDTH
            bz = pz + oz * APRON_BEVEL_WIDTH
            by = py - APRON_BEVEL_DROP
            bevel_normal = normal_from_differences(-APRON_BEVEL_DROP * ox / APRON_BEVEL_WIDTH * 2.0,
                                                   -APRON_BEVEL_DROP * oz / APRON_BEVEL_WIDTH * 2.0, 1.0)
            terrain = self.ground.surface(bx, bz)
            bottom = terrain - APRON_SKIRT_BELOW
            side = (ox, 0.0, oz)
            rings[v] = (mesh.add((px, py, pz), bevel_normal), mesh.add((bx, by, bz), bevel_normal),
                        mesh.add((bx, by, bz), side), mesh.add((bx, bottom, bz), side, uv=(bx + bz, bottom)))
            self.boundary.append((px, pz, py, terrain))
            self.boundary.append((bx, bz, by, terrain))
            self.skirt_bottom.append((bx, bz, bottom, terrain))
        for a, b, cx, cz in edges:
            if a not in rings or b not in rings:
                continue
            ra, rb = rings[a], rings[b]
            mx = (mesh.positions[a][0] + mesh.positions[b][0]) * 0.5
            mz = (mesh.positions[a][2] + mesh.positions[b][2]) * 0.5
            out = (mx - cx, 0.0, mz - cz)
            bevel_up = (out[0], 4.0, out[2])
            mesh.triangle(ra[0], rb[0], rb[1], bevel_up)
            mesh.triangle(ra[0], rb[1], ra[1], bevel_up)
            mesh.triangle(ra[2], rb[2], rb[3], out)
            mesh.triangle(ra[2], rb[3], ra[3], out)

    def water_mesh(self, hole):
        mesh = MeshData()
        level = hole.water_level
        up = (0.0, 1.0, 0.0)
        center = mesh.add((hole.center[0], level, hole.center[1]), up)
        rim = []
        for k in range(32):
            a = k * 2.0 * math.pi / 32.0
            r = hole.reach * 1.1
            rim.append(mesh.add((hole.center[0] + r * math.cos(a), level, hole.center[1] + r * math.sin(a)), up))
        for k in range(32):
            mesh.triangle(center, rim[k], rim[(k + 1) % 32], up)
        return mesh


def mud_meshes(layout, ground, apron):
    """Eight patches: one under the bus's front, seven on the apron's edge at seeded places."""
    rng = random.Random(SEED + 4)
    centers = [((BUS["center"][0] - 0.3, BUS["center"][1] + BUS["half"][1] - 0.6), 3.0)]
    points = layout.outline.points
    while len(centers) < 8:
        p = points[rng.randrange(len(points))]
        if any(math.sqrt((p[0] - c[0][0]) ** 2 + (p[1] - c[0][1]) ** 2) < 10.0 for c in centers):
            continue
        if circle_distance(OIL_TANK["center"], OIL_TANK["pad"] + 2.0, p[0], p[1]) < 0.0:
            continue
        cx = round(p[0] + rng.uniform(-1.0, 1.0), 3)
        cz = round(p[1] + rng.uniform(-1.0, 1.0), 3)
        centers.append(((cx, cz), round(rng.uniform(2.0, 5.0), 3)))

    def surface(x, z):
        if layout.outline.inside(x, z):
            return apron.top(x, z), apron.top_normal(x, z)
        h = ground.surface(x, z)
        e = 0.25
        return h, normal_from_differences(ground.surface(x + e, z) - ground.surface(x - e, z),
                                          ground.surface(x, z + e) - ground.surface(x, z - e), e)

    meshes = []
    segments = 48
    for (cx, cz), radius in centers:
        phase = rng.uniform(0.0, 64.0)
        mesh = MeshData()
        rings = int(math.ceil(radius / 0.25))
        grid = []
        for ring in range(rings + 1):
            row = []
            for k in range(segments if ring else 1):
                a = k * 2.0 * math.pi / segments
                edge = radius * (1.0 + 0.22 * periodic_noise(phase + k * 8.0 / segments, 64, SEED + 5))
                r = edge * ring / rings
                x = cx + r * math.cos(a)
                z = cz + r * math.sin(a)
                h, n = surface(x, z)
                row.append(mesh.add((x + n[0] * 0.015, h + n[1] * 0.015, z + n[2] * 0.015), n))
            grid.append(row)
        up = (0.0, 1.0, 0.0)
        for k in range(segments):
            mesh.triangle(grid[0][0], grid[1][k], grid[1][(k + 1) % segments], up)
        for ring in range(1, rings):
            for k in range(segments):
                a, b = grid[ring][k], grid[ring][(k + 1) % segments]
                c, d = grid[ring + 1][k], grid[ring + 1][(k + 1) % segments]
                mesh.triangle(a, c, d, up)
                mesh.triangle(a, d, b, up)
        meshes.append(mesh)
    return meshes


# ---------------------------------------------------------------------------------------------------------------------
# glTF

class Glb:
    """A glTF 2.0 binary with quantized normals (KHR_mesh_quantization) and shared index buffers. Its nodes are the
    scene's roots, so an instance's children are the nodes themselves (the instance takes the model's name)."""

    def __init__(self, root):
        self.buffer = bytearray()
        self.views = []
        self.accessors = []
        self.meshes = []
        self.root = root
        self.nodes = []
        self.materials = []
        self.index_accessors = {}

    def material(self, name, color, roughness):
        self.materials.append({"name": name, "pbrMetallicRoughness": {"baseColorFactor": list(color) + [1.0],
                                                                       "metallicFactor": 0.0, "roughnessFactor": roughness}})
        return len(self.materials) - 1

    def _view(self, data, target, stride=None):
        while len(self.buffer) % 4:
            self.buffer.append(0)
        view = {"buffer": 0, "byteOffset": len(self.buffer), "byteLength": len(data), "target": target}
        if stride:
            view["byteStride"] = stride
        self.buffer.extend(data)
        self.views.append(view)
        return len(self.views) - 1

    def _indices(self, indices):
        key = tuple(indices)
        if key in self.index_accessors:
            return self.index_accessors[key]
        large = max(indices) > 65535
        data = struct.pack("<%d%s" % (len(indices), "I" if large else "H"), *indices)
        view = self._view(data, 34963)
        self.accessors.append({"bufferView": view, "componentType": 5125 if large else 5123, "count": len(indices),
                               "type": "SCALAR", "max": [max(indices)], "min": [min(indices)]})
        self.index_accessors[key] = len(self.accessors) - 1
        return self.index_accessors[key]

    def primitive(self, mesh, material):
        count = len(mesh.positions)
        positions = struct.pack("<%df" % (3 * count), *(c for p in mesh.positions for c in p))
        normals = b"".join(struct.pack("<4b", *(max(-127, min(127, int(round(c * 127.0)))) for c in n), 0) for n in mesh.normals)
        uvs = struct.pack("<%df" % (2 * count), *(quantize(c, 65536) for uv in mesh.uvs for c in uv))
        lo = [min(p[k] for p in mesh.positions) for k in range(3)]
        hi = [max(p[k] for p in mesh.positions) for k in range(3)]
        attributes = {}
        for name, data, component, kind, stride, normalized in (("POSITION", positions, 5126, "VEC3", 12, False),
                                                                ("NORMAL", normals, 5120, "VEC3", 4, True),
                                                                ("TEXCOORD_0", uvs, 5126, "VEC2", 8, False)):
            accessor = {"bufferView": self._view(data, 34962, stride), "componentType": component, "count": count, "type": kind}
            if normalized:
                accessor["normalized"] = True
            if name == "POSITION":
                accessor["min"] = lo
                accessor["max"] = hi
            self.accessors.append(accessor)
            attributes[name] = len(self.accessors) - 1
        return {"attributes": attributes, "indices": self._indices(mesh.indices), "material": material}

    def node(self, name, primitives):
        self.meshes.append({"name": name, "primitives": primitives})
        self.nodes.append({"name": name, "mesh": len(self.meshes) - 1})

    def bytes(self, generator):
        while len(self.buffer) % 4:
            self.buffer.append(0)
        document = {
            "asset": {"version": "2.0", "generator": generator},
            "extensionsUsed": ["KHR_mesh_quantization"],
            "extensionsRequired": ["KHR_mesh_quantization"],
            "scene": 0,
            "scenes": [{"name": self.root, "nodes": list(range(len(self.nodes)))}],
            "nodes": self.nodes,
            "meshes": self.meshes,
            "materials": self.materials,
            "buffers": [{"byteLength": len(self.buffer)}],
            "bufferViews": self.views,
            "accessors": self.accessors,
        }
        text = json.dumps(document, separators=(",", ":")).encode("utf-8")
        text += b" " * ((4 - len(text) % 4) % 4)
        glb = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(self.buffer))
        return glb + struct.pack("<II", len(text), 0x4E4F534A) + text + struct.pack("<II", len(self.buffer), 0x004E4942) + bytes(self.buffer)


# ---------------------------------------------------------------------------------------------------------------------
# Building everything

class World:
    """The layout, the sampled ground, the apron and its holes: what the models and the checks are made from."""

    def __init__(self):
        self.layout = Layout()
        self.ground = Ground(self.layout)
        self.apron = Apron(self.layout, self.ground)


def grid_indices(n):
    indices = []
    for j in range(n - 1):
        for i in range(n - 1):
            a = j * n + i
            b = a + 1
            c = a + n
            d = c + 1
            # Counter-clockwise seen from above (+Y), split along a-d as Ground._triangle assumes.
            indices += [a, c, d, a, d, b]
    return indices


def terrain_models(world):
    """The four models as {file name: bytes}."""
    ground = world.ground
    outline = world.layout.outline
    generator = "Strata Tools/GenerateShowcaseTerrain.py"
    models = {}

    glb = Glb("Island")
    ground_material = glb.material("Ground", (0.42, 0.33, 0.24), 1.0)
    ring_material = glb.material("Ring", (0.38, 0.30, 0.22), 1.0)
    n = INNER_TILE_SIZE + 1
    shared = grid_indices(n)
    covered = {}
    for tj in range(INNER_TILES):
        for ti in range(INNER_TILES):
            mesh = MeshData()
            for j in range(n):
                for i in range(n):
                    gi = ti * INNER_TILE_SIZE + i
                    gj = tj * INNER_TILE_SIZE + j
                    mesh.add((ground.inner_world(gi), ground.inner_at(gi, gj), ground.inner_world(gj)), ground.inner_normal(gi, gj))
            indices = []
            for t in range(0, len(shared), 3):
                corners = shared[t:t + 3]
                hidden = True
                for v in corners:
                    x, _, z = mesh.positions[v]
                    key = (x, z)
                    if key not in covered:
                        covered[key] = outline.signed_distance(x, z, 2.0) <= -1.0
                    hidden = hidden and covered[key]
                if not hidden:
                    indices += corners
            mesh.indices = indices if len(indices) < len(shared) else shared
            primitives = [glb.primitive(mesh, ground_material)]
            # Skirts down the outer edges of the edge tiles (one strip each, ordered so one index buffer serves all).
            for side, (on_edge, along) in enumerate(((tj == 0, lambda k: (k, 0)), (ti == INNER_TILES - 1, lambda k: (n - 1, k)),
                                                     (tj == INNER_TILES - 1, lambda k: (n - 1 - k, n - 1)), (ti == 0, lambda k: (0, n - 1 - k)))):
                if not on_edge:
                    continue
                skirt = MeshData()
                for k in range(n):
                    i, j = along(k)
                    gi = ti * INNER_TILE_SIZE + i
                    gj = tj * INNER_TILE_SIZE + j
                    x = ground.inner_world(gi)
                    z = ground.inner_world(gj)
                    h = ground.inner_at(gi, gj)
                    normal = ground.inner_normal(gi, gj)
                    skirt.add((x, h, z), normal)
                    skirt.add((x, h - SKIRT_DEPTH, z), normal, uv=(x, z))
                strip = []
                for k in range(n - 1):
                    a, b, c, d = 2 * k, 2 * k + 1, 2 * k + 2, 2 * k + 3
                    # Every strip runs the same way around the inner terrain (+X along its -Z edge), so one winding
                    # faces all of them outward.
                    strip += [a, d, b, a, c, d]
                skirt.indices = strip
                primitives.append(glb.primitive(skirt, ground_material))
            glb.node("Ground_%d_%d" % (ti, tj), primitives)
    ring_shared = grid_indices(RING_TILE_VERTICES)
    k = 0
    per_tile = RING_TILE_SIZE // int(RING_SPACING)
    for tj in range(RING_TILES):
        for ti in range(RING_TILES):
            if ti == RING_TILES // 2 and tj == RING_TILES // 2:
                continue
            mesh = MeshData()
            for j in range(RING_TILE_VERTICES):
                for i in range(RING_TILE_VERTICES):
                    gi = ti * per_tile + i
                    gj = tj * per_tile + j
                    mesh.add((ground.ring_world(gi), ground.ring[gj][gi], ground.ring_world(gj)), ground.ring_normals[gj][gi])
            mesh.indices = ring_shared
            glb.node("Ring_%d" % k, [glb.primitive(mesh, ring_material)])
            k += 1
    models["Terrain.glb"] = glb.bytes(generator)

    glb = Glb("ApronSlab")
    asphalt = glb.material("Asphalt", (0.07, 0.07, 0.07), 0.55)
    glb.node("Apron", [glb.primitive(world.apron.top_mesh, asphalt)])
    glb.node("ApronFine", [glb.primitive(world.apron.fine_mesh, asphalt)])
    models["Apron.glb"] = glb.bytes(generator)

    glb = Glb("Puddles")
    water = glb.material("Water", (0.018, 0.020, 0.022), 0.03)
    for index, hole in enumerate(world.apron.holes):
        glb.node("Puddle_%d" % index, [glb.primitive(world.apron.water_mesh(hole), water)])
    models["Water.glb"] = glb.bytes(generator)

    glb = Glb("MudPatches")
    mud = glb.material("Mud", (0.12, 0.09, 0.06), 0.8)
    for index, mesh in enumerate(mud_meshes(world.layout, ground, world.apron)):
        glb.node("Mud_%d" % index, [glb.primitive(mesh, mud)])
    models["Mud.glb"] = glb.bytes(generator)
    return models


# ---------------------------------------------------------------------------------------------------------------------
# Checks

def horizon(ground, eye, azimuth_degrees, near=1.0):
    """The highest elevation angle (degrees) of the sampled terrain seen from `eye` toward an azimuth."""
    a = math.radians(azimuth_degrees)
    dx = math.cos(a)
    dz = math.sin(a)
    best = -90.0
    t = near
    while True:
        x = eye[0] + dx * t
        z = eye[2] + dz * t
        if max(abs(x), abs(z)) >= RING_HALF:
            return best
        angle = math.degrees(math.atan2(ground.surface(x, z) - eye[1], t))
        if angle > best:
            best = angle
        t += 1.0 if max(abs(x), abs(z)) < INNER_HALF else 5.0


def shot1_camera(ground):
    return (-29.7, ground.surface(-29.7, -58.0) + 1.7, -58.0)


def run_checks(world, verbose=True):
    """Evaluates the six rules; returns a list of failures (rule 5 only reports)."""
    ground = world.ground
    layout = world.layout
    apron = world.apron
    failures = []

    def report(text):
        if verbose:
            print(text)

    # 1. The golden sun clears the terrain from the apron.
    worst = (-90.0, None)
    x0, z0, x1, z1 = layout.outline.bounds
    for zi in range(int(math.ceil(z0 / 2.0)), int(math.floor(z1 / 2.0)) + 1):
        for xi in range(int(math.ceil(x0 / 2.0)), int(math.floor(x1 / 2.0)) + 1):
            x = xi * 2.0
            z = zi * 2.0
            if not layout.outline.inside(x, z):
                continue
            eye = (x, apron.top(x, z) + 0.3, z)
            for azimuth in range(30, 61, 3):
                angle = horizon(ground, eye, azimuth)
                if angle > worst[0]:
                    worst = (angle, (x, z, azimuth))
    report("rule 1: the terrain toward azimuths 30-60 from the apron reaches %.2f degrees (at most 4.0) from (%.0f, %.0f) toward %d"
           % (worst[0], worst[1][0], worst[1][1], worst[1][2]))
    if worst[0] > 4.0:
        failures.append("rule 1")

    # 2. The dawn sun reaches shot 1's camera on the rim.
    eye = shot1_camera(ground)
    worst = max((horizon(ground, eye, azimuth, near=2.0), azimuth) for azimuth in range(135, 146))
    report("rule 2: from shot 1's camera (%.1f, %.1f, %.1f) the terrain toward azimuths 135-145 reaches %.2f degrees (at most 3.5) toward %d"
           % (eye[0], eye[1], eye[2], worst[0], worst[1]))
    if worst[0] > 3.5:
        failures.append("rule 2")

    # 3. Pads are flat under their footprints.
    flat_failures = []
    for name, distance, (cx, cz, extent) in layout.pads:
        heights = []
        slope = 0.0
        steps = int(math.ceil(extent / 0.5))
        for zi in range(-steps, steps + 1):
            z = cz + zi * 0.5
            for xi in range(-steps, steps + 1):
                x = cx + xi * 0.5
                if distance(x, z) > 0.0:
                    continue
                h = ground.surface(x, z)
                heights.append(h)
                gx = (ground.surface(x + 0.25, z) - ground.surface(x - 0.25, z)) / 0.5
                gz = (ground.surface(x, z + 0.25) - ground.surface(x, z - 0.25)) / 0.5
                slope = max(slope, math.degrees(math.atan(math.sqrt(gx * gx + gz * gz))))
        if not heights:
            flat_failures.append("%s has no footprint" % name)
            continue
        variation = max(heights) - min(heights)
        report("rule 3: %s varies by %.3f m (at most 0.03) and slopes by %.2f degrees (at most 2)" % (name, variation, slope))
        if variation > 0.03 or slope > 2.0:
            flat_failures.append(name)
    if flat_failures:
        failures.append("rule 3 (%s)" % ", ".join(flat_failures))

    # 4. The inner terrain meets the ring.
    gap = 0.0
    last = ground.inner_count - 1
    for k in range(ground.inner_count):
        for i, j in ((k, 0), (k, last), (0, k), (last, k)):
            x = ground.inner_world(i)
            z = ground.inner_world(j)
            gap = max(gap, abs(quantize(ground.inner_at(i, j), 4096) - quantize(ground.ring_surface(x, z), 4096)))
    report("rule 4: the inner terrain and the ring meet within %.4f m (at most 0.02)" % gap)
    if gap > 0.02:
        failures.append("rule 4")

    # 5. Steep ground lies in the cliff band (reported).
    steep_outside = 0
    steep = 0
    limit = math.tan(math.radians(45.0))
    for j in range(ground.inner_count - 1):
        for i in range(ground.inner_count - 1):
            x = ground.inner_world(i) + 0.5
            z = ground.inner_world(j) + 0.5
            if x * x + z * z > WALK_RADIUS * WALK_RADIUS:
                continue
            gx = ground.inner_at(i + 1, j) - ground.inner_at(i, j)
            gz = ground.inner_at(i, j + 1) - ground.inner_at(i, j)
            if gx * gx + gz * gz > limit * limit:
                steep += 1
                u, v = valley_axes(x, z)
                if not (CLIFF_BAND["u"][0] <= u <= CLIFF_BAND["u"][1] and CLIFF_BAND["v"][0] <= v <= CLIFF_BAND["v"][1]):
                    steep_outside += 1
    report("rule 5: %d square meters within %.0f m slope above 45 degrees, %d of them outside the cliff band (reported)"
           % (steep, WALK_RADIUS, steep_outside))

    # 6. The apron's edge stands above the terrain and its skirt reaches below it.
    above = min(top - terrain for _, _, top, terrain in apron.boundary)
    below = min(terrain - bottom for _, _, bottom, terrain in apron.skirt_bottom)
    report("rule 6: the apron's edge stands at least %.4f m above the terrain (at least 0.02), its skirt reaches %.4f m below it (at least 0.03)"
           % (above, below))
    if above < 0.02 or below < 0.03:
        failures.append("rule 6")
    return failures


# ---------------------------------------------------------------------------------------------------------------------
# Stations and shots

def toward_sun(elevation, azimuth):
    e = math.radians(elevation)
    a = math.radians(azimuth)
    return (math.cos(e) * math.cos(a), math.sin(e), math.cos(e) * math.sin(a))


def station(name):
    for row in STATIONS:
        if row[0] == name:
            return row
    raise KeyError(name)


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def add(a, b, scale=1.0):
    return (a[0] + b[0] * scale, a[1] + b[1] * scale, a[2] + b[2] * scale)


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def normalize(a):
    length = math.sqrt(dot(a, a))
    return (a[0] / length, a[1] / length, a[2] / length)


def reflect(d, n):
    return sub(d, (2.0 * dot(d, n) * n[0], 2.0 * dot(d, n) * n[1], 2.0 * dot(d, n) * n[2]))


def angle_between(a, b):
    return math.degrees(math.acos(max(-1.0, min(1.0, dot(normalize(a), normalize(b))))))


def project(eye, look, fov, point):
    """Where a point falls in a 16:9 picture (x right, y down, 0-1), or None behind the camera."""
    forward = normalize(sub(look, eye))
    right = normalize(cross(forward, (0.0, 1.0, 0.0)))
    up = cross(right, forward)
    d = sub(point, eye)
    depth = dot(d, forward)
    if depth <= 1e-6:
        return None
    tangent = math.tan(math.radians(fov) * 0.5)
    x = dot(d, right) / (depth * tangent * 16.0 / 9.0)
    y = dot(d, up) / (depth * tangent)
    return (0.5 + 0.5 * x, 0.5 - 0.5 * y)


def mirror_on_plane(camera, sun, point, normal):
    """The point of a plane where the camera sees the sun reflected: on the line from the camera's mirror image along
    the direction toward the sun."""
    image = sub(camera, (2.0 * dot(sub(camera, point), normal) * normal[0], 2.0 * dot(sub(camera, point), normal) * normal[1],
                         2.0 * dot(sub(camera, point), normal) * normal[2]))
    t = -dot(sub(image, point), normal) / dot(sun, normal)
    return add(image, sun, t)


def mirror_on_cylinder(camera, sun, center, radius):
    """The point of a vertical cylinder where the camera sees the sun reflected: its normal bisects the horizontal
    directions to the camera and to the sun, and the view from it to the camera falls by the sun's elevation."""
    sun_h = normalize((sun[0], 0.0, sun[2]))
    point = (center[0] + radius * sun_h[0], 0.0, center[1] + radius * sun_h[2])
    for _ in range(50):
        to_camera = normalize((camera[0] - point[0], 0.0, camera[2] - point[2]))
        n = normalize(add(to_camera, sun_h))
        point = (center[0] + radius * n[0], 0.0, center[1] + radius * n[2])
    horizontal = math.sqrt((camera[0] - point[0]) ** 2 + (camera[2] - point[2]) ** 2)
    y = camera[1] + horizontal * sun[1] / math.sqrt(sun[0] * sun[0] + sun[2] * sun[2])
    return (point[0], y, point[2]), n


def shots(world):
    """The tour's six shots (and the macro bookmark's sun): cameras, targets, subjects and the mirror points."""
    ground = world.ground
    golden = toward_sun(*station("Golden")[3:5])
    dawn = toward_sun(*station("Dawn")[3:5])
    dusk = toward_sun(*station("Dusk")[3:5])
    result = []

    # 1. Dawn vista from the rim: dollies 3 m each way along the rim (v).
    key = shot1_camera(ground)
    along = (-SQRT_HALF, 0.0, SQRT_HALF)
    look = (40.0, 0.0, 40.0)
    result.append({"shot": 1, "station": "Dawn", "key": key, "from": add(key, along, -3.0), "to": add(key, along, 3.0),
                   "look": look, "fov": 50.0,
                   "subjects": {"sun": add(key, dawn, 1000.0), "yard": (6.0, 0.0, 4.0)}})

    # 2. The side of the bus: the camera sees the sun in the letterboard at z 0.10, 8.5 m away, from 1.60 m.
    normal = (1.0, 0.0, 0.0)
    eye_height = 1.60
    distance = 8.5
    mirror_z = 0.10
    view_back = reflect((-golden[0], -golden[1], -golden[2]), normal)   # From the mirror point toward the camera
    horizontal = math.sqrt(view_back[0] ** 2 + view_back[2] ** 2)
    point_y = eye_height + distance * golden[1] / horizontal
    point = (LETTERBOARD["x"], point_y, mirror_z)
    key = add(point, view_back, distance / horizontal)
    key = (key[0], eye_height, key[2])
    result.append({"shot": 2, "station": "Golden", "key": key, "from": add(key, (0.0, 0.0, 1.0), -1.5),
                   "to": add(key, (0.0, 0.0, 1.0), 1.5), "look": point, "fov": 40.0,
                   "mirror": {"surface": "Bus.Body letterboard", "point": point, "normal": normal},
                   "subjects": {"highlight": point, "bus front": (LETTERBOARD["x"], 1.5, LETTERBOARD["z"][1])}})

    # 3. The oil tank at low sun: 16 m from the tank at its azimuth -40, 1.70 m up; dollies 6 degrees each way around it.
    center = OIL_TANK["center"]

    def around(azimuth):
        a = math.radians(azimuth)
        return (center[0] + 16.0 * math.cos(a), 1.70, center[1] + 16.0 * math.sin(a))

    key = around(-40.0)
    point, n = mirror_on_cylinder(key, golden, center, OIL_TANK["radius"])
    pad = OIL_TANK["pad_height"]
    result.append({"shot": 3, "station": "Golden", "key": key, "from": around(-46.0), "to": around(-34.0),
                   "look": (center[0], 3.60, center[1]), "fov": 45.0,
                   "mirror": {"surface": "OilTank.Shell", "point": point, "normal": n},
                   "subjects": {"streak": point, "streak bottom": (point[0], pad, point[2]),
                                "streak top": (point[0], pad + OIL_TANK["shell"], point[2])}})

    # 4. Wet apron into the sun from knee height: the glint's mirror point lies in the first shallow puddle.
    key = (POTHOLE_ORIGIN[0], 0.75, POTHOLE_ORIGIN[1])
    along = (SQRT_HALF, 0.0, SQRT_HALF)
    puddle = world.apron.holes[0]
    point = mirror_on_plane(key, golden, (puddle.center[0], puddle.water_level, puddle.center[1]), (0.0, 1.0, 0.0))
    result.append({"shot": 4, "station": "Golden", "key": key, "from": add(key, along, -1.0), "to": add(key, along, 1.0),
                   "look": (12.98, 2.0, 9.28), "fov": 55.0,
                   "mirror": {"surface": "Puddle_0", "point": point, "normal": (0.0, 1.0, 0.0), "puddle": 0},
                   "subjects": {"sun": add(key, golden, 1000.0), "glint": point,
                                "bus base": (BUS["center"][0], 0.0, BUS["center"][1]), "tank base": (center[0], 0.0, center[1])}})

    # 5. The hut from its doorway, 0.5 m out of the door; dollies 0.4 m each way along the door's axis.
    door = local_to_world(HUT["center"], HUT["yaw"], -0.8, HUT["half"][1])
    facing = (SQRT_HALF, 0.0, SQRT_HALF)
    key = (door[0] + 0.5 * facing[0], 1.50, door[1] + 0.5 * facing[2])
    look = (door[0] - 4.4 * facing[0], 0.90, door[1] - 4.4 * facing[2])
    back_wall = local_to_world(HUT["center"], HUT["yaw"], -0.8, -HUT["half"][1] + 0.2)
    result.append({"shot": 5, "station": "Golden", "key": key, "from": add(key, facing, 0.4), "to": add(key, facing, -0.4),
                   "look": look, "fov": 70.0, "subjects": {"back wall": (back_wall[0], 1.2, back_wall[1])}})

    # 6. Dusk: the fire, the lantern and the fuel tank; dollies 2 m sideways each way.
    key = (-2.0, 1.6, -2.0)
    look = (-16.0, 1.2, -16.0)
    side = (SQRT_HALF, 0.0, -SQRT_HALF)
    result.append({"shot": 6, "station": "Dusk", "key": key, "from": add(key, side, -2.0), "to": add(key, side, 2.0),
                   "look": look, "fov": 50.0,
                   "subjects": {"fire": (FIRE_PIT["center"][0], 0.3, FIRE_PIT["center"][1]), "lantern A": LANTERN_A,
                                "fuel tank": (FUEL_TANK["center"][0], FUEL_TANK["axis_height"], FUEL_TANK["center"][1]),
                                "dusk sun": add(key, dusk, 1000.0)}})
    for shot in result:
        shot["image"] = {name: project(shot["key"], shot["look"], shot["fov"], p) for name, p in shot["subjects"].items()}
    return result


def mirror_errors(world, shot):
    """How far shot's mirror point is from its surface (m) and its reflection from the sun (degrees)."""
    golden = toward_sun(*station(shot["station"])[3:5])
    mirror = shot["mirror"]
    point = mirror["point"]
    n = mirror["normal"]
    view = normalize(sub(point, shot["key"]))
    angle = angle_between(reflect(view, n), golden)
    if shot["shot"] == 2:
        off = abs(point[0] - LETTERBOARD["x"])
        inside = LETTERBOARD["y"][0] <= point[1] <= LETTERBOARD["y"][1] and LETTERBOARD["z"][0] <= point[2] <= LETTERBOARD["z"][1]
    elif shot["shot"] == 3:
        off = abs(math.sqrt((point[0] - OIL_TANK["center"][0]) ** 2 + (point[2] - OIL_TANK["center"][1]) ** 2) - OIL_TANK["radius"])
        inside = OIL_TANK["pad_height"] <= point[1] <= OIL_TANK["pad_height"] + OIL_TANK["shell"]
    else:
        puddle = world.apron.holes[mirror["puddle"]]
        off = abs(point[1] - puddle.water_level)
        dx = point[0] - puddle.center[0]
        dz = point[2] - puddle.center[1]
        d = math.sqrt(dx * dx + dz * dz)
        inside = d < puddle.radius_toward(dx / d, dz / d) * 0.8 if d > 1e-9 else True
    return off, angle, inside


def stations_report(world):
    rows = []
    for name, hour, elevation, drawn, azimuth, scale, fog, exposure, shadow, local in STATIONS:
        rows.append({"station": name, "hour": hour, "elevation": elevation, "drawn": drawn, "azimuth": azimuth,
                     "lightScale": scale, "intensity": SUN_LUX * scale, "fog": fog, "exposure": exposure,
                     "shadowDistance": shadow, "localLights": local, "towardSun": toward_sun(drawn, azimuth)})
    return {"stations": rows, "shots": shots(world)}


def format_point(p):
    return "(%.2f, %.2f, %.2f)" % p


def print_stations(report):
    print("station  hour   elev  drawn  azimuth  scale  fog      exposure  shadow  local  toward the sun")
    for row in report["stations"]:
        print("%-8s %5.2f  %4.1f  %5.1f  %7.1f  %5.2f  %.4f  %8.1f  %6.0f  %-5s  %s"
              % (row["station"], row["hour"], row["elevation"], row["drawn"], row["azimuth"], row["lightScale"], row["fog"],
                 row["exposure"], row["shadowDistance"], "on" if row["localLights"] else "off", format_point(row["towardSun"])))
    for shot in report["shots"]:
        print("shot %d (%s): key %s, from %s, to %s, look %s, fov %.0f"
              % (shot["shot"], shot["station"], format_point(shot["key"]), format_point(shot["from"]), format_point(shot["to"]),
                 format_point(shot["look"]), shot["fov"]))
        if "mirror" in shot:
            print("    mirror point on %s: %s" % (shot["mirror"]["surface"], format_point(shot["mirror"]["point"])))
        for name, image in shot["image"].items():
            print("    %s at image %s" % (name, "behind the camera" if image is None else "(%.2f, %.2f)" % image))


# ---------------------------------------------------------------------------------------------------------------------
# Commands

def write_models(models, output):
    os.makedirs(output, exist_ok=True)
    for name, data in models.items():
        with open(os.path.join(output, name), "wb") as file:
            file.write(data)
        print("%s: %d bytes" % (name, len(data)))


def command_terrain(arguments):
    world = World()
    failures = run_checks(world)
    if failures:
        print("FAIL: %s; nothing written" % ", ".join(failures))
        return 1
    write_models(terrain_models(world), arguments.output)
    return 0


def command_check(arguments):
    failures = run_checks(World())
    print("FAIL: %s" % ", ".join(failures) if failures else "PASS")
    return 1 if failures else 0


def command_stations(arguments):
    report = stations_report(World())
    if arguments.json:
        with open(arguments.json, "w", encoding="utf-8", newline="\n") as file:
            json.dump(report, file, indent=1)
            file.write("\n")
    print_stations(report)
    return 0


def command_selftest(arguments):
    failures = []
    world = World()
    failures += run_checks(world)
    first = terrain_models(world)
    second = terrain_models(World())
    write_models(first, os.path.join(arguments.work, "first"))
    for name, data in first.items():
        if second[name] != data:
            failures.append("%s differs between two runs" % name)
        committed = os.path.join(arguments.committed, name)
        if not os.path.isfile(committed):
            failures.append("%s is not committed in %s" % (name, arguments.committed))
            continue
        with open(committed, "rb") as file:
            if file.read() != data:
                failures.append("the committed %s differs from what the generator writes (run it again and import the result)" % name)
    for shot in shots(world):
        if "mirror" not in shot:
            continue
        off, angle, inside = mirror_errors(world, shot)
        print("shot %d: mirror point %.4f m off its surface, its reflection %.4f degrees off the sun, %s"
              % (shot["shot"], off, angle, "on the surface" if inside else "OFF the surface"))
        if off > 0.01 or angle > 0.1 or not inside:
            failures.append("shot %d's mirror relation" % shot["shot"])
    print("FAIL: %s" % "; ".join(failures) if failures else "PASS")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    terrain = commands.add_parser("terrain", help="Check the design, then write Terrain.glb, Apron.glb, Water.glb and Mud.glb")
    terrain.add_argument("--output", required=True, help="The directory to write them into")
    commands.add_parser("check", help="Evaluate the design's rules (exit 1 on a failure)")
    stations_parser = commands.add_parser("stations", help="Print the times of day and the shots' cameras")
    stations_parser.add_argument("--json", help="Also write them to this JSON file")
    selftest = commands.add_parser("selftest", help="Check, generate twice and compare with the committed models")
    selftest.add_argument("--committed", required=True, help="The committed models' directory")
    selftest.add_argument("--work", required=True, help="A directory to write into")
    arguments = parser.parse_args()
    try:
        return {"terrain": command_terrain, "check": command_check, "stations": command_stations,
                "selftest": command_selftest}[arguments.command](arguments)
    except CheckFailed as error:
        print("FAIL: %s" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
