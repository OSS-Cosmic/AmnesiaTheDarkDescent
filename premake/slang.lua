-- premake/slang.lua -- Slang compiler acquisition + per-file SPIR-V build rules.
-- Downloads a pinned prebuilt slangc when needed and creates incremental rules for
-- entry-point shaders. Loaded after helpers.lua (it uses runtime_dir) and before
-- amnesia.lua (which calls slang_prebuild).
-- All helpers are global so the dofile'd sub-scripts can use them directly.

-- Pinned Slang release for the premake auto-download.
SLANG_VERSION  = "2026.17.1"
SLANG_PREBUILT = ROOT .. "/build-premake/_deps/slang-prebuilt"

local function slangc_exe()
    return os.target() == "windows" and "slangc.exe" or "slangc"
end

local function winpath(p) return (p:gsub("/", "\\")) end

-- Host CPU -> Slang asset arch token. The workspace pins x86_64, but detect so an
-- aarch64 host still resolves the right asset.
local function slang_host_arch()
    if os.target() == "windows" then
        local pa = (os.getenv("PROCESSOR_ARCHITECTURE") or ""):lower()
        return pa:find("arm") and "aarch64" or "x86_64"
    end
    local m = (os.outputof("uname -m") or "x86_64"):lower():gsub("%s+", "")
    return (m == "aarch64" or m == "arm64") and "aarch64" or "x86_64"
end

-- Download + extract the pinned prebuilt slangc at configure time (when premake5
-- runs). Pure Lua via premake's http.download +
-- zip.extract -- no python, no external tar: we fetch the .zip asset, which Slang
-- publishes for every platform. Idempotent (skips if slangc is already present).
local function download_slangc()
    local root   = SLANG_PREBUILT .. "/slang-" .. SLANG_VERSION
    local slangc = root .. "/bin/" .. slangc_exe()

    -- Tolerate both archive layouts: bin/slangc directly, or wrapped */bin/slangc.
    local function find_extracted()
        if os.isfile(slangc) then return slangc end
        local m = os.matchfiles(root .. "/**/bin/" .. slangc_exe())
        return m[1]
    end

    local found = find_extracted()
    if found then return found end

    local os_name = os.target()
    if os_name == "macosx" then os_name = "macos" end
    local asset = string.format("slang-%s-%s-%s.zip", SLANG_VERSION, os_name, slang_host_arch())
    local url   = "https://github.com/shader-slang/slang/releases/download/v"
        .. SLANG_VERSION .. "/" .. asset
    local archive = SLANG_PREBUILT .. "/" .. asset

    os.mkdir(root)
    print("Slang: downloading " .. url)
    local res, code = http.download(url, archive, {})
    if res ~= "OK" then
        os.remove(archive)
        error(string.format("Slang: download failed (%s, code %s). URL: %s", tostring(res), tostring(code), url))
    end
    zip.extract(archive, root)
    os.remove(archive)

    found = find_extracted()
    if not found then
        error("Slang: could not locate slangc inside extracted archive at " .. root)
    end
    if os.target() ~= "windows" then
        os.execute(string.format('chmod +x "%s"', found))  -- zip extraction drops the exec bit
    end
    return found
end

-- Resolve a slangc executable for shader compilation.
--  1. --slangc=<path>
--  2. the pinned prebuilt release, reused from build-premake/_deps/slang-prebuilt
--     if already extracted, otherwise downloaded now (configure time)
function resolve_slangc()
    if _OPTIONS["slangc"] then return _OPTIONS["slangc"] end
    return download_slangc()
end

-- Slang entry-point stage suffixes -- a .slang file is a shader to compile only if
-- its name ends in one of these. The remaining .slang files are include-only
-- headers, pulled in via the -I path rather than compiled.
local SLANG_STAGE_SUFFIXES = {
    ".vert.slang", ".frag.slang", ".comp.slang", ".cs.slang", ".geom.slang",
    ".tesc.slang", ".tese.slang", ".rgen.slang", ".rchit.slang", ".rmiss.slang",
    ".rahit.slang", ".rint.slang", ".rcall.slang", ".rt.slang", ".3d.slang",
}

