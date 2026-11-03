#!/usr/bin/env python3
"""Generates the Showcase sample's props (High Desert Outpost), its bare steel's maps and the editor command scripts that
make its materials and prefabs.

The props and maps are committed and a CTest (StrataTools.ShowcaseProps) keeps them equal to what this script writes:

    python3 -I Tools/GenerateShowcaseProps.py props     --output Samples/Showcase/Assets/Generated/Props
    python3 -I Tools/GenerateShowcaseProps.py materials --output <Materials.json>
    python3 -I Tools/GenerateShowcaseProps.py tune      --output <Tune.json>
    python3 -I Tools/GenerateShowcaseProps.py prefabs   --output <Prefabs.json>
    python3 -I Tools/GenerateShowcaseProps.py selftest  --committed Samples/Showcase/Assets/Generated/Props --work <dir>

Standard library only, seeded by SEED, every vertex attribute quantized as GenerateShowcaseTerrain.py's (which provides
the glTF writer); circles use math.cos and math.sin, whose last-bit differences the quantization absorbs. Meters, +Y up,
each model's origin at its footprint's center on the ground, texture coordinates in meters (a material's UVTiling is 1 /
its texture's real size), no tangents (the importer computes them). Every node is one material slot, named
<Prop>.<Slot> and given a factor-only glTF material of the slot's name (MeshRenderer's Material overrides every submesh,
so a slot is a node); <Prop>.Collision nodes are convex hulls of at most 32 vertices that the prefab script turns into
convex mesh colliders.

props      writes Bus.glb, OilTank.glb, FuelTank.glb, Pipes.glb, Hut.glb, LeanTo.glb, Embers.glb and Wick.glb, and the bare
           steel's three maps from one tileable field (1024 x 1024 over 4 m): WeatheredSteel_diff.png (sRGB: steel mixed
           toward rust by the rust mask), WeatheredSteel_arm.png (linear: R 255, G roughness, B metalness, 0 on rust) and
           WeatheredSteel_nor_gl.png (the sheet's waves and dents, and pits on rust).
materials  writes the command script that creates the sample's materials (Assets/Materials/*.stmat) from the scans and
           the steel's maps (material.create).
tune       writes the command script that gives the existing materials the table's values (material.set): how the
           sample follows a change of the table.
prefabs    writes the command script that turns the models into prefabs (Assets/Prefabs/*.stprefab): each slot's
           material, convex colliders from the hulls, the bus's tyres, the tank's pipes and ladder, the hut's lean-to, the
           fire pit's embers and light, the lantern's wick and light. Run both in an editor with the project open, e.g.
           StrataEditor --no-gpu --no-automation --quit-after-commands --project <dir> --commands <script>.
selftest   props twice (byte-equal), each model byte-equal to the committed one and the maps equal by their pixels, the
           bus's letterboard vertical, every node present with its triangle count in range, every hull convex with at
           most 32 vertices. Exit codes: 0 done, 1 a check failed, 2 usage.
"""

import argparse
import json
import math
import os
import random
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import GenerateShowcaseTerrain as terrain  # noqa: E402  (the glTF writer, the noise and the layout's anchors)

SEED = 20261102
SMOOTH_ANGLE = 35.0  # Degrees: faces meeting at a sharper angle keep a crease

# The bus (D7): 10.8 m long (+Z is its front), 2.44 m wide; the profile's heights.
BUS_HALF_LENGTH = 5.4
BUS_HALF_WIDTH = 1.22
BUS_CORNER = 0.18
BUS_FLOOR = 0.62
BUS_BAND = (1.52, 2.25)          # The window band, inset 3 cm
BUS_BAND_INSET = 0.03
BUS_WINDOWS = (1.62, 2.15)       # The openings in the band (boarded)
BUS_LETTERBOARD = (2.25, 2.62)   # Vertical and flush with the lower panel: shot 2's highlight runs on it
BUS_ROOF_RADIUS = 0.33           # The roof's edge arc, up to 2.95
BUS_CROWN = 0.04                 # The roof's crown above the arc's top (2.99)
BUS_ARCHES = (3.2, -2.6)
BUS_ARCH_RADIUS = 0.56
BUS_ARCH_HEIGHT = 0.5
BUS_SEAM_PITCH = 1.15
BUS_GROOVE = (0.004, 0.003)      # Half width, depth
BUS_BARE_LETTERBOARD = ((-1.45, -0.30), (0.85, 2.0))
BUS_BARE_LOWER = (-1.45, -0.30)
BUS_BARE_ROOF = (-3.0, -1.85)
BUS_RAILS = (0.95, 1.30, 1.52)
BUS_DOOR = (3.75, 4.65)          # On the -X side (the far side from shot 2)
BUS_WHEEL_RADIUS = 0.3 * 1.65    # old_tyre at scale 1.65
BUS_FLAT_SQUASH = 0.85           # Flat tyres: their height scaled
# The wheels: (x, z, flat). The front pair is flat; the rear axle has twin wheels.
BUS_WHEELS = [(1.0, BUS_ARCHES[0], True), (-1.0, BUS_ARCHES[0], True)] + [(s * x, BUS_ARCHES[1], False) for s in (1.0, -1.0) for x in (1.0, 0.70)]

OIL_TANK_RADIUS = 4.0
OIL_TANK_PAD = (4.35, 0.3)
OIL_TANK_COURSES = 6
OIL_TANK_COURSE = 1.2
OIL_TANK_ROOF_RISE = 0.33
OIL_TANK_NOZZLE = (200.0, 0.5)   # Azimuth (degrees) and height of the pipe's nozzle

FUEL_TANK_RADIUS = 1.3
FUEL_TANK_HALF_LENGTH = 3.2
FUEL_TANK_HEAD = 0.65
FUEL_TANK_AXIS = 1.75

STEEL_SIZE = 1024
STEEL_TILE_METERS = 4.0
# The bare steel's look (linear values): its color, darkened by shade = a - b * f along the rain streaks; its roughness
# a + b * f on steel; rust's color and roughness. Weathered steel is darker than clean iron (0.56), and its clean patches
# are smooth while the streaks are dull: that spread, not one roughness, is what keeps an 8 m tank from reading as foil.
# At 0.42 the letterboard's bare sections, which mirror the bright sky beside the low sun in shot 2, clipped as flat
# white panels (a quarter of their pixels at 4K); at 0.34, with the stronger waves below, an eighth.
STEEL_COLOR = (0.34, 0.34, 0.35)
STEEL_SHADE = (1.10, 0.30)
STEEL_ROUGHNESS = (0.16, 0.38)
RUST_COLOR = (0.20, 0.09, 0.04)
RUST_ROUGHNESS = 0.85
# The steel's relief (meters of height, lattice cells over the 4 m tile): oil-canning waves of 0.36-0.8 m, dents of
# 6.6 cm, and pits of 1.7 cm on rust only. The waves tilt the sheet by up to about 4 degrees: at half that, the glare
# of the sky beside the low sun lay on a panel as one even sheet. Only the waves are that strong: they are low
# frequencies, so they cannot shimmer, while the dents stay shallow.
STEEL_WAVES = ((0.024, 5), (0.016, 7), (0.008, 11))
STEEL_DENTS = (0.0015, 61)
STEEL_PITS = (0.0008, 229)

# Slots and their factor-only glTF materials (color, roughness, metallic); the .stmat files replace them.
SLOT_FACTORS = {
    "BusPaint": ((0.55, 0.42, 0.10), 0.45, 0.0),
    "WeatheredSteel": ((0.56, 0.56, 0.57), 0.35, 1.0),
    "Planks": ((0.30, 0.22, 0.14), 1.0, 0.0),
    "Grime": ((0.035, 0.032, 0.030), 0.85, 0.0),
    "TankPaint": ((0.10, 0.22, 0.12), 0.70, 0.0),
    "Concrete": ((0.55, 0.55, 0.55), 1.0, 0.0),
    "Plaster": ((0.65, 0.58, 0.47), 1.0, 0.0),
    "CorrugatedRed": ((0.40, 0.13, 0.08), 1.0, 0.0),
    "CorrugatedRust": ((0.30, 0.17, 0.10), 1.0, 0.0),
    "Embers": ((0.05, 0.04, 0.035), 0.9, 0.0),
    "Collision": ((1.0, 0.0, 1.0), 1.0, 0.0),
}

# Every model's nodes: (node, slot, triangle range). The selftest holds the generated models to it.
MODELS = {
    "Bus": [("Bus.Body", "BusPaint", (12000, 30000)), ("Bus.BarePanels", "WeatheredSteel", (300, 4000)),
            ("Bus.Rails", "WeatheredSteel", (100, 2000)), ("Bus.Bumpers", "WeatheredSteel", (12, 200)),
            ("Bus.Boards", "Planks", (200, 3000)), ("Bus.Chassis", "Grime", (12, 2000)), ("Bus.Collision", "Collision", (12, 12))],
    "OilTank": [("OilTank.Shell", "WeatheredSteel", (7000, 12000)), ("OilTank.Roof", "WeatheredSteel", (400, 2000)),
                ("OilTank.Fittings", "WeatheredSteel", (8000, 20000)), ("OilTank.Pad", "Concrete", (200, 600)),
                ("OilTank.Collision", "Collision", (60, 60))],
    "FuelTank": [("FuelTank.Shell", "TankPaint", (2500, 6000)), ("FuelTank.Saddles", "WeatheredSteel", (100, 1500)),
                 ("FuelTank.Fittings", "WeatheredSteel", (2000, 6000)), ("FuelTank.Collision", "Collision", (44, 44))],
    "Pipes": [("Pipes.Pipe", "WeatheredSteel", (800, 8000)), ("Pipes.Supports", "BusPaint", (50, 1000))],
    "Hut": [("Hut.Walls", "Plaster", (60, 400)), ("Hut.Floor", "Planks", (12, 12)), ("Hut.Door", "Planks", (12, 12)),
            ("Hut.Shutters", "Planks", (24, 24)), ("Hut.Rafters", "Planks", (60, 200)), ("Hut.Furniture", "Planks", (60, 200)),
            ("Hut.Lintels", "Planks", (24, 24)), ("Hut.Roof", "CorrugatedRed", (12, 12))]
           + [("Hut.Collision.%d" % k, "Collision", (12, 12)) for k in range(1, 7)],
    "LeanTo": [("LeanTo.Posts", "Planks", (48, 48)), ("LeanTo.Roof", "CorrugatedRust", (12, 12))]
              + [("LeanTo.Collision.%d" % k, "Collision", (12, 12)) for k in range(1, 5)],
    "Embers": [("Embers.Embers", "Embers", (500, 3000))],
    "Wick": [("Wick.Wick", "Embers", (40, 400))],
}


# ---------------------------------------------------------------------------------------------------------------------
# Vectors

def add(a, b, s=1.0):
    return (a[0] + b[0] * s, a[1] + b[1] * s, a[2] + b[2] * s)


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def scale(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def normalize(a):
    length = math.sqrt(dot(a, a))
    return (a[0] / length, a[1] / length, a[2] / length)


def combine(origin, axes, local):
    """origin + local.x * axes[0] + local.y * axes[1] + local.z * axes[2]."""
    return (origin[0] + local[0] * axes[0][0] + local[1] * axes[1][0] + local[2] * axes[2][0],
            origin[1] + local[0] * axes[0][1] + local[1] * axes[1][1] + local[2] * axes[2][1],
            origin[2] + local[0] * axes[0][2] + local[1] * axes[1][2] + local[2] * axes[2][2])


WORLD_AXES = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))


def yaw_axes(degrees):
    """The axes of a frame turned by a yaw about +Y (local +Z toward (sin, 0, cos))."""
    a = math.radians(degrees)
    c, s = math.cos(a), math.sin(a)
    return ((c, 0.0, -s), (0.0, 1.0, 0.0), (s, 0.0, c))


def frame_from(axis):
    """Two unit vectors perpendicular to `axis` (and each other), with the first horizontal when it can be."""
    axis = normalize(axis)
    helper = (0.0, 1.0, 0.0) if abs(axis[1]) < 0.9 else (1.0, 0.0, 0.0)
    u = normalize(cross(helper, axis))
    v = cross(axis, u)
    return u, v


# ---------------------------------------------------------------------------------------------------------------------
# Building meshes

