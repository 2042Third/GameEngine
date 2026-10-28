#!/usr/bin/env python3
"""Sweeps the physics simulation's tuning over the settle scenes and reports what each value does (Docs/DeveloperGuide.md,
"Settling and the sweep" and "Contact recycling").

    python Tools/PhysicsSweep.py distances --tests <StrataTests> --output <dir> [--repeats 3] [--seconds 10]
    python Tools/PhysicsSweep.py knobs --tests <StrataTests> --output <dir> --distance <m> [--repeats 3]
    python Tools/PhysicsSweep.py workers --tests <StrataTests> --output <dir> [--workers 4,8,12,16,24] [--repeats 3]
    python Tools/PhysicsSweep.py pits --tests <StrataTests> --output <dir> [--layouts 9] [--distances ...] [--thresholds ...]
    python Tools/PhysicsSweep.py playground --cli <StrataCLI> --editor <StrataEditor> --output <dir> [--distances ...]
    python Tools/PhysicsSweep.py report --output <dir>

Each point is one run of the test binary's helper `--strata-test-helper=physics-settle <scene> <seconds> <result.json>
[options]`, a fresh process that plays a settle scene (StrataTests/src/Physics/PhysicsScenes.h) and writes what it measured,
held under Tools/PerfLock.py (--lock-label, unless --no-lock) so that it never measures beside another agent's measurement.
Results stay in the output directory (outside the repository), one file per scene, point and repeat; a run that finds a
result already there skips it, so an interrupted sweep continues.

- distances: the contact recycle distances (--distances, meters; box3d's 0.05 among them) on every scene; the probe scenes
  (SeamProbe, CratePush, KinematicPlatform), whose results are deterministic, run once per point and also at the probes'
  extra distances (--probe-distances: 0.7 m, the owner's "0.7" read as meters). CPU measures repeat --repeats times, the
  points interleaved (each repeat goes through the distances in a rotated order).
- knobs: at one distance, one knob at a time (sleep threshold, contact hertz, damping ratio, push speed, collision steps)
  on the scenes that settle and the probes.
- workers: box3d's worker count (--workers) on the falling boxes and the crowd.
- pits: the ball pits (--scenes, BallPit500 and BallPit2k) in --layouts arrangements of their balls (the helper's --layout:
  the same lattice with its balls' fixed offsets shifted), per recycle distance and sleep threshold. A pit's time to sleep
  is chaotic (a difference in the last bit of a ball's start changes it by seconds), so one run ranks nothing; the report
  gives each point's median, lowest and highest time to 99% asleep over the layouts and how many slept. Its measures are
  simulated seconds, not wall time, so it runs without the perf lock, --jobs runs at a time.
- playground: the Physics Playground as its regression plays it (AutoDemo off), through a headless editor (--no-gpu, a
  private session directory) and StrataCLI on a copy (project.openSample): per distance, project.setSettings on
  Physics.Simulation, then 20 s of fixed steps (play.step, physics.stats every 15 steps), the script's TowerTopDrift,
  GhostsOnPitFloor and BallsInPit, then StartDominoes and DominoesDown after 20 s more. Written as
  <output>/Playground/<point>.json.
- report: the table of the results found (markdown, also written to <output>/report.md), and the rule that picks the
  default recycle distance: (a) the ghost gate against distance 0 (the seam probes' excursions at most 2 mm more, the
  ledges' travel at most 1 cm more, the crate within 5% and the head crate's slip at most 1 cm more, every domino down,
  the tower's drift at most the larger of 10 mm and 1.2 times distance 0's); (b) the cost gate (Boxes70k's busy library
  step within 3% of the best distance's); (c) among the distances that pass both, the shortest BallPit2k time to 99%
  asleep, then the lowest settle CPU, then the nearest to box3d's 5 cm.

Exit code: 0, 1 when a run failed, 2 for usage errors. Only the standard library is used.
"""

import argparse
import concurrent.futures
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time

REPOSITORY_DIRECTORY = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PERF_LOCK = os.path.join(REPOSITORY_DIRECTORY, "Tools", "PerfLock.py")

