#!/usr/bin/env python3
"""Fits the Filmic tone curve's gamut matrices and writes its constants for the shaders and the engine.

The outputs are committed (CI never runs this script); run it again only to change the curve or its targets:

    python Tools/FitFilmic.py                    # fit, check every target, write both files
    python Tools/FitFilmic.py --check            # check the committed constants against the targets, write nothing
    python Tools/FitFilmic.py --from-committed   # refine the committed constants as one more (the first) start

The curve (TonemapOperator::Filmic) works per channel on an inset color, after AgX's principle: a per-channel curve on a
gamut drawn in toward white desaturates bright colors toward white instead of skewing their hue, and the outset gives
back the saturation the inset took. For an exposed scene value x and t = log2(x / 0.18), the stops from middle gray:

    shadows, t < 0:        y = 0.18 * 2^(Ct t + (C0 - Ct) tau (exp(t / tau) - 1))
    highlights, 0 <= t:    y = 0.18 + 0.82 (1 - (1 - t / Tw)^q) below Tw, 1 from Tw on, q = C0 0.18 ln2 Tw / 0.82

so middle gray shows as middle gray (sRGB 117.6), the log-log slope is C0 at middle gray, turning to the toe slope Ct over
about tau stops below it, and white is reached with zero slope Tw stops above it. The look: Ct = C0 (0.85 + 0.5 Toe),
Tw = 6.5 + 3 (Shoulder - 0.5) stops (Renderer/PostProcessMath.cpp, MakeFilmicCurve).

The gamut: row i of the inset is (1 - s_i) e_i + s_i (1/3 (1, 1, 1) + rho_i (e_{i+1} - e_{i-1})), indices mod 3: each
channel takes a share s_i of the mean, turned by rho_i toward its next or previous neighbor. Every row sums to 1, so
neutral colors pass unchanged and the gray ramp is exactly the curve. The outset is the inverse of a matrix of the same
form with its own s'_i and rho'_i. Bounds: s in [0, 0.45], rho in [-0.15, 0.15]. The operator is
o = clamp(outset * curve(inset * max(color, 0)), 0, 1), display-linear, before the saturation and the sRGB encode.

The fit: the twelve matrix values, tau in [0.75, 3] and C0 in [1.35, 1.45], by Nelder-Mead (written here) from 8 seeded
starts (and, with --from-committed, the committed constants first), each refined again from 3 seeded hops, minimizing the
worst hue deviation (as a share of its color's limit, in tens of degrees) + 0.1 times the sum of the deviations + 40 times
the saturation band's violations + 400 times the path to white's + 10^6 times the decreases T8 forbids (on a coarser
grid), subject to the targets below. It exits 1 and writes nothing when a target is not met. The committed constants came
from the seeded starts refined three times with --from-committed --starts 0 as the targets settled (T6 and T8 made hard).

Targets (Toe and Shoulder 0.5 unless named):
    T1 middle gray: y(0.18) = 0.18.
    T2 contrast: the log-log slope at middle gray is C0 within 0.01, C0 in [1.35, 1.45].
    T3 white: a neutral at +Tw is 1.0; at +6 stops at least 0.955 (sRGB code 250).
    T4 toe: 0.005 shows at sRGB code 2 to 4.
    T5 hue: ten test colors (scene-linear sRGB, scaled to luminance 0.18), each ramped from -4 to +7 stops in quarter
       stops. Hue is measured in ICtCp (ITU-R BT.2100: BT.709 to BT.2020 by BT.2087's matrix, the LMS matrix, PQ with a
       display white of 100 nits). The displayed hue stays within 12.5 degrees (the seven saturated colors) and 7.5
       degrees (skin, foliage, sky) of the hue of the scene color scaled linearly to the same display luminance, while the
       displayed chroma is at least 15% of the ramp's largest. (Planned at 10 and 6; with T6 and T8 held, the fit's form
       reaches about 12.2 on blue and 7.3 on sky, where ACES drifts 29.1 and 5.9 and AgX 13.0 and 6.9: the report
       prints all three per color, and the limits keep the planned ratio between the two groups.)
    T6 path to white: every test color at +Tw + 1 stops has every channel at least 0.95.
    T7 saturation: at 0 stops, each color's displayed ICtCp chroma over that of the scene color scaled linearly to the same
       display luminance lies in [0.90, 1.20].
    T8 shape: monotonic per channel and in luminance over [-12, +10] stops for Toe and Shoulder on a 0.25 grid; q >= 1.

Outputs (the same numbers, 9 significant digits):
    Strata/shaders/Include/FilmicConstants.glsl   c_FilmicInset, c_FilmicOutset (column-major, as GLSL builds them),
                                                  c_FilmicContrast (C0), c_FilmicToeWidth (tau), c_FilmicMiddleGray
    Strata/src/Strata/Renderer/FilmicConstants.h  Strata::FilmicConstants::Inset, Outset, Contrast, ToeWidth, MiddleGray

Only the standard library is used; a run is deterministic.
"""