class Part:
    """The triangles of one node. Corners carry positions and texture coordinates; normals come from smoothing the faces
    that meet within SMOOTH_ANGLE (sharper edges keep a crease)."""

    def __init__(self):
        self.triangles = []

    def triangle(self, a, b, c, ua, ub, uc, outward=None):
        n = cross(sub(b, a), sub(c, a))
        if dot(n, n) < 1e-14:
            return
        if outward is not None and dot(n, outward) < 0.0:
            b, c, ub, uc = c, b, uc, ub
        self.triangles.append((a, b, c, ua, ub, uc))

    def quad(self, a, b, c, d, ua, ub, uc, ud, outward=None):
        self.triangle(a, b, c, ua, ub, uc, outward)
        self.triangle(a, c, d, ua, uc, ud, outward)

    def mesh(self):
        threshold = math.cos(math.radians(SMOOTH_ANGLE))
        faces = []
        corners = {}
        for index, (a, b, c, _, _, _) in enumerate(self.triangles):
            n = cross(sub(b, a), sub(c, a))
            unit = normalize(n)
            faces.append((n, unit))
            for p in (a, b, c):
                corners.setdefault(position_key(p), []).append(index)
        mesh = terrain.MeshData()
        for index, triangle in enumerate(self.triangles):
            unit = faces[index][1]
            ids = []
            for k in range(3):
                p = triangle[k]
                total = (0.0, 0.0, 0.0)
                for other in corners[position_key(p)]:
                    if dot(faces[other][1], unit) >= threshold:
                        total = add(total, faces[other][0])
                n = normalize(total)
                uv = triangle[3 + k]
                key = (position_key(p), tuple(int(round(c * 127.0)) for c in n), int(round(uv[0] * 65536)), int(round(uv[1] * 65536)))
                ids.append(mesh.add(p, n, uv, key=key))
            mesh.indices += ids
        return mesh


def position_key(p):
    return (int(round(p[0] * 4096)), int(round(p[1] * 4096)), int(round(p[2] * 4096)))


def planar_uv(p, n):
    """Texture coordinates in meters, projected along the face's dominant axis."""
    ax, ay, az = abs(n[0]), abs(n[1]), abs(n[2])
    if ay >= ax and ay >= az:
        return (p[0], p[2])
    if ax >= az:
        return (p[2], p[1])
    return (p[0], p[1])


def box(part, center, half, axes=WORLD_AXES):
    """A box: its six faces, outward, planar texture coordinates."""
    for axis in range(3):
        for sign in (-1.0, 1.0):
            n = scale(axes[axis], sign)
            u_axis = (axis + 1) % 3
            v_axis = (axis + 2) % 3
            pts = []
            for su, sv in ((-1.0, -1.0), (1.0, -1.0), (1.0, 1.0), (-1.0, 1.0)):
                local = [0.0, 0.0, 0.0]
                local[axis] = sign * half[axis]
                local[u_axis] = su * half[u_axis]
                local[v_axis] = sv * half[v_axis]
                pts.append(combine(center, axes, local))
            uvs = [planar_uv(p, n) for p in pts]
            part.quad(pts[0], pts[1], pts[2], pts[3], uvs[0], uvs[1], uvs[2], uvs[3], n)


def cylinder(part, base, axis, radius, length, segments, caps=(True, True), u_offset=0.0, rows=1):
    """A cylinder from `base` along `axis`: its side (u along the circumference, v along the axis) and caps."""
    axis = normalize(axis)
    eu, ev = frame_from(axis)
    rings = []
    for r in range(rows + 1):
        t = length * r / rows
        ring = []
        for k in range(segments + 1):
            a = 2.0 * math.pi * k / segments
            d = add(scale(eu, math.cos(a)), ev, math.sin(a))
            ring.append((add(add(base, axis, t), d, radius), d, (u_offset + radius * a, t)))
        rings.append(ring)
    for r in range(rows):
        for k in range(segments):
            a0, a1 = rings[r][k], rings[r][k + 1]
            b0, b1 = rings[r + 1][k], rings[r + 1][k + 1]
            outward = add(a0[1], a1[1])
            part.quad(a0[0], a1[0], b1[0], b0[0], a0[2], a1[2], b1[2], b0[2], outward)
    for end, (enabled, ring, direction) in enumerate(((caps[0], rings[0], scale(axis, -1.0)), (caps[1], rings[-1], axis))):
        if not enabled:
            continue
        center = add(base, axis, length * end)
        cuv = planar_uv(center, direction)
        for k in range(segments):
            part.triangle(center, ring[k][0], ring[k + 1][0], cuv, planar_uv(ring[k][0], direction), planar_uv(ring[k + 1][0], direction), direction)


def revolve(part, center, profile, segments, u_scale=1.0, v_offset=0.0, start=0.0, sweep=2.0 * math.pi):
    """A surface of revolution about the vertical axis through `center`: profile is [(radius, height)] from the axis;
    faces point away from the profile's left (the outside of a profile going up)."""
    rings = []
    s = v_offset
    for k, (r, y) in enumerate(profile):
        if k:
            s += math.sqrt((r - profile[k - 1][0]) ** 2 + (y - profile[k - 1][1]) ** 2)
        ring = []
        for j in range(segments + 1):
            a = start + sweep * j / segments
            ring.append(((center[0] + r * math.cos(a), center[1] + y, center[2] + r * math.sin(a)), (a * u_scale, s)))
        rings.append(ring)
    for k in range(len(profile) - 1):
        r0, y0 = profile[k]
        r1, y1 = profile[k + 1]
        # The profile's outward side: its tangent turned by -90 degrees in the (radius, height) plane.
        tr, ty = r1 - r0, y1 - y0
        for j in range(segments):
            a = start + sweep * (j + 0.5) / segments
            outward = (ty * math.cos(a), -tr, ty * math.sin(a))
            p = rings[k][j]
            q = rings[k][j + 1]
            p1 = rings[k + 1][j]
            q1 = rings[k + 1][j + 1]
            part.quad(p[0], q[0], q1[0], p1[0], p[1], q[1], q1[1], p1[1], outward if (tr * tr + ty * ty) > 0.0 else None)


def sweep_tube(part, points, radius, segments, closed_ends=True):
    """A tube along a polyline (already filleted): rings perpendicular to the path's tangents."""
    tangents = []
    for i in range(len(points)):
        a = points[max(i - 1, 0)]
        b = points[min(i + 1, len(points) - 1)]
        tangents.append(normalize(sub(b, a)))
    eu, ev = frame_from(tangents[0])
    rings = []
    length = 0.0
    for i, (p, t) in enumerate(zip(points, tangents)):
        if i:
            length += math.sqrt(dot(sub(p, points[i - 1]), sub(p, points[i - 1])))
            # Parallel transport of the frame (no twist along the bends).
            eu = normalize(sub(eu, scale(t, dot(eu, t))))
            ev = cross(t, eu)
        ring = []
        for k in range(segments + 1):
            a = 2.0 * math.pi * k / segments
            d = add(scale(eu, math.cos(a)), ev, math.sin(a))
            ring.append((add(p, d, radius), d, (radius * a, length)))
        rings.append(ring)
    for i in range(len(points) - 1):
        for k in range(segments):
            a0, a1 = rings[i][k], rings[i][k + 1]
            b0, b1 = rings[i + 1][k], rings[i + 1][k + 1]
            part.quad(a0[0], a1[0], b1[0], b0[0], a0[2], a1[2], b1[2], b0[2], add(a0[1], b1[1]))
    if closed_ends:
        for ring, p, direction in ((rings[0], points[0], scale(tangents[0], -1.0)), (rings[-1], points[-1], tangents[-1])):
            for k in range(segments):
                part.triangle(p, ring[k][0], ring[k + 1][0], (0.0, 0.0), ring[k][2], ring[k + 1][2], direction)


def fillet_path(points, radius, steps=6):
    """A polyline with its corners rounded by arcs of `radius` (elbows)."""
    out = [points[0]]
    for i in range(1, len(points) - 1):
        a, b, c = points[i - 1], points[i], points[i + 1]
        d0 = normalize(sub(b, a))
        d1 = normalize(sub(c, b))
        angle = math.acos(max(-1.0, min(1.0, dot(d0, d1))))
        if angle < 1e-4:
            out.append(b)
            continue
        cut = radius * math.tan(angle * 0.5)
        start = add(b, d0, -cut)
        end = add(b, d1, cut)
        bisector = normalize(sub(d1, d0))
        center = add(b, bisector, radius / math.cos(angle * 0.5))
        r0 = sub(start, center)
        r1 = sub(end, center)
        for k in range(steps + 1):
            t = k / steps
            # Spherical interpolation of the two radii (both of length `radius`).
            s0 = math.sin((1.0 - t) * angle) / math.sin(angle)
            s1 = math.sin(t * angle) / math.sin(angle)
            out.append(add(center, add(scale(r0, s0), r1, s1)))
    out.append(points[-1])
    return out


def prism(part, outline, origin, u_axis, v_axis, thickness, center=None):
    """A slab: a polygon star-shaped from `center` (default: its vertices' mean) in the plane (u, v) through origin,
    extruded `thickness` along u x v."""
    n = normalize(cross(u_axis, v_axis))
    count = len(outline)
    cu, cv = center if center else (sum(p[0] for p in outline) / count, sum(p[1] for p in outline) / count)
    for side, offset, direction in ((0, 0.0, scale(n, -1.0)), (1, thickness, n)):
        center = add(combine(origin, (u_axis, v_axis, n), (cu, cv, 0.0)), n, offset)
        for k in range(count):
            a = add(combine(origin, (u_axis, v_axis, n), (outline[k][0], outline[k][1], 0.0)), n, offset)
            b = add(combine(origin, (u_axis, v_axis, n), (outline[(k + 1) % count][0], outline[(k + 1) % count][1], 0.0)), n, offset)
            part.triangle(center, a, b, (cu, cv), outline[k], outline[(k + 1) % count], direction)
    for k in range(count):
        a2 = outline[k]
        b2 = outline[(k + 1) % count]
        a = combine(origin, (u_axis, v_axis, n), (a2[0], a2[1], 0.0))
        b = combine(origin, (u_axis, v_axis, n), (b2[0], b2[1], 0.0))
        mid = ((a2[0] + b2[0]) * 0.5 - cu, (a2[1] + b2[1]) * 0.5 - cv)
        outward = add(scale(u_axis, mid[0]), v_axis, mid[1])
        length = math.sqrt((b2[0] - a2[0]) ** 2 + (b2[1] - a2[1]) ** 2)
        part.quad(a, b, add(b, n, thickness), add(a, n, thickness), (0.0, 0.0), (length, 0.0), (length, thickness), (0.0, thickness), outward)


class Hull:
    """A convex collision hull: shared vertices (no creases), at most 32."""

    def __init__(self, points, triangles):
        self.points = points
        self.triangles = triangles

    def mesh(self):
        mesh = terrain.MeshData()
        center = scale(tuple(sum(p[k] for p in self.points) for k in range(3)), 1.0 / len(self.points))
        ids = [mesh.add(p, normalize(sub(p, center)), (p[0], p[2]), key=position_key(p)) for p in self.points]
        for a, b, c in self.triangles:
            n = cross(sub(self.points[b], self.points[a]), sub(self.points[c], self.points[a]))
            if dot(n, sub(self.points[a], center)) < 0.0:
                b, c = c, b
            mesh.indices += [ids[a], ids[b], ids[c]]
        return mesh


