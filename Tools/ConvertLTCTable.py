#!/usr/bin/env python3
"""Converts the tables rect lights are shaded with (Renderer/LTCTable.inl) from the published fit.

The output is committed (CI never runs the conversion); run it again only to take the tables from another upstream commit:

    python -I Tools/ConvertLTCTable.py <ltc_code>/fit/results/ltc.js Strata/src/Strata/Renderer/LTCTable.inl
    python -I Tools/ConvertLTCTable.py --check <ltc_code>/fit/results/ltc.js Strata/src/Strata/Renderer/LTCTable.inl
    python -I Tools/ConvertLTCTable.py selftest

Rect lights are shaded with linearly transformed cosines (Heitz, Dupuy, Hill and Neubelt, "Real-Time Polygonal-Light Shading
with Linearly Transformed Cosines", ACM Transactions on Graphics 35(4), 2016): a GGX lobe is a clamped cosine under a linear
transform of directions, so its integral over a polygon is the cosine's integral over the transformed polygon, which has a
closed form. The transforms are fitted per roughness and view angle; the authors publish the fit's results for GGX with the
height-correlated Smith term (the BRDF of Strata's PBR.slang) as fit/results/ltc.js of https://github.com/selfshadow/ltc_code.
The fit is not run again here: it is a Nelder-Mead search per cell, hours in standard-library Python, and the published
results are the reference every implementation is compared with.

The conversion changes no value: every number keeps its decimal text (a float suffix is added). The script refuses a source
that is not the file of the pinned commit (SOURCE_SHA256), so the header's commit cannot go stale: taking another commit
means changing SOURCE_COMMIT, SOURCE_SHA256 and VALUES_SHA256 together, and reading the license again.

Layout (packTab and genSphereTab of fit/fitLTC.cpp): two arrays of 64 x 64 texels of four floats, texel (a, t) at entry
a + t * 64 for the perceptual roughness a / 63 (the fit's alpha is its square) and the view parameter
sqrt(1 - cos(theta_v)) = t / 63.
    g_ltc_1: the inverse transform over its middle term (packTab's invM[0][0], [0][2], [2][0], [2][2] over [1][1], indexed
             by column, then row): a texel (x, y, z, w) is the matrix of rows (x, 0, z), (0, 1, 0), (y, 0, w). At t = 0
             (the view along the normal) the lobe is isotropic, so x is 1, and y and z are 0.
    g_ltc_2: x the lobe's magnitude (its directional albedo for F0 = 1), y its Fresnel term, z unused (0), w the projected
             solid angle of a sphere cap clipped by the horizon over pi times its form factor's length, for
             (the form factor's z * 0.5 + 0.5, its length): row 0 (length 0) is max(z, 0), and nothing is brighter than 1.

`selftest` (the CTest StrataTools.LTCTable) needs no upstream file: it checks that the committed table is what this script
writes for the pinned values, that the layout's properties above hold, and that the parser refuses malformed sources.

The upstream file is data, never code: it is read as text and every number must match a decimal pattern. Only the standard
library is used; a run writes the same bytes every time.
"""

import argparse
import hashlib
import math
import os
import re
import sys

REPOSITORY_DIRECTORY = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTPUT = os.path.join(REPOSITORY_DIRECTORY, "Strata", "src", "Strata", "Renderer", "LTCTable.inl")

SOURCE_REPOSITORY = "https://github.com/selfshadow/ltc_code"
SOURCE_COMMIT = "31e5e96b54f98f33098f8503003119ba2231a1c6"
SOURCE_FILE = "fit/results/ltc.js"
# SHA-256 of fit/results/ltc.js at SOURCE_COMMIT, and of the tables' numbers as upstream wrote them (digest_values).
SOURCE_SHA256 = "21071160163defd419b8f754ab604a8e60c44fcf539bfff9916e0e8e82316c1d"
VALUES_SHA256 = "d1b80368837553c0d608d41068b2c8bfd5a1054616697d0ae730d51c556c6606"

ARRAYS = ("g_ltc_1", "g_ltc_2")
SIZE = 64
CHANNELS = 4
VALUES_PER_LAYER = SIZE * SIZE * CHANNELS
MAX_SOURCE_BYTES = 4 * 1024 * 1024

# The largest finite float and the smallest normal one: every value must be written exactly enough as a 32-bit float.
FLOAT_MAX = 3.4028234663852886e38
FLOAT_MIN_NORMAL = 1.1754943508222875e-38

