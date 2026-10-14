#!/usr/bin/env python3
"""Runs a command while it holds the machine-wide perf lock, so that measurements of agents and people sharing a machine
never overlap (Docs/DeveloperGuide.md, "Perf tests": A/B measurements while several agents work on one machine).

    python Tools/PerfLock.py --label <who> -- <command> [arguments...]
    python Tools/PerfLock.py --status

The lock is a file in the user's temporary directory, locked with the operating system's file locks (fcntl on Linux and
macOS, msvcrt on Windows), so it is released whenever its holder ends, also when it crashes. A holder file beside it names
who holds it, since when and what runs, for --status.

- Waits for the lock up to --wait minutes (30 by default), then exits 75 without running the command.
- Ends the command after --limit minutes (25 by default) and exits 124; otherwise exits with the command's exit code.
- Prints a line to stderr when it has to wait, naming the holder, and when it starts the command.

Exit codes: the command's, 75 when the lock was not acquired in time, 124 when the command ran out of time, 2 for usage
errors. Only the standard library is used.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

if os.name == 'nt':
    import msvcrt
else:
    import fcntl

c_ExitNotAcquired = 75
c_ExitTimedOut = 124
c_LockDirectory = os.path.join(tempfile.gettempdir(), 'StrataPerfLock')
c_LockFile = os.path.join(c_LockDirectory, 'perf.lock')
c_HolderFile = os.path.join(c_LockDirectory, 'holder.json')


def try_lock(handle):
    """Takes the lock without waiting; False when another process holds it."""
    try:
        if os.name == 'nt':
            handle.seek(0)
            msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError:
        return False


def unlock(handle):
    if os.name == 'nt':
        handle.seek(0)
        msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
    else:
        fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def read_holder():
    try:
        with open(c_HolderFile, encoding='utf-8') as file:
            return json.load(file)
    except (OSError, ValueError):
        return None


def describe(holder):
    if not holder:
        return 'unknown holder'
    started = time.strftime('%H:%M:%S', time.localtime(holder.get('started', 0)))
    return '%s (pid %s, since %s): %s' % (holder.get('label', '?'), holder.get('pid', '?'), started, ' '.join(holder.get('command', [])))


def is_locked():
    """Whether another process holds the lock now (takes and drops it when free)."""
    os.makedirs(c_LockDirectory, exist_ok=True)
    with open(c_LockFile, 'a+') as handle:
        if try_lock(handle):
            unlock(handle)
            return False
        return True


def main():
    parser = argparse.ArgumentParser(description='Runs a command under the machine-wide perf lock.')
    parser.add_argument('--label', help='who measures (an agent\'s stream key, a person\'s name)')
    parser.add_argument('--wait', type=float, default=30.0, help='minutes to wait for the lock (default 30)')
    parser.add_argument('--limit', type=float, default=25.0, help='minutes the command may run (default 25)')
    parser.add_argument('--status', action='store_true', help='print who holds the lock and exit')
    parser.add_argument('command', nargs=argparse.REMAINDER, help='-- <command> [arguments...]')
    options = parser.parse_args()

    if options.status:
        print('held by ' + describe(read_holder()) if is_locked() else 'free')
        return 0
    command = options.command[1:] if options.command[:1] == ['--'] else options.command
    if not command or not options.label:
        parser.print_usage(sys.stderr)
        print('PerfLock.py: --label and a command after -- are required', file=sys.stderr)
        return 2

    os.makedirs(c_LockDirectory, exist_ok=True)
    with open(c_LockFile, 'a+') as handle:
        deadline = time.monotonic() + options.wait * 60.0
        announced = False
        while not try_lock(handle):
            if time.monotonic() >= deadline:
                print('PerfLock: not acquired within %g minutes, held by %s' % (options.wait, describe(read_holder())), file=sys.stderr)
                return c_ExitNotAcquired
            if not announced:
                print('PerfLock: waiting for %s' % describe(read_holder()), file=sys.stderr)
                announced = True
            time.sleep(2.0)
        try:
            with open(c_HolderFile, 'w', encoding='utf-8') as file:
                json.dump({'label': options.label, 'pid': os.getpid(), 'started': time.time(), 'command': command}, file)
            print('PerfLock: %s runs %s' % (options.label, ' '.join(command)), file=sys.stderr)
            try:
                return subprocess.run(command, timeout=options.limit * 60.0).returncode
            except subprocess.TimeoutExpired:
                print('PerfLock: the command ran longer than %g minutes and was ended' % options.limit, file=sys.stderr)
                return c_ExitTimedOut
            except OSError as error:
                print('PerfLock: cannot run %s: %s' % (command[0], error), file=sys.stderr)
                return 2
        finally:
            try:
                os.remove(c_HolderFile)
            except OSError:
                pass
            unlock(handle)


if __name__ == '__main__':
    sys.exit(main())