DISTANCES = [0.0, 0.0035, 0.007, 0.01, 0.025, 0.035, 0.05, 0.075, 0.1]
PROBE_DISTANCES = [0.7]
DEFAULT_DISTANCE = 0.05
SETTLE_SCENES = ["BallPit2k", "Boxes10k", "Tower20", "Pyramid15", "Dominoes64", "Boxes30kTracked", "Boxes70k", "Characters1000"]
PROBE_SCENES = ["SeamProbe", "CratePush", "KinematicPlatform"]
KNOBS = {
    "sleep-threshold": [0.03, 0.05, 0.08, 0.12],
    "contact-hertz": [20.0, 30.0, 45.0],
    "contact-damping": [5.0, 10.0, 20.0],
    "contact-push-speed": [0.5, 1.0, 3.0],
    "collision-steps": [1, 2],
}
KNOB_SCENES = ["BallPit2k", "Boxes10k", "Tower20", "Dominoes64"] + PROBE_SCENES
WORKER_SCENES = ["Boxes10k", "Boxes30kTracked", "Boxes70k", "Characters1000"]
PIT_SCENES = ["BallPit500", "BallPit2k"]
PIT_DISTANCES = [0.0, 0.007, 0.025, 0.035, 0.05, 0.075]
PIT_THRESHOLDS = [0.05, 0.08]
# Measured in a separate pass of the probes with the busy phase only: the falling boxes settle within the perf lab's frames.
CPU_SCENES = set(SETTLE_SCENES)


def point_name(options):
    """A file-name-safe name of a point's options, e.g. "recycle-distance=0.05"."""
    return ",".join(f"{key}={value:g}" if isinstance(value, float) else f"{key}={value}" for key, value in options)


def result_path(output, scene, options, repeat):
    return os.path.join(output, scene, f"{point_name(options)}-r{repeat}.json")


def run_point(args, scene, options, repeat):
    """Runs one point unless its result exists; returns its result (a dict) or None when the run failed."""
    path = result_path(args.output, scene, options, repeat)
    if os.path.exists(path):
        with open(path, encoding="utf-8") as file:
            return json.load(file)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    command = [args.tests, "--strata-test-helper=physics-settle", scene, str(args.seconds), path]
    for key, value in options:
        command += [f"--{key}", f"{value:g}" if isinstance(value, float) else str(value)]
    if not args.no_lock:
        command = [sys.executable, PERF_LOCK, "--label", args.lock_label, "--limit", "20", "--"] + command
    print(f"{scene} {point_name(options)} #{repeat}", flush=True)
    completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if completed.returncode != 0 or not os.path.exists(path):
        print(f"  failed ({completed.returncode}): {completed.stdout.strip()[-400:]}", flush=True)
        return None
    with open(path, encoding="utf-8") as file:
        return json.load(file)


def sweep(args, scenes, points, repeats_for):
    """Runs every scene at every point, the repeats interleaved: repeat r goes through the points rotated by r."""
    failed = False
    most = max(repeats_for(scene) for scene in scenes)
    for repeat in range(most):
        rotated = points[repeat % len(points):] + points[:repeat % len(points)]
        for options in rotated:
            for scene in scenes:
                if repeat >= repeats_for(scene):
                    continue
                if run_point(args, scene, options, repeat) is None:
                    failed = True
    return failed


def command_distances(args):
    distances = [float(value) for value in args.distances.split(",")]
    probes = [float(value) for value in args.probe_distances.split(",")] if args.probe_distances else []
    failed = sweep(args, SETTLE_SCENES, [[("recycle-distance", d)] for d in distances], lambda scene: args.repeats)
    failed |= sweep(args, PROBE_SCENES, [[("recycle-distance", d)] for d in distances + probes], lambda scene: 1)
    return 1 if failed else 0


def command_knobs(args):
    failed = False
    for knob, values in KNOBS.items():
        points = [[("recycle-distance", args.distance), (knob, value)] for value in values]
        failed |= sweep(args, KNOB_SCENES, points, lambda scene: args.repeats if scene in CPU_SCENES else 1)
    return 1 if failed else 0


