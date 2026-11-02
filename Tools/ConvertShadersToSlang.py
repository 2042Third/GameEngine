#!/usr/bin/env python3
"""Converts Strata/shaders from GLSL to HLSL-syntax Slang, in place (lane Slang of wave 8, Docs/DeveloperGuide.md "Shaders").

    python -I Tools/ConvertShadersToSlang.py [--listing <file outside the tree>]

It ran once, for the conversion commit, and left the repository in the next commit; history keeps it so that a tree of GLSL
shaders can be converted again (git show <that commit>:Tools/ConvertShadersToSlang.py). Not a parser of GLSL: a walker
over tokens with the type knowledge this tree needs. It converts every stage file and include, deletes the GLSL files,
writes Include/Common.slang's binding comment and the section the conversion needs, runs the two generators whose outputs
are shaders (Tools/FitFilmic.py --rewrite, Tools/FitAOMultiBounce.py --write) and applies the hand fixes, each of which
must match exactly once (else nothing is written). --listing writes every rewrite of the kinds that can be silently wrong
(mul, matrix indices and constructors, select, atomics with a hoisted value, vector comparisons, the hand fixes) as
`file:line: before -> after`, for review line by line. Standard library only.
"""
import argparse
import collections
import os
import re
import subprocess
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parent.parent
SHADERS = str(REPOSITORY / "Strata" / "shaders").replace("\\", "/")
# Written by their generators instead of converted (their GLSL is read for its matrices' names only).
GENERATED = {"Include/FilmicConstants.glsl": ["Tools/FitFilmic.py", "--rewrite"],
             "Include/AmbientOcclusionFit.glsl": ["Tools/FitAOMultiBounce.py", "--write"]}
DROPPED = "\x00dropped directive\x00"


class ToolError(Exception):
    pass


TOKEN = re.compile(
    r"(?P<comment>//[^\n]*|/\*.*?\*/)"
    r"|(?P<preprocessor>^[ \t]*\#(?:\\\n|[^\n])*)"
    r"|(?P<identifier>[A-Za-z_][A-Za-z_0-9]*)"
    r"|(?P<number>0[xX][0-9a-fA-F]+[uU]?|(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?[uUfF]?)"
    r"|(?P<space>[ \t\r\n]+)"
    r"|(?P<operator><<=|>>=|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||\+=|-=|\*=|/=|%=|&=|\|=|\^=|.)",
    re.MULTILINE | re.DOTALL)

Token = collections.namedtuple("Token", "kind text")

TYPES = {
    "vec2": "float2", "vec3": "float3", "vec4": "float4",
    "ivec2": "int2", "ivec3": "int3", "ivec4": "int4",
    "uvec2": "uint2", "uvec3": "uint3", "uvec4": "uint4",
    "bvec2": "bool2", "bvec3": "bool3", "bvec4": "bool4",
    "mat2": "float2x2", "mat3": "float3x3", "mat4": "float4x4",
}
TEXTURES = {
    "texture2D": "Texture2D<float4>", "utexture2D": "Texture2D<uint4>",
    "texture2DArray": "Texture2DArray<float4>", "utexture2DArray": "Texture2DArray<uint4>",
    "textureCube": "TextureCube<float4>", "textureCubeArray": "TextureCubeArray<float4>",
    "texture3D": "Texture3D<float4>",
}
SAMPLERS = {"sampler": "SamplerState", "samplerShadow": "SamplerComparisonState"}
IMAGES = {
    "image2D": ("RWTexture2D", "float4"), "image2DArray": ("RWTexture2DArray", "float4"), "image3D": ("RWTexture3D", "float4"),
    "uimage2D": ("RWTexture2D", "uint4"), "uimage2DArray": ("RWTexture2DArray", "uint4"),
}
COMBINED = {"sampler2D", "sampler2DArray", "samplerCube", "samplerCubeArray", "sampler3D", "sampler2DShadow", "sampler2DArrayShadow"}
RENAMES = {
    "fract": "frac", "dFdx": "ddx", "dFdy": "ddy", "dFdxFine": "ddx_fine", "dFdyFine": "ddy_fine", "dFdxCoarse": "ddx_coarse",
    "dFdyCoarse": "ddy_coarse", "uintBitsToFloat": "asfloat", "floatBitsToUint": "asuint", "floatBitsToInt": "asint",
    "intBitsToFloat": "asfloat", "nonuniformEXT": "NonUniformResourceIndex", "shared": "groupshared",
    "unpackUnorm4x8": "UnpackUnorm4x8", "bitfieldReverse": "reversebits", "packHalf2x16": "PackHalf2x16", "unpackHalf2x16": "UnpackHalf2x16",
    "gl_FragCoord": "st_FragCoord", "gl_FrontFacing": "st_FrontFacing", "gl_FragDepth": "st_FragDepth",
    "gl_Position": "st_Position", "gl_VertexIndex": "st_VertexIndex", "gl_InstanceIndex": "st_InstanceIndex",
    "gl_GlobalInvocationID": "st_GlobalInvocationID", "gl_LocalInvocationID": "st_LocalInvocationID",
    "gl_WorkGroupID": "st_WorkGroupID", "gl_LocalInvocationIndex": "st_LocalInvocationIndex",
}
ATOMICS = {"atomicAdd": "InterlockedAdd", "atomicMax": "InterlockedMax", "atomicMin": "InterlockedMin", "atomicOr": "InterlockedOr",
           "atomicAnd": "InterlockedAnd", "atomicExchange": "InterlockedExchange"}
COMPARISONS = {"lessThan": "<", "lessThanEqual": "<=", "greaterThan": ">", "greaterThanEqual": ">=", "equal": "==", "notEqual": "!="}
ELEMENT_OF_FORMAT = {"r32ui": "uint4", "rg32ui": "uint4", "rgba32ui": "uint4", "r16ui": "uint4", "rg16ui": "uint4", "r8ui": "uint4"}


def tokenize(text):
    return [Token(match.lastgroup, match.group(0)) for match in TOKEN.finditer(text)]


def significant(tokens, index):
    while index < len(tokens) and tokens[index].kind in ("space", "comment"):
        index += 1
    return index


def significant_back(tokens, index):
    while index >= 0 and tokens[index].kind in ("space", "comment"):
        index -= 1
    return index


def match_close(tokens, index):
    opening = tokens[index].text
    closing = {"(": ")", "[": "]", "{": "}"}[opening]
    depth = 0
    for position in range(index, len(tokens)):
        if tokens[position].kind != "operator":
            continue
        if tokens[position].text == opening:
            depth += 1
        elif tokens[position].text == closing:
            depth -= 1
            if depth == 0:
                return position
    raise ValueError("unbalanced " + opening)