def box_hull(center, half, axes=WORLD_AXES):
    points = [combine(center, axes, (sx * half[0], sy * half[1], sz * half[2])) for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
    # Index = 4 * x + 2 * y + z (0 for -, 1 for +).
    faces = [(0, 1, 3), (0, 3, 2), (4, 6, 7), (4, 7, 5), (0, 4, 5), (0, 5, 1), (2, 3, 7), (2, 7, 6), (0, 2, 6), (0, 6, 4), (1, 5, 7), (1, 7, 3)]
    return Hull(points, faces)


def prism_hull(center, axis_u, axis_v, axis, radius, low, high, sides):
    """An n-gon prism: its axis from center + low * axis to center + high * axis."""
    points = []
    for end in (low, high):
        for k in range(sides):
            a = 2.0 * math.pi * k / sides
            points.append(add(add(center, axis, end), add(scale(axis_u, math.cos(a) * radius), axis_v, math.sin(a) * radius)))
    faces = []
    for k in range(sides):
        n = (k + 1) % sides
        faces += [(k, n, sides + n), (k, sides + n, sides + k)]
    for k in range(1, sides - 1):
        faces += [(0, k + 1, k), (sides, sides + k, sides + k + 1)]
    return Hull(points, faces)


# ---------------------------------------------------------------------------------------------------------------------
# The bus

def bus_columns():
    """The side's columns (z) and the ends' columns (x): every break the side's features need, at most 0.2 m apart."""
    end = BUS_HALF_LENGTH - BUS_CORNER
    zs = set()
    steps = int(math.ceil(2 * end / 0.2))
    for k in range(steps + 1):
        zs.add(-end + 2 * end * k / steps)
    for groove in bus_grooves():
        for dz in (-BUS_GROOVE[0], 0.0, BUS_GROOVE[0]):
            zs.add(groove[0] + dz)
    for center in BUS_ARCHES:
        for k in range(29):
            zs.add(center - BUS_ARCH_RADIUS - 0.02 + (2 * BUS_ARCH_RADIUS + 0.04) * k / 28)
    for lo, hi in bus_windows():
        zs.update((lo, hi))
    for lo, hi in BUS_BARE_LETTERBOARD + (BUS_BARE_LOWER, BUS_BARE_ROOF):
        zs.update((lo, hi))
    zs.update(BUS_DOOR)
    zs = sorted(round(z, 4) for z in zs if -end - 1e-9 <= z <= end + 1e-9)
    merged = []
    for z in zs:
        if merged and z - merged[-1] < 0.003:
            continue
        merged.append(z)
    half = BUS_HALF_WIDTH - BUS_CORNER
    xs = set(-half + 2 * half * k / 10 for k in range(11))
    xs.update((-0.95, 0.95, -0.40, 0.40))
    for x in (-0.45, 0.45):
        for dx in (-BUS_GROOVE[0], 0.0, BUS_GROOVE[0]):
            xs.add(x + dx)
    return merged, sorted(round(x, 4) for x in xs)


def bus_grooves():
    """(z, low y, high y) of the side's vertical seams: every 1.15 m on the lower panel, and at the bare sections' ends."""
    grooves = []
    start = -(BUS_HALF_LENGTH - BUS_CORNER)
    for k in range(1, 10):
        grooves.append((round(start + BUS_SEAM_PITCH * k, 4), BUS_FLOOR, BUS_BAND[0]))
    for lo, hi in BUS_BARE_LETTERBOARD:
        grooves += [(lo, BUS_LETTERBOARD[0], BUS_LETTERBOARD[1]), (hi, BUS_LETTERBOARD[0], BUS_LETTERBOARD[1])]
    grooves += [(BUS_BARE_LOWER[0], BUS_FLOOR, BUS_BAND[0]), (BUS_BARE_LOWER[1], BUS_FLOOR, BUS_BAND[0])]
    return grooves


def bus_windows():
    windows = []
    z = -4.9
    while z + 0.95 <= 4.2:
        windows.append((round(z, 4), round(z + 0.95, 4)))
        z += BUS_SEAM_PITCH
    windows.append((4.35, 5.0))
    return windows


def bus_profile():
    """The rings of the body's loft: (y, inset of the width, inset of the length)."""
    rings = [(y, 0.0, 0.0) for y in (0.62, 0.70, 0.78, 0.86, 0.94, 1.02, 1.10, 1.30, 1.52)]
    rings += [(BUS_BAND[0], BUS_BAND_INSET, BUS_BAND_INSET), (BUS_WINDOWS[0], BUS_BAND_INSET, BUS_BAND_INSET),
              (BUS_WINDOWS[1], BUS_BAND_INSET, BUS_BAND_INSET), (BUS_BAND[1], BUS_BAND_INSET, BUS_BAND_INSET)]
    rings += [(BUS_LETTERBOARD[0], 0.0, 0.0), (BUS_LETTERBOARD[1], 0.0, 0.0)]
    r = BUS_ROOF_RADIUS
    for k in range(1, 7):
        a = math.radians(15.0 * k)
        rings.append((BUS_LETTERBOARD[1] + r * math.sin(a), r * (1.0 - math.cos(a)), r * (1.0 - math.cos(a))))
    top = BUS_LETTERBOARD[1] + r
    half = BUS_HALF_WIDTH - r
    for k in range(1, 5):
        hx = half * (1.0 - k / 4.0)
        rings.append((top + BUS_CROWN * (1.0 - (hx / half) ** 2), BUS_HALF_WIDTH - hx, r))
    return rings


def build_bus():
    rng = random.Random(SEED + 1)
    columns, end_columns = bus_columns()
    grooves = bus_grooves()
    profile = bus_profile()
    panel_offsets = [0.37 * rng.randrange(0, 11) for _ in range(12)]
    body, bare, rails, bumpers, boards, chassis = Part(), Part(), Part(), Part(), Part(), Part()

    # Ring points: (x, z, segment, column) in order around the body (seen from above, +X side first, toward the front).
    def ring_points(hx, hz, rc):
        points = []
        side = hz - rc
        for z in columns:
            points.append((hx, max(-side, min(side, z)), "+x", z))
        for k in range(1, 4):
            a = math.radians(22.5 * k)
            points.append((hx - rc + rc * math.cos(a), hz - rc + rc * math.sin(a), "corner", None))
        ends = hx - rc
        for x in reversed(end_columns):
            points.append((max(-ends, min(ends, x)), hz, "front", x))
        for k in range(1, 4):
            a = math.radians(90.0 + 22.5 * k)
            points.append((-(hx - rc) + rc * math.cos(a), hz - rc + rc * math.sin(a), "corner", None))
        for z in reversed(columns):
            points.append((-hx, max(-side, min(side, z)), "-x", z))
        for k in range(1, 4):
            a = math.radians(180.0 + 22.5 * k)
            points.append((-(hx - rc) + rc * math.cos(a), -(hz - rc) + rc * math.sin(a), "corner", None))
        for x in end_columns:
            points.append((max(-ends, min(ends, x)), -hz, "rear", x))
        for k in range(1, 4):
            a = math.radians(270.0 + 22.5 * k)
            points.append((hx - rc + rc * math.cos(a), -(hz - rc) + rc * math.sin(a), "corner", None))
        return points

    def in_arch(z, y):
        return any((z - c) ** 2 + (y - BUS_ARCH_HEIGHT) ** 2 < BUS_ARCH_RADIUS ** 2 for c in BUS_ARCHES)

    rings = []
    lengths = []
    s = BUS_FLOOR
    for k, (y, inset_x, inset_z) in enumerate(profile):
        if k:
            py, pix, _ = profile[k - 1]
            s += math.sqrt((y - py) ** 2 + (inset_x - pix) ** 2)
        lengths.append(s)
        hx = BUS_HALF_WIDTH - inset_x
        hz = BUS_HALF_LENGTH - inset_z
        rc = max(BUS_CORNER - inset_x, 0.0)
        ring = []
        for x, z, segment, column in ring_points(hx, hz, rc):
            px, py2, pz = x, y, z
            if segment in ("+x", "-x") and inset_x == 0.0:
                if py2 <= BUS_BAND[0] + 1e-9 and in_arch(pz, py2):
                    # Onto the arch's circle, out from its center.
                    center = min(BUS_ARCHES, key=lambda c: abs(c - pz))
                    dz = pz - center
                    dy = py2 - BUS_ARCH_HEIGHT
                    d = math.sqrt(dz * dz + dy * dy)
                    pz = center + dz * BUS_ARCH_RADIUS / d
                    py2 = BUS_ARCH_HEIGHT + dy * BUS_ARCH_RADIUS / d
                for gz, lo, hi in grooves:
                    if column is not None and abs(column - gz) < 1e-6 and lo - 1e-9 <= y <= hi + 1e-9:
                        px -= math.copysign(BUS_GROOVE[1], px)
            if segment == "rear" and inset_x == 0.0 and y <= BUS_BAND[0] + 1e-9 and column is not None and abs(abs(column) - 0.45) < 1e-6:
                pz += BUS_GROOVE[1]
            ring.append(((px, py2, pz), segment, column))
        rings.append(ring)

    # Which way the ring's quads face: a quad of the +X side must face +X.
    def cell(k, j):
        a = rings[k][j][0]
        b = rings[k][(j + 1) % len(rings[k])][0]
        c = rings[k + 1][(j + 1) % len(rings[k])][0]
        d = rings[k + 1][j][0]
        return a, b, c, d

    probe = cell(0, len(columns) // 2)
    flip = dot(cross(sub(probe[1], probe[0]), sub(probe[2], probe[0])), (1.0, 0.0, 0.0)) < 0.0
    windows = bus_windows()
    count = len(rings[0])
    for k in range(len(rings) - 1):
        y0, ix0, _ = profile[k]
        y1, ix1, _ = profile[k + 1]
        for j in range(count):
            a, b, c, d = cell(k, j)
            seg_a = rings[k][j][1]
            seg_b = rings[k][(j + 1) % count][1]
            column_a = rings[k][j][2]
            column_b = rings[k][(j + 1) % count][2]
            mid_y = (y0 + y1) * 0.5
            target = body
            if seg_a == seg_b and seg_a in ("+x", "-x"):
                mid_z = (column_a + column_b) * 0.5
                # A cell wholly inside an arch goes; the others keep their corners inside it on its circle.
                if ix0 == 0.0 and ix1 == 0.0 and all(in_arch(cz, cy) for cz in (column_a, column_b) for cy in (y0, y1)):
                    continue
                if BUS_WINDOWS[0] < mid_y < BUS_WINDOWS[1] and any(lo < mid_z < hi for lo, hi in windows):
                    continue
                if seg_a == "+x" and ix0 == 0.0 and ix1 == 0.0:
                    if BUS_LETTERBOARD[0] < mid_y < BUS_LETTERBOARD[1] and any(lo < mid_z < hi for lo, hi in BUS_BARE_LETTERBOARD):
                        target = bare
                    if mid_y < BUS_BAND[0] and BUS_BARE_LOWER[0] < mid_z < BUS_BARE_LOWER[1]:
                        target = bare
                panel = int(math.floor((mid_z + BUS_HALF_LENGTH) / BUS_SEAM_PITCH))
                offset = panel_offsets[max(0, min(len(panel_offsets) - 1, panel))]
                uvs = [(p[2] + offset, sl) for p, sl in ((a, lengths[k]), (b, lengths[k]), (c, lengths[k + 1]), (d, lengths[k + 1]))]
            elif seg_a == seg_b and seg_a in ("front", "rear"):
                mid_x = (column_a + column_b) * 0.5
                if BUS_WINDOWS[0] < mid_y < BUS_WINDOWS[1] and (abs(mid_x) < 0.95 if seg_a == "front" else abs(mid_x) < 0.40):
                    continue
                uvs = [(p[0], sl) for p, sl in ((a, lengths[k]), (b, lengths[k]), (c, lengths[k + 1]), (d, lengths[k + 1]))]
            else:
                uvs = [(p[0] + p[2], sl) for p, sl in ((a, lengths[k]), (b, lengths[k]), (c, lengths[k + 1]), (d, lengths[k + 1]))]
            if y0 >= BUS_LETTERBOARD[1] + BUS_ROOF_RADIUS - 1e-6:
                mid_z = (a[2] + c[2]) * 0.5
                if BUS_BARE_ROOF[0] < mid_z < BUS_BARE_ROOF[1]:
                    target = bare
            if flip:
                target.quad(a, d, c, b, uvs[0], uvs[3], uvs[2], uvs[1])
            else:
                target.quad(a, b, c, d, uvs[0], uvs[1], uvs[2], uvs[3])

    # The body's underside at the floor (the chassis hides most of it).
    bottom = [p for p, _, _ in rings[0]]
    center = (0.0, BUS_FLOOR, 0.0)
    for k in range(len(bottom)):
        a = bottom[k]
        b = bottom[(k + 1) % len(bottom)]
        body.triangle(center, a, b, (0.0, 0.0), (a[0], a[2]), (b[0], b[2]), (0.0, -1.0, 0.0))

    # Wheel wells: the arch's circle carried 0.35 m in, closed by an inner wall.
    for sign in (1.0, -1.0):
        for center_z in BUS_ARCHES:
            low = math.asin((BUS_FLOOR - BUS_ARCH_HEIGHT) / BUS_ARCH_RADIUS)
            arc = [low + (math.pi - 2.0 * low) * k / 16 for k in range(17)]
            pts = [(center_z + BUS_ARCH_RADIUS * math.cos(a), BUS_ARCH_HEIGHT + BUS_ARCH_RADIUS * math.sin(a)) for a in arc]
            outer = sign * BUS_HALF_WIDTH
            inner = sign * (BUS_HALF_WIDTH - 0.35)
            for (z0, y0), (z1, y1) in zip(pts, pts[1:]):
                toward = (0.0, BUS_ARCH_HEIGHT - (y0 + y1) * 0.5, center_z - (z0 + z1) * 0.5)
                chassis.quad((outer, y0, z0), (outer, y1, z1), (inner, y1, z1), (inner, y0, z0),
                             (z0, 0.0), (z1, 0.0), (z1, 0.35), (z0, 0.35), toward)
            base = (inner, BUS_FLOOR, center_z)
            for (z0, y0), (z1, y1) in zip(pts, pts[1:]):
                chassis.triangle(base, (inner, y0, z0), (inner, y1, z1), (center_z, BUS_FLOOR), (z0, y0), (z1, y1), (sign, 0.0, 0.0))

    # Rub rails: half-rounds 6 cm tall, 1.8 cm proud, broken at the arches and (on the -X side) the door.
    for sign in (1.0, -1.0):
        for y in BUS_RAILS:
            breaks = [(c - BUS_ARCH_RADIUS - 0.06, c + BUS_ARCH_RADIUS + 0.06) for c in BUS_ARCHES if y < BUS_ARCH_HEIGHT + BUS_ARCH_RADIUS + 0.04]
            if sign < 0.0:
                breaks.append(BUS_DOOR)
            start = -(BUS_HALF_LENGTH - BUS_CORNER)
            spans = []
            for lo, hi in sorted(breaks):
                if lo > start:
                    spans.append((start, lo))
                start = max(start, hi)
            spans.append((start, BUS_HALF_LENGTH - BUS_CORNER))
            profile_points = [(0.018 * math.cos(math.radians(a)), 0.03 * math.sin(math.radians(a))) for a in range(-90, 91, 30)]
            for z0, z1 in spans:
                for (d0, h0), (d1, h1) in zip(profile_points, profile_points[1:]):
                    p0 = (sign * (BUS_HALF_WIDTH + d0), y + h0, z0)
                    p1 = (sign * (BUS_HALF_WIDTH + d1), y + h1, z0)
                    outward = (sign * (d0 + d1), h0 + h1, 0.0)
                    rails.quad(p0, p1, (p1[0], p1[1], z1), (p0[0], p0[1], z1), (z0, h0), (z0, h1), (z1, h1), (z1, h0), outward)
                for z, direction in ((z0, -1.0), (z1, 1.0)):
                    cap = [(sign * (BUS_HALF_WIDTH + d), y + h, z) for d, h in profile_points]
                    for k in range(1, len(cap) - 1):
                        rails.triangle(cap[0], cap[k], cap[k + 1], (0.0, 0.0), (0.0, 0.0), (0.0, 0.0), (0.0, 0.0, direction))

    # Boards behind the openings: the frame's reveal (3 cm deep) and planks 2 cm behind the frame.
    band = BUS_HALF_WIDTH - BUS_BAND_INSET

    def boarded(opening_lo, opening_hi, along, depth_axis, sign, face):
        """An opening of the band from opening_lo to opening_hi along `along` (2 for z, 0 for x), at `face`."""
        y0, y1 = BUS_WINDOWS

        def point(t, y, depth):
            p = [0.0, y, 0.0]
            p[along] = t
            p[depth_axis] = sign * (face - depth)
            return tuple(p)

        inward = [0.0, 0.0, 0.0]
        inward[depth_axis] = -sign
        for (t0, ya), (t1, yb), toward in (((opening_lo, y0), (opening_hi, y0), (0.0, 1.0, 0.0)), ((opening_lo, y1), (opening_hi, y1), (0.0, -1.0, 0.0))):
            body.quad(point(t0, ya, 0.0), point(t1, yb, 0.0), point(t1, yb, 0.03), point(t0, ya, 0.03),
                      (t0, 0.0), (t1, 0.0), (t1, 0.03), (t0, 0.03), toward)
        for t, direction in ((opening_lo, 1.0), (opening_hi, -1.0)):
            toward = [0.0, 0.0, 0.0]
            toward[along] = direction
            body.quad(point(t, y0, 0.0), point(t, y1, 0.0), point(t, y1, 0.03), point(t, y0, 0.03),
                      (y0, 0.0), (y1, 0.0), (y1, 0.03), (y0, 0.03), tuple(toward))
        planks = 4
        height = (y1 - y0) / planks
        for k in range(planks):
            lo = y0 + k * height + 0.006
            hi = y0 + (k + 1) * height - 0.006
            center = [0.0, (lo + hi) * 0.5, 0.0]
            center[along] = (opening_lo + opening_hi) * 0.5
            center[depth_axis] = sign * (face - 0.02 - 0.01)
            half = [0.0, (hi - lo) * 0.5, 0.0]
            half[along] = (opening_hi - opening_lo) * 0.5 + 0.02
            half[depth_axis] = 0.01
            box(boards, tuple(center), tuple(half))

    for sign in (1.0, -1.0):
        for lo, hi in windows:
            boarded(lo, hi, 2, 0, sign, band)
    boarded(-0.95, 0.95, 0, 2, 1.0, BUS_HALF_LENGTH - BUS_BAND_INSET)
    boarded(-0.40, 0.40, 0, 2, -1.0, BUS_HALF_LENGTH - BUS_BAND_INSET)

    box(chassis, (0.0, 0.485, 0.0), (1.07, 0.135, 4.7))
    # Wheel hubs inside the tyres (the scanned tyre has no rim).
    for x, z, flat in BUS_WHEELS:
        y = BUS_WHEEL_RADIUS * (BUS_FLAT_SQUASH if flat else 1.0)
        cylinder(chassis, (x - 0.1, y, z), (1.0, 0.0, 0.0), 0.31, 0.2, 20)
    box(bumpers, (0.0, 0.65, BUS_HALF_LENGTH + 0.06), (1.15, 0.10, 0.06))
    box(bumpers, (0.0, 0.65, -BUS_HALF_LENGTH - 0.06), (1.15, 0.10, 0.06))
    collision = box_hull((0.0, 1.645, 0.0), (BUS_HALF_WIDTH, 1.345, BUS_HALF_LENGTH))
    return {"Bus.Body": body, "Bus.BarePanels": bare, "Bus.Rails": rails, "Bus.Bumpers": bumpers, "Bus.Boards": boards,
            "Bus.Chassis": chassis, "Bus.Collision": collision}


# ---------------------------------------------------------------------------------------------------------------------
# The tanks, the pipes

def build_oil_tank():
    rng = random.Random(SEED + 2)
    shell, roof, fittings, pad = Part(), Part(), Part(), Part()
    origin = (0.0, 0.0, 0.0)
    pad_radius, pad_height = OIL_TANK_PAD
    revolve(pad, origin, [(pad_radius, 0.0), (pad_radius, pad_height)], 96)
    revolve(pad, origin, [(pad_radius, pad_height), (0.0, pad_height)], 96)
    segments = 160
    for course in range(OIL_TANK_COURSES):
        y0 = pad_height + course * OIL_TANK_COURSE
        rows = [(OIL_TANK_RADIUS, y0 + OIL_TANK_COURSE * k / 4) for k in range(5)]
        offset = 0.37 * rng.randrange(0, 11)
        # u = arc length plus the course's offset: u_scale turns radians into meters.
        part = Part()
        revolve(part, origin, rows, segments, u_scale=OIL_TANK_RADIUS, v_offset=y0)
        for a, b, c, ua, ub, uc in part.triangles:
            shell.triangles.append((a, b, c, (ua[0] + offset, ua[1]), (ub[0] + offset, ub[1]), (uc[0] + offset, uc[1])))
    top = pad_height + OIL_TANK_COURSES * OIL_TANK_COURSE
    roof_profile = [(OIL_TANK_RADIUS + 0.05 - (OIL_TANK_RADIUS + 0.05) * k / 6, top + OIL_TANK_ROOF_RISE * k / 6) for k in range(7)]
    part = Part()
    revolve(part, origin, roof_profile, 48)
    for a, b, c, _, _, _ in part.triangles:
        roof.triangles.append((a, b, c, (a[0], a[2]), (b[0], b[2]), (c[0], c[2])))
    # Sixteen radial seams on the roof: strips 4 cm wide, 1.2 cm proud.
    slope = OIL_TANK_ROOF_RISE / (OIL_TANK_RADIUS + 0.05)
    for k in range(16):
        a = 2.0 * math.pi * k / 16
        radial = (math.cos(a), 0.0, math.sin(a))
        tangent = (-math.sin(a), 0.0, math.cos(a))
        normal = normalize((radial[0] * slope, 1.0, radial[2] * slope))
        r0, r1 = 0.25, OIL_TANK_RADIUS
        p0 = (radial[0] * r0, top + OIL_TANK_ROOF_RISE - slope * r0, radial[2] * r0)
        p1 = (radial[0] * r1, top + OIL_TANK_ROOF_RISE - slope * r1, radial[2] * r1)
        corners = [add(p, tangent, s * 0.02) for p in (p0, p1) for s in (-1.0, 1.0)]
        raised = [add(c, normal, 0.012) for c in corners]
        roof.quad(raised[0], raised[2], raised[3], raised[1], (0.0, 0.0), (1.0, 0.0), (1.0, 0.04), (0.0, 0.04), normal)
        for s, i, j in ((-1.0, 0, 2), (1.0, 1, 3)):
            roof.quad(corners[i], corners[j], raised[j], raised[i], (0.0, 0.0), (1.0, 0.0), (1.0, 0.012), (0.0, 0.012), scale(tangent, s))
    # Weld beads: half-rounds (r 1.2 cm) at the course joints, and three vertical ones per course.
    bead = [(math.cos(math.radians(a)), math.sin(math.radians(a))) for a in range(-90, 91, 45)]
    for course in range(1, OIL_TANK_COURSES):
        y = pad_height + course * OIL_TANK_COURSE
        revolve(fittings, origin, [(OIL_TANK_RADIUS + 0.012 * c, y + 0.012 * s) for c, s in bead], segments, u_scale=OIL_TANK_RADIUS)
    for course in range(OIL_TANK_COURSES):
        y0 = pad_height + course * OIL_TANK_COURSE
        for base in (0.0, 120.0, 240.0):
            a = math.radians(base + 30.0 * course)
            radial = (math.cos(a), 0.0, math.sin(a))
            tangent = (-math.sin(a), 0.0, math.cos(a))
            points = [add(scale(radial, OIL_TANK_RADIUS + 0.012 * c), tangent, 0.012 * s) for c, s in bead]
            for p, q in zip(points, points[1:]):
                outward = add(scale(radial, 1.0), sub(add(p, q), scale(radial, 2.0 * OIL_TANK_RADIUS)))
                fittings.quad((p[0], y0, p[2]), (q[0], y0, q[2]), (q[0], y0 + OIL_TANK_COURSE, q[2]), (p[0], y0 + OIL_TANK_COURSE, p[2]),
                              (0.0, y0), (0.012, y0), (0.012, y0 + OIL_TANK_COURSE), (0.0, y0 + OIL_TANK_COURSE), outward)
    # The top angle ring (8 x 8 cm).
    revolve(fittings, origin, [(OIL_TANK_RADIUS, top - 0.08), (OIL_TANK_RADIUS + 0.08, top - 0.08), (OIL_TANK_RADIUS + 0.08, top),
                               (OIL_TANK_RADIUS, top)], segments, u_scale=OIL_TANK_RADIUS)
    # The manway (r 0.3, 24 bolts) 0.8 m up, facing tank azimuth -90; the nozzle (r 0.08) at azimuth 200.
    facing = (0.0, 0.0, -1.0)
    manway = (0.0, pad_height + 0.8, -(OIL_TANK_RADIUS - 0.03))
    cylinder(fittings, manway, facing, 0.3, 0.1, 32, caps=(False, True))
    eu, ev = frame_from(facing)
    for k in range(24):
        a = 2.0 * math.pi * k / 24
        bolt = add(add(manway, facing, 0.1), add(scale(eu, 0.26 * math.cos(a)), ev, 0.26 * math.sin(a)))
        cylinder(fittings, bolt, facing, 0.016, 0.025, 6, caps=(False, True))
    a = math.radians(OIL_TANK_NOZZLE[0])
    radial = (math.cos(a), 0.0, math.sin(a))
    nozzle = (radial[0] * (OIL_TANK_RADIUS - 0.05), OIL_TANK_NOZZLE[1], radial[2] * (OIL_TANK_RADIUS - 0.05))
    cylinder(fittings, nozzle, radial, 0.08, 0.35, 16, caps=(False, False))
    cylinder(fittings, add(nozzle, radial, 0.33), radial, 0.14, 0.03, 16)
    collision = prism_hull((0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 0.0, 1.0), (0.0, 1.0, 0.0), OIL_TANK_RADIUS + 0.1, 0.0,
                           top + OIL_TANK_ROOF_RISE, 16)
    return {"OilTank.Shell": shell, "OilTank.Roof": roof, "OilTank.Fittings": fittings, "OilTank.Pad": pad, "OilTank.Collision": collision}


def oil_tank_pipe_path():
    """The pipe from the tank's nozzle to the valve stand (D3: (-6.0, 0, 12.0) in the world, the tank at (2.7, 0, 17.2))
    and a 3 m stub on to a standpipe, in the tank's frame."""
    a = math.radians(OIL_TANK_NOZZLE[0])
    radial = (math.cos(a), 0.0, math.sin(a))
    y = OIL_TANK_NOZZLE[1]
    start = (radial[0] * (OIL_TANK_RADIUS + 0.33), y, radial[2] * (OIL_TANK_RADIUS + 0.33))
    bend = add(start, radial, 1.4)
    valve = (-6.0 - terrain.OIL_TANK["center"][0], y, 12.0 - terrain.OIL_TANK["center"][1])
    corner = (bend[0], y, valve[2])
    end = (valve[0] - 3.0, y, valve[2])
    return [start, bend, corner, valve, end], valve, end


def build_pipes():
    pipe, supports = Part(), Part()
    points, valve, end = oil_tank_pipe_path()
    path = fillet_path(points, 0.3)
    sweep_tube(pipe, path, 0.08, 12)
    # Flanges every 4 m along the path, H-frames every 3 m on its straight runs.
    lengths = [0.0]
    for p, q in zip(path, path[1:]):
        lengths.append(lengths[-1] + math.sqrt(dot(sub(q, p), sub(q, p))))

    def along(distance):
        for k in range(len(path) - 1):
            if lengths[k + 1] >= distance:
                t = (distance - lengths[k]) / max(lengths[k + 1] - lengths[k], 1e-9)
                return add(path[k], sub(path[k + 1], path[k]), t), normalize(sub(path[k + 1], path[k]))
        return path[-1], normalize(sub(path[-1], path[-2]))

    d = 4.0
    while d < lengths[-1] - 0.5:
        p, t = along(d)
        cylinder(pipe, add(p, t, -0.015), t, 0.14, 0.03, 16)
        d += 4.0
    d = 1.5
    while d < lengths[-1] - 0.3:
        p, t = along(d)
        side = normalize(cross((0.0, 1.0, 0.0), t))
        for s in (-1.0, 1.0):
            post = add(p, side, 0.16 * s)
            box(supports, (post[0], 0.21, post[2]), (0.02, 0.21, 0.02), (side, (0.0, 1.0, 0.0), t))
        box(supports, (p[0], 0.40, p[2]), (0.18, 0.02, 0.025), (side, (0.0, 1.0, 0.0), t))
        d += 3.0
    # The valve stand (a riser and its wheel) and the standpipe at the stub's end.
    cylinder(pipe, (valve[0], 0.0, valve[2]), (0.0, 1.0, 0.0), 0.09, 1.1, 16)
    wheel_center = (valve[0], 1.18, valve[2])
    cylinder(pipe, (valve[0], 1.1, valve[2]), (0.0, 1.0, 0.0), 0.02, 0.1, 8, caps=(False, True))
    ring = [add(wheel_center, (0.18 * math.cos(2.0 * math.pi * k / 24), 0.0, 0.18 * math.sin(2.0 * math.pi * k / 24))) for k in range(25)]
    sweep_tube(pipe, ring, 0.015, 8, closed_ends=False)
    for k in range(3):
        a = 2.0 * math.pi * k / 3
        sweep_tube(pipe, [wheel_center, add(wheel_center, (0.18 * math.cos(a), 0.0, 0.18 * math.sin(a)))], 0.01, 6)
    cylinder(pipe, (end[0], 0.0, end[2]), (0.0, 1.0, 0.0), 0.1, 1.2, 16)
    return {"Pipes.Pipe": pipe, "Pipes.Supports": supports}


def build_fuel_tank():
    shell, saddles, fittings = Part(), Part(), Part()
    axis_y = FUEL_TANK_AXIS
    r = FUEL_TANK_RADIUS
    half = FUEL_TANK_HALF_LENGTH
    segments = 64
    # The shell and its 2:1 heads, as one profile along Z: (z, radius).
    profile = [(-half - FUEL_TANK_HEAD * math.cos(math.radians(a)), r * math.sin(math.radians(a))) for a in range(0, 91, 10)]
    profile += [(-half + 2 * half * k / 16, r) for k in range(1, 17)]
    profile += [(half + FUEL_TANK_HEAD * math.sin(math.radians(a)), r * math.cos(math.radians(a))) for a in range(10, 91, 10)]
    rings = []
    for z, radius in profile:
        ring = []
        for k in range(segments + 1):
            a = 2.0 * math.pi * k / segments
            ring.append(((radius * math.cos(a), axis_y + radius * math.sin(a), z), (z, r * a)))
        rings.append(ring)
    for i in range(len(rings) - 1):
        for k in range(segments):
            a0, a1 = rings[i][k], rings[i][k + 1]
            b0, b1 = rings[i + 1][k], rings[i + 1][k + 1]
            mid = scale(add(add(a0[0], a1[0]), add(b0[0], b1[0])), 0.25)
            outward = (mid[0], mid[1] - axis_y, mid[2] - max(-half, min(half, mid[2])))
            shell.quad(a0[0], a1[0], b1[0], b0[0], a0[1], a1[1], b1[1], b0[1], outward)
    bead = [(math.sin(math.radians(a)), math.cos(math.radians(a))) for a in range(-90, 91, 45)]
    for z in (-half, -half / 2, 0.0, half / 2, half):
        weld = []
        for dz, dr in bead:
            weld.append([((r + 0.012 * dr) * math.cos(2.0 * math.pi * k / segments), axis_y + (r + 0.012 * dr) * math.sin(2.0 * math.pi * k / segments),
                          z + 0.012 * dz) for k in range(segments + 1)])
        for i in range(len(weld) - 1):
            for k in range(segments):
                p0, p1, q0, q1 = weld[i][k], weld[i][k + 1], weld[i + 1][k], weld[i + 1][k + 1]
                outward = (p0[0] + q1[0], p0[1] + q1[1] - 2.0 * axis_y, (p0[2] + q1[2]) - 2.0 * z)
                fittings.quad(p0, p1, q1, q0, (0.0, 0.0), (0.1, 0.0), (0.1, 0.01), (0.0, 0.01), outward)
    # Saddles: plates curved to the shell, wear plates under it, base plates.
    for z0 in (-2.0, 2.0):
        outline = [(-1.1, 0.02), (1.1, 0.02), (1.1, axis_y - math.sqrt(r * r - 1.1 * 1.1))]
        for k in range(1, 23):
            x = 1.1 - 2.2 * k / 22
            outline.append((x, axis_y - math.sqrt(r * r - x * x)))
        # Fanned from below the shell's lowest point: the plate's top follows the shell, so it is not convex.
        prism(saddles, outline, (0.0, 0.0, z0 - 0.015), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), 0.03, center=(0.0, 0.2))
        box(saddles, (0.0, 0.01, z0), (1.15, 0.01, 0.125))
        wear = []
        for k in range(13):
            a = math.radians(-150.0 + 120.0 * k / 12)
            wear.append((math.cos(a), math.sin(a)))
        for (c0, s0), (c1, s1) in zip(wear, wear[1:]):
            for zz in ((z0 - 0.125, z0 + 0.125),):
                p0 = ((r + 0.012) * c0, axis_y + (r + 0.012) * s0, zz[0])
                p1 = ((r + 0.012) * c1, axis_y + (r + 0.012) * s1, zz[0])
                saddles.quad(p0, p1, (p1[0], p1[1], zz[1]), (p0[0], p0[1], zz[1]), (zz[0], c0), (zz[0], c1), (zz[1], c1), (zz[1], c0),
                             (c0 + c1, s0 + s1, 0.0))
    # A bolted manhole on top and two nozzles below.
    top = (0.0, axis_y + r - 0.02, 0.0)
    cylinder(fittings, top, (0.0, 1.0, 0.0), 0.3, 0.1, 32, caps=(False, True))
    for k in range(16):
        a = 2.0 * math.pi * k / 16
        cylinder(fittings, (0.26 * math.cos(a), top[1] + 0.1, 0.26 * math.sin(a)), (0.0, 1.0, 0.0), 0.014, 0.022, 6, caps=(False, True))
    for z in (-1.0, 1.0):
        cylinder(fittings, (0.0, axis_y - r + 0.02, z), (0.0, -1.0, 0.0), 0.05, 0.2, 12, caps=(False, True))
    collision = prism_hull((0.0, axis_y, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0), r + 0.02, -half - FUEL_TANK_HEAD,
                           half + FUEL_TANK_HEAD, 12)
    return {"FuelTank.Shell": shell, "FuelTank.Saddles": saddles, "FuelTank.Fittings": fittings, "FuelTank.Collision": collision}


# ---------------------------------------------------------------------------------------------------------------------
# The hut, the lean-to, the embers, the wick

HUT_HALF = (2.6, 2.2)       # Outside, local x and z (the door wall at +z)
HUT_WALL = 0.2
HUT_HEIGHT = (2.6, 2.3)     # At the door wall and at the back
HUT_DOOR = (-0.8, 0.9, 2.05)
HUT_WINDOW = (1.2, 0.8, 0.6, 1.0)


def hut_wall_top(z):
    """The walls' top: 2.6 m at the door wall, falling to 2.3 m at the back."""
    t = (z + HUT_HALF[1]) / (2.0 * HUT_HALF[1])
    return HUT_HEIGHT[1] + (HUT_HEIGHT[0] - HUT_HEIGHT[1]) * t


def build_hut():
    walls, floor, door, shutters, rafters, furniture, lintels, roof = (Part() for _ in range(8))
    hx, hz = HUT_HALF
    w = HUT_WALL
    door_x, door_w, door_h = HUT_DOOR
    window_x, window_w, window_h, sill = HUT_WINDOW
    floor_y = 0.1
    # The door wall as six boxes around the door and the window.
    front = hz - w * 0.5
    top = HUT_HEIGHT[0]
    door_lo, door_hi = door_x - door_w * 0.5, door_x + door_w * 0.5
    win_lo, win_hi = window_x - window_w * 0.5, window_x + window_w * 0.5
    door_top = floor_y + door_h
    pieces = [(-hx, door_lo, 0.0, top), (door_hi, win_lo, 0.0, top), (win_lo, win_hi, 0.0, sill), (win_lo, win_hi, sill + window_h, top),
              (win_hi, hx, 0.0, top), (door_lo, door_hi, door_top, top)]
    for x0, x1, y0, y1 in pieces:
        box(walls, ((x0 + x1) * 0.5, (y0 + y1) * 0.5, front), ((x1 - x0) * 0.5, (y1 - y0) * 0.5, w * 0.5))
    # The back wall and the side walls (their tops follow the roof's slope).
    box(walls, (0.0, HUT_HEIGHT[1] * 0.5, -hz + w * 0.5), (hx, HUT_HEIGHT[1] * 0.5, w * 0.5))
    for sign in (-1.0, 1.0):
        outline = [(-hz + w, 0.0), (hz - w, 0.0), (hz - w, hut_wall_top(hz - w)), (-hz + w, hut_wall_top(-hz + w))]
        # The slab extends along -X from its origin (z x y), so the origin is each wall's outer (+X) or inner (-X) face.
        prism(walls, outline, (hx if sign > 0 else -hx + w, 0.0, 0.0), (0.0, 0.0, 1.0), (0.0, 1.0, 0.0), w)
    box(floor, (0.0, floor_y * 0.5, 0.0), (hx - w, floor_y * 0.5, hz - w))
    # The door leaf, open 70 degrees outward on its hinge at the door's -x jamb.
    leaf_axes = yaw_axes(-70.0)
    hinge = (door_lo, 0.0, hz)
    box(door, combine(hinge, leaf_axes, (door_w * 0.5, floor_y + door_h * 0.5, 0.02)), (door_w * 0.5, door_h * 0.5, 0.02), leaf_axes)
    # The shutters, folded back against the wall beside the window.
    for x in (win_lo - window_w * 0.25 - 0.01, win_hi + window_w * 0.25 + 0.01):
        box(shutters, (x, sill + window_h * 0.5, hz + 0.02), (window_w * 0.25, window_h * 0.5, 0.015))
    box(lintels, (door_x, door_top + 0.06, hz + 0.02), (door_w * 0.5 + 0.1, 0.06, 0.02))
    box(lintels, (window_x, sill + window_h + 0.05, hz + 0.02), (window_w * 0.5 + 0.1, 0.05, 0.02))
    # Rafters (5 x 15 cm every 0.6 m) along the slope, and the roof's sheets on them, overhanging 0.3 m.
    run = 2.0 * hz + 0.6
    rise = (HUT_HEIGHT[0] - HUT_HEIGHT[1]) * run / (2.0 * hz)
    slope = normalize((0.0, rise, run))
    up = normalize(cross(slope, (1.0, 0.0, 0.0)))
    if up[1] < 0.0:
        up = scale(up, -1.0)
    axes = ((1.0, 0.0, 0.0), up, slope)
    length = math.sqrt(run * run + rise * rise)
    mid_y = (HUT_HEIGHT[0] + HUT_HEIGHT[1]) * 0.5
    count = int(math.floor((2.0 * hx) / 0.6))
    for k in range(count + 1):
        x = -hx + 0.1 + (2.0 * hx - 0.2) * k / count
        box(rafters, combine((x, mid_y, 0.0), axes, (0.0, 0.075, 0.0)), (0.025, 0.075, length * 0.5), axes)
    box(roof, combine((0.0, mid_y, 0.0), axes, (0.0, 0.165, 0.0)), (hx + 0.3, 0.015, length * 0.5), axes)
    # Inside: a shelf on the back wall, a table by the +x wall.
    box(furniture, (0.0, 1.42, -hz + w + 0.15), (1.5, 0.015, 0.15))
    for x in (-1.2, 1.2):
        box(furniture, (x, 1.33, -hz + w + 0.02), (0.015, 0.08, 0.02))
    table = (hx - w - 0.45, 0.0, -0.4)
    box(furniture, (table[0], 0.75, table[2]), (0.35, 0.02, 0.6))
    for dx in (-0.3, 0.3):
        for dz in (-0.55, 0.55):
            box(furniture, (table[0] + dx, 0.37, table[2] + dz), (0.025, 0.36, 0.025))
    # Colliders: three walls, and the door wall as three boxes so the door stays open to the walk.
    hulls = [box_hull((0.0, HUT_HEIGHT[1] * 0.5, -hz + w * 0.5), (hx, HUT_HEIGHT[1] * 0.5, w * 0.5)),
             box_hull((-hx + w * 0.5, top * 0.5, 0.0), (w * 0.5, top * 0.5, hz)),
             box_hull((hx - w * 0.5, top * 0.5, 0.0), (w * 0.5, top * 0.5, hz)),
             box_hull(((-hx + door_lo) * 0.5, top * 0.5, front), ((door_lo + hx) * 0.5, top * 0.5, w * 0.5)),
             box_hull(((door_hi + hx) * 0.5, top * 0.5, front), ((hx - door_hi) * 0.5, top * 0.5, w * 0.5)),
             box_hull((door_x, (door_top + top) * 0.5, front), (door_w * 0.5, (top - door_top) * 0.5, w * 0.5))]
    parts = {"Hut.Walls": walls, "Hut.Floor": floor, "Hut.Door": door, "Hut.Shutters": shutters, "Hut.Rafters": rafters,
             "Hut.Furniture": furniture, "Hut.Lintels": lintels, "Hut.Roof": roof}
    for k, hull in enumerate(hulls):
        parts["Hut.Collision.%d" % (k + 1)] = hull
    return parts


def build_lean_to():
    posts, roof = Part(), Part()
    half_w, half_d = 1.5, 2.6
    heights = (2.4, 2.0)  # At the hut's side (-x) and at the outer side (+x)
    hulls = {}
    k = 1
    for x, h in ((-half_w + 0.15, heights[0]), (half_w - 0.05, heights[1])):
        for z in (-half_d + 0.05, half_d - 0.05):
            box(posts, (x, h * 0.5, z), (0.05, h * 0.5, 0.05))
            hulls["LeanTo.Collision.%d" % k] = box_hull((x, h * 0.5, z), (0.05, h * 0.5, 0.05))
            k += 1
    run = 2.0 * half_w + 0.3
    drop = (heights[0] - heights[1]) * run / (2.0 * half_w)
    slope = normalize((run, -drop, 0.0))
    up = normalize(cross((0.0, 0.0, 1.0), slope))
    if up[1] < 0.0:
        up = scale(up, -1.0)
    axes = (slope, up, (0.0, 0.0, 1.0))
    length = math.sqrt(run * run + drop * drop)
    box(roof, combine((0.15, (heights[0] + heights[1]) * 0.5, 0.0), axes, (0.0, 0.015, 0.0)), (length * 0.5, 0.015, half_d + 0.2), axes)
    parts = {"LeanTo.Posts": posts, "LeanTo.Roof": roof}
    parts.update(hulls)
    return parts


def build_embers():
    rng = random.Random(SEED + 3)
    embers = Part()
    octahedron = [(1.0, 0.0, 0.0), (-1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, -1.0, 0.0), (0.0, 0.0, 1.0), (0.0, 0.0, -1.0)]
    faces = [(0, 2, 4), (2, 1, 4), (1, 3, 4), (3, 0, 4), (2, 0, 5), (1, 2, 5), (3, 1, 5), (0, 3, 5)]
    placed = 0
    while placed < 25:
        radius = rng.uniform(0.03, 0.08)
        d = 0.35 * math.sqrt(rng.random())
        a = rng.uniform(0.0, 2.0 * math.pi)
        center = (d * math.cos(a), radius * 0.45, d * math.sin(a))
        jitter = {}

        def point(v):
            key = tuple(round(c, 6) for c in v)
            if key not in jitter:
                jitter[key] = radius * rng.uniform(0.7, 1.15)
            n = normalize(v)
            return add(center, (n[0], n[1] * 0.6, n[2]), jitter[key])

        for i, j, k in faces:
            a3, b3, c3 = octahedron[i], octahedron[j], octahedron[k]
            ab, bc, ca = normalize(add(a3, b3)), normalize(add(b3, c3)), normalize(add(c3, a3))
            for tri in ((a3, ab, ca), (ab, b3, bc), (ca, bc, c3), (ab, bc, ca)):
                pts = [point(v) for v in tri]
                outward = sub(scale(add(add(pts[0], pts[1]), pts[2]), 1.0 / 3.0), center)
                embers.triangle(pts[0], pts[1], pts[2], (pts[0][0], pts[0][2]), (pts[1][0], pts[1][2]), (pts[2][0], pts[2][2]), outward)
        placed += 1
    return {"Embers.Embers": embers}


def build_wick():
    wick = Part()
    radius = 0.0075
    # A capsule 4 cm tall: hemispheres on a 2.5 cm cylinder.
    profile = [(radius * math.sin(math.radians(a)), radius - radius * math.cos(math.radians(a))) for a in range(0, 91, 30)]
    profile += [(radius * math.cos(math.radians(a)), 0.0325 + radius * math.sin(math.radians(a))) for a in range(0, 91, 30)]
    revolve(wick, (0.0, 0.0, 0.0), profile, 10, u_scale=radius)
    return {"Wick.Wick": wick}


# ---------------------------------------------------------------------------------------------------------------------
# The bare steel's maps

def lattice_rows(period_x, period_y, seed, width, height):
    """Value noise on a periodic lattice (period_x by period_y cells over the image), as a list of rows of floats."""
    cell_x = width / period_x
    weights = []
    for x in range(width):
        f = x / cell_x
        i = int(f)
        t = f - i
        weights.append((i % period_x, (i + 1) % period_x, t * t * (3.0 - 2.0 * t)))
    rows_at_lattice = []
    for j in range(period_y):
        values = [terrain.hash2(i, j, seed) for i in range(period_x)]
        rows_at_lattice.append([values[a] + (values[b] - values[a]) * t for a, b, t in weights])
    cell_y = height / period_y
    rows = []
    for y in range(height):
        f = y / cell_y
        j = int(f)
        t = f - j
        t = t * t * (3.0 - 2.0 * t)
        r0 = rows_at_lattice[j % period_y]
        r1 = rows_at_lattice[(j + 1) % period_y]
        rows.append([a + (b - a) * t for a, b in zip(r0, r1)])
    return rows


def weighted_sum(specs, size):
    """The weighted mean of lattice noises [(weight, period x, period y, seed)], one field in memory at a time."""
    total = sum(spec[0] for spec in specs)
    rows = [[0.0] * size for _ in range(size)]
    for weight, period_x, period_y, seed in specs:
        field = lattice_rows(period_x, period_y, seed, size, size)
        w = weight / total
        for y in range(size):
            rows[y] = [r + w * s for r, s in zip(rows[y], field[y])]
    return rows


def steel_fields(size=STEEL_SIZE):
    """f (rain streaks and blotches, 0-1) and the rust mask m (0-1), tileable over the image."""
    # Rain streaks: narrow along x, stretched 8 times along y. Blotches and the rust's second field: round.
    streaks = weighted_sum([(0.5, 128, 16, SEED + 10), (0.3, 256, 32, SEED + 11), (0.2, 512, 64, SEED + 12)], size)
    blotches = weighted_sum([(0.5, 8, 8, SEED + 13), (0.25, 16, 16, SEED + 14), (0.125, 32, 32, SEED + 15), (0.0625, 64, 64, SEED + 16)], size)
    g = weighted_sum([(0.5, 4, 4, SEED + 17), (0.25, 8, 8, SEED + 18), (0.125, 16, 16, SEED + 19), (0.0625, 32, 32, SEED + 20)], size)
    f_rows = []
    m_rows = []
    for y in range(size):
        f_row = []
        m_row = []
        for s, b, gv in zip(streaks[y], blotches[y], g[y]):
            f = 0.5 + 1.8 * (0.55 * s + 0.45 * b - 0.5)
            f = 0.0 if f < 0.0 else (1.0 if f > 1.0 else f)
            f_row.append(f)
            m_row.append(terrain.smoothstep(0.55, 0.70, 0.6 * f + 0.4 * (0.5 + 1.8 * (gv - 0.5))))
        f_rows.append(f_row)
        m_rows.append(m_row)
    return f_rows, m_rows


def srgb_code(linear):
    linear = 0.0 if linear < 0.0 else (1.0 if linear > 1.0 else linear)
    encoded = linear * 12.92 if linear <= 0.0031308 else 1.055 * linear ** (1.0 / 2.4) - 0.055
    return int(round(encoded * 255.0))


def png_bytes(width, height, rows):
    """An 8-bit RGB PNG of byte rows, every row with filter 1 (Sub) and zlib level 9 (smooth noise compresses best)."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    raw = bytearray()
    for row in rows:
        raw.append(1)
        raw += bytes((row[i] - (row[i - 3] if i >= 3 else 0)) & 0xFF for i in range(len(row)))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def steel_normal_rows(m_rows, size=STEEL_SIZE):
    """The bare steel's tangent-space normals (OpenGL convention, +Y toward the image's top) as byte rows, from a tileable
    height field in meters: sheet steel is never flat, and its waviness is what breaks the sun's reflection on a flat
    panel into the streaks that read as metal (a flat mirror panel shows one even glare). The waves and dents are low
    frequencies a mip chain keeps, so they do not shimmer; the pits are only on rust."""
    texel = STEEL_TILE_METERS / size
    heights = [[0.0] * size for _ in range(size)]
    specs = [(amplitude, cells, SEED + 30 + k) for k, (amplitude, cells) in enumerate(STEEL_WAVES + (STEEL_DENTS,))]
    for amplitude, cells, seed in specs:
        field = lattice_rows(cells, cells, seed, size, size)
        for y in range(size):
            heights[y] = [h + amplitude * v for h, v in zip(heights[y], field[y])]
    pits = lattice_rows(STEEL_PITS[1], STEEL_PITS[1], SEED + 40, size, size)
    for y in range(size):
        heights[y] = [h + STEEL_PITS[0] * p * m for h, p, m in zip(heights[y], pits[y], m_rows[y])]
    rows = []
    for y in range(size):
        above = heights[(y - 1) % size]
        below = heights[(y + 1) % size]
        row = heights[y]
        out = bytearray()
        for x in range(size):
            slope_x = (row[(x + 1) % size] - row[x - 1]) / (2.0 * texel)
            slope_y = (above[x] - below[x]) / (2.0 * texel)
            length = math.sqrt(slope_x * slope_x + slope_y * slope_y + 1.0)
            out += bytes(int(round((0.5 + 0.5 * c / length) * 255.0)) for c in (-slope_x, -slope_y, 1.0))
        rows.append(out)
    return rows


def steel_maps():
    """The bare steel's color, arm and normal maps (PNG bytes) and the rust's share of the tile."""
    f_rows, m_rows = steel_fields()
    arm = []
    diff = []
    rusty = 0
    for f_row, m_row in zip(f_rows, m_rows):
        arm_row = bytearray()
        diff_row = bytearray()
        for f, m in zip(f_row, m_row):
            roughness = (STEEL_ROUGHNESS[0] + STEEL_ROUGHNESS[1] * f) * (1.0 - m) + RUST_ROUGHNESS * m
            arm_row += bytes((255, int(round(roughness * 255.0)), int(round(255.0 * (1.0 - m)))))
            # Steel darkens where the rain streaks ran (f high), rust is brown.
            shade = STEEL_SHADE[0] - STEEL_SHADE[1] * f
            diff_row += bytes(srgb_code(STEEL_COLOR[k] * shade * (1.0 - m) + RUST_COLOR[k] * m) for k in range(3))
            if m > 0.5:
                rusty += 1
        arm.append(arm_row)
        diff.append(diff_row)
    share = rusty / float(STEEL_SIZE * STEEL_SIZE)
    normals = steel_normal_rows(m_rows)
    return (png_bytes(STEEL_SIZE, STEEL_SIZE, diff), png_bytes(STEEL_SIZE, STEEL_SIZE, arm),
            png_bytes(STEEL_SIZE, STEEL_SIZE, normals), share)


# ---------------------------------------------------------------------------------------------------------------------
# Models, scripts and checks

BUILDERS = {"Bus": build_bus, "OilTank": build_oil_tank, "FuelTank": build_fuel_tank, "Pipes": build_pipes, "Hut": build_hut,
            "LeanTo": build_lean_to, "Embers": build_embers, "Wick": build_wick}


def build_models():
    """{model name: {node name: MeshData}} in MODELS' order."""
    models = {}
    for name, nodes in MODELS.items():
        parts = BUILDERS[name]()
        missing = [node for node, _, _ in nodes if node not in parts]
        extra = [node for node in parts if node not in [n for n, _, _ in nodes]]
        if missing or extra:
            raise terrain.CheckFailed("%s's nodes differ from MODELS: missing %s, extra %s" % (name, missing, extra))
        models[name] = {node: parts[node].mesh() for node, _, _ in nodes}
    return models


