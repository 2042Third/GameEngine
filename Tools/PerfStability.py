#!/usr/bin/env python3
"""Runs the perf tests several times and reports how stable every metric is.

A perf gate is only useful when it fails on regressions and passes on noise. Integrators run this before merging a change
that can affect performance, on the reference machine (AGENTS.md, "Testing"):

    python Tools/PerfStability.py --runs 5                    # ctest --preset windows-release -L perf, five times
    python Tools/PerfStability.py --preset linux-release --runs 3
    python Tools/PerfStability.py --results a.json b.json     # report on results files kept from earlier runs

Each run executes `ctest --preset <preset> -L perf` from the repository and keeps its log and its results file
(<build>/PerfResults/<config>.json, which CTest clears at the start of every run) in the output directory. The report:

- per run: whether CTest passed, how long it took, the failed tests, every metric that needed more than one attempt of
  its confirmed check (with the attempts), and the canary (Perf.Harness.Sort200k, a fixed sort) relative to its
  reference measurement. A failing run whose confirmation attempts were all slow, while the canary was slow too, was
  slowed down by the machine's other work rather than by the code;
- per metric: its unit, budget and target, in how many runs it was recorded, the minimum, median and maximum of its
  value (the deciding value of a confirmed check), the largest value as a share of the budget, the pass rate, and how
  many runs confirmed it with more than one attempt.

Exit code: 0 when every run passed (with --results: every recorded value was within its budget), 1 otherwise, 2 for
usage errors. Only the standard library is used.
"""

import argparse
import datetime
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time

REPOSITORY_DIRECTORY = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUDGET_FILE = os.path.join(REPOSITORY_DIRECTORY, "StrataTests", "Perf", "Budgets.json")
CANARY_METRIC = "Perf.Harness.Sort200k"
FAILED_TEST = re.compile(r"^\s*\d+\s*-\s*(\S+)\s*\((Failed|Timeout|Subprocess aborted|SEGFAULT|Exception|Not Run)\)")


def fail(message):
    print(f"PerfStability: {message}", file=sys.stderr)
    sys.exit(2)


def load_json(path):
    try:
        with open(path, "r", encoding="utf-8") as file:
            return json.load(file)
    except (OSError, ValueError) as error:
        fail(f"cannot read '{path}': {error}")


def find_preset(presets, kind, name):
    for preset in presets.get(kind, []):
        if preset.get("name") == name:
            return preset
    fail(f"CMakePresets.json has no {kind[:-1]} '{name}'")


def inherited(presets, kind, preset, key):
    """The value of `key` in a preset or the first preset it inherits from that sets it."""
    if key in preset:
        return preset[key]
    parents = preset.get("inherits", [])
    for parent in [parents] if isinstance(parents, str) else parents:
        value = inherited(presets, kind, find_preset(presets, kind, parent), key)
        if value is not None:
            return value
    return None


def resolve_results_file(preset_name):
    """<binaryDir>/PerfResults/<configuration>.json of a test preset."""
    presets = load_json(os.path.join(REPOSITORY_DIRECTORY, "CMakePresets.json"))
    test_preset = find_preset(presets, "testPresets", preset_name)
    configure_name = inherited(presets, "testPresets", test_preset, "configurePreset")
    configuration = inherited(presets, "testPresets", test_preset, "configuration")
    if not configure_name or not configuration:
        fail(f"test preset '{preset_name}' names no configure preset and configuration")
    configure_preset = find_preset(presets, "configurePresets", configure_name)
    binary_directory = inherited(presets, "configurePresets", configure_preset, "binaryDir")
    if not binary_directory:
        fail(f"configure preset '{configure_name}' has no binaryDir")
    binary_directory = binary_directory.replace("${sourceDir}", REPOSITORY_DIRECTORY).replace("${presetName}", configure_name)
    if "${" in binary_directory:
        fail(f"cannot resolve the binaryDir '{binary_directory}' of preset '{configure_name}'")
    return os.path.normpath(os.path.join(binary_directory, "PerfResults", f"{configuration}.json"))


def run_ctest(arguments, run, output_directory, results_file):
    """Runs the perf tests once; returns the run's record (results file copied next to its log)."""
    command = [arguments.ctest, "--preset", arguments.preset, "-L", arguments.label, "--output-on-failure"]
    log_path = os.path.join(output_directory, f"run{run}.log")
    print(f"Run {run} of {arguments.runs}: {' '.join(command)}", flush=True)
    start = time.monotonic()
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        try:
            completed = subprocess.run(command, cwd=REPOSITORY_DIRECTORY, stdout=log, stderr=subprocess.STDOUT, check=False)
        except OSError as error:
            fail(f"cannot run '{arguments.ctest}': {error}")
    seconds = time.monotonic() - start

    failed_tests = []
    with open(log_path, "r", encoding="utf-8", errors="replace") as log:
        for line in log:
            match = FAILED_TEST.match(line)
            if match:
                failed_tests.append(f"{match.group(1)} ({match.group(2)})")

    results = None
    if os.path.isfile(results_file):
        kept = os.path.join(output_directory, f"run{run}.json")
        shutil.copyfile(results_file, kept)
        results = load_json(kept)
    passed = completed.returncode == 0
    print(f"Run {run}: {'passed' if passed else 'FAILED'} in {seconds:.0f} s" + (f" ({', '.join(failed_tests)})" if failed_tests else ""), flush=True)
    return {"name": f"run {run}", "passed": passed, "seconds": seconds, "failed_tests": failed_tests, "results": results or {}, "log": log_path}