def split_arguments(tokens):
    arguments = [[]]
    depth = 0
    for token in tokens:
        if token.kind == "operator" and token.text in "([{":
            depth += 1
        elif token.kind == "operator" and token.text in ")]}":
            depth -= 1
        if token.kind == "operator" and token.text == "," and depth == 0:
            arguments.append([])
        else:
            arguments[-1].append(token)
    if len(arguments) == 1 and not any(t.kind not in ("space", "comment") for t in arguments[0]):
        return []
    return arguments


def strip(tokens):
    start = significant(tokens, 0)
    end = significant_back(tokens, len(tokens) - 1)
    return tokens[start:end + 1]


class Knowledge:
    """What the tree declares: resources' types per file, the names that are matrices."""

    def __init__(self, sources):
        self.sources = sources
        self.resources = {}
        self.includes = {}
        self.matrices = set()
        self.matrix_arrays = set()
        self.matrix_functions = set()
        for path, text in sources.items():
            self.includes[path] = re.findall(r'#include "([^"]+)"', text)
            self.resources[path] = re.findall(r"uniform\s+(?:writeonly\s+|readonly\s+)?(u?texture\w+|u?image\w+)\s+(\w+)", text)
            self.resources[path] += [(kind, name) for kind, name in re.findall(r"\b(?:writeonly|readonly)\s+(?:uniform\s+)?(u?image\w+)\s+(\w+)", text)]
            for name, array in re.findall(r"\bmat[234]\s+(\w+)\s*(\[)?", text):
                self.matrices.add(name)
                if array:
                    self.matrix_arrays.add(name)
            for name in re.findall(r"^\s*(?:const\s+)?mat[234]\s+(\w+)\s*\(", text, re.MULTILINE):
                self.matrix_functions.add(name)
                self.matrices.discard(name)
            self.matrices.update(re.findall(r"\bmat[234]\s+(\w+)\s*=", text))

    def types_in(self, path, seen=None):
        seen = seen if seen is not None else set()
        if path in seen or path not in self.sources:
            return []
        seen.add(path)
        found = list(self.resources[path])
        for include in self.includes[path]:
            found += self.types_in(include, seen)
        return found


