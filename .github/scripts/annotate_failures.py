"""Turns build and test failures in CI logs into GitHub annotations, so they can be read from the checks API and the
pull request UI without opening the full logs.

Usage: annotate_failures.py <log>...  (missing logs are skipped)

Emits one annotation per located compiler/linker/test error (the first few) and a summary annotation with the
surrounding output: the failing tests' output and the lines around each error."""
import os
import re
import sys

c_MaxLocatedErrors = 8
c_MaxSummaryCharacters = 60000
c_ContextLines = 6

PATTERNS = [
    # GCC, Clang and doctest built with them: path:line[:column]: [fatal] error: message / path:line: ERROR: message
    re.compile(r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?:(?P<column>\d+):)?\s*(?:fatal )?(?:error|ERROR):\s*(?P<message>.*)$"),
    # MSVC and doctest built with it: path(line[,column]): [fatal] error C1234: message / path(line): ERROR: message
    re.compile(r"^\s*(?P<file>[^\s(][^(]*)\((?P<line>\d+)(?:,(?P<column>\d+))?\):\s*(?:fatal )?(?:error|ERROR)(?:\s+\w+)?:\s*(?P<message>.*)$"),
]
UNLOCATED = re.compile(
    r"(undefined reference|Undefined symbols|ld: |error LNK|collect2|CMake Error|\*\*\*Failed|\*\*\*Exception|"
    r"Subprocess aborted|Timeout|SegFault|FATAL ERROR|Fatal signal|terminate called|Assertion|Sanitizer|"
    r"The following tests FAILED)")


def escape_data(text):
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def escape_property(text):
    return escape_data(text).replace(":", "%3A").replace(",", "%2C")


def relative(path):
    workspace = os.environ.get("GITHUB_WORKSPACE", "")
    normalized = path.replace("\\", "/")
    if workspace:
        prefix = workspace.replace("\\", "/").rstrip("/") + "/"
        if normalized.lower().startswith(prefix.lower()):
            return normalized[len(prefix):]
    return normalized


def main():
    located = []
    seen = set()
    summary = []
    for log in sys.argv[1:]:
        if not os.path.exists(log):
            continue
        with open(log, encoding="utf-8", errors="replace") as file:
            lines = [line.rstrip("\n") for line in file]
        marked = set()
        for index, line in enumerate(lines):
            match = None
            for pattern in PATTERNS:
                match = pattern.match(line)
                if match:
                    break
            if match:
                key = (match.group("file"), match.group("line"), match.group("message"))
                if key not in seen:
                    seen.add(key)
                    located.append((relative(match.group("file")), match.group("line"), match.group("column"), match.group("message")))
                marked.update(range(max(0, index - c_ContextLines), min(len(lines), index + c_ContextLines + 1)))
            elif UNLOCATED.search(line):
                marked.update(range(max(0, index - c_ContextLines), min(len(lines), index + c_ContextLines + 1)))
        if marked:
            summary.append("==== %s ====" % os.path.basename(log))
            previous = None
            for index in sorted(marked):
                if previous is not None and index != previous + 1:
                    summary.append("...")
                summary.append(lines[index])
                previous = index

    for path, line, column, message in located[:c_MaxLocatedErrors]:
        properties = "file=%s,line=%s" % (escape_property(path), line)
        if column:
            properties += ",col=%s" % column
        print("::error %s::%s" % (properties, escape_data(message)))

    text = "\n".join(summary)
    if len(text) > c_MaxSummaryCharacters:
        text = text[:c_MaxSummaryCharacters] + "\n... (cut)"
    if text:
        print("::error title=Failure summary::%s" % escape_data(text))
    else:
        print("::error title=Failure summary::No error lines were recognized in %s" % escape_data(", ".join(sys.argv[1:])))


if __name__ == "__main__":
    main()
