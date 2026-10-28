#!/usr/bin/env python3
"""The look-dev harness: fixed pictures of the 3D samples, a new project and a look-dev scene, taken through headless
editors, and the comparison of two sets of them.

  python -I Tools/LookDev.py capture --build <bin dir> [--label <text>] [--output <dir>] [--projects a,b] [--shots <glob>]
         [--sizes 3840x2160,1920x1080] [--aa taa,fxaa,off] [--quality project|low|medium|high|ultra] [--repeat 5] [--live]
         [--keep-work] [--work <dir> [--fresh-work]]
  python -I Tools/LookDev.py compare <before dir> <after dir> --output <dir>
  python -I Tools/LookDev.py selftest --build <bin dir> --output <dir>

capture copies the samples (never opening them in place), builds the Basic3D and look-dev projects with editor command
scripts (Tools/LookDev/LookDevScene.json), and captures the shots of Tools/LookDev/Shots.json at every size and
anti-aliasing mode, each repeated: the pictures, crops of their regions of interest enlarged 4 times, every repeat's
renderStats and the repeats' differences with the first (0 expected), a table of the GPU's stage times and a static page
(run.json, shots/, crops/, stats/, stages.csv, index.html). compare reports the differing pixels of every picture two
capture sets share, difference images, the crops side by side and the ratio of their GPU times. selftest builds the
look-dev project and captures one shot small, the CTest StrataTools.LookDev. Docs/DeveloperGuide.md, "Look-dev harness".

Standard library only; the PNG reader and writer are written here. Exit codes: 0 done, 1 some shots failed (capture),
a side lacks a picture the other has (compare) or a check failed (selftest), 2 usage.
"""

import argparse
import datetime
import fnmatch
import html
import json
import math
import multiprocessing
import os
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOOKDEV_DIR = ROOT / "Tools" / "LookDev"
EXE = ".exe" if os.name == "nt" else ""

# viewport.capture's anti-aliasing per mode: component.set values on every PostProcess entity of the shot's scene.
AA_MODES = {
    "taa": {"AntiAliasing": True, "AntiAliasingMethod": "TAA"},
    "fxaa": {"AntiAliasing": True, "AntiAliasingMethod": "FXAA"},
    "off": {"AntiAliasing": False},
}
QUALITIES = ("project", "low", "medium", "high", "ultra")
CROP_SCALE = 4
CALL_TIMEOUT_MS = 300000
OPEN_TIMEOUT_MS = 900000
EDITOR_IDLE_SECONDS = 900
SCRIPT_FRAMES = 36000  # Ten minutes of a headless editor's frames for a command script
WORLD_EXTENT = 20000  # Meters each way from the origin: every cell of a world (WorldData::c_SupportedExtent)


class ToolError(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------------------
# PNG: 8-bit RGB and RGBA, not interlaced, the five filter types.

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class Image:
    def __init__(self, width, height, channels, data=None):
        self.width = width
        self.height = height
        self.channels = channels
        self.data = data if data is not None else bytearray(width * height * channels)

    def row(self, y):
        stride = self.width * self.channels
        return self.data[y * stride:(y + 1) * stride]

    def crop(self, x0, y0, x1, y1):
        out = Image(x1 - x0, y1 - y0, self.channels)
        stride = self.width * self.channels
        out_stride = out.width * self.channels
        for y in range(y0, y1):
            start = y * stride + x0 * self.channels
            out.data[(y - y0) * out_stride:(y - y0 + 1) * out_stride] = self.data[start:start + out_stride]
        return out

    def enlarge(self, factor):
        """Nearest neighbor: every pixel becomes factor x factor pixels."""
        out = Image(self.width * factor, self.height * factor, self.channels)
        out_stride = out.width * self.channels
        for y in range(self.height):
            row = self.row(y)
            wide = bytearray(out_stride)
            for x in range(self.width):
                pixel = row[x * self.channels:(x + 1) * self.channels]
                start = x * factor * self.channels
                wide[start:start + factor * self.channels] = pixel * factor
            for repeat in range(factor):
                line = y * factor + repeat
                out.data[line * out_stride:(line + 1) * out_stride] = wide
        return out


def _swar_masks(length):
    low = int.from_bytes(b"\x7f" * length, "little")
    high = int.from_bytes(b"\x80" * length, "little")
    return low, high


def _swar_add(a, b, low, high):
    """Adds two byte strings held as integers byte by byte, modulo 256, without carries between bytes."""
    return ((a & low) + (b & low)) ^ ((a ^ b) & high)


def _unfilter_row(kind, raw, prior, bpp, masks):
    length = len(raw)
    low, high = masks
    if kind == 0:
        return bytearray(raw)
    if kind == 2:  # Up
        value = _swar_add(int.from_bytes(raw, "little"), int.from_bytes(prior, "little"), low, high)
        return bytearray(value.to_bytes(length, "little"))
    if kind == 1:  # Sub: a running sum per channel, as a scan of doubling shifts
        value = int.from_bytes(raw, "little")
        everything = (1 << (8 * length)) - 1
        shift = bpp
        while shift < length:
            value = _swar_add(value, (value << (8 * shift)) & everything, low, high)
            shift *= 2
        return bytearray(value.to_bytes(length, "little"))
    row = bytearray(raw)
    if kind == 3:  # Average
        for i in range(bpp):
            row[i] = (row[i] + (prior[i] >> 1)) & 255
        for i in range(bpp, length):
            row[i] = (row[i] + ((row[i - bpp] + prior[i]) >> 1)) & 255
        return row
    if kind == 4:  # Paeth
        for i in range(bpp):
            row[i] = (row[i] + prior[i]) & 255
        for i in range(bpp, length):
            a = row[i - bpp]
            b = prior[i]
            c = prior[i - bpp]
            pa = b - c if b > c else c - b
            pb = a - c if a > c else c - a
            pc = a + b - c - c
            if pc < 0:
                pc = -pc
            if pa <= pb and pa <= pc:
                predictor = a
            elif pb <= pc:
                predictor = b
            else:
                predictor = c
            row[i] = (row[i] + predictor) & 255
        return row
    raise ToolError(f"unknown PNG filter type {kind}")


def read_png(path):
    data = Path(path).read_bytes()
    if not data.startswith(PNG_SIGNATURE):
        raise ToolError(f"{path} is not a PNG file")
    offset = len(PNG_SIGNATURE)
    header = None
    compressed = bytearray()
    while offset + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        body = data[offset + 8:offset + 8 + length]
        if len(body) != length:
            raise ToolError(f"{path}: a chunk runs past the end of the file")
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break
        offset += 12 + length
    if header is None:
        raise ToolError(f"{path}: no IHDR chunk")
    width, height, depth, color, _, _, interlace = header
    channels = {2: 3, 6: 4}.get(color)
    if depth != 8 or channels is None or interlace != 0:
        raise ToolError(f"{path}: only 8-bit RGB and RGBA PNGs without interlacing are read (depth {depth}, color type {color})")
    try:
        raw = zlib.decompress(bytes(compressed))
    except zlib.error as error:
        raise ToolError(f"{path}: {error}") from error
    stride = width * channels
    if len(raw) != (stride + 1) * height:
        raise ToolError(f"{path}: {len(raw)} bytes of image data, {(stride + 1) * height} expected")
    image = Image(width, height, channels)
    masks = _swar_masks(stride)
    prior = bytes(stride)
    for y in range(height):
        start = y * (stride + 1)
        row = _unfilter_row(raw[start], raw[start + 1:start + 1 + stride], prior, channels, masks)
        image.data[y * stride:(y + 1) * stride] = row
        prior = row
    return image


def _chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)