class Converter:
    def __init__(self, path, knowledge):
        self.path = path
        self.knowledge = knowledge
        self.resource_types = {}
        for kind, name in knowledge.types_in(path):
            self.resource_types.setdefault(name, set()).add(kind)
        self.hoists = 0
        self.stats = collections.Counter()
        self.hand = []
        self.listing = []  # (kind, GLSL text, Slang text) of the rewrites that can be silently wrong

    # --- Expressions -----------------------------------------------------------------------------------------------------

    def text(self, tokens):
        return self.rewrite(tokens).strip()

    def combined(self, tokens):
        """The texture and sampler expressions of sampler2D(texture, sampler)."""
        stripped = strip(tokens)
        if stripped and stripped[0].kind == "identifier" and stripped[0].text in COMBINED:
            open_index = significant(stripped, 1)
            close = match_close(stripped, open_index)
            parts = split_arguments(stripped[open_index + 1:close])
            return stripped[0].text, self.text(parts[0]), self.text(parts[1])
        return None, self.text(tokens), None

    def fetch_dimensions(self, name):
        kinds = self.resource_types.get(name, set())
        if len(kinds) == 1:
            kind = next(iter(kinds))
            if "2DArray" in kind or "3D" in kind:
                return "int4"
            return "int3"
        return None

    def original(self, tokens, index):
        """The GLSL text of the call whose name is tokens[index]."""
        following = significant(tokens, index + 1)
        return "".join(t.text for t in tokens[index:match_close(tokens, following) + 1])

    def call(self, name, arguments, tokens, index):
        """The replacement of a call, or None when it stays a call (arguments rewritten)."""
        self.stats[name] += 1
        if name == "texelFetch":
            _, texture, _ = self.combined(arguments[0])
            position = self.text(arguments[1])
            level = self.text(arguments[2])
            vector = self.fetch_dimensions(texture.split("[")[0])
            if vector is None:
                return f"TexelFetch({texture}, {position}, {level})"
            return f"{texture}.Load({vector}({position}, {level}))"
        if name in ("texture", "textureLod", "textureGrad", "textureGather"):
            kind, texture, sampler = self.combined(arguments[0])
            if kind is None:
                self.hand.append(f"{name} without a combined sampler")
                return None
            uv = strip(arguments[1])
            uv_text = self.text(arguments[1])
            rest = [self.text(a) for a in arguments[2:]]
            if kind in ("sampler2DShadow", "sampler2DArrayShadow"):
                # The reference is the coordinate's last component: vec3(tap, z), vec4(uv, layer, z).
                constructor = uv_text[:uv_text.index("(")]
                inner = split_arguments(strip(arguments[1])[2:-1])
                reference = self.text(inner[-1])
                coordinate = ", ".join(self.text(a) for a in inner[:-1])
                size = "float3" if kind == "sampler2DArrayShadow" else "float2"
                coordinate = f"{size}({coordinate})" if kind == "sampler2DArrayShadow" else coordinate
                return f"{texture}.SampleCmpLevelZero({sampler}, {coordinate}, {reference})"
            if name == "texture":
                return f"{texture}.Sample({sampler}, {uv_text})"
            if name == "textureLod":
                return f"{texture}.SampleLevel({sampler}, {uv_text}, {rest[0]})"
            if name == "textureGrad":
                return f"{texture}.SampleGrad({sampler}, {uv_text}, {rest[0]}, {rest[1]})"
            component = rest[0] if rest else "0"
            method = {"0": "GatherRed", "1": "GatherGreen", "2": "GatherBlue", "3": "GatherAlpha"}[component]
            return f"{texture}.{method}({sampler}, {uv_text})"
        if name == "textureSize":
            _, texture, _ = self.combined(arguments[0])
            return f"TextureSize({texture}, {self.text(arguments[1])})"
        if name == "textureQueryLevels":
            _, texture, _ = self.combined(arguments[0])
            return f"TextureLevels({texture})"
        if name == "imageStore":
            return f"{self.text(arguments[0])}[{self.text(arguments[1])}] = {self.text(arguments[2])}"
        if name == "imageLoad":
            return f"{self.text(arguments[0])}[{self.text(arguments[1])}]"
        if name == "imageSize":
            return f"ImageSize({self.text(arguments[0])})"
        if name == "inversesqrt":
            return f"(1.0 / sqrt({self.text(arguments[0])}))"
        if name == "barrier":
            return "GroupMemoryBarrierWithGroupSync()"
        if name == "atan" and len(arguments) == 2:
            return f"atan2({self.text(arguments[0])}, {self.text(arguments[1])})"
        if name in COMPARISONS:
            result = f"({self.text(arguments[0])} {COMPARISONS[name]} {self.text(arguments[1])})"
            self.listing.append(("vector comparison", self.original(tokens, index), result))
            return result
        if name == "not":
            return f"!({self.text(arguments[0])})"
        if name == "mix":
            selector = strip(arguments[2])
            if selector and selector[0].text in tuple(COMPARISONS) + ("isnan", "isinf"):
                # GLSL's mix(x, y, bvec) takes y where true.
                result = f"select({self.text(arguments[2])}, {self.text(arguments[1])}, {self.text(arguments[0])})"
                self.listing.append(("select (mix's arguments reversed)", self.original(tokens, index), result))
                return result
            return f"lerp({self.text(arguments[0])}, {self.text(arguments[1])}, {self.text(arguments[2])})"
        if name in ("mat2", "mat3", "mat4"):
            target = TYPES[name]
            rendered = [self.text(a) for a in arguments]
            if len(rendered) == 1:
                head = strip(arguments[0])
                if head and head[0].kind == "number":
                    size = int(name[3])
                    rows = ", ".join(rendered[0] if r == c else "0.0" for r in range(size) for c in range(size))
                    return f"{target}({rows})"
                return f"(({target})({rendered[0]}))"
            # GLSL fills columns, HLSL rows.
            result = f"transpose({target}({', '.join(rendered)}))"
            self.listing.append(("matrix constructor (GLSL's columns)", self.original(tokens, index), result))
            return result
        if name in ATOMICS:
            return self.atomic(name, arguments, tokens, index)
        return None

    def atomic(self, name, arguments, tokens, index):
        target = self.text(arguments[0])
        value = self.text(arguments[1])
        previous = significant_back(tokens, index - 1)
        after = significant(tokens, match_close(tokens, significant(tokens, index + 1)) + 1)
        statement = (previous < 0 or tokens[previous].text in (";", "{", "}", ")", "else")) and after < len(tokens) and tokens[after].text == ";"
        if statement:
            return f"{ATOMICS[name]}({target}, {value})"
        self.hoists += 1
        temporary = f"st_Atomic{self.hoists}"
        self.listing.append(("atomic with a hoisted value", self.original(tokens, index), f"{ATOMICS[name]}({target}, {value}, {temporary})"))
        return f"@HOIST<uint {temporary}; {ATOMICS[name]}({target}, {value}, {temporary});>@{temporary}"

    def rewrite(self, tokens):
        out = []
        index = 0
        count = len(tokens)
        while index < count:
            token = tokens[index]
            if token.kind == "preprocessor":
                out.append(self.directive(token.text))
                index += 1
                continue
            if token.kind != "identifier":
                out.append(token.text)
                index += 1
                continue
            following = significant(tokens, index + 1)
            # Array constructors: uint[16](...), vec2[](...)
            if following < count and tokens[following].text == "[" and (token.text in TYPES or token.text in ("float", "uint", "int", "bool")):
                close = match_close(tokens, following)
                after = significant(tokens, close + 1)
                if after < count and tokens[after].text == "(":
                    end = match_close(tokens, after)
                    arguments = split_arguments(tokens[after + 1:end])
                    out.append("{ " + ", ".join(self.text(a) for a in arguments) + " }")
                    self.stats["array constructor"] += 1
                    index = end + 1
                    continue
            if following < count and tokens[following].text == "(":
                end = match_close(tokens, following)
                arguments = split_arguments(tokens[following + 1:end])
                replacement = self.call(token.text, arguments, tokens, index)
                if replacement is not None:
                    out.append(replacement)
                    index = end + 1
                    continue
                out.append(self.name(token.text))
                out.append("".join(t.text for t in tokens[index + 1:following]))
                out.append("(")
                out.append(self.rewrite(tokens[following + 1:end]))
                out.append(")")
                index = end + 1
                continue
            out.append(self.name(token.text))
            index += 1
        return "".join(out)

    def name(self, text):
        return TYPES.get(text, RENAMES.get(text, text))

    def directive(self, text):
        stripped = text.strip()
        if stripped.startswith("#version") or stripped.startswith("#extension"):
            return DROPPED
        match = re.match(r'#include "(.*)\.glsl"', stripped)
        if match:
            return text.replace(".glsl", ".slang")
        if stripped.startswith("#define"):
            match = re.match(r"([ \t]*#define[ \t]+\w+(?:\([^)]*\))?[ \t]*)(.*)", text, re.DOTALL)
            if match and match.group(2):
                return match.group(1) + self.rewrite(tokenize(match.group(2)))
        return text

    # --- Matrix multiplication -----------------------------------------------------------------------------------------

    def is_matrix_end(self, tokens, last):
        """Walks back over the primary expression ending at `last`: its first token and whether it is a matrix."""
        index = last
        indexed = 0
        while True:
            if tokens[index].text == "]":
                index = self.open_of(tokens, index)
                indexed += 1
                index = significant_back(tokens, index - 1)
                continue
            break
        matrix = False
        if tokens[index].text == ")":
            open_index = self.open_of(tokens, index)
            before = significant_back(tokens, open_index - 1)
            if before >= 0 and tokens[before].kind == "identifier":
                name = tokens[before].text
                matrix = name in ("transpose", "float2x2", "float3x3", "float4x4") or name in self.knowledge.matrix_functions
                first = before
            else:
                inner = tokens[significant(tokens, open_index + 1):index]
                matrix = len(inner) > 0 and inner[0].text in ("float2x2", "float3x3", "float4x4")
                if inner and inner[0].text == "(":
                    matrix = "float3x3" in "".join(t.text for t in inner[:3]) or "float4x4" in "".join(t.text for t in inner[:3])
                first = open_index
            return first, matrix and indexed == 0
        if tokens[index].kind != "identifier":
            return index, False
        name = tokens[index].text
        allowed = (0, 1) if name in self.knowledge.matrix_arrays else (0,)
        matrix = name in self.knowledge.matrices and indexed in allowed
        first = index
        while True:
            before = significant_back(tokens, first - 1)
            if before >= 0 and tokens[before].text == ".":
                previous = significant_back(tokens, before - 1)
                while tokens[previous].text == "]":
                    previous = significant_back(tokens, self.open_of(tokens, previous) - 1)
                first = previous
                continue
            break
        return first, matrix

    def open_of(self, tokens, close):
        closing = tokens[close].text
        opening = {")": "(", "]": "["}[closing]
        depth = 0
        for position in range(close, -1, -1):
            if tokens[position].kind != "operator":
                continue
            if tokens[position].text == closing:
                depth += 1
            elif tokens[position].text == opening:
                depth -= 1
                if depth == 0:
                    return position
        raise ValueError("unbalanced")

    def is_matrix_start(self, tokens, first):
        """Walks over the primary expression starting at `first`: its last token and whether it is a matrix."""
        index = first
        while tokens[index].text in ("-", "+", "!", "~"):
            index = significant(tokens, index + 1)
        matrix = False
        if tokens[index].text == "(":
            close = match_close(tokens, index)
            inner = "".join(t.text for t in tokens[index:close])
            matrix = inner.startswith("((float") or inner.startswith("(float3x3") or inner.startswith("(float4x4")
            return close, matrix
        if tokens[index].kind == "number":
            return index, False
        if tokens[index].kind != "identifier":
            return index, False
        name = None
        indexed = 0
        last = index
        while True:
            name = tokens[last].text
            indexed = 0
            following = significant(tokens, last + 1)
            if following < len(tokens) and tokens[following].text == "(":
                close = match_close(tokens, following)
                matrix = name in ("transpose", "float2x2", "float3x3", "float4x4") or name in self.knowledge.matrix_functions
                last = close
                following = significant(tokens, last + 1)
                if following < len(tokens) and tokens[following].text == ".":
                    last = significant(tokens, following + 1)
                    continue
                return last, matrix
            while following < len(tokens) and tokens[following].text == "[":
                last = match_close(tokens, following)
                indexed += 1
                following = significant(tokens, last + 1)
            if following < len(tokens) and tokens[following].text == ".":
                last = significant(tokens, following + 1)
                continue
            break
        allowed = (0, 1) if name in self.knowledge.matrix_arrays else (0,)
        return last, name in self.knowledge.matrices and indexed in allowed

    def multiplications(self, text):
        """Rewrites a * b to mul(a, b) where either is a matrix, inner products first."""
        lines = text.split("\n")
        result = []
        for line in lines:
            comment = line.find("//")
            code, trailing = (line, "") if comment < 0 else (line[:comment], line[comment:])
            if "*" in code and not code.lstrip().startswith("#"):
                code = self.multiplications_in(code)
            result.append(code + trailing)
        return "\n".join(result)

    def multiplications_in(self, code):
        for _ in range(8):
            tokens = tokenize(code)
            changed = False
            for index, token in enumerate(tokens):
                if token.text != "*" or token.kind != "operator":
                    continue
                left_end = significant_back(tokens, index - 1)
                right_start = significant(tokens, index + 1)
                if left_end < 0 or right_start >= len(tokens):
                    continue
                try:
                    left_start, left_matrix = self.is_matrix_end(tokens, left_end)
                    right_end, right_matrix = self.is_matrix_start(tokens, right_start)
                except (ValueError, IndexError):
                    continue
                if left_matrix or right_matrix:
                    left = "".join(t.text for t in tokens[left_start:left_end + 1])
                    right = "".join(t.text for t in tokens[right_start:right_end + 1])
                    code = "".join(t.text for t in tokens[:left_start]) + f"mul({left}, {right})" + "".join(t.text for t in tokens[right_end + 1:])
                    self.stats["mul"] += 1
                    operands = f"{'matrix' if left_matrix else 'not a matrix'} * {'matrix' if right_matrix else 'not a matrix'}"
                    self.listing.append((f"mul ({operands})", f"{left} * {right}", f"mul({left}, {right})"))
                    changed = True
                    break
            if not changed:
                break
        return code

    def matrix_indices(self, text):
        """GLSL's m[column][row] is HLSL's m[row][column]; m[column].xyz reads a column."""
        def swap(match):
            name = match.group(1)
            if name.split(".")[-1] not in self.knowledge.matrices or name.split(".")[-1] in self.knowledge.matrix_arrays:
                return match.group(0)
            self.stats["matrix index"] += 1
            result = f"{name}[{match.group(3)}][{match.group(2)}]"
            self.listing.append(("matrix index", match.group(0), result))
            return result

        text = re.sub(r"\b([A-Za-z_][\w.]*)\[(\w+)\]\[(\w+)\]", swap, text)

        def column(match):
            name = match.group(1)
            if name.split(".")[-1] not in self.knowledge.matrices or name.split(".")[-1] in self.knowledge.matrix_arrays:
                return match.group(0)
            self.stats["matrix column"] += 1
            result = f"transpose({name})[{match.group(2)}].{match.group(3)}"
            self.listing.append(("matrix column", match.group(0), result))
            return result

        return re.sub(r"\b([A-Za-z_][\w.]*)\[(\w+)\]\.([xyzw]+)\b(?!\s*[-+*/]?=[^=])", column, text)

    # --- Declarations ----------------------------------------------------------------------------------------------------

    def register(self, binding, default_space):
        match = re.match(r"ST_(SRV|SAMPLER|CBV|UAV)\((.+)\)$", binding)
        space = f", space{default_space}" if default_space not in ("0", 0) else ""
        if match:
            letter = {"SRV": "t", "SAMPLER": "s", "CBV": "b", "UAV": "u"}[match.group(1)]
            return f"register({letter}{match.group(2)}{space})"
        return f"register(t{binding}{space})"

    def layout_arguments(self, tokens):
        arguments = {}
        for argument in split_arguments(tokens):
            text = "".join(t.text for t in strip(argument))
            if "=" in text:
                key, value = text.split("=", 1)
                arguments[key.strip()] = value.strip()
            else:
                arguments[text] = True
        return arguments

    def declaration(self, tokens, start):
        """A layout(...) declaration at file scope: (replacement text, index after it)."""
        open_index = significant(tokens, start + 1)
        close = match_close(tokens, open_index)
        layout = self.layout_arguments(tokens[open_index + 1:close])
        index = significant(tokens, close + 1)
        words = []
        while tokens[index].text not in (";", "{"):
            if tokens[index].kind in ("identifier", "number") or tokens[index].text in ("[", "]", "*", "+"):
                words.append(tokens[index].text)
            index = significant(tokens, index + 1)
        if tokens[index].text == "{":
            return self.block(tokens, layout, words, index)
        end = index + 1
        if "local_size_x" in layout:
            sizes = (layout["local_size_x"], layout.get("local_size_y", "1"), layout.get("local_size_z", "1"))
            self.numthreads = sizes
            return "", end
        if "early_fragment_tests" in layout:
            # Where the GLSL declares it decides, under its #ifdef: the entry point's attribute follows the macro.
            self.early_tests = True
            return "#define ST_EARLY_FRAGMENT_TESTS [earlydepthstencil]", end
        if "location" in layout:
            return self.stage_variable(layout, words), end
        return self.resource(layout, words, tokens[close + 1:index]), end

    def stage_variable(self, layout, words):
        direction = "in" if "in" in words else "out"
        flat = "flat" in words
        rest = [w for w in words if w not in ("in", "out", "flat", "centroid", "noperspective")]
        kind, name = rest[0], rest[1]
        hlsl = TYPES.get(kind, kind)
        self.interface.append(("variable", direction, flat, hlsl, name, layout["location"]))
        self.stats["stage variable"] += 1
        return f"static {hlsl} {name};"

    def resource(self, layout, words, declaration_tokens):
        space = layout.get("set", "0")
        words = [w for w in words if w not in ("uniform", "readonly", "writeonly", "restrict", "coherent")]
        kind = words[0]
        name = words[1]
        array = "".join(words[2:])
        registered = self.register(layout["binding"], space)
        self.stats["resource"] += 1
        if kind in TEXTURES:
            return f"{TEXTURES[kind]} {name}{array} : {registered};"
        if kind in SAMPLERS:
            return f"{SAMPLERS[kind]} {name}{array} : {registered};"
        if kind in IMAGES:
            wrapper, element = IMAGES[kind]
            formats = [key for key in layout if key not in ("set", "binding")]
            attribute = f'[vk::image_format("{formats[0]}")] ' if formats else ""
            return f"{attribute}{wrapper}<{element}> {name}{array} : {registered};"
        self.hand.append(f"unknown resource {kind} {name}")
        return "// HAND: " + " ".join(words)

    def block(self, tokens, layout, words, open_brace):
        close = match_close(tokens, open_brace)
        index = significant(tokens, close + 1)
        instance = None
        instance_array = ""
        while tokens[index].text != ";":
            if tokens[index].kind == "identifier" and instance is None:
                instance = tokens[index].text
            elif instance is not None:
                instance_array += tokens[index].text
            index = significant(tokens, index + 1)
        end = index + 1
        kind_word = "uniform" if "uniform" in words else "buffer"
        block_name = [w for w in words if w not in ("uniform", "buffer", "readonly", "writeonly", "restrict", "coherent")][0]
        body = self.rewrite(tokens[open_brace + 1:close])
        readonly = "readonly" in words
        # The members, without comments.
        members = []
        statement = []
        for token in tokens[open_brace + 1:close]:
            if token.kind in ("comment", "space"):
                continue
            if token.text == ";":
                members.append(statement)
                statement = []
            else:
                statement.append(token)
        if "push_constant" in layout:
            self.stats["push constant"] += 1
            return f"struct {block_name}\n{{{body}}};\n[[vk::push_constant]] ConstantBuffer<{block_name}> {instance};", end
        space = layout.get("set", "0")
        registered = self.register(layout["binding"], space)
        if kind_word == "uniform":
            self.stats["constant buffer"] += 1
            if instance is None and len(members) == 1 and len(members[0]) == 2:
                return f"ConstantBuffer<{self.name(members[0][0].text)}> {members[0][1].text} : {registered};", end
            if instance is not None:
                return f"struct {block_name}\n{{{body}}};\nConstantBuffer<{block_name}> {instance} : {registered};", end
            aliases = "\n".join(f"#define {m[1].text} st_{block_name}.{m[1].text}" for m in members)
            return f"struct {block_name}\n{{{body}}};\nConstantBuffer<{block_name}> st_{block_name} : {registered};\n{aliases}", end
        # Storage buffers: one member that is an array is a structured buffer of its element.
        self.stats["storage buffer"] += 1
        wrapper = "StructuredBuffer" if readonly else "RWStructuredBuffer"
        if len(members) == 1 and instance is None and len(members[0]) >= 3 and members[0][2].text == "[":
            element = self.name(members[0][0].text)
            return f"{wrapper}<{element}> {members[0][1].text} : {registered};", end
        if instance is not None:
            return (f"struct {block_name}\n{{{body}}};\n{wrapper}<{block_name}> st_{instance} : {registered};\n"
                    f"#define {instance} st_{instance}[0]"), end
        aliases = "\n".join(f"#define {m[1].text} st_{block_name}[0].{m[1].text}" for m in members)
        self.hand.append(f"multi-member storage block {block_name}")
        return f"struct {block_name}\n{{{body}}};\n{wrapper}<{block_name}> st_{block_name} : {registered};\n{aliases}", end

    # --- A whole file ------------------------------------------------------------------------------------------------------

    def convert(self, text):
        self.interface = []
        self.numthreads = None
        self.early_tests = False
        tokens = tokenize(text)
        out = []
        index = 0
        depth = 0
        last = len(tokens)
        statement_start = True
        while index < last:
            token = tokens[index]
            if token.kind in ("space", "comment"):
                out.append(token.text)
                index += 1
                continue
            if token.kind == "preprocessor":
                stripped = token.text.strip()
                if depth == 0 and re.match(r"#\s*(if|ifdef|ifndef|else|elif|endif)\b", stripped) and not stripped.endswith("_GLSL") and "_GLSL" not in stripped:
                    self.interface.append(("directive", stripped))
                out.append(self.directive(token.text))
                index += 1
                continue
            if depth == 0 and statement_start:
                if token.text == "layout":
                    replacement, index = self.declaration(tokens, index)
                    out.append(replacement)
                    statement_start = True
                    continue
                if token.text == "invariant":
                    while tokens[index].text != ";":
                        index += 1
                    index += 1
                    self.stats["invariant"] += 1
                    out.append("// invariant gl_Position: see the hand fixes")
                    continue
                if token.text == "const":
                    out.append("static ")
                elif token.kind == "identifier" and token.text not in ("struct", "void", "invariant"):
                    # A global variable (type name ...; or =) is private to the invocation: HLSL's plain globals are uniforms.
                    second = significant(tokens, index + 1)
                    third = significant(tokens, second + 1)
                    if tokens[second].kind == "identifier" and tokens[third].text in (";", "="):
                        out.append("static ")
                        self.stats["global variable"] += 1
            # The extent of one statement or function, rewritten as a piece (so calls keep their parenthesis).
            end = index
            local_depth = 0
            while end < last:
                current = tokens[end]
                if current.kind == "preprocessor" and local_depth == 0:
                    break
                if current.kind == "operator" and current.text in "{([":
                    local_depth += 1
                elif current.kind == "operator" and current.text in "})]":
                    local_depth -= 1
                    if local_depth == 0 and current.text == "}":
                        following = significant(tokens, end + 1)
                        if following < last and tokens[following].text == ";":
                            end = following
                        break
                elif current.kind == "operator" and current.text == ";" and local_depth == 0:
                    break
                end += 1
            piece = tokens[index:end + 1]
            out.append(self.rewrite(piece))
            index = end + 1
            statement_start = True
        converted = "".join(out)
        converted = self.finish_hoists(converted)
        converted = self.matrix_indices(converted)
        converted = self.multiplications(converted)
        return converted

    def finish_hoists(self, text):
        lines = text.split("\n")
        result = []
        for line in lines:
            hoisted = re.findall(r"@HOIST<(.*?)>@", line)
            if not hoisted:
                result.append(line)
                continue
            line = re.sub(r"@HOIST<.*?>@", "", line)
            indentation = line[:len(line) - len(line.lstrip())]
            previous = result[-1].strip() if result else ""
            braceless = re.match(r"(else\s+)?if\s*\(.*\)$|else$", previous) is not None
            if braceless:
                outer = indentation[:-1]
                result.append(outer + "{")
                result.extend(indentation + statement for statement in hoisted)
                result.append(line)
                result.append(outer + "}")
            else:
                result.extend(indentation + statement for statement in hoisted)
                result.append(line)
        return "\n".join(result)