NUMBER = re.compile(r"-?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][-+]?[0-9]+)?")

# The license of ltc_code (its LICENSE file at SOURCE_COMMIT), word for word: it asks for the notice in source and binary
# redistributions and for a reference to the paper.
LICENSE = """Copyright (c) 2017, Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* If you use (or adapt) the source code in your own work, please include a
  reference to the paper:

  Real-Time Polygonal-Light Shading with Linearly Transformed Cosines.
  Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt.
  ACM Transactions on Graphics (Proceedings of ACM SIGGRAPH 2016) 35(4), 2016.
  Project page: https://eheitzresearch.wordpress.com/415-2/

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."""

DECLARATION = f"constexpr float c_LTCTable[{len(ARRAYS)}][{SIZE} * {SIZE} * {CHANNELS}] = {{"


class ConversionError(Exception):
    """A source or a table that is not what the conversion expects."""


def check_values(tokens, what):
    """Refuses a layer whose numbers are not VALUES_PER_LAYER decimal numbers a 32-bit float holds."""
    if len(tokens) != VALUES_PER_LAYER:
        raise ConversionError(f"{what} holds {len(tokens)} values, not {VALUES_PER_LAYER}")
    for index, token in enumerate(tokens):
        if not NUMBER.fullmatch(token):
            raise ConversionError(f"{what}: value {index} ({token[:40]!r}) is not a decimal number")
        value = float(token)
        if not math.isfinite(value) or abs(value) > FLOAT_MAX:
            raise ConversionError(f"{what}: value {index} ({token}) is not a finite 32-bit float")
        if value != 0.0 and abs(value) < FLOAT_MIN_NORMAL:
            raise ConversionError(f"{what}: value {index} ({token}) is below the smallest normal 32-bit float")


def parse_source(text):
    """The two arrays of an ltc.js as lists of number texts (upstream's own digits), in ARRAYS' order."""
    layers = []
    for name in ARRAYS:
        found = re.findall(r"\bvar\s+" + name + r"\s*=\s*\[([^\[\]]*)\]\s*;", text)
        if len(found) != 1:
            raise ConversionError(f"the source declares {name} {len(found)} times, not once")
        tokens = [token.strip() for token in found[0].split(",")]
        # Upstream ends every line, also the last, with a comma.
        if tokens and tokens[-1] == "":
            tokens.pop()
        check_values(tokens, name)
        layers.append(tokens)
    return layers


def check_layout(layers):
    """Refuses tables without the properties the shading relies on (the module's description says where they come from)."""
    transforms = [float(token) for token in layers[0]]
    terms = [float(token) for token in layers[1]]
    for entry in range(SIZE * SIZE):
        m00, m02, m20, m22 = transforms[entry * CHANNELS:(entry + 1) * CHANNELS]
        if m22 <= 0.0:
            raise ConversionError(f"{ARRAYS[0]}: entry {entry} has m22 = {m22}, not above 0")
        if entry < SIZE and (m00 != 1.0 or m02 != 0.0 or m20 != 0.0):
            raise ConversionError(f"{ARRAYS[0]}: entry {entry} (the view along the normal) is not isotropic: {m00}, {m02}, {m20}")
        magnitude, fresnel, unused, sphere = terms[entry * CHANNELS:(entry + 1) * CHANNELS]
        if not 0.0 < magnitude <= 1.0 or not 0.0 <= fresnel <= magnitude:
            raise ConversionError(f"{ARRAYS[1]}: entry {entry} has magnitude {magnitude} and Fresnel term {fresnel}")
        if unused != 0.0:
            raise ConversionError(f"{ARRAYS[1]}: entry {entry} has z = {unused}, not 0")
        if not 0.0 <= sphere <= 1.0:
            raise ConversionError(f"{ARRAYS[1]}: entry {entry} has a sphere term of {sphere}, outside [0, 1]")
        # Row 0 of the sphere table: a cap of no size is a point light, whose clipped cosine is max(z, 0).
        if entry < SIZE:
            expected = max(entry / (SIZE - 1) * 2.0 - 1.0, 0.0)
            if abs(sphere - expected) > 1e-5:
                raise ConversionError(f"{ARRAYS[1]}: entry {entry} has a sphere term of {sphere}, not {expected}")


def digest_values(layers):
    """SHA-256 of the tables' numbers as upstream wrote them, a line each, layer after layer."""
    return hashlib.sha256("".join(token + "\n" for layer in layers for token in layer).encode("ascii")).hexdigest()


