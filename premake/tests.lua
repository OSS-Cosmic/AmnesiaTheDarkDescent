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
        ROOT .. "/HPL2/core/sources/graphics/CubeMipGen.cpp",
        ROOT .. "/HPL2/core/sources/graphics/BlockCompressionDecode.cpp",
        ROOT .. "/HPL2/core/sources/graphics/RIFormat.c",
        ROOT .. "/HPL2/core/sources/graphics/RIPipelineDesc.cpp",
        ROOT .. "/HPL2/core/sources/graphics/StandardShadowCull.cpp",
        ROOT .. "/HPL2/core/sources/graphics/RendererBackendSwitch.cpp",
        ROOT .. "/HPL2/core/sources/graphics/ToneMapBackendParams.cpp",
    }
    -- StandardShadowCull.cpp shares its predicates with the cull compute shader
    -- through amnesia/slang/StandardCull.h, and the frustum test compares itself
    -- against MathLib's MvpToPlanes, so both include paths are required here.
    removefiles { ROOT .. "/tests/graphics/ri_descriptor_builders.cpp" }
    includedirs { ROOT .. "/HPL2/core/include", ROOT .. "/amnesia/slang" }
    vulkan_includes()
    mathlib_use()
    add_utest()
    add_test_postbuild()
    -- gmake2 drops postbuildcommands on kind "Utility" projects, so the python
    -- suite rides on the first test project instead of getting its own.
    --
    -- That suite reads COMPILED shaders out of <runtime>/compiled_shaders/vk, which
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
    -- LightParameters.cpp takes kLightRadianceFloor / kPointLightSourceRadiusSq /
    -- kLightReachMaxScale from amnesia/slang/Constants.h rather than keeping local
    -- copies, so the light grid and the shader cannot drift apart. This project
    -- compiles that source outside the engine, which is the only place the shader
    -- include path comes for free, so it has to be spelled out here too.
    includedirs { ROOT .. "/HPL2/core/include", ROOT .. "/amnesia/slang" }
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
        ROOT .. "/premake/config/common",
    }
    defines { "USE_SDL2" }
    vulkan_includes()
    mathlib_use()
    add_utest()
    add_test_postbuild()

-- Device-free coverage for the RIDescriptor builders.  It links the engine so
-- the test calls the production builders, but uses fake backend handles and
-- never initializes a graphics device.
project "RIDescriptorBuilderTests"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++17"
    objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
    targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
    files { ROOT .. "/tests/graphics/ri_descriptor_builders.cpp" }
    includedirs { ROOT .. "/HPL2/core/include" }
    defines { "USE_SDL2", "WIN32_LEAN_AND_MEAN", "IGNORE_HPL_MAIN" }
    vulkan_includes()
    link_engine('tests')
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
        -- These reach ffxGetPermutationBlobByIndex and the blob accessors, which
        -- only the static archive exports -- the engine itself no longer links it.
        fsr_link_static_vk_archive()
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
        -- This is the test the static archive exists for: it reaches internal
        -- ffx_vk.cpp symbols such as findMemoryTypeIndex that no shared module
        -- exports.
        fsr_link_static_vk_archive()
        links { "volk" }
        filter "system:linux"
            links { "dl", "pthread" }
        filter {}
        add_utest()
        add_test_postbuild()

    -- The engine loads FSR instead of linking it, so a module that builds but
    -- exports nothing, never deploys, or shares one backend's shader blobs with
    -- the other would otherwise go unnoticed until the game asks for an
    -- upscaler. Needs no GPU: it loads the deployed modules and makes one
    -- device-free ffxQuery call.
    project "FsrApiModuleExportsTests"
        kind "ConsoleApp"
        language "C++"
        cppdialect "C++17"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files { ROOT .. "/tests/fsr/module_exports/fsr_api_module_exports_test.cpp" }
        fsr_module_test_use()
        filter "system:linux"
            links { "dl" }
        filter {}
        add_utest()
        add_test_postbuild()
end