def stage_of(path):
    return {"vert": "vertex", "frag": "fragment", "comp": "compute"}.get(path.rsplit(".", 1)[-1])


def entry_wrapper(converter, stage):
    """The entry point around the GLSL main, which became st_Main: stage variables in and out through structs."""
    fields_in = []
    fields_out = []
    copies_in = []
    copies_out = []
    for item in converter.interface:
        if item[0] == "directive":
            for target in (fields_in, fields_out, copies_in, copies_out):
                target.append(item[1])
            continue
        _, direction, flat, hlsl, name, location = item
        if direction == "in":
            prefix = "nointerpolation " if flat else ""
            semantic = f"ATTRIBUTE{location}" if stage == "vertex" else f"TEXCOORD{location}"
            fields_in.append(f"\t[[vk::location({location})]] {prefix}{hlsl} {name} : {semantic};")
            copies_in.append(f"\t{name} = input.{name};")
        else:
            prefix = "nointerpolation " if flat else ""
            semantic = f"SV_Target{location}" if stage == "fragment" else f"TEXCOORD{location}"
            fields_out.append(f"\t[[vk::location({location})]] {prefix}{hlsl} {name} : {semantic};")
            copies_out.append(f"\toutput.{name} = {name};")

    def prune(lines):
        # Conditionals that hold nothing are dropped, innermost first.
        changed = True
        while changed:
            changed = False
            for position in range(len(lines) - 1):
                if re.match(r"#\s*(if|ifdef|ifndef)\b", lines[position]) and re.match(r"#\s*endif", lines[position + 1]):
                    del lines[position:position + 2]
                    changed = True
                    break
        return lines

    fields_in, fields_out, copies_in, copies_out = prune(fields_in), prune(fields_out), prune(copies_in), prune(copies_out)
    text = ["", "// --- Entry point ---"]
    if stage == "compute":
        sizes = ", ".join(converter.numthreads)
        text += [f"[numthreads({sizes})]",
                 "void main(uint3 globalID : SV_DispatchThreadID, uint3 localID : SV_GroupThreadID, uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)",
                 "{", "\tst_GlobalInvocationID = globalID;", "\tst_LocalInvocationID = localID;", "\tst_WorkGroupID = groupID;",
                 "\tst_LocalInvocationIndex = localIndex;", "\tst_Main();", "}"]
        return "\n".join(text) + "\n"
    text += ["struct ST_Input", "{"] + fields_in + ["};", "struct ST_Output", "{"] + fields_out
    if stage == "vertex":
        text += ["\tfloat4 st_Position : SV_Position;"]
    if converter.writes_depth:
        text += ["\tfloat st_FragDepth : SV_Depth;"]
    text += ["};"]
    if converter.early_tests:
        text += ["#ifdef ST_EARLY_FRAGMENT_TESTS", "ST_EARLY_FRAGMENT_TESTS", "#endif"]
    if stage == "vertex":
        text += ["ST_Output main(ST_Input input, int vertexIndex : SV_VulkanVertexID, int instanceIndex : SV_VulkanInstanceID)", "{"]
        text += ["\tst_VertexIndex = vertexIndex;", "\tst_InstanceIndex = instanceIndex;"]
    else:
        text += ["ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)", "{"]
        text += ["\tst_FragCoord = fragCoord;", "\tst_FrontFacing = frontFacing;"]
    text += copies_in + ["\tst_Main();", "\tST_Output output;"] + copies_out
    if stage == "vertex":
        text += ["\toutput.st_Position = st_Position;"]
    if converter.writes_depth:
        text += ["\toutput.st_FragDepth = st_FragDepth;"]
    text += ["\treturn output;", "}"]
    return "\n".join(text) + "\n"


