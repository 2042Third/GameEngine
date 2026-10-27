#!/usr/bin/env python3
"""Runs the perf tests several times and reports how stable every metric is.

A perf gate is only useful when it fails on regressions and passes on noise. Integrators run this before merging a change
that can affect performance, on the reference machine (Docs/DeveloperGuide.md, "Perf tests"):

    python Tools/PerfStability.py --runs 5                    # ctest --preset windows-release -L perf, five times
    python Tools/PerfStability.py --preset linux-release --runs 3
    python Tools/PerfStability.py --runs 5 --wait-quiet 20    # each run waits until the machine is less than 20% busy
    python Tools/PerfStability.py --results a.json b.json     # report on results files kept from earlier runs
    python Tools/PerfStability.py --results run*.json --propose-budgets proposed.json [--rebaseline <metric>=<reason>]
    python Tools/PerfStability.py --results b*.json --baseline a*.json --max-ratio 1.02   # an interleaved A/B

Each run executes `ctest --preset <preset> -L perf` from the repository and keeps its log and its results file
(<build>/PerfResults/<config>.json, which CTest clears at the start of every run) in the output directory. The report:

- per run: whether CTest passed, how long it took, how busy the machine's CPUs were meanwhile (the perf tests included,
  which alone keep the reference machine about 20% busy; Windows; elsewhere the load average), the canary
  (Perf.Harness.Sort200k, a fixed single-threaded sort) relative to its reference measurement, how many metrics needed
  confirmation and failed, the failed tests, and every metric that needed more than one attempt of its confirmed check
  (with the attempts);
- per metric: its unit, budget and target, in how many runs it was recorded, the minimum, median and maximum of its
  value (the deciding value of a confirmed check), the largest value as a share of the budget, the pass rate, and how
  many runs confirmed it with more than one attempt (a provisional metric, which has no budget until the reference
  machine measures it, shows "provisional" and always passes);
- how many runs passed, the longest series of passing runs and the last one.

Load or regression: a confirmed check's attempts follow each other within about a minute, so other work that lasts
longer (a build) slows all of them, and on a shared machine that has failed single metrics as often as many. A failure
in a run on a busy machine therefore says nothing about the code, however many metrics failed: run the failing suite
again on a quiet machine (alone, or this tool with --wait-quiet), where a regression stays over its budget in every
attempt. The canary only shows whether one thread runs at full speed (a throttled or oversubscribed machine): load on
many cores leaves a core free for it, so it is usually unchanged in runs that load failed.

--propose-budgets <file> (with --results: the valid runs of a campaign on the reference machine) writes Budgets.json as
the rules of Docs/DeveloperGuide.md, "Perf tests", would change it, in the same layout and order, and prints every entry
it changed or left for a decision with its class (per metric, M is the median of its values and the spread their range
over M; Measured keeps four significant digits rounded to nearest, Budget four rounded down, at most 1.5 times Measured):

- unstable (spread above 0.25, not a count): unchanged, to be measured alone;
- provisional: Measured M and Budget 1.5 M; a Target every value met becomes the budget if lower and is dropped, one not
  met stays while it is below the budget (an acceptance not met);
- cheaper (M below 0.9 times Measured): Measured M, the budget lowered to 1.5 M if that is lower;
- same (M within 0.9 to 1.1 times Measured): unchanged;
- dearer within (M above 1.1 times Measured, every value within the budget): Measured M, the budget unchanged;
- over budget (a value over the budget: a failure its confirmed check measured in every attempt): unchanged, unless
  --rebaseline <metric>=<reason> names it after the procedure of the guide showed a machine change (Measured M, Budget
  1.5 M); a regression is fixed, never budgeted;
- counts (unit count) whose values are all equal: Measured and Budget that count;
- not recorded (fewer than three values): unchanged.

--baseline <files> (with --results) compares two sets of results files: per metric both have, the median of each and the
ratio results / baseline; --max-ratio flags ratios above it (exit code 1), except metrics in ms whose medians are both
below --slack-below (0.5 ms, the GPU timers' resolution) and differ by at most --slack (0.01 ms); counts must be equal.

Exit code: 0 when every run passed (with --results: every recorded value was within its budget; with --baseline: no ratio
was flagged), 1 otherwise, 2 for usage errors. Only the standard library is used.
"""