def model_bytes(name, meshes):
    glb = terrain.Glb(name)
    materials = {}
    for node, slot, _ in MODELS[name]:
        if slot not in materials:
            color, roughness, metallic = SLOT_FACTORS[slot]
            materials[slot] = glb.material(slot, color, roughness, metallic)
        glb.node(node, [glb.primitive(meshes[node], materials[slot])])
    return glb.bytes("Strata Tools/GenerateShowcaseProps.py")


def generate():
    """{file name: bytes} of the models and the maps, and the rust's share."""
    files = {}
    models = build_models()
    for name, meshes in models.items():
        files["%s.glb" % name] = model_bytes(name, meshes)
    diff, arm, normals, share = steel_maps()
    files["WeatheredSteel_diff.png"] = diff
    files["WeatheredSteel_arm.png"] = arm
    files["WeatheredSteel_nor_gl.png"] = normals
    return files, models, share


TEXTURE_SETS = {"asphalt_02": "2k", "brown_mud_03": "1k", "dry_ground_01": "2k", "rocky_terrain_02": "1k", "rusty_metal_03": "1k",
                "green_metal_rust": "2k", "beige_wall_001": "2k", "weathered_brown_planks": "1k", "rusty_painted_metal": "1k",
                "rusty_corrugated_iron": "1k"}