# The functions of Include/Common.slang's conversion section.
PRELUDE_FUNCTIONS = """int2 TextureSize(Texture2D<float4> texture, int level)
{
	uint width, height, levels;
	texture.GetDimensions(level, width, height, levels);
	return int2(width, height);
}

int2 TextureSize(Texture2D<uint4> texture, int level)
{
	uint width, height, levels;
	texture.GetDimensions(level, width, height, levels);
	return int2(width, height);
}

int3 TextureSize(Texture2DArray<float4> texture, int level)
{
	uint width, height, elements, levels;
	texture.GetDimensions(level, width, height, elements, levels);
	return int3(width, height, elements);
}

int3 TextureSize(Texture2DArray<uint4> texture, int level)
{
	uint width, height, elements, levels;
	texture.GetDimensions(level, width, height, elements, levels);
	return int3(width, height, elements);
}

int3 TextureSize(Texture3D<float4> texture, int level)
{
	uint width, height, depth, levels;
	texture.GetDimensions(level, width, height, depth, levels);
	return int3(width, height, depth);
}

int2 TextureSize(TextureCube<float4> texture, int level)
{
	uint width, height, levels;
	texture.GetDimensions(level, width, height, levels);
	return int2(width, height);
}

int TextureLevels(Texture2D<float4> texture)
{
	uint width, height, levels;
	texture.GetDimensions(0, width, height, levels);
	return int(levels);
}

int TextureLevels(TextureCube<float4> texture)
{
	uint width, height, levels;
	texture.GetDimensions(0, width, height, levels);
	return int(levels);
}

int TextureLevels(Texture2DArray<float4> texture)
{
	uint width, height, elements, levels;
	texture.GetDimensions(0, width, height, elements, levels);
	return int(levels);
}

int2 ImageSize(RWTexture2D<float4> image)
{
	uint width, height;
	image.GetDimensions(width, height);
	return int2(width, height);
}

int3 ImageSize(RWTexture2DArray<float4> image)
{
	uint width, height, elements;
	image.GetDimensions(width, height, elements);
	return int3(width, height, elements);
}

// Sources declared with two types under #ifdef (the same name, a texture or an array): one form for both.
float4 TexelFetch(Texture2D<float4> texture, int2 position, int level)
{
	return texture.Load(int3(position, level));
}

float4 TexelFetch(Texture2DArray<float4> texture, int3 position, int level)
{
	return texture.Load(int4(position, level));
}

uint4 TexelFetch(Texture2D<uint4> texture, int2 position, int level)
{
	return texture.Load(int3(position, level));
}

uint4 TexelFetch(Texture2DArray<uint4> texture, int3 position, int level)
{
	return texture.Load(int4(position, level));
}

float4 UnpackUnorm4x8(uint value)
{
	return float4(float(value & 255u), float((value >> 8) & 255u), float((value >> 16) & 255u), float(value >> 24)) / 255.0;
}

// GLSL's umulExtended: HLSL has no 32 x 32 to 64 multiplication without 64-bit integers.
void UnsignedMultiplyExtended(uint a, uint b, out uint high, out uint low)
{
	uint a0 = a & 65535u;
	uint a1 = a >> 16;
	uint b0 = b & 65535u;
	uint b1 = b >> 16;
	uint p00 = a0 * b0;
	uint p01 = a0 * b1;
	uint p10 = a1 * b0;
	uint p11 = a1 * b1;
	uint middle = (p00 >> 16) + (p01 & 65535u) + (p10 & 65535u);
	low = (p00 & 65535u) | (middle << 16);
	high = p11 + (p01 >> 16) + (p10 >> 16) + (middle >> 16);
}

// Slang lowers f32tof16 and f16tof32 to 16-bit floats on SPIR-V (Float16, which the engine's device does not enable).
uint PackHalf2x16(float2 value)
{
	__target_switch
	{
	case spirv:
		return spirv_asm { result:$$uint = OpExtInst glsl450 PackHalf2x16 $value };
	default:
		return f32tof16(value.x) | (f32tof16(value.y) << 16);
	}
}

float2 UnpackHalf2x16(uint value)
{
	__target_switch
	{
	case spirv:
		return spirv_asm { result:$$float2 = OpExtInst glsl450 UnpackHalf2x16 $value };
	default:
		return float2(f16tof32(value & 65535u), f16tof32(value >> 16));
	}
}
"""