import argparse
import datetime
import decimal
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
# --wait-quiet: the machine counts as quiet after this many windows of this many seconds in a row below the limit.
QUIET_WINDOW_SECONDS = 10
QUIET_WINDOWS = 3
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


def system_cpu_times():
    """(idle, total) CPU time of the whole machine so far, on Windows; None elsewhere or when it cannot be read."""
    if os.name != "nt":
        return None
    import ctypes
    from ctypes import wintypes
    idle, kernel, user = wintypes.FILETIME(), wintypes.FILETIME(), wintypes.FILETIME()
    if not ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user)):
        return None

    def ticks(filetime):
        return (filetime.dwHighDateTime << 32) | filetime.dwLowDateTime

    # Kernel time includes idle time.
    return ticks(idle), ticks(kernel) + ticks(user)


def machine_load(before):
    """How busy all CPUs of the machine were since `before` (system_cpu_times), in percent, the perf tests included;
    elsewhere the last minute's load average per CPU. None when unknown."""
    if os.name != "nt":
        if not hasattr(os, "getloadavg"):
            return None
        return min(100.0, os.getloadavg()[0] / (os.cpu_count() or 1) * 100.0)
    after = system_cpu_times()
    if before is None or after is None or after[1] <= before[1]:
        return None
    return (1.0 - (after[0] - before[0]) / (after[1] - before[1])) * 100.0


def wait_until_quiet(limit, timeout_minutes):
    """Waits until the machine's CPU load stays below `limit` percent for QUIET_WINDOWS windows of QUIET_WINDOW_SECONDS
    in a row. Returns the seconds it waited, or None when the machine did not become quiet within the timeout."""
    start = time.monotonic()
    quiet_windows = 0
    while quiet_windows < QUIET_WINDOWS:
        if time.monotonic() - start > timeout_minutes * 60:
            return None
        before = system_cpu_times()
        time.sleep(QUIET_WINDOW_SECONDS)
        load = machine_load(before)
        if load is None:
            fail("--wait-quiet needs the machine's CPU load, which this platform does not report")
        quiet_windows = quiet_windows + 1 if load < limit else 0
    return time.monotonic() - start


def run_ctest(arguments, run, output_directory, results_file):
    """Runs the perf tests once; returns the run's record (results file copied next to its log)."""
    command = [arguments.ctest, "--preset", arguments.preset, "-L", arguments.label, "--output-on-failure"]
    log_path = os.path.join(output_directory, f"run{run}.log")
    if arguments.wait_quiet is not None:
        waited = wait_until_quiet(arguments.wait_quiet, arguments.wait_timeout)
        if waited is None:
            print(f"Run {run}: the machine stayed busier than {arguments.wait_quiet:g}% for {arguments.wait_timeout:g} minutes; "
                  "running anyway", flush=True)
        else:
            print(f"Run {run}: waited {waited:.0f} s for the machine to be less than {arguments.wait_quiet:g}% busy", flush=True)
    print(f"Run {run} of {arguments.runs}: {' '.join(command)}", flush=True)
    start = time.monotonic()
    cpu_before = system_cpu_times()
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        try:
            completed = subprocess.run(command, cwd=REPOSITORY_DIRECTORY, stdout=log, stderr=subprocess.STDOUT, check=False)
        except OSError as error:
            fail(f"cannot run '{arguments.ctest}': {error}")
    seconds = time.monotonic() - start
    load = machine_load(cpu_before)

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
    busy = f", machine {load:.0f}% busy" if load is not None else ""
    print(f"Run {run}: {'passed' if passed else 'FAILED'} in {seconds:.0f} s{busy}" + (f" ({', '.join(failed_tests)})" if failed_tests else ""),
          flush=True)
    return {"name": f"run {run}", "passed": passed, "seconds": seconds, "load": load, "failed_tests": failed_tests, "results": results or {},
            "log": log_path}