def load_result_files(paths):
    runs = []
    for path in paths:
        results = load_json(path)
        if not isinstance(results, dict):
            fail(f"'{path}' is not a results file")
        within = all(entry.get("Value", 0) <= entry.get("Budget", 0) for entry in results.values() if isinstance(entry, dict))
        runs.append({"name": os.path.basename(path), "passed": within, "seconds": None, "failed_tests": [], "results": results, "log": None})
    return runs


def format_number(value):
    if value is None:
        return "-"
    if float(value).is_integer():
        return str(int(value))
    magnitude = abs(value)
    if magnitude == 0 or magnitude >= 100:
        return f"{value:.1f}"
    if magnitude >= 1:
        return f"{value:.3f}"
    return f"{value:.4g}"


def print_table(header, rows):
    widths = [max(len(str(row[column])) for row in [header] + rows) for column in range(len(header))]
    for index, row in enumerate([header] + rows):
        cells = [str(cell).ljust(widths[column]) if column == 0 else str(cell).rjust(widths[column]) for column, cell in enumerate(row)]
        print("  ".join(cells).rstrip())
        if index == 0:
            print("  ".join("-" * width for width in widths))


def report(runs, budgets):
    canary_reference = budgets.get(CANARY_METRIC, {}).get("Measured")

    print("\nRuns")
    for run in runs:
        results = run["results"]
        state = "passed" if run["passed"] else "FAILED"
        duration = f" in {run['seconds']:.0f} s" if run["seconds"] is not None else ""
        canary = results.get(CANARY_METRIC, {}).get("Value")
        speed = f"; canary {format_number(canary)} ms" if canary is not None else "; no canary"
        if canary is not None and canary_reference:
            speed += f" ({canary / canary_reference:.2f}x its reference)"
        print(f"- {run['name']}: {state}{duration}{speed}")
        for test in run["failed_tests"]:
            print(f"    failed test: {test}")
        for key in sorted(results):
            entry = results[key]
            attempts = entry.get("Attempts", []) if isinstance(entry, dict) else []
            if len(attempts) > 1 or (isinstance(entry, dict) and entry.get("Value", 0) > entry.get("Budget", 0)):
                verdict = "within" if entry["Value"] <= entry["Budget"] else "OVER"
                values = ", ".join(format_number(value) for value in attempts) if attempts else format_number(entry["Value"])
                print(f"    {key}: attempts {values} {entry['Unit']} -> {verdict} the budget of {format_number(entry['Budget'])}")
        if not results:
            print("    (no results file)")

    keys = sorted({key for run in runs for key, entry in run["results"].items() if isinstance(entry, dict)})
    rows = []
    for key in keys:
        entries = [run["results"][key] for run in runs if isinstance(run["results"].get(key), dict)]
        values = [entry["Value"] for entry in entries]
        budget = entries[-1]["Budget"]
        target = entries[-1].get("Target")
        passes = sum(1 for entry in entries if entry["Value"] <= entry["Budget"])
        confirmations = sum(1 for entry in entries if len(entry.get("Attempts", [])) > 1)
        worst = max(values)
        share = f"{worst / budget * 100:.0f}%" if budget > 0 else ("ok" if worst <= 0 else "over")
        rows.append([key, entries[-1]["Unit"], format_number(budget), format_number(target), len(entries), format_number(min(values)),
                     format_number(statistics.median(values)), format_number(worst), share, f"{passes}/{len(entries)}", confirmations])
    print("\nMetrics (value: the deciding attempt of a confirmed check)")
    if rows:
        print_table(["metric", "unit", "budget", "target", "runs", "min", "median", "max", "max/budget", "passed", "confirmed"], rows)
    else:
        print("(no results)")

    passed_runs = sum(1 for run in runs if run["passed"])
    streak = 0
    for run in runs:
        streak = streak + 1 if run["passed"] else 0
    print(f"\n{passed_runs} of {len(runs)} runs passed; the last {streak} in a row.")
    return passed_runs == len(runs)


def main():
    parser = argparse.ArgumentParser(description="Runs the perf tests several times and reports the spread and pass rate of every metric.")
    parser.add_argument("--preset", default="windows-release", help="CTest test preset (default: windows-release)")
    parser.add_argument("--runs", type=int, default=5, help="how many times to run the perf tests (default: 5)")
    parser.add_argument("--label", default="perf", help="CTest label to run (default: perf)")
    parser.add_argument("--ctest", default="ctest", help="the ctest executable (default: ctest on PATH)")
    parser.add_argument("--output", help="directory for the logs and results of the runs (default: a new PerfStability-<time> directory "
                        "next to the results file)")
    parser.add_argument("--results", nargs="+", metavar="FILE", help="report on these results files instead of running the tests")
    arguments = parser.parse_args()

    budgets = load_json(BUDGET_FILE)
    if arguments.results:
        runs = load_result_files(arguments.results)
    else:
        if arguments.runs < 1:
            fail("--runs needs a positive number")
        results_file = resolve_results_file(arguments.preset)
        output_directory = arguments.output or os.path.join(os.path.dirname(results_file),
                                                            "PerfStability-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
        os.makedirs(output_directory, exist_ok=True)
        print(f"Logs and results go to {output_directory}")
        runs = [run_ctest(arguments, run, output_directory, results_file) for run in range(1, arguments.runs + 1)]
    sys.exit(0 if report(runs, budgets) else 1)


if __name__ == "__main__":
    main()
