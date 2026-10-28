#!/usr/bin/env python3
"""Writes the white locus Post Process's white balance follows (Renderer/WhiteBalanceLocus.inl).

The output is committed (CI never runs this script); run it again only to change the table:

    python Tools/GenerateWhiteBalance.py

White balance (PostProcessComponent::WhiteBalanceTemperature, WhiteBalanceTint; PostProcessMath::GetWhiteBalanceMatrix)
names the light that should show as white by a temperature on this locus, moved along the locus's normal by the tint.
The table holds an entry every 5 mireds from 60 to 510 (16,667 K to 1,961 K), 91 entries, each the locus's chromaticity in
CIE 1960 (u, v) and its unit normal toward green (+v); the engine interpolates linearly in mireds. An entry is:
    - at or above 5000 K (200 mireds and below): the CIE daylight locus, the D illuminants' chromaticity polynomial (CIE 15)
      in T * 1.4388 / 1.4380, the correction that makes nominal 6500 K D65's temperature (CIE 15 defines D65 at 6504 K).
      The polynomial gives D65's tabulated chromaticity only to 5e-5 in (u, v), so the daylight part is shifted by that
      difference: 6500 K is sRGB's white exactly, and the white balance's default (the identity) lies on the locus;
    - at or below 4000 K (250 mireds and above): the Planckian locus, Planck's law (c2 = 1.4388e-2 m K) integrated against
      the CIE 1931 2-degree color matching functions, the CIE's published table from 380 to 780 nm in 5 nm steps embedded
      below as data (CIE 018:2019 and the CIE's dataset of the 1931 standard observer);
    - between them, the two blended linearly in mireds, so the locus has no jump.
Normals are the normalized perpendiculars of the blended table's tangent (central differences, one-sided at the ends).

Only the standard library is used; a run writes the same bytes every time.
"""

import math
import os

REPOSITORY_DIRECTORY = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTPUT = os.path.join(REPOSITORY_DIRECTORY, "Strata", "src", "Strata", "Renderer", "WhiteBalanceLocus.inl")

FIRST_MIREDS = 60
LAST_MIREDS = 510
STEP_MIREDS = 5
DAYLIGHT_MIREDS = 200  # 5000 K: the daylight locus at and below
PLANCKIAN_MIREDS = 250  # 4000 K: Planck's locus at and above
SECOND_RADIATION_CONSTANT = 1.4388e-2  # m K
D65 = (0.3127, 0.3290)  # sRGB's white (x, y)

