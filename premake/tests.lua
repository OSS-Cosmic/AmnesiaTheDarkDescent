-- Keep the jitter/motion-vector and reactive-mask math contracts covered by
-- headless tests. The math source links nothing, so it stays independent of
-- engine/GPU deps.
-- The tests/graphics/*.cpp glob is intentional: another TU with main() would
-- clash; put new tests in this TU or give them their own project.

-- cmd.exe can parse slash-separated paths as switches, and spaces in an
-- unquoted checkout path split the post-build command. %{cfg.buildtarget.abspath}
-- expands with forward slashes even for vs2022, and path.translate applied to the
-- token expands too late to help, so the Windows branch spells the separators
-- statically. It therefore has to mirror targetdir below -- keep the two in sync.
local function add_test_postbuild()
    filter "system:windows"
        postbuildcommands {
            '"' .. path.translate("tests/%{cfg.buildcfg}/%{prj.name}.exe", "\\") .. '"'
        }
    filter "system:not windows"
        postbuildcommands { '"%{cfg.buildtarget.abspath}"' }
    filter {}
end

-- unittest discovery does not recurse into directories without __init__.py.
-- Keep the search under tests/ and use *_test.py so the argparse-only
-- tests/fsr/check_shader_regeneration.py and
-- tests/fsr/check_patch_restaging.py are not treated as test modules.
-- The runner preserves these rules and maps an empty suite to success so it
-- does not fail the build; real test failures remain fatal.
local function add_python_test_postbuild()
    filter "system:windows"
        postbuildcommands {
            'cd /d "' .. path.translate(ROOT, "\\") .. '" && "'
                .. path.translate(_OPTIONS["python"] or "python", "\\")
                .. '" "'
                .. path.translate(ROOT .. "/scripts/run_python_tests.py", "\\")
                .. '"'
        }
    filter "system:not windows"
        postbuildcommands {
            'cd "' .. ROOT .. '" && "'
                .. (_OPTIONS["python"] or "python3")
                .. '" "' .. ROOT .. '/scripts/run_python_tests.py"'
        }
    filter {}
end

-- Keep the test foundation entirely local and offline. Every C++ test project
-- calls this helper, including memory variants and conditional FSR projects.
local function add_utest()
    includedirs { ROOT .. "/tests/third_party/utest" }
end

project "TemporalCameraTests"
    kind "ConsoleApp"
    language "C++"
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
    files {
        ROOT .. "/tests/graphics/*.cpp",
        ROOT .. "/HPL2/core/sources/graphics/TemporalCamera.cpp",
        ROOT .. "/HPL2/core/sources/graphics/TemporalUpscalerPolicy.cpp",
        ROOT .. "/HPL2/core/sources/graphics/TemporalReactiveMaskMath.cpp",
        ROOT .. "/HPL2/core/sources/graphics/WaterReflectionJitterMath.cpp",
        ROOT .. "/HPL2/core/sources/graphics/FsrUpscalerParams.cpp",
        ROOT .. "/HPL2/core/sources/graphics/RayConeLod.cpp",
        ROOT .. "/HPL2/core/sources/graphics/CubeMipGen.cpp",
        ROOT .. "/HPL2/core/sources/graphics/BlockCompressionDecode.cpp",
        ROOT .. "/HPL2/core/sources/graphics/RIFormat.c",
        ROOT .. "/HPL2/core/sources/graphics/StandardShadowCull.cpp",
        ROOT .. "/HPL2/core/sources/graphics/RendererBackendSwitch.cpp",
    }
    -- StandardShadowCull.cpp shares its predicates with the cull compute shader
    -- through amnesia/slang/StandardCull.h, and the frustum test compares itself
    -- against MathLib's MvpToPlanes, so both include paths are required here.
    includedirs { ROOT .. "/HPL2/core/include", ROOT .. "/amnesia/slang" }
    mathlib_use()
    add_utest()
    add_test_postbuild()
    -- gmake2 drops postbuildcommands on kind "Utility" projects, so the python
    -- suite rides on the first test project instead of getting its own.
    --
    -- That suite reads COMPILED shaders out of <runtime>/compiled_shaders, which
    -- only the Amnesia project produces (slang_prebuild() in premake/amnesia.lua).
    -- Without an explicit order dependency a parallel make can link this project
    -- and fire the postbuild while Amnesia is still compiling, and the shader
    -- tests then find nothing. They fail loudly rather than pass vacuously
    -- ("build first, or this test proves nothing"), so the symptom is a red CI
    -- run on a cold tree and a green one on a warm tree -- the worst kind.
    if _OPTIONS["with-python-tests"] ~= "no" then
        dependson { "Amnesia" }
        add_python_test_postbuild()
    end

-- The merged light element: one object carries the retail attributes plus the
-- ray-traced "Re_" overrides. LightParameters.cpp links nothing (no engine, no
-- GPU), so the element table, the class choice and the three parameter
-- resolvers get their own headless project.
project "LightParametersTests"
    kind "ConsoleApp"
    language "C++"
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
    files {
        ROOT .. "/tests/scene/*.cpp",
        ROOT .. "/HPL2/core/sources/scene/LightParameters.cpp",
    }
    includedirs { ROOT .. "/HPL2/core/include" }
    add_utest()
    add_test_postbuild()

-- The View > Render mode submenu stopped being indexed by eRenderer once the
-- lit renderer gained one entry per backend. EditorRenderMode.h is header-only
-- and links nothing (no engine, no GUI), so the item<->(renderer, backend)
-- mapping - including how a pre-split layout's RenderMode int resolves - gets
-- its own headless project.
project "EditorRenderModeTests"
    kind "ConsoleApp"
    language "C++"
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
    files { ROOT .. "/tests/editors/*.cpp" }
    includedirs {
        ROOT .. "/HPL2/core/include",
        ROOT .. "/HPL2/tools/editors/common",
    }
    add_utest()
    add_test_postbuild()

project "ParticleScheduleTests"
    kind "ConsoleApp"
    language "C++"
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
    -- Its own project rather than a TU in TemporalCameraTests: cParticleSchedule
    -- links nothing (no engine, no GPU), so keeping it isolated makes the
    -- lifetime-parity suite runnable on its own.
    files {
        ROOT .. "/tests/graphics/particles/*.cpp",
        ROOT .. "/HPL2/core/sources/graphics/ParticleSchedule.cpp",
    }
    includedirs { ROOT .. "/HPL2/core/include" }
    add_utest()
    add_test_postbuild()

project "FsrUpscalerParamsTests"
    kind "ConsoleApp"
    language "C++"
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
    files {
        ROOT .. "/tests/graphics/fsr/*.cpp",
        ROOT .. "/HPL2/core/sources/graphics/FsrUpscalerParams.cpp",
    }
    includedirs { ROOT .. "/HPL2/core/include" }
    add_utest()
    add_test_postbuild()

-- Planar water reflection bounds and sort key. Pure math, but it calls into
-- cMath / cFrustum / cBoundingVolume, so this project links the engine math
-- sources plus a small stub TU standing in for the SDL / tinyxml2 / vertex
-- buffer symbols those pull in. Own directory because the tests/graphics/*.cpp
-- glob above already owns a main().
project "StandardWaterReflectionTests"
    kind "ConsoleApp"
    language "C++"
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
    files {
        ROOT .. "/tests/graphics/waterreflection/*.cpp",
        ROOT .. "/HPL2/core/sources/graphics/StandardWaterReflectionClip.cpp",
        ROOT .. "/HPL2/core/sources/graphics/StandardWaterReflectionSort.cpp",
        ROOT .. "/HPL2/core/sources/graphics/StandardWaterMath.cpp",
        ROOT .. "/HPL2/core/sources/math/Math.cpp",
        ROOT .. "/HPL2/core/sources/math/MathTypes.cpp",
        ROOT .. "/HPL2/core/sources/math/Frustum.cpp",
        ROOT .. "/HPL2/core/sources/math/BoundingVolume.cpp",
        ROOT .. "/HPL2/core/sources/math/Quaternion.cpp",
    }
    includedirs {
        ROOT .. "/HPL2/core/include",
        ROOT .. "/HPL2/include",
        ROOT .. "/HPL2/extern/volk",
        ROOT .. "/HPL2/extern/Vulkan-Headers/include",
        ROOT .. "/HPL2/extern/VulkanMemoryAllocator/include",
        ROOT .. "/premake/config/common",
    }
    defines { "USE_SDL2", "VK_USE_PLATFORM_XLIB_KHR" }
    mathlib_use()
    add_utest()
    add_test_postbuild()

-- The bindless slot pools are pure CPU data structures (IndexPool + ObjectPool),
-- so they test without Vulkan or the engine. Own directory because the
-- tests/graphics/*.cpp glob above already owns a main().
project "BindlessPoolTests"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
    files {
        ROOT .. "/tests/graphics/bindless/*.cpp",
        ROOT .. "/HPL2/core/sources/graphics/BindlessPool.cpp",
        ROOT .. "/HPL2/core/sources/graphics/IndexPool.cpp",
    }
    includedirs { ROOT .. "/HPL2/core/include" }
    add_utest()
    add_test_postbuild()

-- Keep this one-main-per-project rule for the non-recursive tests/resources/*.cpp
-- glob; the cache test is nested and therefore not part of this project.
project "FileSearcherTests"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
    files {
        ROOT .. "/tests/resources/*.cpp",
        ROOT .. "/HPL2/core/sources/resources/FileSearcher.cpp",
        ROOT .. "/HPL2/core/sources/system/String.cpp",
        -- String.cpp contains cColor-returning helpers; this keeps the
        -- focused link self-contained without bringing in the engine.
        ROOT .. "/HPL2/core/sources/graphics/Color.cpp",
    }
    includedirs { ROOT .. "/HPL2/core/include" }
    defines { "USE_SDL2" }
    link_sdl2()
    add_utest()
    add_test_postbuild()

-- ResourceCacheTests deliberately links only the CPU resource seams.  In
-- particular, do not pull in TextureManager/ImageManager: their constructors
-- require live graphics resources and would turn this into a GPU test.
project "ResourceCacheTests"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
    files {
        ROOT .. "/tests/resources/cache/cache.cpp",
        ROOT .. "/HPL2/core/sources/resources/ResourceManager.cpp",
        ROOT .. "/HPL2/core/sources/resources/ResourceBase.cpp",
        ROOT .. "/HPL2/core/sources/resources/FileSearcher.cpp",
        ROOT .. "/HPL2/core/sources/system/String.cpp",
        ROOT .. "/HPL2/core/sources/system/Hasher.cpp",
        ROOT .. "/HPL2/core/sources/graphics/Color.cpp",
    }
    includedirs { ROOT .. "/HPL2/core/include" }
    defines { "USE_SDL2" }
    link_sdl2()
    add_utest()
    add_test_postbuild()

-- The Fluid Studios adapter is intentionally tested as a small standalone
-- executable. Do not link HPL2 or any graphics/audio dependency here: this
-- project exercises the C allocator and C++ platform boundary in isolation.
project "FluidStudiosMemoryTests"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
    files {
        ROOT .. "/tests/memory/fluid_studios_platform_test.cpp",
    }
    defines { "MMGR_TESTING" }
    memory_backend(true)
    memory_platform(true)
    add_utest()
    add_test_postbuild()

project "FluidStudiosMemoryTestsNoBacktrace"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
    files {
        ROOT .. "/tests/memory/fluid_studios_platform_test.cpp",
    }
    defines { "MMGR_BACKTRACE=0", "MMGR_TESTING" }
    memory_backend(true)
    memory_platform(true)
    add_utest()
    add_test_postbuild()

-- The HPL bridge is compiled directly with the backend and operators so this
-- test never acquires production-engine linkage or changes the other memory
-- test projects above.
project "HplMemoryManagerTests"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
    files {
        ROOT .. "/tests/memory/hpl_memory_manager_test.cpp",
        ROOT .. "/tests/memory/hpl_memory_manager_fixture_before_main.cpp",
        ROOT .. "/tests/memory/hpl_memory_manager_fixture_after_report.cpp",
        ROOT .. "/tests/memory/hpl_memory_manager_lifetime_tests.cpp",
        ROOT .. "/tests/memory/hpl_memory_manager_macro_contract_test.cpp",
        ROOT .. "/tests/memory/hpl_memory_manager_attribution_tests.cpp",
        ROOT .. "/tests/memory/hpl_memory_log_stub.cpp",
        ROOT .. "/HPL2/core/sources/system/MemoryManager.cpp",
    }
    includedirs {
        ROOT .. "/HPL2/core/include",
        ROOT .. "/HPL2/extern/FluidStudios/MemoryManager",
        ROOT .. "/tests/memory",
    }
    defines { "MEMORY_MANAGER_ACTIVE", "MMGR_TESTING" }
    memory_engine(true)
    memory_consumer(true)
    exceptionhandling "On"
    -- Keep observable allocation-count assertions portable across compilers
    -- that may elide new/delete pairs. The bridge/backend retain Release flags;
    -- the standalone runner also tests optimized callers with a probed flag.
    filter "files:**/tests/memory/hpl_memory_manager*.cpp"
        optimize "Off"
    filter "system:not windows"
        buildoptions { "-fexceptions" }
    filter {}
    add_utest()
    add_test_postbuild()

