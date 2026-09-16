#!/usr/bin/env python3
"""Standalone incremental-build test for premake/memory_rebuild.lua."""

from __future__ import annotations

import ctypes
import ctypes.util
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "premake" / "memory_rebuild.lua"


def _lua_error(lua: ctypes.CDLL, state: ctypes.c_void_p) -> str:
    size = ctypes.c_size_t()
    value = lua.lua_tolstring(state, -1, ctypes.byref(size))
    return value[:size.value].decode(errors="replace") if value else "unknown Lua error"


def run_helper(root: Path, mode: str | None) -> dict[str, str]:
    """Execute the production Lua helper in a real Lua state."""
    library = ctypes.util.find_library("lua5.4") or ctypes.util.find_library("lua5.3")
    if not library:
        raise RuntimeError("liblua5.3 or liblua5.4 is required")
    lua = ctypes.CDLL(library)
    lua.luaL_newstate.restype = ctypes.c_void_p
    state = lua.luaL_newstate()
    if not state:
        raise RuntimeError("luaL_newstate failed")
    lua.luaL_openlibs.argtypes = [ctypes.c_void_p]
    lua.luaL_openlibs(state)
    lua.luaL_loadstring.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lua.luaL_loadstring.restype = ctypes.c_int
    lua.lua_pcallk.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                               ctypes.c_int, ctypes.c_longlong, ctypes.c_void_p]
    lua.lua_pcallk.restype = ctypes.c_int
    lua.lua_tolstring.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_size_t)]
    lua.lua_tolstring.restype = ctypes.c_char_p
    lua.lua_close.argtypes = [ctypes.c_void_p]

    out = root / "helper-record.txt"
    build_out = root / "build"
    (root / "build-premake" / "memory-rebuild").mkdir(parents=True, exist_ok=True)
    selected = "nil" if mode is None else ("true" if mode == "yes" else "false")
    script = f"""
ROOT = {json.dumps(str(root))}
BUILD_OUT = {json.dumps(str(build_out))}
_OPTIONS = {{}}
function memory_tracking_enabled() return _OPTIONS[\"memory-tracking\"] == \"yes\" end
os.mkdir = function(_) return true end
local engine_obj, engine_target, consumer_obj, consumer_target, consumer_files
consumer_target = ""
function objdir(v) if not engine_obj then engine_obj = v else consumer_obj = v end end
function targetdir(v) engine_target = v end
function files(v) consumer_files = v end
dofile({json.dumps(str(HELPER))})
memory_rebuild_engine({selected})
memory_rebuild_consumer({selected})
local f = assert(io.open({json.dumps(str(out))}, \"wb\"))
f:write(\"engine_obj\\t\", engine_obj, \"\\n\")
f:write(\"engine_target\\t\", engine_target, \"\\n\")
f:write(\"consumer_obj\\t\", consumer_obj, \"\\n\")
f:write(\"consumer_target\\t\", consumer_target, \"\\n\")
f:write(\"stamp\\t\", consumer_files[1], \"\\n\")
f:close()
"""
    try:
        if lua.luaL_loadstring(state, script.encode()) != 0:
            raise RuntimeError(_lua_error(lua, state))
        if lua.lua_pcallk(state, 0, 0, 0, 0, None) != 0:
            raise RuntimeError(_lua_error(lua, state))
    finally:
        lua.lua_close(state)
    return dict(line.split("\t", 1) for line in out.read_text().splitlines())


def concrete(path: str, project: str) -> Path:
    return Path(path.replace("%{prj.name}", project).replace("%{cfg.buildcfg}", "Debug"))


