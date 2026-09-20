"""Inventory production Slang entry points and optionally validate SPIR-V metadata.

This intentionally does not compile shaders.  Compilation is owned by
premake/slang.lua; this utility checks the source/filename contract and the
entry-point metadata in already-produced SPIR-V artifacts.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path


STAGES = {
    ".vert.slang": "vertex",
    ".frag.slang": "fragment",
    ".comp.slang": "compute",
    ".cs.slang": "compute",
    ".geom.slang": "geometry",
    ".tesc.slang": "tessellation-control",
    ".tese.slang": "tessellation-evaluation",
    ".rgen.slang": "raygeneration",
    ".rchit.slang": "closesthit",
    ".rmiss.slang": "miss",
    ".rahit.slang": "anyhit",
    ".rint.slang": "intersection",
    ".rcall.slang": "callable",
    ".rt.slang": "multi-raytracing",
    ".3d.slang": "multi-raster",
}

ENTRY_NAMES = (
    "vsMain",
    "psMain",
    "csMain",
    "binLights",
    "rayGen",
    "ptMiss",
    "ptAnyHit",
    "ptCloseHit",
    "waterReflRayGen",
    "waterReflMiss",
    "waterReflAnyHit",
    "waterReflCloseHit",
)

ENTRY_RE = re.compile(
    r"(?m)^\s*(?:public\s+)?(?:inline\s+)?(?:[A-Za-z_]\w*(?:\s*<[^>]+>)?\s+)+"
    r"(" + "|".join(ENTRY_NAMES) + r")\s*\("
)

SPIRV_STAGE = {
    "Vertex": "vertex",
    "TessellationControl": "tessellation-control",
    "TessellationEvaluation": "tessellation-evaluation",
    "Geometry": "geometry",
    "Fragment": "fragment",
    "GLCompute": "compute",
    "RayGenerationKHR": "raygeneration",
    "IntersectionKHR": "intersection",
    "AnyHitKHR": "anyhit",
    "ClosestHitKHR": "closesthit",
    "MissKHR": "miss",
    "CallableKHR": "callable",
}

ARTIFACT_MAGIC = "HPL2_SHADER_ARTIFACT"
ARTIFACT_VERSION = "1"


def source_stage(path: Path) -> str | None:
    return next((stage for suffix, stage in STAGES.items() if path.name.endswith(suffix)), None)


def inventory(root: Path) -> list[tuple[str, str, list[str]]]:
    result = []
    for path in sorted(root.rglob("*.slang")):
        stage = source_stage(path)
        if stage is None:
            continue
        text = path.read_text(encoding="utf-8")
        entries = list(dict.fromkeys(ENTRY_RE.findall(text)))
        if not entries:
            raise ValueError(f"{path}: entry-shader suffix has no recognized entry function")
        result.append((path.relative_to(root).as_posix(), stage, entries))
    return result


def spirv_entries(disassembler: Path, artifact: Path) -> list[tuple[str, str]]:
    proc = subprocess.run(
        [str(disassembler), str(artifact), "-o", "-"],
        check=False,
        capture_output=True,
        text=True,
    )
    if proc.returncode:
        raise ValueError(f"{artifact}: spirv-dis failed: {proc.stderr.strip()}")
    result = []
    for line in proc.stdout.splitlines():
        match = re.search(r"OpEntryPoint\s+(\w+)\s+%\S+\s+\"([^\"]+)\"", line)
        if match:
            stage = SPIRV_STAGE.get(match.group(1), match.group(1))
            result.append((match.group(2), stage))
    if not result:
        raise ValueError(f"{artifact}: no OpEntryPoint metadata")
    return result


def validate_metadata(artifact: Path) -> None:
    """Validate the sidecar contract used by RIProgram's file loader."""
    metadata = artifact.with_name(artifact.name + ".meta")
    if not metadata.is_file():
        # Raw SPIR-V is a deployed legacy format and RIProgram intentionally
        # accepts it without a sidecar. Generated artifacts may still carry
        # the richer envelope, which is checked when present.
        return
    lines = metadata.read_text(encoding="utf-8").splitlines()
    expected = [ARTIFACT_MAGIC, f"version={ARTIFACT_VERSION}", "format=spirv"]
    if lines[:3] != expected:
        raise ValueError(f"{artifact}: stale or mismatched metadata in {metadata}")
    if artifact.stat().st_size < 4:
        raise ValueError(f"{artifact}: truncated SPIR-V artifact")
    with artifact.open("rb") as stream:
        magic = stream.read(4)
    if magic not in (b"\x03\x02\x23\x07", b"\x07\x23\x02\x03"):
        raise ValueError(f"{artifact}: metadata says SPIR-V but bytes have the wrong magic")


def validate_artifacts(root: Path, artifact_dir: Path, disassembler: Path) -> None:
    for relative, source_stage_name, source_entries in inventory(root):
        artifact = artifact_dir / (Path(relative).name.removesuffix(".slang") + ".spv")
        if not artifact.is_file():
            raise ValueError(f"{relative}: missing artifact {artifact}")
        validate_metadata(artifact)
        actual = spirv_entries(disassembler, artifact)
        expected = set(source_entries)
        actual_names = {name for name, _ in actual}
        if expected != actual_names:
            raise ValueError(
                f"{relative}: source entries {sorted(expected)} != SPIR-V entries {sorted(actual_names)}"
            )
        if source_stage_name == "multi-raster":
            expected_stages = {"vertex", "fragment"}
        elif source_stage_name == "multi-raytracing":
            expected_stages = {stage for _, stage in actual}
        else:
            expected_stages = {source_stage_name}
        actual_stages = {stage for _, stage in actual}
        if actual_stages != expected_stages:
            raise ValueError(
                f"{relative}: suffix stage {sorted(expected_stages)} != SPIR-V stages {sorted(actual_stages)}"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("amnesia/slang"))
    parser.add_argument("--compiled-dir", type=Path)
    parser.add_argument("--spirv-dis", type=Path, default=Path(shutil.which("spirv-dis") or "spirv-dis"))
    args = parser.parse_args()

    try:
        entries = inventory(args.root)
        for relative, stage, names in entries:
            print(f"{relative}\t{stage}\t{','.join(names)}")
        if args.compiled_dir:
            if not args.spirv_dis.is_file() and shutil.which(str(args.spirv_dis)) is None:
                raise ValueError(f"spirv-dis not found: {args.spirv_dis}")
            validate_artifacts(args.root, args.compiled_dir, args.spirv_dis)
            print(f"validated {len(entries)} SPIR-V artifacts in {args.compiled_dir}")
    except (OSError, ValueError) as exc:
        print(f"shader inventory validation failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