# The CIE 1931 2-degree standard observer's color matching functions (wavelength in nm, x-bar, y-bar, z-bar), 5 nm steps.
COLOR_MATCHING_FUNCTIONS = [
    (380, 0.001368, 3.9e-05, 0.006450001),
    (385, 0.002236, 6.4e-05, 0.01054999),
    (390, 0.004243, 0.00012, 0.02005001),
    (395, 0.00765, 0.000217, 0.03621),
    (400, 0.01431, 0.000396, 0.06785001),
    (405, 0.02319, 0.00064, 0.1102),
    (410, 0.04351, 0.00121, 0.2074),
    (415, 0.07763, 0.00218, 0.3713),
    (420, 0.13438, 0.004, 0.6456),
    (425, 0.21477, 0.0073, 1.0390501),
    (430, 0.2839, 0.0116, 1.3856),
    (435, 0.3285, 0.01684, 1.62296),
    (440, 0.34828, 0.023, 1.74706),
    (445, 0.34806, 0.0298, 1.7826),
    (450, 0.3362, 0.038, 1.77211),
    (455, 0.3187, 0.048, 1.7441),
    (460, 0.2908, 0.06, 1.6692),
    (465, 0.2511, 0.0739, 1.5281),
    (470, 0.19536, 0.09098, 1.28764),
    (475, 0.1421, 0.1126, 1.0419),
    (480, 0.09564, 0.13902, 0.8129501),
    (485, 0.05795001, 0.1693, 0.6162),
    (490, 0.03201, 0.20802, 0.46518),
    (495, 0.0147, 0.2586, 0.3533),
    (500, 0.0049, 0.323, 0.272),
    (505, 0.0024, 0.4073, 0.2123),
    (510, 0.0093, 0.503, 0.1582),
    (515, 0.0291, 0.6082, 0.1117),
    (520, 0.06327, 0.71, 0.07824999),
    (525, 0.1096, 0.7932, 0.05725001),
    (530, 0.1655, 0.862, 0.04216),
    (535, 0.2257499, 0.9148501, 0.02984),
    (540, 0.2904, 0.954, 0.0203),
    (545, 0.3597, 0.9803, 0.0134),
    (550, 0.4334499, 0.9949501, 0.008749999),
    (555, 0.5120501, 1.0, 0.005749999),
    (560, 0.5945, 0.995, 0.0039),
    (565, 0.6784, 0.9786, 0.002749999),
    (570, 0.7621, 0.952, 0.0021),
    (575, 0.8425, 0.9154, 0.0018),
    (580, 0.9163, 0.87, 0.001650001),
    (585, 0.9786, 0.8163, 0.0014),
    (590, 1.0263, 0.757, 0.0011),
    (595, 1.0567, 0.6949, 0.001),
    (600, 1.0622, 0.631, 0.0008),
    (605, 1.0456, 0.5668, 0.0006),
    (610, 1.0026, 0.503, 0.00034),
    (615, 0.9384, 0.4412, 0.00024),
    (620, 0.8544499, 0.381, 0.00019),
    (625, 0.7514, 0.321, 0.0001),
    (630, 0.6424, 0.265, 4.999999e-05),
    (635, 0.5419, 0.217, 3e-05),
    (640, 0.4479, 0.175, 2e-05),
    (645, 0.3608, 0.1382, 1e-05),
    (650, 0.2835, 0.107, 0.0),
    (655, 0.2187, 0.0816, 0.0),
    (660, 0.1649, 0.061, 0.0),
    (665, 0.1212, 0.04458, 0.0),
    (670, 0.0874, 0.032, 0.0),
    (675, 0.0636, 0.0232, 0.0),
    (680, 0.04677, 0.017, 0.0),
    (685, 0.0329, 0.01192, 0.0),
    (690, 0.0227, 0.00821, 0.0),
    (695, 0.01584, 0.005723, 0.0),
    (700, 0.01135916, 0.004102, 0.0),
    (705, 0.008110916, 0.002929, 0.0),
    (710, 0.005790346, 0.002091, 0.0),
    (715, 0.004109457, 0.001484, 0.0),
    (720, 0.002899327, 0.001047, 0.0),
    (725, 0.00204919, 0.00074, 0.0),
    (730, 0.001439971, 0.00052, 0.0),
    (735, 0.0009999493, 0.0003611, 0.0),
    (740, 0.0006900786, 0.0002492, 0.0),
    (745, 0.0004760213, 0.0001719, 0.0),
    (750, 0.0003323011, 0.00012, 0.0),
    (755, 0.0002348261, 8.48e-05, 0.0),
    (760, 0.0001661505, 6e-05, 0.0),
    (765, 0.000117413, 4.24e-05, 0.0),
    (770, 8.307527e-05, 3e-05, 0.0),
    (775, 5.870652e-05, 2.12e-05, 0.0),
    (780, 4.150994e-05, 1.499e-05, 0.0),
]


def xy_to_uv(x, y):
    denominator = -2.0 * x + 12.0 * y + 3.0
    return 4.0 * x / denominator, 6.0 * y / denominator


def daylight_polynomial_uv(kelvin):
    corrected = kelvin * 1.4388 / 1.4380
    if corrected <= 7000.0:
        x = -4.6070e9 / corrected ** 3 + 2.9678e6 / corrected ** 2 + 0.09911e3 / corrected + 0.244063
    else:
        x = -2.0064e9 / corrected ** 3 + 1.9018e6 / corrected ** 2 + 0.24748e3 / corrected + 0.237040
    y = -3.000 * x * x + 2.870 * x - 0.275
    return xy_to_uv(x, y)