def command_workers(args):
    counts = [int(value) for value in args.workers.split(",")]
    failed = sweep(args, WORKER_SCENES, [[("workers", count)] for count in counts], lambda scene: args.repeats)
    return 1 if failed else 0


def command_pits(args):
    distances = [float(value) for value in args.distances.split(",")]
    thresholds = [float(value) for value in args.thresholds.split(",")]
    runs = [(scene, [("recycle-distance", distance), ("sleep-threshold", threshold), ("layout", layout)])
            for scene in args.scenes.split(",") for threshold in thresholds for distance in distances for layout in range(args.layouts)]
    # Simulated seconds do not depend on the machine's load: no lock, several runs at a time.
    args.no_lock = True
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(lambda run: run_point(args, run[0], run[1], 0), runs))
    return 1 if any(result is None for result in results) else 0


def call(cli, environment, project, command, parameters=None):
    """Runs one command through StrataCLI against the editor of `project`; returns its result (a dict) or raises."""
    arguments = [cli, "call", command, json.dumps(parameters or {}), "--project", project]
    completed = subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=environment, timeout=1800)
    if completed.returncode != 0:
        raise RuntimeError(f"{command} failed ({completed.returncode}): {completed.stderr.strip()[-400:]}")
    return json.loads(completed.stdout)