def write_png(path, image):
    if image.channels not in (3, 4):
        raise ToolError("only RGB and RGBA images are written")
    stride = image.width * image.channels
    raw = bytearray()
    for y in range(image.height):
        raw.append(0)
        raw += image.data[y * stride:(y + 1) * stride]
    color = 2 if image.channels == 3 else 6
    content = (PNG_SIGNATURE + _chunk(b"IHDR", struct.pack(">IIBBBBB", image.width, image.height, 8, color, 0, 0, 0))
               + _chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + _chunk(b"IEND", b""))
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def make_leaf_texture():
    """256 x 256 RGBA: seven leaves (ellipses at fixed places) of color (60, 110, 40), opaque inside, transparent outside."""
    size = 256
    leaves = [(64, 60, 40, 22, 0.6), (150, 48, 46, 20, -0.4), (210, 120, 34, 18, 1.2), (100, 140, 50, 24, -1.0),
              (60, 200, 36, 16, 0.3), (170, 196, 44, 22, 0.9), (128, 96, 28, 12, 0.0)]
    image = Image(size, size, 4)
    for y in range(size):
        for x in range(size):
            inside = False
            for cx, cy, rx, ry, angle in leaves:
                dx = x + 0.5 - cx
                dy = y + 0.5 - cy
                u = dx * math.cos(angle) + dy * math.sin(angle)
                v = -dx * math.sin(angle) + dy * math.cos(angle)
                if (u / rx) ** 2 + (v / ry) ** 2 <= 1.0:
                    inside = True
                    break
            offset = (y * size + x) * 4
            image.data[offset:offset + 4] = bytes((60, 110, 40, 255 if inside else 0))
    return image


def difference(before, after):
    """(differing pixels, largest channel difference, mean channel difference, the difference image x 8) of two images."""
    if (before.width, before.height) != (after.width, after.height):
        raise ToolError(f"sizes differ: {before.width}x{before.height} and {after.width}x{after.height}")
    channels = min(before.channels, after.channels, 3)
    pixels = before.width * before.height
    out = Image(before.width, before.height, 3)
    differing = 0
    largest = 0
    total = 0
    for index in range(pixels):
        a = before.data[index * before.channels:index * before.channels + channels]
        b = after.data[index * after.channels:index * after.channels + channels]
        if a == b:
            continue
        differing += 1
        for channel in range(channels):
            delta = abs(a[channel] - b[channel])
            total += delta
            if delta > largest:
                largest = delta
            out.data[index * 3 + channel] = min(255, delta * 8)
    mean = total / (pixels * channels) if pixels else 0.0
    return differing, largest, mean, out


def compare_files(before_path, after_path):
    """Pixels of two PNG files compared; identical files are identical pictures (one encoder wrote both)."""
    if Path(before_path).read_bytes() == Path(after_path).read_bytes():
        return {"differingPixels": 0, "largestDifference": 0, "meanDifference": 0.0}, None
    differing, largest, mean, image = difference(read_png(before_path), read_png(after_path))
    return {"differingPixels": differing, "largestDifference": largest, "meanDifference": mean}, image


# ---------------------------------------------------------------------------------------------------------------------
# Shots and projects

