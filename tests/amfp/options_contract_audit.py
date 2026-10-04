#!/usr/bin/env python3
"""Audit the AMFP options merge contract without building the game."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / "amfp" / "src" / "game" / "LuxMainMenu_Options.cpp"
HEADER = ROOT / "amfp" / "src" / "game" / "LuxMainMenu_Options.h"
CONFIG_CPP = ROOT / "amfp" / "src" / "game" / "LuxConfigHandler.cpp"
MESSAGE_HEADER = ROOT / "amfp" / "src" / "game" / "LuxMessageHandler.h"
MESSAGE_CPP = ROOT / "amfp" / "src" / "game" / "LuxMessageHandler.cpp"
EFFECT_CPP = ROOT / "amfp" / "src" / "game" / "LuxEffectHandler.cpp"
PLAYER_HELPERS_HEADER = ROOT / "amfp" / "src" / "game" / "LuxPlayerHelpers.h"
PLAYER_HELPERS_CPP = ROOT / "amfp" / "src" / "game" / "LuxPlayerHelpers.cpp"
MAP_HEADER = ROOT / "amfp" / "src" / "game" / "LuxMapHandler.h"
MAP_CPP = ROOT / "amfp" / "src" / "game" / "LuxMapHandler.cpp"
COMMENTARY_CPP = ROOT / "amfp" / "src" / "game" / "LuxCommentaryIcon.cpp"

NEW_KEYS = (
    "RendererBackend",
    "SuperSamplingProvider",
    "SuperSamplingQuality",
    "RenderScale",
    "HRTFActive",
    "ShowEffectSubtitles",
    "ShowDeathHints",
    "ShowCommentary",
)
LEGACY_KEYS = (
    "AdaptiveVsync",
    "SSAOActive",
    "SSAOSamples",
    "SSAOResolution",
    "ColorGradingActive",
    "BloomActive",
    "ImageTrailActive",
    "SepiaActive",
    "RadialBlurActive",
)
RESOURCE_KEY_ALIASES = {
    # The options resource object uses the AMFP name.  SSAOSamples is the
    # persisted Graphics config key used by cLuxConfigHandler.
    "SSAOSamples": "SSAONumOfSamples",
}
FORBIDDEN_OPTIONS_REFERENCES = re.compile(
    r"Insanity", re.IGNORECASE
)
LIFECYCLE = (
    ("load/populate", "SetInputValues"),
    ("initial dump", "DumpInitialValues"),
    ("current dump", "DumpCurrentValues"),
    ("apply", "ApplyChanges"),
    ("cancel/restore", "PressCancel"),
)
APPLY_PATTERNS = {
    "RendererBackend": r"mRendererBackend|GetSelectedRendererBackend",
    "SuperSamplingProvider": r"mSuperSampling(?:\.provider|RequestedProvider)",
    "SuperSamplingQuality": r"mSuperSampling(?:\.quality|RequestedQuality)",
    "RenderScale": r"SetRenderScale|mfRenderScaleRequested",
    "HRTFActive": r"mbHRTFActive|mpChBHRTF",
    "ShowEffectSubtitles": r"SetShowEffectSubtitles|ShowEffectSubtitles",
    "ShowDeathHints": r"SetShowHint|ShowHint",
    "ShowCommentary": r"SetShowCommentary|GetShowCommentary",
}
LEGACY_APPLY_PATTERNS = {
    "AdaptiveVsync": r"mbAdaptiveVSync|mpChBAdaptiveVSync",
    "SSAOActive": r"mbSSAOActive|mpChBSSAO",
    "SSAOSamples": r"mlSSAOSamples|SSAONumOfSamples|mpCBSSAOSamples",
    "SSAOResolution": r"mlSSAOResolution|mpCBSSAOResolution",
    "ColorGradingActive": r"mpChBColorGrading|GetPostEffect_ColorGrading",
    "BloomActive": r"mpChBBloom|GetPostEffect_Bloom",
    "ImageTrailActive": r"mpChBImageTrail|GetPostEffect_ImageTrail",
    "SepiaActive": r"mpChBSepia|GetPostEffect_Sepia",
    "RadialBlurActive": r"mpChBRadialBlur|GetPostEffect_RadialBlur",
}
HEADER_PATTERNS = {
    "RendererBackend": r"mpCBRendererBackend",
    "SuperSamplingProvider": r"mSuperSamplingRequestedProvider|mpCBTemporalUpscaler",
    "SuperSamplingQuality": r"mSuperSamplingRequestedQuality|mpCBTemporalUpscalerQuality",
    "RenderScale": r"mfRenderScaleRequested|mpCBRenderScale",
    "HRTFActive": r"mpChBHRTF",
    "ShowEffectSubtitles": r"mpChBShowEffectSubtitles",
    "ShowDeathHints": r"mpChBShowDeathHints",
    "ShowCommentary": r"mpChBShowCommentary",
}

CANCEL_RUNTIME_PATTERNS = {
    "ShowEffectSubtitles": (
        r"gpBase->mpMessageHandler->SetShowEffectSubtitles\s*\(\s*"
        r"mInitialValues\.GetVarBool\(\s*\"ShowEffectSubtitles\"\s*\)\s*\)"
    ),
    "ShowDeathHints": (
        r"gpBase->mpPlayer->GetHelperDeath\(\)->SetShowHint\s*\(\s*"
        r"mInitialValues\.GetVarBool\(\s*\"ShowDeathHints\"\s*\)\s*\)"
    ),
    "ShowCommentary": (
        r"gpBase->mpMapHandler->SetShowCommentary\s*\(\s*"
        r"mInitialValues\.GetVarBool\(\s*\"ShowCommentary\"\s*\)\s*\)"
    ),
}

RUNTIME_SOURCES = {
    "message header": MESSAGE_HEADER,
    "message source": MESSAGE_CPP,
    "effect source": EFFECT_CPP,
    "player helpers header": PLAYER_HELPERS_HEADER,
    "player helpers source": PLAYER_HELPERS_CPP,
    "map header": MAP_HEADER,
    "map source": MAP_CPP,
    "commentary source": COMMENTARY_CPP,
}


def strip_comments(source: str) -> str:
    """Remove C++ comments while leaving string literals available to audit."""
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", source, flags=re.DOTALL)


def function_body(source: str, name: str) -> str | None:
    """Return one method body, using brace depth instead of a brittle regex."""
    source = strip_comments(source)
    match = re.search(rf"\b{re.escape(name)}\s*\([^;{{}}]*\)\s*{{", source)
    if match is None:
        return None

    opening = source.find("{", match.start(), match.end())
    depth = 0
    quote: str | None = None
    escaped = False
    for index in range(opening, len(source)):
        char = source[index]
        if quote is not None:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
            continue
        if char in ('"', "'"):
            quote = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[opening:index + 1]
    return None


def config_access(source: str, key: str, operations: str) -> bool:
    return re.search(
        rf"(?:{operations})\s*\([^\n;]*\"{re.escape(key)}\"", source
    ) is not None


def resource_access(source: str, key: str, operations: str) -> bool:
    """Check a resource-object key, honoring AMFP's SSAO name."""
    resource_key = RESOURCE_KEY_ALIASES.get(key, key)
    return config_access(source, resource_key, operations)