-- Always-available Windows Vulkan mapped-buffer flush check. This stays outside
-- the D3D12 option block so it remains usable in the default Vulkan build.
if os.target() == "windows" then
    project "RIVulkanMappedBufferFlushSmoke"
        kind "ConsoleApp"
        language "C++"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files { ROOT .. "/tests/graphics/ri_vulkan/mapped_buffer_flush_smoke.cpp" }
        includedirs { ROOT .. "/HPL2/core/include" }
        defines { "USE_SDL2", "WIN32_LEAN_AND_MEAN", "IGNORE_HPL_MAIN" }
        vulkan_includes()
        link_engine('tests')
        -- No add_test_postbuild(): GPU smoke tests run manually.
end

-- RID3D12 device bring-up smoke check. Opt-in build (requires DEVICE_SUPPORT_D3D12);
-- opt-in run because it touches real GPU state — NOT attached to the ordinary
-- test postbuild hook. Invoke manually from build-premake/tests/<config>/RID3D12DeviceSmoke.exe.
if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    project "RID3D12PushConstantShaderFixture"
        kind "StaticLib"
        language "C++"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        local push_constant_fixture_dir = ROOT .. "/tests/graphics/ri_d3d12/shaders"
        files { push_constant_fixture_dir .. "/_dxil_target.cpp" }
        slang_dxil_prebuild {
            sources = {
                { path = push_constant_fixture_dir .. "/push_constant.slang", entry = "VSMain", stage = "vertex", output = "push_constant.vert.dxil", reflection = true },
                { path = push_constant_fixture_dir .. "/push_constant.slang", entry = "PSMain", stage = "fragment", output = "push_constant.frag.dxil", reflection = true },
                { path = push_constant_fixture_dir .. "/push_constant.slang", entry = "CSMain", stage = "compute", output = "push_constant.comp.dxil", reflection = true },
                -- Split across two files on purpose: one slangc invocation per
                -- entry point means each numbers registers from scratch, which
                -- is what push_constant.slang (one file, three entries) cannot
                -- reproduce. Guards HPL_PUSH_CONSTANT_REGISTER.
                { path = push_constant_fixture_dir .. "/split_push_constant.vert.slang", entry = "VSMain", stage = "vertex", output = "split_push_constant.vert.dxil", reflection = true },
                { path = push_constant_fixture_dir .. "/split_push_constant.frag.slang", entry = "PSMain", stage = "fragment", output = "split_push_constant.frag.dxil", reflection = true },
            },
            -- amnesia/slang is on the path for HostDefinitions.h, which owns
            -- the pinned push-constant register the split fixture exercises.
            include_dirs = { push_constant_fixture_dir, ROOT .. "/amnesia/slang" },
            output_dir = BUILD_OUT .. "/tests/%{cfg.buildcfg}/compiled_shaders/d3d12",
        }

    project "RID3D12PushConstantSmoke"
        kind "ConsoleApp"
        language "C++"
        cppdialect "C++20"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files { ROOT .. "/tests/graphics/ri_d3d12/push_constant_smoke.cpp" }
        includedirs { ROOT .. "/HPL2/core/include", ROOT .. "/amnesia/slang" }
        defines { "USE_SDL2", "WIN32_LEAN_AND_MEAN", "IGNORE_HPL_MAIN" }
        vulkan_includes()
        link_engine('tests')
        dependson { "RID3D12PushConstantShaderFixture" }
        -- Manual: requires a real D3D12 device.
end

if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    project "RID3D12PSOSmoke"
        kind "ConsoleApp"
        language "C++"
        cppdialect "C++20"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files { ROOT .. "/tests/graphics/ri_d3d12/pso_smoke.cpp" }
        includedirs { ROOT .. "/HPL2/core/include" }
        -- Exercise RIProgram's descriptor-binding contract in the smoke path:
        -- the test must bind the output through DescriptorBinding, not a
        -- private shader-visible UAV heap.
        defines { "USE_SDL2", "WIN32_LEAN_AND_MEAN", "IGNORE_HPL_MAIN", "RI_D3D12_DESCRIPTOR_BINDING_CONTRACT" }
        vulkan_includes()
        link_engine('tests')
        dependson { "RID3D12ShaderFixtures" }
        -- No add_test_postbuild(): this smoke requires a real D3D12 device.
