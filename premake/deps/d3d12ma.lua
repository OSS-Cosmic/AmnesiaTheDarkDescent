-- D3D12 Memory Allocator (GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator).
-- The dependency is header/source based; keep its implementation in this
-- single premake project so the allocator TU cannot be linked more than once.

if os.target() == "windows" and _OPTIONS["with-d3d12"] == "yes" then
    local requested_root = _OPTIONS["d3d12ma-dir"]
    if requested_root then
        if not requested_root:match("^%a:[/\\]") and not requested_root:match("^[/\\]") then
            requested_root = ROOT .. "/" .. requested_root
        end
        D3D12MA_ROOT = path.getabsolute(requested_root)
    else
        D3D12MA_ROOT = DEPS_EXTERN .. "/D3D12MemoryAllocator"
    end

    project "D3D12MA"
        kind "StaticLib"
        language "C++"
        set_output("static")
        files {
            D3D12MA_ROOT .. "/src/D3D12MemAlloc.cpp",
            D3D12MA_ROOT .. "/include/D3D12MemAlloc.h",
        }
        includedirs { D3D12MA_ROOT .. "/include" }
        filter "system:windows"
            systemversion "latest"
        filter {}
end
