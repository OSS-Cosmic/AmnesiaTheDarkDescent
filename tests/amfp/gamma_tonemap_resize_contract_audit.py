#!/usr/bin/env python3
"""Check the AMFP gamma, tone-map, and resize contracts without a renderer."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GAME = ROOT / "amfp" / "src" / "game"
PATHS = {
    "config header": GAME / "LuxConfigHandler.h",
    "config source": GAME / "LuxConfigHandler.cpp",
    "map header": GAME / "LuxMapHandler.h",
    "map source": GAME / "LuxMapHandler.cpp",
    "menu header": GAME / "LuxMainMenu.h",
    "menu source": GAME / "LuxMainMenu.cpp",
    "pre-menu header": GAME / "LuxPreMenu.h",
    "pre-menu source": GAME / "LuxPreMenu.cpp",
    "options source": GAME / "LuxMainMenu_Options.cpp",
    "base header": GAME / "LuxBase.h",
    "base source": GAME / "LuxBase.cpp",
}


def strip_comments(source: str) -> str:
    """Remove C++ comments while preserving strings and source layout."""
    result: list[str] = []
    index = 0
    quote: str | None = None
    escaped = False
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if quote is not None:
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


def function_body(source: str, name: str) -> str | None:
    """Return a method body using brace depth, tolerating constructor initializers."""
    source = strip_comments(source)
    match = re.search(
        rf"\b{re.escape(name)}\s*\([^;{{}}]*\)\s*(?::[^{{}}]*)?{{", source
    )
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
        elif char in ('"', "'"):
            quote = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    return None


def require(issues: list[str], label: str, pattern: str, source: str) -> None:
    if re.search(pattern, source, flags=re.DOTALL) is None:
        issues.append(label)


def require_body(
    issues: list[str], source: str, name: str, label: str
) -> str | None:
    body = function_body(source, name)
    if body is None:
        issues.append(f"missing {label} implementation")
    return body


def require_tone_map(issues: list[str], label: str, body: str | None) -> None:
    if body is None:
        return
    for field, value in (
        ("mfExposure", r"(?:hpl::)?kSceneExposure"),
        ("mfShadowLift", r"1\.0f"),
        ("mfGamma", r"gpBase\s*->\s*mpConfigHandler\s*->\s*GetGamma\s*\(\)"),
    ):
        require(issues, f"{label}: missing {field} tone-map parameter", rf"\b{field}\s*=\s*{value}", body)


def main() -> int:
    issues: list[str] = []
    missing = [label for label, path in PATHS.items() if not path.is_file()]
    if missing:
        print("AMFP gamma/tone-map/resize contract audit: FAIL")
        for label in missing:
            print(f"- missing {label}: {PATHS[label].relative_to(ROOT)}")
        return 1

    source = {
        label: strip_comments(path.read_text(encoding="utf-8"))
        for label, path in PATHS.items()
    }

    config_h = source["config header"]
    config_cpp = source["config source"]
    require(issues, "config header: missing GetGamma declaration", r"float\s+GetGamma\s*\(\s*\)\s*const\s*;", config_h)
    require(issues, "config header: missing SetGamma declaration", r"void\s+SetGamma\s*\(\s*float\s+afGamma\s*\)\s*;", config_h)
    require(issues, "config header: missing gamma member", r"\bfloat\s+mfGamma\s*;", config_h)
    config_ctor = require_body(issues, config_cpp, "cLuxConfigHandler::cLuxConfigHandler", "config constructor")
    config_load = require_body(issues, config_cpp, "cLuxConfigHandler::LoadMainConfig", "config load")
    config_save = require_body(issues, config_cpp, "cLuxConfigHandler::SaveMainConfig", "config save")
    config_set = require_body(issues, config_cpp, "cLuxConfigHandler::SetGamma", "SetGamma")
    if config_ctor is not None:
        require(issues, "config constructor: gamma default is not 1.0", r"mfGamma\s*=\s*1\.0f", config_ctor)
    if config_load is not None:
        require(issues, "config load: Graphics/Gamma default is not 1.0", r"GetFloat\s*\(\s*\"Graphics\"\s*,\s*\"Gamma\"\s*,\s*1\.0f\s*\)", config_load)
    if config_save is not None:
        require(issues, "config save: Graphics/Gamma is not persisted", r"SetFloat\s*\(\s*\"Graphics\"\s*,\s*\"Gamma\"\s*,\s*mfGamma\s*\)", config_save)
    if config_set is not None:
        require(issues, "SetGamma: value is not stored", r"mfGamma\s*=\s*afGamma", config_set)
        require(issues, "SetGamma: map tone-map refresh is not wired", r"mpMapHandler[\s\S]*RefreshToneMapGamma\s*\(\s*\)", config_set)
        require(issues, "SetGamma: menu tone-map refresh is not wired", r"mpMainMenu[\s\S]*RefreshToneMapGamma\s*\(\s*\)", config_set)

    map_h = source["map header"]
    map_cpp = source["map source"]
    require(issues, "map header: missing RefreshToneMapGamma declaration", r"void\s+RefreshToneMapGamma\s*\(\s*\)\s*;", map_h)
    require(issues, "map header: missing tone-map member", r"iPostEffect\s*\*\s*mpPostEffect_ToneMapping\s*;", map_h)
    map_ctor = require_body(issues, map_cpp, "cLuxMapHandler::cLuxMapHandler", "map constructor")
    map_refresh = require_body(issues, map_cpp, "cLuxMapHandler::RefreshToneMapGamma", "map tone-map refresh")
    require_tone_map(issues, "map constructor", map_ctor)
    require_tone_map(issues, "map tone-map refresh", map_refresh)
    if map_ctor is not None:
        require(issues, "map constructor: tone map is not created", r"CreatePostEffect\s*\(\s*&?\s*toneMapParams\s*\)", map_ctor)
        require(issues, "map constructor: tone map is not added to the composite", r"AddPostEffect\s*\(\s*mpPostEffect_ToneMapping\s*,\s*0\s*\)", map_ctor)
    if map_refresh is not None:
        require(issues, "map tone-map refresh: params are not applied", r"mpPostEffect_ToneMapping\s*->\s*SetParams\s*\(\s*&toneMapParams\s*\)", map_refresh)

    menu_h = source["menu header"]
    menu_cpp = source["menu source"]
    require(issues, "menu header: missing RefreshToneMapGamma declaration", r"void\s+RefreshToneMapGamma\s*\(\s*\)\s*;", menu_h)
    require(issues, "menu header: missing screen-size handler member", r"EventHandler\s*<\s*const\s+cVector2l\s*&\s*>\s+mScreenSizeChangedHandler\s*;", menu_h)
    menu_ctor = require_body(issues, menu_cpp, "cLuxMainMenu::cLuxMainMenu", "main-menu constructor")
    menu_refresh = require_body(issues, menu_cpp, "cLuxMainMenu::RefreshToneMapGamma", "main-menu tone-map refresh")
    recalc = require_body(issues, menu_cpp, "cLuxMainMenu::RecalcLayout", "main-menu layout recalculation")
    resize = require_body(issues, menu_cpp, "cLuxMainMenu::OnScreenSizeChange", "main-menu screen-size handler")
    require_tone_map(issues, "main-menu constructor", menu_ctor)
    require_tone_map(issues, "main-menu tone-map refresh", menu_refresh)
    if menu_ctor is not None:
        require(issues, "main-menu constructor: tone map is not created", r"CreatePostEffect\s*\(\s*&?\s*tonemapParams\s*\)", menu_ctor)
        require(issues, "main-menu constructor: screen-size event is not connected", r"mScreenSizeChangedHandler\s*\.\s*Connect\s*\([^;]*OnScreenSizeChanged\s*\(\s*\)\s*\)", menu_ctor)
        require(issues, "main-menu constructor: initial layout is not calculated", r"\bRecalcLayout\s*\(\s*\)", menu_ctor)
    if menu_refresh is not None:
        require(issues, "main-menu tone-map refresh: params are not applied", r"mpPostEffect_ToneMap\s*->\s*SetParams\s*\(\s*&tonemapParams\s*\)", menu_refresh)
    if recalc is not None:
        require(issues, "main-menu layout: live screen size is not read", r"ScreenSizeF\s*\(\s*\)", recalc)
        require(issues, "main-menu layout: screen-dependent positions are not recalculated", r"mvTopMenuStartPos\s*=|mvLogoPos\s*=", recalc)
    if resize is not None:
        require(issues, "main-menu screen-size handler: layout is not recalculated", r"RecalcLayout\s*\(\s*\)", resize)
        require(issues, "main-menu screen-size handler: GUI recreation is not requested", r"mbRecreateGui\s*=\s*true", resize)

    pre_h = source["pre-menu header"]
    pre_cpp = source["pre-menu source"]
    require(issues, "pre-menu header: missing inline screen-size layout handler", r"void\s+OnScreenSizeChange\s*\([^)]*cVector2l[^)]*\)", pre_h)
    pre_resize = require_body(issues, pre_h, "OnScreenSizeChange", "pre-menu layout handler")
    pre_ctor = require_body(issues, pre_cpp, "cLuxPreMenu::cLuxPreMenu", "pre-menu constructor")
    if pre_resize is not None:
        require(issues, "pre-menu layout handler: offset is not recalculated", r"LuxCalcGuiSetScreenOffset\s*\(", pre_resize)
        require(issues, "pre-menu layout handler: virtual size is not refreshed", r"mpGuiSet\s*->\s*SetVirtualSize\s*\(", pre_resize)
    if pre_ctor is not None:
        require(issues, "pre-menu constructor: screen-size event is not connected", r"mScreenSizeChangedHandler\s*\.\s*Connect\s*\([^;]*OnScreenSizeChanged\s*\(\s*\)\s*\)", pre_ctor)

    options = source["options source"]
    options_ctor = require_body(issues, options, "cLuxMainMenu_Options::cLuxMainMenu_Options", "options constructor")
    set_values = require_body(issues, options, "cLuxMainMenu_Options::SetInputValues", "options input load")
    initial = require_body(issues, options, "cLuxMainMenu_Options::DumpInitialValues", "options initial snapshot")
    current = require_body(issues, options, "cLuxMainMenu_Options::DumpCurrentValues", "options current snapshot")
    apply = require_body(issues, options, "cLuxMainMenu_Options::ApplyChanges", "options apply")
    cancel = require_body(issues, options, "cLuxMainMenu_Options::PressCancel", "options cancel")
    if options_ctor is not None:
        require(issues, "options constructor: gamma minimum is not 0.3", r"mfGammaMin\s*=\s*0\.3f", options_ctor)
        require(issues, "options constructor: gamma step is not 0.05", r"mfGammaStep\s*=\s*0\.05f", options_ctor)
        require(issues, "options constructor: gamma maximum is not 2.0", r"mfGammaMax\s*=\s*2\.0f", options_ctor)
    if set_values is not None:
        require(issues, "options input load: Gamma resource value is not loaded", r"GetVarFloat\s*\(\s*\"Gamma\"\s*\)", set_values)
    if initial is not None:
        require(issues, "options initial snapshot: Gamma is missing", r"AddVarFloat\s*\(\s*\"Gamma\"", initial)
    if current is not None:
        require(issues, "options current snapshot: Gamma is missing", r"AddVarFloat\s*\(\s*\"Gamma\"", current)
    if apply is not None:
        require(issues, "options apply: config gamma is not updated", r"SetGamma\s*\(\s*GetGamma\s*\(\s*\)\s*\)", apply)
        require(issues, "options apply: window size is not applied", r"GetWindow\s*\(\s*\)\s*->\s*SetSize\s*\(", apply)
    if cancel is not None:
        require(issues, "options cancel: initial gamma is not restored", r"SetGamma\s*\(\s*mInitialValues\.GetVarFloat\s*\(\s*\"Gamma\"", cancel)

    base_h = source["base header"]
    base_cpp = source["base source"]
    require(issues, "base header: missing screen-size handler declaration", r"void\s+OnScreenSizeChange\s*\([^)]*cVector2l[^)]*\)", base_h)
    base_ctor = require_body(issues, base_cpp, "cLuxBase::cLuxBase", "base constructor")
    base_init = require_body(issues, base_cpp, "cLuxBase::InitGame", "base game initialization")
    base_resize = require_body(issues, base_cpp, "cLuxBase::OnScreenSizeChange", "base screen-size handler")
    if base_ctor is not None:
        require(issues, "base constructor: map handler pointer is not initialized", r"mpMapHandler\s*=\s*NULL", base_ctor)
        require(issues, "base constructor: main-menu pointer is not initialized", r"mpMainMenu\s*=\s*NULL", base_ctor)
    if base_init is not None:
        require(issues, "base initialization: screen-size event is not connected", r"mScreenSizeChangedHandler\s*\.\s*Connect\s*\([^;]*OnScreenSizeChanged\s*\(\s*\)\s*\)", base_init)
    if base_resize is not None:
        require(issues, "base screen-size handler: HUD offset is not recalculated", r"LuxCalcGuiSetScreenOffset\s*\(", base_resize)
        require(issues, "base screen-size handler: HUD virtual size is not refreshed", r"mpGameHudSet\s*->\s*SetVirtualSize\s*\(", base_resize)

    if issues:
        print("AMFP gamma/tone-map/resize contract audit: FAIL")
        for issue in issues:
            print(f"- {issue}")
        return 1

    print(
        "AMFP gamma/tone-map/resize contract audit: PASS "
        "(config accessors, refresh wiring, tone-map defaults, and resize layout contracts)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