def over_budget(entry):
    """Whether a results entry is over its budget. Provisional metrics (no budget until the reference machine measures
    them) never are."""
    return isinstance(entry, dict) and not entry.get("Provisional") and entry.get("Value", 0) > entry.get("Budget", 0)


def load_result_files(paths):
    runs = []
    for path in paths:
        results = load_json(path)
        if not isinstance(results, dict):
            fail(f"'{path}' is not a results file")
        within = not any(over_budget(entry) for entry in results.values())
        runs.append({"name": os.path.basename(path), "passed": within, "seconds": None, "load": None, "failed_tests": [], "results": results,
                     "log": None})
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
        busy = f", machine {run['load']:.0f}% busy" if run["load"] is not None else ""
        canary = results.get(CANARY_METRIC, {}).get("Value")
        speed = f"; canary {format_number(canary)} ms" if canary is not None else "; no canary"
        if canary is not None and canary_reference:
            speed += f" ({canary / canary_reference:.2f}x its reference)"
        entries = [entry for entry in results.values() if isinstance(entry, dict)]
        confirmed = sum(1 for entry in entries if len(entry.get("Attempts", [])) > 1)
        over = sum(1 for entry in entries if over_budget(entry))
        # The tally does not tell load from a regression (load has failed single metrics): the run's load and a rerun on a
        # quiet machine do, see the module's description.
        tally = f"; {confirmed} of {len(entries)} metrics needed confirmation, {over} failed" if entries else ""
        print(f"- {run['name']}: {state}{duration}{busy}{speed}{tally}")
        for test in run["failed_tests"]:
            print(f"    failed test: {test}")
        for key in sorted(results):
            entry = results[key]
            attempts = entry.get("Attempts", []) if isinstance(entry, dict) else []
            if isinstance(entry, dict) and not entry.get("Provisional") and (len(attempts) > 1 or over_budget(entry)):
                verdict = "OVER" if over_budget(entry) else "within"
                values = ", ".join(format_number(value) for value in attempts) if attempts else format_number(entry["Value"])
                print(f"    {key}: attempts {values} {entry['Unit']} -> {verdict} the budget of {format_number(entry['Budget'])}")
        if not results:
            print("    (no results file)")

    keys = sorted({key for run in runs for key, entry in run["results"].items() if isinstance(entry, dict)})
    rows = []
    for key in keys:
        entries = [run["results"][key] for run in runs if isinstance(run["results"].get(key), dict)]
        values = [entry["Value"] for entry in entries]
        budget = entries[-1].get("Budget")
        target = entries[-1].get("Target")
        passes = sum(1 for entry in entries if not over_budget(entry))
        confirmations = sum(1 for entry in entries if len(entry.get("Attempts", [])) > 1)
        worst = max(values)
        if entries[-1].get("Provisional"):
            share = "provisional"
        else:
            share = f"{worst / budget * 100:.0f}%" if budget > 0 else ("ok" if worst <= 0 else "over")
        rows.append([key, entries[-1]["Unit"], format_number(budget), format_number(target), len(entries), format_number(min(values)),
                     format_number(statistics.median(values)), format_number(worst), share, f"{passes}/{len(entries)}", confirmations])
    print("\nMetrics (value: the deciding attempt of a confirmed check)")
    if rows:
        print_table(["metric", "unit", "budget", "target", "runs", "min", "median", "max", "max/budget", "passed", "confirmed"], rows)
    else:
        print("(no results)")

    passed_runs = sum(1 for run in runs if run["passed"])
    streak = longest = 0
    for run in runs:
        streak = streak + 1 if run["passed"] else 0
        longest = max(longest, streak)
    print(f"\n{passed_runs} of {len(runs)} runs passed; at most {longest} in a row, the last {streak}.")
    return passed_runs == len(runs)


