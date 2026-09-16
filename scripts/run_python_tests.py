#!/usr/bin/env python3
"""Run the repository's Python tests from any working directory.

Two discovery passes are needed: the suite uses both ``*_test.py`` and
``*_validation.py`` naming, and a single ``unittest discover`` pattern only
matches one of them.

Unittest returns 5 when a suite is empty. That is tolerated per pattern, but
it is reported loudly and an empty result across *every* pattern is fatal --
the tests directory has been deleted once already, and a silent pass against a
missing directory is indistinguishable from a green run.
"""

from pathlib import Path
import subprocess
import sys

PATTERNS = ("*_test.py", "*_validation.py")


def discover(repo_root, pattern, extra_args):
    command = [
        sys.executable,
        "-m",
        "unittest",
        "discover",
        "-t",
        str(repo_root),
        "-s",
        str(repo_root / "tests"),
        "-p",
        pattern,
        *extra_args,
    ]
    print(f"==> unittest discover -p {pattern}", flush=True)
    # Discovery imports test modules relative to the repo root, so pin the
    # child's working directory instead of inheriting the caller's.
    return subprocess.run(command, cwd=str(repo_root)).returncode


def main():
    repo_root = Path(__file__).resolve().parent.parent
    tests_dir = repo_root / "tests"
    if not tests_dir.is_dir():
        print(f"error: {tests_dir} does not exist", file=sys.stderr)
        return 1

    extra_args = sys.argv[1:]
    empty = []
    for pattern in PATTERNS:
        status = discover(repo_root, pattern, extra_args)
        if status == 5:
            print(f"warning: no tests matched {pattern}", file=sys.stderr)
            empty.append(pattern)
            continue
        if status != 0:
            return status

    if len(empty) == len(PATTERNS):
        print("error: no tests matched any pattern", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
