-- premake5.lua -- build system for HPL2 / Amnesia (Surfel-GI fork)
--
-- This file lets you generate a Visual Studio solution or gmake2 Makefiles via:
--
--     premake5 vs2026       (Windows)
--     premake5 gmake2       (Linux)
--     make -C build-premake config=release -j$(nproc)
--
-- Most in-tree third-party libs are built from source as hand-written premake
-- projects (premake/deps/*.lua). The two libraries that ship their own large,
-- feature-probing CMake build systems -- SDL2 and openal-soft -- are built by
-- driving their own CMake as a prebuild step and then linked (premake/external.lua).
-- See BUILD.md "Building with premake" for details.

-- Repo root (directory containing this file). Every path below is built off it
-- so the included sub-scripts don't depend on their own location.
ROOT = _MAIN_SCRIPT_DIR

dofile "premake/options.lua"

if _OPTIONS["with-d3d12"] == "yes" and os.target() ~= "windows" then
    premake.error("--with-d3d12=yes is only supported on Windows")
end

dofile "premake/helpers.lua"
dofile "premake/memory.lua"
dofile "premake/memory_rebuild.lua"
dofile "premake/agility.lua"
dofile "premake/slang.lua"   -- Slang slangc download + per-file SPIR-V build rule (uses runtime_dir from helpers)
dofile "premake/modules/export-compile-commands.lua"   -- `premake5 export-compile-commands` action

workspace "Amnesia"
    location (ROOT .. "/build-premake")
    configurations { "Debug", "Release" }
    architecture "x86_64"
    cppdialect "C++20"
    cdialect "gnu11"   -- HPL2 uses GNU extensions such as alloca
    startproject "Amnesia"

    -- Use the MSVC DLL runtime in Debug and Release configurations.
    staticruntime "off"

    -- HPL2 sources assume UNICODE/_UNICODE are not defined globally
    -- (PlatformWin32.cpp opts in file-locally). Premake defaults to Unicode, so
    -- use MultiByte for the generated Visual Studio projects.
    characterset "MBCS"

    filter "configurations:Debug"
        defines { "_DEBUG" }
        symbols "On"
        runtime "Debug"
    filter "configurations:Release"
        defines { "NDEBUG" }
        optimize "On"
        symbols "On"      -- RelWithDebInfo: -O2 + -g (DWARF) for profiling/RGP
        runtime "Release"
    filter {}

    -- HPL2 / the RI layer use unions + type-punning that violate strict aliasing.
    -- At -O2 gcc/clang's strict-aliasing assumption miscompiles them (release-only
    -- SIGSEGV in RISharedPointer<RITextureView>::Get / cHybridRenderer::Draw). MSVC
    -- doesn't assume strict aliasing, so this is gcc/clang only. Workspace-wide so
    -- HPL2, the game, the tools and the from-source deps all get it (the game-only
    -- amnesia.lua flag covered just Amnesia and missed the engine).
    filter "system:not windows"
        buildoptions { "-fno-strict-aliasing" }
    filter {}

    filter "system:windows"
        systemversion "latest"
        defines { "NOMINMAX=1", "ML_INTRINSIC_LEVEL=1", "_CRT_SECURE_NO_WARNINGS" }
    filter {}
    -- Fresh filter block so the workspace-scope DEVICE_SUPPORT_D3D12 define reliably
    -- propagates to every project (premake's filter-state can otherwise skip the
    -- nested `defines` call and leak into per-project preprocessor lists).
    if _OPTIONS["with-d3d12"] == "yes" then
        filter "system:windows"
            defines { "DEVICE_SUPPORT_D3D12" }
            if AGILITY_INCLUDE_DIR then
                includedirs { AGILITY_INCLUDE_DIR, AGILITY_INCLUDE_DIR .. "/d3dx12" }
            end
        filter {}
    end
    -- Cap compiler workers per project. An unrestricted /MP multiplied by an
    -- unrestricted MSBuild /m can exhaust the PDB service during a Debug build.
    -- /FS lets the remaining workers coordinate writes to each project PDB.
    filter { "system:windows", "action:vs*" }
        buildoptions { "/MP4", "/FS" }
    filter {}

-- Build-only helper used by the opt-in Windows/D3D12 shader rules. It has no
-- engine dependencies and emits generated headers under build-premake/.
if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    project "RIShaderEmbed"
        kind "ConsoleApp"
        language "C++"
        targetname "ri_shader_embed"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tools/%{cfg.buildcfg}")
        files { ROOT .. "/tools/ri_shader_embed.cpp" }
end


-- External CMake-driven dependencies (SDL2 + openal-soft).
dofile "premake/external.lua"

-- XeSS is shared by Amnesia and the editor tools. Stage it once so parallel
-- project builds do not race while replacing the same runtime DLL.
xess_declare_staging_projects()

-- Agility SDK staging projects (Windows + --with-d3d12=yes; no-op otherwise).
-- Declared here so project() sees the workspace scope, matching external.lua.
agility_declare_staging_projects()

-- Hand-written from-source third-party dependency projects.
dofile "premake/deps/d3d12ma.lua"
dofile "premake/deps/zlib.lua"
dofile "premake/deps/ogg.lua"
dofile "premake/deps/vorbis.lua"
dofile "premake/deps/jpeg.lua"
dofile "premake/deps/png.lua"
dofile "premake/deps/tinyxml2.lua"
dofile "premake/deps/freealut.lua"
dofile "premake/deps/newton.lua"
dofile "premake/deps/angelscript.lua"
dofile "premake/deps/devil.lua"
dofile "premake/deps/volk.lua"
dofile "premake/deps/oalwrapper.lua"
-- MathLib is header-only (interface): no project, consumed via mathlib_use() in helpers.

-- Engine, game and tools.
dofile "premake/hpl2.lua"
dofile "premake/amnesia.lua"
if _OPTIONS["with-tools"] ~= "no" then
    dofile "premake/tools.lua"
end
if _OPTIONS["with-tests"] ~= "no" then
    dofile "premake/tests.lua"
end