# The materials (D8): name, scan texture set or None, UVTiling (1 / the texture's real size), base color, metallic,
# roughness, other properties. The painted metal sets are paint: their arm maps' metalness is zero (S1's check), so
# their materials are dielectric; bare steel is WeatheredSteel, metal by its own map.
MATERIALS = [
    ("Asphalt", "asphalt_02", 1.0 / 3.0, (0.62, 0.62, 0.62), 0.0, 0.50, {}),
    ("Mud", "brown_mud_03", 1.0 / 1.3, (0.85, 0.85, 0.85), 0.0, 0.80, {}),
    ("Ground", "dry_ground_01", 0.25, (1.0, 1.0, 1.0), 0.0, 1.0, {}),
    ("RingGround", "rocky_terrain_02", 1.0 / 90.0, (1.0, 1.0, 1.0), 0.0, 1.0, {}),
    ("Water", None, 1.0, (0.018, 0.020, 0.022), 0.0, 0.03, {"IOR": 1.5}),
    ("BusPaint", "rusty_metal_03", 1.0 / 1.35, (0.85, 0.85, 0.85), 0.0, 0.35, {"Clearcoat": 0.0}),
    ("TankPaint", "green_metal_rust", 1.0, (1.0, 1.0, 1.0), 0.0, 0.70, {}),
    ("Plaster", "beige_wall_001", 1.0 / 3.0, (0.85, 0.85, 0.85), 0.0, 1.0, {}),
    ("Concrete", "beige_wall_001", 0.25, (0.55, 0.55, 0.55), 0.0, 1.0, {}),
    ("Planks", "weathered_brown_planks", 1.0 / 1.8, (1.0, 1.0, 1.0), 0.0, 1.0, {}),
    ("CorrugatedRed", "rusty_painted_metal", 1.0 / 2.2, (1.0, 1.0, 1.0), 0.0, 1.0, {}),
    ("CorrugatedRust", "rusty_corrugated_iron", 0.5, (1.0, 1.0, 1.0), 0.0, 1.0, {}),
    ("Grime", None, 1.0, (0.035, 0.032, 0.030), 0.0, 0.85, {}),
    ("Embers", None, 1.0, (0.05, 0.04, 0.035), 0.0, 0.9, {"EmissiveColor": [1.0, 0.35, 0.08], "EmissiveIntensity": 4000.0}),
]
PROPS_DIRECTORY = "Generated/Props"


