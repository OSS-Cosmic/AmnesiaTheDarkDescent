#!/usr/bin/env python3
"""Regression coverage for premake/memory_rebuild.lua."""

from __future__ import annotations

import ctypes
import ctypes.util
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "premake" / "memory_rebuild.lua"


def _lua_error(lua: ctypes.CDLL, state: ctypes.c_void_p) -> str:
    size = ctypes.c_size_t()
    value = lua.lua_tolstring(state, -1, ctypes.byref(size))
    return value[:size.value].decode(errors="replace") if value else "unknown Lua error"


def _lua_library() -> str | None:
    return ctypes.util.find_library("lua5.4") or ctypes.util.find_library("lua5.3")


def run_helper(
    root: Path,
    memory_tracking: bool,
    with_d3d12: bool,
    target: str = "linux",
) -> dict[str, str]:
    """Execute the production Lua helper in a real Lua state.

    The option table and os.target() are the equivalents available to the
    real Premake invocation.  The helper itself is loaded from the workspace;
    this is intentionally not a Python reimplementation of its logic.
    """
    library = _lua_library()
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
    lua.lua_pcallk.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_longlong,
        ctypes.c_void_p,
    ]
    lua.lua_pcallk.restype = ctypes.c_int
    lua.lua_tolstring.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    lua.lua_tolstring.restype = ctypes.c_char_p
    lua.lua_close.argtypes = [ctypes.c_void_p]

    out = root / "helper-record.txt"
    build_out = root / "build"
    (root / "build-premake" / "memory-rebuild").mkdir(parents=True, exist_ok=True)
    options = {
        "memory-tracking": "yes" if memory_tracking else "no",
        "with-d3d12": "yes" if with_d3d12 else "no",
    }
    options_lua = "{" + ", ".join(
        f"[{json.dumps(name)}] = {json.dumps(value)}" for name, value in options.items()
    ) + "}"
    script = f"""
ROOT = {json.dumps(str(root))}
BUILD_OUT = {json.dumps(str(build_out))}
_OPTIONS = {options_lua}
function memory_tracking_enabled() return _OPTIONS[\"memory-tracking\"] == \"yes\" end
os.target = function() return {json.dumps(target)} end
os.mkdir = function(_) return true end
local engine_obj, engine_target, consumer_obj, consumer_files
function objdir(v) if engine_obj == nil then engine_obj = v else consumer_obj = v end end
function targetdir(v) engine_target = v end
function files(v) consumer_files = v end
dofile({json.dumps(str(HELPER))})
memory_rebuild_engine()
memory_rebuild_consumer()
local f = assert(io.open({json.dumps(str(out))}, \"wb\"))
f:write(\"engine_obj\\t\", engine_obj, \"\\n\")
f:write(\"engine_target\\t\", engine_target, \"\\n\")
f:write(\"consumer_obj\\t\", consumer_obj, \"\\n\")
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


def _makefile(
    cxx: str,
    ar: str,
    source: Path,
    backend_obj: Path,
    archive: Path,
    stamp: Path,
    stamp_obj: Path,
    exe: Path,
) -> str:
    return f"""CXX := {cxx}