def load_json(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ToolError(f"cannot read {path}: {error}") from error


def load_shots():
    shots = load_json(LOOKDEV_DIR / "Shots.json")
    if shots.get("Format") != "StrataLookDevShots" or shots.get("Version") != 1:
        raise ToolError("Tools/LookDev/Shots.json is not a version 1 StrataLookDevShots file")
    projects = shots.get("Projects", {})
    for shot in shots.get("Shots", []):
        if shot.get("Project") not in projects:
            raise ToolError(f"shot {shot.get('Name')} names an unknown project {shot.get('Project')}")
        for mode in shot.get("AntiAliasing", []):
            if mode not in AA_MODES:
                raise ToolError(f"shot {shot['Name']} names an unknown anti-aliasing mode {mode}")
        if "DebugView" in shot and not isinstance(shot["DebugView"], str):
            raise ToolError(f"shot {shot['Name']}: DebugView must be a viewport.capture debugView name")
    return shots


def parse_sizes(text):
    sizes = []
    for item in text.split(","):
        try:
            width, height = (int(value) for value in item.lower().split("x"))
        except ValueError as error:
            raise ToolError(f"--sizes: '{item}' is not <width>x<height>") from error
        if not (16 <= width <= 4096 and 16 <= height <= 4096):
            raise ToolError(f"--sizes: {item} is outside 16 to 4096 pixels each way")
        sizes.append((width, height))
    return sizes


def is_inside_tracked_tree(path):
    """Inside the repository, outside its build trees (build*) and hidden directories (.worktrees, .git)."""
    try:
        relative = Path(path).resolve().relative_to(ROOT)
    except ValueError:
        return False
    parts = relative.parts
    return not parts or not (parts[0].startswith("build") or parts[0].startswith("."))


def json_escape(text):
    return json.dumps(text)[1:-1]


class Children:
    """The tool's child processes: a private session directory, the editor to launch, and a log of what ran."""

    def __init__(self, build, work, log):
        self.build = Path(build)
        self.editor = self.build / f"StrataEditor{EXE}"
        self.cli = self.build / f"StrataCLI{EXE}"
        for program in (self.editor, self.cli):
            if not program.is_file():
                raise ToolError(f"--build: {program} does not exist")
        self.environment = dict(os.environ)
        # A session directory of the run's own, which the tool does not create: the editor and the CLI make it private (in
        # the user's temporary directory, on a volume that keeps permissions).
        self.session = Path(tempfile.gettempdir()) / f"StrataLookDev-{os.getpid()}-{int(time.time())}"
        self.environment["STRATA_SESSION_DIR"] = str(self.session)
        self.environment["STRATA_EDITOR_PATH"] = str(self.editor)
        self.log = log
        self.calls = 0
        self.params_dir = Path(work) / "params"
        self.params_dir.mkdir(parents=True, exist_ok=True)

    def run(self, arguments, timeout):
        self.log.write(" ".join(str(argument) for argument in arguments) + "\n")
        self.log.flush()
        try:
            result = subprocess.run([str(argument) for argument in arguments], capture_output=True, env=self.environment,
                                    timeout=timeout, stdin=subprocess.DEVNULL)
        except subprocess.TimeoutExpired as error:
            raise ToolError(f"{Path(arguments[0]).name} did not finish within {timeout} s") from error
        self.log.write(result.stderr.decode("utf-8", "replace"))
        return result

    def version(self):
        result = self.run([self.editor, "--version"], 60)
        lines = result.stdout.decode("utf-8", "replace").strip().splitlines()
        return lines[0] if lines else ""

    def run_script(self, script, name):
        """Runs a command script in a headless editor without automation, which quits when the script ends."""
        output = Path(script).with_suffix(".log")
        result = self.run([self.editor, "--headless", "--no-automation", "--commands", script, "--quit-after-commands",
                           "--frames", SCRIPT_FRAMES], SCRIPT_FRAMES / 30)
        output.write_bytes(result.stdout + result.stderr)
        if result.returncode != 0:
            raise ToolError(f"the command script of {name} failed (exit code {result.returncode}; {output})")


class Editor:
    """One editor serving one project, driven through StrataCLI."""

    def __init__(self, children, project, windowed):
        self.children = children
        self.project = Path(project)
        arguments = [children.cli, "launch", "--project", self.project, "--idle-timeout", EDITOR_IDLE_SECONDS]
        if not windowed:
            arguments.insert(2, "--headless")
        result = children.run(arguments, OPEN_TIMEOUT_MS / 1000)
        if result.returncode != 0:
            raise ToolError(f"the editor for {project} did not start: {result.stderr.decode('utf-8', 'replace').strip()}")

    def _arguments(self, method, params, timeout_ms):
        self.children.calls += 1
        params_file = self.children.params_dir / f"{self.children.calls:06d}.json"
        params_file.write_text(json.dumps(params if params is not None else {}), encoding="utf-8")
        return [self.children.cli, "call", method, f"@{params_file}", "--project", self.project, "--timeout", timeout_ms]

    def call(self, method, params=None, timeout_ms=CALL_TIMEOUT_MS):
        result = self.children.run(self._arguments(method, params, timeout_ms), timeout_ms / 1000 + 60)
        if result.returncode != 0:
            raise ToolError(f"{method} failed (exit code {result.returncode}): {result.stderr.decode('utf-8', 'replace').strip()}")
        try:
            return json.loads(result.stdout.decode("utf-8"))
        except ValueError as error:
            raise ToolError(f"{method} answered something that is not JSON") from error

    def start_call(self, method, params, timeout_ms=CALL_TIMEOUT_MS):
        """A call that answers later (a live capture): the process, to finish with finish_call."""
        arguments = [str(argument) for argument in self._arguments(method, params, timeout_ms)]
        self.children.log.write(" ".join(arguments) + " (in the background)\n")
        return subprocess.Popen(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.children.environment,
                                stdin=subprocess.DEVNULL)

    def finish_call(self, method, process, timeout_s):
        try:
            stdout, stderr = process.communicate(timeout=timeout_s)
        except subprocess.TimeoutExpired as error:
            process.kill()
            raise ToolError(f"{method} did not answer within {timeout_s} s") from error
        if process.returncode != 0:
            raise ToolError(f"{method} failed (exit code {process.returncode}): {stderr.decode('utf-8', 'replace').strip()}")
        return json.loads(stdout.decode("utf-8"))

    def quit(self):
        try:
            self.call("editor.quit", {"force": True}, 60000)
        except ToolError as error:
            self.children.log.write(f"editor.quit: {error}\n")


def prepare_project(key, project, children, work, fresh):
    """The project's directory in the work directory, made unless a usable one is there: (directory, seconds spent)."""
    started = time.monotonic()
    directory = Path(work) / "projects" / key
    if any(directory.glob("*.stproj")) and not fresh:
        return directory, 0.0
    if directory.exists():
        shutil.rmtree(directory)
    directory.parent.mkdir(parents=True, exist_ok=True)
    if "Sample" in project:
        # Only the sample's files: no editor state that opening it in place may have left beside it.
        shutil.copytree(ROOT / "Samples" / project["Sample"], directory, ignore=shutil.ignore_patterns(".strata"))
    elif project.get("Template"):
        script = Path(work) / f"{key}.json"
        steps = [{"command": "project.create",
                  "parameters": {"directory": str(directory), "name": project["Name"], "template": project["Template"]}}]
        script.write_text(json.dumps(steps, indent=1), encoding="utf-8")
        children.run_script(script, key)
    elif project.get("Script"):
        leaf = Path(work) / "LeafTexture.png"
        write_png(leaf, make_leaf_texture())
        text = (LOOKDEV_DIR / project["Script"]).read_text(encoding="utf-8")
        text = text.replace("@LOOKDEV_DIRECTORY@", json_escape(str(directory))).replace("@LEAF_TEXTURE@", json_escape(str(leaf)))
        script = Path(work) / f"{key}.json"
        script.write_text(text, encoding="utf-8")
        children.run_script(script, key)
    else:
        raise ToolError(f"project {key} has no Sample, Template or Script")
    if not any(directory.glob("*.stproj")):
        raise ToolError(f"project {key} was not made: {directory} has no project file")
    return directory, time.monotonic() - started


# ---------------------------------------------------------------------------------------------------------------------
# Capture

def shot_file_name(shot, size, mode):
    return f"{shot['Name']}-{size[0]}x{size[1]}-{mode}"


def camera_parameters(shot):
    if shot.get("Camera") in ("scene", "editor"):
        return {"camera": shot["Camera"]}
    if "Position" in shot and "Target" in shot:
        params = {"position": shot["Position"], "target": shot["Target"]}
        if "Fov" in shot:
            params["fov"] = shot["Fov"]
        return params
    raise ToolError(f"shot {shot['Name']} has neither Camera nor Position and Target")


def resolve_setup(editor, step):
    """A setup step's parameters, its "entityName" replaced by the first entity of that name."""
    params = json.loads(json.dumps(step.get("Parameters", {})))
    name = params.pop("entityName", None)
    if name is not None:
        found = editor.call("entity.find", {"name": name}).get("entities", [])
        if not found:
            raise ToolError(f"setup: no entity named {name}")
        params["entity"] = found[0]
    return params


def open_scene(editor, scene, load_world):
    editor.call("scene.open", {"scene": scene, "force": True}, OPEN_TIMEOUT_MS)
    if load_world:
        # Every cell of the world, kept loaded: the shots show the same content whatever the camera and however fast cells
        # stream (they stream around the editor camera otherwise).
        editor.call("world.loadCells", {"region": {"min": [-WORLD_EXTENT, -WORLD_EXTENT], "max": [WORLD_EXTENT, WORLD_EXTENT]}, "wait": True},
                    OPEN_TIMEOUT_MS)


def capture_still(editor, shot, size, mode, repeat, quality, output, work):
    """Every repeat of a shot at a size and mode: their results (without the image), the first saved in shots/."""
    results = []
    name = shot_file_name(shot, size, mode)
    for index in range(repeat):
        path = output / "shots" / f"{name}.png" if index == 0 else Path(work) / "repeats" / f"{name}-{index}.png"
        params = dict(camera_parameters(shot))
        params.update({"width": size[0], "height": size[1], "overlays": False, "path": str(path), "overwrite": True})
        if "DebugView" in shot:
            params["debugView"] = shot["DebugView"]
        if quality != "project":
            params["quality"] = quality
        result = editor.call("viewport.capture", params)
        result.pop("Image", None)
        result["file"] = str(path)
        results.append(result)
    return results


def capture_live(editor, shot, size, output):
    """A live shot: the Game view at the size, the game played to the step, frames read back as presented."""
    live = shot["Live"]
    editor.call("editor.showPanel", {"panel": "Game"})
    editor.call("viewport.setSettings", {"gameView": {"mode": "custom", "width": size[0], "height": size[1]}})
    editor.call("play.start", {"paused": True}, OPEN_TIMEOUT_MS)
    try:
        # The editor numbers the frames beside the path: <shot>-<W>x<H>-live-1.png and on.
        path = output / "shots" / f"{shot['Name']}-{size[0]}x{size[1]}-live.png"
        capture = editor.start_call("viewport.capture", {"live": True, "camera": "scene", "atFixedStep": live["AtFixedStep"],
                                                         "frames": live["Frames"], "path": str(path), "overwrite": True})
        steps = live["AtFixedStep"] + live["Frames"] + 2
        if live.get("Input"):
            editor.call("input.sequence", {"steps": steps, "events": live["Input"]}, OPEN_TIMEOUT_MS)
        else:
            editor.call("play.step", {"steps": steps}, OPEN_TIMEOUT_MS)
        result = editor.finish_call("viewport.capture", capture, CALL_TIMEOUT_MS / 1000)
    finally:
        editor.call("play.stop", {})
    result.pop("Image", None)
    return result


def postprocess(task):
    """Crops of a shot's first picture and the repeats' differences with it (run in worker processes)."""
    first, repeats, crops, crop_dir, keep_repeats = task
    outcome = {"selfDifference": [], "crops": []}
    for repeat in repeats:
        numbers, _ = compare_files(first, repeat)
        outcome["selfDifference"].append(numbers["differingPixels"])
        if not keep_repeats:
            Path(repeat).unlink(missing_ok=True)
    if crops:
        image = read_png(first)
        for crop in crops:
            x0, y0, x1, y1 = crop["Rect"]
            rect = (int(x0 * image.width), int(y0 * image.height), int(x1 * image.width), int(y1 * image.height))
            if rect[2] <= rect[0] or rect[3] <= rect[1]:
                continue
            path = Path(crop_dir) / f"{Path(first).stem}-{crop['Name']}.png"
            write_png(path, image.crop(*rect).enlarge(CROP_SCALE))
            outcome["crops"].append(str(path))
    return outcome


def gpu_busy(result):
    stages = result.get("renderStats", {}).get("gpuMs", {}) or {}
    return sum(value for value in stages.values() if isinstance(value, (int, float)))


def write_stages(output, records):
    stage_names = []
    for record in records:
        for result in record["results"]:
            for stage in (result.get("renderStats", {}).get("gpuMs", {}) or {}):
                if stage not in stage_names:
                    stage_names.append(stage)
    header = ["shot", "size", "aa", "gpuBusyMs"] + stage_names + ["triangles", "draws", "shadowDraws", "cachedCascades", "preset", "shadowMapSize"]
    lines = [",".join(header)]
    rows = []
    for record in records:
        results = record["results"]
        if not results:
            continue
        def median_of(get):
            values = [get(result) for result in results]
            values = [value for value in values if isinstance(value, (int, float))]
            return statistics.median(values) if values else ""
        stats = results[0].get("renderStats", {})
        quality = stats.get("quality", {}) if isinstance(stats.get("quality"), dict) else {}
        row = [record["shot"], record["size"], record["mode"], median_of(gpu_busy)]
        row += [median_of(lambda result, stage=stage: (result.get("renderStats", {}).get("gpuMs", {}) or {}).get(stage)) for stage in stage_names]
        row += [stats.get("triangles", ""), stats.get("draws", ""), stats.get("shadowDraws", ""), stats.get("cachedCascades", ""),
                quality.get("Preset", ""), stats.get("shadowMapSize", "")]
        rows.append(row)
        lines.append(",".join(f"{value:.4f}" if isinstance(value, float) else str(value) for value in row))
    (output / "stages.csv").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return header, rows


def read_stages(path):
    table = {}
    if not Path(path).is_file():
        return table
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    if not lines:
        return table
    header = lines[0].split(",")
    for line in lines[1:]:
        values = line.split(",")
        row = dict(zip(header, values))
        table[f"{row['shot']}-{row['size']}-{row['aa']}"] = row
    return table


def write_index(output, title, sections, header, rows):
    """A static page: every picture with its crops, and the table of stage times."""
    parts = [f"<!DOCTYPE html><html><head><meta charset='utf-8'><title>{html.escape(title)}</title>",
             "<style>body{font-family:sans-serif;background:#1d1f21;color:#ddd}img{max-width:960px;margin:2px}"
             "td,th{padding:2px 6px;border-bottom:1px solid #444;font-size:12px}h2{margin-top:2em}</style></head><body>",
             f"<h1>{html.escape(title)}</h1>"]
    for name, images in sections:
        parts.append(f"<h2>{html.escape(name)}</h2>")
        for image in images:
            relative = os.path.relpath(image, output).replace(os.sep, "/")
            parts.append(f"<a href='{html.escape(relative)}'><img src='{html.escape(relative)}' title='{html.escape(relative)}'></a>")
    if header:
        parts.append("<h2>GPU stages (median ms)</h2><table><tr>" + "".join(f"<th>{html.escape(str(cell))}</th>" for cell in header) + "</tr>")
        for row in rows:
            parts.append("<tr>" + "".join(f"<td>{html.escape(f'{cell:.3f}' if isinstance(cell, float) else str(cell))}</td>" for cell in row) + "</tr>")
        parts.append("</table>")
    parts.append("</body></html>")
    (Path(output) / "index.html").write_text("\n".join(parts), encoding="utf-8")


def select_shots(shots, args):
    projects = set(args.projects.split(",")) if args.projects else None
    selected = []
    for shot in shots["Shots"]:
        if projects is not None and shot["Project"] not in projects:
            continue
        if args.shots and not fnmatch.fnmatch(shot["Name"], args.shots):
            continue
        # Stills by default, stills and live shots with --live, live shots alone with --live-only.
        live = "Live" in shot
        if (args.live_only and not live) or (live and not args.live and not args.live_only):
            continue
        selected.append(shot)
    return selected


def capture_command(args):
    sizes = parse_sizes(args.sizes)
    modes = [mode.strip() for mode in args.aa.split(",") if mode.strip()]
    for mode in modes:
        if mode not in AA_MODES:
            raise ToolError(f"--aa: unknown mode {mode} (taa, fxaa, off)")
    if args.quality not in QUALITIES:
        raise ToolError(f"--quality: one of {', '.join(QUALITIES)}")
    if args.repeat < 1:
        raise ToolError("--repeat needs at least 1")
    if args.fresh_work and not args.work:
        raise ToolError("--fresh-work needs --work")
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M")
    output = Path(args.output) if args.output else Path(tempfile.gettempdir()) / "StrataLookDev" / f"{stamp}-{args.label or 'capture'}"
    output = output.resolve()
    if is_inside_tracked_tree(output):
        raise ToolError(f"--output {output} is inside the repository's tracked tree (use a build* or .worktrees directory or one outside)")
    work = Path(args.work).resolve() if args.work else output / "work"
    if args.work and is_inside_tracked_tree(work):
        raise ToolError(f"--work {work} is inside the repository's tracked tree")
    shots = load_shots()
    selected = select_shots(shots, args)
    if not selected:
        raise ToolError("no shot matches --projects and --shots")
    for directory in ("shots", "crops", "stats"):
        (output / directory).mkdir(parents=True, exist_ok=True)
    (work / "repeats").mkdir(parents=True, exist_ok=True)
    log = open(output / "run.log", "w", encoding="utf-8")
    children = Children(args.build, work, log)
    run = {"date": datetime.datetime.now().isoformat(timespec="seconds"), "label": args.label, "build": str(Path(args.build).resolve()),
           "version": children.version(), "options": {"sizes": args.sizes, "aa": modes, "quality": args.quality, "repeat": args.repeat,
           "live": args.live, "projects": args.projects, "shots": args.shots}, "editor": None, "projects": {}, "shots": []}
    records = []
    failed = []
    postprocess_tasks = []
    try:
        by_project = {}
        for shot in selected:
            by_project.setdefault(shot["Project"], []).append(shot)
        for key, project_shots in by_project.items():
            project = shots["Projects"][key]
            print(f"{key}: preparing the project", flush=True)
            try:
                directory, made_seconds = prepare_project(key, project, children, work, args.fresh_work)
                windowed = any("Live" in shot for shot in project_shots)
                opened = time.monotonic()
                editor = Editor(children, directory, windowed)
            except ToolError as error:
                print(f"{key}: {error}", flush=True)
                for shot in project_shots:
                    run["shots"].append({"shot": shot["Name"], "project": key, "ok": False, "error": str(error)})
                    failed.append(shot["Name"])
                continue
            run["projects"][key] = {"directory": str(directory), "madeSeconds": round(made_seconds, 1),
                                    "openSeconds": round(time.monotonic() - opened, 1)}
            try:
                if run["editor"] is None:
                    # The editor's section names the GPU and the window it draws in.
                    run["editor"] = editor.call("editor.status", {"sections": ["editor"]}).get("editor")
                if args.quality != "project":
                    methods = editor.call("rpc.listMethods", {})
                    capture = [method for method in methods.get("methods", []) if method.get("name") == "viewport.capture"]
                    if not capture or '"quality"' not in json.dumps(capture[0]):
                        raise ToolError(f"--quality {args.quality}: this build's viewport.capture has no quality parameter")
                for shot in project_shots:
                    started = time.monotonic()
                    outcome = {"shot": shot["Name"], "project": key, "ok": True}
                    try:
                        open_scene(editor, shot.get("Scene", project["Scene"]), project.get("LoadWorld", False))
                        for step in shot.get("Setup", []):
                            editor.call(step["Command"], resolve_setup(editor, step))
                        if "Live" in shot:
                            for size in sizes:
                                result = capture_live(editor, shot, size, output)
                                (output / "stats" / f"{shot['Name']}-{size[0]}x{size[1]}-live.json").write_text(json.dumps(result, indent=1), encoding="utf-8")
                        else:
                            post_process = editor.call("entity.find", {"component": "PostProcess"}).get("entities", [])
                            for mode in shot.get("AntiAliasing", modes):
                                if mode not in modes:
                                    continue
                                if post_process:
                                    editor.call("component.set", {"entities": post_process, "component": "PostProcess", "values": AA_MODES[mode]})
                                for size in sizes:
                                    print(f"  {shot['Name']} {size[0]}x{size[1]} {mode}", flush=True)
                                    results = capture_still(editor, shot, size, mode, args.repeat, args.quality, output, work)
                                    name = shot_file_name(shot, size, mode)
                                    records.append({"shot": shot["Name"], "size": f"{size[0]}x{size[1]}", "mode": mode, "results": results, "name": name})
                                    postprocess_tasks.append((name, (results[0]["file"], [result["file"] for result in results[1:]],
                                                                    shot.get("Crops", []), str(output / "crops"), args.keep_work)))
                    except ToolError as error:
                        outcome.update({"ok": False, "error": str(error)})
                        failed.append(shot["Name"])
                        print(f"  {shot['Name']} failed: {error}", flush=True)
                    outcome["seconds"] = round(time.monotonic() - started, 1)
                    run["shots"].append(outcome)
            finally:
                editor.quit()
    finally:
        log.close()
        shutil.rmtree(children.session, ignore_errors=True)

    # The pictures are read after the editors quit, on every core: nothing else runs while the GPU times are measured.
    print(f"Reading {len(postprocess_tasks)} pictures", flush=True)
    with multiprocessing.Pool(max(1, min(len(postprocess_tasks), (os.cpu_count() or 2) - 1))) as pool:
        outcomes = pool.map(postprocess, [task for _, task in postprocess_tasks]) if postprocess_tasks else []
    images_by_shot = {}
    for record, (name, task), outcome in zip(records, postprocess_tasks, outcomes):
        stats = {"repeats": record["results"], "selfDifference": outcome["selfDifference"]}
        (output / "stats" / f"{name}.json").write_text(json.dumps(stats, indent=1), encoding="utf-8")
        images_by_shot.setdefault(record["shot"], []).extend([task[0]] + outcome["crops"])
        if any(outcome["selfDifference"]):
            print(f"  {name}: repeats differ from the first by {outcome['selfDifference']} pixels", flush=True)
    for live in sorted((output / "shots").glob("*-live-*.png")):
        images_by_shot.setdefault(live.name.split("-")[0], []).append(str(live))
    header, rows = write_stages(output, records)
    write_index(output, f"Look-dev capture {args.label or ''} ({run['version']})", sorted(images_by_shot.items()), header, rows)
    run["failed"] = failed
    (output / "run.json").write_text(json.dumps(run, indent=1), encoding="utf-8")
    if not args.keep_work and not args.work:
        shutil.rmtree(work, ignore_errors=True)
    print(f"Captured {len(records)} pictures into {output}" + (f"; failed: {', '.join(failed)}" if failed else ""))
    return 1 if failed else 0


# ---------------------------------------------------------------------------------------------------------------------
# Compare

def compare_task(task):
    name, before, after, diff_path = task
    numbers, image = compare_files(before, after)
    if image is not None:
        write_png(diff_path, image)
        numbers["differenceImage"] = str(diff_path)
    return name, numbers


def side_by_side(before, after):
    """The two images next to each other, 4 pixels apart (RGB)."""
    height = max(before.height, after.height)
    out = Image(before.width + after.width + 4, height, 3)
    for y in range(height):
        for image, x0 in ((before, 0), (after, before.width + 4)):
            if y >= image.height:
                continue
            row = image.row(y)
            if image.channels == 4:
                rgb = bytearray(image.width * 3)
                rgb[0::3] = row[0::4]
                rgb[1::3] = row[1::4]
                rgb[2::3] = row[2::4]
                row = rgb
            start = (y * out.width + x0) * 3
            out.data[start:start + image.width * 3] = row
    return out


def crop_task(task):
    before, after, target = task
    write_png(target, side_by_side(read_png(before), read_png(after)))
    return target


def compare_command(args):
    before = Path(args.before).resolve()
    after = Path(args.after).resolve()
    output = Path(args.output).resolve()
    if is_inside_tracked_tree(output):
        raise ToolError(f"--output {output} is inside the repository's tracked tree")
    for side in (before, after):
        if not (side / "shots").is_dir():
            raise ToolError(f"{side} is not a capture set (it has no shots directory)")
    (output / "diff").mkdir(parents=True, exist_ok=True)
    (output / "crops").mkdir(parents=True, exist_ok=True)
    before_shots = {path.name for path in (before / "shots").glob("*.png")}
    after_shots = {path.name for path in (after / "shots").glob("*.png")}
    missing = sorted(before_shots ^ after_shots)
    tasks = [(name, str(before / "shots" / name), str(after / "shots" / name), str(output / "diff" / name)) for name in sorted(before_shots & after_shots)]
    with multiprocessing.Pool(max(1, min(len(tasks), (os.cpu_count() or 2) - 1))) as pool:
        pictures = dict(pool.map(compare_task, tasks)) if tasks else {}
    # The crops of the pictures that differ, side by side (the crops of identical pictures are identical).
    differing = [Path(name).stem for name, numbers in pictures.items() if numbers["differingPixels"]]
    crop_tasks = []
    for path in sorted((before / "crops").glob("*.png")):
        other = after / "crops" / path.name
        if other.is_file() and any(path.stem.startswith(stem + "-") for stem in differing):
            crop_tasks.append((str(path), str(other), str(output / "crops" / path.name)))
    with multiprocessing.Pool(max(1, min(len(crop_tasks), (os.cpu_count() or 2) - 1))) as pool:
        crops = pool.map(crop_task, crop_tasks) if crop_tasks else []
    before_stages = read_stages(before / "stages.csv")
    after_stages = read_stages(after / "stages.csv")
    ratios = {}
    for key, row in after_stages.items():
        base = before_stages.get(key)
        if not base:
            continue
        ratios[key] = {}
        for column, value in row.items():
            if column in ("shot", "size", "aa", "preset", "shadowMapSize", "triangles", "draws", "shadowDraws", "cachedCascades"):
                continue
            try:
                numerator = float(value)
                denominator = float(base.get(column, ""))
            except ValueError:
                continue
            if denominator > 0:
                ratios[key][column] = round(numerator / denominator, 4)
    report = {"before": str(before), "after": str(after), "missing": missing, "pictures": pictures, "gpuRatios": ratios}
    (output / "compare.json").write_text(json.dumps(report, indent=1), encoding="utf-8")
    sections = []
    for name, numbers in sorted(pictures.items()):
        if numbers.get("differenceImage"):
            sections.append((f"{name}: {numbers['differingPixels']} pixels differ, by at most {numbers['largestDifference']}",
                             [numbers["differenceImage"]]))
    if crops:
        sections.append(("Crops, before | after", crops))
    header = ["picture", "gpuBusy after / before"]
    rows = [[key, ratio.get("gpuBusyMs", "")] for key, ratio in sorted(ratios.items())]
    write_index(output, f"Look-dev compare: {before.name} -> {after.name}", sections, header, rows)
    differing = sum(1 for numbers in pictures.values() if numbers["differingPixels"])
    print(f"{len(pictures)} pictures compared, {differing} differ; {len(missing)} on one side only" + (f": {', '.join(missing)}" if missing else ""))
    return 1 if missing else 0


# ---------------------------------------------------------------------------------------------------------------------
# Self-test (the CTest StrataTools.LookDev)

def selftest_command(args):
    output = Path(args.output).resolve()
    if output.exists():
        shutil.rmtree(output)
    problems = []

    # The PNG writer and reader round trip, every filter type of the reader included.
    image = Image(37, 23, 4)
    for index in range(len(image.data)):
        image.data[index] = (index * 7 + (index // 37) * 13) & 255
    write_png(output / "roundtrip.png", image)
    if read_png(output / "roundtrip.png").data != image.data:
        problems.append("the PNG writer and reader do not round trip")
    stride = 6
    prior = bytes(range(10, 10 + stride))
    raw = bytes((3, 200, 7, 9, 250, 1))
    for kind in range(5):
        expected = bytearray(stride)
        for i in range(stride):
            a = expected[i - 3] if i >= 3 else 0
            b = prior[i]
            c = prior[i - 3] if i >= 3 else 0
            predictor = [0, a, b, (a + b) // 2, None][kind]
            if kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                predictor = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
            expected[i] = (raw[i] + predictor) & 255
        if _unfilter_row(kind, raw, prior, 3, _swar_masks(stride)) != expected:
            problems.append(f"PNG filter type {kind} is not undone")

    capture = argparse.Namespace(build=args.build, label="selftest", output=str(output / "capture"), projects="LookDev",
                                 shots="lookdev.contact", sizes="320x180", aa="fxaa", quality="project", repeat=2, live=False,
                                 live_only=False, keep_work=False, work=None, fresh_work=False)
    code = capture_command(capture)
    if code != 0:
        problems.append(f"capture exited with {code}")
    shot = output / "capture" / "shots" / "lookdev.contact-320x180-fxaa.png"
    if not shot.is_file():
        problems.append(f"{shot} was not captured")
    else:
        picture = read_png(shot)
        if (picture.width, picture.height) != (320, 180):
            problems.append(f"the picture is {picture.width}x{picture.height}, not 320x180")
        crops = list((output / "capture" / "crops").glob("lookdev.contact-320x180-fxaa-*.png"))
        if not crops:
            problems.append("no crop was written")
        for crop in crops:
            cropped = read_png(crop)
            if cropped.width % CROP_SCALE or cropped.height % CROP_SCALE or cropped.width == 0:
                problems.append(f"{crop.name} is {cropped.width}x{cropped.height}, not a multiple of {CROP_SCALE}")
        stats_path = output / "capture" / "stats" / "lookdev.contact-320x180-fxaa.json"
        stats = load_json(stats_path) if stats_path.is_file() else {}
        if len(stats.get("repeats", [])) != 2:
            problems.append("the stats do not hold both repeats")
        elif any(repeat.get("renderStats", {}).get("pendingAssets", -1) != 0 for repeat in stats["repeats"]):
            problems.append("a repeat's renderStats is missing or names pending assets")
        if stats.get("selfDifference") != [0]:
            problems.append(f"the repeats differ: {stats.get('selfDifference')}")
        index = (output / "capture" / "index.html")
        if not index.is_file() or "lookdev.contact" not in index.read_text(encoding="utf-8"):
            problems.append("index.html does not name the shot")
        compare = argparse.Namespace(before=str(output / "capture"), after=str(output / "capture"), output=str(output / "compare"))
        if compare_command(compare) != 0:
            problems.append("compare of the capture with itself failed")
        report = load_json(output / "compare" / "compare.json") if (output / "compare" / "compare.json").is_file() else {}
        if any(numbers.get("differingPixels") for numbers in report.get("pictures", {}).values()) or not report.get("pictures"):
            problems.append("compare of the capture with itself found differences or nothing")
    run = load_json(output / "capture" / "run.json") if (output / "capture" / "run.json").is_file() else {}
    if not run.get("shots") or not all(shot.get("ok") for shot in run["shots"]):
        problems.append(f"run.json reports failed shots: {run.get('shots')}")
    for problem in problems:
        print(f"selftest: {problem}")
    print("selftest: " + ("passed" if not problems else f"{len(problems)} problems"))
    return 1 if problems else 0


def main():
    parser = argparse.ArgumentParser(description="The look-dev harness (Docs/DeveloperGuide.md, \"Look-dev harness\").")
    commands = parser.add_subparsers(dest="command", required=True)
    capture = commands.add_parser("capture", help="capture the shots with a build")
    capture.add_argument("--build", required=True, help="the folder with StrataEditor and StrataCLI")
    capture.add_argument("--label", default="")
    capture.add_argument("--output")
    capture.add_argument("--projects", help="comma-separated project keys of Shots.json")
    capture.add_argument("--shots", help="a glob of shot names")
    capture.add_argument("--sizes", default="3840x2160,1920x1080")
    capture.add_argument("--aa", default="taa,fxaa,off")
    capture.add_argument("--quality", default="project")
    capture.add_argument("--repeat", type=int, default=5)
    capture.add_argument("--live", action="store_true", help="also the live shots (windowed editors)")
    capture.add_argument("--live-only", action="store_true", help="only the live shots")
    capture.add_argument("--keep-work", action="store_true")
    capture.add_argument("--work", help="the work directory: copies and projects reused between runs")
    capture.add_argument("--fresh-work", action="store_true", help="make the work directory's projects again")
    compare = commands.add_parser("compare", help="compare two capture sets")
    compare.add_argument("before")
    compare.add_argument("after")
    compare.add_argument("--output", required=True)
    selftest = commands.add_parser("selftest", help="the CTest: build the look-dev project and capture one shot small")
    selftest.add_argument("--build", required=True)
    selftest.add_argument("--output", required=True)
    args = parser.parse_args()
    try:
        if args.command == "capture":
            return capture_command(args)
        if args.command == "compare":
            return compare_command(args)
        return selftest_command(args)
    except ToolError as error:
        print(f"LookDev: {error}", file=sys.stderr)
        return 2 if args.command != "selftest" else 1


if __name__ == "__main__":
    sys.exit(main())