def apply_coverage(source: str, key: str) -> bool:
    return re.search(APPLY_PATTERNS[key], source) is not None


def cancel_coverage(source: str, key: str) -> bool:
    # Cancel must restore the modern values from the initial snapshot, not
    # merely repopulate the controls.  Require the key in the restore body so
    # this remains useful if SetInputValues is later refactored.
    if not resource_access(source, key, r"GetVar\w*|Set\w*"):
        return False
    runtime_pattern = CANCEL_RUNTIME_PATTERNS.get(key)
    return runtime_pattern is None or re.search(runtime_pattern, source) is not None


def accessor_present(source: str, accessor: str, member: str) -> bool:
    return re.search(
        rf"\bbool\s+{re.escape(accessor)}\s*\(\s*\)\s*"
        rf"\{{\s*return\s+{re.escape(member)}\s*;\s*\}}",
        strip_comments(source),
    ) is not None


def setter_present(source: str, setter: str, member: str) -> bool:
    return re.search(
        rf"\bvoid\s+{re.escape(setter)}\s*\(\s*bool\s+abX\s*\)\s*"
        rf"\{{\s*{re.escape(member)}\s*=\s*abX\s*;\s*\}}",
        strip_comments(source),
    ) is not None


def setter_declared(source: str, setter: str) -> bool:
    return re.search(
        rf"\bvoid\s+{re.escape(setter)}\s*\(\s*bool\s+abX\s*\)\s*;",
        strip_comments(source),
    ) is not None