def significant(value, rounding):
    """`value` with four significant digits, rounded as `rounding` (decimal.ROUND_HALF_EVEN, ROUND_FLOOR)."""
    if value == 0:
        return 0.0
    number = decimal.Decimal(repr(float(value)))
    quantum = decimal.Decimal(1).scaleb(number.adjusted() - 3)
    return float(number.quantize(quantum, rounding=rounding))


def format_entry_number(value, unit):
    if unit == "count":
        return str(int(round(value)))
    return repr(float(value))


def format_entry(entry):
    """An entry in Budgets.json's layout: { "Measured": .., "Budget": .., "Unit": .., "Target": .. }."""
    unit = entry["Unit"]
    parts = [f'"Measured": {format_entry_number(entry["Measured"], unit)}', f'"Budget": {format_entry_number(entry["Budget"], unit)}',
             f'"Unit": "{unit}"']
    if entry.get("Target") is not None:
        parts.append(f'"Target": {format_entry_number(entry["Target"], unit)}')
    return "{ " + ", ".join(parts) + " }"


def classify(key, entry, values, rebaselines):
    """The class of a metric and its proposed entry (None: unchanged), by the rules in the module's description."""
    unit = entry.get("Unit")
    if len(values) < 3:
        return "not recorded", None
    median = statistics.median(values)
    if unit == "count" and len(set(values)) == 1:
        if not entry.get("Provisional") and entry.get("Measured") == values[0] and entry.get("Budget") == values[0]:
            return "same", None
        return "count", {"Measured": values[0], "Budget": values[0], "Unit": unit}
    # A failure confirmed in every attempt is decided by the guide's procedure, however much the metric spreads.
    if not entry.get("Provisional") and any(value > entry["Budget"] for value in values) and key not in rebaselines:
        return "over budget", None
    spread = (max(values) - min(values)) / median if median > 0 else 0.0
    if unit != "count" and spread > 0.25 and key not in rebaselines:
        return "unstable", None
    measured = significant(median, decimal.ROUND_HALF_EVEN)
    # 1.5 times the rounded measurement, rounded down, so the parser's limit holds for the numbers written.
    ceiling = significant(1.5 * measured, decimal.ROUND_FLOOR)
    target = entry.get("Target")
    if entry.get("Provisional"):
        proposed = {"Measured": measured, "Budget": ceiling, "Unit": unit}
        if target is not None:
            if all(value <= target for value in values):
                proposed["Budget"] = min(ceiling, target)
            elif target < ceiling:
                proposed["Target"] = target
        return "provisional", proposed
    budget = entry["Budget"]
    if key in rebaselines:
        proposed = {"Measured": measured, "Budget": ceiling, "Unit": unit}
        if target is not None and target < ceiling:
            proposed["Target"] = target
        return "over budget, re-baselined", proposed
    if median < 0.9 * entry["Measured"]:
        proposed = {"Measured": measured, "Budget": min(budget, ceiling), "Unit": unit}
    elif median > 1.1 * entry["Measured"]:
        proposed = {"Measured": measured, "Budget": min(budget, ceiling), "Unit": unit}
    else:
        return "same", None
    if target is not None and target < proposed["Budget"]:
        proposed["Target"] = target
    return "cheaper" if median < entry["Measured"] else "dearer within", proposed


