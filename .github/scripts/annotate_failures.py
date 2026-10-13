"""Turns build and test failures in CI logs into GitHub annotations, so they can be read from the checks API and the
pull request UI without opening the full logs.

Usage: annotate_failures.py <log>...  (missing logs are skipped)

Emits one annotation per located compiler/linker/test error (the first few, with doctest's values and logged messages),
one listing the doctest test cases that reported errors, and the surrounding output (the failing tests' output and the
lines around each error) as summary annotations. GitHub keeps 4096 bytes of an annotation's message and 10 error
annotations per step, so the summary is cut into a few annotations of at most c_MaxAnnotationBytes: its start (the
first errors) and its end (where a crash shows)."""
import os
import re
import sys

c_MaxLocatedErrors = 5
c_MaxSummaryAnnotations = 3
c_MaxAnnotationBytes = 3900
c_MaxLogCharacters = 15000
c_ContextLines = 6
c_TailLines = 40
c_MaxDetailLines = 3

PATTERNS = [
    # GCC, Clang and doctest built with them: path:line[:column]: [fatal] error: message / path:line: ERROR: message /
    # path:line: FATAL ERROR: message (doctest's failed REQUIREs and crashes)
    re.compile(r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?:(?P<column>\d+):)?\s*(?:(?:fatal|FATAL) )?(?:error|ERROR):\s*(?P<message>.*)$"),
    # MSVC and doctest built with it: path(line[,column]): [fatal] error C1234: message / path(line): ERROR: message
    re.compile(r"^\s*(?P<file>[^\s(][^(]*)\((?P<line>\d+)(?:,(?P<column>\d+))?\):\s*(?:(?:fatal|FATAL) )?(?:error|ERROR)(?:\s+\w+)?:\s*(?P<message>.*)$"),
]
# doctest prints what a failed assertion compared and the messages logged with it on the indented lines after it.
DETAIL = re.compile(r"^\s+(values|logged|with expansion|exception):")
# doctest's console reporter names the test case before its errors.
TEST_CASE = re.compile(r"^TEST CASE:\s+(?P<name>.*)$")
TEST_SUITE = re.compile(r"^TEST SUITE:\s+(?P<name>.*)$")
UNLOCATED = re.compile(
    r"(undefined reference|Undefined symbols|ld: error|ld: symbol|error LNK|collect2|CMake Error|\*\*\*Failed|\*\*\*Exception|"
    r"Subprocess aborted|Timeout|SegFault|FATAL ERROR|Fatal signal|Output flood|terminate called|Assertion|Sanitizer|"
    r"The following tests FAILED)")


FAILED_TEST = re.compile(r"Test\s+#\d+:.*\*\*\*(Failed|Exception|Timeout)")
NEXT_TEST = re.compile(r"^\s+Start\s+\d+:|tests passed|tests failed")


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


def split_utf8(text, limit):
    """Cuts text at line ends into pieces of at most `limit` UTF-8 bytes (a longer line is cut itself)."""
    pieces = []
    current = []
    size = 0
    for line in text.split("\n"):
        encoded = line.encode("utf-8")
        if len(encoded) > limit:
            line = encoded[:limit].decode("utf-8", errors="ignore")
            encoded = line.encode("utf-8")
        if current and size + len(encoded) + 1 > limit:
            pieces.append("\n".join(current))
            current = []
            size = 0
        current.append(line)
        size += len(encoded) + 1
    if current:
        pieces.append("\n".join(current))
    return pieces


def main():
    located = []
    seen = set()
    summary = []
    failed_cases = []
    for log in sys.argv[1:]:
        if not os.path.exists(log):
            continue
        with open(log, encoding="utf-8", errors="replace") as file:
            lines = [line.rstrip("\n") for line in file]
        marked = set()
        suite = ""
        case = ""
        for index, line in enumerate(lines):
            suite_match = TEST_SUITE.match(line)
            if suite_match:
                suite = suite_match.group("name")
            case_match = TEST_CASE.match(line)
            if case_match:
                case = case_match.group("name")
            match = None
            for pattern in PATTERNS:
                match = pattern.match(line)
                if match:
                    break
            if match:
                message = match.group("message")
                details = []
                for later in lines[index + 1:index + 1 + c_MaxDetailLines]:
                    if not DETAIL.match(later):
                        break
                    details.append(later.strip())
                if details:
                    message += "\n" + "\n".join(details)
                key = (match.group("file"), match.group("line"), message)
                if key not in seen:
                    seen.add(key)
                    located.append((relative(match.group("file")), match.group("line"), match.group("column"), message))
                if case:
                    name = "%s / %s: %s:%s" % (suite, case, relative(match.group("file")), match.group("line"))
                    if name not in failed_cases:
                        failed_cases.append(name)
                marked.update(range(max(0, index - c_ContextLines), min(len(lines), index + c_ContextLines + 1)))
            elif UNLOCATED.search(line):
                marked.update(range(max(0, index - c_ContextLines), min(len(lines), index + c_ContextLines + 1)))
        # CTest prints a failed test's output after its result line; the end of that output names the test case a
        # crashed process was running (STRATA_TEST_TRACE), so keep the lines before the next test starts.
        for index, line in enumerate(lines):
            if FAILED_TEST.search(line):
                end = next((later for later in range(index + 1, len(lines)) if NEXT_TEST.search(lines[later])), len(lines))
                marked.update(range(max(index, end - c_TailLines), end))
        marked.update(range(max(0, len(lines) - c_TailLines), len(lines)))
        if marked and (located or any(UNLOCATED.search(lines[index]) for index in marked)):
            section = []
            previous = None
            for index in sorted(marked):
                if previous is not None and index != previous + 1:
                    section.append("...")
                section.append(lines[index])
                previous = index
            # Each log gets its own budget, keeping its start (first errors) and its end (where a crash shows), so one
            # long log cannot crowd out another.
            text = "\n".join(section)
            if len(text) > c_MaxLogCharacters:
                half = c_MaxLogCharacters // 2
                text = text[:half] + "\n... (cut) ...\n" + text[-half:]
            summary.append("==== %s ====" % os.path.basename(log))
            summary.append(text)

    for path, line, column, message in located[:c_MaxLocatedErrors]:
        properties = "file=%s,line=%s" % (escape_property(path), line)
        if column:
            properties += ",col=%s" % column
        print("::error %s::%s" % (properties, escape_data(split_utf8(message, c_MaxAnnotationBytes)[0])))

    # The test cases that reported errors (doctest prints expected failures too, so a passing suite can be listed).
    if failed_cases:
        listing = split_utf8("\n".join(failed_cases), c_MaxAnnotationBytes)[0]
        print("::error title=Test cases with errors::%s" % escape_data(listing))

    pieces = split_utf8("\n".join(summary), c_MaxAnnotationBytes) if summary else []
    if len(pieces) > c_MaxSummaryAnnotations:
        pieces = pieces[:1] + pieces[-(c_MaxSummaryAnnotations - 1):]
    for number, piece in enumerate(pieces, 1):
        print("::error title=Failure summary %d of %d::%s" % (number, len(pieces), escape_data(piece)))
    if not pieces:
        print("::error title=Failure summary::No error lines were recognized in %s" % escape_data(", ".join(sys.argv[1:])))


if __name__ == "__main__":
    main()
