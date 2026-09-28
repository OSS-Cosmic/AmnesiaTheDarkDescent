#!/usr/bin/env python3
"""Check the AMFP player-lighting parity contract without building the game."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HELPER_H = ROOT / "amfp" / "src" / "game" / "LuxPlayerHelpers.h"
HELPER_CPP = ROOT / "amfp" / "src" / "game" / "LuxPlayerHelpers.cpp"
PROBE_H = ROOT / "amfp" / "src" / "game" / "LuxLightProbeBrightness.h"


def strip_comments(source: str) -> str:
    """Remove C++ comments while preserving strings, chars, and line breaks."""
    result: list[str] = []
    index = 0
    quote: str | None = None
    escaped = False
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if quote:
            result.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
            index += 1
        elif char in ('"', "'"):
            quote = char
            result.append(char)
            index += 1
        elif char == "/" and next_char == "/":
            result.append(" ")
            index += 2
            while index < len(source) and source[index] != "\n":
                index += 1
            if index < len(source):
                result.append("\n")
        elif char == "/" and next_char == "*":
            index += 2
            while index < len(source) - 1 and not (
                source[index] == "*" and source[index + 1] == "/"
            ):
                result.append("\n" if source[index] == "\n" else " ")
                index += 1
            if index < len(source) - 1:
                result.extend("  ")
                index += 2
        else:
            result.append(char)
            index += 1
    return "".join(result)


def function_body(source: str, qualified_name: str) -> str | None:
    """Return a function body using brace depth, tolerant of whitespace."""
    source = strip_comments(source)
    match = re.search(
        rf"{re.escape(qualified_name)}\s*\([^;{{}}]*\)\s*{{", source
    )
    if match is None:
        return None

    opening = source.find("{", match.start(), match.end())
    depth = 0
    quote: str | None = None
    escaped = False
    for index in range(opening, len(source)):
        char = source[index]
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
        elif char in ('"', "'"):
            quote = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    return None


def has(pattern: str, source: str) -> bool:
    return re.search(pattern, source, flags=re.DOTALL) is not None


def main() -> int:
    issues: list[str] = []
    for path, label in (
        (HELPER_H, "LuxPlayerHelpers.h"),
        (HELPER_CPP, "LuxPlayerHelpers.cpp"),
        (PROBE_H, "LuxLightProbeBrightness.h"),
    ):
        if not path.is_file():
            issues.append(f"missing {label}: {path.relative_to(ROOT)}")

    if issues:
        print("AMFP player-lighting contract: FAIL")
        for issue in issues:
            print(f"- {issue}")
        return 1

    header = strip_comments(HELPER_H.read_text(encoding="utf-8"))
    cpp = strip_comments(HELPER_CPP.read_text(encoding="utf-8"))
    probe = strip_comments(PROBE_H.read_text(encoding="utf-8"))
    update = function_body(cpp, "cLuxPlayerLightLevel::Update")
    probe_update = function_body(cpp, "cLuxPlayerLightLevel::UpdateFromProbe")
    map_enter = function_body(cpp, "cLuxPlayerLightLevel::OnMapEnter")
    lantern_entities = function_body(cpp, "cLuxPlayerLantern::CreateWorldEntities")

    if update is None:
        issues.append("missing cLuxPlayerLightLevel::Update implementation")
    if probe_update is None:
        issues.append("missing cLuxPlayerLightLevel::UpdateFromProbe implementation")
    if map_enter is None:
        issues.append("missing cLuxPlayerLightLevel::OnMapEnter implementation")
    if lantern_entities is None:
        issues.append("missing cLuxPlayerLantern::CreateWorldEntities implementation")

    header_contract = {
        "probe brightness state": r"\bcLuxLightProbeBrightness\s+mProbeBrightness\s*;",
        "probe gain state": r"\bfloat\s+mfLightProbeGain\s*(?:=\s*[^;]+)?;",
        "probe availability state": r"\bbool\s+mbUsingProbe\s*(?:=\s*false\s*)?;",
        "normalized level accessor": r"\bGetLevel\s*\([^;{}]*\)",
        "probe irradiance accessor": r"\bGetProbeIrradiance\s*\([^;{}]*\)",
        "probe availability accessor": r"\bIsUsingProbe\s*\([^;{}]*\)",
        "UpdateFromProbe declaration": r"\bUpdateFromProbe\s*\([^;{}]*\)",
    }
    for label, pattern in header_contract.items():
        if not has(pattern, header):
            issues.append(f"header missing {label}")

    brightness_update = function_body(probe, "Update")
    probe_update_legacy = function_body(probe, "UpdateLegacy")
    probe_component = function_body(probe, "Component")

    probe_contract = {
        "probe brightness class": r"\bclass\s+cLuxLightProbeBrightness\b",
        "probe update": r"\bvoid\s+Update\s*\([^;{}]*\)\s*{",
        "legacy publishing path": r"\bvoid\s+UpdateLegacy\s*\([^;{}]*\)\s*{",
        "lantern state setter": r"\bSetLantern\s*\(\s*bool\b",
        "lantern-separated level": r"GetLevel\s*\([^{}]*\)\s*(?:const\s*)?{[^{}]*mfEnvironment[^{}]*mbLantern",
    }
    for label, pattern in probe_contract.items():
        if not has(pattern, probe):
            issues.append(f"LuxLightProbeBrightness.h missing {label}")

    if brightness_update is not None and not has(
        r"std::isfinite\s*\(\s*gain\s*\)[\s\S]*gain\s*<=\s*0\.0f",
        brightness_update,
    ):
        issues.append("LuxLightProbeBrightness::Update does not sanitize non-finite or non-positive gain")
    if probe_update_legacy is not None and not has(
        r"std::isfinite\s*\(\s*level\s*\)[\s\S]*level\s*<\s*0\.0f",
        probe_update_legacy,
    ):
        issues.append("LuxLightProbeBrightness::UpdateLegacy does not reject non-finite or negative levels")
    if probe_component is not None and not has(
        r"std::isfinite\s*\(\s*value\s*\)[\s\S]*value\s*>\s*0\.0f",
        probe_component,
    ):
        issues.append("LuxLightProbeBrightness::Component does not reject non-finite or non-positive samples")

    if update is not None:
        update_contract = {
            "Standard backend branch": r"GetRendererBackend\s*\(\s*\)\s*==\s*(?:hpl::)?eRendererBackend_Standard",
            "five sample positions": r"\bconst\s+int\s+\w+\s*=\s*5\s*;[\s\S]*?\b\w+\s*\[\s*\w+\s*\]\s*=\s*{",
            "lantern exclusion": r"\bvSkipLights\s*\.\s*push_back\s*\(\s*mpPlayer\s*->\s*GetHelperLantern\s*\(\s*\)\s*->\s*GetLight\s*\(\s*\)\s*\)",
            "lantern/environment separation": r"mProbeBrightness\s*\.\s*SetLantern\s*\(\s*mpPlayer\s*->\s*GetHelperLantern\s*\(\s*\)\s*->\s*IsActive\s*\(\s*\)\s*\)",
        }
        for label, pattern in update_contract.items():
            if not has(pattern, update):
                issues.append(f"Update missing {label}")

        ambient_match = re.search(
            r"\biLight\s*\*\s*(?P<ambient>\w+)\s*=\s*\w+\s*->\s*GetLight\s*\(\s*"
            r'"PlayerDarknessAmbient"\s*\)',
            update,
        )
        if not has(r"\bcWorld\s*\*\s*\w+\s*=", update):
            issues.append("Update does not obtain the current cWorld")
        if ambient_match is None:
            issues.append("Update missing cWorld::GetLight(\"PlayerDarknessAmbient\") lookup")
        else:
            ambient = ambient_match.group("ambient")
            if not has(
                rf"\bvSkipLights\s*\.\s*push_back\s*\(\s*{re.escape(ambient)}\s*\)",
                update,
            ):
                issues.append("Update does not add the darkness ambient pointer to vSkipLights")

        if not has(
            r"UpdateFromProbe\s*\([^;{}]*\bvSkipLights\b[^;{}]*\)",
            update,
        ):
            issues.append("Update does not pass vSkipLights into UpdateFromProbe")

    if probe_update is not None:
        incomplete_result = (
            r"if\s*\(\s*!\s*pProbe\s*->\s*GetResult\s*\([^;{}]*\)\s*\)"
            r"\s*return\s+false\s*;"
        )
        if not has(incomplete_result, probe_update):
            issues.append("UpdateFromProbe does not retain the last complete result on an incomplete probe")
        if not has(r"GetResult\s*\([^;{}]*\)[\s\S]*mProbeBrightness\s*\.\s*Update\s*\(", probe_update):
            issues.append("UpdateFromProbe does not publish completed probe samples")
        exclude_match = re.search(
            r"std::vector\s*<\s*iLight\s*\*\s*>\s*(?P<exclude>\w+)\s*=\s*avSkipLights\s*;",
            probe_update,
        )
        if exclude_match is None:
            issues.append("UpdateFromProbe does not copy vSkipLights into the probe exclusion vector")
        else:
            exclude = re.escape(exclude_match.group("exclude"))
            if not has(
                rf"\b{exclude}\s*\.\s*push_back\s*\(\s*mpPlayer\s*->\s*GetHelperLantern\s*\(\s*\)\s*->\s*GetLight\s*\(\s*\)\s*\)",
                probe_update,
            ):
                issues.append("UpdateFromProbe does not exclude the lantern light from probe samples")
            if not has(
                rf"SetExcludedLights\s*\([^;]*&\s*{exclude}\s*\[\s*0\s*\][^;]*\b{exclude}\s*\.\s*size\s*\(\s*\)",
                probe_update,
            ):
                issues.append("UpdateFromProbe does not submit the exclusion vector to the probe")

    if map_enter is not None and not has(
        r"Reset\s*\(\s*\)[\s\S]*lightProbe[\s\S]*->\s*Reset\s*\(\s*\)", map_enter
    ):
        issues.append("OnMapEnter does not reset the light probe")

    if lantern_entities is not None:
        for model in ("Legacy", "RayTraced"):
            if not has(
                rf"SetTuning\s*\(\s*(?:hpl::)?eLightModel_{model}\s*,", lantern_entities
            ):
                issues.append(f"lantern world light missing {model} tuning")

    if issues:
        print("AMFP player-lighting contract: FAIL")
        for issue in issues:
            print(f"- {issue}")
        return 1

    print("AMFP player-lighting contract: PASS (probe state, sampling, exclusions, reset, and dual tuning)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
