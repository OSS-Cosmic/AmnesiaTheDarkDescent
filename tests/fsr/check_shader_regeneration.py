#!/usr/bin/env python3
"""Check FSR shader output determinism and incremental regeneration."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[2]
DRIVER = REPO_ROOT / "scripts" / "compile_fsr_shaders.py"
DEFAULT_BUILD_DIR = REPO_ROOT / "build-premake" / "external" / "FsrSdkExternal" / "Debug"
DEFAULT_SDK_ROOT = (
    REPO_ROOT / "build-premake" / "_deps" / "fsr-sdk" / "FidelityFX-SDK-1.1.4"
)
DEFAULT_FFX_SC = DEFAULT_BUILD_DIR / "ffx_sc" / (
    "FidelityFX_SC.exe" if os.name == "nt" else "FidelityFX_SC"
)
INCLUDE_RE = re.compile(rb'^#include\s+["<]([^">]+)[">]', re.MULTILINE)
INDIRECTION_TABLE_RE = re.compile(
    rb"static const uint32_t [^\n]+_IndirectionTable\[\] = \{(.*?)\};",
    re.DOTALL,
)
INTEGER_RE = re.compile(rb"\b\d+\b")


class CheckError(RuntimeError):
    """A user-facing checker failure."""


def default_glslang() -> Path:
    for name in ("glslangValidator", "glslang"):
        found = shutil.which(name)
        if found:
            return Path(found)
    # Keep the default diagnostic tied to the same build tree as FidelityFX_SC.
    for name in ("glslangValidator", "glslang", "glslang.exe"):
        candidate = DEFAULT_BUILD_DIR / name
        if candidate.is_file():
            return candidate
    return DEFAULT_BUILD_DIR / (
        "glslangValidator.exe" if os.name == "nt" else "glslangValidator"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-root", type=Path, default=DEFAULT_SDK_ROOT)
    parser.add_argument("--ffx-sc", type=Path, default=DEFAULT_FFX_SC)
    parser.add_argument("--glslang", type=Path, default=default_glslang())
    parser.add_argument(
        "--spirv-val",
        type=Path,
        help="optional spirv-val executable; validation is otherwise skipped",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        help="optional thread count forwarded to FidelityFX_SC; unset tests its default",
    )
    parser.add_argument(
        "--temp-root",
        type=Path,
        default=Path(tempfile.gettempdir()),
        help="parent for temporary outputs (default: platform temp directory)",
    )
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def files_snapshot(directory: Path) -> dict[str, tuple[str, int, int]]:
    """Return digest, mtime, and size for every regular output file."""
    if not directory.is_dir():
        raise CheckError(f"output directory was not created: {directory}")
    return {
        path.relative_to(directory).as_posix(): (
            sha256(path),
            path.stat().st_mtime_ns,
            path.stat().st_size,
        )
        for path in sorted(directory.rglob("*"))
        if path.is_file()
    }


def header_paths(directory: Path) -> dict[str, Path]:
    return {
        path.name: path
        for path in sorted(directory.glob("*.h"))
        if path.is_file()
    }


def driver_command(
    args: argparse.Namespace, output_dir: Path
) -> list[str]:
    command = [
        sys.executable,
        str(DRIVER),
        "--sdk-root",
        str(args.sdk_root),
        "--ffx-sc",
        str(args.ffx_sc),
        "--glslang",
        str(args.glslang),
        "--out-dir",
        str(output_dir),
        "--stamp",
        str(output_dir / "fsr3upscaler.stamp"),
        "--depfile",
        str(output_dir / "fsr3upscaler.d"),
    ]
    if args.spirv_val is not None:
        command.extend(("--spirv-val", str(args.spirv_val)))
    if args.jobs is not None:
        command.extend(("--jobs", str(args.jobs)))
    return command


def run_driver(args: argparse.Namespace, output_dir: Path, label: str) -> None:
    command = driver_command(args, output_dir)
    print(f"[{label}] running shader driver into {output_dir}", flush=True)
    result = subprocess.run(
        command,
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    if result.returncode != 0:
        diagnostic = result.stdout.strip() or "(shader driver produced no diagnostic output)"
        raise CheckError(
            f"{label}: scripts/compile_fsr_shaders.py failed with exit "
            f"{result.returncode}.\n{diagnostic}"
        )
    print(f"[{label}] driver exited 0", flush=True)


def first_difference(left: bytes, right: bytes) -> tuple[int, str, str]:
    offset = next(
        (index for index, (a, b) in enumerate(zip(left, right)) if a != b),
        min(len(left), len(right)),
    )
    if offset == len(left) == len(right):
        return offset, "", ""
    left_line = left[:offset].count(b"\n") + 1
    right_line = right[:offset].count(b"\n") + 1
    left_lines = left.splitlines() or [b""]
    right_lines = right.splitlines() or [b""]
    left_text = (
        left_lines[left_line - 1].decode("utf-8", "replace")
        if left_line <= len(left_lines)
        else "<end of file>"
    )
    right_text = (
        right_lines[right_line - 1].decode("utf-8", "replace")
        if right_line <= len(right_lines)
        else "<end of file>"
    )
    return offset, left_text, right_text


def permutation_evidence(left: Path, right: Path) -> tuple[bool, str]:
    """Identify accessor-only differences caused by unique-blob ordering."""
    left_headers = header_paths(left)
    right_headers = header_paths(right)
    common = sorted(set(left_headers) & set(right_headers))
    blob_names = [name for name in common if not name.endswith("_permutations.h")]
    if any(left_headers[name].read_bytes() != right_headers[name].read_bytes() for name in blob_names):
        return False, ""

    changed_permutations = [
        name
        for name in common
        if name.endswith("_permutations.h")
        and left_headers[name].read_bytes() != right_headers[name].read_bytes()
    ]
    if not changed_permutations:
        return False, ""

    samples: list[str] = []
    for name in changed_permutations[:3]:
        left_data = left_headers[name].read_bytes()
        right_data = right_headers[name].read_bytes()
        left_includes = [item.decode("utf-8", "replace") for item in INCLUDE_RE.findall(left_data)]
        right_includes = [item.decode("utf-8", "replace") for item in INCLUDE_RE.findall(right_data)]
        left_table_match = INDIRECTION_TABLE_RE.search(left_data)
        right_table_match = INDIRECTION_TABLE_RE.search(right_data)
        left_table = (
            [int(item) for item in INTEGER_RE.findall(left_table_match.group(1))]
            if left_table_match
            else []
        )
        right_table = (
            [int(item) for item in INTEGER_RE.findall(right_table_match.group(1))]
            if right_table_match
            else []
        )
        same_blob_set = set(left_includes) == set(right_includes)
        if same_blob_set and (left_includes != right_includes or left_table != right_table):
            samples.append(
                f"{name}: same {len(set(left_includes))} unique-blob includes, "
                f"include order differs={left_includes != right_includes}, "
                f"indirection table differs={left_table != right_table}"
            )

    if not samples:
        return False, ""
    return True, (
        "the non-permutation blob headers are byte-identical, but permutation "
        "accessors retain the same unique-blob set with different include/table "
        "ordering; this is a unique-blob index assignment/order-dependence finding.\n"
        + "\n".join(f"  {sample}" for sample in samples)
    )


def check_determinism(left: Path, right: Path) -> None:
    left_headers = header_paths(left)
    right_headers = header_paths(right)
    left_names = set(left_headers)
    right_names = set(right_headers)
    if not left_names or not right_names:
        raise CheckError(
            "determinism check FAILED: one run emitted no headers.\n"
            f"  run A: {left} ({len(left_names)} headers)\n"
            f"  run B: {right} ({len(right_names)} headers)"
        )
    if left_names != right_names:
        missing_left = sorted(right_names - left_names)
        missing_right = sorted(left_names - right_names)
        raise CheckError(
            "determinism check FAILED: emitted header filename sets differ.\n"
            f"  run A: {left}\n  run B: {right}\n"
            f"  only in run B: {missing_left[:8]}\n"
            f"  only in run A: {missing_right[:8]}"
        )

    different = [
        name
        for name in sorted(left_names)
        if left_headers[name].read_bytes() != right_headers[name].read_bytes()
    ]
    if not different:
        print(f"[determinism] passed: {len(left_names)} headers are byte-identical", flush=True)
        return

    finding = ""
    is_index_finding, evidence = permutation_evidence(left, right)
    if is_index_finding:
        finding = f"\n  determinism finding: {evidence}"
    sample = different[0]
    left_data = left_headers[sample].read_bytes()
    right_data = right_headers[sample].read_bytes()
    offset, left_line, right_line = first_difference(left_data, right_data)
    raise CheckError(
        "determinism check FAILED: emitted headers are not byte-identical.\n"
        f"  run A: {left_headers[sample]}\n"
        f"  run B: {right_headers[sample]}\n"
        f"  differing headers: {len(different)} of {len(left_names)}\n"
        f"  first differing byte offset: {offset}\n"
        f"  run A first differing line: {left_line}\n"
        f"  run B first differing line: {right_line}\n"
        f"  run A SHA-256: {sha256(left_headers[sample])}\n"
        f"  run B SHA-256: {sha256(right_headers[sample])}"
        f"{finding}"
    )


def depfile_inputs(depfile: Path) -> set[Path]:
    try:
        contents = depfile.read_text(encoding="utf-8")
    except OSError as error:
        raise CheckError(f"could not read generated depfile {depfile}: {error}") from error
    tokens = contents.replace("\\\n", " ").split()
    if len(tokens) < 2:
        raise CheckError(f"generated depfile has no inputs: {depfile}")
    return {
        Path(token).resolve()
        for token in tokens[1:]
        if token != "\\" and token.isprintable()
    }


def choose_fsr3_header(sdk_root: Path, depfile: Path) -> Path:
    include_root = (
        sdk_root
        / "sdk"
        / "include"
        / "FidelityFX"
        / "gpu"
        / "fsr3upscaler"
    )
    inputs = depfile_inputs(depfile)
    candidates = sorted(
        path
        for path in include_root.glob("*.h")
        if path.is_file() and path.resolve() in inputs
    )
    if not candidates:
        raise CheckError(
            "generated depfile does not identify an FSR3 Upscaler include header "
            f"under {include_root}"
        )
    header = candidates[0]
    print(
        f"[incremental] selected FSR3 include {header} from generated depfile",
        flush=True,
    )
    return header


def fsr3_include_closure(sdk_root: Path) -> set[Path]:
    shader_dir = (
        sdk_root
        / "sdk"
        / "src"
        / "backends"
        / "vk"
        / "shaders"
        / "fsr3upscaler"
    )
    gpu_include_dir = sdk_root / "sdk" / "include" / "FidelityFX" / "gpu"
    fsr3_include_dir = gpu_include_dir / "fsr3upscaler"
    shaders = sorted(
        path.resolve() for path in shader_dir.glob("*.glsl") if path.is_file()
    )
    if not shaders:
        raise CheckError(
            f"FSR3 Upscaler shader entry points are missing under {shader_dir}"
        )

    include_roots = (gpu_include_dir, fsr3_include_dir)
    pending = list(shaders)
    visited: set[Path] = set()
    headers: set[Path] = set()
    while pending:
        source = pending.pop()
        if source in visited:
            continue
        visited.add(source)
        try:
            contents = source.read_bytes()
        except OSError as error:
            raise CheckError(
                f"could not read FSR3 include-closure source {source}: {error}"
            ) from error

        for raw_name in INCLUDE_RE.findall(contents):
            include_name = raw_name.decode("utf-8", "replace")
            candidates = [source.parent / include_name]
            candidates.extend(root / include_name for root in include_roots)
            included = next(
                (candidate.resolve() for candidate in candidates if candidate.is_file()),
                None,
            )
            if included is None:
                raise CheckError(
                    "could not resolve FSR3 Upscaler include "
                    f'"{include_name}" from {source}'
                )
            if included.suffix == ".h":
                headers.add(included)
            pending.append(included)

    return headers


def perturb_header(header: Path) -> tuple[bytes, os.stat_result]:
    original_stat = header.stat()
    original_contents = header.read_bytes()
    header.write_bytes(
        original_contents
        + b"\n// FSR shader regeneration checker input perturbation\n"
    )
    return original_contents, original_stat


def restore_header(
    header: Path, original_contents: bytes, original_stat: os.stat_result
) -> None:
    header.write_bytes(original_contents)
    os.utime(
        header,
        ns=(original_stat.st_atime_ns, original_stat.st_mtime_ns),
    )


def check_incremental(args: argparse.Namespace, output_dir: Path) -> None:
    run_driver(args, output_dir, "incremental/initial")
    before = files_snapshot(output_dir)
    stamp = output_dir / "fsr3upscaler.stamp"
    if not stamp.is_file():
        raise CheckError(f"incremental check: driver did not write its stamp: {stamp}")
    stamp_before = stamp.stat().st_mtime_ns
    header = choose_fsr3_header(args.sdk_root, output_dir / "fsr3upscaler.d")
    original_contents, original_stat = perturb_header(header)
    print(f"[incremental] changing SDK GPU include contents {header}", flush=True)
    driver_error: CheckError | None = None
    try:
        try:
            run_driver(args, output_dir, "incremental/touched-rerun")
        except CheckError as error:
            driver_error = error
    finally:
        restore_header(header, original_contents, original_stat)
    if driver_error is not None:
        raise driver_error

    after = files_snapshot(output_dir)
    stamp_after = stamp.stat().st_mtime_ns
    changed_headers = [
        name
        for name, details in after.items()
        if name.endswith(".h") and details != before.get(name)
    ]
    if stamp_after <= stamp_before and not changed_headers:
        raise CheckError(
            "incremental regeneration check FAILED: touching an SDK GPU "
            "include resulted in a no-op.\n"
            f"  touched: {header}\n"
            f"  output directory: {output_dir}\n"
            f"  stamp mtime before/after: {stamp_before}/{stamp_after}\n"
            f"  regenerated headers: {changed_headers}\n"
            "  The driver must observe the dependency change and update its "
            "stamp or regenerate affected outputs."
        )
    print(
        f"[incremental] passed: stamp_updated={stamp_after > stamp_before}, "
        f"changed_headers={len(changed_headers)}",
        flush=True,
    )


def check_dependency_soundness(args: argparse.Namespace, output_dir: Path) -> None:
    run_driver(args, output_dir, "dependency/initial")
    depfile = output_dir / "fsr3upscaler.d"
    depfile_entries = depfile_inputs(depfile)
    closure = fsr3_include_closure(args.sdk_root)
    missing = sorted(closure - depfile_entries)
    if missing:
        raise CheckError(
            "dependency soundness check FAILED: generated depfile omits "
            f"{len(missing)} header(s) from the FSR3 Upscaler include closure.\n"
            + "\n".join(f"  missing: {path}" for path in missing[:12])
        )

    outside_closure = depfile_entries - closure
    blur_header = (
        args.sdk_root
        / "sdk"
        / "include"
        / "FidelityFX"
        / "gpu"
        / "blur"
        / "ffx_blur.h"
    ).resolve()
    print(
        f"[dependency] soundness passed: depfile contains all {len(closure)} "
        "headers in the FSR3 Upscaler include closure",
        flush=True,
    )
    print(
        f"[dependency] informational: {len(outside_closure)} depfile entries "
        "lie outside that closure; "
        f"{blur_header.name} is a worked example of the deliberate conservative "
        "over-approximation",
        flush=True,
    )


def check_noop(args: argparse.Namespace, output_dir: Path) -> None:
    run_driver(args, output_dir, "no-op/initial")
    before = files_snapshot(output_dir)
    run_driver(args, output_dir, "no-op/unchanged-rerun")
    after = files_snapshot(output_dir)
    if before != after:
        changed = sorted(
            name
            for name in set(before) | set(after)
            if before.get(name) != after.get(name)
        )
        raise CheckError(
            "no-op check FAILED: the unchanged rerun modified output files.\n"
            f"  output directory: {output_dir}\n  changed files: {changed[:12]}"
        )
    print(f"[no-op] passed: unchanged rerun modified no files in {output_dir}", flush=True)


def main() -> int:
    args = parse_args()
    if args.jobs is not None and args.jobs < 1:
        print("check_shader_regeneration.py: --jobs must be positive", file=sys.stderr)
        return 2
    if not DRIVER.is_file():
        print(f"check_shader_regeneration.py: missing driver: {DRIVER}", file=sys.stderr)
        return 2

    try:
        temp_root = args.temp_root.expanduser().resolve()
        temp_root.mkdir(parents=True, exist_ok=True)
        work_root = Path(tempfile.mkdtemp(prefix="fsr-regeneration-", dir=temp_root))
    except OSError as error:
        print(f"check_shader_regeneration.py: cannot create temporary output: {error}", file=sys.stderr)
        return 2

    deterministic_a = work_root / "run-a"
    deterministic_b = work_root / "run-b"
    incremental = work_root / "incremental"
    dependency = work_root / "dependency"
    noop = work_root / "noop"
    failures: list[str] = []
    print(f"FSR shader regeneration checker work root: {work_root}")
    print(f"SDK root: {args.sdk_root}")
    print(f"FidelityFX_SC: {args.ffx_sc}")
    print(f"glslang: {args.glslang}")

    try:
        run_driver(args, deterministic_a, "determinism/run-a")
        run_driver(args, deterministic_b, "determinism/run-b")
        check_determinism(deterministic_a, deterministic_b)
    except CheckError as error:
        failures.append(str(error))
        print(f"ERROR: {error}", file=sys.stderr, flush=True)

    for check in (
        lambda: check_incremental(args, incremental),
        lambda: check_dependency_soundness(args, dependency),
        lambda: check_noop(args, noop),
    ):
        try:
            check()
        except CheckError as error:
            failures.append(str(error))
            print(f"ERROR: {error}", file=sys.stderr, flush=True)

    if failures:
        print(
            "FSR shader regeneration checker FAILED; temporary output directories "
            f"were retained at {work_root}.",
            file=sys.stderr,
        )
        return 1

    shutil.rmtree(work_root)
    print("FSR shader regeneration checker passed all determinism and incremental checks.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
