-- Amnesia game executable project. Asset staging lives in deploy.sh.
local AMN = ROOT .. "/amnesia"

-- Short git hash used for AMNESIA_TDD_TAG.
local git_tag = os.outputof('git -C "' .. ROOT .. '" log -1 --format=%h') or "unknown"
git_tag = git_tag:gsub("%s+", "")
if git_tag == "" then git_tag = "unknown" end
local build_version = _OPTIONS["build-version"] or "V0000"

project "Amnesia"
    language "C++"
    set_output("runtime")
    filter "system:windows"
        kind "WindowedApp"
    filter "system:not windows"
        kind "ConsoleApp"
    filter {}

    files { AMN .. "/src/game/*.cpp", AMN .. "/src/game/*.h" }

    includedirs {
        ROOT .. "/HPL2/core/include",
        AMN .. "/slang",
        AMN .. "/glsl",
        DEPS_SOURCES .. "/AngelScript/include",
        DEPS_EXTERN .. "/tinyxml2",
    }
    generated_includes()
    deps_public_includes()   -- ogg/vorbis/IL/Newton/OALWrapper public headers
    vulkan_includes()
    mathlib_use()

    defines {
        "USERDIR_RESOURCES",
        "USE_GAMEPAD",
        'AMNESIA_TDD_VERSION="' .. build_version .. '"',
        'AMNESIA_TDD_TAG="' .. git_tag .. '"',
    }

    link_engine()      -- HPL2 + full dependency set (no transitive propagation in premake)

    -- Per-shader Slang -> SPIR-V build rules next to the executable (incremental,
    -- no python).
    slang_prebuild()
    if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
        defines { "DEVICE_SUPPORT_D3D12" }
        slang_dxil_production_prebuild()
    end

    -- RPATH so the colocated SDL2/OpenAL shared libs in ./libs are found.
    filter "system:linux"
        defines { "LINUX" }
        buildoptions { "-Wno-switch", "-Wno-undefined-var-template", "-Wno-extern-c-compat" }
        linkoptions { "-Wl,-rpath,'$$ORIGIN/libs'", "-Wl,-rpath,'$$ORIGIN'" }
        linkgroups "On"
    filter { "system:linux", "configurations:Release" }
        buildoptions { "-fno-strict-aliasing" }
    filter {}