AR := {ar}
ENGINE_OBJ := {backend_obj}
ARCHIVE := {archive}
STAMP := {stamp}
STAMP_OBJ := {stamp_obj}
EXE := {exe}
CONSUMER := {source.parent / "consumer.cpp"}
BACKEND := {source}
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
"""


def check_incremental_build() -> None:
    cxx = shutil.which("g++") or shutil.which("clang++")
    ar = shutil.which("ar")
    make = shutil.which("make") or shutil.which("mingw32-make")
    nm = shutil.which("nm") or shutil.which("llvm-nm")
    if not cxx or not ar or not make or not nm:
        raise RuntimeError("g++, ar, make, and nm are required")

    with tempfile.TemporaryDirectory(prefix="memory-rebuild-") as temp:
        root = Path(temp)
        sources = root / "sources"
        sources.mkdir()
        (sources / "consumer.cpp").write_text(
            '#include <cstdio>\n'
            'extern "C" const char backend_mode[];\n'
            'extern "C" const char hpl_memory_build_mode_stamp[];\n'
            "int main() { std::printf(\"%s/%s\\n\", backend_mode, "
            "hpl_memory_build_mode_stamp); }\n"
        )
        for memory in ("no", "yes"):
            for backend in ("vulkan", "d3d12"):
                key = f"{memory}-{backend}"
                symbol = f"backend_only_{memory}_{backend}"
                (sources / f"backend_{key}.cpp").write_text(
                    f'extern "C" const char backend_mode[] = "{key}";\n'
                    f'extern "C" const char {symbol}[] = "{key}";\n'
                )

        # The first three entries explicitly exercise vulkan -> d3d12 ->
        # vulkan without changing memory tracking.  The second sequence does
        # the same while proving the option is not ignored when enabled.  The
        # extra no-vulkan entries cover both non-Windows + d3d12=yes and
        # Windows + d3d12=no fallback cases.
        sequence = [
            (False, False, "linux"),
            (False, False, "linux"),
            (False, True, "windows"),
            (False, False, "linux"),
            (False, True, "linux"),
            (False, False, "windows"),
            (True, False, "linux"),
            (True, True, "windows"),
            (True, False, "linux"),
        ]
        records: dict[str, dict[str, str]] = {}
        stamp_mtimes: dict[str, int] = {}
        exe_mtimes: dict[str, int] = {}
        previous_key: str | None = None
        exe = root / "bin" / "memory-consumer"

        for memory_tracking, with_d3d12, target in sequence:
            key = f"{'yes' if memory_tracking else 'no'}-{'d3d12' if target == 'windows' and with_d3d12 else 'vulkan'}"
            record = run_helper(root, memory_tracking, with_d3d12, target)
            stamp = Path(record["stamp"])
            stamp_text = stamp.read_text()
            if f'const char hpl_memory_build_mode_stamp[] = "{key}";' not in stamp_text:
                raise AssertionError(f"stamp does not report {key}: {stamp_text!r}")

            engine_obj = concrete(record["engine_obj"], "Engine")
            consumer_obj = concrete(record["consumer_obj"], "Consumer")
            archive = concrete(record["engine_target"], "Engine") / "libmemory.a"
            paths = {
                "engine_obj": str(engine_obj),
                "consumer_obj": str(consumer_obj),
                "archive": str(archive),
            }
            if key in records and paths != {
                field: records[key][field]
                for field in ("engine_obj", "consumer_obj", "archive")
            }:
                raise AssertionError(f"paths changed for repeated {key}: {paths}")
            for other_key, other in records.items():
                if other_key != key:
                    for field in ("engine_obj", "consumer_obj", "archive"):
                        if paths[field] == other[field]:
                            raise AssertionError(
                                f"{field} path is shared by {key} and {other_key}: {paths[field]}"
                            )
            records[key] = {**paths, "stamp": str(stamp)}

            # Identical stamp contents must not rewrite the shared source.
            before = stamp.stat().st_mtime_ns
            run_helper(root, memory_tracking, with_d3d12, target)
            if stamp.stat().st_mtime_ns != before:
                raise AssertionError(f"{key} stamp mtime changed on same-mode generation")

            source = sources / f"backend_{key}.cpp"
            backend_obj = engine_obj / "backend.o"
            stamp_obj = consumer_obj / "memory_mode_stamp.o"
            makefile = root / "Makefile"
            makefile.write_text(
                _makefile(cxx, ar, source, backend_obj, archive, stamp, stamp_obj, exe)
            )
            subprocess.run(
                [make, "-f", str(makefile)],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            output = subprocess.check_output([str(exe)], text=True).strip()
            if output != f"{key}/{key}":
                raise AssertionError(f"{key} executable reported {output!r}")

            members = subprocess.check_output([ar, "t", str(archive)], text=True).splitlines()
            if members != ["backend.o"]:
                raise AssertionError(f"{key} archive has stale members: {members}")
            symbols = subprocess.check_output([nm, "-g", str(archive)], text=True)
            current_symbol = f"backend_only_{key.replace('-', '_')}"
            if current_symbol not in symbols:
                raise AssertionError(f"{key} archive is missing {current_symbol}")
            for other_key in records:
                if other_key != key and f"backend_only_{other_key.replace('-', '_')}" in symbols:
                    raise AssertionError(f"{key} archive retained stale {other_key} symbol")

            current_stamp_mtime = stamp.stat().st_mtime_ns
            current_exe_mtime = exe.stat().st_mtime_ns
            if previous_key == key and current_stamp_mtime != stamp_mtimes[key]:
                raise AssertionError(f"{key} stamp changed during repeated make")
            if previous_key == key and current_exe_mtime != exe_mtimes[key]:
                raise AssertionError(f"{key} executable changed during repeated make")
            stamp_mtimes[key] = current_stamp_mtime
            exe_mtimes[key] = current_exe_mtime
            previous_key = key


class MemoryRebuildTest(unittest.TestCase):
    def test_ctypes_lua_and_incremental_make_regression(self) -> None:
        missing = [
            name
            for name, command in (
                ("Lua", _lua_library()),
                ("C++ compiler", shutil.which("g++") or shutil.which("clang++")),
                ("ar", shutil.which("ar")),
                ("make", shutil.which("make") or shutil.which("mingw32-make")),
                ("nm", shutil.which("nm") or shutil.which("llvm-nm")),
            )
            if not command
        ]
        if missing:
            self.skipTest("missing regression prerequisites: " + ", ".join(missing))
        check_incremental_build()


if __name__ == "__main__":
    unittest.main()