end

if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    project "RID3D12RenderScopeSmoke"
        kind "ConsoleApp"
        language "C++"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files {
            ROOT .. "/tests/graphics/ri_d3d12/render_scope_smoke.cpp",
        }
        includedirs {
            ROOT .. "/HPL2/core/include",
        }
        defines { "USE_SDL2", "WIN32_LEAN_AND_MEAN", "IGNORE_HPL_MAIN" }
        vulkan_includes()
        link_engine('tests')
        -- No add_test_postbuild(): GPU smoke tests run manually.
end

if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    project "RID3D12GlobalManagedSetsSmoke"
        kind "ConsoleApp"
        language "C++"
        cppdialect "C++20"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files { ROOT .. "/tests/graphics/ri_d3d12/global_managed_sets_smoke.cpp" }
        includedirs { ROOT .. "/HPL2/core/include", ROOT .. "/amnesia/slang" }
        defines { "USE_SDL2", "WIN32_LEAN_AND_MEAN", "IGNORE_HPL_MAIN" }
        vulkan_includes()
        link_engine('tests')
        -- No add_test_postbuild(): this smoke requires a real D3D12 device.
end

if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    project "RID3D12DeviceSmoke"
        kind "ConsoleApp"
        language "C++"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files {
            ROOT .. "/tests/graphics/ri_d3d12/device_smoke.cpp",
        }
        includedirs {
            ROOT .. "/HPL2/core/include",
        }
        -- IGNORE_HPL_MAIN keeps LowLevelSystemSDL from providing its own WinMain
        -- (which expects hplMain) so this test can use a plain main().
        defines { "USE_SDL2", "WIN32_LEAN_AND_MEAN", "IGNORE_HPL_MAIN" }
        vulkan_includes()   -- RIPreamble.h still pulls in volk.h under DEVICE_SUPPORT_VULKAN
        link_engine('tests')       -- HPL2 + full dep set; carries d3d12/dxgi/dxguid via link_engine
        -- No add_test_postbuild(): GPU smoke tests run manually.
end

if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    project "RID3D12BufferSmoke"
        kind "ConsoleApp"
        language "C++"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files {
            ROOT .. "/tests/graphics/ri_d3d12/buffer_smoke.cpp",
        }
        includedirs {
            ROOT .. "/HPL2/core/include",
        }
        defines { "USE_SDL2", "WIN32_LEAN_AND_MEAN", "IGNORE_HPL_MAIN" }
        vulkan_includes()
        link_engine('tests')
        -- No add_test_postbuild(): GPU smoke tests run manually.
end

if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    project "RID3D12QueueSmoke"
        kind "ConsoleApp"
        language "C++"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files {
            ROOT .. "/tests/graphics/ri_d3d12/queue_smoke.cpp",
        }
        includedirs {
            ROOT .. "/HPL2/core/include",
        }
        defines { "USE_SDL2", "WIN32_LEAN_AND_MEAN", "IGNORE_HPL_MAIN" }
        vulkan_includes()
        link_engine('tests')
        -- No add_test_postbuild(): GPU smoke tests run manually.
end

if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    project "RID3D12BarrierSmoke"
        kind "ConsoleApp"
        language "C++"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files {
            ROOT .. "/tests/graphics/ri_d3d12/barrier_smoke.cpp",
        }
        includedirs {
            ROOT .. "/HPL2/core/include",
        }
        defines { "USE_SDL2", "WIN32_LEAN_AND_MEAN", "IGNORE_HPL_MAIN" }
        vulkan_includes()
        link_engine('tests')
        -- No add_test_postbuild(): GPU smoke tests run manually.
end

-- DXR acceleration-structure bring-up. Skips itself when the adapter reports
-- rayTracingTier == 0, so it is safe to run anywhere, but it still needs a real
-- device and so stays off the automatic postbuild hook.
if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    project "RID3D12AccelStructureSmoke"
        kind "ConsoleApp"
        language "C++"
        cppdialect "C++20"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files {
            ROOT .. "/tests/graphics/ri_d3d12/accel_structure_smoke.cpp",
        }
        includedirs {
            ROOT .. "/HPL2/core/include",
        }
        defines { "USE_SDL2", "WIN32_LEAN_AND_MEAN", "IGNORE_HPL_MAIN" }
        vulkan_includes()
        link_engine('tests')
        -- No add_test_postbuild(): GPU smoke tests run manually.