# --- Hand fixes ----------------------------------------------------------------------------------------------------------

# What the converter cannot do, fixed on its output: (number, file, before, after). Each must match exactly once.
HAND_FIXES = [
    # 1-3. A comparison of vectors is one bool in GLSL, a bool vector here: the compiler rejects each of them.
    (1, "Scene/Mesh.vert.slang", "if (previousSkin.Rows[0] != skin.Rows[0] || previousSkin.Rows[1] != skin.Rows[1] || previousSkin.Rows[2] != skin.Rows[2])",
     "if (any(previousSkin.Rows[0] != skin.Rows[0]) || any(previousSkin.Rows[1] != skin.Rows[1]) || any(previousSkin.Rows[2] != skin.Rows[2]))"),
    (2, "Scene/Mesh.vert.slang", "(previous.WorldRows[0] != instance.WorldRows[0] || previous.WorldRows[1] != instance.WorldRows[1]",
     "(any(previous.WorldRows[0] != instance.WorldRows[0]) || any(previous.WorldRows[1] != instance.WorldRows[1])"),
    (3, "Scene/Mesh.vert.slang", "|| previous.WorldRows[2] != instance.WorldRows[2] || previousPosition != position))",
     "|| any(previous.WorldRows[2] != instance.WorldRows[2]) || any(previousPosition != position)))"),
    # 4-6. The same.
    (4, "Effects/SceneColorCopy.comp.slang", "if (size == last + 1)", "if (all(size == last + 1))"),
    (5, "PostProcess/MotionBlurNeighbors.comp.slang", "if (neighbor != tile + int2(x, y))", "if (any(neighbor != tile + int2(x, y)))"),
    (6, "PostProcess/TAA.comp.slang", "(motion == float2(0.0) ?", "(all(motion == float2(0.0)) ?"),
    # 7. A multiplication over two lines.
    (7, "Scene/ShadowMesh.vert.slang", """st_Position = u_LocalShadows[view / ST_LOCAL_SHADOW_FACES].ViewProjection[view % ST_LOCAL_SHADOW_FACES]
		* float4(TransformPositionRelative(instance, localPosition), 1.0);""",
     """st_Position = mul(u_LocalShadows[view / ST_LOCAL_SHADOW_FACES].ViewProjection[view % ST_LOCAL_SHADOW_FACES],
		float4(TransformPositionRelative(instance, localPosition), 1.0));"""),
    # 8-9. texture() in a compute shader: GLSL reads level 0 there (glslang emits an explicit level 0); HLSL's Sample would
    # ask for compute derivatives.
    (8, "PostProcess/DepthOfFieldComposite.comp.slang", "u_Own.Sample(u_LinearClampSampler, uv).rgb", "u_Own.SampleLevel(u_LinearClampSampler, uv, 0.0).rgb"),
    (9, "PostProcess/DepthOfFieldComposite.comp.slang", "u_Near.Sample(u_LinearClampSampler, uv);", "u_Near.SampleLevel(u_LinearClampSampler, uv, 0.0);"),
    # 10. A lone column write.
    (10, "Scene/Cull.comp.slang", "world[3].xyz -= u_Cull.CameraPosition.xyz;",
     "world[0][3] -= u_Cull.CameraPosition.x;\n\tworld[1][3] -= u_Cull.CameraPosition.y;\n\tworld[2][3] -= u_Cull.CameraPosition.z;"),
    # 11. HLSL has no 32 x 32 to 64 multiplication.
    (11, "Scene/CullBuild.comp.slang", "umulExtended(count, lod.IndexCount / 3u, high, low);", "UnsignedMultiplyExtended(count, lod.IndexCount / 3u, high, low);"),
    # 12. Nor the dispatch's size: the rows are full whenever there is more than one.
    (12, "Scene/CullBuild.comp.slang", "gl_NumWorkGroups.x * gl_WorkGroupSize.x", "c_CullRowItems"),
    (12, "Scene/CullBuild.comp.slang", "\nvoid st_Main()",
     "\n// Dispatch (SceneRendererGPUDriven.cpp) fills rows of 65,535 groups of 64 whenever there is more than one row; with one\n"
     "// row the y term is 0.\nstatic const uint c_CullRowItems = 65535u * 64u;\n\nvoid st_Main()"),
]