def command_playground(args):
    distances = [float(value) for value in args.distances.split(",")]
    environment = dict(os.environ)
    # A session directory of this sweep's own, in the user's temporary directory (the editor and StrataCLI make it, private to
    # the user, when it is missing; a volume without access control lists cannot hold one).
    os.makedirs(os.path.join(args.output, "Playground"), exist_ok=True)
    environment["STRATA_SESSION_DIR"] = os.path.join(tempfile.gettempdir(), f"StrataPhysicsSweep-{os.getpid()}")
    project = os.path.abspath(os.path.join(args.output, "Playground", "Project"))
    editor = subprocess.Popen([args.editor, "--no-gpu", "--idle-timeout", "600"], env=environment, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
    failed = False
    try:
        # The editor answers once it has started: until then StrataCLI finds none.
        for attempt in range(120):
            if subprocess.run([args.cli, "call", "editor.status"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=environment,
                              timeout=60).returncode == 0:
                break
            time.sleep(1.0)
        else:
            raise RuntimeError("the editor did not answer within two minutes")
        if os.path.exists(project):
            command, parameters = "project.open", {"path": project}
        else:
            command, parameters = "project.openSample", {"sample": "PhysicsPlayground", "directory": project}
        opened = subprocess.run([args.cli, "call", command, json.dumps(parameters)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                env=environment, timeout=1800)
        if opened.returncode != 0:
            raise RuntimeError(f"{command} failed: " + opened.stderr.strip()[-400:])
        game = call(args.cli, environment, project, "entity.find", {"name": "Game"})["entities"][0]
        start = call(args.cli, environment, project, "entity.find", {"name": "StartDominoes"})["entities"][0]
        for distance in distances:
            path = os.path.join(args.output, "Playground", point_name([("recycle-distance", distance)]) + ".json")
            if os.path.exists(path):
                continue
            print(f"Playground recycle-distance={distance:g}", flush=True)
            call(args.cli, environment, project, "project.setSettings",
                 {"section": "Physics.Simulation", "value": {"ContactRecycleDistance": distance, "SleepThreshold": args.sleep_threshold}})
            call(args.cli, environment, project, "play.start", {"paused": True})
            call(args.cli, environment, project, "script.setField", {"entity": game, "class": "Playground", "field": "AutoDemo", "value": False})
            series = []
            for chunk in range(80):
                call(args.cli, environment, project, "play.step", {"steps": 15})
                stats = call(args.cli, environment, project, "physics.stats", {"detail": True})
                series.append({"seconds": (chunk + 1) * 0.25, "awake": stats["sleep"]["awake"], "moving": stats["sleep"]["moving"],
                               "recycled": stats["contacts"]["recycled"], "awakeContacts": stats["contacts"]["awake"],
                               "stepMs": stats["step"]["ms"], "maxSpeed": stats["settling"]["maxSpeed"],
                               "contactPeak": stats["contacts"]["peak"], "contactRoomGrowths": stats["contacts"]["roomGrowths"],
                               "contactCapacity": stats["contacts"]["capacity"]})
            fields = call(args.cli, environment, project, "script.get", {"entity": game, "class": "Playground"})["scripts"][0]["fields"]
            call(args.cli, environment, project, "input.click", {"entity": start})
            call(args.cli, environment, project, "play.step", {"steps": 1200})
            after = call(args.cli, environment, project, "script.get", {"entity": game, "class": "Playground"})["scripts"][0]["fields"]
            call(args.cli, environment, project, "play.stop")
            asleep = next((sample["seconds"] for sample in series if sample["moving"] > 0 and sample["awake"] <= 0.01 * sample["moving"]), -1.0)
            recycled = sum(sample["recycled"] or 0 for sample in series)
            awake = sum(sample["awakeContacts"] or 0 for sample in series)
            result = {"recycleDistance": distance, "sleepThreshold": args.sleep_threshold, "secondsTo99Asleep": asleep,
                      "meanRecycledShare": recycled / awake if awake else 0.0,
                      "series": series, "probes": {"towerTopDrift": fields.get("TowerTopDrift"), "ghostsOnPitFloor": fields.get("GhostsOnPitFloor"),
                                                   "ballsInPit": fields.get("BallsInPit"), "ballsEscaped": fields.get("BallsEscaped"),
                                                   "dominoesDown": after.get("DominoesDown")}}
            with open(path, "w", encoding="utf-8", newline="\n") as file:
                json.dump(result, file, indent=1)
    except (RuntimeError, subprocess.TimeoutExpired, KeyError, IndexError, json.JSONDecodeError) as error:
        print(f"Playground sweep failed: {error}", file=sys.stderr)
        failed = True
    finally:
        try:
            subprocess.run([args.cli, "call", "editor.quit", json.dumps({"force": True}), "--project", project], env=environment,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
        except subprocess.TimeoutExpired:
            pass
        try:
            editor.wait(timeout=60)
        except subprocess.TimeoutExpired:
            editor.kill()
    return 1 if failed else 0


def load_results(output):
    """{scene: {point name: [results of the repeats]}}"""
    results = {}
    if not os.path.isdir(output):
        return results
    for scene in sorted(os.listdir(output)):
        directory = os.path.join(output, scene)
        if not os.path.isdir(directory):
            continue
        for name in sorted(os.listdir(directory)):
            if not name.endswith(".json"):
                continue
            point = name.rsplit("-r", 1)[0]
            with open(os.path.join(directory, name), encoding="utf-8") as file:
                results.setdefault(scene, {}).setdefault(point, []).append(json.load(file))
    return results


def median(values):
    values = [value for value in values if value is not None]
    return statistics.median(values) if values else None


def measure(results, scene, point, key):
    """The median over the repeats of a summary value, or of a probe's ("probes/<name>")."""
    runs = results.get(scene, {}).get(point, [])
    if key.startswith("probes/"):
        return median([run.get("probes", {}).get(key[len("probes/"):]) for run in runs])
    return median([run.get(key) for run in runs])


def describe(value, digits=3):
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def distance_point(distance):
    return point_name([("recycle-distance", distance)])


def apply_rule(results, distances):
    """The rule of the module's docstring: returns (chosen distance or None, {distance: reasons it failed})."""
    base = distance_point(0.0)
    failures = {}

    def check(distance, condition, reason):
        if not condition:
            failures.setdefault(distance, []).append(reason)

    seam_keys = sorted(key for key in results.get("SeamProbe", {}).get(base, [{}])[0].get("probes", {}) if key.endswith("excursionMm"))
    ledge_keys = sorted(key for key in results.get("SeamProbe", {}).get(base, [{}])[0].get("probes", {}) if key.endswith(".travel"))
    for distance in distances:
        point = distance_point(distance)
        for key in seam_keys:
            value = measure(results, "SeamProbe", point, "probes/" + key)
            reference = measure(results, "SeamProbe", base, "probes/" + key)
            if value is not None and reference is not None:
                check(distance, value <= reference + 2.0, f"{key} {value:.2f} mm against {reference:.2f}")
        for key in ledge_keys:
            value = measure(results, "SeamProbe", point, "probes/" + key)
            reference = measure(results, "SeamProbe", base, "probes/" + key)
            if value is not None and reference is not None:
                check(distance, value <= reference + 0.01, f"{key} {value:.3f} m against {reference:.3f}")
        crate = measure(results, "CratePush", point, "probes/crate.distance")
        crateBase = measure(results, "CratePush", base, "probes/crate.distance")
        if crate is not None and crateBase:
            check(distance, abs(crate - crateBase) <= 0.05 * abs(crateBase), f"crate pushed {crate:.3f} m against {crateBase:.3f}")
        slip = measure(results, "CratePush", point, "probes/headCrate.slip")
        slipBase = measure(results, "CratePush", base, "probes/headCrate.slip")
        if slip is not None and slipBase is not None:
            check(distance, slip <= slipBase + 0.01, f"head crate slipped {slip:.3f} m against {slipBase:.3f}")
        down = measure(results, "Dominoes64", point, "probes/dominoes.down")
        if down is not None:
            check(distance, down >= 64, f"{down:.0f} of 64 dominoes down")
        drift = measure(results, "Tower20", point, "probes/tower.topDriftMm")
        driftBase = measure(results, "Tower20", base, "probes/tower.topDriftMm")
        if drift is not None and driftBase is not None:
            check(distance, drift <= max(10.0, 1.2 * driftBase), f"tower drift {drift:.1f} mm against {driftBase:.1f}")
    library = {d: measure(results, "Boxes70k", distance_point(d), "busyLibraryMs") for d in distances}
    known = [value for value in library.values() if value is not None]
    if known:
        best = min(known)
        for distance, value in library.items():
            if value is not None:
                check(distance, value <= 1.03 * best, f"Boxes70k library step {value:.3f} ms against the best {best:.3f}")
    passing = [d for d in distances if d not in failures]
    if not passing:
        return None, failures

    def order(distance):
        point = distance_point(distance)
        asleep = measure(results, "BallPit2k", point, "secondsTo99Asleep")
        asleep = asleep if asleep is not None and asleep >= 0.0 else float("inf")
        cpu = measure(results, "BallPit2k", point, "settleCpuMs")
        return (asleep, cpu if cpu is not None else float("inf"), abs(distance - DEFAULT_DISTANCE))

    return min(passing, key=order), failures


def describe_pits(results):
    """The pits' table: per scene, sleep threshold and distance, the time to 99% asleep over the layouts."""
    groups = {}
    for scene in PIT_SCENES:
        for point, runs in results.get(scene, {}).items():
            options = dict(part.split("=", 1) for part in point.split(","))
            if "layout" not in options:
                continue
            key = (scene, float(options.get("sleep-threshold", "0.08")), float(options.get("recycle-distance", "0.05")))
            groups.setdefault(key, []).extend(run.get("secondsTo99Asleep") for run in runs)
    if not groups:
        return []
    lines = ["", "## Pits over layouts", "",
             "| pit | sleep threshold m/s | distance m | layouts | asleep | median 99% asleep s | lowest s | highest s |",
             "| --- | --- | --- | --- | --- | --- | --- | --- |"]

    def text(value):
        return "not asleep" if value == float("inf") else f"{value:.2f}"

    for (scene, threshold, distance), times in sorted(groups.items()):
        # A pit not asleep within the run counts as later than any that slept.
        ordered = sorted(float("inf") if time is None or time < 0.0 else time for time in times)
        asleep = [time for time in ordered if time != float("inf")]
        middle = ordered[len(ordered) // 2] if len(ordered) % 2 else (ordered[len(ordered) // 2 - 1] + ordered[len(ordered) // 2]) / 2.0
        lines.append(f"| {scene} | {threshold:g} | {distance:g} | {len(ordered)} | {len(asleep)} | {text(middle)} | {text(ordered[0])} | "
                     f"{text(ordered[-1])} |")
    return lines


def command_report(args):
    results = load_results(args.output)
    if not results:
        print(f"No results in '{args.output}'", file=sys.stderr)
        return 1
    distances = sorted({float(point.split("=", 1)[1]) for scene in results.values() for point in scene
                        if point.startswith("recycle-distance=") and "," not in point})
    lines = ["# Contact recycle distance sweep", ""]
    columns = [
        ("Boxes70k", "busyLibraryMs", "70k library ms"), ("Boxes70k", "busyStrataMs", "70k Strata ms"),
        ("Boxes70k", "busyCollideMs", "70k collide ms"), ("Boxes70k", "busyRecycledShare", "70k recycled"),
        ("BallPit2k", "secondsTo99Asleep", "pit 99% asleep s"), ("BallPit2k", "secondsToAllAsleep", "pit all asleep s"),
        ("BallPit2k", "settleCpuMs", "pit settle CPU ms"), ("BallPit2k", "restingJitterMm", "pit jitter mm"),
        ("BallPit2k", "meanRecycledShare", "pit recycled"), ("Boxes10k", "secondsTo99Asleep", "10k 99% asleep s"),
        ("Boxes10k", "settleCpuMs", "10k settle CPU ms"), ("Tower20", "probes/tower.topDriftMm", "tower drift mm"),
        ("Tower20", "secondsToAllAsleep", "tower asleep s"), ("Dominoes64", "probes/dominoes.allDownSeconds", "dominoes down s"),
        ("CratePush", "probes/crate.distance", "crate m"), ("CratePush", "probes/headCrate.slip", "head crate slip m"),
        ("KinematicPlatform", "probes/platform.slipMm", "platform slip mm"),
    ]
    lines.append("| distance m | " + " | ".join(title for _, _, title in columns) + " |")
    lines.append("| --- |" + " --- |" * len(columns))
    all_distances = sorted(set(distances) | {float(point.split("=", 1)[1]) for point in results.get("SeamProbe", {}) if "," not in point})
    for distance in all_distances:
        point = distance_point(distance)
        lines.append(f"| {distance:g} | " + " | ".join(describe(measure(results, scene, point, key)) for scene, key, _ in columns) + " |")
    seam = results.get("SeamProbe", {})
    if seam:
        keys = sorted(next(iter(seam.values()))[0].get("probes", {}))
        lines += ["", "## Seam and ledge probes", "", "| probe | " + " | ".join(f"{d:g}" for d in all_distances) + " |",
                  "| --- |" + " --- |" * len(all_distances)]
        for key in keys:
            lines.append(f"| {key} | " + " | ".join(describe(measure(results, "SeamProbe", distance_point(d), "probes/" + key)) for d in all_distances) + " |")
    chosen, failures = apply_rule(results, distances)
    lines += ["", "## The rule", ""]
    for distance in distances:
        reasons = failures.get(distance)
        lines.append(f"- {distance:g} m: " + ("passes" if not reasons else "fails: " + "; ".join(reasons[:4]) + (" ..." if len(reasons) > 4 else "")))
    lines.append("")
    lines.append(f"Chosen: {chosen:g} m" if chosen is not None else "Chosen: none passes")
    knob_points = sorted({point for scene in results.values() for point in scene if "," in point and "layout=" not in point})
    if knob_points:
        lines += ["", "## Knobs", "", "| point | pit 99% asleep s | pit settle CPU ms | 10k settle CPU ms | tower drift mm | dominoes down s | 70k library ms |",
                  "| --- | --- | --- | --- | --- | --- | --- |"]
        for point in knob_points:
            lines.append(f"| {point} | " + " | ".join(describe(measure(results, scene, point, key)) for scene, key in (
                ("BallPit2k", "secondsTo99Asleep"), ("BallPit2k", "settleCpuMs"), ("Boxes10k", "settleCpuMs"), ("Tower20", "probes/tower.topDriftMm"),
                ("Dominoes64", "probes/dominoes.allDownSeconds"), ("Boxes70k", "busyLibraryMs"))) + " |")
    lines += describe_pits(results)
    worker_points = sorted({point for scene in results.values() for point in scene if point.startswith("workers=")}, key=lambda p: int(p.split("=")[1]))
    if worker_points:
        lines += ["", "## Workers", "", "| workers | " + " | ".join(f"{scene} library ms | {scene} step ms" for scene in WORKER_SCENES) + " |",
                  "| --- |" + " --- | --- |" * len(WORKER_SCENES)]
        for point in worker_points:
            cells = []
            for scene in WORKER_SCENES:
                library = measure(results, scene, point, "busyLibraryMs")
                strata = measure(results, scene, point, "busyStrataMs")
                cells += [describe(library), describe(library + strata if library is not None and strata is not None else None)]
            lines.append(f"| {point.split('=')[1]} | " + " | ".join(cells) + " |")
    text = "\n".join(lines) + "\n"
    with open(os.path.join(args.output, "report.md"), "w", encoding="utf-8", newline="\n") as file:
        file.write(text)
    print(text)
    return 0


def main():
    parser = argparse.ArgumentParser(description="The physics tuning sweep (Docs/DeveloperGuide.md, \"Settling and the sweep\").")
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("distances", "knobs", "workers", "pits", "playground", "report"):
        command = commands.add_parser(name)
        command.add_argument("--output", required=True, help="Where the results go (outside the repository)")
        if name == "report":
            continue
        if name == "playground":
            command.add_argument("--cli", required=True, help="The StrataCLI executable")
            command.add_argument("--editor", required=True, help="The StrataEditor executable")
            command.add_argument("--distances", default=",".join(f"{d:g}" for d in DISTANCES))
            command.add_argument("--sleep-threshold", type=float, default=0.05)
            continue
        command.add_argument("--tests", required=True, help="The StrataTests executable of an optimized build")
        command.add_argument("--seconds", type=float, default=10.0, help="Simulated seconds per point (default 10)")
        command.add_argument("--repeats", type=int, default=3, help="Repeats of the CPU measures (default 3)")
        command.add_argument("--lock-label", default="physics-sweep", help="Who holds the perf lock (Tools/PerfLock.py)")
        command.add_argument("--no-lock", action="store_true", help="Run without the perf lock")
        if name == "distances":
            command.add_argument("--distances", default=",".join(f"{d:g}" for d in DISTANCES))
            command.add_argument("--probe-distances", default=",".join(f"{d:g}" for d in PROBE_DISTANCES))
        if name == "knobs":
            command.add_argument("--distance", type=float, required=True, help="The recycle distance the knobs are swept at")
        if name == "workers":
            command.add_argument("--workers", default="4,8,12,16,24")
        if name == "pits":
            command.add_argument("--scenes", default=",".join(PIT_SCENES))
            command.add_argument("--layouts", type=int, default=9, help="Arrangements of the balls per point (1 to 9)")
            command.add_argument("--distances", default=",".join(f"{d:g}" for d in PIT_DISTANCES))
            command.add_argument("--thresholds", default=",".join(f"{t:g}" for t in PIT_THRESHOLDS))
            command.add_argument("--jobs", type=int, default=4, help="Runs at a time (default 4)")
    args = parser.parse_args()
    if args.command == "playground":
        if not os.path.isfile(args.cli) or not os.path.isfile(args.editor):
            parser.error("no StrataCLI or StrataEditor at the paths given")
        args.cli = os.path.abspath(args.cli)
        args.editor = os.path.abspath(args.editor)
    elif args.command != "report":
        if not os.path.isfile(args.tests):
            parser.error(f"no test executable at '{args.tests}'")
        if args.repeats < 1 or args.seconds <= 0:
            parser.error("--repeats and --seconds must be positive")
        if args.command == "pits" and (not 1 <= args.layouts <= 9 or args.jobs < 1):
            parser.error("--layouts must be from 1 to 9 and --jobs positive")
        args.tests = os.path.abspath(args.tests)
    os.makedirs(args.output, exist_ok=True)
    handlers = {"distances": command_distances, "knobs": command_knobs, "workers": command_workers, "pits": command_pits,
                "playground": command_playground, "report": command_report}
    return handlers[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