end

if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    project "RID3D12UploaderSmoke"
        kind "ConsoleApp"
        language "C++"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files {
            ROOT .. "/tests/graphics/ri_d3d12/uploader_smoke.cpp",
        }
        includedirs {
            ROOT .. "/HPL2/core/include",
        }
        defines { "USE_SDL2", "WIN32_LEAN_AND_MEAN", "IGNORE_HPL_MAIN" }
        vulkan_includes()
        link_engine('tests')
        -- No add_test_postbuild(): GPU smoke tests run manually.
end

-- Image-region copies require a real D3D12 device and are intentionally
-- opt-in; run manually from build-premake/tests/<config>.
if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    project "RID3D12ImageCopySmoke"
        kind "ConsoleApp"
        language "C++"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files { ROOT .. "/tests/graphics/ri_d3d12/image_copy_smoke.cpp" }
        includedirs { ROOT .. "/HPL2/core/include" }
        defines { "USE_SDL2", "WIN32_LEAN_AND_MEAN", "IGNORE_HPL_MAIN" }
        vulkan_includes()
        link_engine('tests')
        -- No add_test_postbuild(): GPU smoke tests run manually.
end

-- Mip generation requires a real D3D12 device and is intentionally opt-in;
-- run manually from build-premake/tests/<config>.
if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    project "RID3D12MipGenerationSmoke"
        kind "ConsoleApp"
        language "C++"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files { ROOT .. "/tests/graphics/ri_d3d12/mip_generation_smoke.cpp" }
        includedirs { ROOT .. "/HPL2/core/include" }
        d3d12ma_includes()
        defines { "USE_SDL2", "WIN32_LEAN_AND_MEAN", "IGNORE_HPL_MAIN" }
        vulkan_includes()
        link_engine('tests')
        -- No add_test_postbuild(): GPU smoke tests run manually.
end

if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    project "RID3D12SwapchainSmoke"
        kind "ConsoleApp"
        language "C++"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files {
            ROOT .. "/tests/graphics/ri_d3d12/swapchain_smoke.cpp",
        }
        includedirs {
            ROOT .. "/HPL2/core/include",
        }
        defines { "USE_SDL2", "WIN32_LEAN_AND_MEAN", "IGNORE_HPL_MAIN" }
        vulkan_includes()
        link_engine('tests')
        -- No add_test_postbuild(): GPU smoke tests run manually.
end

if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    project "RID3D12DescriptorComputeShaderFixture"
        kind "StaticLib"
        language "C++"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        local descriptor_fixture_dir = ROOT .. "/tests/graphics/ri_d3d12/shaders"
        files { descriptor_fixture_dir .. "/_dxil_target.cpp" }
        slang_dxil_prebuild {
            sources = {
                { path = descriptor_fixture_dir .. "/descriptor_compute.slang", entry = "CSMain", stage = "compute", output = "descriptor_compute.comp.dxil", reflection = true },
            },
            include_dirs = { descriptor_fixture_dir, ROOT .. "/amnesia/slang" },
            output_dir = BUILD_OUT .. "/tests/%{cfg.buildcfg}/compiled_shaders/d3d12",
        }
end

if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    project "RID3D12DescriptorComputeFixture"
        kind "ConsoleApp"
        language "C++"
        cppdialect "C++20"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files {
            ROOT .. "/tests/graphics/ri_d3d12/descriptor_compute_fixture.cpp",
        }
        includedirs {
            ROOT .. "/HPL2/core/include",
        }
        defines { "USE_SDL2", "WIN32_LEAN_AND_MEAN", "IGNORE_HPL_MAIN" }
        vulkan_includes()
        link_engine('tests')
        dependson { "RID3D12DescriptorComputeShaderFixture" }
        -- No add_test_postbuild(): this fixture requires a real D3D12 device.
end