def apply_hand_fixes(outputs, listing):
    for number, name, before, after in HAND_FIXES:
        text = outputs.get(name)
        count = text.count(before) if text is not None else 0
        if count != 1:
            raise ToolError(f"hand fix {number} matches {count} times in {name}, not once: {before[:70]!r}")
        outputs[name] = text.replace(before, after)
        listing.append((name, f"hand fix {number}", before, after))


# --- Finishing -----------------------------------------------------------------------------------------------------------

INVARIANT_BEFORE = "// invariant gl_Position: see the hand fixes"
INVARIANT_AFTER = ("// The position is invariant (the depth prepass's EQUAL test): Slang cannot say it, so the build decorates it\n"
                   "// (INVARIANT_POSITION in Strata/CMakeLists.txt).")
MACROS = re.compile(r"// Descriptor binding numbers follow NVRHI's Vulkan register mapping:[^\n]*\n(?://[^\n]*\n)*"
                    r"#define ST_SRV\(slot\)[^\n]*\n#define ST_SAMPLER\(slot\)[^\n]*\n#define ST_CBV\(slot\)[^\n]*\n#define ST_UAV\(slot\)[^\n]*\n")
BINDINGS = """// Bindings are HLSL registers: register(tN) sampled textures and read-only buffers, sN samplers, bN constant buffers, uN
// storage images and read-write buffers; spaceS is descriptor set S, the binding set's position in the pipeline state. The
// build shifts each class to NVRHI's Vulkan ranges, t +0, s +128, b +256, u +384 (VulkanBindingOffsets,
// CMake/StrataShaders.cmake).
"""
CONVERSION = """
// --- What the conversion from GLSL (wave 8) needs ---

// GLSL's stage variables as private globals, which each converted entry point fills before calling its st_Main. New shaders
// take these as entry-point parameters (Docs/DeveloperGuide.md, "Shaders").
static float4 st_FragCoord;
static bool st_FrontFacing;
static float st_FragDepth;
static float4 st_Position;
static int st_VertexIndex;
static int st_InstanceIndex;
static uint3 st_GlobalInvocationID;
static uint3 st_LocalInvocationID;
static uint3 st_WorkGroupID;
static uint st_LocalInvocationIndex;

// GLSL's expression forms that HLSL has only as methods with out parameters, and what HLSL lacks.
"""
GROUP = re.compile(r"^[ \t]*#[ \t]*(?:if|ifdef|ifndef)\b[^\n]*\n(?:[ \t]*\n|[ \t]*#[ \t]*(?:else|elif)\b[^\n]*\n)*"
                   r"[ \t]*#[ \t]*endif\b[^\n]*\n", re.MULTILINE)