def propose_budgets(runs, output, rebaseline_arguments):
    """Writes the proposed Budgets.json; returns whether no metric over its budget was left undecided."""
    rebaselines = {}
    for argument in rebaseline_arguments or []:
        metric, separator, reason = argument.partition("=")
        if not separator or not reason.strip():
            fail(f"--rebaseline needs <metric>=<reason>, got '{argument}'")
        rebaselines[metric] = reason.strip()
    with open(BUDGET_FILE, "r", encoding="utf-8") as file:
        lines = file.read().split("\n")
    budgets = load_json(BUDGET_FILE)
    for metric in rebaselines:
        if metric not in budgets:
            fail(f"--rebaseline names {metric}, which Budgets.json has not")
    changes = []
    undecided = []
    for index, line in enumerate(lines):
        stripped = line.strip()
        if not stripped.startswith('"') or stripped.startswith('"_comment"'):
            continue
        key = stripped.split('"')[1]
        entry = budgets.get(key)
        if not isinstance(entry, dict):
            continue
        values = [run["results"][key]["Value"] for run in runs if isinstance(run["results"].get(key), dict) and "Value" in run["results"][key]]
        kind, proposed = classify(key, entry, values, rebaselines)
        detail = (f"median {format_number(statistics.median(values))} of {len(values)}, {format_number(min(values))}-{format_number(max(values))}"
                  if values else "no values")
        if proposed is not None:
            indentation = line[:len(line) - len(line.lstrip())]
            comma = "," if stripped.endswith(",") else ""
            lines[index] = f'{indentation}"{key}": {format_entry(proposed)}{comma}'
            before = "provisional" if entry.get("Provisional") else f"Measured {format_number(entry['Measured'])}, Budget {format_number(entry['Budget'])}"
            after = f"Measured {format_number(proposed['Measured'])}, Budget {format_number(proposed['Budget'])}"
            if proposed.get("Target") is not None:
                after += f", Target {format_number(proposed['Target'])}"
            elif entry.get("Target") is not None:
                after += " (target dropped)"
            reason = f"; {rebaselines[key]}" if key in rebaselines else ""
            changes.append((kind, f"{key}: {before} -> {after} ({detail}){reason}"))
        elif kind != "same":
            changes.append((kind, f"{key}: unchanged ({detail})"))
            if kind == "over budget":
                undecided.append(key)
    with open(output, "w", encoding="utf-8", newline="\n") as file:
        file.write("\n".join(lines))
    for kind in ("count", "provisional", "cheaper", "dearer within", "over budget, re-baselined", "over budget", "unstable", "not recorded"):
        entries = [text for entry_kind, text in changes if entry_kind == kind]
        if entries:
            print(f"\n{kind} ({len(entries)}):")
            for text in entries:
                print(f"  {text}")
    print(f"\nWrote {output}" + (f"; {len(undecided)} metrics over budget left to the procedure of the guide" if undecided else ""))
    return not undecided


def compare_baseline(runs, baseline_runs, max_ratio, slack, slack_below):
    """Per metric of both sets: the medians and their ratio; returns whether no metric was flagged."""
    keys = sorted({key for run in runs for key in run["results"]} & {key for run in baseline_runs for key in run["results"]})
    rows = []
    flagged = []
    for key in keys:
        values = [run["results"][key]["Value"] for run in runs if isinstance(run["results"].get(key), dict)]
        base = [run["results"][key]["Value"] for run in baseline_runs if isinstance(run["results"].get(key), dict)]
        if not values or not base:
            continue
        unit = next(run["results"][key].get("Unit") for run in runs if isinstance(run["results"].get(key), dict))
        median, base_median = statistics.median(values), statistics.median(base)
        if unit == "count":
            verdict = "ok" if median == base_median else "DIFFERS"
            ratio = "-"
        else:
            ratio_value = median / base_median if base_median > 0 else (1.0 if median <= 0 else float("inf"))
            ratio = f"{ratio_value:.3f}"
            within_slack = unit == "ms" and median < slack_below and base_median < slack_below and abs(median - base_median) <= slack
            verdict = "ok" if max_ratio is None or ratio_value <= max_ratio or within_slack else "OVER"
        if verdict != "ok":
            flagged.append(key)
        rows.append([key, unit, format_number(base_median), format_number(median), ratio, verdict])
    print_table(["metric", "unit", "baseline", "results", "ratio", ""], rows)
    print(f"\n{len(rows)} metrics compared" + (f"; flagged: {', '.join(flagged)}" if flagged else "; none flagged"))
    return not flagged


