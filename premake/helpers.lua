-- premake/helpers.lua -- shared paths, glob, and output-dir helpers.
-- (Slang shader compilation lives in premake/slang.lua.)
-- All helpers are global so the dofile'd sub-scripts can use them directly.

DEPS_EXTERN  = ROOT .. "/HPL2/extern"
DEPS_SOURCES = DEPS_EXTERN   -- merged: submodules + vendored sources share one dir
CONFIG_DIR   = ROOT .. "/premake/config"   -- pre-generated, per-platform config headers
BUILD_OUT    = "%{wks.location}"           -- build-premake/

-- Public headers for the in-tree dependencies. Replaces the old aggregated
-- HPL2/dependencies/include tree (removed): submodule libs (ogg, vorbis) expose
-- their own include/, vendored libs (DevIL, Newton, OALWrapper, freealut) expose
-- headers next to their sources. openal-soft and SDL headers come separately via
-- link_openal() / link_sdl2().
function deps_public_includes()
    includedirs {
        CONFIG_DIR .. "/common",            -- generated <ogg/config_types.h>
        DEPS_EXTERN .. "/ogg/include",      -- <ogg/*.h>
        DEPS_EXTERN .. "/vorbis/include",   -- <vorbis/*.h>
        DEPS_SOURCES .. "/DevIL/include",   -- <IL/il.h>
        DEPS_SOURCES .. "/Newton",          -- <Newton.h>
        DEPS_SOURCES .. "/OpenAL",          -- "OpenAL/OAL_*.h" (OALWrapper public)
        DEPS_SOURCES .. "/freealut",        -- <AL/EFX-Util.h>, <AL/xram.h>
        DEPS_SOURCES .. "/freealut/AL",     -- <alut.h>
    }
end

-- Runtime artifacts + shared libs go to build-premake/amnesia/<config>/
-- alongside the game; the optional subdir (e.g. "libs") nests under it.
function runtime_dir(subdir)
    local d = BUILD_OUT .. "/amnesia/%{cfg.buildcfg}"
    if subdir and subdir ~= "" then d = d .. "/" .. subdir end
    return d
end

-- Standard output layout for a target. kind: "runtime" (exe / shared lib that
-- must sit next to Amnesia) or "static" (intermediate static lib).
function set_output(kind)
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    if kind == "runtime" then
        targetdir (runtime_dir(""))
        -- The engine resolves config/resources relative to the working dir.
        -- Windows developers set ATDD_DIR to the game install/data directory.
        filter "system:windows"
            debugdir "$(ATDD_DIR)"
        filter "system:not windows"
            debugdir (runtime_dir(""))
        filter {}
    else
        targetdir (BUILD_OUT .. "/lib/%{cfg.buildcfg}")
    end
end

-- glob(patterns) -> expands a list of ROOT-relative or absolute matchfiles
-- patterns into a flat list; patterns with no matches contribute no files.
function glob(patterns)
    local out = {}
    for _, p in ipairs(patterns) do
        for _, f in ipairs(os.matchfiles(p)) do
            table.insert(out, f)
        end
    end
    return out
end

-- MathLib is header-only. Consumers call mathlib_use() to pull in its include dir.
--
-- On Linux/x86_64 ml.h falls back to ML_INTRINSIC_LEVEL = SSE3 (no __SSE4_2__ macro).
-- That level's SSE4.1 emulation (emu_mm_dp_ps) is built from the SSE3 intrinsic
-- _mm_hadd_ps, which GCC will not inline unless SSSE3 is enabled at the target. The
-- x86_64 baseline is only SSE2, so we enable it explicitly with -mssse3 (SSSE3 -- NOT
-- -msse4.2). -mssse3 makes the intrinsics compile WITHOUT defining __SSE4_2__, so the
-- intrinsic level stays SSE3 and the math is bit-identical to the reference build.
-- Do NOT use -msse4.2: it bumps ML_INTRINSIC_LEVEL to SSE4 and diverged the renderer's
-- vector/matrix math (culling, decal projection) -> missing meshes/decals.
-- MSVC defines ML_INTRINSIC_LEVEL=1 via premake5.lua.
function mathlib_use()
    includedirs { DEPS_EXTERN .. "/MathLib" }
    filter "toolset:gcc or clang"
        buildoptions { "-mssse3" }
    filter {}
end

-- Vulkan headers + VMA are header-only. Also emit the platform surface defines:
-- volk is compiled here as a plain premake project, so nothing propagates them
-- to its consumers automatically, and the
-- engine's surface-creation code (RISwapchain.cpp) is #ifdef-gated on
-- VK_USE_PLATFORM_*_KHR -- without the define here the surface is never created
-- and the first vkGetPhysicalDeviceSurfaceSupportKHR call dereferences garbage.
function vulkan_includes()
    includedirs {
        DEPS_EXTERN .. "/Vulkan-Headers/include",
        DEPS_EXTERN .. "/VulkanMemoryAllocator/include",
        DEPS_EXTERN .. "/volk",
    }
    vulkan_platform_defines()
end

-- Per-platform Vulkan surface defines (volk's VOLK_STATIC_DEFINES equivalent).
function vulkan_platform_defines()
    filter "system:windows"
        defines { "VK_USE_PLATFORM_WIN32_KHR" }
    filter "system:linux"
        if _OPTIONS["graphics-x11"] ~= "off" then
            defines { "VK_USE_PLATFORM_XLIB_KHR" }
        end
        if _OPTIONS["graphics-wayland"] ~= "off" then
            defines { "VK_USE_PLATFORM_WAYLAND_KHR" }
        end
    filter {}
end

-- BuildID source/header. These files are checked into the tree and are used
-- as-is; add the committed per-platform source + header to the file list.
function buildid(idname, dir)
    local suffix = os.target() == "windows" and "Win32" or "Linux"
    files {
        dir .. "/BuildID_" .. idname .. "_" .. suffix .. ".cpp",
        dir .. "/BuildID_" .. idname .. ".h",
    }
end

CMAKE = _OPTIONS["cmake"] or "cmake"

-- Link the engine + its full dependency set into a final executable. premake
-- does NOT transitively propagate a static library's links to consumers, so
-- every executable (Amnesia, editors, MshConverter) must link the whole set.
-- linkgroups handles the circular static-lib references (vorbis<->ogg, etc.).
function link_engine()
    links {
        "HPL2", "OALWrapper", "AngelScript", "Newton", "tinyxml2",
        "vorbisfile", "vorbis", "ogg", "freealut",
        "zlib", "volk", "IL", "png", "jpeg",
    }
    link_sdl2()
    link_openal()
    link_nrd()
    link_fsr()
    link_xess()
    memory_consumer()
    memory_rebuild_consumer()
    filter "system:linux"
        links { "pthread", "dl" }
    filter "system:windows"
        -- Static SDL2/openal-soft do not bring their CMake target dependency
        -- lists with them, so final Windows executables link the Win32 libs here.
        links {
            "kernel32",
            "setupapi",
            "winmm",
            "imm32",
            "version",
            "cfgmgr32",
            "ole32",
            "oleaut32",
            "uuid",
            "advapi32",
            "user32",
            "gdi32",
            "shell32",
            "avrt",
            "dinput8",
        }
    filter "toolset:gcc or clang"
        linkgroups "On"
    filter {}
end