def format_value(token):
    """A number of the source as a C++ float literal with the same digits."""
    if "." not in token and "e" not in token and "E" not in token:
        token += ".0"
    return token + "f"


def render(layers):
    """The .inl's text for the tables."""
    lines = [
        "// Generated by Tools/ConvertLTCTable.py; do not edit.",
        "// The tables rect lights are shaded with (Renderer/AreaLights.h): linearly transformed cosines fitted to GGX with the",
        f"// height-correlated Smith term. Two layers of {SIZE} x {SIZE} texels of four floats, a texel per line, row by row: texel (a, t)",
        f"// is entry a + t * {SIZE}, for the perceptual roughness a / {SIZE - 1} and the view parameter sqrt(1 - cos(theta_v)) = t / {SIZE - 1}.",
        "//   Layer 0: the inverse transform over its middle term: a texel (x, y, z, w) is the matrix of rows (x, 0, z), (0, 1, 0),",
        "//            (y, 0, w).",
        "//   Layer 1: x the lobe's magnitude, y its Fresnel term, z unused (0), w the projected solid angle of a sphere cap clipped",
        "//            by the horizon over pi times its form factor's length, for (the form factor's z * 0.5 + 0.5, its length).",
        "//",
        f"// The values are those of {SOURCE_FILE} ({ARRAYS[0]}, {ARRAYS[1]}) of {SOURCE_REPOSITORY}",
        f"// at commit {SOURCE_COMMIT}, digit for digit. That project's license:",
        "//",
    ]
    lines += [("//   " + line).rstrip() for line in LICENSE.split("\n")]
    lines += ["", DECLARATION]
    for layer in layers:
        lines.append("\t{")
        for start in range(0, VALUES_PER_LAYER, CHANNELS):
            lines.append("\t\t" + ", ".join(format_value(token) for token in layer[start:start + CHANNELS]) + ",")
        lines.append("\t},")
    lines.append("};")
    return "\n".join(lines) + "\n"


def parse_table(text):
    """The number texts of an .inl this script wrote, per layer, as the source had them."""
    lines = text.split("\n")
    try:
        start = lines.index(DECLARATION)
    except ValueError:
        raise ConversionError("the table does not declare c_LTCTable as this script writes it") from None
    layers = []
    line = start + 1
    for name in ARRAYS:
        if line >= len(lines) or lines[line] != "\t{":
            raise ConversionError(f"the table's layer of {name} does not start where it should (line {line + 1})")
        line += 1
        tokens = []
        while line < len(lines) and lines[line] != "\t},":
            for literal in lines[line].strip().rstrip(",").split(", "):
                if not literal.endswith("f"):
                    raise ConversionError(f"line {line + 1} holds {literal[:40]!r}, not a float literal")
                tokens.append(literal[:-1])
            line += 1
        line += 1
        # format_value's ".0" is the only thing a literal has that the source's number had not.
        tokens = [token[:-2] if token.endswith(".0") and NUMBER.fullmatch(token[:-2]) else token for token in tokens]
        check_values(tokens, f"the table's layer of {name}")
        layers.append(tokens)
    return layers


def read_text(path, limit):
    """A file's text, refusing one that is larger than anything this script reads."""
    try:
        size = os.path.getsize(path)
        if size > limit:
            raise ConversionError(f"{path} is {size} bytes, more than the {limit} this script reads")
        with open(path, "rb") as handle:
            data = handle.read(limit + 1)
    except OSError as error:
        raise ConversionError(f"cannot read {path}: {error}") from None
    try:
        return data, data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ConversionError(f"{path} is not UTF-8: {error}") from None


def convert(source_path):
    """The .inl's text for the pinned upstream file at `source_path`."""
    data, text = read_text(source_path, MAX_SOURCE_BYTES)
    digest = hashlib.sha256(data).hexdigest()
    if digest != SOURCE_SHA256:
        raise ConversionError(
            f"{source_path} is not {SOURCE_FILE} of commit {SOURCE_COMMIT} (SHA-256 {digest}, expected {SOURCE_SHA256}): "
            "to take another commit, change SOURCE_COMMIT, SOURCE_SHA256 and VALUES_SHA256 together")
    layers = parse_source(text)
    check_layout(layers)
    digest = digest_values(layers)
    if digest != VALUES_SHA256:
        raise ConversionError(f"the source's values have the SHA-256 {digest}, not VALUES_SHA256 ({VALUES_SHA256})")
    text = render(layers)
    # The table must read back as the source's own digits: selftest checks the committed file that way.
    if parse_table(text) != layers:
        raise ConversionError("the table does not read back as the source's numbers")
    return text