if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    project "RID3D12ExternalBindlessComputeSmoke"
        kind "ConsoleApp"
        language "C++"
        cppdialect "C++20"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files { ROOT .. "/tests/graphics/ri_d3d12/external_bindless_compute_smoke.cpp" }
        includedirs { ROOT .. "/HPL2/core/include" }
        defines { "USE_SDL2", "WIN32_LEAN_AND_MEAN", "IGNORE_HPL_MAIN" }
        vulkan_includes()
        link_engine('tests')
        dependson { "RID3D12ShaderFixtures" }
        -- Deliberately manual: this requires a real D3D12 device.
end

if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    project "RID3D12RIProgramImageDescriptorsSmoke"
        kind "ConsoleApp"
        language "C++"
        cppdialect "C++20"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        files { ROOT .. "/tests/graphics/ri_d3d12/ri_program_image_descriptors_smoke.cpp" }
        includedirs { ROOT .. "/HPL2/core/include" }
        defines { "USE_SDL2", "WIN32_LEAN_AND_MEAN", "IGNORE_HPL_MAIN" }
        vulkan_includes()
        link_engine('tests')
        dependson { "RID3D12ShaderFixtures" }
        -- Manual opt-in: requires a real D3D12 device and debug layer.
end

if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    project "RID3D12ShaderFixtures"
        kind "StaticLib"
        language "C++"
        objdir (BUILD_OUT .. "/obj/%{prj.name}/%{cfg.buildcfg}")
        targetdir (BUILD_OUT .. "/tests/%{cfg.buildcfg}")
        local fixture_dir = ROOT .. "/tests/graphics/ri_d3d12/shaders"
        files { fixture_dir .. "/_dxil_target.cpp" }
        slang_dxil_prebuild {
            sources = {
                { path = fixture_dir .. "/triangle.slang", entry = "VSMain", stage = "vertex", output = "triangle.vert.dxil", reflection = true },
                { path = fixture_dir .. "/triangle.slang", entry = "PSMain", stage = "fragment", output = "triangle.frag.dxil", reflection = true },
                { path = fixture_dir .. "/compute.slang", entry = "CSMain", stage = "compute", output = "compute.comp.dxil", reflection = true },
                { path = fixture_dir .. "/descriptor_compute.slang", entry = "CSMain", stage = "compute", output = "descriptor_compute.comp.dxil", reflection = true },
                { path = fixture_dir .. "/geometry_stream.slang", entry = "CSMain", stage = "compute", output = "geometry_stream.comp.dxil", reflection = true },
                { path = fixture_dir .. "/ri_program_geometry_stream.slang", entry = "CSMain", stage = "compute", output = "ri_program_geometry_stream.comp.dxil", reflection = true },
                { path = ROOT .. "/amnesia/slang/UI/gui.vert.slang", entry = "vsMain", stage = "vertex", output = "gui.vert.dxil", reflection = true },
                { path = ROOT .. "/amnesia/slang/UI/gui.frag.slang", entry = "psMain", stage = "fragment", output = "gui.frag.dxil", reflection = true },
                { path = fixture_dir .. "/external_bindless_compute.slang", entry = "CSMain", stage = "compute", output = "external_bindless_compute.comp.dxil", reflection = true },
                { path = fixture_dir .. "/ri_program_image_descriptors.slang", entry = "CSMain", stage = "compute", output = "ri_program_image_descriptors.comp.dxil", reflection = true },
            },
            include_dirs = { fixture_dir, ROOT .. "/amnesia/slang" },
            shared_deps = {
                fixture_dir .. "/shared.slang",
                ROOT .. "/amnesia/slang/BindlessTriangle.slang",
                ROOT .. "/amnesia/slang/bindless.slang",
                ROOT .. "/amnesia/slang/GeometryStream.slang",
                ROOT .. "/amnesia/slang/SceneTypes.slang",
                ROOT .. "/amnesia/slang/Constants.h",
                ROOT .. "/amnesia/slang/PerFrame/resource.slang",
                ROOT .. "/amnesia/slang/HostDefinitions.h",
            },
            output_dir = BUILD_OUT .. "/tests/%{cfg.buildcfg}/compiled_shaders/d3d12",
        }
end