def daylight_uv(kelvin):
    """The daylight locus shifted so that nominal 6500 K is D65's (u, v)."""
    reference = xy_to_uv(*D65)
    at6500 = daylight_polynomial_uv(6500.0)
    u, v = daylight_polynomial_uv(kelvin)
    return u + reference[0] - at6500[0], v + reference[1] - at6500[1]


def planckian_uv(kelvin):
    big_x = big_y = big_z = 0.0
    for wavelength, x_bar, y_bar, z_bar in COLOR_MATCHING_FUNCTIONS:
        meters = wavelength * 1e-9
        radiance = meters ** -5 / math.expm1(SECOND_RADIATION_CONSTANT / (meters * kelvin))
        big_x += radiance * x_bar
        big_y += radiance * y_bar
        big_z += radiance * z_bar
    denominator = big_x + 15.0 * big_y + 3.0 * big_z
    return 4.0 * big_x / denominator, 6.0 * big_y / denominator


def locus_uv(mireds):
    kelvin = 1e6 / mireds
    if mireds <= DAYLIGHT_MIREDS:
        return daylight_uv(kelvin)
    if mireds >= PLANCKIAN_MIREDS:
        return planckian_uv(kelvin)
    weight = (mireds - DAYLIGHT_MIREDS) / (PLANCKIAN_MIREDS - DAYLIGHT_MIREDS)
    daylight = daylight_uv(kelvin)
    planckian = planckian_uv(kelvin)
    return tuple((1.0 - weight) * d + weight * p for d, p in zip(daylight, planckian))


def build_table():
    mireds = list(range(FIRST_MIREDS, LAST_MIREDS + 1, STEP_MIREDS))
    points = [locus_uv(float(m)) for m in mireds]
    table = []
    for index, (m, (u, v)) in enumerate(zip(mireds, points)):
        before = points[max(index - 1, 0)]
        after = points[min(index + 1, len(points) - 1)]
        tangent = (after[0] - before[0], after[1] - before[1])
        length = math.hypot(*tangent)
        normal = (-tangent[1] / length, tangent[0] / length)
        if normal[1] < 0.0:  # Toward green: +v
            normal = (-normal[0], -normal[1])
        table.append((m, u, v, normal[0], normal[1]))
    return table


def interpolate(table, mireds):
    for first, second in zip(table, table[1:]):
        if first[0] <= mireds <= second[0]:
            weight = (mireds - first[0]) / (second[0] - first[0])
            return tuple((1.0 - weight) * a + weight * b for a, b in zip(first[1:3], second[1:3]))
    raise ValueError(mireds)


def main():
    table = build_table()
    d65 = xy_to_uv(*D65)
    at6500 = interpolate(table, 1e6 / 6500.0)
    gap = max(math.hypot(b[1] - a[1], b[2] - a[2]) for a, b in zip(table, table[1:]))
    print(f"6500 K: (u, v) = ({at6500[0]:.6f}, {at6500[1]:.6f}); D65 ({d65[0]:.6f}, {d65[1]:.6f}); largest step {gap:.5f}")
    if max(abs(a - b) for a, b in zip(at6500, d65)) > 1e-5:
        raise SystemExit("the locus at 6500 K is not D65 within 1e-5")
    lines = [
        "// Generated by Tools/GenerateWhiteBalance.py; do not edit.",
        "// The white locus white balance follows: { mireds, u, v, normal u, normal v } in CIE 1960 (u, v), every 5 mireds from 60 to",
        "// 510; the normal points toward green (+v). The daylight locus at and above 5000 K, Planck's below 4000 K, blended between.",
    ]
    for m, u, v, nu, nv in table:
        lines.append(f"{{ {m}.0, {u:.9f}, {v:.9f}, {nu:.9f}, {nv:.9f} }},")
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")
    print(f"Wrote {os.path.relpath(OUTPUT, REPOSITORY_DIRECTORY)} ({len(table)} entries)")


if __name__ == "__main__":
    main()