local function is_entry_shader(file)
    for _, suffix in ipairs(SLANG_STAGE_SUFFIXES) do
        if file:sub(-#suffix) == suffix then return true end
    end
    return false
end

-- Compile every entry-point .slang shader under amnesia/slang into
-- <runtime>/compiled_shaders. Each entry-point shader gets an incremental
-- per-file build rule, so only changed shaders recompile.
-- Must be called inside the consuming project so files{}/filter{} apply to it.
function slang_prebuild()
    -- slangc is resolved (and auto-downloaded if needed) here at configure time.
    local slangc = resolve_slangc()
    local src = ROOT .. "/amnesia/slang"
    local out = runtime_dir("") .. "/compiled_shaders"

    -- Add only the entry shaders to the file list; the per-file rule below matches
    -- them via "files:**.slang". Include-only headers stay off the list (uncompiled)
    -- but remain reachable through the -I path.
    local shaders = {}
    for _, f in ipairs(os.matchfiles(src .. "/**.slang")) do
        if is_entry_shader(f) then table.insert(shaders, f) end
    end
    files(shaders)

    -- Transitive shader dependencies. A per-file rule's only implicit input is the
    -- entry .slang it matches; slangc's `import`ed modules and `#include`d headers
    -- are invisible to the build graph. When a shared module's buffer layout changed
    -- (e.g. PerFrame.resource / SceneTypes), the entry .slang mtime was unchanged so
    -- the .spv was NOT rebuilt -- and `make clean` does not delete buildoutputs -- so
    -- the runtime kept loading vertex shaders compiled against the OLD
    -- gPerFrame/gSceneObjects layout (zeroed transforms -> invisible decals/
    -- translucent/water). List every shared (non-entry) module and header as an
    -- explicit buildinput so editing one retriggers all shaders, including when
    -- prior compiled outputs remain in the build directory.
    local shared_deps = { slangc }
    for _, f in ipairs(os.matchfiles(src .. "/**.slang")) do
        if not is_entry_shader(f) then table.insert(shared_deps, f) end
    end
    for _, f in ipairs(os.matchfiles(src .. "/**.h")) do
        table.insert(shared_deps, f)
    end

    -- Target SPIR-V with the SM 6.6 profile and emit it directly; preserve entry-point
    -- names, use column-major matrices, and enable Vulkan scalar layout. %{file.*}
    -- tokens are expanded per shader at build time; %{file.basename} strips only the
    -- trailing .slang (foo.vert.slang -> foo.vert) for the output basename.
    --
    -- Only the slang root (-I"<src>") is passed -- NOT a per-file -I"%{file.directory}".
    -- Under MSBuild that token expands to %(RootDir)%(Directory), which ends in a
    -- trailing backslash, so the emitted argument is -I"C:\...\Dir\". The terminal \"
    -- is parsed as an escaped quote by the Windows command-line tokenizer, swallowing
    -- the closing quote and corrupting the following -I"<src>" -- the real include
    -- root is lost and every `import` fails (cannot open file 'bindless.slang' etc.),
    -- so no .spv is produced. The per-file include is redundant anyway: every module
    -- import resolves from the slang root (bare names map to root-level .slang;
    -- dotted names like SurfelGI.SurfelTypes map to subpaths), and #include "..." is
    -- resolved relative to the including file's own directory by slangc automatically.
    local flags = "-target spirv -profile sm_6_6 -emit-spirv-directly "
        .. "-fvk-use-entrypoint-name -matrix-layout-column-major -fvk-use-scalar-layout"
    local compile = string.format(
        '"%s" "%%{file.abspath}" %s -I"%s" -o "%s/%%{file.basename}.spv"',
        slangc, flags, src, out)

    filter "files:**.slang"
        buildmessage "Slang %{file.relpath}"
        buildinputs(shared_deps)
        buildoutputs { out .. "/%{file.basename}.spv" }
    filter { "files:**.slang", "system:not windows" }
        buildcommands {
            string.format('mkdir -p "%s"', out),
            compile,
        }
    filter { "files:**.slang", "system:windows" }
        buildcommands {
            string.format('if not exist "%s" mkdir "%s"', out, out),
            compile,
        }
    filter {}

    filter "system:windows"
        postbuildcommands {
            'if not "$(ATDD_DIR)"=="" if not exist "$(ATDD_DIR)\\core\\shaders" mkdir "$(ATDD_DIR)\\core\\shaders"',
            string.format('if not "$(ATDD_DIR)"=="" if exist "%s\\*.spv" copy /Y "%s\\*.spv" "$(ATDD_DIR)\\core\\shaders\\" >nul',
                winpath(out), winpath(out)),
        }
    filter {}
end