def materials_script(tune=False):
    """material.create steps of every material (D8), or with tune the material.set steps that give the existing
    materials the same values (how a project made before a change of the table follows it)."""
    tables = []
    for name, scan, tiling, color, metallic, roughness, extra in MATERIALS:
        properties = {"BaseColor": [color[0], color[1], color[2], 1.0], "Metallic": metallic, "Roughness": roughness}
        if scan:
            res = TEXTURE_SETS[scan]
            prefix = "Textures/%s/%s_" % (scan, scan)
            properties.update({"BaseColorMap": prefix + "diff_%s.jpg" % res, "NormalMap": prefix + "nor_gl_%s.jpg" % res,
                               "MetallicRoughnessMap": prefix + "arm_%s.jpg" % res, "OcclusionMap": prefix + "arm_%s.jpg" % res,
                               "UVTiling": [round(tiling, 5), round(tiling, 5)]})
        properties.update(extra)
        tables.append((name, properties))
    # Bare steel: color, metal/roughness and relief from the generated maps over 4 m.
    tables.append(("WeatheredSteel", {
        "BaseColor": [1.0, 1.0, 1.0, 1.0], "Metallic": 1.0, "Roughness": 1.0, "NormalScale": 1.0,
        "BaseColorMap": PROPS_DIRECTORY + "/WeatheredSteel_diff.png", "MetallicRoughnessMap": PROPS_DIRECTORY + "/WeatheredSteel_arm.png",
        "NormalMap": PROPS_DIRECTORY + "/WeatheredSteel_nor_gl.png", "UVTiling": [1.0 / STEEL_TILE_METERS, 1.0 / STEEL_TILE_METERS]}))
    steps = []
    for name, properties in tables:
        path = "Materials/%s.stmat" % name
        if tune:
            steps.append({"command": "material.set", "parameters": {"material": path, "properties": properties},
                          "expect": {"/warnings": {"equals": []}}})
        else:
            steps.append({"command": "material.create", "parameters": {"path": path, "properties": properties},
                          "expect": {"/warnings": {"equals": []}}})
    return steps