def expect_refused(text, what):
    """Fails unless parse_source refuses `text`."""
    try:
        parse_source(text)
    except ConversionError:
        return
    raise ConversionError(f"the parser accepted a source with {what}")


def selftest():
    """Checks the committed table and the parser without an upstream file."""
    data, text = read_text(OUTPUT, 4 * MAX_SOURCE_BYTES)
    if b"\r" in data:
        raise ConversionError(f"{OUTPUT} has carriage returns: the table is committed with LF line endings")
    layers = parse_table(text)
    digest = digest_values(layers)
    if digest != VALUES_SHA256:
        raise ConversionError(f"the committed table's values have the SHA-256 {digest}, not VALUES_SHA256 ({VALUES_SHA256})")
    if render(layers) != text:
        raise ConversionError("the committed table is not what this script writes for its values (header, layout or digits)")
    check_layout(layers)

    # The parser reads back a source built the way upstream writes it, and refuses malformed ones.
    def build(first, second):
        def lines(tokens):
            return "".join(", ".join(tokens[start:start + CHANNELS]) + ", \n" for start in range(0, len(tokens), CHANNELS))
        return f"var {ARRAYS[0]} = [\n{lines(first)}];\nvar {ARRAYS[1]} = [{lines(second)}];\n"

    if parse_source(build(layers[0], layers[1])) != layers:
        raise ConversionError("the parser does not read back the source it was given")
    expect_refused(build(layers[0][:-1], layers[1]), "a missing value")
    expect_refused(build(layers[0] + ["0"], layers[1]), "a value too many")
    expect_refused(build(layers[0], layers[1]).replace(f"var {ARRAYS[1]}", "var g_other"), "a missing array")
    expect_refused(build(layers[0], layers[1]) + build(layers[0], layers[1]), "arrays declared twice")
    for bad in ("nan", "inf", "1e999", "1e-45", "0x10", "1f", "alert(1)", "", "1 2", "[1]"):
        expect_refused(build(layers[0][:-1] + [bad], layers[1]), f"the value {bad!r}")

    # check_layout refuses what the shading cannot use.
    def expect_layout_refused(changed, what):
        try:
            check_layout(changed)
        except ConversionError:
            return
        raise ConversionError(f"the layout check accepted {what}")

    expect_layout_refused([["2"] + layers[0][1:], layers[1]], "a transform that is not isotropic along the normal")
    expect_layout_refused([layers[0][:3] + ["0"] + layers[0][4:], layers[1]], "an m22 of 0")
    expect_layout_refused([layers[0], layers[1][:2] + ["0.5"] + layers[1][3:]], "a z that is not 0")
    expect_layout_refused([layers[0], layers[1][:-1] + ["1.5"]], "a sphere term above 1")
    print(f"LTCTable.inl: {len(ARRAYS)} layers of {SIZE} x {SIZE} texels, values {digest[:16]}... of {SOURCE_FILE} at {SOURCE_COMMIT[:8]}")


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "selftest":
        selftest()
        return
    parser = argparse.ArgumentParser(description="Converts ltc_code's fit results to Strata's LTC table (or: selftest).")
    parser.add_argument("source", help=f"{SOURCE_FILE} of {SOURCE_REPOSITORY} at commit {SOURCE_COMMIT[:8]}")
    parser.add_argument("output", nargs="?", default=OUTPUT, help="the table to write (default: Renderer/LTCTable.inl)")
    parser.add_argument("--check", action="store_true", help="fail when the table differs from what the conversion writes")
    arguments = parser.parse_args()
    text = convert(arguments.source)
    if arguments.check:
        committed = read_text(arguments.output, 4 * MAX_SOURCE_BYTES)[1]
        if committed != text:
            raise ConversionError(f"{arguments.output} is not what the conversion writes: run the script without --check")
        print(f"{arguments.output} is up to date")
        return
    try:
        with open(arguments.output, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
    except OSError as error:
        raise ConversionError(f"cannot write {arguments.output}: {error}") from None
    print(f"Wrote {arguments.output} ({len(ARRAYS)} layers of {SIZE} x {SIZE} texels, {len(text)} bytes)")


if __name__ == "__main__":
    try:
        main()
    except ConversionError as error:
        raise SystemExit(f"ConvertLTCTable: {error}") from None
