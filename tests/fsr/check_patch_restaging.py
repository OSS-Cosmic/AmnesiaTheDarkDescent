#!/usr/bin/env python3
"""Check FSR SDK patch restaging across configure and rebuild cycles."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Callable


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SDK_ROOT = (
    REPO_ROOT / "build-premake" / "_deps" / "fsr-sdk" / "FidelityFX-SDK-1.1.4"
)
DEFAULT_VULKAN_HEADERS = REPO_ROOT / "HPL2" / "extern" / "Vulkan-Headers" / "include"
DEFAULT_VOLK_HEADERS = REPO_ROOT / "HPL2" / "extern" / "volk"
DRIVER = REPO_ROOT / "scripts" / "compile_fsr_shaders.py"
NO_WORK_OUTPUT = "ninja: no work to do."
BRACKET_RE = re.compile(
    r"\[(?P<equals>=*)\[(?P<body>.*?)\](?P=equals)\]", re.DOTALL
)
BRACKET_OPEN_RE = re.compile(r"\[(?P<equals>=*)\[")


class CheckError(RuntimeError):
    """A user-facing checker failure."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-root", type=Path, default=DEFAULT_SDK_ROOT)
    parser.add_argument(
        "--vulkan-headers", type=Path, default=DEFAULT_VULKAN_HEADERS
    )
    parser.add_argument("--volk-headers", type=Path, default=DEFAULT_VOLK_HEADERS)
    parser.add_argument(
        "--glslang",
        type=Path,
        help="glslangValidator or glslang executable; discovered from PATH by default",
    )
    parser.add_argument(
        "--python",
        type=Path,
        help="Python 3 executable used by CMake; discovered from PATH by default",
    )
    parser.add_argument(
        "--temp-root",
        type=Path,
        default=Path(tempfile.gettempdir()),
        help="parent for the disposable work root (default: platform temp directory)",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        help="optional parallel job count forwarded to CMake",
    )
    return parser.parse_args()


def find_executable(value: str | Path) -> Path | None:
    found = shutil.which(os.fspath(value))
    if found is None:
        return None
    return Path(found).resolve()


def explicit_or_path_tool(value: Path | None, names: tuple[str, ...]) -> Path | None:
    if value is not None:
        return find_executable(value)
    for name in names:
        found = find_executable(name)
        if found is not None:
            return found
    return None


def quoted_tail(output: str, line_count: int = 24) -> str:
    lines = output.rstrip().splitlines()
    tail = "\n".join(lines[-line_count:]) if lines else "(no output)"
    return f'"""\n{tail}\n"""'