def runtime_coverage(
    sources: dict[str, str], missing: list[str]
) -> None:
    message_header = sources["message header"]
    message_cpp = sources["message source"]
    effect_cpp = sources["effect source"]
    player_helpers_header = sources["player helpers header"]
    player_helpers_cpp = sources["player helpers source"]
    map_header = sources["map header"]
    map_cpp = sources["map source"]
    commentary_cpp = sources["commentary source"]

    if not accessor_present(message_header, "ShowEffectSubtitles", "mbShowEffectSubtitles"):
        missing.append("ShowEffectSubtitles: missing MessageHandler accessor")
    if not setter_present(message_header, "SetShowEffectSubtitles", "mbShowEffectSubtitles"):
        missing.append("ShowEffectSubtitles: missing MessageHandler setter")
    if not config_access(message_cpp, "ShowEffectSubtitles", r"GetBool"):
        missing.append("ShowEffectSubtitles: missing MessageHandler config load")
    if not config_access(message_cpp, "ShowEffectSubtitles", r"SetBool"):
        missing.append("ShowEffectSubtitles: missing MessageHandler config save")
    if re.search(
        r"if\s*\(\s*gpBase->mpMessageHandler->ShowEffectSubtitles\s*\(\s*\)\s*"
        r"==\s*false\s*\)\s*return\s*;",
        strip_comments(effect_cpp),
    ) is None:
        missing.append("ShowEffectSubtitles: missing LuxEffectHandler draw gate")

    if not accessor_present(player_helpers_header, "ShowHint", "mbShowHint"):
        missing.append("ShowDeathHints: missing LuxPlayerDeath accessor")
    if not setter_present(player_helpers_header, "SetShowHint", "mbShowHint"):
        missing.append("ShowDeathHints: missing LuxPlayerDeath setter")
    if not config_access(player_helpers_cpp, "ShowDeathHints", r"GetBool"):
        missing.append("ShowDeathHints: missing LuxPlayerDeath config load")
    if not config_access(player_helpers_cpp, "ShowDeathHints", r"SetBool"):
        missing.append("ShowDeathHints: missing LuxPlayerDeath config save")

    if not accessor_present(map_header, "GetShowCommentary", "mbShowCommentary"):
        missing.append("ShowCommentary: missing LuxMapHandler accessor")
    if not setter_declared(map_header, "SetShowCommentary"):
        missing.append("ShowCommentary: missing LuxMapHandler setter declaration")
    map_setter = function_body(map_cpp, "SetShowCommentary")
    if map_setter is None or re.search(r"mbShowCommentary\s*=\s*abX\s*;", map_setter) is None:
        missing.append("ShowCommentary: missing LuxMapHandler setter implementation")
    if map_setter is None or re.search(
        r"pIcon->SetCommentarySuppressed\s*\(\s*mbShowCommentary\s*==\s*false\s*\)\s*;",
        map_setter,
    ) is None:
        missing.append("ShowCommentary: setter must preserve and apply icon activation state")
    if not config_access(map_cpp, "ShowCommentary", r"GetBool"):
        missing.append("ShowCommentary: missing LuxMapHandler config load")
    if not config_access(map_cpp, "ShowCommentary", r"SetBool"):
        missing.append("ShowCommentary: missing LuxMapHandler config save")

    on_interact = function_body(commentary_cpp, "OnInteract")
    if on_interact is None or re.search(
        r"if\s*\(\s*gpBase->mpMapHandler->GetShowCommentary\s*\(\s*\)\s*"
        r"==\s*false\s*\)",
        on_interact,
    ) is None:
        missing.append("ShowCommentary: missing LuxCommentaryIcon interaction gate")

    on_set_active = function_body(commentary_cpp, "OnSetActive")
    if on_set_active is None or re.search(
        r"mpMeshEntity->SetActive\s*\(\s*abX\s*\)\s*;.*"
        r"mpMeshEntity->SetVisible\s*\(\s*abX\s*\)\s*;",
        on_set_active,
        re.DOTALL,
    ) is None:
        missing.append("ShowCommentary: icon activation must update mesh visibility")

    commentary_state = function_body(commentary_cpp, "SetCommentarySuppressed")
    if commentary_state is None or re.search(
        r"mbCommentaryActiveBeforeSuppressed\s*=\s*IsActive\s*\(\s*\)\s*;.*"
        r"SetActive\s*\(\s*false\s*\).*"
        r"SetActive\s*\(\s*mbCommentaryActiveBeforeSuppressed\s*\)\s*;",
        commentary_state,
        re.DOTALL,
    ) is None:
        missing.append("ShowCommentary: icon suppression must preserve prior activation state")

    if on_set_active is None or re.search(
        r"abX\s*&&\s*gpBase->mpMapHandler->GetShowCommentary\s*\(\s*\)\s*==\s*false.*"
        r"SetActive\s*\(\s*false\s*\)",
        on_set_active,
        re.DOTALL,
    ) is None:
        missing.append("ShowCommentary: icon activation must remain gated while disabled")

    play_commentary_start = function_body(effect_cpp, "Start")
    if play_commentary_start is None or re.search(
        r"gpBase->mpMapHandler->GetShowCommentary\s*\(\s*\)\s*==\s*false.*"
        r"return\s*;",
        play_commentary_start,
        re.DOTALL,
    ) is None:
        missing.append("ShowCommentary: commentary effect start must be gated")