project "HplMemoryManagerDisabledFacadeTests"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
    files {
        ROOT .. "/tests/memory/hpl_memory_manager_disabled_compile_test.cpp",
        ROOT .. "/tests/memory/hpl_memory_log_stub.cpp",
        ROOT .. "/HPL2/core/sources/system/MemoryManager.cpp",
        -- The disabled facade has no consumer helper sources; retain an
        -- explicit empty operator TU as its compile/link contract.
        ROOT .. "/HPL2/core/sources/memory/MemoryOperators.cpp",
    }
    includedirs { ROOT .. "/HPL2/core/include" }
    memory_consumer(false)
    exceptionhandling "Off"
    add_utest()
    add_test_postbuild()

-- Same one-main-per-project rule as above applies to the tests/fsr/*.cpp glob.
-- The project only exists when FSR is enabled, so --with-fsr=no emits no
-- reference to the SDK includes or the archive.
if _OPTIONS["with-fsr"] ~= "no" then
    project "FsrShaderBlobTests"
        kind "ConsoleApp"
        language "C++"
        cppdialect "C++17"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files {
            ROOT .. "/tests/fsr/*.cpp",
            ROOT .. "/HPL2/core/sources/graphics/spirv_reflect.c",
        }
        includedirs {
            ROOT .. "/HPL2/core/include",
            ROOT .. "/HPL2/core/include/graphics",
        }
        fsr_shader_blob_test_use()
        link_fsr()
        add_utest()
        add_test_postbuild()

    project "FsrVulkanLoaderTests"
        kind "ConsoleApp"
        language "C++"
        cppdialect "C++17"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files {
            -- Keep this one-main test explicit; the existing non-recursive globs must not pick it up.
            ROOT .. "/tests/fsr/vulkan_loader/fsr_vulkan_loader_test.cpp",
        }
        vulkan_includes()
        link_fsr()
        links { "volk" }
        filter "system:linux"
            links { "dl", "pthread" }
        filter {}
        add_utest()
        add_test_postbuild()
end