REGISTER_COMMENTS = {"SRV": "t", "SAMPLER": "s", "CBV": "b", "UAV": "u"}


def finish(name, text, prelude):
    """The converted text of `name` in its committed form: the Common include first, Slang guards, register names in
    comments, Include/Common.slang's binding comment and conversion section (`prelude`: the functions after the stage
    globals), no preprocessor group left empty where declarations moved into the entry point, no runs of blank lines."""
    text = text.replace('#include "Include/Prelude.slang"\n', '#include "Include/Common.slang"\n', 1)
    text = text.replace(DROPPED + "\n", "").replace(DROPPED, "")
    text = text.replace(INVARIANT_BEFORE, INVARIANT_AFTER)
    text = re.sub(r"\bSTRATA_(\w+)_GLSL\b", r"STRATA_\1_SLANG", text)
    text = re.sub(r"\bST_(SRV|SAMPLER|CBV|UAV)\((\d+)\)", lambda match: REGISTER_COMMENTS[match.group(1)] + match.group(2), text)
    if name == "Include/Common.slang":
        if len(MACROS.findall(text)) != 1:
            raise ToolError("Include/Common.slang: the binding macros are not where the conversion expects them")
        text = MACROS.sub(lambda match: BINDINGS, text)
        end = text.rstrip().rfind("#endif")
        if end < 0:
            raise ToolError("Include/Common.slang has no closing #endif")
        text = text[:end].rstrip("\n") + "\n" + CONVERSION + prelude.strip("\n") + "\n\n#endif\n"
    while True:
        dropped = GROUP.sub("", text)
        if dropped == text:
            break
        text = dropped
    return re.sub(r"\n{3,}", "\n\n", text)


# --- Listing -------------------------------------------------------------------------------------------------------------

def write_listing(path, outputs, listing):
    """Each rewrite as file:line: before -> after, the line found from the rewritten text in the file written."""
    lines = []
    searched = collections.defaultdict(int)
    for name, kind, before, after in listing:
        text = outputs.get(name, "")
        position = text.find(after, searched[(name, after)])
        if position < 0:
            position = text.find(after)
        line = "?" if position < 0 else str(text.count("\n", 0, position) + 1)
        if position >= 0:
            searched[(name, after)] = position + 1
        flat_before = " ".join(before.split())
        flat_after = " ".join(after.split())
        lines.append(f"Strata/shaders/{name}:{line}: {kind}: {flat_before} -> {flat_after}")
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


# --- The tool ------------------------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Converts Strata/shaders from GLSL to HLSL-syntax Slang, in place.")
    parser.add_argument("--listing", help="write every rewrite that can be silently wrong to this file (outside the tree)")
    args = parser.parse_args()
    if args.listing and Path(args.listing).resolve().is_relative_to(REPOSITORY):
        print(f"ConvertShadersToSlang: --listing {args.listing} is inside the repository", file=sys.stderr)
        return 2
    try:
        sources = {}
        for directory, _, files in os.walk(SHADERS):
            for name in files:
                path = os.path.join(directory, name).replace("\\", "/")
                relative = os.path.relpath(path, SHADERS).replace("\\", "/")
                if relative.endswith(".slang"):
                    raise ToolError(f"{relative} is Slang already: the tree is converted")
                with open(path, encoding="utf-8", newline="\n") as handle:
                    sources[relative] = handle.read()
        knowledge = Knowledge(sources)
        totals = collections.Counter()
        hand = []
        outputs = {}
        listing = []
        for relative, text in sorted(sources.items()):
            if relative in GENERATED:
                continue
            converter = Converter(relative, knowledge)
            converted = converter.convert(text)
            stage = stage_of(relative)
            target = relative[:-len(".glsl")] + ".slang" if relative.endswith(".glsl") else relative + ".slang"
            if stage:
                converter.writes_depth = "gl_FragDepth" in text
                converted = re.sub(r"\bvoid(\s+)main\s*\(\s*\)", r"void\1st_Main()", converted)
                converted = '#include "Include/Common.slang"\n' + converted + entry_wrapper(converter, stage)
            outputs[target] = converted
            totals.update(converter.stats)
            hand += [f"{relative}: {item}" for item in converter.hand]
            listing += [(target, kind, before, after) for kind, before, after in converter.listing]
        apply_hand_fixes(outputs, listing)
        for target in outputs:
            outputs[target] = finish(target, outputs[target], PRELUDE_FUNCTIONS)
    except (ToolError, ValueError, OSError) as error:
        print(f"ConvertShadersToSlang: {error}; nothing was written", file=sys.stderr)
        return 1

    for target, text in outputs.items():
        with open(os.path.join(SHADERS, target), "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
    for relative in sources:
        os.remove(os.path.join(SHADERS, relative))
    for relative, command in GENERATED.items():
        result = subprocess.run([sys.executable, "-I", str(REPOSITORY / command[0])] + command[1:], cwd=REPOSITORY)
        if result.returncode != 0:
            print(f"ConvertShadersToSlang: {' '.join(command)} failed ({result.returncode})", file=sys.stderr)
            return 1
    for key, value in totals.most_common():
        print(f"{value:5} {key}")
    # Forms the converter chose that review should see (a multi-member storage block becomes a one-element array of its
    # struct, its members reached through defines).
    for item in hand:
        print("NOTE", item)
    print(f"{len(HAND_FIXES)} hand fix edits applied; {len(outputs)} files written, {len(sources)} GLSL files removed")
    if args.listing:
        write_listing(args.listing, outputs, listing)
        print(f"{len(listing)} rewrites listed in {args.listing}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
