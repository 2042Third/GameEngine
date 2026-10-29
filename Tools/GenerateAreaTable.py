#!/usr/bin/env python3
"""Writes the area table morphological anti-aliasing blends with (Renderer/MorphologicalAreaTable.inl).

The output is committed (CI never runs this script); run it again only to change the table:

    python Tools/GenerateAreaTable.py

Morphological anti-aliasing (AntiAliasingMode::Morphological; PostProcess/MorphologicalWeights.frag) finds, for a pixel on an
edge between two rows (or columns) of an aliased image, how far the edge runs on each side of it (d1 pixels before it, d2
after it, each at most 32) and how the staircase turns at each end of the run: the crossing edge there goes up (into the
row above the edge), down (into the row below), both ways or nowhere. It reconstructs the line the staircase stands for:
    - the edge lies on height 0 between x = 0 and x = L = d1 + d2 + 1 (pixel i spans [i, i + 1]; the pixel looked at is d1);
    - an end whose crossing goes up holds the line at +0.5 (halfway up the step), down at -0.5, both ways or nowhere at 0;
    - the line runs from the first end's height to 0 at the middle of the run (L / 2), and from there to the second end's
      height: a run between a step down and a step up is one straight line through its middle (a Z shape), a run with one
      step leans from it to the middle (L);
    - a run between two steps the same way (a U: a bump or a dent of the edge) blends nothing: fine texture is made of
      them (a dot is a run of one between two steps down), and rounding them off would wash it out.
For the pixel [d1, d1 + 1] the table holds the area between the line and the edge on each side, exactly (the line is
piecewise linear: each piece is integrated split at its zero): R, the area below the edge (inside the pixel below it,
covered by the color above), and G, the area above (inside the pixel above, covered by the color below). Both are at most
0.5; they are stored in 8 bits of [0, 1].

Layout: an RG8 texture 4 x 33 texels wide and 4 x 33 high: texel (e1 * 33 + d1, e2 * 33 + d2) for the first end's crossing
e1 and the second's e2 (0 nowhere, 1 up, 2 down, 3 both ways) and distances d1, d2 in [0, 32]. The .inl lists its bytes
row by row, R then G per texel.

Only the standard library is used; a run writes the same bytes every time.
"""

import os

REPOSITORY_DIRECTORY = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTPUT = os.path.join(REPOSITORY_DIRECTORY, "Strata", "src", "Strata", "Renderer", "MorphologicalAreaTable.inl")

MAX_DISTANCE = 32
SIZE = MAX_DISTANCE + 1
CROSSINGS = 4
HEIGHTS = (0.0, 0.5, -0.5, 0.0)  # nowhere, up, down, both ways


def split_areas(x0, f0, x1, f1):
    """The areas where the linear function from (x0, f0) to (x1, f1) is below 0 and above 0."""
    if x1 <= x0:
        return 0.0, 0.0
    if f0 >= 0.0 and f1 >= 0.0:
        return 0.0, 0.5 * (f0 + f1) * (x1 - x0)
    if f0 <= 0.0 and f1 <= 0.0:
        return -0.5 * (f0 + f1) * (x1 - x0), 0.0
    t = f0 / (f0 - f1)
    xm = x0 + t * (x1 - x0)
    below0, above0 = split_areas(x0, f0, xm, 0.0)
    below1, above1 = split_areas(xm, 0.0, x1, f1)
    return below0 + below1, above0 + above1


def line(x, length, h1, h2):
    middle = 0.5 * length
    if x <= middle:
        return h1 + (0.0 - h1) * (x / middle) if middle > 0.0 else 0.0
    return 0.0 + (h2 - 0.0) * ((x - middle) / (length - middle))


def pixel_areas(d1, d2, e1, e2):
    length = float(d1 + d2 + 1)
    h1 = HEIGHTS[e1]
    h2 = HEIGHTS[e2]
    if h1 * h2 > 0.0:
        # Both ends step the same way: a bump or a dent of the edge (a U). Fine texture is made of them (a dot, a short dash),
        # and rounding them off would wash it out, so they blend nothing.
        return 0.0, 0.0
    left = float(d1)
    right = left + 1.0
    middle = 0.5 * length
    below = 0.0
    above = 0.0
    # Integrate piece by piece: the line bends only at the middle.
    points = [left]
    if left < middle < right:
        points.append(middle)
    points.append(right)
    for a, b in zip(points, points[1:]):
        bel, abo = split_areas(a, line(a, length, h1, h2), b, line(b, length, h1, h2))
        below += bel
        above += abo
    return below, above


def build():
    width = CROSSINGS * SIZE
    data = bytearray(width * width * 2)
    for e1 in range(CROSSINGS):
        for e2 in range(CROSSINGS):
            for d1 in range(SIZE):
                for d2 in range(SIZE):
                    below, above = pixel_areas(d1, d2, e1, e2)
                    x = e1 * SIZE + d1
                    y = e2 * SIZE + d2
                    offset = (y * width + x) * 2
                    data[offset] = int(round(min(max(below, 0.0), 1.0) * 255.0))
                    data[offset + 1] = int(round(min(max(above, 0.0), 1.0) * 255.0))
    return width, data


def main():
    width, data = build()
    # Checks of the construction: a Z run of 7 between a step down and a step up passes through its middle (its middle
    # pixel splits evenly), a run without steps and a U blend nothing, and no area exceeds half a pixel.
    z = pixel_areas(3, 3, 2, 1)
    if abs(z[0] - z[1]) > 1e-12:
        raise SystemExit(f"the middle of a Z run is not even: {z}")
    if pixel_areas(5, 1, 0, 0) != (0.0, 0.0) or pixel_areas(0, 0, 2, 2) != (0.0, 0.0):
        raise SystemExit("a run without steps, or a U, blends")
    if max(data) > 128:
        raise SystemExit("an area exceeds half a pixel")
    lines = [
        "// Generated by Tools/GenerateAreaTable.py; do not edit.",
        f"// Morphological anti-aliasing's area table: RG8, {width} x {width} texels, row by row; texel (e1 * {SIZE} + d1, e2 * {SIZE} + d2) holds",
        "// the areas below (R) and above (G) a run's edge inside its pixel d1 (see the script).",
    ]
    for row in range(width):
        values = data[row * width * 2:(row + 1) * width * 2]
        for start in range(0, len(values), 32):
            lines.append(", ".join(str(v) for v in values[start:start + 32]) + ",")
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")
    print(f"Wrote {os.path.relpath(OUTPUT, REPOSITORY_DIRECTORY)} ({width} x {width} texels, {len(data)} bytes)")


if __name__ == "__main__":
    main()