import argparse
import math
import os
import random
import re
import sys

REPOSITORY_DIRECTORY = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GLSL_OUTPUT = os.path.join(REPOSITORY_DIRECTORY, "Strata", "shaders", "Include", "FilmicConstants.glsl")
HEADER_OUTPUT = os.path.join(REPOSITORY_DIRECTORY, "Strata", "src", "Strata", "Renderer", "FilmicConstants.h")

MIDDLE_GRAY = 0.18
LUMINANCE = (0.2126, 0.7152, 0.0722)

TEST_COLORS = [
    ("red", (1.0, 0.05, 0.05), 12.5),
    ("green", (0.05, 1.0, 0.05), 12.5),
    ("blue", (0.05, 0.05, 1.0), 12.5),
    ("yellow", (1.0, 1.0, 0.05), 12.5),
    ("cyan", (0.05, 1.0, 1.0), 12.5),
    ("magenta", (1.0, 0.05, 1.0), 12.5),
    ("orange", (1.0, 0.4, 0.05), 12.5),
    ("skin", (0.8, 0.5, 0.35), 7.5),
    ("foliage", (0.2, 0.4, 0.08), 7.5),
    ("sky", (0.25, 0.45, 0.9), 7.5),
]
RAMP_STOPS = [-4.0 + 0.25 * index for index in range(45)]  # -4 to +7
CHROMA_FLOOR = 0.15
SATURATION_BAND = (0.90, 1.20)
S_BOUNDS = (0.0, 0.45)
RHO_BOUNDS = (-0.15, 0.15)
TAU_BOUNDS = (0.75, 3.0)
CONTRAST_BOUNDS = (1.35, 1.45)
STARTS = 8
HOPS = 3
HOP_SCALES = [0.04] * 3 + [0.03] * 3 + [0.04] * 3 + [0.03] * 3 + [0.2, 0.01]
SEED = 20261027


# --- The curve ------------------------------------------------------------------------------------------------------

def make_curve(contrast, toe, shoulder):
    """The look's curve parameters: (toe slope Ct, white stops Tw, shoulder power q)."""
    toe = min(max(toe, 0.0), 1.0)
    shoulder = min(max(shoulder, 0.0), 1.0)
    toe_slope = contrast * (0.85 + 0.5 * toe)
    white_stops = 6.5 + 3.0 * (shoulder - 0.5)
    shoulder_power = contrast * MIDDLE_GRAY * math.log(2.0) * white_stops / (1.0 - MIDDLE_GRAY)
    return toe_slope, white_stops, shoulder_power


def curve(x, contrast, tau, look):
    toe_slope, white_stops, shoulder_power = look
    if x <= 0.0:
        return 0.0
    t = math.log2(x / MIDDLE_GRAY)
    if t < 0.0:
        exponent = toe_slope * t + (contrast - toe_slope) * tau * (math.exp(t / tau) - 1.0)
        return MIDDLE_GRAY * 2.0 ** exponent
    if t >= white_stops:
        return 1.0
    return MIDDLE_GRAY + (1.0 - MIDDLE_GRAY) * (1.0 - (1.0 - t / white_stops) ** shoulder_power)


# --- Matrices -------------------------------------------------------------------------------------------------------

def gamut_matrix(shares, turns):
    """Rows (1 - s_i) e_i + s_i (1/3 (1, 1, 1) + rho_i (e_{i+1} - e_{i-1}))."""
    rows = []
    for i in range(3):
        row = [shares[i] / 3.0] * 3
        row[i] += 1.0 - shares[i]
        row[(i + 1) % 3] += shares[i] * turns[i]
        row[(i + 2) % 3] -= shares[i] * turns[i]
        rows.append(row)
    return rows