def prefabs_script():
    """Command steps that make Prefabs/<Prop>.stprefab from the models, in a new scene that is discarded at the end."""
    steps = [{"command": "scene.new", "parameters": {"name": "PrefabWork"}}]

    def find(name, bind):
        # Node names are unique, and each model is alone in the scene while its prefab is made.
        steps.append({"command": "entity.find", "parameters": {"name": name}, "bind": {bind: "/entities/0"}})

    def instantiate(prefab, bind, parent=None, transform=None):
        parameters = {"prefab": prefab}
        if parent:
            parameters["parent"] = {"$ref": parent}
        if transform:
            parameters["components"] = {"Transform": transform}
        steps.append({"command": "prefab.instantiate", "parameters": parameters, "bind": {bind: "/id"}})

    def slots(model):
        for node, slot, _ in MODELS[model]:
            find(node, node)
            if slot == "Collision":
                steps.append({"command": "component.get", "parameters": {"entity": {"$ref": node}, "component": "MeshRenderer"},
                              "bind": {node + ".Mesh": "/values/Mesh"}})
                # Set the collider's mesh before the renderer goes: unset, it would use the renderer's.
                steps.append({"command": "component.add", "parameters": {"entity": {"$ref": node}, "component": "MeshCollider",
                                                                          "values": {"Mesh": {"$ref": node + ".Mesh"}, "Convex": True}}})
                steps.append({"command": "component.remove", "parameters": {"entity": {"$ref": node}, "component": "MeshRenderer"}})
            else:
                steps.append({"command": "component.set", "parameters": {"entity": {"$ref": node}, "component": "MeshRenderer",
                                                                          "values": {"Material": "Materials/%s.stmat" % slot}}})

    def light(name, parent, y, color, light_range):
        steps.append({"command": "entity.create", "parameters": {"name": name, "parent": {"$ref": parent}, "components": {
            "Transform": {"Translation": [0.0, y, 0.0]},
            "PointLight": {"Color": color, "Intensity": 0.0, "Range": light_range, "CastShadows": True}}}})

    def save(root, prefab):
        steps.append({"command": "prefab.create", "parameters": {"entities": [{"$ref": root}], "path": "Prefabs/%s.stprefab" % prefab,
                                                                 "link": False}})
        steps.append({"command": "entity.delete", "parameters": {"entities": [{"$ref": root}]}})

    def glb(model):
        return "%s/%s.glb" % (PROPS_DIRECTORY, model)

    def scan(name):
        return "Scans/%s_1k/%s_1k.gltf" % (name, name)

    # The bus on six tyres (scale 1.65; the front pair flat), axles along X.
    instantiate(glb("Bus"), "Bus")
    slots("Bus")
    axle = [0.0, math.sin(math.radians(45.0)), 0.0, math.cos(math.radians(45.0))]
    for index, (x, z, flat) in enumerate(BUS_WHEELS):
        squash = BUS_FLAT_SQUASH if flat else 1.0
        instantiate(scan("old_tyre"), "Tyre%d" % index, "Bus", {"Translation": [round(x, 3), round(BUS_WHEEL_RADIUS * squash, 4), z],
                                                                 "Rotation": [round(c, 6) for c in axle], "Scale": [1.65, round(1.65 * squash, 4), 1.65]})
    save("Bus", "Bus")
    # The oil tank with its pipes and the ladder leaning 15 degrees on it at the tank's azimuth 30.
    instantiate(glb("OilTank"), "OilTank")
    slots("OilTank")
    instantiate(glb("Pipes"), "Pipes", "OilTank")
    slots("Pipes")
    lean = math.radians(-15.0) * 0.5
    turn = math.radians(60.0) * 0.5
    rotation = [math.cos(turn) * math.sin(lean), math.cos(lean) * math.sin(turn), -math.sin(turn) * math.sin(lean), math.cos(turn) * math.cos(lean)]
    foot = 4.65
    azimuth = math.radians(30.0)
    instantiate(scan("ladder_sectioned_01"), "Ladder", "OilTank", {"Translation": [round(foot * math.cos(azimuth), 4), 0.0, round(foot * math.sin(azimuth), 4)],
                                                                    "Rotation": [round(c, 6) for c in rotation]})
    save("OilTank", "OilTank")
    instantiate(glb("FuelTank"), "FuelTank")
    slots("FuelTank")
    save("FuelTank", "FuelTank")
    # The hut with its lean-to on its +x side.
    instantiate(glb("Hut"), "Hut")
    slots("Hut")
    instantiate(glb("LeanTo"), "LeanTo", "Hut", {"Translation": [terrain.LEAN_TO["local"][0], 0.0, terrain.LEAN_TO["local"][1]]})
    slots("LeanTo")
    save("Hut", "Hut")
    # The fire pit: the stones (their origin is their middle), the embers and the light "Fire".
    steps.append({"command": "entity.create", "parameters": {"name": "FirePit"}, "bind": {"FirePit": "/id"}})
    instantiate(scan("stone_fire_pit"), "Stones", "FirePit", {"Translation": [0.0, 0.19, 0.0]})
    instantiate(glb("Embers"), "Embers", "FirePit", {"Translation": [0.0, 0.05, 0.0]})
    slots("Embers")
    light("Fire", "FirePit", 0.35, [1.0, 0.55, 0.25], 12.0)
    save("FirePit", "FirePit")
    # The lantern: the wick and the light "Light" inside it.
    instantiate(scan("Lantern_01"), "Lantern")
    instantiate(glb("Wick"), "Wick", "Lantern", {"Translation": [0.0, 0.1, 0.0]})
    slots("Wick")
    light("Light", "Lantern", 0.14, [1.0, 0.62, 0.32], 8.0)
    save("Lantern", "Lantern")
    steps.append({"command": "scene.open", "parameters": {"scene": "Scenes/Outpost.stscene", "force": True}})
    return steps


