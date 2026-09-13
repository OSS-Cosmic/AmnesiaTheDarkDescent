-- Shared HPL2 memory-tracking build wiring.
--
-- The allocator backend belongs to HPL2, while C++ global new/delete
-- operators belong to each final executable.  Keeping those two pieces
-- separate is important: putting MemoryOperators.cpp in the static engine
-- would make the operators unavailable to (or duplicated by) consumers.

local MEMORY_ROOT = ROOT .. "/HPL2/extern/FluidStudios/MemoryManager"
local MEMORY_OPERATORS = ROOT .. "/HPL2/core/sources/memory/MemoryOperators.cpp"

function memory_tracking_enabled()
    return _OPTIONS["memory-tracking"] == "yes"
end

-- The C allocator implementation. Call only for a target that owns the
-- backend (HPL2 or a deliberately standalone memory test). The HPL2-facing
-- MEMORY_MANAGER_ACTIVE define is applied by memory_engine(), not here, so a
-- standalone Fluid Studios test remains independent of the HPL facade.
function memory_backend(enabled)
    if enabled == nil then enabled = memory_tracking_enabled() end
    if not enabled then return end
    files { MEMORY_ROOT .. "/mmgr.c" }
    includedirs { MEMORY_ROOT }
end

-- The C++ platform boundary and the platform libraries it requires.
-- mmgr_platform.cpp is part of the backend owner; the libraries are repeated
-- on consumers because premake does not propagate static-library link lists.
function memory_platform(enabled)
    if enabled == nil then enabled = memory_tracking_enabled() end
    if not enabled then return end
    files { MEMORY_ROOT .. "/mmgr_platform.cpp" }
    includedirs { MEMORY_ROOT }
    filter "system:linux"
        links { "pthread" }
    filter "system:windows"
        links { "dbghelp" }
    filter {}
end

-- Enable the backend in the HPL2 static library.  This is intentionally a
-- no-op when --memory-tracking=no so the normal build has no mmgr dependency.
function memory_engine(enabled)
    if enabled == nil then enabled = memory_tracking_enabled() end
    if not enabled then return end
    memory_backend(true)
    memory_platform(true)
    defines { "MEMORY_MANAGER_ACTIVE" }
end

-- Add the process-wide operators to a final executable exactly once.  The
-- file filter is intentional: the engine uses -fno-exceptions on gcc/clang,
-- but MemoryOperators.cpp implements throwing operator new and must retain
-- exception support. Disabled consumers omit it; the disabled-facade test
-- explicitly compiles the empty translation unit as a compile contract.
function memory_consumer(enabled)
    if enabled == nil then enabled = memory_tracking_enabled() end
    if not enabled then return end

    files { MEMORY_OPERATORS }

    defines { "MEMORY_MANAGER_ACTIVE" }
    includedirs { MEMORY_ROOT }
    filter "files:**/MemoryOperators.cpp"
        exceptionhandling "On"
    filter { "files:**/MemoryOperators.cpp", "toolset:gcc or clang" }
        buildoptions { "-fexceptions" }
    filter {}
    filter "system:linux"
        links { "pthread" }
    filter "system:windows"
        links { "dbghelp" }
    filter {}
end
