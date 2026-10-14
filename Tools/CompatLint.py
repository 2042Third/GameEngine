#!/usr/bin/env python3
"""Compiles translation units with the compilers closest to CI's, without writing objects, so that what CI's older
compilers and standard libraries reject is found before a push (AGENTS.md, "CI before main").

    python3 Tools/CompatLint.py --changed-since main      # the .cpp files changed since main, and new ones
    python3 Tools/CompatLint.py Strata/src/Strata/Scene/Scene.cpp StrataTests/src/Scene/SceneTests.cpp
    python3 Tools/CompatLint.py --all                     # every translation unit of the build

- GCC 14 stands in for CI's Linux compiler (and its libstdc++), with CI's warnings as errors. It skips `src/Platform`,
  whose macOS code GCC cannot compile; CI builds the Linux code there.
- LLVM 16 with its own libc++ stands in for CI's Apple Clang 15 and its libc++ (which, for one, has no floating-point
  `std::from_chars`).

Each file is compiled with the exact command of the Debug build (a Ninja Multi-Config build tree: the macos and linux
presets), its compiler swapped and the precompiled header replaced by the header it wraps. The build must have been
configured (and built once, for generated headers). On macOS the compilers come from Homebrew:
`brew install gcc@14 llvm@16`; --gcc and --clang name others.

A changed header affects every file that includes it: pass those files, or --all.

Exit code: 0 when every file compiled without errors or warnings, 1 otherwise, 2 for usage errors. Only the standard
library is used.
"""

import argparse
import concurrent.futures
import os
import re
import shlex
import subprocess
import sys

c_Root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))


def run(args, cwd=None):
    return subprocess.run(args, cwd=cwd, capture_output=True, text=True)


def changed_sources(base):
    tracked = run(['git', 'diff', '--name-only', base], cwd=c_Root).stdout.split()
    untracked = run(['git', 'ls-files', '--others', '--exclude-standard'], cwd=c_Root).stdout.split()
    return sorted({path for path in tracked + untracked if path.endswith('.cpp') and os.path.exists(os.path.join(c_Root, path))})


def build_commands(build):
    """Maps each source file (absolute) to its Debug compile command."""
    ninja = run(['ninja', '-C', build, '-f', 'build-Debug.ninja', '-t', 'compdb'])
    if ninja.returncode != 0:
        raise RuntimeError('cannot read the Debug build of %s: %s' % (build, ninja.stderr.strip()))
    import json
    commands = {}
    for entry in json.loads(ninja.stdout):
        source = os.path.normpath(os.path.join(entry['directory'], entry['file']))
        command = entry.get('command')
        if command and source.endswith('.cpp') and ' -o ' in command:
            commands[source] = command
    return commands


def adapt(command, build, compiler, options):
    args = shlex.split(command)
    out = []
    index = 1
    while index < len(args):
        arg = args[index]
        if arg in ('-MD', '-c', '-Winvalid-pch') or arg.startswith('-mmacosx-version-min'):
            index += 1
        elif arg in ('-MT', '-MF', '-o', '-arch'):
            index += 2
        elif arg == '-Xclang' and index + 1 < len(args) and args[index + 1] == '-include-pch':
            index += 4
        elif arg == '-Xclang' and index + 1 < len(args) and args[index + 1] == '-include':
            # CMake's wrapper of the precompiled header (a clang pragma and the real header): the header itself, since
            # compilers use a .pch they find beside a forced include on their own.
            with open(os.path.join(build, args[index + 3]), encoding='utf-8') as wrapper:
                for line in wrapper:
                    match = re.match(r'\s*#\s*include\s+"(.+)"', line)
                    if match:
                        out += ['-include', match.group(1)]
            index += 4
        else:
            out.append(arg)
            index += 1
    if compiler == 'clang':
        prefix = [options.clang, '-nostdinc++', '-isystem', options.libcxx]
        if sys.platform == 'darwin':
            sdk = run(['xcrun', '--show-sdk-path']).stdout.strip()
            prefix += ['-isysroot', sdk, '-mmacosx-version-min=14.0']
        return prefix + out + ['-fsyntax-only']
    return [options.gcc] + out + ['-fsyntax-only']


def lint(source, command, compiler, options):
    relative = os.path.relpath(source, c_Root)
    if compiler == 'gcc' and sys.platform == 'darwin' and '/src/Platform/' in source:
        return relative, compiler, None, True
    result = run(adapt(command, options.build, compiler, options), cwd=options.build)
    messages = [line for line in result.stderr.split('\n') if re.search(r'\b(error|warning)\b', line)]
    if result.returncode != 0 or messages:
        return relative, compiler, '\n'.join(messages[:20]) or result.stderr[:2000], False
    return relative, compiler, None, False


def main():
    parser = argparse.ArgumentParser(description='Compiles translation units with compilers close to CI\'s.')
    parser.add_argument('files', nargs='*', help='source files (.cpp), relative to the repository')
    parser.add_argument('--changed-since', metavar='REF', help='the .cpp files changed since a git ref, and new ones')
    parser.add_argument('--all', action='store_true', help='every translation unit of the build')
    parser.add_argument('--build', default=os.path.join(c_Root, 'build', 'macos' if sys.platform == 'darwin' else 'linux'),
        help='the build tree (default build/macos or build/linux)')
    parser.add_argument('--gcc', default='/opt/homebrew/bin/g++-14')
    parser.add_argument('--clang', default='/opt/homebrew/opt/llvm@16/bin/clang++')
    parser.add_argument('--libcxx', default='/opt/homebrew/opt/llvm@16/include/c++/v1', help='the libc++ headers for --clang')
    options = parser.parse_args()
    options.build = os.path.abspath(options.build)

    for tool in (options.gcc, options.clang):
        if not os.path.exists(tool):
            print('CompatLint: %s is not installed (brew install gcc@14 llvm@16)' % tool, file=sys.stderr)
            return 2
    try:
        commands = build_commands(options.build)
    except RuntimeError as error:
        print('CompatLint: %s' % error, file=sys.stderr)
        return 2

    if options.all:
        sources = sorted(source for source in commands if source.startswith(c_Root + os.sep) and '/vendor/' not in source)
    else:
        files = list(options.files) + (changed_sources(options.changed_since) if options.changed_since else [])
        sources = sorted({os.path.normpath(os.path.join(c_Root, path)) for path in files if path.endswith('.cpp')})
    if not sources:
        print('CompatLint: no source files to compile', file=sys.stderr)
        return 2
    missing = [os.path.relpath(source, c_Root) for source in sources if source not in commands]
    if missing:
        print('CompatLint: not in the build (configure again?): %s' % ', '.join(missing), file=sys.stderr)
        return 2

    jobs = [(source, commands[source], compiler) for source in sources for compiler in ('gcc', 'clang')]
    failures = 0
    skipped = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for relative, compiler, problem, skip in pool.map(lambda job: lint(*job, options), jobs):
            if skip:
                skipped += 1
            elif problem:
                failures += 1
                print('FAIL %s [%s]\n%s' % (relative, compiler, problem))
    print('CompatLint: %d files, %d compilations failed, %d skipped (platform code with GCC on macOS)' % (len(sources), failures, skipped))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