def letterboard_faces(meshes):
    """The bus's letterboard: Body and BarePanels faces of the +X side between its heights."""
    faces = []
    for node in ("Bus.Body", "Bus.BarePanels"):
        mesh = meshes[node]
        for t in range(0, len(mesh.indices), 3):
            a, b, c = (mesh.positions[i] for i in mesh.indices[t:t + 3])
            center = scale(add(add(a, b), c), 1.0 / 3.0)
            if center[0] > BUS_HALF_WIDTH - 0.01 and BUS_LETTERBOARD[0] < center[1] < BUS_LETTERBOARD[1] and abs(center[2]) < BUS_HALF_LENGTH - BUS_CORNER:
                faces.append((a, b, c))
    return faces


def letterboard(meshes):
    """What the stations' solve rests on: the letterboard's plane (x), its heights, its extent along z, and its faces'
    largest tilt from vertical (degrees), measured from the generated mesh."""
    faces = letterboard_faces(meshes)
    if not faces:
        raise terrain.CheckFailed("the bus has no letterboard faces between %.2f and %.2f m on its +X side" % BUS_LETTERBOARD)
    tilt = 0.0
    flat_x = []
    for a, b, c in faces:
        n = normalize(cross(sub(b, a), sub(c, a)))
        tilt = max(tilt, math.degrees(math.asin(min(1.0, abs(n[1])))))
        if n[0] > 0.9999:
            flat_x.append(a[0])
    # The plane of the faces that face +X; a tilted letterboard has none, and its tilt fails the check.
    plane = max(flat_x) if flat_x else max(p[0] for face in faces for p in face)
    return {"x": plane, "y": BUS_LETTERBOARD, "z": (-(BUS_HALF_LENGTH - BUS_CORNER), BUS_HALF_LENGTH - BUS_CORNER), "tilt": tilt,
            "faces": len(faces)}


def tank_shell(meshes):
    """The oil tank's shell measured from its mesh: its radius, its heights and its normals' largest deviation from the
    radial direction (degrees)."""
    mesh = meshes["OilTank.Shell"]
    radii = [math.sqrt(p[0] * p[0] + p[2] * p[2]) for p in mesh.positions]
    deviation = 0.0
    for p, n in zip(mesh.positions, mesh.normals):
        radial = normalize((p[0], 0.0, p[2]))
        deviation = max(deviation, math.degrees(math.acos(max(-1.0, min(1.0, dot(radial, n))))))
    return {"radius": sum(radii) / len(radii), "radiusSpread": max(radii) - min(radii), "y": (min(p[1] for p in mesh.positions),
            max(p[1] for p in mesh.positions)), "deviation": deviation}


def hull_problems(name, mesh):
    problems = []
    points = mesh.positions
    if len(points) > 32:
        problems.append("%s has %d vertices (at most 32)" % (name, len(points)))
    for t in range(0, len(mesh.indices), 3):
        a, b, c = (points[i] for i in mesh.indices[t:t + 3])
        n = cross(sub(b, a), sub(c, a))
        if any(dot(sub(p, a), n) > 1e-6 for p in points):
            problems.append("%s is not convex" % name)
            break
    return problems


def png_rows(data):
    """The decompressed image data (filter bytes and filtered rows) of a PNG."""
    offset = 8
    compressed = bytearray()
    while offset < len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        if kind == b"IDAT":
            compressed += data[offset + 8:offset + 8 + length]
        offset += 12 + length
    return zlib.decompress(bytes(compressed))


def png_pixels(data):
    """The decoded pixels of an 8-bit RGB PNG this script writes (any filter), as bytes."""
    offset = 8
    compressed = bytearray()
    width = height = 0
    while offset < len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        body = data[offset + 8:offset + 8 + length]
        if kind == b"IHDR":
            width, height = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            compressed += body
        offset += 12 + length
    raw = zlib.decompress(bytes(compressed))
    stride = width * 3
    out = bytearray()
    prior = bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            left = row[i - 3] if i >= 3 else 0
            up = prior[i]
            corner = prior[i - 3] if i >= 3 else 0
            if kind == 1:
                row[i] = (row[i] + left) & 0xFF
            elif kind == 2:
                row[i] = (row[i] + up) & 0xFF
            elif kind == 3:
                row[i] = (row[i] + ((left + up) >> 1)) & 0xFF
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                row[i] = (row[i] + (left if pa <= pb and pa <= pc else (up if pb <= pc else corner))) & 0xFF
        out += row
        prior = row
    return width, height, bytes(out)


def check_models(models):
    problems = []
    for name, nodes in MODELS.items():
        for node, slot, (lo, hi) in nodes:
            mesh = models[name][node]
            triangles = len(mesh.indices) // 3
            print("%s: %d triangles (%d-%d)" % (node, triangles, lo, hi))
            if not lo <= triangles <= hi:
                problems.append("%s has %d triangles, not %d-%d" % (node, triangles, lo, hi))
            if slot == "Collision":
                problems += hull_problems(node, mesh)
    board = letterboard(models["Bus"])
    print("Bus letterboard: %d faces at x %.4f, tilted at most %.5f degrees" % (board["faces"], board["x"], board["tilt"]))
    if board["tilt"] > 0.01 or abs(board["x"] - BUS_HALF_WIDTH) > 1e-3:
        problems.append("the bus's letterboard is not vertical at x %.2f" % BUS_HALF_WIDTH)
    shell = tank_shell(models["OilTank"])
    print("OilTank shell: radius %.4f (spread %.4f), normals within %.3f degrees of radial" % (shell["radius"], shell["radiusSpread"], shell["deviation"]))
    if abs(shell["radius"] - OIL_TANK_RADIUS) > 0.005 or shell["deviation"] > 1.0:
        problems.append("the oil tank's shell is not a cylinder of radius %.1f" % OIL_TANK_RADIUS)
    return problems


def write_files(files, output):
    os.makedirs(output, exist_ok=True)
    for name, data in files.items():
        with open(os.path.join(output, name), "wb") as file:
            file.write(data)
        print("%s: %d bytes" % (name, len(data)))


def write_script(steps, output):
    with open(output, "w", encoding="utf-8", newline="\n") as file:
        json.dump(steps, file, indent=1)
        file.write("\n")
    print("%s: %d steps" % (output, len(steps)))


def command_props(arguments):
    files, models, share = generate()
    problems = check_models(models)
    print("rust covers %.1f%% of the steel" % (share * 100.0))
    if problems:
        print("FAIL: %s; nothing written" % "; ".join(problems))
        return 1
    write_files(files, arguments.output)
    return 0


def command_selftest(arguments):
    problems = []
    first, models, share = generate()
    problems += check_models(models)
    print("rust covers %.1f%% of the steel" % (share * 100.0))
    if not 0.12 <= share <= 0.28:
        problems.append("rust covers %.1f%% of the steel (15-25%% intended)" % (share * 100.0))
    second, _, _ = generate()
    write_files(first, os.path.join(arguments.work, "first"))
    for name, data in first.items():
        if second[name] != data:
            problems.append("%s differs between two runs" % name)
        committed = os.path.join(arguments.committed, name)
        if not os.path.isfile(committed):
            problems.append("%s is not committed in %s" % (name, arguments.committed))
            continue
        with open(committed, "rb") as file:
            stored = file.read()
        if name.endswith(".png"):
            # Another zlib may compress differently: compare the filtered rows, then (if they differ) the pixels.
            try:
                same = png_rows(stored) == png_rows(data) or png_pixels(stored) == png_pixels(data)
            except (zlib.error, struct.error, IndexError) as error:
                same = False
                problems.append("the committed %s cannot be read (%s)" % (name, error))
            if not same:
                problems.append("the committed %s's pixels differ from what the generator writes" % name)
        elif stored != data:
            problems.append("the committed %s differs from what the generator writes (run it again and import the result)" % name)
    print("FAIL: %s" % "; ".join(problems) if problems else "PASS")
    return 1 if problems else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    props = commands.add_parser("props", help="Write the models and the steel's maps")
    props.add_argument("--output", required=True, help="The directory to write them into")
    materials = commands.add_parser("materials", help="Write the materials' command script")
    materials.add_argument("--output", required=True, help="The command script to write (.json)")
    tune = commands.add_parser("tune", help="Write the command script that gives the existing materials the table's values")
    tune.add_argument("--output", required=True, help="The command script to write (.json)")
    prefabs = commands.add_parser("prefabs", help="Write the prefabs' command script")
    prefabs.add_argument("--output", required=True, help="The command script to write (.json)")
    selftest = commands.add_parser("selftest", help="Generate twice and compare with the committed files")
    selftest.add_argument("--committed", required=True, help="The committed files' directory")
    selftest.add_argument("--work", required=True, help="A directory to write into")
    arguments = parser.parse_args()
    try:
        if arguments.command == "props":
            return command_props(arguments)
        if arguments.command in ("materials", "tune"):
            write_script(materials_script(arguments.command == "tune"), arguments.output)
            return 0
        if arguments.command == "prefabs":
            write_script(prefabs_script(), arguments.output)
            return 0
        return command_selftest(arguments)
    except terrain.CheckFailed as error:
        print("FAIL: %s" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