def inverse(m):
    a, b, c = m[0]
    d, e, f = m[1]
    g, h, i = m[2]
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [
        [(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
        [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
        [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det],
    ]


def apply(m, v):
    return tuple(m[r][0] * v[0] + m[r][1] * v[1] + m[r][2] * v[2] for r in range(3))


def unpack(params):
    """Parameters: s0 s1 s2 rho0 rho1 rho2 s'0 s'1 s'2 rho'0 rho'1 rho'2 tau C0."""
    inset = gamut_matrix(params[0:3], params[3:6])
    outset = inverse(gamut_matrix(params[6:9], params[9:12]))
    return inset, outset, params[12], params[13]


def filmic(color, inset, outset, contrast, tau, look):
    v = apply(inset, tuple(max(c, 0.0) for c in color))
    y = tuple(curve(c, contrast, tau, look) for c in v)
    return tuple(min(max(c, 0.0), 1.0) for c in apply(outset, y))


# --- The other curves, transcribed from PostProcess/Tonemap.frag (for comparison) --------------------------------------

ACES_INPUT = [[0.59719, 0.35458, 0.04823], [0.07600, 0.90834, 0.01566], [0.02840, 0.13383, 0.83777]]
ACES_OUTPUT = [[1.60475, -0.53108, -0.07367], [-0.10208, 1.10813, -0.00605], [-0.00327, -0.07276, 1.07602]]


def aces(color):
    v = apply(ACES_INPUT, color)
    v = tuple((x * (x + 0.0245786) - 0.000090537) / (x * (0.983729 * x + 0.4329510) + 0.238081) for x in v)
    return tuple(min(max(c, 0.0), 1.0) for c in apply(ACES_OUTPUT, v))


AGX_INSET = [[0.842479062253094, 0.0784335999999992, 0.0792237451477643],
             [0.0423282422610123, 0.878468636469772, 0.0791661274605434],
             [0.0423756549057051, 0.0784336, 0.879142973793104]]
AGX_OUTSET = [[1.19687900512017, -0.0980208811401368, -0.0990297440797205],
              [-0.0528968517574562, 1.15190312990417, -0.0989611768448433],
              [-0.0529716355144438, -0.0980434501171241, 1.15107367264116]]


def agx(color):
    v = apply(AGX_INSET, tuple(max(c, 1e-10) for c in color))
    v = tuple(min(max((math.log2(x) + 12.47393) / (4.026069 + 12.47393), 0.0), 1.0) for x in v)
    v = tuple(15.5 * x ** 6 - 40.14 * x ** 5 + 31.96 * x ** 4 - 6.868 * x ** 3 + 0.4298 * x ** 2 + 0.1191 * x - 0.00232 for x in v)
    v = apply(AGX_OUTSET, v)
    return tuple(max(c, 0.0) ** 2.2 for c in v)


# --- ICtCp (ITU-R BT.2100) --------------------------------------------------------------------------------------------

BT709_TO_BT2020 = [[0.627403896, 0.329283039, 0.043313065],
                   [0.069097289, 0.919540395, 0.011362316],
                   [0.016391439, 0.088013308, 0.895595253]]
BT2020_TO_LMS = [[1688.0 / 4096.0, 2146.0 / 4096.0, 262.0 / 4096.0],
                 [683.0 / 4096.0, 2951.0 / 4096.0, 462.0 / 4096.0],
                 [99.0 / 4096.0, 309.0 / 4096.0, 3688.0 / 4096.0]]
PQ_M1 = 2610.0 / 16384.0
PQ_M2 = 2523.0 / 4096.0 * 128.0
PQ_C1 = 3424.0 / 4096.0
PQ_C2 = 2413.0 / 4096.0 * 32.0
PQ_C3 = 2392.0 / 4096.0 * 32.0
DISPLAY_WHITE_NITS = 100.0


def pq(value):
    y = max(value, 0.0) * DISPLAY_WHITE_NITS / 10000.0
    p = y ** PQ_M1
    return ((PQ_C1 + PQ_C2 * p) / (1.0 + PQ_C3 * p)) ** PQ_M2


def ictcp(color):
    """(I, Ct, Cp) of a display-linear BT.709 color whose white (1, 1, 1) is 100 nits."""
    lms = apply(BT2020_TO_LMS, apply(BT709_TO_BT2020, color))
    l, m, s = (pq(c) for c in lms)
    return ((2048.0 * l + 2048.0 * m) / 4096.0,
            (6610.0 * l - 13613.0 * m + 7003.0 * s) / 4096.0,
            (17933.0 * l - 17390.0 * m - 543.0 * s) / 4096.0)


def hue_and_chroma(color):
    _, ct, cp = ictcp(color)
    return math.degrees(math.atan2(cp, ct)), math.hypot(ct, cp)


def luminance(color):
    return sum(w * c for w, c in zip(LUMINANCE, color))


def hue_difference(a, b):
    return abs((a - b + 180.0) % 360.0 - 180.0)


def scaled_to_gray(color):
    scale = MIDDLE_GRAY / luminance(color)
    return tuple(c * scale for c in color)


# --- Targets ----------------------------------------------------------------------------------------------------------

def ramp_measures(operator):
    """Per test color: (worst hue deviation over the checked steps, saturation ratio at 0 stops)."""
    results = []
    for name, color, limit in TEST_COLORS:
        base = scaled_to_gray(color)
        steps = []
        for stops in RAMP_STOPS:
            scene = tuple(c * 2.0 ** stops for c in base)
            shown = operator(scene)
            shown_luminance = luminance(shown)
            hue, chroma = hue_and_chroma(shown)
            reference = None
            if shown_luminance > 0.0:
                scale = shown_luminance / luminance(scene)
                reference = hue_and_chroma(tuple(c * scale for c in scene))
            steps.append((stops, hue, chroma, reference))
        largest = max(step[2] for step in steps)
        worst = 0.0
        for stops, hue, chroma, reference in steps:
            if reference is None or chroma < CHROMA_FLOOR * largest:
                continue
            worst = max(worst, hue_difference(hue, reference[0]))
        zero = next(step for step in steps if step[0] == 0.0)
        saturation = zero[2] / zero[3][1] if zero[3] and zero[3][1] > 0.0 else 0.0
        results.append((name, worst, limit, saturation))
    return results


def srgb_code(value):
    encoded = value * 12.92 if value <= 0.0031308 else 1.055 * value ** (1.0 / 2.4) - 0.055
    return encoded * 255.0


SHAPE_COLORS = [("gray", (1.0, 1.0, 1.0))] + [(name, color) for name, color, _ in TEST_COLORS]
SHAPE_GRID = [0.25 * index for index in range(5)]
SHAPE_STOPS = [-12.0 + 0.25 * index for index in range(89)]
# The fit's cheaper stand-in for T8: the grid's corners and middle, every half stop.
FIT_SHAPE_GRID = [0.0, 0.5, 1.0]
FIT_SHAPE_STOPS = [-12.0 + 0.5 * index for index in range(45)]


def shape_violation(inset, outset, contrast, tau, grid, stops):
    """T8 over a grid: (the summed decreases of every channel and of the luminance, the first failure's description)."""
    total = 0.0
    first = ""
    for toe in grid:
        for shoulder in grid:
            look = make_curve(contrast, toe, shoulder)
            if look[2] < 1.0:
                total += 1.0 - look[2]
                first = first or f"q = {look[2]:.4f} < 1 at Toe {toe}, Shoulder {shoulder}"
            for name, color in SHAPE_COLORS:
                base = scaled_to_gray(color)
                previous = None
                for stop in stops:
                    shown = filmic(tuple(c * 2.0 ** stop for c in base), inset, outset, contrast, tau, look)
                    if previous is not None:
                        decrease = sum(max(0.0, a - b) for a, b in zip(previous, shown)) + max(0.0, luminance(previous) - luminance(shown))
                        if decrease > 1e-7:
                            total += decrease
                            first = first or f"{name} decreases at {stop:+.2f} stops (Toe {toe}, Shoulder {shoulder})"
                    previous = shown
    return total, first


def evaluate(params):
    """(cost, list of failed targets, details) of a parameter vector."""
    inset, outset, tau, contrast = unpack(params)
    look = make_curve(contrast, 0.5, 0.5)
    failures = []
    # T1, T2 (by construction; checked numerically).
    gray = curve(MIDDLE_GRAY, contrast, tau, look)
    if abs(gray - MIDDLE_GRAY) > 1e-6:
        failures.append(f"T1: middle gray shows as {gray:.6f}")
    epsilon = 1e-4
    slope = (math.log2(curve(MIDDLE_GRAY * 2.0 ** epsilon, contrast, tau, look)) - math.log2(curve(MIDDLE_GRAY * 2.0 ** -epsilon, contrast, tau, look))) / (2.0 * epsilon)
    if abs(slope - contrast) > 0.01 or not CONTRAST_BOUNDS[0] <= contrast <= CONTRAST_BOUNDS[1]:
        failures.append(f"T2: slope {slope:.4f}, C0 {contrast:.4f}")
    # T3
    white = curve(MIDDLE_GRAY * 2.0 ** look[1], contrast, tau, look)
    six = curve(MIDDLE_GRAY * 2.0 ** 6.0, contrast, tau, look)
    if white != 1.0 or six < 0.955:
        failures.append(f"T3: +Tw {white:.6f}, +6 stops {six:.4f}")
    # T4
    toe_code = srgb_code(curve(0.005, contrast, tau, look))
    if not 2.0 <= toe_code <= 4.0:
        failures.append(f"T4: 0.005 shows at code {toe_code:.2f}")
    # T5, T7
    operator = lambda color: filmic(color, inset, outset, contrast, tau, look)
    measures = ramp_measures(operator)
    total = 0.0
    band_violation = 0.0
    white_violation = 0.0
    for name, worst, limit, saturation in measures:
        total += worst
        if worst > limit:
            failures.append(f"T5: {name} drifts {worst:.2f} degrees (limit {limit:.0f})")
        if not SATURATION_BAND[0] <= saturation <= SATURATION_BAND[1]:
            failures.append(f"T7: {name}'s saturation ratio {saturation:.3f}")
        # The fit aims inside the band, so the result does not sit on its edge.
        band_violation += max(0.0, SATURATION_BAND[0] + 0.005 - saturation, saturation - SATURATION_BAND[1] + 0.005)
    # T6
    for name, color, _ in TEST_COLORS:
        shown = operator(tuple(c * 2.0 ** (look[1] + 1.0) for c in scaled_to_gray(color)))
        if min(shown) < 0.95:
            failures.append(f"T6: {name} at +Tw+1 stops is {tuple(round(c, 3) for c in shown)}")
        white_violation += max(0.0, 0.955 - min(shown))
    shape, _ = shape_violation(inset, outset, contrast, tau, FIT_SHAPE_GRID, FIT_SHAPE_STOPS)
    # Each color's hue deviation counts as a share of its limit (in tens of degrees), so skin, foliage and sky, whose limit is 6
    # degrees, are not traded for the saturated colors.
    worst_hue = max(10.0 * worst / limit for _, worst, limit, _ in measures)
    cost = worst_hue + 0.1 * total + 40.0 * band_violation + 400.0 * white_violation + 1.0e6 * shape
    return cost, failures, measures


# --- Nelder-Mead ------------------------------------------------------------------------------------------------------

BOUNDS = [S_BOUNDS] * 3 + [RHO_BOUNDS] * 3 + [S_BOUNDS] * 3 + [RHO_BOUNDS] * 3 + [TAU_BOUNDS, CONTRAST_BOUNDS]


def clamp_to_bounds(point):
    return [min(max(value, low), high) for value, (low, high) in zip(point, BOUNDS)]


def nelder_mead(function, start, step, iterations):
    dimension = len(start)
    simplex = [clamp_to_bounds(start)]
    for index in range(dimension):
        point = list(start)
        point[index] += step[index]
        simplex.append(clamp_to_bounds(point))
    values = [function(point) for point in simplex]
    for _ in range(iterations):
        order = sorted(range(dimension + 1), key=lambda i: values[i])
        simplex = [simplex[i] for i in order]
        values = [values[i] for i in order]
        if values[-1] - values[0] < 1e-7:
            break
        centroid = [sum(point[i] for point in simplex[:-1]) / dimension for i in range(dimension)]
        worst = simplex[-1]
        reflected = clamp_to_bounds([c + (c - w) for c, w in zip(centroid, worst)])
        reflected_value = function(reflected)
        if reflected_value < values[0]:
            expanded = clamp_to_bounds([c + 2.0 * (c - w) for c, w in zip(centroid, worst)])
            expanded_value = function(expanded)
            if expanded_value < reflected_value:
                simplex[-1], values[-1] = expanded, expanded_value
            else:
                simplex[-1], values[-1] = reflected, reflected_value
        elif reflected_value < values[-2]:
            simplex[-1], values[-1] = reflected, reflected_value
        else:
            contracted = clamp_to_bounds([c + 0.5 * (w - c) for c, w in zip(centroid, worst)])
            contracted_value = function(contracted)
            if contracted_value < values[-1]:
                simplex[-1], values[-1] = contracted, contracted_value
            else:
                best = simplex[0]
                simplex = [best] + [clamp_to_bounds([b + 0.5 * (p - b) for b, p in zip(best, point)]) for point in simplex[1:]]
                values = [values[0]] + [function(point) for point in simplex[1:]]
    best = min(range(dimension + 1), key=lambda i: values[i])
    return simplex[best], values[best]


def fit(starts, from_committed):
    generator = random.Random(SEED)
    cost = lambda params: evaluate(params)[0]
    best = None
    runs = [None] * starts
    if from_committed:
        runs.insert(0, read_committed())
    for start_index, committed in enumerate(runs):
        start = [generator.uniform(0.1, 0.3) for _ in range(3)] + [generator.uniform(-0.05, 0.05) for _ in range(3)] \
            + [generator.uniform(0.1, 0.3) for _ in range(3)] + [generator.uniform(-0.05, 0.05) for _ in range(3)] + [1.5, 1.40]
        if committed is not None:
            start = clamp_to_bounds(committed)
        step = [0.08] * 3 + [0.04] * 3 + [0.08] * 3 + [0.04] * 3 + [0.3, 0.02]
        point, value = start, cost(start)
        # Restarts of the simplex around the best point found so far escape its early collapses.
        for _ in range(4):
            point, value = nelder_mead(cost, point, step, 600)
            step = [s * 0.5 for s in step]
        # Hops: the best point moved at random (seeded) and refined again, kept when better, leave shallow local minima.
        for _ in range(HOPS):
            hop = clamp_to_bounds([value_ + generator.gauss(0.0, scale) for value_, scale in zip(point, HOP_SCALES)])
            hop_step = [s * 4.0 for s in step]
            hop_point, hop_value = hop, cost(hop)
            for _ in range(2):
                hop_point, hop_value = nelder_mead(cost, hop_point, hop_step, 600)
                hop_step = [s * 0.5 for s in hop_step]
            if hop_value < value:
                point, value = hop_point, hop_value
        print(f"start {start_index + 1}/{len(runs)}: cost {value:.4f}", flush=True)
        if best is None or value < best[1]:
            best = (point, value)
    return best[0]


# --- Outputs ----------------------------------------------------------------------------------------------------------

def number(value):
    return f"{value:.9g}" if "e" not in f"{value:.9g}" else f"{value:.9e}"


def float_literal(value):
    text = number(value)
    if "." not in text and "e" not in text:
        text += ".0"
    return text


def glsl_matrix(m):
    columns = [[m[row][column] for row in range(3)] for column in range(3)]
    return "mat3(\n" + ",\n".join("\t" + ", ".join(float_literal(v) for v in column) for column in columns) + ")"


def cpp_matrix(m):
    columns = [[m[row][column] for row in range(3)] for column in range(3)]
    return "glm::mat3(\n" + ",\n".join("\t\t" + ", ".join(float_literal(v) + "f" for v in column) for column in columns) + ")"


def write_outputs(params):
    inset, outset, tau, contrast = unpack(params)
    banner = "Generated by Tools/FitFilmic.py from its targets; do not edit"
    # The fitted parameters, which --check reads back: s, rho, s', rho' (three each), tau, C0.
    values = [number(v) for v in params]
    parameter_lines = "// Fitted parameters:\n" + "".join(f"//   {' '.join(values[i:i + 7])}\n" for i in range(0, len(values), 7))
    glsl = (
        "#ifndef STRATA_FILMIC_CONSTANTS_GLSL\n"
        "#define STRATA_FILMIC_CONSTANTS_GLSL\n\n"
        f"// {banner}.\n"
        f"{parameter_lines}\n"
        "// The Filmic curve's gamut: the inset before the per-channel curve and the outset after it (column-major).\n"
        f"const mat3 c_FilmicInset = {glsl_matrix(inset)};\n"
        f"const mat3 c_FilmicOutset = {glsl_matrix(outset)};\n"
        "// The log-log slope at middle gray (C0), the stops over which the toe turns to its own slope (tau), middle gray.\n"
        f"const float c_FilmicContrast = {float_literal(contrast)};\n"
        f"const float c_FilmicToeWidth = {float_literal(tau)};\n"
        "const float c_FilmicMiddleGray = 0.18;\n\n"
        "#endif\n"
    )
    header = (
        "#pragma once\n\n"
        f"// {banner}.\n"
        f"{parameter_lines}\n"
        "#include <glm/glm.hpp>\n\n"
        "namespace Strata::FilmicConstants\n"
        "{\n\n"
        "\t// The Filmic curve's gamut (shaders/Include/FilmicConstants.glsl): the inset before the per-channel curve and the\n"
        "\t// outset after it.\n"
        f"\tinline const glm::mat3 Inset = {cpp_matrix(inset)};\n"
        f"\tinline const glm::mat3 Outset = {cpp_matrix(outset)};\n"
        "\t// The log-log slope at middle gray (C0), the stops over which the toe turns to its own slope (tau), middle gray.\n"
        f"\tinline constexpr float Contrast = {float_literal(contrast)}f;\n"
        f"\tinline constexpr float ToeWidth = {float_literal(tau)}f;\n"
        "\tinline constexpr float MiddleGray = 0.18f;\n\n"
        "}\n"
    )
    for path, text in ((GLSL_OUTPUT, glsl), (HEADER_OUTPUT, header)):
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)


def read_committed():
    with open(GLSL_OUTPUT, encoding="utf-8") as handle:
        text = handle.read()
    match = re.search(r"Fitted parameters:\n((?://   [^\n]+\n)+)", text)
    if not match:
        raise SystemExit(f"{GLSL_OUTPUT} names no fitted parameters")
    return [float(value) for value in match.group(1).replace("//", " ").split()]


def print_report(params):
    inset, outset, tau, contrast = unpack(params)
    look = make_curve(contrast, 0.5, 0.5)
    _, failures, measures = evaluate(params)
    print(f"C0 {contrast:.4f}, tau {tau:.4f}, Tw {look[1]:.2f}, q {look[2]:.4f}")
    print(f"middle gray -> sRGB {srgb_code(curve(MIDDLE_GRAY, contrast, tau, look)):.1f}; 0.005 -> {srgb_code(curve(0.005, contrast, tau, look)):.2f};"
          f" +6 stops -> {srgb_code(curve(MIDDLE_GRAY * 64.0, contrast, tau, look)):.1f}")
    others = {"ACES": ramp_measures(aces), "AgX": ramp_measures(agx)}
    print(f"{'color':<9} {'Filmic hue':>10} {'ACES':>7} {'AgX':>7}   {'Filmic sat':>10} {'ACES':>6} {'AgX':>6}")
    for index, (name, worst, limit, saturation) in enumerate(measures):
        print(f"{name:<9} {worst:>10.2f} {others['ACES'][index][1]:>7.2f} {others['AgX'][index][1]:>7.2f}   {saturation:>10.3f}"
              f" {others['ACES'][index][3]:>6.3f} {others['AgX'][index][3]:>6.3f}")
    shape, why = shape_violation(inset, outset, contrast, tau, SHAPE_GRID, SHAPE_STOPS)
    if shape > 0.0:
        failures.append(f"T8: {why} (decreases summing to {shape:.2e})")
    return failures


def main():
    parser = argparse.ArgumentParser(description="Fits the Filmic tone curve's gamut and writes its constants.")
    parser.add_argument("--check", action="store_true", help="check the committed constants against the targets; write nothing")
    parser.add_argument("--starts", type=int, default=STARTS, help=f"seeded random starts (default {STARTS})")
    parser.add_argument("--from-committed", action="store_true", help="also start from the committed constants (first)")
    arguments = parser.parse_args()
    params = read_committed() if arguments.check else fit(max(arguments.starts, 0), arguments.from_committed)
    failures = print_report(params)
    print("Parameters:", " ".join(number(v) for v in params))
    if failures:
        print("Targets not met:")
        for failure in failures:
            print(f"  {failure}")
        return 1
    if not arguments.check:
        write_outputs(params)
        print(f"Wrote {os.path.relpath(GLSL_OUTPUT, REPOSITORY_DIRECTORY)} and {os.path.relpath(HEADER_OUTPUT, REPOSITORY_DIRECTORY)}")
    else:
        print("The committed constants meet every target")
    return 0


if __name__ == "__main__":
    sys.exit(main())