def main():
    parser = argparse.ArgumentParser(description="Runs the perf tests several times and reports the spread and pass rate of every metric.")
    parser.add_argument("--preset", default="windows-release", help="CTest test preset (default: windows-release)")
    parser.add_argument("--runs", type=int, default=5, help="how many times to run the perf tests (default: 5)")
    parser.add_argument("--label", default="perf", help="CTest label to run (default: perf)")
    parser.add_argument("--ctest", default="ctest", help="the ctest executable (default: ctest on PATH)")
    parser.add_argument("--output", help="directory for the logs and results of the runs (default: a new PerfStability-<time> directory "
                        "next to the results file)")
    parser.add_argument("--wait-quiet", type=float, metavar="PERCENT", help="before each run, wait until the machine's CPU load stays "
                        f"below this many percent for {QUIET_WINDOWS * QUIET_WINDOW_SECONDS} s (machines shared with other work)")
    parser.add_argument("--wait-timeout", type=float, default=60.0, metavar="MINUTES", help="how long --wait-quiet waits at most before "
                        "a run starts anyway (default: 60)")
    parser.add_argument("--results", nargs="+", metavar="FILE", help="report on these results files instead of running the tests")
    parser.add_argument("--propose-budgets", metavar="FILE", help="with --results: write Budgets.json as the rules would change it")
    parser.add_argument("--rebaseline", action="append", metavar="METRIC=REASON", help="with --propose-budgets: re-baseline this metric "
                        "over its budget, for the reason given (a machine change the guide's procedure showed)")
    parser.add_argument("--baseline", nargs="+", metavar="FILE", help="with --results: compare them with these results files")
    parser.add_argument("--max-ratio", type=float, help="with --baseline: flag metrics whose median rose by more than this ratio")
    parser.add_argument("--slack", type=float, default=0.01, help="with --baseline: ms within which medians below --slack-below are equal")
    parser.add_argument("--slack-below", type=float, default=0.5, help="with --baseline: medians (ms) below which --slack applies")
    arguments = parser.parse_args()

    budgets = load_json(BUDGET_FILE)
    if (arguments.propose_budgets or arguments.baseline) and not arguments.results:
        fail("--propose-budgets and --baseline need --results")
    if arguments.rebaseline and not arguments.propose_budgets:
        fail("--rebaseline needs --propose-budgets")
    if arguments.propose_budgets:
        runs = load_result_files(arguments.results)
        report(runs, budgets)
        sys.exit(0 if propose_budgets(runs, arguments.propose_budgets, arguments.rebaseline) else 1)
    if arguments.baseline:
        sys.exit(0 if compare_baseline(load_result_files(arguments.results), load_result_files(arguments.baseline), arguments.max_ratio,
                                       arguments.slack, arguments.slack_below) else 1)
    if arguments.results:
        runs = load_result_files(arguments.results)
    else:
        if arguments.runs < 1:
            fail("--runs needs a positive number")
        if arguments.wait_quiet is not None and not 0 < arguments.wait_quiet <= 100:
            fail("--wait-quiet needs a percentage above 0 and at most 100")
        if arguments.wait_timeout <= 0:
            fail("--wait-timeout needs a positive number of minutes")
        results_file = resolve_results_file(arguments.preset)
        output_directory = arguments.output or os.path.join(os.path.dirname(results_file),
                                                            "PerfStability-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
        os.makedirs(output_directory, exist_ok=True)
        print(f"Logs and results go to {output_directory}")
        runs = [run_ctest(arguments, run, output_directory, results_file) for run in range(1, arguments.runs + 1)]
    sys.exit(0 if report(runs, budgets) else 1)


if __name__ == "__main__":
    main()
