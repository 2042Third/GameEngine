#!/usr/bin/env python3
"""Generates the Showcase sample's ground (High Desert Outpost), lays out its scene and checks the shots it rests on.

The outputs are committed and a CTest (StrataTools.ShowcaseTerrain) keeps them equal to what this script writes:

    python3 -I Tools/GenerateShowcaseTerrain.py terrain --output Samples/Showcase/Assets/Generated/Terrain
    python3 -I Tools/GenerateShowcaseTerrain.py check
    python3 -I Tools/GenerateShowcaseTerrain.py stations [--json <file>]
    python3 -I Tools/GenerateShowcaseTerrain.py selftest --committed Samples/Showcase/Assets/Generated/Terrain
        --stations Samples/Showcase/Scripts/Stations.h --sample Samples/Showcase
        --render-commands StrataTests/Editor/ShowcaseRenderCommands.json --work <dir>
    python3 -I Tools/GenerateShowcaseTerrain.py layout --output <Layout.json>
    python3 -I Tools/GenerateShowcaseTerrain.py controls --output <Controls.json>
    python3 -I Tools/GenerateShowcaseTerrain.py dress --output <Dress.json>
    python3 -I Tools/GenerateShowcaseTerrain.py bake --output <Bake.json>
    python3 -I Tools/GenerateShowcaseTerrain.py pictures --output <Pictures.json>
    python3 -I Tools/GenerateShowcaseTerrain.py picture --project Samples/Showcase
    python3 -I Tools/GenerateShowcaseTerrain.py setup --station Dawn|Golden|Dusk --format lookdev|script [--output <file>]
    python3 -I Tools/GenerateShowcaseTerrain.py checkshots --capture <LookDev output directory> [--live]

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
          (where the camera sees the sun reflected: on the bus's letterboard and the oil tank's shell as
          GenerateShowcaseProps.py builds them, measured from its meshes, and on the first puddle); --json writes the
          same as JSON.
selftest  check, terrain twice (byte-equal), each model byte-equal to the committed one (--committed), shots 2-4's
          mirror relations within 1 cm and 0.1 degree, and the rows of c_Stations in the scripts' Stations.h (--stations)
          equal to this file's STATIONS. Then what the pictures rest on: the pictures script takes each picture right
          after its station's steps, from its LookDev shot's camera (LookDev/Shots.json's showcase.shot<N>), at its size,
          at High, without overlays and expecting a complete, settled picture, and ends at Golden with a scene.save and a
          wait long enough for the thumbnail (scripts with one of these wrong must fail the check); the stations' steps
          that other files hold are the ones setup writes (--render-commands: every sky preset of
          ShowcaseRenderCommands.json is part of a station's script steps and all three are there; in LookDev/Shots.json
          the Setup of every showcase.shot<N> of a station other than the saved one begins with that station's LookDev
          steps, and no other showcase shot sets a sky); the sample's Picture.png (--sample) shows what its
          Screenshots/Shot2.png shows; and picture refuses what it must (in --work).
layout    writes the editor command script that builds the scene's entities in Scenes/Outpost.stscene (which holds the
          sky, the sun, the terrain and the Post Process, and with the materials and prefabs of GenerateShowcaseProps.py
          present): the ground's materials, the fog, the camera rig, the props on the surfaces under them, the seeded rocks
          and brush, the six reflection probes, the tour's markers, the overlay, the director and the Golden station; it
          is run once (StrataEditor --quit-after-commands), later edits are commands of their own.
controls  writes the command script of the director's controls: the project's input actions (Gameplay: Move, Look, Jump,
          Run, Up, Down; Showcase: Fly, Photo, HideUI, Tour, Bookmark1-7), the photo mode's canvas and the loading scene.
dress     writes the command script of the dressing, run once on the laid-out scene: drums and tyres along the bus's sill
          on shot 4's side (they break the straight edge its puddles reflect), stones on the apron's edge and rocks on
          the rim framing shot 1.
bake      writes the command script that bakes the six probes: per station (Dawn, Dusk, Golden last, the saved one) its
          edit-mode steps with only its two probes on, then lighting.bakeProbes of them eight times: a bake draws the
          probes' present captures, so each pass adds a bounce, and after eight the captures no longer depend on those
          the project held (one run reproduces the committed ones).
pictures  writes the command script that retakes the sample's pictures after a change to its look: Screenshots/Shot1.png
          to Shot6.png (960 x 540 at High, each shot's key camera at its station, no overlays), Assets/Icon.png (256 x 256
          of shot 2) and, by a scene.save at Golden (the scene's camera stands at shot 2's key), the project's thumbnail
          (.strata/Thumbnail.png). Every capture must report a complete picture (nothing loading, textures at the levels
          the view wants, the local lights' shadows and the probes drawn, the exposure and temporal anti-aliasing's
          history settled) or the script stops. Run it on the sample in place after bake, in a session of its own.
picture   copies the project's thumbnail (--project's .strata/Thumbnail.png) to its Picture.png, the last step of a
          retake. It refuses (exit 1, nothing copied) a thumbnail that is missing, not 480 x 270, older than a screenshot
          or the icon (the pictures script did not write it), or that does not show what Screenshots/Shot2.png shows
          (the scene was saved at another station or from another camera).
setup     prints a station's edit-mode steps: the GoldenHour preset turned to its sun, the sun's intensity and shadow
          distance, the fog, the exposure, the local lights (their intensity and source radius), the embers and wicks, and
          its probes on (LookDev Setup entries naming entities, or command steps that find them).
checkshots reads a LookDev capture: the brightest 0.1% of shot 2's letterboard within 40 px of the computed highlight, the
          brightest column of shot 3's tank within 30 px of the computed streak, and the shell beside the streak with
          and without the yard's probe (at least 8 codes apart; at 1920 x 1080 with TAA); with --live the highlights' shimmer in the live shots (the
          second differences of 32 px block means over four frames: mean at most 1%, 99th percentile at most 3%).
Exit codes: 0 done, 1 a check failed, 2 usage.
"""

import argparse
import copy
import json
import math
import os
import random
import re
import shutil
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
# The puddles' water: the base color and roughness of the sample's Water material, which the props generator makes from
# these two, and of Water.glb's stand-in for it (the material's IOR, 1.33, is its own: the model's factors hold none).
WATER_COLOR = (0.035, 0.037, 0.040)
WATER_ROUGHNESS = 0.03

# The layout's anchors (the showcase spec's D3; the props generator and the layout use the same numbers).
BUS = {"center": (11.8, 0.1), "half": (1.22, 5.4), "yaw": 0}
BUS_CORNER_REACH = 0.18  # The bus's front (+Z) beyond the letterboard's end: its corners' radius
OIL_TANK = {"center": (2.7, 17.2), "pad": 4.35}  # The props generator builds the tank (radius 4.0) on its pad
FUEL_TANK = {"center": (-10.0, -20.0), "half": (1.4, 3.9), "yaw": -45, "axis_height": 1.75}
HUT = {"center": (-22.0, -18.0), "half": (2.6, 2.2), "yaw": 45}
LEAN_TO = {"local": (3.9, 0.0), "half": (1.5, 2.6)}
FIRE_PIT = {"center": (-15.0, -14.0), "radius": 0.75}
DRUM_CLUSTERS = (((-2.0, 20.0), 1.5), ((-17.5, -21.5), 1.2), ((9.5, 7.5), 1.2))
LANTERN_A = (-20.76, 2.0, -15.63)
POTHOLE_ORIGIN = (-10.0, -10.0)
CLIFF_BAND = {"u": (-60.0, -36.0), "v": (-80.0, 60.0)}
WALK_RADIUS = 140.0


