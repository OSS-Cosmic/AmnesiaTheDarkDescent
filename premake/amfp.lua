-- Amnesia: A Machine for Pigs game executable project.
-- The AMFP source tree is kept separate from the TDD Amnesia target so the two
-- products can be generated and staged independently.
local AMFP = ROOT .. "/amfp"
local PRODUCT = product_context("amfp")

product_activate(PRODUCT)

project(PRODUCT.project)
    language "C++"
    set_output("runtime", PRODUCT.product)
    targetname "AmnesiaAMFP"
    filter "system:windows"
        kind "WindowedApp"
    filter "system:not windows"
        kind "ConsoleApp"
    filter {}

    files { AMFP .. "/src/game/*.cpp", AMFP .. "/src/game/*.h" }
    removefiles {
        AMFP .. "/src/game/LuxProp_OilBarrel.cpp",
        AMFP .. "/src/game/LuxInsanityHandler.cpp",
    }

    includedirs {
        ROOT .. "/HPL2/core/include",
        AMFP .. "/src/game",
        ROOT .. "/amnesia/slang",
        ROOT .. "/amnesia/glsl",
        ANGELSCRIPT_AMFP_INCLUDE,
        DEPS_EXTERN .. "/tinyxml2",
    }
    config_stamp(PRODUCT.project, { "AMFP", ANGELSCRIPT_AMFP_INCLUDE })
    generated_includes()
    deps_public_includes()
    vulkan_includes()
    mathlib_use()

    defines {
        "USERDIR_RESOURCES",
        "USE_GAMEPAD",
        "USE_SDL2",
    }

    link_engine("game", PRODUCT.product)

    -- AMFP uses the shared shader sources but gets its own generated shader
    -- output under build-premake/amfp/<configuration>/.
    slang_prebuild(PRODUCT)
    if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
        defines { "DEVICE_SUPPORT_D3D12" }
        slang_dxil_production_prebuild(PRODUCT)
    end

    -- LuxBase.cpp writes a crash minidump with MiniDumpWriteDump.
    filter "system:windows"
        links { "dbghelp" }
    filter "system:linux"
        defines { "LINUX" }
        buildoptions { "-Wno-switch", "-Wno-undefined-var-template", "-Wno-extern-c-compat" }
        linkoptions { "-Wl,-rpath,'$$ORIGIN/libs'", "-Wl,-rpath,'$$ORIGIN'" }
        linkgroups "On"
    filter {}

product_deactivate()