def check_incremental_build() -> None:
    cxx, ar, make = shutil.which("g++") or shutil.which("clang++"), shutil.which("ar"), shutil.which("make")
    if not cxx or not ar or not make:
        raise RuntimeError("g++, ar, and make are required")
    with tempfile.TemporaryDirectory(prefix="memory-rebuild-") as temp:
        root = Path(temp)
        sources = root / "sources"
        sources.mkdir()
        (sources / "backend_yes.cpp").write_text('extern "C" const char backend_mode[] = "yes";\nextern "C" const char backend_only[] = "backend-only";\n')
        (sources / "backend_no.cpp").write_text('extern "C" const char backend_mode[] = "no";\n')
        (sources / "consumer.cpp").write_text('#include <cstdio>\nextern "C" const char backend_mode[];\nextern "C" const char hpl_memory_build_mode_stamp[];\nint main() { std::printf("%s/%s\\n", backend_mode, hpl_memory_build_mode_stamp); }\n')

        initial = run_helper(root, None)
        if '"no"' not in Path(initial["stamp"]).read_text():
            raise AssertionError("default memory mode did not generate no stamp")
        previous_stamp_mtime: dict[str, int] = {}
        previous_exe_mtime: dict[str, int] = {}
        previous_mode: str | None = None
        for mode in ("yes", "yes", "no", "no", "yes"):
            record = run_helper(root, mode)
            stamp = Path(record["stamp"])
            before = stamp.stat().st_mtime_ns
            run_helper(root, mode)
            if stamp.stat().st_mtime_ns != before:
                raise AssertionError(f"{mode} stamp mtime changed on same-mode generation")
            obj = concrete(record["consumer_obj"], "Consumer")
            backend_obj = concrete(record["engine_obj"], "Engine") / "backend.o"
            archive = concrete(record["engine_target"], "Engine") / "libmemory.a"
            stamp_obj, exe = obj / "memory_mode_stamp.o", root / "bin" / "memory-consumer"
            mf = root / "Makefile"
            mf.write_text(f"""CXX := {cxx}
AR := {ar}
MODE ?= no
ENGINE_OBJ := {backend_obj}
ARCHIVE := {archive}
STAMP := {stamp}
STAMP_OBJ := {stamp_obj}
EXE := {exe}
BACKEND := {sources}/backend_$(MODE).cpp
CONSUMER := {sources}/consumer.cpp
.PHONY: all
all: $(EXE)
$(EXE): $(CONSUMER) $(STAMP_OBJ) $(ARCHIVE)
\tmkdir -p $(dir $@)
\t$(CXX) $(CONSUMER) $(STAMP_OBJ) $(ARCHIVE) -o $@
$(STAMP_OBJ): $(STAMP)
\tmkdir -p $(dir $@)
\t$(CXX) -std=c++17 -c $< -o $@
$(ENGINE_OBJ): $(BACKEND)
\tmkdir -p $(dir $@)
\t$(CXX) -std=c++17 -c $< -o $@
$(ARCHIVE): $(ENGINE_OBJ)
\tmkdir -p $(dir $@)
\t$(AR) rcs $@ $<
""")
            subprocess.run([make, "-f", str(mf), f"MODE={mode}"], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            output = subprocess.check_output([str(exe)], text=True).strip()
            if output != f"{mode}/{mode}":
                raise AssertionError(f"mode {mode} produced {output!r}; obj={backend_obj} archive={archive}")
            members = subprocess.check_output([ar, "t", str(archive)], text=True).splitlines()
            if members != ["backend.o"]:
                raise AssertionError(f"{mode} archive has stale members: {members}")
            nm = subprocess.check_output(["nm", "-g", str(archive)], text=True)
            if ("backend_only" in nm) != (mode == "yes"):
                raise AssertionError(f"backend-only symbol isolation failed for {mode}")
            if previous_mode == mode and stamp.stat().st_mtime_ns != previous_stamp_mtime[mode]:
                raise AssertionError(f"{mode} stamp changed after repeated make setup: {previous_stamp_mtime[mode]} -> {stamp.stat().st_mtime_ns}")
            if previous_mode == mode and exe.stat().st_mtime_ns != previous_exe_mtime[mode]:
                raise AssertionError(f"{mode} executable changed during same-mode make")
            previous_stamp_mtime[mode] = stamp.stat().st_mtime_ns
            previous_exe_mtime[mode] = exe.stat().st_mtime_ns
            previous_mode = mode


if __name__ == "__main__":
    check_incremental_build()
    print("[memory-rebuild] passed: ctypes Lua + ordinary make mode sequence")
