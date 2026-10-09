"""Runs a command and copies its output, bounded, so a process that floods its output cannot exhaust the CI machine
(CTest keeps a test's whole output in memory).

Usage: run_limited.py <log file> <max output bytes> <command> [arguments...]

The first part of the output and every "[test case]" line (STRATA_TEST_TRACE) are kept in the log and echoed. Once the
output exceeds the limit, the process is killed, and the most repeated lines and the test case that was running are
reported. Exits with the command's exit code, or 3 when the output limit was hit."""
import collections
import subprocess
import sys

c_KeptBytes = 4 * 1024 * 1024
c_TestCasePrefix = "[test case]"


def main():
    log_path, limit, command = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    total = 0
    kept = 0
    current_test = "(none)"
    counts = collections.Counter()
    flooded = False
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        for raw in process.stdout:
            line = raw.decode("utf-8", errors="replace").rstrip("\n")
            total += len(raw)
            if line.startswith(c_TestCasePrefix):
                current_test = line[len(c_TestCasePrefix):].strip()
            elif kept >= c_KeptBytes:
                counts[line[:300]] += 1
            if kept < c_KeptBytes or line.startswith(c_TestCasePrefix):
                kept += len(raw)
                log.write(line + "\n")
                print(line, flush=True)
            if total > limit:
                flooded = True
                process.kill()
                break
        process.wait()
        if flooded:
            report = ["Output flood: more than %d bytes; the process was killed while running test case: %s" % (limit, current_test),
                      "Most repeated lines after the first %d bytes:" % c_KeptBytes]
            report += ["  %8d x %s" % (count, text) for text, count in counts.most_common(10)]
            for line in report:
                log.write(line + "\n")
                print(line, flush=True)
            return 3
    return process.returncode


if __name__ == "__main__":
    sys.exit(main())