def main() -> int:
    missing: list[str] = []

    source_files = {
        "options source": CPP,
        "options header": HEADER,
        "config source": CONFIG_CPP,
        **RUNTIME_SOURCES,
    }
    for label, path in source_files.items():
        if not path.is_file():
            missing.append(f"missing {label}: {path.relative_to(ROOT)}")
    if missing:
        print("AMFP options contract audit: FAIL")
        for issue in missing:
            print(f"- {issue}")
        return 1

    cpp = CPP.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    config_cpp = CONFIG_CPP.read_text(encoding="utf-8")
    runtime_sources = {
        label: path.read_text(encoding="utf-8")
        for label, path in RUNTIME_SOURCES.items()
    }
    code = strip_comments(cpp)
    bodies = {label: function_body(cpp, method) for label, method in LIFECYCLE}

    if FORBIDDEN_OPTIONS_REFERENCES.search(cpp) or FORBIDDEN_OPTIONS_REFERENCES.search(header):
        missing.append(
            "options source exposes an Insanity control reference"
        )

    for label, method in LIFECYCLE:
        if bodies[label] is None:
            missing.append(f"header/source missing lifecycle method: {method}")
        if re.search(rf"\b{re.escape(method)}\s*\(", header) is None:
            missing.append(f"header missing lifecycle declaration: {method}")

    for key in NEW_KEYS:
        if re.search(HEADER_PATTERNS[key], strip_comments(header)) is None:
            missing.append(f"{key}: missing header widget/state declaration")
        for label, _ in LIFECYCLE:
            body = bodies[label]
            if label == "apply":
                covered = body is not None and apply_coverage(body, key)
            elif label == "cancel/restore":
                covered = body is not None and cancel_coverage(body, key)
            elif label == "load/populate":
                covered = body is not None and resource_access(body, key, r"GetVar\w*")
            else:
                covered = body is not None and resource_access(body, key, r"AddVar\w*")
            if not covered:
                missing.append(f"{key}: missing {label} coverage")

    for key in LEGACY_KEYS:
        if key == "SSAOSamples":
            if not re.search(r'"SSAONumOfSamples"', code):
                missing.append("legacy key SSAOSamples missing AMFP resource alias SSAONumOfSamples")
            if not re.search(r'"SSAOSamples"', config_cpp):
                missing.append("legacy config key missing from cLuxConfigHandler: SSAOSamples")
        elif not resource_access(code, key, r"GetVar\w*|AddVar\w*|Set\w*"):
            missing.append(f"legacy key missing from options resource values: {key}")

        body = bodies["apply"]
        if body is None or re.search(LEGACY_APPLY_PATTERNS[key], body) is None:
            missing.append(f"{key}: missing ApplyChanges coverage")

    runtime_coverage(runtime_sources, missing)

    if missing:
        print("AMFP options contract audit: FAIL")
        for issue in missing:
            print(f"- {issue}")
        return 1

    print(
        "AMFP options contract audit: PASS "
        f"({len(NEW_KEYS)} new keys × {len(LIFECYCLE)} lifecycle phases; "
        f"{len(LEGACY_KEYS)} legacy keys present)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