def run_command(label: str, command: list[str]) -> str:
    print(f"[{label}] running: {' '.join(command)}", flush=True)
    result = subprocess.run(
        command,
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    if result.returncode != 0:
        raise CheckError(
            f"{label} failed with exit {result.returncode}; output tail:\n"
            f"{quoted_tail(result.stdout)}"
        )
    return result.stdout


def configure_command(
    cmake: Path,
    source_root: Path,
    build_root: Path,
    args: argparse.Namespace,
    python: Path,
    glslang: Path,
) -> list[str]:
    return [
        str(cmake),
        "-S",
        str(source_root / "cmake" / "fsr"),
        "-B",
        str(build_root),
        "-G",
        "Ninja",
        f"-DFSR_SDK_ROOT={args.sdk_root}",
        f"-DFSR_VULKAN_HEADERS={args.vulkan_headers}",
        f"-DFSR_VOLK_HEADERS={args.volk_headers}",
        f"-DFSR_OUTPUT_LIB_DIR={build_root / 'lib'}",
        f"-DPython3_EXECUTABLE={python}",
        f"-DFSR_GLSLANG={glslang}",
        "-DCMAKE_BUILD_TYPE=Release",
    ]


def build_command(cmake: Path, build_root: Path, jobs: int | None) -> list[str]:
    command = [
        str(cmake),
        "--build",
        str(build_root),
        "--target",
        "ffx_fsr3upscaler_vk",
        "--parallel",
    ]
    if jobs is not None:
        command.append(str(jobs))
    return command


def copy_source_tree(work_root: Path, name: str) -> Path:
    source_root = work_root / name
    # The wrapper resolves its driver relative to the copied CMake project, so
    # both inputs must retain this otherwise-unused source-tree layout.
    shutil.copytree(REPO_ROOT / "cmake" / "fsr", source_root / "cmake" / "fsr")
    driver_destination = source_root / "scripts" / DRIVER.name
    driver_destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(DRIVER, driver_destination)
    return source_root


def find_first_patch_call(contents: str) -> tuple[int, int, str]:
    call_start = contents.find("ffx_sdk_replace_required(")
    if call_start == -1:
        raise CheckError("portable_sdk.cmake contains no ffx_sdk_replace_required call")

    depth = 1
    index = call_start + len("ffx_sdk_replace_required(")
    quote: str | None = None
    while index < len(contents):
        if quote is not None:
            if contents[index] == "\\":
                index += 2
                continue
            if contents[index] == quote:
                quote = None
            index += 1
            continue

        # Bracket arguments contain C++ snippets whose parentheses are not
        # CMake call nesting.
        bracket = BRACKET_OPEN_RE.match(contents, index)
        if bracket is not None:
            closing = f"]{bracket.group('equals')}]"
            end = contents.find(closing, bracket.end())
            if end == -1:
                raise CheckError("unterminated CMake bracket argument in portable_sdk.cmake")
            index = end + len(closing)
            continue
        if contents[index] == '"':
            quote = contents[index]
        elif contents[index] == "#":
            newline = contents.find("\n", index)
            index = len(contents) if newline == -1 else newline
            continue
        elif contents[index] == "(":
            depth += 1
        elif contents[index] == ")":
            depth -= 1
            if depth == 0:
                return call_start, index + 1, contents[call_start : index + 1]
        index += 1

    raise CheckError("unterminated ffx_sdk_replace_required call in portable_sdk.cmake")


def patch_call_brackets(call: str) -> list[re.Match[str]]:
    brackets = list(BRACKET_RE.finditer(call))
    if len(brackets) < 2:
        raise CheckError(
            "the first ffx_sdk_replace_required call does not have bracketed "
            "old_text and new_text arguments"
        )
    return brackets


def patch_target_relative_path(source_root: Path, call: str) -> str:
    variable_match = re.search(r"\$\{(FSR_STAGED_[A-Za-z0-9_]+)\}", call)
    if variable_match is None:
        raise CheckError("could not find an FSR_STAGED_* target variable in the first patch")
    variable = variable_match.group(1)
    cmake_contents = (source_root / "cmake" / "fsr" / "CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    assignment = re.search(
        rf"set\(\s*{re.escape(variable)}\s+\"(?P<value>[^\"]+)\"\s*\)",
        cmake_contents,
    )
    if assignment is None:
        raise CheckError(f"could not find the CMake set(...) for {variable}")
    value = assignment.group("value")
    _, separator, relative = value.partition("}/")
    if not separator or not relative.startswith("sdk/"):
        raise CheckError(f"could not derive an SDK-relative path from {variable}: {value}")
    return Path(relative).as_posix()


def edit_first_patch_new_text(patch_path: Path, marker: str) -> str:
    contents = patch_path.read_text(encoding="utf-8")
    call_start, call_end, call = find_first_patch_call(contents)
    brackets = patch_call_brackets(call)
    new_text = brackets[1]
    body = new_text.group("body")
    separator = "" if body.endswith("\n") else "\n"
    updated_body = f"{body}{separator}{marker}"
    updated_call = (
        f"{call[:new_text.start('body')]}{updated_body}"
        f"{call[new_text.end('body'):]}"
    )
    patch_path.write_text(
        f"{contents[:call_start]}{updated_call}{contents[call_end:]}",
        encoding="utf-8",
    )
    return patch_target_relative_path(patch_path.parents[3], updated_call)


def edit_first_patch_old_text(patch_path: Path, replacement: str) -> None:
    contents = patch_path.read_text(encoding="utf-8")
    call_start, call_end, call = find_first_patch_call(contents)
    old_text = patch_call_brackets(call)[0]
    updated_call = (
        f"{call[:old_text.start('body')]}{replacement}"
        f"{call[old_text.end('body'):]}"
    )
    patch_path.write_text(
        f"{contents[:call_start]}{updated_call}{contents[call_end:]}",
        encoding="utf-8",
    )


def check_initial(
    cmake: Path,
    source_root: Path,
    build_root: Path,
    args: argparse.Namespace,
    python: Path,
    glslang: Path,
) -> None:
    copy_source_tree(source_root.parent, source_root.name)
    run_command(
        "initial/configure",
        configure_command(cmake, source_root, build_root, args, python, glslang),
    )
    run_command("initial/build", build_command(cmake, build_root, args.jobs))
    print("[initial] passed: configure and build succeeded", flush=True)


def check_noop(
    cmake: Path,
    source_root: Path,
    build_root: Path,
    args: argparse.Namespace,
    python: Path,
    glslang: Path,
) -> None:
    run_command(
        "noop/configure",
        configure_command(cmake, source_root, build_root, args, python, glslang),
    )
    output = run_command("noop/build", build_command(cmake, build_root, args.jobs))
    if NO_WORK_OUTPUT not in output:
        raise CheckError(
            "unchanged configure and build did not report a Ninja no-op; "
            f"build output tail:\n{quoted_tail(output)}"
        )
    print("[noop] passed: unchanged reconfigure and rebuild did no work", flush=True)


def check_patch_edit(
    cmake: Path,
    source_root: Path,
    build_root: Path,
    args: argparse.Namespace,
    python: Path,
    glslang: Path,
) -> None:
    patch_path = source_root / "cmake" / "fsr" / "patches" / "portable_sdk.cmake"
    marker = f"// fsr patch restaging check {secrets.token_hex(8)}"
    relative_path = edit_first_patch_new_text(patch_path, marker)
    print(f"[patch-edit] inserted marker in {patch_path}", flush=True)
    run_command(
        "patch-edit/configure",
        configure_command(cmake, source_root, build_root, args, python, glslang),
    )
    output = run_command(
        "patch-edit/build", build_command(cmake, build_root, args.jobs)
    )
    if NO_WORK_OUTPUT in output:
        raise CheckError(
            "changing the staged patch did not trigger a rebuild; "
            f"build output tail:\n{quoted_tail(output)}"
        )

    published = build_root / "fsr_sdk_staged" / relative_path
    try:
        published_contents = published.read_text(encoding="utf-8")
    except OSError as error:
        raise CheckError(f"published patched file is missing or unreadable: {published}") from error
    if marker not in published_contents:
        raise CheckError(
            f"published patched file does not contain the marker: {published}"
        )
    print(
        f"[patch-edit] passed: marker reached {published} and rebuild did work",
        flush=True,
    )


def run_phase(
    label: str, action: Callable[[], None], failures: list[str]
) -> bool:
    try:
        action()
    except (CheckError, OSError) as error:
        message = str(error)
        failures.append(f"{label}: {message}")
        print(f"ERROR: [{label}] {message}", file=sys.stderr, flush=True)
        return False
    return True


def main() -> int:
    args = parse_args()
    if args.jobs is not None and args.jobs < 1:
        print("check_patch_restaging.py: --jobs must be positive", file=sys.stderr)
        return 2

    args.sdk_root = args.sdk_root.expanduser().resolve()
    args.vulkan_headers = args.vulkan_headers.expanduser().resolve()
    args.volk_headers = args.volk_headers.expanduser().resolve()

    missing: list[str] = []
    cmake = find_executable("cmake")
    if cmake is None:
        missing.append("cmake")
    ninja = find_executable("ninja")
    if ninja is None:
        missing.append("ninja")
    if find_executable("g++") is None:
        missing.append("g++")
    python = explicit_or_path_tool(args.python, ("python3",))
    if python is None:
        missing.append("python3")
    glslang = explicit_or_path_tool(args.glslang, ("glslangValidator", "glslang"))
    if glslang is None:
        missing.append("glslangValidator or glslang")
    if not args.sdk_root.is_dir():
        missing.append(f"SDK root ({args.sdk_root})")
    if not DRIVER.is_file():
        missing.append(f"shader driver ({DRIVER})")
    if missing:
        print(f"SKIP: missing prerequisite(s): {', '.join(missing)}")
        return 0

    temp_root = args.temp_root.expanduser().resolve()
    if not temp_root.is_dir():
        print(
            f"check_patch_restaging.py: temporary parent is not a directory: {temp_root}",
            file=sys.stderr,
        )
        return 2
    try:
        work_root = Path(tempfile.mkdtemp(prefix="fsr-restaging-", dir=temp_root))
    except OSError as error:
        print(f"check_patch_restaging.py: cannot create temporary work root: {error}", file=sys.stderr)
        return 2

    failures: list[str] = []
    print(f"FSR patch restaging checker work root: {work_root}", flush=True)
    print(f"SDK root: {args.sdk_root}", flush=True)
    print(f"glslang: {glslang}", flush=True)
    print(f"python: {python}", flush=True)

    source_root = work_root / "src"
    build_root = work_root / "build"
    initial_ok = run_phase(
        "initial",
        lambda: check_initial(
            cmake, source_root, build_root, args, python, glslang
        ),
        failures,
    )

    if initial_ok:
        run_phase(
            "noop",
            lambda: check_noop(
                cmake, source_root, build_root, args, python, glslang
            ),
            failures,
        )
        run_phase(
            "patch-edit",
            lambda: check_patch_edit(
                cmake, source_root, build_root, args, python, glslang
            ),
            failures,
        )
    else:
        print("[noop] skipped: initial phase failed", flush=True)
        print("[patch-edit] skipped: initial phase failed", flush=True)

    guard_source = work_root / "anchor-src"
    guard_build = work_root / "anchor-build"

    def anchor_guard_phase() -> None:
        nonlocal guard_source
        # A fresh build tree keeps the expected configure failure from
        # poisoning the good build used by the restaging check.
        guard_source = copy_source_tree(work_root, "anchor-src")
        patch_path = guard_source / "cmake" / "fsr" / "patches" / "portable_sdk.cmake"
        _, _, call = find_first_patch_call(patch_path.read_text(encoding="utf-8"))
        target_relative = patch_target_relative_path(guard_source, call)
        guard_token = f"fsr-restaging-anchor-guard-{secrets.token_hex(8)}"
        upstream = args.sdk_root / target_relative
        if guard_token in upstream.read_text(encoding="utf-8", errors="replace"):
            raise CheckError("random anchor-guard token unexpectedly occurs in the SDK source")
        edit_first_patch_old_text(patch_path, guard_token)

        command = configure_command(
            cmake, guard_source, guard_build, args, python, glslang
        )
        print("[anchor-guard] running configure expected to fail", flush=True)
        result = subprocess.run(
            command,
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
        )
        if result.returncode == 0:
            raise CheckError(
                "configure unexpectedly succeeded after corrupting old_text; "
                f"output tail:\n{quoted_tail(result.stdout)}"
            )
        expected = "FidelityFX SDK patch anchor was not found"
        if expected not in result.stdout:
            raise CheckError(
                f"configure failed without the expected diagnostic {expected!r}; "
                f"output tail:\n{quoted_tail(result.stdout)}"
            )
        print("[anchor-guard] passed: configure rejected the missing upstream anchor", flush=True)

    run_phase("anchor-guard", anchor_guard_phase, failures)

    if failures:
        print(
            "FSR patch restaging checker FAILED; temporary work root was retained at "
            f"{work_root}.",
            file=sys.stderr,
            flush=True,
        )
        return 1

    shutil.rmtree(work_root)
    print("FSR patch restaging checker passed all phases.", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
