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

-- Pinned DXC release. The Slang release does not bundle dxcompiler.dll, so
-- `-target dxil` otherwise depends on whatever DXC is on PATH (the Vulkan SDK
-- locally, nothing on hosted CI runners).
DXC_VERSION = "1.9.2607"
DXC_ASSET   = "dxc_2026_07_29.zip"
DXC_SHA256  = "a1dfb116ba3eeae6a1582291b53a8e7bf65ad760676bd3194685c8f7367cd241"

local function file_sha256(filename)
    local output = os.outputof('certutil -hashfile "' .. winpath(filename) .. '" SHA256') or ''
    for candidate in output:gmatch('%x+') do
        if #candidate == 64 then return candidate:lower() end
    end
    error('DXC: could not calculate SHA-256 for ' .. filename)
end

-- Stage dxcompiler.dll + dxil.dll beside slangc.exe. Windows searches the
-- executable's directory before PATH, so slangc loads this copy rather than
-- any DXC a developer happens to have installed. Idempotent.
local function stage_dxc(slangc)
    local bin = path.getdirectory(slangc)
    local dlls = { "dxcompiler.dll", "dxil.dll" }
    local stamp = bin .. "/dxc-" .. DXC_VERSION .. ".stamp"
    if os.isfile(stamp) then return end

    local root = SLANG_PREBUILT .. "/dxc-" .. DXC_VERSION
    local archive = SLANG_PREBUILT .. "/" .. DXC_ASSET
    local url = "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v"
        .. DXC_VERSION .. "/" .. DXC_ASSET
    os.mkdir(root)
    print("DXC: downloading " .. url)
    local res, code = http.download(url, archive, {})
    if res ~= "OK" then
        os.remove(archive)
        error(string.format("DXC: download failed (%s, code %s). URL: %s", tostring(res), tostring(code), url))
    end
    local actual = file_sha256(archive)
    if actual ~= DXC_SHA256 then
        os.remove(archive)
        error(string.format("DXC: SHA-256 mismatch for %s (expected %s, got %s)", archive, DXC_SHA256, actual))
    end
    zip.extract(archive, root)
    os.remove(archive)

    for _, dll in ipairs(dlls) do
        local src = root .. "/bin/x64/" .. dll
        if not os.isfile(src) then
            error("DXC: " .. dll .. " not found in extracted archive at " .. root)
        end
        local ok, err = os.copyfile(src, bin .. "/" .. dll)
        if not ok then error("DXC: failed to stage " .. dll .. ": " .. tostring(err)) end
    end
    io.writefile(stamp, DXC_VERSION .. "\n")
end

-- Resolve a slangc executable for shader compilation.
--  1. --slangc=<path>
--  2. the pinned prebuilt release, reused from build-premake/_deps/slang-prebuilt
--     if already extracted, otherwise downloaded now (configure time). On
--     Windows the pinned DXC is staged beside it for the DXIL targets.
function resolve_slangc()
    if _OPTIONS["slangc"] then return _OPTIONS["slangc"] end
    local slangc = download_slangc()
    if os.target() == "windows" then stage_dxc(slangc) end
    return slangc
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
    -- Each backend owns a sibling folder under compiled_shaders. Both producers
    -- emit the same %{file.basename} spelling into their own folder, so the
    -- loader resolves a logical shader name by picking the extension.
    local out = runtime_dir("") .. "/compiled_shaders/vk"

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
        buildoutputs { out .. "/%{file.basename}.spv", out .. "/%{file.basename}.spv.meta" }
    filter { "files:**.slang", "system:not windows" }
        buildcommands {
            string.format('mkdir -p "%s"', out),
            compile,
            string.format("printf 'HPL2_SHADER_ARTIFACT\\nversion=1\\nformat=spirv\\nsource=%%{file.basename}.slang\\nstage=unknown\\nentry=unknown\\nreflection=embedded\\n' > \"%s/%%{file.basename}.spv.meta\"", out),
        }
    filter { "files:**.slang", "system:windows" }
        buildcommands {
            string.format('if not exist "%s" mkdir "%s"', out, out),
            compile,
            string.format('> "%s\\%%{file.basename}.spv.meta" (echo HPL2_SHADER_ARTIFACT&echo version=1&echo format=spirv&echo source=%%{file.basename}.slang&echo stage=unknown&echo entry=unknown&echo reflection=embedded)', winpath(out)),
        }
    filter {}

    filter "system:windows"
        postbuildcommands {
            'if not "$(ATDD_DIR)"=="" if not exist "$(ATDD_DIR)\\core\\shaders" mkdir "$(ATDD_DIR)\\core\\shaders"',
            string.format('if not "$(ATDD_DIR)"=="" if exist "%s\\*.spv" copy /Y "%s\\*.spv" "$(ATDD_DIR)\\core\\shaders\\" >nul',
                winpath(out), winpath(out)),
            string.format('if not "$(ATDD_DIR)"=="" if exist "%s\\*.spv.meta" copy /Y "%s\\*.spv.meta" "$(ATDD_DIR)\\core\\shaders\\" >nul',
                winpath(out), winpath(out)),
        }
    filter {}