# The three times of day (the showcase spec's D4): name, photo hour, sun elevation, elevation the light is drawn at,
# azimuth, light scale, fog density, exposure compensation (EV), shadow distance (m), local lights on.
# Dusk's azimuth is where the last sun passes beside the bus (the checks' rule 7): at 32 the bus's shadow lay over the
# hut, the fire and the fuel tank's near end, which then had only the sky's light (the probes hold no sun).
STATIONS = (
    ("Dawn", 6.5, 5.0, 5.0, 140.0, 1.0, 0.006, 0.0, 600.0, False),
    ("Golden", 17.75, 6.0, 6.0, 45.0, 1.0, 0.0006, -0.7, 200.0, False),
    ("Dusk", 19.0, 1.0, 2.0, 16.0, 0.002, 0.0015, -2.5, 150.0, True),
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

    def material(self, name, color, roughness, metallic=0.0):
        self.materials.append({"name": name, "pbrMetallicRoughness": {"baseColorFactor": list(color) + [1.0],
                                                                       "metallicFactor": metallic, "roughnessFactor": roughness}})
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

def letterboard_sections(props, z_range):
    """The letterboard's painted and bare sections along the bus (world z), split where its bare steel begins and ends."""
    edges = sorted({z_range[0], z_range[1]} | {z for section in props.BUS_BARE_LETTERBOARD for z in section})
    return [(BUS["center"][1] + a, BUS["center"][1] + b) for a, b in zip(edges, edges[1:])]


class World:
    """The layout, the sampled ground, the apron and its holes: what the models and the checks are made from."""

    def __init__(self):
        self.layout = Layout()
        self.ground = Ground(self.layout)
        self.apron = Apron(self.layout, self.ground)
        self._surfaces = None

    def surfaces(self):
        """Where the highlights of shots 2 and 3 lie, measured from the props generator's meshes (Tools/
        GenerateShowcaseProps.py) and placed as the layout places them: the bus's letterboard and the oil tank's shell,
        each standing on the apron's top."""
        if self._surfaces is None:
            tools = os.path.dirname(os.path.abspath(__file__))
            if tools not in sys.path:
                sys.path.insert(0, tools)
            import GenerateShowcaseProps as props  # Imported here: it imports this module
            bus = props.BUILDERS["Bus"]()
            board = props.letterboard({name: bus[name].mesh() for name in ("Bus.Body", "Bus.BarePanels")})
            shell = props.tank_shell({"OilTank.Shell": props.BUILDERS["OilTank"]()["OilTank.Shell"].mesh()})
            bus_base = apron_plane(BUS["center"][0])
            tank_base = apron_plane(OIL_TANK["center"][0])
            self._surfaces = {
                "letterboard": {"x": BUS["center"][0] + board["x"], "y": (bus_base + board["y"][0], bus_base + board["y"][1]),
                                "z": (BUS["center"][1] + board["z"][0], BUS["center"][1] + board["z"][1]), "tilt": board["tilt"],
                                "sections": letterboard_sections(props, (board["z"][0], board["z"][1]))},
                "shell": {"center": OIL_TANK["center"], "radius": shell["radius"], "y": (tank_base + shell["y"][0], tank_base + shell["y"][1]),
                          "deviation": shell["deviation"]},
            }
        return self._surfaces


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
    water = glb.material("Water", WATER_COLOR, WATER_ROUGHNESS)
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
    """Evaluates the seven rules; returns a list of failures (rule 5 only reports)."""
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

    # 7. The dusk sun reaches shot 6's subjects. A reflection probe holds no sun (a bake draws neither its disk nor the
    # highlights it makes), so what the sun does not reach at dusk has the sky's light and the fire's alone: with the bus's
    # shadow over them, the hut's front, the fire's ground and the fuel tank's near end came out near black. Seen from each
    # subject, the sun stands beside the bus's footprint and above the terrain, where the light is drawn.
    dusk = station("Dusk")
    beside, over = dusk_sun_reach(world, dusk[4])
    report("rule 7: the dusk sun (azimuth %g, drawn at %g degrees) stands %.2f degrees beside the bus from %s (at least %g) and %.2f degrees "
           "above the terrain from %s (at least %g)" % (dusk[4], dusk[3], beside[0], beside[1], DUSK_SUN_BESIDE, over[0], over[1], DUSK_SUN_OVER))
    if beside[0] < DUSK_SUN_BESIDE or over[0] < DUSK_SUN_OVER:
        failures.append("rule 7")
    return failures


# Rule 7's margins (degrees): how far beside the bus's footprint and how far above the terrain the dusk sun stands from
# each of shot 6's subjects. Beside the bus they lie between two pictures: at azimuth 17 (0.25 from the fire, none from
# lantern A) the shadow's soft edge crossed the fire pit, at 16 (1.25 and 1.0) it lies clear of both. Above the terrain
# the limit holds the light's one direction clear of a horizon sampled every metre; it is less than the disk's radius,
# which the shadow map does not draw.
DUSK_SUN_BESIDE = 0.75
DUSK_SUN_OVER = 0.15
# Where the dusk sun stood before rule 7: the bus covers it from the fire, lantern A and the hut's door wall.
DUSK_AZIMUTH_BEHIND_BUS = 32.0


def dusk_sun_reach(world, azimuth):
    """How the dusk sun at an azimuth reaches shot 6's subjects and the hut's door wall: (degrees beside the bus's
    footprint, subject) and (degrees above the terrain, subject) of the subject with the least of each. Beside the bus
    is 0 where its footprint covers the sun's azimuth (at the drawn elevation its body does then)."""
    drawn = station("Dusk")[3]
    subjects = {name: point for name, point in [shot for shot in shots(world) if shot["shot"] == 6][0]["subjects"].items() if name != "dusk sun"}
    subjects["the hut's door wall"] = hut_point((0.0, 1.2, HUT["half"][1]))
    corners = [local_to_world(BUS["center"], BUS["yaw"], sx * BUS["half"][0], sz * BUS["half"][1]) for sx in (-1, 1) for sz in (-1, 1)]
    beside = (360.0, None)
    over = (90.0, None)
    for name, point in subjects.items():
        # The corners' azimuths from the subject, from the sun's: the footprint covers the sun when they lie on both sides
        # of it (seen from outside, a convex outline spans less than half a turn).
        offsets = [(math.degrees(math.atan2(z - point[2], x - point[0])) - azimuth + 180.0) % 360.0 - 180.0 for x, z in corners]
        covered = min(offsets) < 0.0 < max(offsets) and max(offsets) - min(offsets) < 180.0
        beside = min(beside, (0.0 if covered else min(abs(offset) for offset in offsets), name))
        over = min(over, (drawn - horizon(world.ground, point, azimuth), name))
    return beside, over


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
    """The tour's six shots: cameras, targets, subjects and the mirror points."""
    ground = world.ground
    board = world.surfaces()["letterboard"]
    shell = world.surfaces()["shell"]
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
    point = (board["x"], point_y, mirror_z)
    key = add(point, view_back, distance / horizontal)
    key = (key[0], eye_height, key[2])
    result.append({"shot": 2, "station": "Golden", "key": key, "from": add(key, (0.0, 0.0, 1.0), -1.5),
                   "to": add(key, (0.0, 0.0, 1.0), 1.5), "look": point, "fov": 40.0,
                   "mirror": {"surface": "Bus.Body letterboard", "point": point, "normal": normal},
                   "subjects": {"highlight": point, "bus front": (board["x"], 1.5, board["z"][1] + BUS_CORNER_REACH)}})

    # 3. The oil tank at low sun: 16 m from the tank at its azimuth -40, 1.70 m up; dollies 6 degrees each way around it.
    center = OIL_TANK["center"]

    def around(azimuth):
        a = math.radians(azimuth)
        return (center[0] + 16.0 * math.cos(a), 1.70, center[1] + 16.0 * math.sin(a))

    key = around(-40.0)
    point, n = mirror_on_cylinder(key, golden, center, shell["radius"])
    result.append({"shot": 3, "station": "Golden", "key": key, "from": around(-46.0), "to": around(-34.0),
                   "look": (center[0], 3.60, center[1]), "fov": 45.0,
                   "mirror": {"surface": "OilTank.Shell", "point": point, "normal": n},
                   "subjects": {"streak": point, "streak bottom": (point[0], shell["y"][0], point[2]),
                                "streak top": (point[0], shell["y"][1], point[2])}})

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
    surfaces = world.surfaces()
    if shot["shot"] == 2:
        board = surfaces["letterboard"]
        off = abs(point[0] - board["x"])
        # The letterboard's faces are vertical (their tilt adds to the reflection's error).
        angle += board["tilt"] * 2.0
        inside = board["y"][0] <= point[1] <= board["y"][1] and board["z"][0] <= point[2] <= board["z"][1]
    elif shot["shot"] == 3:
        shell = surfaces["shell"]
        off = abs(math.sqrt((point[0] - shell["center"][0]) ** 2 + (point[2] - shell["center"][1]) ** 2) - shell["radius"])
        inside = shell["y"][0] <= point[1] <= shell["y"][1] and shell["deviation"] < 1.0
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
# The scene: its layout, the stations' edit-mode steps and the checks of the captured shots

SCENE = "Scenes/Outpost.stscene"
PREFABS = "Prefabs"

# The scans' extents in their own frames (meters, the min and max of their vertices, read from their glTF accessors):
# where drums and tyres stand and how big the rocks' box colliders are (80% of the extents).
SCAN_BOUNDS = {
    "namaqualand_boulder_02": ((-1.184, -0.059, -0.609), (1.348, 0.823, 0.626)),
    "namaqualand_boulder_03": ((-1.055, -0.015, -1.390), (1.352, 1.458, 1.676)),
    "namaqualand_cliff_01": ((-3.872, -0.419, -3.611), (4.407, 4.542, 0.780)),
    "barrel_03": ((-0.317, 0.0, -0.319), (0.317, 0.930, 0.319)),
    "Barrel_01": ((-0.282, -0.007, -0.282), (0.282, 0.873, 0.282)),
    "Barrel_02": ((-0.244, 0.005, -0.242), (0.244, 0.885, 0.242)),
    "old_tyre": ((-0.300, -0.300, -0.083), (0.300, 0.300, 0.082)),
    "covered_car": ((-0.847, 0.0, -2.166), (0.942, 1.411, 2.214)),
}
DRUMS = ("barrel_03", "Barrel_01", "Barrel_02")
LANTERN_B_DRUM = (9.6, 7.4)
TYRE_STACK = (9.5, -7.0)
TYRES_SCATTERED = ((-6.5, 19.5), (21.5, -9.0), (-17.0, -9.5))
CLIFFS = 8
BOULDERS = {"namaqualand_boulder_02": 25, "namaqualand_boulder_03": 20}
BRUSH = (40, 20)  # Near the apron's edge, at the slopes' feet
DRESS_STONE_CLUSTERS = 14  # The dressing's stones on the apron's edge
# The dressing's rocks on the rim: where shot 1's key sees them (image x, y), the scan and its scale. The ground there
# lies 20-40 m below and ahead of the camera, so they are boulders of 3-6 m, clear of the hero boulder at (0.2, 0.84).
DRESS_RIM_ROCKS = ((0.04, 0.90, "namaqualand_boulder_02", 2.4), (0.11, 0.98, "namaqualand_boulder_03", 1.6),
                   (0.31, 0.93, "namaqualand_boulder_02", 1.1), (0.02, 0.78, "namaqualand_boulder_03", 1.3))

def hut_point(local):
    """A point of the hut's frame (x right, y up from its pad at height 0, z toward the door) in the world."""
    x, z = local_to_world(HUT["center"], HUT["yaw"], local[0], local[2])
    return (x, local[1], z)


# The probes (the showcase spec's D10): (part, position, yaw, half extents, blend distance, priority, box projection,
# bake), where bake is None (baked where it stands) or the position and half extents it is baked with.
# The hut's box is its interior: the walls' outer faces (their inner faces lie a blend distance inside, at full weight),
# from under the floor to just under the roof's top where it is lowest (2.48 m at the back wall), so nothing outside (the
# lean-to, the ground around the hut, the roof's top) takes the dark interior's light instead of the yard's. Its capture is taken in the back corner
# away from the door's and the window's sun beams (hut-local (1.9, 0.7, -1.3)): taken in the middle, it faces the low
# sun's patch on the back wall, and the dark corner D15 measures came out only 0.3 EV below the yard. So the bake puts the
# probe there, with a box about it that still holds the interior (every pass after the first sees the interior lit by
# the capture of the pass before: BAKE_PASSES), then back. A probe whose capture was taken off its box's center projects no box: the capture is looked up as a distant
# environment, which the rough plaster and planks inside cannot tell apart.
HUT_PROBE_BOX = ((0.0, 1.15, 0.0), (2.6, 1.3, 2.2))
HUT_PROBE_CAPTURE = (1.9, 0.7, -1.3)
HUT_PROBE_BAKE_EXTENTS = tuple(HUT_PROBE_BOX[1][k] + abs(HUT_PROBE_CAPTURE[k] - HUT_PROBE_BOX[0][k]) for k in range(3))
PROBES = (
    ("Yard", (2.0, 1.6, 4.0), 0, (34.0, 14.0, 34.0), 8.0, 0, True, None),
    ("Hut", hut_point(HUT_PROBE_BOX[0]), HUT["yaw"], HUT_PROBE_BOX[1], 0.2, 2, False,
     (hut_point(HUT_PROBE_CAPTURE), HUT_PROBE_BAKE_EXTENTS)),
)
# The local lights: their candela at dusk and their source radius (m: the flames' size, which widens only their highlights;
# the Manual's fire pit 0.2-0.3, lantern 0.03-0.06), and the glowing embers and wicks, which only burn at dusk.
LOCAL_LIGHTS = (("Fire", 400.0, 0.25), ("LanternA.Light", 40.0, 0.04), ("LanternB.Light", 25.0, 0.04))
GLOWS = ("FirePit.Embers", "LanternA.Wick", "LanternB.Wick")
FOG = {"BaseHeight": -8.0, "HeightFalloff": 0.10, "StartDistance": 80.0, "MaxOpacity": 0.7, "ColorFromSky": True, "SunScattering": 0.6}
EYE_HEIGHT = 1.6
FIRE_LIGHT_HEIGHT = 0.9

# The graybox's measurement views of shot 5's check (D15): the sunlit apron, and the hut's dark corner away from the sun
# patches of the door and the window (hut-local x right, z toward the door).
MEASURE_YARD = ((-9.5, 1.5, -9.5), (-12.0, None, -12.0), 25.0)
MEASURE_HUT_LOCAL = ((-1.9, 1.5, 1.5), (-2.25, 0.6, -1.7), 15.0)

# Highlight crops of the live shots (the showcase spec's D14) and where checkshots looks for each subject.
LIVE_CROPS = {2: ("highlight", (0.42, 0.40, 0.58, 0.60)), 3: ("streak", (0.30, 0.15, 0.42, 0.80)),
              4: ("glint", (0.48, 0.55, 0.62, 0.75)), 6: ("lantern", (0.34, 0.38, 0.46, 0.54))}
STREAK_CROP = (0.30, 0.15, 0.42, 0.80)
BESIDE_STREAK_CROP = (0.50, 0.15, 0.66, 0.75)
# Shot 6's fuel tank: the end that faces the fire (Shots.json's tank crop), and how much brighter the fire makes it.
TANK_END_CROP = (0.60, 0.38, 0.65, 0.54)
FIRELIGHT_CODES = 2.0
LIVE_MODES = ("", ".fxaa", ".off")
SHIMMER_LIMITS = (1.0, 3.0)  # Mean and 99th percentile of the second differences (percent of full scale)


def surface_at(world, x, z):
    """The height a prop stands on at (x, z): the apron's top inside its outline, else the terrain."""
    if world.layout.outline.inside(x, z):
        return world.apron.top(x, z)
    return world.ground.surface(x, z)


def rounded(values, digits=4):
    return [round(v, digits) for v in values]


def bind_name(text):
    """A command script's binding name for an entity's name (letters, digits, '_', '-' and '.')."""
    return re.sub(r"[^A-Za-z0-9_.-]", "_", text)


def look_angles(eye, target):
    """The yaw (about +Y, 0 looking along -Z) and pitch (positive up) in degrees of a camera at eye looking at target."""
    d = sub(target, eye)
    return math.degrees(math.atan2(-d[0], -d[2])), math.degrees(math.atan2(d[1], math.sqrt(d[0] * d[0] + d[2] * d[2])))


def ground_at_image(world, eye, look, fov, ix, iy):
    """The terrain's point seen at image position (ix, iy) of a 16:9 picture (0-1, y down)."""
    forward = normalize(sub(look, eye))
    right = normalize(cross(forward, (0.0, 1.0, 0.0)))
    up = cross(right, forward)
    tangent = math.tan(math.radians(fov) * 0.5)
    ray = normalize(add(add(forward, right, (2.0 * ix - 1.0) * tangent * 16.0 / 9.0), up, (1.0 - 2.0 * iy) * tangent))
    t = 0.5
    while t < 400.0:
        p = add(eye, ray, t)
        if p[1] <= world.ground.surface(p[0], p[2]):
            return (p[0], world.ground.surface(p[0], p[2]), p[2])
        t += 0.1
    raise CheckFailed("the ray at image (%.2f, %.2f) meets no ground" % (ix, iy))


class SceneScript:
    """An editor command script: steps that find entities by name and refer to them by bound names."""

    def __init__(self):
        self.steps = []

    def add(self, command, parameters=None, bind=None, expect=None):
        step = {"command": command}
        if parameters is not None:
            step["parameters"] = parameters
        if bind is not None:
            step["bind"] = bind
        if expect is not None:
            step["expect"] = expect
        self.steps.append(step)

    def find(self, name, bind=None):
        self.add("entity.find", {"name": name}, {bind or name: "/entities/0"})
        return {"$ref": bind or name}

    def create(self, name, parent=None, components=None):
        parameters = {"name": name}
        if parent is not None:
            parameters["parent"] = parent
        if components:
            parameters["components"] = components
        self.add("entity.create", parameters, {name: "/id"})
        return {"$ref": name}

    def instantiate(self, prefab, bind, parent, components):
        self.add("prefab.instantiate", {"prefab": prefab, "parent": parent, "components": components}, {bind: "/id"})
        return {"$ref": bind}

    def rename(self, entity, name):
        self.add("entity.rename", {"entity": entity, "name": name})


def transform(position, yaw=0.0, scale=None, pitch=0.0):
    values = {"Translation": rounded(position), "Rotation": [round(pitch, 3), round(yaw, 3), 0.0]}
    if scale is not None:
        values["Scale"] = rounded(scale if isinstance(scale, (list, tuple)) else (scale, scale, scale))
    return values


def box_collider(bounds, share=0.8, scale=1.0):
    low, high = bounds
    return {"HalfExtents": rounded([(high[k] - low[k]) * 0.5 * share for k in range(3)]),
            "Offset": rounded([(high[k] + low[k]) * 0.5 for k in range(3)])}


def scan_path(name):
    return "Scans/%s_1k/%s_1k.gltf" % (name, name)


def station_steps(name, script_form):
    """A station's edit-mode steps (D4): the sky preset turned to its sun, the sun's intensity and shadow distance, the
    fog, the exposure, the local lights, the embers and wicks, and its two probes on (the other four off). As command
    steps that find their entities (script_form) or as LookDev Setup entries naming them (entityName)."""
    _, _, elevation, drawn, azimuth, scale, fog, exposure, shadow, local = station(name)
    steps = []

    def step(command, entity, parameters):
        parameters = dict(parameters)
        if script_form:
            if entity is not None:
                parameters["entity"] = {"$ref": "station_" + bind_name(entity)}
            steps.append({"command": command, "parameters": parameters})
        else:
            if entity is not None:
                parameters = dict({"entityName": entity}, **parameters)
            steps.append({"Command": command, "Parameters": parameters})

    names = ["Sun", "Fog", "Post Process"] + [light for light, *_ in LOCAL_LIGHTS] + list(GLOWS)
    names += ["Probe.%s.%s" % (s[0], part[0]) for s in STATIONS for part in PROBES]
    if script_form:
        for entity in names:
            steps.append({"command": "entity.find", "parameters": {"name": entity}, "bind": {"station_" + bind_name(entity): "/entities/0"}})
    preset = {"preset": "GoldenHour", "sunElevation": drawn, "sunAzimuth": azimuth}
    if script_form:
        preset["light"] = {"$ref": "station_Sun"}
    step("lighting.applySkyPreset", None, preset)
    step("component.set", "Sun", {"component": "DirectionalLight", "values": {"Intensity": SUN_LUX * scale, "ShadowDistance": shadow}})
    step("component.set", "Fog", {"component": "HeightFog", "values": {"Density": fog}})
    step("component.set", "Post Process", {"component": "PostProcess", "values": {"Exposure": exposure}})
    for light, candela, radius in LOCAL_LIGHTS:
        step("component.set", light, {"component": "PointLight", "values": {"Intensity": candela if local else 0.0, "SourceRadius": radius}})
    for glow in GLOWS:
        step("entity.setActive", glow, {"active": local})
    for other in STATIONS:
        for part in PROBES:
            step("entity.setActive", "Probe.%s.%s" % (other[0], part[0]), {"active": other[0] == name})
    return steps


def ground_material_steps(world):
    """Steps that put the sample's materials on the ground's nodes: the inner terrain, the ring, the apron, the puddles and
    the mud patches (the models carry factor-only stand-ins)."""
    groups = [("Ground", ["Ground_%d_%d" % (i, j) for j in range(INNER_TILES) for i in range(INNER_TILES)]),
              ("RingGround", ["Ring_%d" % k for k in range(RING_TILES * RING_TILES - 1)]),
              ("Asphalt", ["Apron", "ApronFine"]),
              ("Water", ["Puddle_%d" % k for k in range(len(world.apron.holes))]),
              ("Mud", ["Mud_%d" % k for k in range(len(mud_meshes(world.layout, world.ground, world.apron)))])]
    script = SceneScript()
    for material, names in groups:
        entities = [script.find(name, "node_" + bind_name(name)) for name in names]
        script.add("component.set", {"entities": entities, "component": "MeshRenderer", "values": {"Material": "Materials/%s.stmat" % material}})
    return script.steps


def layout_script(world):
    """The editor command script that builds the scene's entities (D9) in Scenes/Outpost.stscene, which holds the sky,
    the sun, the terrain and the Post Process: the fog, the camera rig, the props at the heights of the surfaces they
    stand on, the seeded rocks and brush, the six probes, the tour's markers, the overlay and the director."""
    script = SceneScript()
    rng = random.Random(SEED + 300)
    layout = world.layout
    report = shots(world)
    s = script
    s.add("scene.open", {"scene": SCENE})
    s.add("script.build", expect={"/success": {"equals": True}})
    sky = s.find("Sky")
    s.add("component.set", {"entity": sky, "component": "SkyLight", "values": {"AerialPerspectiveScale": 4.0}})
    s.steps += ground_material_steps(world)
    s.create("Fog", components={"HeightFog": dict(FOG, Density=station("Golden")[6])})

    # The camera rig at shot 2's key: the body turns with the yaw, the eye with the pitch.
    shot2 = report[1]
    yaw, pitch = look_angles(shot2["key"], shot2["look"])
    player = s.create("Player", components={
        "Transform": transform((shot2["key"][0], shot2["key"][1] - EYE_HEIGHT, shot2["key"][2]), yaw),
        "CharacterController": {"Height": 1.8, "Radius": 0.3, "Layer": 3, "Enabled": False}})
    eye = s.create("Eye", player, {"Transform": transform((0.0, EYE_HEIGHT, 0.0), pitch=pitch),
                                   "Camera": {"Primary": True, "PerspectiveFOV": shot2["fov"], "PerspectiveNear": 0.1,
                                              "PerspectiveFar": 3000.0, "DepthOfField": False, "Aperture": 2.8}})

    # The props: static, on the Props layer, each standing on the surface under its footprint's center.
    props = s.create("Props", components={"RigidBody": {"Type": "Static", "Layer": 2}})

    def place(prefab, bind, x, z, yaw_degrees=0.0, y=None, extra=None):
        components = {"Transform": transform((x, surface_at(world, x, z) if y is None else y, z), yaw_degrees)}
        if extra:
            components.update(extra)
        return s.instantiate(prefab, bind, props, components)

    place("%s/Bus.stprefab" % PREFABS, "Bus", BUS["center"][0], BUS["center"][1], BUS["yaw"])
    place("%s/OilTank.stprefab" % PREFABS, "OilTank", OIL_TANK["center"][0], OIL_TANK["center"][1])
    place("%s/FuelTank.stprefab" % PREFABS, "FuelTank", FUEL_TANK["center"][0], FUEL_TANK["center"][1], FUEL_TANK["yaw"])
    place("%s/Hut.stprefab" % PREFABS, "Hut", HUT["center"][0], HUT["center"][1], HUT["yaw"])
    car = local_to_world(HUT["center"], HUT["yaw"], *LEAN_TO["local"])
    place(scan_path("covered_car"), "CoveredCar", car[0], car[1], HUT["yaw"], extra={"BoxCollider": box_collider(SCAN_BOUNDS["covered_car"], 0.9)})
    s.rename({"$ref": "CoveredCar"}, "CoveredCar")
    # The fire pit and the two lanterns: renamed so that each light, ember and wick has a name of its own (D9).
    place("%s/FirePit.stprefab" % PREFABS, "FirePit", FIRE_PIT["center"][0], FIRE_PIT["center"][1])
    s.rename(s.find("Embers", "FirePitEmbers"), "FirePit.Embers")
    # The fire's light where its flames are, above the stones: at the prefab's 0.35 m the ring of stones shadows the ground.
    s.add("component.set", {"entity": s.find("Fire", "FireLight"), "component": "Transform", "values": {"Translation": [0.0, FIRE_LIGHT_HEIGHT, 0.0]}})
    lantern_b_y = surface_at(world, *LANTERN_B_DRUM) + SCAN_BOUNDS["Barrel_01"][1][1]
    for lantern, (x, y, z) in (("LanternA", LANTERN_A), ("LanternB", (LANTERN_B_DRUM[0], lantern_b_y, LANTERN_B_DRUM[1]))):
        place("%s/Lantern.stprefab" % PREFABS, lantern, x, z, y=y)
        s.rename({"$ref": lantern}, lantern)
        s.rename(s.find("Light", lantern + "Light"), lantern + ".Light")
        s.rename(s.find("Wick", lantern + "Wick"), lantern + ".Wick")
        # The scan's glass is see-through: it must not shut the flame's light in.
        glass = s.find("Lantern_01_glass", lantern + "Glass")
        s.rename(glass, lantern + ".Glass")
        s.add("component.set", {"entity": glass, "component": "MeshRenderer", "values": {"CastShadows": False}})
    s.add("component.set", {"entity": {"$ref": "LanternBLight"}, "component": "PointLight", "values": {"CastShadows": False}})

    # Drums in three clusters (seeded, at least 0.7 m apart), Lantern B's drum first at its place.
    clusters = ((DRUM_CLUSTERS[0], 5), (DRUM_CLUSTERS[1], 3), (DRUM_CLUSTERS[2], 3))
    drums = []
    for (center, radius), count in clusters:
        placed = [LANTERN_B_DRUM] if center == DRUM_CLUSTERS[2][0] else []
        if placed:
            drums.append(("Barrel_01", LANTERN_B_DRUM[0], LANTERN_B_DRUM[1], 0.0))
        while len(placed) < count:
            angle = rng.uniform(0.0, 2.0 * math.pi)
            r = radius * math.sqrt(rng.uniform(0.0, 1.0))
            x, z = center[0] + r * math.cos(angle), center[1] + r * math.sin(angle)
            if all((x - px) ** 2 + (z - pz) ** 2 >= 0.7 ** 2 for px, pz in placed):
                placed.append((x, z))
                drums.append((DRUMS[len(drums) % len(DRUMS)], x, z, rng.uniform(0.0, 360.0)))
    for index, (model, x, z, yaw_degrees) in enumerate(drums):
        low, high = SCAN_BOUNDS[model]
        radius = 0.5 * (high[0] - low[0])
        place(scan_path(model), "Drum%d" % index, x, z, yaw_degrees,
              extra={"BoxCollider": {"HalfExtents": rounded([radius * 0.9, high[1] * 0.5, radius * 0.9]), "Offset": [0.0, round(high[1] * 0.5, 4), 0.0]}})
        s.rename({"$ref": "Drum%d" % index}, "Drum%d" % index)
    # Tyres lying flat: a stack of four and three alone (their axis, local Z, turned up).
    low, high = SCAN_BOUNDS["old_tyre"]
    half_width = 0.5 * (high[2] - low[2])
    tyres = [(TYRE_STACK[0], TYRE_STACK[1], k) for k in range(4)] + [(x, z, 0) for x, z in TYRES_SCATTERED]
    for index, (x, z, level) in enumerate(tyres):
        y = surface_at(world, x, z) + half_width * (2 * level + 1)
        s.instantiate(scan_path("old_tyre"), "Tyre%d" % index, props, {"Transform": transform((x, y, z), pitch=90.0)})
        s.rename({"$ref": "Tyre%d" % index}, "Tyre%d" % index)

    # Rocks: the cliff band in the rim's face (left open in front of shot 1's camera), talus at its foot, boulders at the
    # feet of the valley's walls, and shot 1's hero boulder.
    rocks = s.create("Rocks", components={"RigidBody": {"Type": "Static", "Layer": 2}})
    shot1 = report[0]
    camera_v = valley_axes(shot1["key"][0], shot1["key"][2])[1]
    rock_instances = {}

    def add_rock(model, x, z, yaw_degrees, scale, sink):
        low, high = SCAN_BOUNDS[model]
        y = world.ground.surface(x, z) - sink * (high[1] - low[1]) * scale
        rock_instances.setdefault(model, []).append({"parent": rocks, "components": {
            "Transform": transform((x, y, z), yaw_degrees, scale), "BoxCollider": box_collider(SCAN_BOUNDS[model])}})

    vs = [v for v in [-80.0 + 140.0 * (k + 0.5) / 12.0 for k in range(12)] if abs(v - camera_v) > 22.0][:CLIFFS]
    for v in vs:
        u = -43.0 + rng.uniform(-2.0, 2.0)
        scale = rng.uniform(1.8, 3.0)
        x, z = (u - v) * SQRT_HALF, (u + v) * SQRT_HALF
        # Its face (local +Z) looks down the valley; the front's foot is buried in the slope.
        front = (x + 0.78 * scale * SQRT_HALF, z + 0.78 * scale * SQRT_HALF)
        y = world.ground.surface(*front) - 0.2 * scale
        rock_instances.setdefault("namaqualand_cliff_01", []).append({"parent": rocks, "components": {
            "Transform": transform((x, y, z), 45.0 + rng.uniform(-20.0, 20.0), scale), "BoxCollider": box_collider(SCAN_BOUNDS["namaqualand_cliff_01"])}})
    hero = ground_at_image(world, shot1["key"], shot1["look"], shot1["fov"], 0.2, 0.86)
    remaining = dict(BOULDERS)
    remaining["namaqualand_boulder_03"] -= 1
    talus = 18
    for model in sorted(remaining):
        for index in range(remaining[model]):
            for _ in range(200):
                if talus > 0:
                    u, v = rng.uniform(-38.0, -30.0), rng.uniform(-80.0, 60.0)
                else:
                    u = rng.uniform(-30.0, 70.0)
                    v = rng.choice((1.0, -1.0)) * rng.uniform(52.0, 75.0)
                x, z = (u - v) * SQRT_HALF, (u + v) * SQRT_HALF
                if layout.pad_distance(x, z) > 2.0:
                    break
            talus -= 1
            add_rock(model, x, z, rng.uniform(0.0, 360.0), rng.uniform(0.8, 1.6), 0.15)
    s.instantiate(scan_path("namaqualand_boulder_03"), "HeroBoulder", rocks, {
        "Transform": transform((hero[0], hero[1] - 0.2, hero[2]), 30.0, 1.4), "BoxCollider": box_collider(SCAN_BOUNDS["namaqualand_boulder_03"])})
    s.rename({"$ref": "HeroBoulder"}, "HeroBoulder")
    for model, instances in sorted(rock_instances.items()):
        s.add("prefab.instantiate", {"prefab": scan_path(model), "instances": instances})

    # Dead brush on the dirt: most near the apron's edge, the rest at the feet of the slopes; never on a pad or in front
    # of a shot's camera.
    brush = s.create("Brush")
    cameras = [p for shot in report for p in (shot["key"], shot["from"], shot["to"])]
    instances = []
    near, feet = BRUSH
    while len(instances) < near + feet:
        if len(instances) < near:
            x, z = rng.uniform(APRON_X[0] - 14.0, APRON_X[1] + 14.0), rng.uniform(APRON_Z[0] - 14.0, APRON_Z[1] + 14.0)
            d = layout.outline.signed_distance(x, z)
            if d < 1.0 or rng.random() > math.exp(-(d - 1.0) / 6.0):
                continue
        else:
            u, v = rng.uniform(-36.0, -26.0), rng.uniform(-70.0, 50.0)
            x, z = (u - v) * SQRT_HALF, (u + v) * SQRT_HALF
        if any(name != "Apron" and distance(x, z) < 1.0 for name, distance, _ in layout.pads):
            continue
        if any((x - c[0]) ** 2 + (z - c[2]) ** 2 < 5.0 ** 2 for c in cameras):
            continue
        scale = rng.uniform(0.8, 1.4)
        instances.append({"parent": brush, "components": {"Transform": transform((x, world.ground.surface(x, z) - 0.02, z),
                                                                                   rng.uniform(0.0, 360.0), scale)}})
    s.add("prefab.instantiate", {"prefab": scan_path("dry_branches_medium_01"), "instances": instances})

    # The six probes: Golden's on (the saved station), the others off.
    stations_parent = s.create("Stations")
    for name, *_ in STATIONS:
        for part, position, yaw_degrees, extents, blend, priority, box_projection, _ in PROBES:
            probe = "Probe.%s.%s" % (name, part)
            s.create(probe, stations_parent, {"Transform": transform(position, yaw_degrees),
                                              "ReflectionProbe": {"Extents": list(extents), "BlendDistance": blend,
                                                                  "BoxProjection": box_projection, "Priority": priority, "Intensity": 1.0}})
            if name != "Golden":
                s.add("entity.setActive", {"entity": {"$ref": probe}, "active": False})

    # The tour's markers, the bookmark of the rock macro and the measurement views.
    tour = s.create("Tour")
    for shot in report:
        for part in ("from", "key", "to", "look"):
            s.create("Shot%d.%s" % (shot["shot"], part.capitalize()), tour, {"Transform": transform(shot[part])})
    toward = normalize((toward_sun(*station("Golden")[3:5])[0], 0.0, toward_sun(*station("Golden")[3:5])[2]))
    surface = (hero[0] + toward[0] * 1.2 * 1.4, hero[1] + 0.6, hero[2] + toward[2] * 1.2 * 1.4)
    s.create("Macro.Key", tour, {"Transform": transform(add(surface, toward, 0.45))})
    s.create("Macro.Look", tour, {"Transform": transform(surface)})
    yard_eye, yard_look, _ = MEASURE_YARD
    yard_look = (yard_look[0], surface_at(world, yard_look[0], yard_look[2]), yard_look[2])
    hut_eye_local, hut_look_local, _ = MEASURE_HUT_LOCAL
    hut_eye = local_to_world(HUT["center"], HUT["yaw"], hut_eye_local[0], hut_eye_local[2])
    hut_look = local_to_world(HUT["center"], HUT["yaw"], hut_look_local[0], hut_look_local[2])
    for name, position in (("Measure.Yard", yard_eye), ("Measure.YardLook", yard_look),
                           ("Measure.Hut", (hut_eye[0], hut_eye_local[1], hut_eye[1])),
                           ("Measure.HutLook", (hut_look[0], hut_look_local[1], hut_look[1]))):
        s.create(name, tour, {"Transform": transform(position)})

    # The overlay: the tour's fade and caption (the director shows them while it plays).
    s.add("ui.create", {"type": "canvas", "name": "Overlay"}, {"Overlay": "/entity"})
    s.add("component.set", {"entity": {"$ref": "Overlay"}, "component": "UICanvas", "values": {"ReceivesInput": False, "SortOrder": 10}})
    s.add("ui.create", {"type": "image", "parent": {"$ref": "Overlay"}, "name": "Fade", "anchor": "stretch"}, {"Fade": "/entity"})
    s.add("component.set", {"entity": {"$ref": "Fade"}, "component": "UIImage", "values": {"Color": [0.0, 0.0, 0.0, 1.0]}})
    s.add("ui.create", {"type": "text", "parent": {"$ref": "Overlay"}, "name": "Caption", "text": "", "anchor": "bottomCenter",
                        "position": [0, -64], "size": [1200, 64]}, {"Caption": "/entity"})
    s.add("component.set", {"entity": {"$ref": "Caption"}, "component": "UIText", "values": {"FontSize": 36}})
    s.add("entity.setActive", {"entities": [{"$ref": "Fade"}, {"$ref": "Caption"}], "active": False})

    # The director, and the flicker of the fire's and the lanterns' flames.
    for light, *_ in LOCAL_LIGHTS:
        s.add("script.add", {"entity": s.find(light, "flicker_" + bind_name(light)), "class": "Flicker"})
    fields = {"Sun": s.find("Sun"), "Fog": {"$ref": "Fog"}, "PostProcess": s.find("Post Process", "PostProcess"),
              "Player": player, "Eye": eye, "FadeImage": {"$ref": "Fade"}, "Caption": {"$ref": "Caption"},
              "FireLight": {"$ref": "flicker_Fire"}, "LanternALight": {"$ref": "flicker_LanternA.Light"},
              "LanternBLight": {"$ref": "flicker_LanternB.Light"}, "Embers": s.find("FirePit.Embers", "EmbersGlow"),
              "WickA": s.find("LanternA.Wick", "WickA"), "WickB": s.find("LanternB.Wick", "WickB")}
    for name, *_ in STATIONS:
        for part, *_ in PROBES:
            fields[name + part] = {"$ref": "Probe.%s.%s" % (name, part)}
    outpost = s.create("Outpost")
    s.add("script.add", {"entity": outpost, "class": "Outpost", "fields": fields})

    # Golden, the saved station, then save.
    s.steps += station_steps("Golden", True)
    s.add("scene.save")
    return s.steps


# The input actions (the showcase spec's D9): (map, action, kind, bindings).
INPUT_ACTIONS = (
    ("Gameplay", "Move", "Vector2", ["2D:Key/W,Key/S,Key/A,Key/D", "Gamepad/LeftStick"]),
    ("Gameplay", "Look", "Vector2", ["Gamepad/RightStick"]),
    ("Gameplay", "Jump", "Button", ["Key/Space", "Gamepad/A"]),
    ("Gameplay", "Run", "Button", ["Key/LeftShift", "Gamepad/RightBumper"]),
    ("Gameplay", "Up", "Button", ["Key/E", "Gamepad/RightTrigger+"]),
    ("Gameplay", "Down", "Button", ["Key/Q", "Gamepad/LeftTrigger+"]),
    ("Showcase", "Fly", "Button", ["Key/Tab", "Gamepad/Y"]),
    ("Showcase", "Photo", "Button", ["Key/P", "Gamepad/Start"]),
    ("Showcase", "HideUI", "Button", ["Key/H", "Gamepad/Back"]),
    ("Showcase", "Tour", "Button", ["Key/T"]),
) + tuple(("Showcase", "Bookmark%d" % n, "Button", ["Key/D%d" % n]) for n in range(1, 8))

# The photo mode's sliders (the showcase spec's D12): (slider, label, min, max, value, whole numbers).
PHOTO_SLIDERS = (
    ("TimeSlider", "TimeLabel", STATIONS[0][1], STATIONS[-1][1], station("Golden")[1], False),
    ("ExposureSlider", "ExposureLabel", -3.0, 3.0, 0.0, False),
    ("FovSlider", "FovLabel", 20.0, 90.0, 40.0, False),
    ("FocusSlider", "FocusLabel", 0.0, 1.0, 0.5, False),
    ("ApertureSlider", "ApertureLabel", 0.0, 8.0, 2.0, True),
)


def controls_script():
    """The editor command script of the director's controls (S5): the project's input actions, the photo mode's canvas
    (its widgets reporting to Outpost, which the script's fields name), the loading scene and the export setting naming
    it. Run once on the laid-out scene; it saves both scenes."""
    s = SceneScript()
    s.add("scene.open", {"scene": SCENE})
    s.add("script.build", expect={"/success": {"equals": True}})
    for map_name in ("Gameplay", "Showcase"):
        s.add("inputActions.setMap", {"map": map_name})
    for map_name, action, kind, bindings in INPUT_ACTIONS:
        s.add("inputActions.setAction", {"action": "%s/%s" % (map_name, action), "kind": kind, "bindings": bindings})
    outpost = s.find("Outpost")
    target = {"EventTarget": outpost}

    def ui(kind, name, parent, **extra):
        parameters = {"type": kind, "name": name}
        if parent is not None:
            parameters["parent"] = {"$ref": parent}
        parameters.update(extra)
        bind = {name: "/entity"}
        if kind in ("toggle", "button"):
            bind[name + "Label"] = "/parts/label"
        s.add("ui.create", parameters, bind)
        if kind in ("toggle", "button"):
            # The widget kit's labels are sized for menus: the panel's are small and stay on one line.
            s.add("component.set", {"entity": {"$ref": name + "Label"}, "component": "UIText", "values": {"FontSize": 14, "Wrap": False}})

    def set_component(name, component, values):
        s.add("component.set", {"entity": {"$ref": name}, "component": component, "values": values})

    def label(name, text):
        ui("text", name, "PhotoRows", text=text, size=[0, 20])
        set_component(name, "UIText", {"FontSize": 15, "Alignment": "Left"})

    ui("canvas", "PhotoMode", None)
    set_component("PhotoMode", "UICanvas", {"ReferenceResolution": [1280, 720], "MatchWidthOrHeight": 1.0, "SortOrder": 5})
    ui("image", "PhotoPanel", "PhotoMode", anchor="middleRight", position=[-16, 0], size=[300, 600])
    set_component("PhotoPanel", "UIImage", {"Color": [0.04, 0.04, 0.05, 0.72], "CornerRadius": 10})
    ui("verticalLayout", "PhotoRows", "PhotoPanel", anchor="stretch")
    set_component("PhotoRows", "UILayout", {"Padding": [14, 14, 14, 14], "Spacing": [0, 4], "StretchChildren": True})
    for slider, text, low, high, value, whole in PHOTO_SLIDERS:
        label(text, text)
        ui("slider", slider, "PhotoRows", size=[0, 18])
        set_component(slider, "UISlider", dict(target, MinValue=low, MaxValue=high, Value=value, WholeNumbers=whole))
    for toggle, text in (("DofToggle", "Depth of field"), ("AutoFocusToggle", "Autofocus")):
        ui("toggle", toggle, "PhotoRows", text=text, size=[0, 24])
        set_component(toggle, "UIToggle", dict(target, IsOn=toggle == "AutoFocusToggle"))
    label("QualityLabel", "Quality")
    ui("gridLayout", "QualityRow", "PhotoRows", size=[0, 48])
    set_component("QualityRow", "UILayout", {"CellSize": [128, 22], "Columns": 2, "Spacing": [6, 4]})
    for preset in ("Low", "Medium", "High", "Ultra"):
        ui("toggle", "Preset" + preset, "QualityRow", text=preset)
        set_component("Preset" + preset, "UIToggle", dict(target, Group={"$ref": "QualityRow"}, IsOn=preset == "High"))
    label("AntiAliasingLabel", "Anti-aliasing")
    ui("button", "AntiAliasing", "PhotoRows", text="Change", size=[0, 26])
    set_component("AntiAliasing", "UIButton", target)
    label("BookmarksLabel", "Bookmarks")
    ui("gridLayout", "BookmarkRow", "PhotoRows", size=[0, 28])
    set_component("BookmarkRow", "UILayout", {"CellSize": [34, 26], "Columns": 7, "Spacing": [3, 0]})
    for n in range(1, 8):
        ui("button", "Bookmark%d" % n, "BookmarkRow", text=str(n))
        set_component("Bookmark%d" % n, "UIButton", target)
    s.add("entity.setActive", {"entity": {"$ref": "PhotoPanel"}, "active": False})
    for field in ["PhotoPanel", "QualityLabel", "AntiAliasingLabel", "DofToggle", "AutoFocusToggle"] + [n for row in PHOTO_SLIDERS for n in row[:2]]:
        s.add("script.setField", {"entity": outpost, "class": "Outpost", "field": field, "value": {"$ref": field}})
    s.add("scene.save")

    # The loading scene: a black camera, the title and a bar LoadingScreen fills.
    s.add("scene.new", {"name": "Loading"})
    s.create("Camera", components={"Camera": {"Primary": True, "ClearColor": [0.0, 0.0, 0.0, 1.0]}})
    ui("canvas", "LoadingUI", None)
    ui("text", "Title", "LoadingUI", text="High Desert Outpost", anchor="middleCenter", position=[0, -60], size=[1200, 80])
    set_component("Title", "UIText", {"FontSize": 56})
    ui("image", "Track", "LoadingUI", anchor="middleCenter", position=[0, 30], size=[640, 10])
    set_component("Track", "UIImage", {"Color": [1.0, 1.0, 1.0, 0.15], "CornerRadius": 5})
    ui("image", "Bar", "Track", anchor="stretch")
    set_component("Bar", "UIImage", {"Color": [0.95, 0.72, 0.42, 1.0], "CornerRadius": 5, "Type": "Filled", "FillAmount": 0.0})
    ui("text", "Label", "LoadingUI", text="Loading the outpost...", anchor="middleCenter", position=[0, 70], size=[800, 40])
    set_component("Label", "UIText", {"FontSize": 22})
    loading = s.create("Loading")
    s.add("script.add", {"entity": loading, "class": "LoadingScreen", "fields": {"Bar": {"$ref": "Bar"}, "Label": {"$ref": "Label"}}})
    s.add("scene.saveAs", {"path": "Scenes/Loading.stscene"})
    s.add("project.setExportSettings", {"loadingScene": "Scenes/Loading.stscene"})
    s.add("scene.open", {"scene": SCENE})
    return s.steps


# The stations in the order the bake takes them: the saved one last, so the scene it saves is that station's.
BAKE_ORDER = ("Dawn", "Dusk", "Golden")
# How often the bake runs lighting.bakeProbes at a station. A bake draws the probes' present captures, the baked probe's
# own included, so every pass adds the light the last one found (a bounce), and what a few passes leave depends on the
# captures the project held before: two passes from the sample's earlier captures left the dusk hut's 18% above the one
# further passes settle at. Measured in the hut, where the capture's own light counts most, a pass leaves about 0.16 of
# the difference to the settled capture. After eight nothing of the start is left: bakes begun from two different sets of
# captures agree in the mean of every 64 x 64 block to 0.01%, so one run of the script reproduces the committed captures
# whatever the project held (a pass of a station's two probes takes the editor about a third of a second).
BAKE_PASSES = 8


def bake_script():
    """The probes' bake (D10): each station's edit-mode steps with only its own probes on, then lighting.bakeProbes of its
    two BAKE_PASSES times (until the captures no longer depend on those the project held), the saved station last so the
    saved scene is its. A probe baked elsewhere (the hut's) is moved to its bake position and box first and back to its
    own after; its box and blend are set with them, so the script also brings a scene laid out with other values to the
    table's."""
    steps = [{"command": "scene.open", "parameters": {"scene": SCENE}}]
    for name in BAKE_ORDER:
        steps += station_steps(name, True)
        probes = [{"$ref": "station_Probe.%s.%s" % (name, part[0])} for part in PROBES]
        moved = [part for part in PROBES if part[7] is not None]
        for part, position, yaw_degrees, extents, blend, priority, box_projection, bake in moved:
            entity = {"$ref": "station_Probe.%s.%s" % (name, part)}
            steps.append({"command": "component.set", "parameters": {"entity": entity, "component": "Transform",
                                                                     "values": {"Translation": rounded(bake[0])}}})
            steps.append({"command": "component.set", "parameters": {"entity": entity, "component": "ReflectionProbe",
                                                                     "values": {"Extents": rounded(bake[1])}}})
        for _ in range(BAKE_PASSES):
            steps.append({"command": "lighting.bakeProbes", "parameters": {"entities": probes}})
        for part, position, yaw_degrees, extents, blend, priority, box_projection, bake in moved:
            entity = {"$ref": "station_Probe.%s.%s" % (name, part)}
            steps.append({"command": "component.set", "parameters": {"entity": entity, "component": "Transform",
                                                                     "values": {"Translation": rounded(position)}}})
            steps.append({"command": "component.set", "parameters": {"entity": entity, "component": "ReflectionProbe",
                                                                     "values": {"Extents": rounded(extents), "BlendDistance": blend,
                                                                                "BoxProjection": box_projection}}})
    steps.append({"command": "scene.save"})
    return steps


SAVED_STATION = "Golden"     # The station the scene is saved at: the one the game starts in and the thumbnail shows
SHOT_COUNT = 6               # The tour's shots (shots), one screenshot each
PICTURE_SIZE = (960, 540)    # Screenshots/Shot<N>.png
ICON_SIZE = 256              # Assets/Icon.png, of shot 2
THUMBNAIL_SIZE = (480, 270)  # .strata/Thumbnail.png and Picture.png (ProjectThumbnail's c_Width and c_Height)
# What a picture's capture must report. viewport.capture waits for all of it, but only so long, and then returns the
# picture as it is, saying what it lacks: assets still loading, textures below the level the view wants, local lights'
# shadow views or probes' captures not drawn yet, an exposure still adapting, or a history of temporal anti-aliasing that
# holds fewer than the jitter's eight phases (a soft or noisy picture).
PICTURE_COMPLETE = {"/pendingAssets": {"equals": 0}, "/texturesBelowWanted": {"equals": 0},
                    "/localShadowViewsPending": {"equals": 0}, "/reflectionProbesPending": {"equals": 0},
                    "/preExposureSettled": {"equals": True}, "/temporalFrames": {"min": 8}}
# The project's thumbnail renders over the frames after a save, within ProjectThumbnail's limits (c_MaxFrames 120,
# c_MaxTemporalFrames 16, c_MaxExposureFrames 8, c_MaxGIFrames 54 and c_MaxReflectionFrames 2: 200 frames if each ran
# out in turn), is read back and then written in the background; a script that quits right after its scene.save leaves
# none. The script waits three times that; picture refuses a thumbnail the wait did not bring.
THUMBNAIL_RENDER_FRAMES = 200
THUMBNAIL_FRAMES = 3 * THUMBNAIL_RENDER_FRAMES
# Picture.png against Screenshots/Shot2.png, both averaged over 16 x 9 blocks: how far the blocks' mean codes may lie
# apart on average and in any block. The two are one view rendered twice, at 1440 x 810 and at 960 x 540 (measured 0.51
# and 1.67 on the pictures of wave 8's reflections; the pictures taken before them against the ones after: 1.09 and 7.71).
PICTURE_BLOCK = 30
PICTURE_LIMITS = (1.0, 4.0)


def pictures_script(world):
    """The pictures' retake (D14's pictures): each shot's key camera at its station, captured at High without overlays into
    Screenshots/Shot<N>.png, shot 2 also into Assets/Icon.png (256 x 256; the asset keeps its handle), then the saved
    station and a scene.save, after which the editor renders the project's thumbnail (.strata/Thumbnail.png, 480 x 270,
    from the scene's camera, which the layout stands at shot 2's key) that picture copies to Picture.png. A capture that
    reports an incomplete or unsettled picture stops the script (PICTURE_COMPLETE)."""
    steps = [{"command": "scene.open", "parameters": {"scene": SCENE}}]
    for shot in shots(world):
        camera = {"position": rounded(shot["key"], 3), "target": rounded(shot["look"], 3), "fov": shot["fov"]}
        steps += station_steps(shot["station"], True)
        pictures = [("Screenshots/Shot%d.png" % shot["shot"], PICTURE_SIZE)]
        if shot["shot"] == 2:
            pictures.append(("Assets/Icon.png", (ICON_SIZE, ICON_SIZE)))
        for picture, (width, height) in pictures:
            parameters = dict(camera, width=width, height=height, quality="high", overlays=False, path=picture, overwrite=True)
            steps.append({"command": "viewport.capture", "parameters": parameters, "expect": copy.deepcopy(PICTURE_COMPLETE)})
    steps += station_steps(SAVED_STATION, True)
    steps.append({"command": "scene.save"})
    steps.append({"command": "editor.wait", "parameters": {"frames": THUMBNAIL_FRAMES}})
    return steps


def station_block_at(steps, index, blocks):
    """The station whose steps (blocks: station_steps per name) start at steps[index] and follow without a gap, or None."""
    for name, block in blocks.items():
        if steps[index:index + len(block)] == block:
            return name
    return None


def check_pictures(world, shots_file, steps=None):
    """The pictures script (steps; pictures_script's when None) against the shots: it opens the scene, takes each
    screenshot (and the icon, of shot 2) right after its station's steps, from the camera of its LookDev shot (shots_file's
    showcase.shot<N>), at its size, at High, without overlays and expecting a complete picture, and ends at the saved
    station with a scene.save and a wait long enough for the thumbnail the save starts. Returns the failures."""
    failures = []
    with open(shots_file, encoding="utf-8") as file:
        lookdev = {shot["Name"]: shot for shot in json.load(file)["Shots"]}
    if steps is None:
        steps = pictures_script(world)
    blocks = {row[0]: station_steps(row[0], True) for row in STATIONS}
    expected = {"Screenshots/Shot%d.png" % shot["shot"]: (shot, PICTURE_SIZE) for shot in shots(world)}
    if sorted(expected) != ["Screenshots/Shot%d.png" % (number + 1) for number in range(SHOT_COUNT)]:
        return ["the tour's shots are not numbered 1 to %d" % SHOT_COUNT]
    expected["Assets/Icon.png"] = (expected["Screenshots/Shot2.png"][0], (ICON_SIZE, ICON_SIZE))
    if not steps or steps[0] != {"command": "scene.open", "parameters": {"scene": SCENE}}:
        failures.append("the pictures script does not start by opening %s" % SCENE)
    station = None  # The station whose steps were the last thing before this step, captures aside
    taken = []
    saved = []
    index = 0
    while index < len(steps):
        name = station_block_at(steps, index, blocks)
        if name is not None:
            station = name
            index += len(blocks[name])
            continue
        step = steps[index]
        index += 1
        if step.get("command") == "scene.save":
            saved.append(station)
            continue
        if step.get("command") != "viewport.capture":
            station = None  # Anything else, a station's step out of its place included, leaves the scene at no station
            continue
        parameters = step.get("parameters", {})
        path = parameters.get("path")
        taken.append(path)
        if path not in expected:
            failures.append("the pictures script captures %s, which is no picture of the sample" % path)
            continue
        shot, (width, height) = expected[path]
        if station != shot["station"]:
            failures.append("%s is not taken right after the steps of %s, shot %d's station" % (path, shot["station"], shot["shot"]))
        reference = lookdev.get("showcase.shot%d" % shot["shot"])
        if reference is None:
            failures.append("%s has no LookDev shot showcase.shot%d" % (shots_file, shot["shot"]))
            continue
        wanted = {"position": reference["Position"], "target": reference["Target"], "fov": reference["Fov"],
                  "width": width, "height": height, "quality": "high", "overlays": False, "path": path, "overwrite": True}
        if sorted(parameters) != sorted(wanted):
            failures.append("%s is captured with the parameters %s, not %s" % (path, sorted(parameters), sorted(wanted)))
            continue
        camera = list(parameters["position"]) + list(parameters["target"]) + [parameters["fov"]]
        lookdev_camera = list(wanted["position"]) + list(wanted["target"]) + [wanted["fov"]]
        if len(camera) != len(lookdev_camera) or any(abs(a - b) > 1e-3 for a, b in zip(camera, lookdev_camera)):
            failures.append("%s looks from %s, LookDev's showcase.shot%d from %s" % (path, camera, shot["shot"], lookdev_camera))
        for key in ("width", "height", "quality", "overlays", "overwrite"):
            if parameters[key] != wanted[key] or type(parameters[key]) is not type(wanted[key]):
                failures.append("%s is captured with %s %s, not %s" % (path, key, json.dumps(parameters[key]), json.dumps(wanted[key])))
        if step.get("expect") != PICTURE_COMPLETE:
            failures.append("%s's capture does not expect a complete, settled picture (%s)" % (path, ", ".join(sorted(PICTURE_COMPLETE))))
    if sorted(taken) != sorted(expected):
        failures.append("the pictures script captures %s, not %s" % (sorted(str(path) for path in taken), sorted(expected)))
    wait = steps[-1] if steps else {}
    frames = wait.get("parameters", {}).get("frames", 0) if wait.get("command") == "editor.wait" else 0
    if saved != [SAVED_STATION] or len(steps) < 2 or steps[-2].get("command") != "scene.save":
        failures.append("the pictures script does not end with one scene.save right after %s's steps and a wait" % SAVED_STATION)
    if not isinstance(frames, int) or frames < THUMBNAIL_FRAMES:
        failures.append("the pictures script does not end with a wait of at least %d frames for the thumbnail" % THUMBNAIL_FRAMES)
    return failures


def picture_mutations(world):
    """The pictures script with one thing wrong at a time, each of which check_pictures must report: (what, steps)."""
    base = pictures_script(world)
    captures = [index for index, step in enumerate(base) if step["command"] == "viewport.capture"]
    saved_block = len(station_steps(SAVED_STATION, True))
    mutations = []

    def mutate(what, change):
        steps = copy.deepcopy(base)
        change(steps)
        mutations.append((what, steps))

    def set_parameter(index, key, value):
        return lambda steps: steps[index]["parameters"].__setitem__(key, value)

    mutate("a picture of another size", set_parameter(captures[0], "width", 1920))
    mutate("a picture at another quality", set_parameter(captures[1], "quality", "medium"))
    mutate("a picture with the editor's overlays", set_parameter(captures[2], "overlays", True))
    mutate("a picture from another camera", set_parameter(captures[3], "fov", base[captures[3]]["parameters"]["fov"] + 5.0))
    mutate("a picture that keeps an older file", set_parameter(captures[4], "overwrite", False))
    mutate("a picture of another name", set_parameter(captures[5], "path", "Screenshots/Shot7.png"))
    mutate("a picture with a debug view", set_parameter(captures[6], "debugView", "lighting"))
    mutate("a capture that expects nothing", lambda steps: steps[captures[0]].pop("expect"))
    for key in sorted(PICTURE_COMPLETE):
        mutate("a capture that does not expect %s" % key, lambda steps, key=key: steps[captures[-1]]["expect"].pop(key))
    mutate("a missing picture", lambda steps: steps.pop(captures[0]))
    mutate("a station's step changed by hand", set_parameter(captures[-1] - 1, "active", False))
    mutate("a step between a station's steps and its picture",
           lambda steps: steps.insert(captures[-1], {"command": "editor.wait", "parameters": {"frames": 1}}))
    mutate("a picture at another station", lambda steps: steps.__delitem__(slice(captures[-1] - len(station_steps("Dusk", True)), captures[-1])))
    mutate("a scene that is not opened", lambda steps: steps.pop(0))
    mutate("a save at another station", lambda steps: steps.__delitem__(slice(len(steps) - 2 - saved_block, len(steps) - 2)))
    mutate("no save", lambda steps: steps.pop(len(steps) - 2))
    mutate("a wait too short for the thumbnail", set_parameter(len(base) - 1, "frames", THUMBNAIL_RENDER_FRAMES))
    mutate("no wait", lambda steps: steps.pop())
    return mutations


def probe_reference(station_name, part):
    """How a station's steps name one of its probes (station_steps' binding of the entity it found)."""
    return {"$ref": "station_" + bind_name("Probe.%s.%s" % (station_name, part))}


def check_bake(steps=None):
    """The bake's script (steps; bake_script's when None) against the probes' table: it opens the scene; every station is
    baked once, the saved one last; a station's bakes follow its steps with nothing between but component.set steps of its
    probes, and are BAKE_PASSES bakes of exactly its probes, each standing where the table bakes it, with the box it is
    baked with; when another station's steps or the save follow, a probe that was moved is back at the table's place, box,
    blend and projection; and the script ends with its one scene.save. Returns the failures."""
    failures = []
    if steps is None:
        steps = bake_script()
    blocks = {row[0]: station_steps(row[0], True) for row in STATIONS}
    opened = bool(steps) and steps[0] == {"command": "scene.open", "parameters": {"scene": SCENE}}
    if not opened:
        failures.append("the bake script does not start by opening %s" % SCENE)

    def same(first, second):
        return json.dumps(first, sort_keys=True) == json.dumps(second, sort_keys=True)

    def close(name, placed, passes):
        """A station's bakes are over (another station's steps or the save follow): how many there were, and where its
        moved probes were left."""
        if passes != BAKE_PASSES:
            failures.append("%s's probes are baked %d times, not %d" % (name, passes, BAKE_PASSES))
        for part, position, yaw_degrees, extents, blend, priority, box_projection, bake in PROBES:
            if bake is None:
                continue
            wanted = {"Translation": rounded(position), "Extents": rounded(extents), "BlendDistance": blend, "BoxProjection": box_projection}
            left = {field: placed.get((part, field)) for field in wanted}
            if not same(left, wanted):
                failures.append("Probe.%s.%s is left with %s after its bakes, not the table's %s"
                                % (name, part, json.dumps(left, sort_keys=True), json.dumps(wanted, sort_keys=True)))

    station = None  # The station whose steps came last
    placed = {}     # What the script set on that station's probes since: (part, field) -> value
    passes = 0      # That station's bakes so far
    order = []
    saves = 0
    foreign = []
    index = 1 if opened else 0
    while index < len(steps):
        name = station_block_at(steps, index, blocks)
        if name is not None:
            if station is not None:
                close(station, placed, passes)
            station, placed, passes = name, {}, 0
            order.append(name)
            index += len(blocks[name])
            continue
        step = steps[index]
        index += 1
        command, parameters = step.get("command"), step.get("parameters", {})
        if command == "scene.save":
            saves += 1
            if station is not None:
                close(station, placed, passes)
            station, placed, passes = None, {}, 0
            continue
        parts = [] if station is None else [part[0] for part in PROBES if same(parameters.get("entity"), probe_reference(station, part[0]))]
        if (command == "component.set" and parts and sorted(parameters) == ["component", "entity", "values"]
                and parameters["component"] in ("Transform", "ReflectionProbe") and isinstance(parameters["values"], dict)):
            for field, value in parameters["values"].items():
                placed[(parts[0], field)] = value
            continue
        if command == "lighting.bakeProbes" and station is not None:
            passes += 1
            wanted = {"entities": [probe_reference(station, part[0]) for part in PROBES]}
            if not same(parameters, wanted):
                failures.append("a bake at %s has the parameters %s, not %s" % (station, json.dumps(parameters, sort_keys=True), json.dumps(wanted, sort_keys=True)))
            for part, position, yaw_degrees, extents, blend, priority, box_projection, bake in PROBES:
                stands = {field: placed.get((part, field)) for field in ("Translation", "Extents")}
                wanted = {"Translation": None, "Extents": None} if bake is None else {"Translation": rounded(bake[0]), "Extents": rounded(bake[1])}
                if not same(stands, wanted):
                    failures.append("Probe.%s.%s is baked with %s set, not %s (null: as the scene has it)"
                                    % (station, part, json.dumps(stands, sort_keys=True), json.dumps(wanted, sort_keys=True)))
            continue
        foreign.append("step %d (%s)" % (index, command))
    if station is not None:
        close(station, placed, passes)
    if foreign:
        failures.append("the bake script holds what is no part of a station's bake: %s%s" % (", ".join(foreign[:3]), " and more" if len(foreign) > 3 else ""))
    if sorted(order) != sorted(blocks):
        failures.append("the bake script bakes the stations %s, not each of %s once" % (order, sorted(blocks)))
    elif order[-1] != SAVED_STATION:
        failures.append("the bake script bakes %s last, not %s, the station the scene is saved at" % (order[-1], SAVED_STATION))
    if saves != 1 or not steps or steps[-1].get("command") != "scene.save":
        failures.append("the bake script does not end with its one scene.save")
    return failures


def bake_mutations():
    """The bake's script with one thing wrong at a time, each of which check_bake must report: (what, steps)."""
    base = bake_script()
    blocks = {row[0]: station_steps(row[0], True) for row in STATIONS}
    starts = [index for index in range(len(base)) if station_block_at(base, index, blocks) is not None]
    first_end = starts[0] + len(blocks[station_block_at(base, starts[0], blocks)])
    bakes = [index for index in range(first_end, starts[1]) if base[index]["command"] == "lighting.bakeProbes"]
    # The first station's steps for the probes it moves: to the bake's place and box before the bakes, back after them.
    before = [index for index in range(first_end, bakes[0]) if base[index]["command"] == "component.set"]
    after = [index for index in range(bakes[-1] + 1, starts[1]) if base[index]["command"] == "component.set"]
    unmoved = [part[0] for part in PROBES if part[7] is None]
    first_name = station_block_at(base, starts[0], blocks)
    mutations = []

    def mutate(what, change):
        steps = copy.deepcopy(base)
        change(steps)
        mutations.append((what, steps))

    def set_value(index, key, value):
        return lambda steps: steps[index]["parameters"]["values"].__setitem__(key, value)

    mutate("a pass fewer", lambda steps: steps.pop(bakes[0]))
    mutate("a pass more", lambda steps: steps.insert(bakes[0], copy.deepcopy(steps[bakes[0]])))
    mutate("a bake of one probe only", lambda steps: steps[bakes[1]]["parameters"]["entities"].pop())
    mutate("a bake of another station's probes",
           lambda steps: steps[bakes[0]]["parameters"].__setitem__("entities", copy.deepcopy(steps[len(steps) - 2 - len(after)]["parameters"]["entities"])))
    mutate("a bake with a parameter of its own", lambda steps: steps[bakes[-1]]["parameters"].__setitem__("resolution", 256))
    mutate("a probe baked where it stands, not where the table bakes it", lambda steps: steps.pop(before[0]))
    mutate("a probe baked with its own box, not the bake's", lambda steps: steps.pop(before[1]))
    mutate("a probe put back before its last bake", lambda steps: steps.insert(bakes[-1], steps.pop(after[0])))
    mutate("a probe left where it was baked", lambda steps: steps.pop(after[0]))
    mutate("a probe left with the bake's box", lambda steps: steps.pop(after[-1]))
    mutate("a probe left with another blend distance", set_value(after[-1], "BlendDistance", base[after[-1]]["parameters"]["values"]["BlendDistance"] + 0.1))
    mutate("a probe left with its box projection turned", set_value(after[-1], "BoxProjection", not base[after[-1]]["parameters"]["values"]["BoxProjection"]))
    if unmoved:
        mutate("a probe moved that the table bakes where it stands",
               lambda steps: steps.insert(bakes[0], {"command": "component.set", "parameters": {
                   "entity": probe_reference(first_name, unmoved[0]), "component": "Transform", "values": {"Translation": [0.0, 1.0, 0.0]}}}))
    mutate("a station left out", lambda steps: steps.__delitem__(slice(starts[1], starts[2])))
    mutate("a station baked twice", lambda steps: steps.__setitem__(slice(starts[1], starts[1]), copy.deepcopy(steps[starts[0]:starts[1]])))
    mutate("the saved station baked first",
           lambda steps: steps.__setitem__(slice(starts[0], len(steps) - 1), steps[starts[-1]:len(steps) - 1] + steps[starts[0]:starts[-1]]))
    mutate("a station's step changed by hand", lambda steps: steps[first_end - 1]["parameters"].__setitem__("active", not steps[first_end - 1]["parameters"]["active"]))
    mutate("a step between a station's steps and its bakes", lambda steps: steps.insert(first_end, {"command": "editor.wait", "parameters": {"frames": 1}}))
    mutate("a scene that is not opened", lambda steps: steps.pop(0))
    mutate("a save before the end", lambda steps: steps.insert(starts[1], {"command": "scene.save"}))
    mutate("no save", lambda steps: steps.pop())
    return mutations


LOOKDEV_SHOT_NAME = re.compile(r"^showcase\.shot(\d+)(?:\.|$)")


def check_station_copies(world, render_file, shots_file):
    """The stations' steps where other files hold them: every lighting.applySkyPreset of the render test's commands
    (render_file) is part of a station's steps as station_steps writes them (the script form), with all three stations
    there; in LookDev's shots (shots_file) the Setup of every showcase.shot<N>, variants included, of a station other than
    the saved one begins with that station's steps (the LookDev form), and no other showcase shot sets a sky (edit-mode
    shots show the saved scene, live ones the director's stations). Returns the failures."""
    failures = []
    blocks = {row[0]: station_steps(row[0], True) for row in STATIONS}
    render_name = os.path.basename(render_file)
    with open(render_file, encoding="utf-8") as file:
        steps = json.load(file)
    found = set()
    index = 0
    while index < len(steps):
        name = station_block_at(steps, index, blocks)
        if name is not None:
            found.add(name)
            index += len(blocks[name])
            continue
        if steps[index].get("command") == "lighting.applySkyPreset":
            failures.append("%s: the sky preset of step %d is not part of a station's steps as `setup --format script` writes them"
                            % (render_name, index))
        index += 1
    for row in STATIONS:
        if row[0] not in found:
            failures.append("%s holds no steps of %s as `setup --station %s --format script` writes them" % (render_name, row[0], row[0]))

    shots_name = os.path.basename(shots_file)
    lookdev_blocks = {row[0]: station_steps(row[0], False) for row in STATIONS}
    stations_of = {shot["shot"]: shot["station"] for shot in shots(world)}
    with open(shots_file, encoding="utf-8") as file:
        lookdev = json.load(file)["Shots"]
    for shot in lookdev:
        if shot.get("Project") != "Showcase":
            continue
        setup = shot.get("Setup", [])
        presets = sum(1 for entry in setup if entry.get("Command") == "lighting.applySkyPreset")
        match = LOOKDEV_SHOT_NAME.match(shot["Name"])
        wanted = stations_of.get(int(match.group(1))) if match else None
        if wanted is None or wanted == SAVED_STATION:
            if presets:
                failures.append("%s: %s sets a sky, but shows %s" % (shots_name, shot["Name"], "the saved scene" if not shot.get("Live") else "the director's stations"))
            continue
        block = lookdev_blocks[wanted]
        if setup[:len(block)] != block or presets != 1:
            failures.append("%s: %s's Setup does not begin with %s's steps as `setup --station %s --format lookdev` prints them"
                            % (shots_name, shot["Name"], wanted, wanted))
    return failures


def lookdev_module():
    """Tools/LookDev.py, for its PNG reader and writer (8-bit RGB and RGBA, all five filters)."""
    tools = os.path.dirname(os.path.abspath(__file__))
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import LookDev
    return LookDev


def block_means(image, size):
    """The mean code of an image's red, green and blue over each size x size block, row by row."""
    means = []
    for top in range(0, image.height - size + 1, size):
        for left in range(0, image.width - size + 1, size):
            total = 0
            for y in range(top, top + size):
                start = (y * image.width + left) * image.channels
                row = image.data[start:start + size * image.channels]
                total += sum(row[0::image.channels]) + sum(row[1::image.channels]) + sum(row[2::image.channels])
            means.append(total / (size * size * 3.0))
    return means


def check_picture(picture, shot2):
    """A project picture (the thumbnail, or Picture.png) against Screenshots/Shot2.png: 480 x 270, and the same view at the
    same station, by the mean codes of 16 x 9 blocks (PICTURE_LIMITS). Returns the failures."""
    lookdev = lookdev_module()
    images = []
    for path, size in ((picture, THUMBNAIL_SIZE), (shot2, PICTURE_SIZE)):
        try:
            image = lookdev.read_png(path)
        except (OSError, lookdev.ToolError) as error:
            return ["%s cannot be read: %s" % (path, error)]
        if (image.width, image.height) != size:
            return ["%s is %d x %d, not %d x %d" % (path, image.width, image.height, size[0], size[1])]
        images.append(image)
    small = block_means(images[0], PICTURE_BLOCK)
    large = block_means(images[1], PICTURE_BLOCK * PICTURE_SIZE[0] // THUMBNAIL_SIZE[0])
    differences = [abs(a - b) for a, b in zip(small, large)]
    mean, most = sum(differences) / len(differences), max(differences)
    if mean > PICTURE_LIMITS[0] or most > PICTURE_LIMITS[1]:
        return ["%s does not show what %s shows: their blocks' mean codes differ by %.2f on average and %.2f at most (limits %.1f and %.1f); "
                "retake them together (pictures, then picture)" % (picture, shot2, mean, most, PICTURE_LIMITS[0], PICTURE_LIMITS[1])]
    return []


def copy_picture(project):
    """picture: copies the project's thumbnail to its Picture.png, once it is the one the pictures script left: there,
    480 x 270, not older than the screenshots and the icon that script took before its save, and showing what
    Screenshots/Shot2.png shows. Returns the failures; nothing is copied when there are any."""
    thumbnail = os.path.join(project, ".strata", "Thumbnail.png")
    if not os.path.isfile(thumbnail):
        return ["%s does not exist: run the pictures script on the project first" % thumbnail]
    failures = []
    written = os.path.getmtime(thumbnail)
    taken = ["Screenshots/Shot%d.png" % (index + 1) for index in range(SHOT_COUNT)] + ["Assets/Icon.png"]
    for relative in taken:
        path = os.path.join(project, *relative.split("/"))
        if not os.path.isfile(path):
            failures.append("%s does not exist" % path)
        elif os.path.getmtime(path) > written:
            # Equal times pass: file systems keep them in steps of up to two seconds.
            failures.append("%s is newer than %s: the pictures script did not write that thumbnail" % (path, thumbnail))
    if failures:
        return failures
    failures = check_picture(thumbnail, os.path.join(project, "Screenshots", "Shot2.png"))
    if failures:
        return failures
    shutil.copyfile(thumbnail, os.path.join(project, "Picture.png"))
    return []


def check_copy_picture(sample, work):
    """picture on stand-ins of the project under work: it copies the sample's picture when it is the thumbnail the script
    left, and refuses a thumbnail that is missing, of another size, older than a screenshot or of another view."""
    lookdev = lookdev_module()
    failures = []
    project = os.path.join(work, "picture")
    shutil.rmtree(project, ignore_errors=True)
    os.makedirs(os.path.join(project, "Screenshots"))
    os.makedirs(os.path.join(project, "Assets"))
    os.makedirs(os.path.join(project, ".strata"))
    for index in range(SHOT_COUNT):
        shutil.copyfile(os.path.join(sample, "Screenshots", "Shot%d.png" % (index + 1)), os.path.join(project, "Screenshots", "Shot%d.png" % (index + 1)))
    shutil.copyfile(os.path.join(sample, "Assets", "Icon.png"), os.path.join(project, "Assets", "Icon.png"))
    thumbnail = os.path.join(project, ".strata", "Thumbnail.png")
    picture = os.path.join(project, "Picture.png")
    taken = [os.path.join(project, "Screenshots", "Shot%d.png" % (index + 1)) for index in range(SHOT_COUNT)] + [os.path.join(project, "Assets", "Icon.png")]

    def stamp(thumbnail_time, picture_time):
        for path in taken:
            os.utime(path, (picture_time, picture_time))
        os.utime(thumbnail, (thumbnail_time, thumbnail_time))

    def refused(what):
        if not copy_picture(project):
            failures.append("picture copies %s" % what)
        elif os.path.exists(picture):
            failures.append("picture refuses %s, but writes Picture.png" % what)

    refused("a thumbnail that does not exist")
    shutil.copyfile(os.path.join(sample, "Screenshots", "Shot2.png"), thumbnail)
    stamp(2000000000, 2000000000)
    refused("a thumbnail of another size")
    shutil.copyfile(os.path.join(sample, "Picture.png"), thumbnail)
    stamp(2000000000, 2000000010)
    refused("a thumbnail older than the screenshots")
    image = lookdev.read_png(os.path.join(sample, "Picture.png"))
    stride = image.width * image.channels
    rows = [image.data[y * stride:(y + 1) * stride] for y in range(image.height)]
    image.data[:] = b"".join(reversed(rows))  # Upside down: the sky's codes where the ground's were
    lookdev.write_png(thumbnail, image)
    stamp(2000000010, 2000000000)
    refused("a thumbnail of another view")
    shutil.copyfile(os.path.join(sample, "Picture.png"), thumbnail)
    stamp(2000000000, 2000000000)
    problems = copy_picture(project)
    if problems:
        failures.append("picture refuses the thumbnail the script left: %s" % "; ".join(problems))
    else:
        with open(picture, "rb") as copied, open(thumbnail, "rb") as source:
            if copied.read() != source.read():
                failures.append("picture's Picture.png differs from the thumbnail")
    return failures


# ---------------------------------------------------------------------------------------------------------------------
# Checking captured shots (LookDev's output)

def read_capture(directory, name):
    path = os.path.join(directory, "shots", name)
    if not os.path.isfile(path):
        return None
    return lookdev_module().read_png(path)


def luma(image, x, y):
    i = (y * image.width + x) * image.channels
    d = image.data
    return 0.2126 * d[i] + 0.7152 * d[i + 1] + 0.0722 * d[i + 2]


def pixel_rect(image, rect):
    x0, y0, x1, y1 = rect
    return (max(0, int(x0 * image.width)), max(0, int(y0 * image.height)),
            min(image.width, int(math.ceil(x1 * image.width))), min(image.height, int(math.ceil(y1 * image.height))))


def crop_mean(image, rect):
    x0, y0, x1, y1 = pixel_rect(image, rect)
    total = sum(luma(image, x, y) for y in range(y0, y1) for x in range(x0, x1))
    return total / max(1, (x1 - x0) * (y1 - y0))


def image_bounds(points):
    xs = [p[0] for p in points if p is not None]
    ys = [p[1] for p in points if p is not None]
    return (min(xs), min(ys), max(xs), max(ys))


def check_stills(world, directory):
    """Shots 2 and 3 at 1920 x 1080 with TAA: the highlight and the streak where the mirror condition puts them, the yard's
    probe in the tank; shot 6: the metal catching the firelight."""
    results = []
    report = {shot["shot"]: shot for shot in shots(world)}
    surfaces = world.surfaces()
    shot2 = report[2]
    image = read_capture(directory, "showcase.shot2-1920x1080-taa.png")
    if image is None:
        results.append(("shot 2 highlight", False, "showcase.shot2-1920x1080-taa.png was not captured"))
    else:
        board = surfaces["letterboard"]
        expected = shot2["image"]["highlight"]
        point = shot2["mirror"]["point"]

        def centroid(z_range):
            corners = [project(shot2["key"], shot2["look"], shot2["fov"], (board["x"], y, z)) for y in board["y"] for z in z_range]
            x0, y0, x1, y1 = pixel_rect(image, image_bounds(corners))
            pixels = sorted(((luma(image, x, y), x, y) for y in range(y0, y1) for x in range(x0, x1)), reverse=True)
            brightest = pixels[:max(1, len(pixels) // 1000)]
            cx = sum(p[1] for p in brightest) / len(brightest) + 0.5
            cy = sum(p[2] for p in brightest) / len(brightest) + 0.5
            return cx, cy, math.hypot(cx - expected[0] * image.width, cy - expected[1] * image.height)

        # The highlight is measured on the letterboard's section under the mirror point (painted at the key, between the
        # two bare ones): bare steel beside it reflects more of everything and would outshine a correctly placed highlight.
        panel = board["z"]
        for low, high in board["sections"]:
            if low <= point[2] <= high:
                panel = (low, high)
        cx, cy, off = centroid(panel)
        wx, wy, whole = centroid(board["z"])
        results.append(("shot 2 highlight", off <= 40.0, "the brightest 0.1%% of the letterboard's section at z %.2f..%.2f centered at "
                        "(%.0f, %.0f), %.1f px from the computed (%.0f, %.0f) (at most 40); of the whole letterboard at (%.0f, %.0f), %.1f px"
                        % (panel[0], panel[1], cx, cy, off, expected[0] * image.width, expected[1] * image.height, wx, wy, whole)))
    shot3 = report[3]
    image = read_capture(directory, "showcase.shot3-1920x1080-taa.png")
    if image is None:
        results.append(("shot 3 streak", False, "showcase.shot3-1920x1080-taa.png was not captured"))
    else:
        shell = surfaces["shell"]
        center = shell["center"]
        radius = shell["radius"]
        silhouette = []
        for degrees in range(0, 360, 5):
            a = math.radians(degrees)
            for y in shell["y"]:
                silhouette.append(project(shot3["key"], shot3["look"], shot3["fov"], (center[0] + radius * math.cos(a), y, center[1] + radius * math.sin(a))))
        bottom = shot3["image"]["streak bottom"]
        top = shot3["image"]["streak top"]
        left, _, right, _ = image_bounds(silhouette)
        x0, y0, x1, y1 = pixel_rect(image, (left + 0.01, min(top[1], bottom[1]) + 0.02, right - 0.01, max(top[1], bottom[1]) - 0.02))
        columns = [(sum(luma(image, x, y) for y in range(y0, y1)) / max(1, y1 - y0), x) for x in range(x0, x1)]
        brightest = max(columns)[1] + 0.5
        expected = shot3["image"]["streak"][0] * image.width
        off = abs(brightest - expected)
        results.append(("shot 3 streak", off <= 30.0, "the brightest column of the tank at x %.0f, %.1f px from the computed %.0f (at most 30)"
                        % (brightest, off, expected)))
        noprobe = read_capture(directory, "showcase.shot3.noprobe-1920x1080-taa.png")
        if noprobe is not None:
            # The streak itself is the sun's reflection, which no probe changes: the world shows in the shell beside it.
            streak = crop_mean(image, STREAK_CROP) - crop_mean(noprobe, STREAK_CROP)
            beside = crop_mean(image, BESIDE_STREAK_CROP) - crop_mean(noprobe, BESIDE_STREAK_CROP)
            results.append(("shot 3 probe", abs(beside) >= 8.0, "the shell beside the streak differs by %.1f codes with and without "
                            "Probe.Golden.Yard (at least 8), the streak crop by %.1f" % (beside, streak)))
    held = read_capture(directory, "showcase.shot6.held-1920x1080-taa.png")
    nofire = read_capture(directory, "showcase.shot6.nofire-1920x1080-taa.png")
    if held is None or nofire is None:
        results.append(("shot 6 firelight", False, "showcase.shot6.held and .nofire were not captured at 1920 x 1080 with TAA"))
    else:
        # Both at one manual exposure: automatic exposure meters the fire's own light and darkens the frame by it, which
        # hides the light the fire adds. The crop is the tank's end that faces the fire; its body faces away.
        added = crop_mean(held, TANK_END_CROP) - crop_mean(nofire, TANK_END_CROP)
        results.append(("shot 6 firelight", added >= FIRELIGHT_CODES, "the fuel tank's end facing the fire is %.1f codes brighter with "
                        "Fire than without it, at one exposure (at least %.0f)" % (added, FIRELIGHT_CODES)))
    return results


def check_live(directory):
    """Highlight shimmer in the live shots: the second differences of 32 px block means of the encoded luma over four
    frames (a steady dolly moves the image at a constant rate, which they cancel), per shot and anti-aliasing mode."""
    results = []
    for number, (crop, rect) in sorted(LIVE_CROPS.items()):
        for mode in LIVE_MODES:
            name = "showcase.live.shot%d%s" % (number, mode)
            frames = [read_capture(directory, "%s-1920x1080-live-%d.png" % (name, k)) for k in range(1, 5)]
            if any(frame is None for frame in frames):
                results.append((name, False, "its four live frames were not captured"))
                continue
            x0, y0, x1, y1 = pixel_rect(frames[0], rect)
            blocks = [(bx, by) for by in range(y0, y1 - 31, 32) for bx in range(x0, x1 - 31, 32)]
            means = []
            for frame in frames:
                row = []
                for bx, by in blocks:
                    total = sum(luma(frame, x, y) for y in range(by, by + 32) for x in range(bx, bx + 32))
                    row.append(total / (32 * 32 * 255.0))
                means.append(row)
            differences = sorted(abs(means[k + 1][b] - 2.0 * means[k][b] + means[k - 1][b]) * 100.0
                                 for k in (1, 2) for b in range(len(blocks)))
            if not differences:
                results.append((name, False, "the %s crop holds no 32 px block" % crop))
                continue
            mean = sum(differences) / len(differences)
            p99 = differences[min(len(differences) - 1, int(math.ceil(0.99 * len(differences))) - 1)]
            passed = mean <= SHIMMER_LIMITS[0] and p99 <= SHIMMER_LIMITS[1]
            results.append((name, passed, "%s crop, %d blocks: mean %.3f%%, p99 %.3f%% (at most %.1f and %.1f)"
                            % (crop, len(blocks), mean, p99, SHIMMER_LIMITS[0], SHIMMER_LIMITS[1])))
    return results


STATIONS_ROW = re.compile(r"^\s*\{\s*([^{}]+)\},\s*$")


def read_station_rows(path):
    """The rows of c_Stations in the scripts' Stations.h: one line per station."""
    with open(path, encoding="utf-8") as file:
        text = file.read()
    start = text.find("c_Stations")
    end = text.find("} };", start)
    if start < 0 or end < 0:
        raise CheckFailed("%s has no c_Stations table" % path)
    rows = []
    for line in text[start:end].splitlines():
        match = STATIONS_ROW.match(line)
        if not match:
            continue
        values = [v.strip() for v in match.group(1).split(",")]
        if len(values) != 9:
            raise CheckFailed("%s: a c_Stations row has %d values, not 9: %s" % (path, len(values), line.strip()))
        rows.append(tuple(float(v.rstrip("f")) for v in values[:8]) + (values[8] == "true",))
    return rows


def check_station_rows(path):
    failures = []
    rows = read_station_rows(path)
    if len(rows) != len(STATIONS):
        return ["%s holds %d stations, not %d" % (path, len(rows), len(STATIONS))]
    for row, expected in zip(rows, STATIONS):
        own = expected[1:]
        if any(abs(a - b) > 1e-6 * max(1.0, abs(b)) for a, b in zip(row[:8], own[:8])) or row[8] != own[8]:
            failures.append("%s's %s row %s differs from the generator's %s" % (path, expected[0], row, own))
    return failures


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
    behind = dusk_sun_reach(world, DUSK_AZIMUTH_BEHIND_BUS)[0]
    print("rule 7 at azimuth %g: the dusk sun stands %.2f degrees beside the bus from %s" % (DUSK_AZIMUTH_BEHIND_BUS, behind[0], behind[1]))
    if behind[0] >= DUSK_SUN_BESIDE:
        failures.append("rule 7 passes the dusk sun behind the bus")
    station_failures = check_station_rows(arguments.stations)
    print("Stations.h: %s" % ("; ".join(station_failures) if station_failures else "its c_Stations rows equal the generator's"))
    failures += station_failures
    shots_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), "LookDev", "Shots.json")
    bake_failures = check_bake()
    mutations = bake_mutations()
    for what, steps in mutations:
        if not check_bake(steps):
            bake_failures.append("the check of the bake script passes %s" % what)
    print("bake script: %s" % ("; ".join(bake_failures) if bake_failures else
                               "each station's probes baked %d times where the table bakes them, %s last; %d wrong scripts caught"
                               % (BAKE_PASSES, SAVED_STATION, len(mutations))))
    failures += bake_failures
    picture_failures = check_pictures(world, shots_file)
    mutations = picture_mutations(world)
    for what, steps in mutations:
        if not check_pictures(world, shots_file, steps):
            picture_failures.append("the check of the pictures script passes %s" % what)
    print("pictures script: %s" % ("; ".join(picture_failures) if picture_failures else
                                   "each picture taken complete at its station from its LookDev shot's camera; %d wrong scripts caught" % len(mutations)))
    failures += picture_failures
    copy_failures = check_station_copies(world, arguments.render_commands, shots_file)
    print("stations' steps: %s" % ("; ".join(copy_failures) if copy_failures else
                                   "%s and %s hold them as setup writes them" % (os.path.basename(arguments.render_commands), os.path.basename(shots_file))))
    failures += copy_failures
    sample_failures = check_picture(os.path.join(arguments.sample, "Picture.png"), os.path.join(arguments.sample, "Screenshots", "Shot2.png"))
    # picture is tried on the sample's own pictures, so only once they belong together.
    sample_failures += [] if sample_failures else check_copy_picture(arguments.sample, arguments.work)
    print("Picture.png: %s" % ("; ".join(sample_failures) if sample_failures else "shows what Screenshots/Shot2.png shows; picture copies only such a thumbnail"))
    failures += sample_failures
    print("FAIL: %s" % "; ".join(failures) if failures else "PASS")
    return 1 if failures else 0


def write_steps(steps, output):
    directory = os.path.dirname(os.path.abspath(output))
    os.makedirs(directory, exist_ok=True)
    with open(output, "w", encoding="utf-8", newline="\n") as file:
        json.dump(steps, file, indent=1)
        file.write("\n")
    print("%s: %d steps" % (output, len(steps)))


def dress_script(world):
    """The dressing's command script (S6), run once on the laid-out scene: drums and tyres along the bus's sill on the
    side shot 4 sees (their outlines break the long straight edge the puddles would reflect), stones along the apron's
    edge, where the asphalt meets the dirt without a blend, and rocks on the rim framing shot 1. Seeded; everything
    stands on the surface under it, off the potholes, the pads and the shots' cameras. It saves the scene."""
    s = SceneScript()
    rng = random.Random(SEED + 400)
    layout = world.layout
    s.add("scene.open", {"scene": SCENE})
    props = s.find("Props")
    rocks = s.find("Rocks")
    report = shots(world)
    cameras = [p for shot in report for p in (shot["key"], shot["from"], shot["to"])]

    def dry(x, z):
        return all(hole.depth_at(x, z) <= 0.0 for hole in world.apron.holes)

    # The bus's -X side: three drums and a stack of two tyres, 0.3 m off its flank, between and beyond its wheels.
    flank = BUS["center"][0] - BUS["half"][0] - 0.3
    drum_count = 11  # The layout's Drum0-Drum10
    for index, (z_offset, model) in enumerate(((-4.4, "Barrel_02"), (-0.9, "barrel_03"), (1.4, "Barrel_01"))):
        low, high = SCAN_BOUNDS[model]
        radius = 0.5 * (high[0] - low[0])
        x, z = flank - radius - rng.uniform(0.0, 0.25), BUS["center"][1] + z_offset
        if not dry(x, z):
            raise CheckFailed("a drum by the bus at (%.2f, %.2f) stands in a pothole" % (x, z))
        name = "Drum%d" % (drum_count + index)
        s.instantiate(scan_path(model), name, props, {
            "Transform": transform((x, surface_at(world, x, z), z), rng.uniform(0.0, 360.0)),
            "BoxCollider": {"HalfExtents": rounded([radius * 0.9, high[1] * 0.5, radius * 0.9]), "Offset": [0.0, round(high[1] * 0.5, 4), 0.0]}})
        s.rename({"$ref": name}, name)
    low, high = SCAN_BOUNDS["old_tyre"]
    half_width = 0.5 * (high[2] - low[2])
    tyre_count = 4 + len(TYRES_SCATTERED)  # The layout's Tyre0-Tyre6
    x, z = flank - 0.35, BUS["center"][1] + 0.25
    for level in range(2):
        name = "Tyre%d" % (tyre_count + level)
        s.instantiate(scan_path("old_tyre"), name, props, {
            "Transform": transform((x, surface_at(world, x, z) + half_width * (2 * level + 1), z), rng.uniform(0.0, 360.0), pitch=90.0)})
        s.rename({"$ref": name}, name)

    # Stones in clusters of one to three just outside the apron's edge.
    instances = {}
    clusters = 0
    while clusters < DRESS_STONE_CLUSTERS:
        x, z = rng.uniform(APRON_X[0] - 3.0, APRON_X[1] + 3.0), rng.uniform(APRON_Z[0] - 3.0, APRON_Z[1] + 3.0)
        if not 0.2 <= layout.outline.signed_distance(x, z) <= 1.4:
            continue
        if any(name != "Apron" and distance(x, z) < 1.5 for name, distance, _ in layout.pads):
            continue
        if any((x - c[0]) ** 2 + (z - c[2]) ** 2 < 6.0 ** 2 for c in cameras):
            continue
        clusters += 1
        for _ in range(rng.randint(1, 3)):
            sx, sz = x + rng.uniform(-0.8, 0.8), z + rng.uniform(-0.8, 0.8)
            model = rng.choice(("namaqualand_boulder_02", "namaqualand_boulder_03"))
            scale = rng.uniform(0.15, 0.4)
            low, high = SCAN_BOUNDS[model]
            y = world.ground.surface(sx, sz) - 0.3 * (high[1] - low[1]) * scale
            instances.setdefault(model, []).append({"parent": rocks, "components": {
                "Transform": transform((sx, y, sz), rng.uniform(0.0, 360.0), scale), "BoxCollider": box_collider(SCAN_BOUNDS[model])}})

    # Rocks on the rim in shot 1's lower left, around its hero boulder, framing the vista (the lower right shows the hut).
    shot1 = report[0]
    for ix, iy, model, scale in DRESS_RIM_ROCKS:
        x, _, z = ground_at_image(world, shot1["key"], shot1["look"], shot1["fov"], ix, iy)
        if any((x - c[0]) ** 2 + (z - c[2]) ** 2 < 2.5 ** 2 for c in cameras):
            raise CheckFailed("a rim rock at (%.2f, %.2f) stands within 2.5 m of a shot's camera" % (x, z))
        low, high = SCAN_BOUNDS[model]
        y = world.ground.surface(x, z) - 0.2 * (high[1] - low[1]) * scale
        instances.setdefault(model, []).append({"parent": rocks, "components": {
            "Transform": transform((x, y, z), rng.uniform(0.0, 360.0), scale), "BoxCollider": box_collider(SCAN_BOUNDS[model])}})
    for model, model_instances in sorted(instances.items()):
        s.add("prefab.instantiate", {"prefab": scan_path(model), "instances": model_instances})
    s.add("scene.save")
    return s.steps


def command_layout(arguments):
    write_steps(layout_script(World()), arguments.output)
    return 0


def command_dress(arguments):
    write_steps(dress_script(World()), arguments.output)
    return 0


def command_controls(arguments):
    write_steps(controls_script(), arguments.output)
    return 0


def command_bake(arguments):
    write_steps(bake_script(), arguments.output)
    return 0


def command_pictures(arguments):
    write_steps(pictures_script(World()), arguments.output)
    return 0


def command_picture(arguments):
    failures = copy_picture(arguments.project)
    if failures:
        print("FAIL: %s; nothing copied" % "; ".join(failures))
        return 1
    print("%s: the project's thumbnail" % os.path.join(arguments.project, "Picture.png"))
    return 0


def command_setup(arguments):
    steps = station_steps(arguments.station, arguments.format == "script")
    if arguments.output:
        write_steps(steps, arguments.output)
    else:
        print(json.dumps(steps, separators=(", ", ": ")))
    return 0


def command_checkshots(arguments):
    results = check_live(arguments.capture) if arguments.live else check_stills(World(), arguments.capture)
    for name, passed, text in results:
        print("%s %s: %s" % ("PASS" if passed else "FAIL", name, text))
    failed = [name for name, passed, _ in results if not passed]
    print("FAIL: %s" % ", ".join(failed) if failed else "PASS")
    return 1 if failed else 0


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
    selftest.add_argument("--stations", required=True, help="The scripts' Stations.h, whose c_Stations rows must equal the table")
    selftest.add_argument("--sample", required=True, help="The sample's directory, whose Picture.png must show what its Screenshots/Shot2.png shows")
    selftest.add_argument("--render-commands", required=True, help="The render test's command script, whose stations' steps must be setup's")
    layout_parser = commands.add_parser("layout", help="Write the command script that builds the scene's entities")
    layout_parser.add_argument("--output", required=True, help="The command script to write (.json)")
    controls = commands.add_parser("controls", help="Write the command script of the input actions, the photo mode and the loading scene")
    controls.add_argument("--output", required=True, help="The command script to write (.json)")
    dress = commands.add_parser("dress", help="Write the command script of the dressing: drums and tyres by the bus, stones on the apron's edge")
    dress.add_argument("--output", required=True, help="The command script to write (.json)")
    bake = commands.add_parser("bake", help="Write the command script that bakes the six probes")
    bake.add_argument("--output", required=True, help="The command script to write (.json)")
    pictures = commands.add_parser("pictures", help="Write the command script that retakes the screenshots, the icon and the thumbnail")
    pictures.add_argument("--output", required=True, help="The command script to write (.json)")
    picture = commands.add_parser("picture", help="Copy the project's thumbnail to its Picture.png, once it is the one the pictures script left")
    picture.add_argument("--project", required=True, help="The sample's directory (holding .strata/Thumbnail.png)")
    setup = commands.add_parser("setup", help="Print a station's edit-mode steps")
    setup.add_argument("--station", required=True, choices=[row[0] for row in STATIONS])
    setup.add_argument("--format", required=True, choices=("lookdev", "script"), help="LookDev Setup entries or command steps")
    setup.add_argument("--output", help="Write them to this file instead of printing them")
    checkshots = commands.add_parser("checkshots", help="Check a LookDev capture of the showcase's shots")
    checkshots.add_argument("--capture", required=True, help="The LookDev output directory (holding shots/)")
    checkshots.add_argument("--live", action="store_true", help="Measure the live shots' highlight shimmer instead")
    arguments = parser.parse_args()
    try:
        return {"terrain": command_terrain, "check": command_check, "stations": command_stations,
                "selftest": command_selftest, "layout": command_layout, "controls": command_controls, "dress": command_dress,
                "bake": command_bake, "pictures": command_pictures, "picture": command_picture, "setup": command_setup,
                "checkshots": command_checkshots}[arguments.command](arguments)
    except CheckFailed as error:
        print("FAIL: %s" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