end

-- Read entry points from the source rather than assuming that every shader has
-- a function named `main`.  This also preserves all entries in files such as
-- the raster dual-entry shaders and the ray-tracing libraries.
local function discover_slang_entries(file)
    local handle = io.open(file, "r")
    if not handle then error("Slang: cannot read shader source " .. file) end
    local entries = {}
    local stage
    local has_explicit_attribute = false
    local suffix_stages = {
        [".vert.slang"] = "vertex", [".frag.slang"] = "fragment",
        [".comp.slang"] = "compute", [".cs.slang"] = "compute",
        [".3d.slang"] = nil, [".rt.slang"] = nil,
    }
    for line in handle:lines() do
        local declared_stage = line:match("%[shader%s*%(%s*\"([^\"]+)\"%s*%)%]")
        if declared_stage then
            has_explicit_attribute = true
            -- Slang permits the attribute and function declaration on the
            -- same line. Handle that form before carrying the stage to the
            -- following line.
            local declaration = line:match("%]%s*(.+)$") or ""
            local name = declaration:match("[%w_<>%[%]]+%s+([%w_]+)%s*%(")
            if name then
                table.insert(entries, { entry = name, stage = declared_stage })
                stage = nil
            else
                stage = declared_stage
            end
        elseif stage then
            -- Return types may be user structs or scalar/vector types.  The
            -- name immediately before the argument list is the entry symbol.
            local name = line:match("[%w_<>%[%]]+%s+([%w_]+)%s*%(")
            if name then
                table.insert(entries, { entry = name, stage = stage })
                stage = nil
            end
        end
    end
    handle:close()
    -- One legacy compute source predates shader attributes.  Its entry is
    -- still explicit in the source (the function carrying [numthreads]); use
    -- the stage encoded by its .cs.slang suffix rather than inventing `main`.
    if #entries == 0 and not has_explicit_attribute then
        local fallback_stage
        for suffix, candidate in pairs(suffix_stages) do
            if file:sub(-#suffix) == suffix then fallback_stage = candidate end
        end
        if fallback_stage then
            local fallback = io.open(file, "r")
            local threaded
            for line in fallback:lines() do
                if line:match("%[numthreads%s*%(") then threaded = true
                elseif threaded then
                    local name = line:match("[%w_<>%[%]]+%s+([%w_]+)%s*%(")
                    if name then
                        table.insert(entries, { entry = name, stage = fallback_stage })
                        break
                    end
                end
            end
            fallback:close()
        end
    end
    return entries
end

-- Explicit Slang-to-DXIL rules for fixture entry points. A source may contain
-- multiple entries, so entries sharing a file use one fail-fast custom rule
-- with separate declared outputs.
function slang_dxil_prebuild(spec)
    local slangc = resolve_slangc()
    local unique_sources = {}
    local source_entries = {}
    local seen = {}
    for _, entry in ipairs(spec.sources) do
        if not seen[entry.path] then
            seen[entry.path] = true
            table.insert(unique_sources, entry.path)
            source_entries[entry.path] = {}
        end
        table.insert(source_entries[entry.path], entry)
    end
    files(unique_sources)

    local include_flags = ""
    for _, include_dir in ipairs(spec.include_dirs or {}) do
        include_flags = include_flags .. ' -I"' .. include_dir .. '"'
    end
    local shared_deps = { slangc }
    for _, dep in ipairs(spec.shared_deps or {}) do table.insert(shared_deps, dep) end

    for _, source in ipairs(unique_sources) do
        local outputs = {}
        local win_commands = {}
        local nix_commands = {}
        -- Ensure the output directory exists before any compile step. Each
        -- buildcommands entry becomes its own line under MSBuild / make, so a
        -- non-zero exit code from any step stops the chain -- unlike a single
        -- "&&"-joined string, which under cmd.exe's `if exist X del X && ...`
        -- rule swallows the tail whenever the guarded branch is skipped.
        table.insert(win_commands, 'if not exist "' .. spec.output_dir .. '" mkdir "' .. spec.output_dir .. '"')
        table.insert(nix_commands, 'mkdir -p "' .. spec.output_dir .. '"')
        for _, entry in ipairs(source_entries[source]) do
            local output = spec.output_dir .. "/" .. entry.output
            table.insert(outputs, output)
            table.insert(outputs, output .. ".meta")
            local reflection = output .. ".reflection-v1.json"
            if entry.reflection then table.insert(outputs, reflection) end
            -- Compile straight to the target path. slangc writes the output
            -- only after successful compilation, so a failed invocation
            -- leaves no stale artifact behind; premake declares the outputs
            -- so incremental rebuilds still trigger on missing files.
            table.insert(win_commands, string.format(
                '"%s" "%s" -D DXIL -target dxil -profile sm_6_6 -matrix-layout-column-major -entry "%s" -stage "%s"%s -o "%s"%s',
                slangc, entry.path, entry.entry, entry.stage, include_flags, output,
                entry.reflection and string.format(' -reflection-json "%s"', reflection) or ""))
            local metadata = 'echo HPL2_SHADER_ARTIFACT&echo version=1&echo format=dxil'
            if entry.reflection then
                metadata = metadata .. '&echo source=' .. entry.path .. '&echo stage=' .. entry.stage ..
                    '&echo entry=' .. entry.entry .. '&echo reflection=' .. path.getname(reflection)
            end
            table.insert(win_commands, string.format(
                '> "%s.meta" (%s)', output, metadata))
            -- -D DXIL must match the Windows command above: fixtures whose
            -- include_dirs reach amnesia/slang rely on it for the pinned
            -- registers (gPerFrame's b3, gPushConstants' space9). Without it
            -- those pins silently vanish and the stages disagree again.
            table.insert(nix_commands, string.format(
                '"%s" "%s" -D DXIL -target dxil -profile sm_6_6 -matrix-layout-column-major -entry "%s" -stage "%s"%s -o "%s"%s',
                slangc, entry.path, entry.entry, entry.stage, include_flags, output,
                entry.reflection and string.format(' -reflection-json "%s"', reflection) or ""))
            local nix_metadata = 'HPL2_SHADER_ARTIFACT\\nversion=1\\nformat=dxil\\n'
            if entry.reflection then
                nix_metadata = nix_metadata .. 'source=' .. entry.path .. '\\nstage=' .. entry.stage ..
                    '\\nentry=' .. entry.entry .. '\\nreflection=' .. path.getname(reflection) .. '\\n'
            end
            table.insert(nix_commands, string.format(
                "printf '%s' > \"%s.meta\"", nix_metadata, output))
        end

        -- Filter on a filename glob rather than the absolute source path:
        -- premake parses ":" in filter values as a field prefix, so
        -- "files:C:/..." fails with "Invalid field prefix 'c'". Fixture
        -- basenames (triangle.slang, compute.slang, shared.slang) are unique
        -- across the spec, so "**<basename>" is unambiguous.
        local pattern = "files:**" .. path.getname(source)
        filter { pattern }
            buildmessage "Slang DXIL %{file.relpath}"
            buildinputs(shared_deps)
            buildoutputs(outputs)
        filter { pattern, "system:windows" }
            buildcommands(win_commands)
        filter { pattern, "system:not windows" }
            buildcommands(nix_commands)
        filter {}
    end
end

-- Production DXIL build rules.  This is deliberately opt-in with the D3D12
-- backend: the Vulkan/SPIR-V output above remains the default engine path.
-- Each source gets one binary and one versioned reflection sidecar. Sources
-- with multiple entries retain that DXIL library for ray-tracing state-object
-- creation, and also emit executable per-entry blobs for normal graphics and
-- compute PSOs (which cannot consume a lib_6_6 container).
function slang_dxil_production_prebuild()
    local slangc = resolve_slangc()
    local src = ROOT .. "/amnesia/slang"
    local out = runtime_dir("compiled_shaders/d3d12")
    local generated = BUILD_OUT .. "/generated/%{cfg.buildcfg}/dxil"
    local embedder = BUILD_OUT .. "/tools/%{cfg.buildcfg}/ri_shader_embed.exe"
    dependson { "RIShaderEmbed" }
    local sources = {}
    local shared_deps = { slangc }
    for _, file in ipairs(os.matchfiles(src .. "/**.slang")) do
        if is_entry_shader(file) then
            local entries = discover_slang_entries(file)
            if #entries == 0 then
                error("Slang: entry shader has no explicit [shader] entry point: " .. file)
            end
            table.insert(sources, { path = file, entries = entries })
        else
            table.insert(shared_deps, file)
        end
    end
    for _, pattern in ipairs({ "/**.h", "/**.slangh" }) do
        for _, file in ipairs(os.matchfiles(src .. pattern)) do table.insert(shared_deps, file) end
    end
    files((function()
        local result = {}
        for _, source in ipairs(sources) do table.insert(result, source.path) end
        return result
    end)())

    local function artifact_name(file, suffix)
        -- The same spelling the SPIR-V rule uses (%{file.basename}): directory
        -- dropped, .slang stripped. Source basenames are globally unique, so
        -- the loader can resolve by logical name alone and needs no scan.
        return out .. "/" .. path.getbasename(file) .. suffix
    end
    for _, source in ipairs(sources) do
        local outputs, win_commands, nix_commands = {}, {}, {}
        table.insert(win_commands, 'if not exist "' .. out .. '" mkdir "' .. out .. '"')
        table.insert(nix_commands, 'mkdir -p "' .. out .. '"')
        local dxil = artifact_name(source.path, ".dxil")
        local reflection = artifact_name(source.path, ".dxil.reflection-v1.json")
        local meta = dxil .. ".meta"
        local generated_name = path.getname(dxil):gsub("%.dxil$", "") .. ".h"
        local generated_header = generated .. "/" .. generated_name
        table.insert(outputs, dxil)
        table.insert(outputs, reflection)
        table.insert(outputs, meta)
        table.insert(outputs, generated_header)
        -- SM 6.8: the raster vertex shaders read SV_StartInstanceLocation (the
        -- draw's firstInstance carries the bindless object slot; D3D12's
        -- SV_InstanceID never includes it). RID3D12 rejects adapters below 6.8.
        local compileProfile = "sm_6_8"
        local compileEntry = ""
        if #source.entries == 1 then
            local entry = source.entries[1]
            compileEntry = string.format(' -entry "%s" -stage "%s"', entry.entry, entry.stage)
        else
            -- A source with multiple entries is one reusable DXIL library.
            -- Keep all entry points in one container so callers can load the
            -- same logical artifact for each stage/entry pair.
            compileProfile = "lib_6_8"
        end
        local command = string.format(
            '"%s" "%s" -target dxil -profile %s -matrix-layout-column-major -DDXIL%s -I"%s" -o "%s" -reflection-json "%s"',
            slangc, source.path, compileProfile, compileEntry, src, dxil, reflection)
        table.insert(win_commands, command)
        table.insert(nix_commands, command)
        local embed_command = string.format(
            '"%s" --output "%s" --namespace "hpl::ri_embedded" --input "%s"',
            embedder, generated_header, dxil)
        table.insert(win_commands, 'if not exist "' .. generated .. '" mkdir "' .. generated .. '"')
        table.insert(win_commands, embed_command)
        -- This rule is only enabled for Windows/D3D12, but keep the non-Windows
        -- command complete for Premake model generation and fixture reuse.
        table.insert(nix_commands, 'mkdir -p "' .. generated .. '"')
        table.insert(nix_commands, (embed_command:gsub("ri_shader_embed%.exe", "ri_shader_embed")))
        local sourceName = path.getrelative(src, source.path):gsub("[/\\]", "/")
        local entries = {}
        for _, entry in ipairs(source.entries) do
            table.insert(entries, entry.stage .. ":" .. entry.entry)
        end
        local entryText = table.concat(entries, ",")
        local metaText = string.format(
            "HPL2_SHADER_ARTIFACT\\nversion=1\\nformat=dxil\\nsource=%s\\nstage=%s\\nentry=%s\\nreflection=%s\\n",
            sourceName, (#source.entries == 1 and source.entries[1].stage or "library"),
            entryText, path.getname(reflection))
        table.insert(win_commands, string.format(
            '> "%s" (echo HPL2_SHADER_ARTIFACT&echo version=1&echo format=dxil&echo source=%s&echo stage=%s&echo entry=%s&echo reflection=%s)',
            meta, sourceName, (#source.entries == 1 and source.entries[1].stage or "library"), entryText, path.getname(reflection)))
        table.insert(nix_commands, string.format("printf '%s' > \"%s\"", metaText, meta))
        if #source.entries > 1 then
            for _, entry in ipairs(source.entries) do
                if entry.stage == "vertex" or entry.stage == "fragment" or
                   entry.stage == "pixel" or entry.stage == "compute" then
                    local entryDxil = dxil:gsub("%.dxil$", "." .. entry.entry .. ".dxil")
                    local entryReflection = entryDxil .. ".reflection-v1.json"
                    local entryMeta = entryDxil .. ".meta"
                    table.insert(outputs, entryDxil)
                    table.insert(outputs, entryReflection)
                    table.insert(outputs, entryMeta)
                    local entryCommand = string.format(
                        '"%s" "%s" -target dxil -profile sm_6_8 -matrix-layout-column-major -DDXIL -entry "%s" -stage "%s" -I"%s" -o "%s" -reflection-json "%s"',
                        slangc, source.path, entry.entry, entry.stage, src,
                        entryDxil, entryReflection)
                    table.insert(win_commands, entryCommand)
                    table.insert(nix_commands, entryCommand)
                    table.insert(win_commands, string.format(
                        '> "%s" (echo HPL2_SHADER_ARTIFACT&echo version=1&echo format=dxil&echo source=%s&echo stage=%s&echo entry=%s:%s&echo reflection=%s)',
                        entryMeta, sourceName, entry.stage, entry.stage,
                        entry.entry, path.getname(entryReflection)))
                    local entryMetaText = string.format(
                        "HPL2_SHADER_ARTIFACT\\nversion=1\\nformat=dxil\\nsource=%s\\nstage=%s\\nentry=%s:%s\\nreflection=%s\\n",
                        sourceName, entry.stage, entry.stage, entry.entry,
                        path.getname(entryReflection))
                    table.insert(nix_commands, string.format(
                        "printf '%s' > \"%s\"", entryMetaText, entryMeta))
                    table.insert(win_commands, 'if not exist "' .. entryDxil .. '" exit /b 1')
                    table.insert(win_commands, 'if not exist "' .. entryReflection .. '" exit /b 1')
                    table.insert(win_commands, 'if not exist "' .. entryMeta .. '" exit /b 1')
                    table.insert(nix_commands, 'test -s "' .. entryDxil .. '" && test -s "' .. entryReflection .. '" && test -s "' .. entryMeta .. '"')
                end
            end
        end
        -- Declared outputs are normally enough for incremental builders;
        -- these checks also fail a successful-looking command when a tool
        -- version or filesystem issue omitted an artifact.
        table.insert(win_commands, 'if not exist "' .. dxil .. '" exit /b 1')
        table.insert(win_commands, 'if not exist "' .. reflection .. '" exit /b 1')
        table.insert(win_commands, 'if not exist "' .. meta .. '" exit /b 1')
        table.insert(win_commands, 'if not exist "' .. generated_header .. '" exit /b 1')
        table.insert(nix_commands, 'test -s "' .. dxil .. '" && test -s "' .. reflection .. '" && test -s "' .. meta .. '"')
        table.insert(nix_commands, 'test -s "' .. generated_header .. '"')
        local pattern = "files:**" .. path.getname(source.path)
        filter { pattern }
            buildmessage "Slang DXIL %{file.relpath}"
            buildinputs(shared_deps)
            buildoutputs(outputs)
        filter { pattern, "system:windows" }
            buildcommands(win_commands)
        filter { pattern, "system:not windows" }
            buildcommands(nix_commands)
        filter {}
    end

    -- Stage the DXIL next to the SPIR-V that slang_prebuild copies, so a run
    -- from an ATDD_DIR install can resolve `<name>.dxil` the same way it
    -- resolves `<name>.spv`. core/shaders is a flat resource dir and the
    -- loader picks the extension from the active backend, so both backends'
    -- artifacts coexist there. The .reflection-v1.json is not optional: the
    -- D3D12 arm of RIProgram::initialize fatals on a stage with no retained
    -- reflection, and the .meta sidecar names that JSON by filename.
    filter "system:windows"
        postbuildcommands {
            'if not "$(ATDD_DIR)"=="" if not exist "$(ATDD_DIR)\\core\\shaders" mkdir "$(ATDD_DIR)\\core\\shaders"',
            string.format('if not "$(ATDD_DIR)"=="" if exist "%s\\*.dxil" copy /Y "%s\\*.dxil" "$(ATDD_DIR)\\core\\shaders\\" >nul',
                winpath(out), winpath(out)),
            string.format('if not "$(ATDD_DIR)"=="" if exist "%s\\*.dxil.meta" copy /Y "%s\\*.dxil.meta" "$(ATDD_DIR)\\core\\shaders\\" >nul',
                winpath(out), winpath(out)),
            string.format('if not "$(ATDD_DIR)"=="" if exist "%s\\*.reflection-v1.json" copy /Y "%s\\*.reflection-v1.json" "$(ATDD_DIR)\\core\\shaders\\" >nul',
                winpath(out), winpath(out)),
        }
    filter {}
end

-- The D3D12 mip shader is private to HPL2. Compile each entry separately and
-- embed the resulting DXIL into generated headers; no runtime compiler or
-- checked-in binary is part of the engine.
function slang_d3d12_mips_prebuild()
    local slangc = resolve_slangc()
    local shader = ROOT .. "/HPL2/core/sources/graphics/shaders/ri_d3d12_mips.slang"
    local out = BUILD_OUT .. "/generated/%{cfg.buildcfg}"
    local tool = BUILD_OUT .. "/tools/%{cfg.buildcfg}/ri_shader_embed.exe"
    local entries = { {"mip2DArray", "2d_array"}, {"mip3D", "3d"} }
    dependson { "RIShaderEmbed" }
    files { shader }
    local outputs = {}
    local commands = { 'if not exist "' .. out .. '" mkdir "' .. out .. '"' }
    for _, item in ipairs(entries) do
        local dxil = out .. "/ri_d3d12_mips_" .. item[2] .. ".dxil"
        local header = out .. "/ri_d3d12_mips.h"
        table.insert(outputs, dxil)
        table.insert(commands, string.format('"%s" "%s" -target dxil -profile cs_6_6 -entry %s -stage compute -o "%s"', slangc, shader, item[1], dxil))
    end
    table.insert(commands, string.format('"%s" --output "%s" --namespace "hpl::ri_embedded" --name ri_d3d12_mips_2d_array_dxil --input "%s" --name ri_d3d12_mips_3d_dxil --input "%s"', tool, out .. "/ri_d3d12_mips.h", outputs[1], outputs[2]))
    table.insert(outputs, out .. "/ri_d3d12_mips.h")
    filter { "files:**ri_d3d12_mips.slang", "system:windows" }
        buildmessage "D3D12 private mip shader %{file.relpath}"
        buildinputs { slangc, shader, tool }
        buildoutputs(outputs)
        buildcommands(commands)
    filter {}
end
